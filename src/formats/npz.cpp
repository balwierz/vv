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

static std::string parse_npy_header(const uint8_t* buf, size_t n, NpyHeader* out) {
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
        uint64_t avail = (uint64_t)n - (uint64_t)out->data_offset;  // data_offset<=n
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
// overflows) is null, as is NaT.
static std::shared_ptr<arrow::Array> decode_special(const NpyHeader& h, const uint8_t* data, int64_t n) {
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
        if (all_utf8) {
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
slab_to_arrow(const NpyHeader& h, const uint8_t* data, int64_t n) {
    using T = arrow::Type;
    if (h.special) return decode_special(h, data, n);
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

// Convert a 1-D slab to a single-column table.
static std::shared_ptr<arrow::Table>
build_1d_table(const std::string& name, const NpyHeader& h,
                const uint8_t* data, int64_t n) {
    auto col = slab_to_arrow(h, data, n);
    if (!col) return nullptr;
    auto schema = arrow::schema({arrow::field(name, col->type())});
    return arrow::Table::Make(schema, {col}, n);
}

// Convert a 2-D slab (rows × cols) C-contiguous to an Arrow table.
// Cap on the number of columns rendered from a 2-D array: one Arrow Array +
// Field is built per column, so a genuinely-wide array (or a hostile one that
// passes the shape/buffer bounds check) would otherwise allocate unboundedly.
// The full width is reported to the caller so the footer can flag truncation.
static constexpr int64_t kNpzMaxCols = 4096;

// fortran_order arrays would have transposed memory layout — we handle
// the common (False) case; F-order falls back to column-major read.
// Only the first kNpzMaxCols columns are materialised; `full_cols_out` (when
// non-null) receives the declared width so callers can note any truncation.
static std::shared_ptr<arrow::Table>
build_2d_table(const NpyHeader& h, const uint8_t* data,
                int64_t rows, int64_t cols, size_t item_size,
                bool fortran_order, int64_t* full_cols_out = nullptr) {
    if (full_cols_out) *full_cols_out = cols;
    const int64_t full_cols = cols;        // row stride in the C-order buffer
    if (cols > kNpzMaxCols) cols = kNpzMaxCols;
    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols_out;
    std::vector<uint8_t> tmp;
    for (int64_t c = 0; c < cols; ++c) {
        // Gather column c. C-order: stride = full_cols * item_size between rows
        // (the declared width, even when capped). F-order: column is contiguous
        // (stride = item_size).
        tmp.assign(rows * item_size, 0);
        // rows == 0 leaves tmp empty, so tmp.data() is null and the F-order
        // gather would call memcpy(nullptr, …, 0) — a zero length does not
        // make a null argument legal (the parameters are declared
        // non-null), and UBSan traps it. The C-order loop below is already a
        // no-op at rows == 0; guard both so the intent is explicit.
        if (rows > 0) {
            if (fortran_order) {
                std::memcpy(tmp.data(), data + c * rows * item_size,
                            rows * item_size);
            } else {
                for (int64_t r = 0; r < rows; ++r) {
                    std::memcpy(tmp.data() + r * item_size,
                                data + (r * full_cols + c) * item_size,
                                item_size);
                }
            }
        }
        auto a = slab_to_arrow(h, tmp.data(), rows);
        if (!a) return nullptr;
        fields.push_back(arrow::field("c" + std::to_string(c), a->type()));
        cols_out.push_back(a);
    }
    return arrow::Table::Make(arrow::schema(fields), cols_out, rows);
}

// One ZIP entry (one .npy) — name, full extracted bytes, parsed header.
struct Entry {
    std::string                       name;     // without .npy suffix
    std::shared_ptr<std::vector<uint8_t>> bytes;
    NpyHeader                         header;
};

// Read all entries from the .npz archive into memory. The .npy bodies are
// kept compressed-in / decompressed-out: minizip handles inflation on
// read. We don't stream — once the file's on disk, materialising the
// arrays in RAM is the simplest path (and they're typically small).
static std::string load_archive(const std::string& path,
                                  std::vector<Entry>* out) {
    unzFile zf = unzOpen(path.c_str());
    if (!zf) return "Cannot open '" + path + "' as NPZ (zip)";

    if (unzGoToFirstFile(zf) != UNZ_OK) {
        unzClose(zf);
        return "'" + path + "': NPZ archive is empty";
    }

    do {
        char name_buf[1024] = {0};
        unz_file_info info{};
        if (unzGetCurrentFileInfo(zf, &info, name_buf, sizeof(name_buf) - 1,
                                   nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(zf);
            return "'" + path + "': cannot read NPZ entry metadata";
        }
        std::string name = name_buf;
        // Strip .npy suffix.
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".npy") == 0)
            name.resize(name.size() - 4);

        if (unzOpenCurrentFile(zf) != UNZ_OK) {
            unzClose(zf);
            return "'" + path + "': cannot open '" + name + "' inside NPZ";
        }
        auto buf = std::make_shared<std::vector<uint8_t>>();
        // `info.uncompressed_size` is from the zip central directory — i.e.
        // attacker-controllable. Reserving it blindly lets a tiny crafted entry
        // that claims gigabytes force a huge allocation (OOM / crash). It's only
        // a pre-sizing hint (the read loop grows the vector as needed), so clamp
        // it to a sane ceiling; a genuinely large array still loads via the loop.
        constexpr uint64_t kMaxReserve = 64u << 20;   // 64 MiB
        buf->reserve((size_t)std::min<uint64_t>(
            (uint64_t)info.uncompressed_size, kMaxReserve));
        uint8_t chunk[64 * 1024];
        while (true) {
            int n = unzReadCurrentFile(zf, chunk, sizeof(chunk));
            if (n < 0) {
                unzCloseCurrentFile(zf); unzClose(zf);
                return "'" + path + "': read error in '" + name + "'";
            }
            if (n == 0) break;
            buf->insert(buf->end(), chunk, chunk + n);
        }
        unzCloseCurrentFile(zf);

        NpyHeader h;
        std::string err = parse_npy_header(buf->data(), buf->size(), &h);
        if (!err.empty()) {
            unzClose(zf);
            return "'" + path + "' / '" + name + "': " + err;
        }
        // A ZIP may carry two members with the same name; numpy's np.load keeps
        // the last (its central-directory dict overwrites earlier keys), so the
        // earlier bytes are dead. Match that — overwrite the existing entry in
        // place (its first position, the later value) instead of appending a
        // second, unreachable tab that find_entry would always shadow. Warn so
        // the replaced member is not dropped silently.
        bool merged = false;
        for (auto& prev : *out) {
            if (prev.name == name) {
                std::fprintf(stderr,
                    "vv: NPZ '%s': member '%s' appears more than once; "
                    "showing the last, as numpy does\n",
                    path.c_str(), name.c_str());
                prev.bytes  = std::move(buf);
                prev.header = std::move(h);
                merged = true;
                break;
            }
        }
        if (!merged)
            out->push_back({std::move(name), std::move(buf), std::move(h)});
    } while (unzGoToNextFile(zf) == UNZ_OK);

    unzClose(zf);
    return "";
}

// Spec for one tab: either the summary or one named array. Slice index
// applies only to 3-D+ arrays; otherwise -1.
struct OpenSpec {
    // A bare .npy has no container, so its footer says NPY rather than NPZ
    // and it has no summary tab.
    bool        bare_npy   = false;
    bool        is_summary = false;
    std::string entry_name;          // key into archive
    int64_t     slice_idx = 0;
};

#ifdef VV_FUZZ
// Fuzz entry (see tests/fuzz/fuzz_npy.cpp): parse an untrusted .npy buffer and
// build its table, mirroring NpzSource's shape dispatch. parse_npy_header
// validates the declared shape against the buffer size, so an accepted header
// never drives the builders out of bounds — the harness fuzzes the real
// parse + build path (header parsing, slab_to_arrow, build_1d/2d_table).
void npy_fuzz_one(const uint8_t* buf, size_t n) {
    NpyHeader h;
    if (!parse_npy_header(buf, n, &h).empty()) return;
    if (h.unsupported) return;
    const uint8_t* data = buf + h.data_offset;
    std::shared_ptr<arrow::Table> tbl;
    if (h.shape.empty()) {
        (void)slab_to_arrow(h, data, 1);
    } else if (h.shape.size() == 1) {
        tbl = build_1d_table("x", h, data, h.shape[0]);
    } else if (h.shape.size() == 2) {
        int64_t fc = 0;
        tbl = build_2d_table(h, data, h.shape[0], h.shape[1],
                             h.item_size, h.fortran_order, &fc);
    } else {
        int64_t leading = h.shape[0];
        if (leading > 0) {
            int64_t rest = 1;   // product of the trailing dims (bounded: the full
            for (size_t i = 1; i < h.shape.size(); ++i) rest *= h.shape[i]; // product fit `n`)
            tbl = build_2d_table(h, data, leading, rest,
                                 h.item_size, h.fortran_order);
        } else {
            tbl = build_2d_table(h, data, 0, 1, h.item_size, h.fortran_order);
        }
    }
    (void)tbl;
}
#endif

class NpzSource : public WorkbookSource {
    std::shared_ptr<std::vector<Entry>> archive_;   // shared across siblings
    OpenSpec    spec_;
    std::vector<OpenSpec> siblings_;
    PreviewLimit limit_;   // set when a 2-D / sliced array exceeds kNpzMaxCols

    NpzSource(std::shared_ptr<arrow::Table> table,
               std::string path,
               std::string footer,
               std::shared_ptr<std::vector<Entry>> archive,
               OpenSpec spec,
               std::vector<OpenSpec> siblings)
        : WorkbookSource(std::move(table), std::move(path),
                          std::move(footer)),
          archive_(std::move(archive)),
          spec_(std::move(spec)),
          siblings_(std::move(siblings)) {}

    // Locate an entry by name. Returns nullptr if not found.
    static const Entry* find_entry(const std::vector<Entry>& a,
                                     const std::string& nm) {
        for (const auto& e : a) if (e.name == nm) return &e;
        return nullptr;
    }

    // Build the table for a given spec. Returns "" on success.
    static std::string build_table(const std::vector<Entry>& a,
                                    const OpenSpec& spec,
                                    std::shared_ptr<arrow::Table>* tbl,
                                    std::string* footer,
                                    PreviewLimit* limit = nullptr) {
        if (limit) *limit = PreviewLimit{};
        if (spec.is_summary) {
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
            *tbl = arrow::Table::Make(schema, {na, sa, da, ka},
                                       (int64_t)a.size());
            *footer = "Format: NumPy NPZ  |  Tab: summary  |  Arrays: "
                    + std::to_string(a.size());
            return "";
        }

        const Entry* e = find_entry(a, spec.entry_name);
        if (!e) return "NPZ: entry '" + spec.entry_name + "' not found";
        const auto& h = e->header;
        if (h.unsupported) {
            return "'" + spec.entry_name + "': dtype " + h.dtype_str +
                   " (object/structured) not displayable. "
                   "Convert to a fixed numeric dtype with python.";
        }
        const uint8_t* data = e->bytes->data() + h.data_offset;

        if (h.shape.empty()) {
            // 0-D scalar — render as a single-cell table.
            auto col = slab_to_arrow(h, data, 1);
            if (!col) return "NPZ: dtype not supported for '" + e->name + "'";
            auto schema = arrow::schema({arrow::field(e->name, col->type())});
            *tbl = arrow::Table::Make(schema, {col}, 1);
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  scalar  |  dtype: " + h.dtype_str;
            return "";
        }

        if (h.shape.size() == 1) {
            *tbl = build_1d_table(e->name, h, data, h.shape[0]);
            if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  " + shape_str(h.shape) +
                      "  |  dtype: " + h.dtype_str;
            return "";
        }

        if (h.shape.size() == 2) {
            int64_t full_c = 0;
            *tbl = build_2d_table(h, data, h.shape[0], h.shape[1],
                                   h.item_size, h.fortran_order, &full_c);
            if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  " + shape_str(h.shape) +
                      "  |  dtype: " + h.dtype_str;
            if (limit)
                *limit = PreviewLimit{(*tbl)->num_rows(), (*tbl)->num_rows(),
                                      (*tbl)->num_columns(), full_c};
            if ((*tbl)->num_columns() < full_c)
                *footer += "  |  showing first " +
                           std::to_string((*tbl)->num_columns()) + " of " +
                           std::to_string(full_c) + " columns";
            return "";
        }

        // 3-D+. Render a 2-D slice along the leading axis.
        int64_t leading = h.shape[0];
        if (leading <= 0) {
            // Empty leading axis (e.g. shape (0, …)): nothing to slice, and a
            // negative idx clamp would otherwise form a wild pointer.
            *tbl = build_2d_table(h, data, /*rows=*/0, /*cols=*/1,
                                   h.item_size, h.fortran_order);
            if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  " + shape_str(h.shape) +
                      "  |  dtype: " + h.dtype_str + "  |  empty";
            return "";
        }
        int64_t idx = spec.slice_idx;
        if (idx < 0) idx = 0;
        if (idx >= leading) idx = leading - 1;
        // Treat dims [1:] as (rows, cols). For >3-D we collapse the
        // tail into a single column dimension.
        int64_t inner_rows = h.shape[1];
        int64_t inner_cols = 1;
        for (size_t i = 2; i < h.shape.size(); ++i) inner_cols *= h.shape[i];
        size_t slice_bytes = (size_t)inner_rows * inner_cols * h.item_size;
        const uint8_t* slice_data = data + (size_t)idx * slice_bytes;

        int64_t full_c = 0;
        *tbl = build_2d_table(h, slice_data,
                               inner_rows, inner_cols,
                               h.item_size, h.fortran_order, &full_c);
        if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";

        std::string slice_desc = "[" + std::to_string(idx) + ", :, :";
        for (size_t i = 3; i < h.shape.size(); ++i) slice_desc += ", :";
        slice_desc += "]";
        *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                  "  |  " + shape_str(h.shape) +
                  "  |  dtype: " + h.dtype_str +
                  "  |  slice " + slice_desc +
                  " (" + std::to_string(idx + 1) + "/" + std::to_string(leading)
                  + ")  |  [ / ] step slice  •  :slice N jumps";
        if (limit)
            *limit = PreviewLimit{(*tbl)->num_rows(), (*tbl)->num_rows(),
                                  (*tbl)->num_columns(), full_c};
        if ((*tbl)->num_columns() < full_c)
            *footer += "  |  showing first " +
                       std::to_string((*tbl)->num_columns()) + " of " +
                       std::to_string(full_c) + " columns";
        return "";
    }

    static std::string build_one(const std::string& path,
                                   std::shared_ptr<std::vector<Entry>> archive,
                                   OpenSpec spec,
                                   std::vector<OpenSpec> siblings,
                                   std::unique_ptr<NpzSource>* out) {
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        PreviewLimit limit;
        std::string err = build_table(*archive, spec, &tbl, &footer, &limit);
        if (!err.empty()) return err;
        if (!siblings.empty())
            footer += "  |  +" + std::to_string(siblings.size()) +
                       " more tab(s)";
        out->reset(new NpzSource(std::move(tbl), path, std::move(footer),
                                   std::move(archive), std::move(spec),
                                   std::move(siblings)));
        (*out)->limit_ = limit;
        return "";
    }

public:
    // A bare .npy is a single array with no container around it. Wrap it as a
    // one-entry archive and reuse the whole NPZ path — same parser, same
    // hardening, no second implementation to keep in step.
    //
    // README has documented `.npy` since the NumPy viewer landed, but no
    // dispatch branch ever existed, so `vv x.npy` answered "unrecognised file
    // extension". Building the format registry is what surfaced that.
    static std::string open_npy(const std::string& path,
                                 std::unique_ptr<NpzSource>* out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return "Cannot open '" + path + "'";
        auto bytes = std::make_shared<std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (bytes->empty()) return "'" + path + "': empty file";

        Entry e;
        // Tab label: the basename without the .npy suffix, mirroring how an
        // in-archive member is named.
        {
            std::string base = path;
            auto slash = base.find_last_of('/');
            if (slash != std::string::npos) base.erase(0, slash + 1);
            if (base.size() > 4 &&
                fends_ci(base, ".npy")) base.erase(base.size() - 4);
            e.name = base.empty() ? std::string("array") : base;
        }
        e.bytes = bytes;
        std::string err = parse_npy_header(bytes->data(), bytes->size(),
                                            &e.header);
        if (!err.empty()) return "'" + path + "': " + err;
        if (e.header.unsupported)
            return "'" + path + "': unsupported dtype '" + e.header.dtype_str +
                   "' (object / structured arrays are not displayed)";

        auto archive = std::make_shared<std::vector<Entry>>();
        archive->push_back(std::move(e));
        OpenSpec spec;
        spec.bare_npy   = true;
        spec.entry_name = (*archive)[0].name;
        return build_one(path, std::move(archive), std::move(spec), {}, out);
    }

    static std::string open_first(const std::string& path,
                                    std::unique_ptr<NpzSource>* out) {
        auto archive = std::make_shared<std::vector<Entry>>();
        std::string err = load_archive(path, archive.get());
        if (!err.empty()) return err;
        if (archive->empty()) return "'" + path + "': NPZ has no arrays";

        // Tab order: summary, then one tab per non-object array in the
        // order they appear in the archive.
        std::vector<OpenSpec> specs;
        specs.push_back({/*bare_npy=*/false, /*is_summary=*/true, "", 0});
        for (const auto& e : *archive) {
            if (!e.header.unsupported)
                specs.push_back({false, false, e.name, 0});
        }

        OpenSpec first = specs.front();
        std::vector<OpenSpec> siblings(specs.begin() + 1, specs.end());
        return build_one(path, std::move(archive), std::move(first),
                          std::move(siblings), out);
    }

    std::string tab_label() const override {
        return spec_.is_summary ? std::string("summary") : spec_.entry_name;
    }
    PreviewLimit preview_limit() const override { return limit_; }

    std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> result;
        for (const auto& s : siblings_) {
            std::unique_ptr<NpzSource> src;
            std::string err = build_one(path(), archive_, s,
                                         /*siblings=*/{}, &src);
            if (!err.empty()) {
                std::fprintf(stderr, "vv: NPZ tab '%s': %s\n",
                              s.entry_name.c_str(), err.c_str());
                continue;
            }
            result.push_back(std::move(src));
        }
        return result;
    }

    // Slice-axis navigation. Returns true if the slice changed.
    bool change_slice(int delta, bool absolute, int64_t target) override {
        if (spec_.is_summary) return false;
        const Entry* e = find_entry(*archive_, spec_.entry_name);
        if (!e || e->header.shape.size() < 3) return false;
        int64_t max = e->header.shape[0];
        int64_t new_idx = absolute ? target : spec_.slice_idx + delta;
        if (new_idx < 0) new_idx = 0;
        if (new_idx >= max) new_idx = max - 1;
        if (new_idx == spec_.slice_idx) return false;
        spec_.slice_idx = new_idx;
        // Rebuild the underlying table + footer in place.
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        PreviewLimit limit;
        std::string err = build_table(*archive_, spec_, &tbl, &footer, &limit);
        if (!err.empty()) return false;
        replace_table(std::move(tbl), std::move(footer));
        limit_ = limit;
        return true;
    }
};

std::string open_npz_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<NpzSource> s;
    std::string e = NpzSource::open_first(path, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
std::string open_npy_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<NpzSource> s;
    std::string e = NpzSource::open_npy(path, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

}  // namespace npz
