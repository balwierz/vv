// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// NumPy .npy / .npz.

#include "internal.hpp"

// ── NumPy .npz viewer ────────────────────────────────────────────────────────
//
// .npz is a ZIP archive of .npy files. Each .npy carries its own header
// (shape + dtype + fortran-order flag) followed by raw little-endian
// binary data. We mirror the HDF5 multi-tab pattern: one summary tab
// listing every array, then one tab per displayable array. 1-D arrays
// render as a single column; 2-D as a full table; 3-D+ as a 2-D slice
// along the leading axis (the user can step the slice index with
// `[` / `]` or jump to a specific slice with `:slice N`). Object
// (pickled) arrays show up in the summary but don't get a tab — they'd
// require a pickle decoder we don't have.

namespace npz {

struct NpyHeader {
    std::vector<int64_t> shape;
    arrow::Type::type    dtype_id = arrow::Type::NA;
    bool                 fortran_order = false;
    bool                 unsupported = false;  // object/string/structured
    std::string          dtype_str;            // raw "<f4" / "|O" etc.
    size_t               data_offset = 0;
    size_t               item_size = 0;        // bytes per element (0 = unknown)
    // Non-numeric dtypes decoded to Arrow: 'S' (bytes, item_size wide), 'U'
    // (UTF-32, item_size / 4 code points), 'M' (datetime64), 'm'
    // (timedelta64); 0 for the numeric ones. `unit` is the datetime unit
    // ("D", "s", "ns", …) and `mult` its multiplier ([10s] → 10).
    char                 special = 0;
    bool                 big_endian = false;
    std::string          unit;
    int64_t              mult = 1;
};

// Map numpy dtype letter codes onto Arrow types. Endianness must be
// little-endian or native (we'd need to byteswap for big-endian, which
// scientific Python virtually never produces).
static arrow::Type::type npy_letter_to_arrow(char endian, char kind, int size) {
    if (endian == '>') return arrow::Type::NA;       // big-endian: skip
    // Kind:  i=int, u=uint, f=float, b=bool, O=object, U=unicode, S=bytes
    if (kind == 'b') return arrow::Type::BOOL;
    if (kind == 'i') {
        if (size == 1) return arrow::Type::INT8;
        if (size == 2) return arrow::Type::INT16;
        if (size == 4) return arrow::Type::INT32;
        if (size == 8) return arrow::Type::INT64;
    }
    if (kind == 'u') {
        if (size == 1) return arrow::Type::UINT8;
        if (size == 2) return arrow::Type::UINT16;
        if (size == 4) return arrow::Type::UINT32;
        if (size == 8) return arrow::Type::UINT64;
    }
    if (kind == 'f') {
        if (size == 4) return arrow::Type::FLOAT;
        if (size == 8) return arrow::Type::DOUBLE;
    }
    return arrow::Type::NA;
}

// Parse the small ASCII header dict (a Python literal). We don't need a
// real Python parser — find the values for the three keys we care about.
// Tolerate single/double quotes, optional whitespace.
static std::string find_dict_value(const std::string& hdr, const std::string& key) {
    // Look for '<key>': ...  (quoted)
    for (char q : {'\'', '"'}) {
        std::string needle = std::string(1, q) + key + q;
        size_t p = hdr.find(needle);
        if (p == std::string::npos) continue;
        p = hdr.find(':', p + needle.size());
        if (p == std::string::npos) continue;
        ++p;
        while (p < hdr.size() && std::isspace((unsigned char)hdr[p])) ++p;
        // Capture until the next comma at depth 0 (track parens/brackets).
        int depth = 0;
        size_t start = p;
        while (p < hdr.size()) {
            char c = hdr[p];
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') --depth;
            else if (c == ',' && depth == 0) break;
            else if (c == '}' && depth == 0) break;
            ++p;
        }
        std::string v = hdr.substr(start, p - start);
        while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
        return v;
    }
    return "";
}

// Bytes per element that the readers (slab_to_arrow / make_column) actually read
// for each supported Arrow type — numpy bool is 1 byte, not Arrow's 1 bit. 0 for
// unsupported ids. Used to bound-check against what is really read, not the
// header's (attacker-controlled) declared item size.
static size_t npy_element_bytes(arrow::Type::type id) {
    switch (id) {
        case arrow::Type::BOOL:
        case arrow::Type::INT8:  case arrow::Type::UINT8:                       return 1;
        case arrow::Type::INT16: case arrow::Type::UINT16:                      return 2;
        case arrow::Type::INT32: case arrow::Type::UINT32: case arrow::Type::FLOAT:  return 4;
        case arrow::Type::INT64: case arrow::Type::UINT64: case arrow::Type::DOUBLE: return 8;
        default: return 0;
    }
}

// `total`: the bytes the whole .npy holds, when `buf` is only its start (an
// .npz member's header, checked against the member's declared size); else n.
static std::string parse_npy_header(const uint8_t* buf, size_t n, NpyHeader* out,
                                    uint64_t total = UINT64_MAX) {
    if (n < 10) return "npy: too short";
    if (std::memcmp(buf, "\x93NUMPY", 6) != 0) return "npy: bad magic";
    uint8_t major = buf[6], minor = buf[7]; (void)minor;
    size_t hdr_len, hdr_start;
    if (major == 1) {
        uint16_t l = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
        hdr_len = l; hdr_start = 10;
    } else if (major == 2 || major == 3) {
        if (n < 12) return "npy: too short";
        uint32_t l = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8)
                   | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
        hdr_len = l; hdr_start = 12;
    } else {
        return "npy: unsupported version " + std::to_string(major);
    }
    if (n < hdr_start + hdr_len) return "npy: header truncated";
    std::string hdr((const char*)(buf + hdr_start), hdr_len);

    std::string descr = find_dict_value(hdr, "descr");
    // Strip the surrounding quotes — but only when both ends are the *same*
    // quote. A malformed/unterminated value (e.g. "'<f8" with no closing quote)
    // would otherwise have its real last character chopped by the blind
    // substr(1, size-2).
    if (descr.size() >= 2 &&
        (descr.front() == '\'' || descr.front() == '"') &&
        descr.back() == descr.front()) {
        descr = descr.substr(1, descr.size() - 2);
    }
    out->dtype_str = descr;
    if (descr.size() >= 2) {
        // Two-char prefix (endian + kind) + size, or kind + N for |O / |Sn / |UN
        char endian = descr[0];
        char kind   = descr[1];
        int sz = 0;
        if (descr.size() >= 3) {
            try { sz = std::stoi(descr.substr(2)); } catch (...) { sz = 0; }
        }
        if (kind == 'S' || kind == 'U') {
            // Fixed-width bytes (|S8) / UTF-32 code points (<U11).
            const bool wide = kind == 'U';
            if (sz <= 0 || sz > (1 << 24) || (wide && endian != '<' && endian != '>' &&
                                              endian != '=' && endian != '|')) {
                out->unsupported = true;
            } else {
                out->special    = kind;
                out->big_endian = endian == '>';
                out->item_size  = (size_t)sz * (wide ? 4 : 1);
                out->dtype_id   = arrow::Type::STRING;
            }
        } else if ((kind == 'M' || kind == 'm') && sz == 8 && endian != '>') {
            // datetime64 / timedelta64: "<M8[ns]", "<m8[10s]". NaT is INT64_MIN.
            const size_t lb = descr.find('['), rb = descr.find(']');
            std::string u = (lb != std::string::npos && rb != std::string::npos && rb > lb)
                                ? descr.substr(lb + 1, rb - lb - 1) : "";
            size_t d = 0;
            while (d < u.size() && std::isdigit((unsigned char)u[d])) ++d;
            int64_t mult = 1;
            if (d > 0) { try { mult = std::stoll(u.substr(0, d)); } catch (...) { mult = 0; } }
            u = u.substr(d);
            static const char* kDateUnits[] = {"Y", "M", "W", "D", "h", "m", "s", "ms", "us", "ns"};
            bool known = false;
            for (const char* k : kDateUnits) known |= u == k;
            // A calendar unit has no fixed length, so no timedelta of it.
            if (kind == 'm' && (u == "Y" || u == "M")) known = false;
            if (!known || mult <= 0 || mult > 1000000) {
                out->unsupported = true;
            } else {
                out->special   = kind;
                out->unit      = u;
                out->mult      = mult;
                out->item_size = 8;
                out->dtype_id  = kind == 'm' ? arrow::Type::DURATION
                               : (u == "Y" || u == "M" || u == "W" || u == "D") ? arrow::Type::DATE32
                               : arrow::Type::TIMESTAMP;
            }
        } else if (kind == 'O' || kind == 'S' || kind == 'U' || kind == 'V'
            || kind == 'M' || kind == 'm') {
            out->unsupported = true;
        } else {
            out->dtype_id = npy_letter_to_arrow(endian, kind, sz);
            if (out->dtype_id == arrow::Type::NA) out->unsupported = true;
            out->item_size = (size_t)sz;
        }
    } else {
        out->unsupported = true;
    }

    std::string fo = find_dict_value(hdr, "fortran_order");
    out->fortran_order = (fo.find("True") != std::string::npos);

    std::string sh = find_dict_value(hdr, "shape");
    // shape is a tuple like (100, 167, 512) or () or (100,)
    if (!sh.empty() && sh.front() == '(') sh.erase(0, 1);
    if (!sh.empty() && sh.back()  == ')') sh.pop_back();
    out->shape.clear();
    std::stringstream ss(sh);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // strip whitespace
        size_t a = 0; while (a < tok.size() && std::isspace((unsigned char)tok[a])) ++a;
        size_t b = tok.size(); while (b > a && std::isspace((unsigned char)tok[b-1])) --b;
        if (a >= b) continue;
        try { out->shape.push_back(std::stoll(tok.substr(a, b - a))); }
        catch (...) { return "npy: bad shape '" + sh + "'"; }
    }
    out->data_offset = hdr_start + hdr_len;

    // The readers read a fixed number of bytes per element from the Arrow type,
    // not the header's declared item size — so a crafted dtype whose declared
    // size differs (e.g. "|b0" → item_size 0 for a 1-byte bool) would slip past
    // the shape-fits check below (item_size 0 skips it) and read out of bounds.
    // Pin item_size to what is actually read for a supported dtype.
    if (!out->unsupported && out->dtype_id != arrow::Type::NA && !out->special) {
        size_t real = npy_element_bytes(out->dtype_id);
        if (real == 0) out->unsupported = true;   // supported id with no known size
        else           out->item_size = real;
    }

    // Validate the declared shape against the data actually present. .npy/.npz
    // input is untrusted (zip members), and the downstream readers derive
    // element counts and byte offsets straight from the shape. Without this, a
    // header like (1000000000,), a negative dimension, or dims whose product
    // overflows would drive out-of-bounds reads, huge allocations, or
    // wrapped-offset pointer arithmetic. Unsupported dtypes are never read, so
    // they skip the check.
    if (!out->unsupported && out->item_size > 0) {
        uint64_t elems = 1;
        // Product of the NON-ZERO dimensions, tracked separately. A single
        // zero dim makes `elems` zero, after which every later overflow guard
        // is vacuous and the total-size check passes for free — but the
        // readers still multiply sub-ranges of the shape to compute strides
        // and slice sizes, and those products can overflow int64. A crafted
        // shape like (1, 392361265078550784, 29, 0) declares an empty array,
        // sails through the size check, then overflows when the 3-D+ path
        // collapses the trailing dims. Every sub-product divides this one, so
        // bounding it bounds all of them.
        uint64_t nz = 1;
        for (int64_t d : out->shape) {
            if (d < 0) return "npy: negative dimension in shape (" + sh + ")";
            uint64_t dd = (uint64_t)d;
            if (dd != 0 && elems > UINT64_MAX / dd)
                return "npy: shape too large (" + sh + ")";
            elems *= dd;
            if (dd != 0) {
                if (nz > (uint64_t)INT64_MAX / dd)
                    return "npy: shape too large (" + sh + ")";
                nz *= dd;
            }
        }
        if (elems > UINT64_MAX / (uint64_t)out->item_size)
            return "npy: shape too large (" + sh + ")";
        uint64_t need  = elems * (uint64_t)out->item_size;
        const uint64_t whole = total == UINT64_MAX ? (uint64_t)n : total;
        uint64_t avail = whole >= out->data_offset ? whole - (uint64_t)out->data_offset : 0;
        if (need > avail)
            return "npy: declared shape needs " + std::to_string(need) +
                   " bytes but only " + std::to_string(avail) + " present";
    }
    return "";
}

static std::string shape_str(const std::vector<int64_t>& s) {
    std::string r = "(";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) r += ", ";
        r += std::to_string(s[i]);
    }
    if (s.size() == 1) r += ",";
    r += ")";
    return r;
}

// Build an Arrow Column from a contiguous slab of N elements of dtype_id. The
// slab starts at the .npy data offset, which is not aligned to CType — read each
// element via memcpy (a typed load would be a misaligned access: UB, and it
// faults on aarch64).
template <typename CType, typename ArrowBuilder>
static std::shared_ptr<arrow::Array> make_column(const uint8_t* data, int64_t n) {
    ArrowBuilder b;
    (void)b.Reserve(n);
    for (int64_t i = 0; i < n; ++i) {
        CType v;
        std::memcpy(&v, data + (size_t)i * sizeof(CType), sizeof(CType));
        (void)b.UnsafeAppend(v);
    }
    std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
    return a;
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian; H. Hinnant's algorithm).
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static std::shared_ptr<arrow::DataType> special_type(const NpyHeader& h) {
    if (h.special == 'M') {
        if (h.dtype_id == arrow::Type::DATE32) return arrow::date32();
        const std::string& u = h.unit;
        return arrow::timestamp(u == "ms" ? arrow::TimeUnit::MILLI : u == "us" ? arrow::TimeUnit::MICRO
                              : u == "ns" ? arrow::TimeUnit::NANO : arrow::TimeUnit::SECOND);
    }
    if (h.special == 'm') {
        const std::string& u = h.unit;
        return arrow::duration(u == "ms" ? arrow::TimeUnit::MILLI : u == "us" ? arrow::TimeUnit::MICRO
                             : u == "ns" ? arrow::TimeUnit::NANO : arrow::TimeUnit::SECOND);
    }
    return arrow::utf8();
}

// Decode n elements of a string / datetime dtype. A value that does not fit
// the Arrow type (a date beyond int32 days, a multiplied count that
// overflows) is null, as is NaT. |S values become binary when one of them is
// not UTF-8, or when `force_binary` (another block of the column was not).
static std::shared_ptr<arrow::Array> decode_special(const NpyHeader& h, const uint8_t* data, int64_t n,
                                                    bool force_binary = false) {
    std::shared_ptr<arrow::Array> a;
    if (h.special == 'S' || h.special == 'U') {
        std::vector<std::string> vals((size_t)n);
        bool all_utf8 = true;
        for (int64_t i = 0; i < n; ++i) {
            const uint8_t* p = data + (size_t)i * h.item_size;
            std::string& v = vals[(size_t)i];
            if (h.special == 'S') {
                size_t len = h.item_size;
                while (len > 0 && p[len - 1] == 0) --len;       // numpy pads with NULs
                v.assign((const char*)p, len);
                if (all_utf8 && valid_utf8(v) != v) all_utf8 = false;
            } else {
                size_t chars = h.item_size / 4;
                auto cp_at = [&](size_t k) {
                    const uint8_t* q = p + k * 4;
                    return h.big_endian ? (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]
                                        : (uint32_t)q[3] << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
                };
                while (chars > 0 && cp_at(chars - 1) == 0) --chars;
                for (size_t k = 0; k < chars; ++k) {
                    uint32_t c = cp_at(k);
                    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
                    if (c < 0x80) v += (char)c;
                    else if (c < 0x800) { v += (char)(0xC0 | c >> 6); v += (char)(0x80 | (c & 0x3F)); }
                    else if (c < 0x10000) { v += (char)(0xE0 | c >> 12); v += (char)(0x80 | (c >> 6 & 0x3F));
                                            v += (char)(0x80 | (c & 0x3F)); }
                    else { v += (char)(0xF0 | c >> 18); v += (char)(0x80 | (c >> 12 & 0x3F));
                           v += (char)(0x80 | (c >> 6 & 0x3F)); v += (char)(0x80 | (c & 0x3F)); }
                }
            }
        }
        if (all_utf8 && !force_binary) {
            arrow::StringBuilder b;
            for (auto& v : vals) (void)b.Append(v);
            (void)b.Finish(&a);
        } else {                                     // |S holding arbitrary bytes
            arrow::BinaryBuilder b;
            for (auto& v : vals) (void)b.Append(v);
            (void)b.Finish(&a);
        }
        return a;
    }
    // datetime64 / timedelta64
    const std::string& u = h.unit;
    int64_t scale = h.mult;                          // multiply raw counts by this
    if (u == "h") scale *= 3600;
    else if (u == "m") scale *= 60;
    else if (u == "W") scale *= 7;
    else if (u == "D" && h.special == 'm') scale *= 86400;
    auto checked = [&](int64_t v, int64_t k, int64_t* out) {
        return !__builtin_mul_overflow(v, k, out);
    };
    if (h.dtype_id == arrow::Type::DATE32) {
        arrow::Date32Builder b; (void)b.Reserve(n);
        for (int64_t i = 0; i < n; ++i) {
            int64_t v; std::memcpy(&v, data + (size_t)i * 8, 8);
            int64_t days = 0;
            bool ok = v != INT64_MIN;
            if (ok && (u == "Y" || u == "M")) {
                int64_t cnt;
                ok = checked(v, h.mult, &cnt) && cnt > -100000000 && cnt < 100000000;
                if (ok) {
                    int64_t y = 1970, mo = 1;
                    if (u == "Y") y += cnt;
                    else { int64_t q = cnt >= 0 ? cnt / 12 : -((-cnt + 11) / 12); y += q; mo += cnt - q * 12; }
                    days = days_from_civil(y, mo, 1);
                }
            } else if (ok) {
                ok = checked(v, scale, &days);
            }
            if (ok && days >= INT32_MIN && days <= INT32_MAX) (void)b.UnsafeAppend((int32_t)days);
            else (void)b.UnsafeAppendNull();
        }
        (void)b.Finish(&a);
        return a;
    }
    std::unique_ptr<arrow::ArrayBuilder> ab;
    (void)arrow::MakeBuilder(arrow::default_memory_pool(), special_type(h), &ab);
    for (int64_t i = 0; i < n; ++i) {
        int64_t v; std::memcpy(&v, data + (size_t)i * 8, 8);
        int64_t t;
        if (v == INT64_MIN || !checked(v, scale, &t)) { (void)ab->AppendNull(); continue; }
        if (h.special == 'm') (void)static_cast<arrow::DurationBuilder*>(ab.get())->Append(t);
        else                  (void)static_cast<arrow::TimestampBuilder*>(ab.get())->Append(t);
    }
    (void)ab->Finish(&a);
    return a;
}

static std::shared_ptr<arrow::Array>
slab_to_arrow(const NpyHeader& h, const uint8_t* data, int64_t n, bool force_binary = false) {
    using T = arrow::Type;
    if (h.special) return decode_special(h, data, n, force_binary);
    switch (h.dtype_id) {
        case T::BOOL: {
            // numpy bool is 1 byte (0/1). Arrow BoolBuilder appends bool.
            arrow::BooleanBuilder b; (void)b.Reserve(n);
            for (int64_t i = 0; i < n; ++i) (void)b.UnsafeAppend(data[i] != 0);
            std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
            return a;
        }
        case T::INT8:   return make_column<int8_t,   arrow::Int8Builder>(data, n);
        case T::INT16:  return make_column<int16_t,  arrow::Int16Builder>(data, n);
        case T::INT32:  return make_column<int32_t,  arrow::Int32Builder>(data, n);
        case T::INT64:  return make_column<int64_t,  arrow::Int64Builder>(data, n);
        case T::UINT8:  return make_column<uint8_t,  arrow::UInt8Builder>(data, n);
        case T::UINT16: return make_column<uint16_t, arrow::UInt16Builder>(data, n);
        case T::UINT32: return make_column<uint32_t, arrow::UInt32Builder>(data, n);
        case T::UINT64: return make_column<uint64_t, arrow::UInt64Builder>(data, n);
        case T::FLOAT:  return make_column<float,    arrow::FloatBuilder>(data, n);
        case T::DOUBLE: return make_column<double,   arrow::DoubleBuilder>(data, n);
        default: return nullptr;
    }
}

// The Arrow type of an array's column; |S columns holding non-UTF-8 bytes
// are binary.
static std::shared_ptr<arrow::DataType> column_type(const NpyHeader& h, bool binary) {
    using T = arrow::Type;
    if (h.special == 'S') return binary ? arrow::binary() : arrow::utf8();
    if (h.special) return special_type(h);
    switch (h.dtype_id) {
        case T::BOOL:   return arrow::boolean();
        case T::INT8:   return arrow::int8();
        case T::INT16:  return arrow::int16();
        case T::INT32:  return arrow::int32();
        case T::INT64:  return arrow::int64();
        case T::UINT8:  return arrow::uint8();
        case T::UINT16: return arrow::uint16();
        case T::UINT32: return arrow::uint32();
        case T::UINT64: return arrow::uint64();
        case T::FLOAT:  return arrow::float32();
        case T::DOUBLE: return arrow::float64();
        default:        return nullptr;
    }
}

// The bytes of one array's data (what follows its .npy header).
class NpyBytes {
public:
    virtual ~NpyBytes() = default;
    // Bytes [off, off + n) of the data: a pointer into the source when it
    // holds them, else copied into *buf. nullptr with *err set on a failure.
    virtual const uint8_t* get(int64_t off, int64_t n, std::vector<uint8_t>* buf, std::string* err) = 0;
    // Any range costs the same (memory, a mapping); false for a source read
    // in order (a deflated .npz member), where a range far from the last one
    // costs a re-read.
    virtual bool random() const { return true; }
};

// Data in memory or in a mapped file; `keep` owns it.
class ViewBytes final : public NpyBytes {
    std::shared_ptr<const void> keep_;
    const uint8_t*              data_;
    int64_t                     size_;
    const MappedFile*           map_;   // to tell a truncated file, or null
public:
    ViewBytes(std::shared_ptr<const void> keep, const uint8_t* data, int64_t size, const MappedFile* map = nullptr)
        : keep_(std::move(keep)), data_(data), size_(size), map_(map) {}
    const uint8_t* get(int64_t off, int64_t n, std::vector<uint8_t>*, std::string* err) override {
        if (off < 0 || n < 0 || off > size_ || n > size_ - off) {
            *err = "array data ends at byte " + std::to_string(size_);
            return nullptr;
        }
        if (map_ && map_->truncated()) {
            *err = "the file was truncated while it was read";
            return nullptr;
        }
        return data_ + off;
    }
};

// Widest schema an array tab gets: one Arrow field per column, and the table
// view builds every column of the rows it prints (1,455,328 columns: 320 MB
// of schema, 7.9 s and 3.9 GB for `-t -n 3`). A wider array shows its first
// kNpyMaxCols columns as a preview, which exports refuse. An array with no
// rows declares its width without data to back it, so a header could claim
// any width; the cap bounds that too.
static constexpr int64_t kNpyMaxCols = 65536;

// One array as a table read on demand: a block of rows, only the columns
// asked for. 0-D: one cell. 1-D: one column named after the array. 2-D:
// rows × columns c0…. 3-D+: slice i of the leading axis, axis 1 as rows and
// the rest flattened (C order) as columns; change_slice moves the slice.
class NpyArraySource final : public TabularSource {
    std::string               path_, name_;
    bool                      bare_;
    NpyHeader                 h_;
    std::shared_ptr<NpyBytes> bytes_;
    int64_t                   slice_ = 0;
    // Layout of the current view: element (r, c) is
    // base_ + r * row_stride_ + col_offset(c), in elements of the data.
    int64_t                   rows_ = 0, cols_ = 0, full_cols_ = 0, block_ = 1;
    int64_t                   base_ = 0, row_stride_ = 0;
    size_t                    row_axis_ = 0;
    std::vector<int64_t>      strides_;   // F order: element stride per axis
    // The column types, set by types() on first use: an |S column's needs a
    // scan of its values, which tab listing should not pay for.
    mutable std::vector<bool>         binary_;    // per column: |S values not all UTF-8
    mutable std::shared_ptr<arrow::Schema> schema_;
    std::string               footer_;
    mutable arrow::Status     status_;

    int64_t col_offset(int64_t c) const {
        if (!h_.fortran_order || h_.shape.size() < 2) return c;
        int64_t off = 0, rem = c;
        for (size_t axis = h_.shape.size(); axis-- > row_axis_ + 1;) {
            off += (rem % h_.shape[axis]) * strides_[axis];
            rem /= h_.shape[axis];
        }
        return off;
    }
    // Elements [first, last] spanning column c's rows [r0, r0 + n).
    std::pair<int64_t, int64_t> col_span(int64_t c, int64_t r0, int64_t n) const {
        const int64_t first = base_ + r0 * row_stride_ + col_offset(c);
        return {first, first + (n - 1) * row_stride_};
    }
    // Rows [r0, r0 + n) of the columns in `want` (each set column true),
    // gathered one column at a time into contiguous slabs: `fn(c, slab)`.
    template <typename Fn>
    arrow::Status gather(int64_t r0, int64_t n, const std::vector<int64_t>& cols, Fn fn) {
        if (n <= 0 || cols.empty()) {
            for (int64_t c : cols) fn(c, (const uint8_t*)nullptr);
            return arrow::Status::OK();
        }
        const size_t item = h_.item_size;
        // One read covering every requested column's rows: for C order a run
        // of whole rows, at most a block's worth of elements.
        int64_t lo = INT64_MAX, hi = -1;
        for (int64_t c : cols) {
            const auto [a, b] = col_span(c, r0, n);
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        // A source read in order (a deflated member) fetches whole rows of
        // a C-order chunk, which it keeps, so another set of columns of the
        // same rows does not inflate the member again from its start.
        if (!bytes_->random() && !(h_.fortran_order && h_.shape.size() >= 2)) {
            lo = base_ + r0 * row_stride_;
            hi = base_ + (r0 + n) * row_stride_ - 1;
        }
        std::vector<uint8_t> buf, slab((size_t)n * item);
        std::string err;
        const uint8_t* base = bytes_->get(lo * (int64_t)item, (hi - lo + 1) * (int64_t)item, &buf, &err);
        if (!base) return arrow::Status::IOError("'", path_, "' / '", name_, "': ", err);
        for (int64_t c : cols) {
            const int64_t first = col_span(c, r0, n).first - lo;
            if (row_stride_ == 1) {
                std::memcpy(slab.data(), base + first * item, (size_t)n * item);
            } else {
                for (int64_t r = 0; r < n; ++r)
                    std::memcpy(slab.data() + (size_t)r * item, base + (first + r * row_stride_) * item, item);
            }
            fn(c, slab.data());
        }
        return arrow::Status::OK();
    }

    void layout() {
        const auto& sh = h_.shape;
        const std::string fmt = std::string("Format: NumPy ") + (bare_ ? "NPY" : "NPZ");
        base_ = 0;
        row_axis_ = 0;
        strides_.assign(sh.size(), 1);
        for (size_t i = 1; i < sh.size(); ++i) strides_[i] = strides_[i - 1] * sh[i - 1];
        if (sh.empty()) {
            rows_ = 1; cols_ = 1; row_stride_ = 1;
            footer_ = fmt + "  |  Array: " + name_ + "  |  scalar  |  dtype: " + h_.dtype_str;
        } else if (sh.size() == 1) {
            rows_ = sh[0]; cols_ = 1; row_stride_ = 1;
            footer_ = fmt + "  |  Array: " + name_ + "  |  " + shape_str(sh) + "  |  dtype: " + h_.dtype_str;
        } else if (sh.size() == 2) {
            rows_ = sh[0]; cols_ = sh[1];
            row_stride_ = h_.fortran_order ? 1 : sh[1];
            footer_ = fmt + "  |  Array: " + name_ + "  |  " + shape_str(sh) + "  |  dtype: " + h_.dtype_str;
        } else if (sh[0] <= 0) {
            // Empty leading axis (e.g. shape (0, …)): nothing to slice.
            rows_ = 0; cols_ = 1; row_stride_ = 1;
            footer_ = fmt + "  |  Array: " + name_ + "  |  " + shape_str(sh) + "  |  dtype: " + h_.dtype_str +
                      "  |  empty";
        } else {
            slice_ = std::clamp<int64_t>(slice_, 0, sh[0] - 1);
            rows_ = sh[1];
            cols_ = 1;
            for (size_t i = 2; i < sh.size(); ++i) cols_ *= sh[i];
            row_axis_ = 1;
            if (h_.fortran_order) {
                base_ = slice_;                // strides_[0] == 1
                row_stride_ = strides_[1];
            } else {
                base_ = slice_ * rows_ * cols_;
                row_stride_ = cols_;
            }
            std::string desc = "[" + std::to_string(slice_) + ", :, :";
            for (size_t i = 3; i < sh.size(); ++i) desc += ", :";
            desc += "]";
            footer_ = fmt + "  |  Array: " + name_ + "  |  " + shape_str(sh) + "  |  dtype: " + h_.dtype_str +
                      "  |  slice " + desc + " (" + std::to_string(slice_ + 1) + "/" + std::to_string(sh[0]) +
                      ")  |  [ / ] step slice  •  :slice N jumps";
        }
        full_cols_ = cols_;
        cols_ = std::min(cols_, kNpyMaxCols);
        if (cols_ < full_cols_)
            footer_ += "  |  showing first " + std::to_string(cols_) + " of " + std::to_string(full_cols_) +
                       " columns";
        block_ = std::clamp<int64_t>((int64_t{1} << 20) / std::max<int64_t>(cols_, 1), 1, 65536);
        schema_.reset();
    }

    void types() const {
        if (schema_) return;
        auto* self = const_cast<NpyArraySource*>(this);
        // |S: a column is binary when any of its values is not UTF-8, as the
        // whole-column build decided.
        binary_.assign((size_t)cols_, false);
        if (h_.special == 'S' && rows_ > 0) {
            std::vector<int64_t> all((size_t)cols_);
            for (int64_t c = 0; c < cols_; ++c) all[(size_t)c] = c;
            for (int64_t r0 = 0; r0 < rows_ && status_.ok(); r0 += block_) {
                const int64_t n = std::min(block_, rows_ - r0);
                status_ = self->gather(r0, n, all, [&](int64_t c, const uint8_t* slab) {
                    if (binary_[(size_t)c]) return;
                    for (int64_t r = 0; r < n && !binary_[(size_t)c]; ++r) {
                        const uint8_t* p = slab + (size_t)r * h_.item_size;
                        size_t len = h_.item_size;
                        while (len > 0 && p[len - 1] == 0) --len;
                        std::string v((const char*)p, len);
                        if (valid_utf8(v) != v) binary_[(size_t)c] = true;
                    }
                });
            }
        }
        arrow::FieldVector fields;
        fields.reserve((size_t)cols_);
        if (h_.shape.size() <= 1) {
            fields.push_back(arrow::field(name_, column_type(h_, cols_ > 0 && binary_[0])));
        } else {
            for (int64_t c = 0; c < cols_; ++c)
                fields.push_back(arrow::field("c" + std::to_string(c), column_type(h_, binary_[(size_t)c])));
        }
        schema_ = arrow::schema(std::move(fields));
    }

public:
    NpyArraySource(std::string path, std::string name, bool bare, NpyHeader h, std::shared_ptr<NpyBytes> bytes)
        : path_(std::move(path)), name_(std::move(name)), bare_(bare), h_(std::move(h)), bytes_(std::move(bytes)) {
        layout();
    }
    std::shared_ptr<arrow::Schema> schema() const override { types(); return schema_; }
    int64_t total_rows() const override { return rows_; }
    int num_chunks() const override { return (int)((rows_ + block_ - 1) / block_); }
    ChunkMeta chunk_meta(int i) const override {
        const int64_t first = (int64_t)i * block_;
        return {first, std::max<int64_t>(0, std::min(block_, rows_ - first))};
    }
    bool random_access() const override { return true; }
    arrow::Status read_status() const override { types(); return status_; }
    const std::string& path() const override { return path_; }
    std::string footer() const override { return footer_; }
    std::string tab_label() const override { return name_; }
    PreviewLimit preview_limit() const override {
        if (cols_ >= full_cols_) return PreviewLimit{};
        return PreviewLimit{rows_, rows_, cols_, full_cols_};
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        types();
        ARROW_RETURN_NOT_OK(status_);
        const ChunkMeta m = chunk_meta(i);
        std::vector<int64_t> want;   // distinct, in order
        for (int c : cols) {
            if (c < 0 || c >= cols_) return arrow::Status::IndexError("column ", c, " of ", cols_);
            want.push_back(c);
        }
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        std::unordered_map<int64_t, std::shared_ptr<arrow::Array>> by_col;
        auto st = gather(m.first_row, m.num_rows, want, [&](int64_t c, const uint8_t* slab) {
            static const uint8_t kNone = 0;
            by_col[c] = slab_to_arrow(h_, slab ? slab : &kNone, m.num_rows, binary_[(size_t)c]);
        });
        if (!st.ok()) {
            status_ = st;
            return st;
        }
        arrow::FieldVector fields;
        arrow::ArrayVector arrays;
        for (int c : cols) {
            fields.push_back(schema_->field(c));
            arrays.push_back(by_col[c]);
        }
        *out = arrow::Table::Make(arrow::schema(std::move(fields)), arrays, m.num_rows);
        return arrow::Status::OK();
    }
    bool has_slices() const override { return h_.shape.size() >= 3; }
    bool change_slice(int delta, bool absolute, int64_t target) override {
        if (h_.shape.size() < 3 || h_.shape[0] <= 0) return false;
        int64_t idx = absolute ? target : slice_ + delta;
        idx = std::clamp<int64_t>(idx, 0, h_.shape[0] - 1);
        if (idx == slice_) return false;
        slice_ = idx;
        status_ = arrow::Status::OK();
        layout();
        return true;
    }
};

// One ZIP member (one .npy): its name, parsed header and where its bytes
// are. Only the header is read at open; the data when its tab reads it.
struct Entry {
    std::string name;                 // without .npy suffix
    NpyHeader   header;
    uint64_t    cd_pos   = 0;         // its central-directory entry (unzSetOffset64)
    bool        stored   = false;     // compression method 0
    int64_t     data_pos = -1;        // stored: file offset of the member's first byte, -1 unknown
    uint64_t    size     = 0;         // declared uncompressed size
};

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

// File offset of a member's first data byte, read from its central-directory
// entry at `cd` and its local header (ZIP64 offsets included); -1 when a
// signature or the .npy magic there does not match, and the member is then
// read through the zip library instead.
static int64_t member_data_pos(const MappedFile& f, uint64_t cd) {
    const uint8_t* d = f.data();
    const uint64_t n = f.size();
    if (cd > n || n - cd < 46 || le32(d + cd) != 0x02014b50) return -1;
    const uint8_t* c = d + cd;
    const uint16_t name_len = le16(c + 28), extra_len = le16(c + 30);
    if (n - cd < 46u + name_len + extra_len) return -1;
    uint64_t local = le32(c + 42);
    if (local == 0xFFFFFFFFu) {
        // ZIP64 extra field: the 8-byte values present are those whose
        // 32-bit field is 0xFFFFFFFF, in the order uncompressed, compressed,
        // local header offset.
        const uint8_t* x = c + 46 + name_len;
        const uint8_t* end = x + extra_len;
        bool found = false;
        while (x + 4 <= end) {
            const uint16_t id = le16(x), len = le16(x + 2);
            if (x + 4 + len > end) break;
            if (id == 0x0001) {
                size_t at = 0;
                if (le32(c + 24) == 0xFFFFFFFFu) at += 8;
                if (le32(c + 20) == 0xFFFFFFFFu) at += 8;
                if (at + 8 > len) return -1;
                local = le64(x + 4 + at);
                found = true;
                break;
            }
            x += 4 + len;
        }
        if (!found) return -1;
    }
    if (local > n || n - local < 30 || le32(d + local) != 0x04034b50) return -1;
    const uint64_t data = local + 30 + le16(d + local + 26) + le16(d + local + 28);
    if (data > n || n - data < 6 || std::memcmp(d + data, "\x93NUMPY", 6) != 0) return -1;
    return (int64_t)data;
}

// The first bytes of a member, through the end of its .npy header.
// `next(buf, n)` returns up to n further member bytes: 0 at the member's end,
// negative on a read error.
template <class Next>
static std::string read_member_header(Next next, std::vector<uint8_t>* out) {
    auto read_to = [&](size_t want) -> bool {
        while (out->size() < want) {
            uint8_t chunk[64 * 1024];
            const int r = next(chunk, (unsigned)std::min<size_t>(sizeof(chunk), want - out->size()));
            if (r <= 0) return r == 0;
            out->insert(out->end(), chunk, chunk + r);
        }
        return true;
    };
    out->clear();
    if (!read_to(12)) return "read error";
    if (out->size() < 10) return "";   // parse_npy_header reports it
    const uint8_t major = (*out)[6];
    size_t need = 0;
    if (major == 1) need = 10 + le16(out->data() + 8);
    else if ((major == 2 || major == 3) && out->size() >= 12) need = 12 + (size_t)le32(out->data() + 8);
    else return "";
    if (need > (size_t{1} << 24)) return "npy: header of " + std::to_string(need) + " bytes";
    if (!read_to(need)) return "read error";
    return "";
}

// List the archive: each member's name, header and location. Member data is
// not read.
static std::string load_archive(const std::string& path, const MappedFile* map, std::vector<Entry>* out) {
    unzFile zf = unzOpen(path.c_str());
    if (!zf) return "Cannot open '" + path + "' as NPZ (zip)";
    std::unique_ptr<void, int (*)(unzFile)> guard(zf, unzClose);
    if (unzGoToFirstFile(zf) != UNZ_OK) return "'" + path + "': NPZ archive is empty";
    do {
        char name_buf[1024] = {0};
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(zf, &info, name_buf, sizeof(name_buf) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
            return "'" + path + "': cannot read NPZ entry metadata";
        std::string name = name_buf;
        // Strip .npy suffix.
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".npy") == 0)
            name.resize(name.size() - 4);
        Entry e;
        e.name   = name;
        e.cd_pos = (uint64_t)unzGetOffset64(zf);
        e.stored = info.compression_method == 0;
        e.size   = (uint64_t)info.uncompressed_size;
        // A stored member's header is read from the mapped archive, so it
        // does not depend on the zip library's ZIP64 support (minizip 1.1
        // on LP64 does not read ZIP64 local-header offsets).
        if (e.stored && map) e.data_pos = member_data_pos(*map, e.cd_pos);
        std::vector<uint8_t> head;
        std::string err;
        if (e.data_pos >= 0) {
            const uint8_t* p = map->data() + e.data_pos;
            uint64_t left = std::min<uint64_t>(e.size, map->size() - (uint64_t)e.data_pos);
            err = read_member_header([&](uint8_t* buf, unsigned n) -> int {
                const unsigned k = (unsigned)std::min<uint64_t>(n, left);
                std::memcpy(buf, p, k);
                p += k;
                left -= k;
                return (int)k;
            }, &head);
        } else {
            if (unzOpenCurrentFile(zf) != UNZ_OK)
                return "'" + path + "': cannot open '" + name + "' inside NPZ";
            err = read_member_header([&](uint8_t* buf, unsigned n) {
                return unzReadCurrentFile(zf, buf, n);
            }, &head);
            unzCloseCurrentFile(zf);
        }
        if (err == "read error") return "'" + path + "': read error in '" + name + "'";
        if (!err.empty()) return "'" + path + "' / '" + name + "': " + err;
        // `uncompressed_size` comes from the central directory, so it is
        // untrusted: the data is checked against it here, and against what
        // the member really holds when it is read.
        err = parse_npy_header(head.data(), head.size(), &e.header, e.size);
        if (!err.empty()) return "'" + path + "' / '" + name + "': " + err;
        // A ZIP may carry two members with the same name; numpy's np.load keeps
        // the last (its central-directory dict overwrites earlier keys), so the
        // earlier bytes are dead. Match that — overwrite the existing entry in
        // place (its first position, the later value) instead of appending a
        // second, unreachable tab. Warn so the replaced member is not dropped
        // silently.
        bool merged = false;
        for (auto& prev : *out) {
            if (prev.name == name) {
                std::fprintf(stderr,
                    "vv: NPZ '%s': member '%s' appears more than once; "
                    "showing the last, as numpy does\n",
                    path.c_str(), name.c_str());
                prev = std::move(e);
                merged = true;
                break;
            }
        }
        if (!merged) out->push_back(std::move(e));
    } while (unzGoToNextFile(zf) == UNZ_OK);
    return "";
}

// A deflated member's data, inflated in order through the zip library:
// get() reads forward from where the last one stopped, and a range before it
// inflates the member again from its start. Only the range asked for is
// kept.
class ZipStreamBytes final : public NpyBytes {
    std::string path_, name_;
    uint64_t    cd_pos_, skip_;      // skip_: the .npy header's bytes
    unzFile     zf_  = nullptr;
    uint64_t    pos_ = 0;            // member bytes inflated so far
    // The last range read, kept: a range inside it is served from here.
    std::vector<uint8_t> last_;
    int64_t              last_off_ = -1;
    void close() {
        if (zf_) { unzCloseCurrentFile(zf_); unzClose(zf_); zf_ = nullptr; }
    }
    bool reopen(std::string* err) {
        close();
        pos_ = 0;
        zf_ = unzOpen(path_.c_str());
        if (!zf_ || unzSetOffset64(zf_, cd_pos_) != UNZ_OK || unzOpenCurrentFile(zf_) != UNZ_OK) {
            close();
            *err = "cannot open '" + name_ + "' inside NPZ";
            return false;
        }
        return true;
    }
    // Inflate n bytes, appended to *dst (null: discarded). *dst grows with
    // the bytes inflated, not with n: sizes come from the archive and are
    // untrusted, so a member that is shorter fails here at its real end.
    bool read(std::vector<uint8_t>* dst, uint64_t n, std::string* err) {
        uint8_t chunk[64 * 1024];
        while (n > 0) {
            const unsigned want = (unsigned)std::min<uint64_t>(n, sizeof(chunk));
            const int r = unzReadCurrentFile(zf_, chunk, want);
            if (r <= 0) {
                *err = r < 0 ? "read error in '" + name_ + "'"
                             : "'" + name_ + "' ends at byte " + std::to_string(pos_) + ", before its data does";
                close();
                return false;
            }
            if (dst) dst->insert(dst->end(), chunk, chunk + r);
            pos_ += (uint64_t)r;
            n -= (uint64_t)r;
        }
        return true;
    }
public:
    ZipStreamBytes(std::string path, std::string name, uint64_t cd_pos, uint64_t header_bytes)
        : path_(std::move(path)), name_(std::move(name)), cd_pos_(cd_pos), skip_(header_bytes) {}
    ~ZipStreamBytes() override { close(); }
    bool random() const override { return false; }
    const uint8_t* get(int64_t off, int64_t n, std::vector<uint8_t>* buf, std::string* err) override {
        if (last_off_ >= 0 && off >= last_off_ && off + n <= last_off_ + (int64_t)last_.size())
            return last_.data() + (off - last_off_);
        (void)buf;
        const uint64_t at = skip_ + (uint64_t)off;
        if (!zf_ || at < pos_)
            if (!reopen(err)) return nullptr;
        last_off_ = -1;
        if (!read(nullptr, at - pos_, err)) return nullptr;
        last_.clear();
        last_.reserve((size_t)std::min<int64_t>(n, int64_t{64} << 20));
        if (!read(&last_, (uint64_t)n, err)) return nullptr;
        last_off_ = off;
        static const uint8_t kEmpty = 0;
        return last_.empty() ? &kEmpty : last_.data();
    }
};

// A deflated member read whole on first use (an F-order array, whose
// columns are far apart in the member), then served from memory.
class ZipWholeBytes final : public NpyBytes {
    ZipStreamBytes        stream_;   // keeps the whole member as its last range
    uint64_t              size_;
    const uint8_t*        data_ = nullptr;
    int64_t               have_ = 0;
    bool                  loaded_ = false;
    std::string           error_;
public:
    ZipWholeBytes(std::string path, std::string name, uint64_t cd_pos, uint64_t header_bytes, uint64_t data_size)
        : stream_(std::move(path), std::move(name), cd_pos, header_bytes), size_(data_size) {}
    const uint8_t* get(int64_t off, int64_t n, std::vector<uint8_t>*, std::string* err) override {
        if (!loaded_) {
            loaded_ = true;
            std::vector<uint8_t> unused;
            data_ = stream_.get(0, (int64_t)size_, &unused, &error_);
            have_ = data_ ? (int64_t)size_ : 0;
        }
        if (!error_.empty()) { *err = error_; return nullptr; }
        if (off < 0 || n < 0 || off > have_ || n > have_ - off) {
            *err = "array data ends at byte " + std::to_string(have_);
            return nullptr;
        }
        return data_ + off;
    }
};

// The summary tab's table: one row per member.
static std::shared_ptr<arrow::Table> summary_table(const std::vector<Entry>& a) {
    arrow::StringBuilder nb, sb, db, kb;
    for (const auto& e : a) {
        (void)nb.Append(e.name);
        (void)sb.Append(shape_str(e.header.shape));
        (void)db.Append(e.header.dtype_str);
        std::string kind;
        if (e.header.unsupported) {
            // descr is endian + kind + size ("<U11", "|O", "<M8[ns]").
            const char k = e.header.dtype_str.size() >= 2 ? e.header.dtype_str[1] : '?';
            kind = k == 'O' ? "(pickled / object — skipped)"
                 : (k == 'S' || k == 'U') ? "(strings — unsupported width / byte order)"
                 : (k == 'M' || k == 'm') ? "(datetime — unsupported unit / byte order)"
                 : "(unsupported dtype — skipped)";
        }
        else if (e.header.shape.empty()) kind = "scalar";
        else if (e.header.shape.size() == 1) kind = "1-D";
        else if (e.header.shape.size() == 2) kind = "2-D";
        else kind = std::to_string(e.header.shape.size()) + "-D";
        (void)kb.Append(kind);
    }
    std::shared_ptr<arrow::Array> na, sa, da, ka;
    (void)nb.Finish(&na); (void)sb.Finish(&sa);
    (void)db.Finish(&da); (void)kb.Finish(&ka);
    auto schema = arrow::schema({
        arrow::field("name",  arrow::utf8()),
        arrow::field("shape", arrow::utf8()),
        arrow::field("dtype", arrow::utf8()),
        arrow::field("kind",  arrow::utf8()),
    });
    return arrow::Table::Make(schema, {na, sa, da, ka}, (int64_t)a.size());
}

// A member's data as NpyBytes: a stored member found in the mapped
// archive is read from the mapping; any other is inflated in order (C order
// or one axis) or whole on first use (Fortran order).
static std::shared_ptr<NpyBytes> entry_bytes(const std::string& path, const Entry& e,
                                             const std::shared_ptr<const MappedFile>& map) {
    const NpyHeader& h = e.header;
    const uint64_t data_size = e.size >= h.data_offset ? e.size - h.data_offset : 0;
    if (e.stored && e.data_pos >= 0 && map) {
        const uint64_t at = (uint64_t)e.data_pos + h.data_offset;
        const uint64_t have = at <= map->size() ? map->size() - at : 0;
        return std::make_shared<ViewBytes>(map, map->data() + std::min<uint64_t>(at, map->size()),
                                           (int64_t)std::min(have, data_size), map.get());
    }
    if (h.fortran_order && h.shape.size() >= 2)
        return std::make_shared<ZipWholeBytes>(path, e.name, e.cd_pos, h.data_offset, data_size);
    return std::make_shared<ZipStreamBytes>(path, e.name, e.cd_pos, h.data_offset);
}

#ifdef VV_FUZZ
// Fuzz entry (see tests/fuzz/fuzz_npy.cpp): parse an untrusted .npy buffer and
// read its table, as an array tab does. parse_npy_header validates the
// declared shape against the buffer size, so an accepted header never drives
// the reads out of bounds — the harness fuzzes the real parse + read path
// (header parsing, the layout, every chunk of every column, a slice step).
void npy_fuzz_one(const uint8_t* buf, size_t n) {
    NpyHeader h;
    if (!parse_npy_header(buf, n, &h).empty()) return;
    if (h.unsupported) return;
    auto bytes = std::make_shared<ViewBytes>(nullptr, buf + h.data_offset, (int64_t)(n - h.data_offset));
    NpyArraySource src("fuzz.npy", "x", true, h, bytes);
    std::vector<int> cols;
    for (int c = 0; c < src.schema()->num_fields() && c < 64; ++c) cols.push_back(c);
    for (int i = 0; i < src.num_chunks() && i < 64; ++i) {
        std::shared_ptr<arrow::Table> t;
        (void)src.read_chunk(i, cols, &t);
    }
    if (src.has_slices()) (void)src.change_slice(1, false, 0);
}
#endif

// The summary tab of an .npz; its siblings are the array tabs.
class NpzSource : public WorkbookSource {
    std::shared_ptr<std::vector<Entry>> archive_;
    std::shared_ptr<const MappedFile>   map_;      // the archive, for stored members

    NpzSource(std::shared_ptr<arrow::Table> table, std::string path, std::string footer,
              std::shared_ptr<std::vector<Entry>> archive, std::shared_ptr<const MappedFile> map)
        : WorkbookSource(std::move(table), std::move(path), std::move(footer)),
          archive_(std::move(archive)), map_(std::move(map)) {}

public:
    // A bare .npy is a single array with no container around it: one array
    // tab over the file, mapped, with the NPZ path's parser and hardening.
    static std::string open_npy(const std::string& path, std::unique_ptr<TabularSource>* out) {
        auto map = std::make_shared<MappedFile>();
        if (auto e = map->open(path); !e.empty()) return e;
        if (map->size() == 0) return "'" + path + "': empty file";
        // Tab label: the basename without the .npy suffix, mirroring how an
        // in-archive member is named.
        std::string name = path;
        if (auto slash = name.find_last_of('/'); slash != std::string::npos) name.erase(0, slash + 1);
        if (name.size() > 4 && fends_ci(name, ".npy")) name.erase(name.size() - 4);
        if (name.empty()) name = "array";
        NpyHeader h;
        std::string err = parse_npy_header(map->data(), map->size(), &h);
        if (!err.empty()) return "'" + path + "': " + err;
        if (h.unsupported)
            return "'" + path + "': unsupported dtype '" + h.dtype_str +
                   "' (object / structured arrays are not displayed)";
        const uint8_t* data = map->data() + h.data_offset;
        const int64_t size = (int64_t)(map->size() - h.data_offset);
        const MappedFile* m = map.get();
        auto bytes = std::make_shared<ViewBytes>(std::move(map), data, size, m);
        *out = std::make_unique<NpyArraySource>(path, name, true, std::move(h), std::move(bytes));
        return "";
    }

    static std::string open_first(const std::string& path, std::unique_ptr<NpzSource>* out) {
        auto archive = std::make_shared<std::vector<Entry>>();
        // Stored members (np.savez) are read straight from a mapping; without
        // one (an empty or unmappable file) every member goes through the zip
        // library.
        auto map = std::make_shared<MappedFile>();
        std::shared_ptr<const MappedFile> mapped;
        if (map->open(path).empty() && map->size() > 0) mapped = map;
        std::string err = load_archive(path, mapped.get(), archive.get());
        if (!err.empty()) return err;
        if (archive->empty()) return "'" + path + "': NPZ has no arrays";
        size_t tabs = 0;
        for (const auto& e : *archive) tabs += !e.header.unsupported;
        std::string footer = "Format: NumPy NPZ  |  Tab: summary  |  Arrays: " + std::to_string(archive->size());
        if (tabs) footer += "  |  +" + std::to_string(tabs) + " more tab(s)";
        auto table = summary_table(*archive);
        out->reset(new NpzSource(std::move(table), path, std::move(footer), std::move(archive), std::move(mapped)));
        return "";
    }

    std::string tab_label() const override { return "summary"; }

    // One tab per displayable array, in archive order.
    std::vector<std::unique_ptr<TabularSource>> open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> result;
        for (const auto& e : *archive_) {
            if (e.header.unsupported) continue;
            result.push_back(std::make_unique<NpyArraySource>(path(), e.name, false, e.header,
                                                              entry_bytes(path(), e, map_)));
        }
        return result;
    }
};

std::string open_npz_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<NpzSource> s;
    std::string e = NpzSource::open_first(path, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
std::string open_npy_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    return NpzSource::open_npy(path, out);
}

}  // namespace npz
