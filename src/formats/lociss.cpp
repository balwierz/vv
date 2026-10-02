// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// LociSSD Parquet (manifest, v4 colblock codec) and autoSql.

#include "internal.hpp"

static std::shared_ptr<arrow::DataType> autosql_base_type(const std::string& t) {
    if (t == "byte")    return arrow::int8();
    if (t == "ubyte")   return arrow::uint8();
    if (t == "short")   return arrow::int16();
    if (t == "ushort")  return arrow::uint16();
    if (t == "int")     return arrow::int32();
    if (t == "uint")    return arrow::uint32();
    if (t == "bigint")  return arrow::int64();
    if (t == "float")   return arrow::float32();
    if (t == "double")  return arrow::float64();
    // strings, char arrays, enum, set, lstring → utf8
    if (t == "string" || t == "lstring") return arrow::utf8();
    if (t.rfind("char[", 0) == 0)        return arrow::utf8();
    if (t.rfind("enum",  0) == 0)        return arrow::utf8();
    if (t.rfind("set",   0) == 0)        return arrow::utf8();
    return arrow::utf8();  // unknown — fall through
}

std::vector<AutosqlField> parse_autosql(const std::string& sql) {
    std::vector<AutosqlField> out;
    // Find the opening '(' of the field block.
    size_t lp = sql.find('(');
    if (lp == std::string::npos) return out;
    size_t rp = sql.rfind(')');
    if (rp == std::string::npos || rp <= lp) return out;
    std::string body = sql.substr(lp + 1, rp - lp - 1);

    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1);
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    };

    // Walk char-by-char. Skip whitespace and "quoted comments"; collect
    // "<type> <name>" up to the next top-level ';'. Inside `{...}` (enum
    // / set values) commas and even ';' are part of the type.
    size_t p = 0;
    while (p < body.size()) {
        // Skip whitespace / inter-statement comments.
        while (p < body.size()) {
            char c = body[p];
            if (std::isspace((unsigned char)c)) { ++p; continue; }
            if (c == '"') {
                ++p;
                while (p < body.size() && body[p] != '"') ++p;
                if (p < body.size()) ++p;
                continue;
            }
            break;
        }
        if (p >= body.size()) break;

        // Collect everything up to the next top-level ';', tracking braces
        // and quoted strings so ';' inside `{a;b}` or `"text"` is ignored.
        std::string stmt;
        int braces = 0;
        while (p < body.size()) {
            char c = body[p];
            if (c == '"') {
                ++p;
                while (p < body.size() && body[p] != '"') ++p;
                if (p < body.size()) ++p;
                continue;
            }
            if (c == '{') { stmt += c; ++braces; ++p; continue; }
            if (c == '}') { stmt += c; --braces; ++p; continue; }
            if (c == ';' && braces == 0) { ++p; break; }
            stmt += c;
            ++p;
        }
        trim(stmt);
        if (stmt.empty()) continue;

        // Split into "<type> <name>". The type may contain `[...]`,
        // `{...}` (enum/set body), or be a single word. Boundary is the
        // last whitespace run.
        size_t name_start = stmt.find_last_of(" \t\n\r");
        if (name_start == std::string::npos) continue;
        std::string type_part = stmt.substr(0, name_start);
        std::string name_part = stmt.substr(name_start + 1);
        trim(type_part);
        trim(name_part);
        if (type_part.empty() || name_part.empty()) continue;

        AutosqlField f;
        f.name = name_part;
        // Array suffix `[N]` or `[fieldName]` — separate from `char[N]`
        // which is a fixed-size string, not a list. Look at the trailing
        // ']' and find the matching '['.
        if (!type_part.empty() && type_part.back() == ']') {
            size_t br = type_part.rfind('[');
            if (br != std::string::npos) {
                std::string base = type_part.substr(0, br);
                trim(base);
                if (base == "char") {
                    f.arrow_type = arrow::utf8();   // fixed-width string
                } else {
                    f.is_list   = true;
                    f.elem_type = autosql_base_type(base);
                    f.arrow_type = arrow::list(f.elem_type);
                }
                out.push_back(std::move(f));
                continue;
            }
        }
        f.arrow_type = autosql_base_type(type_part);
        out.push_back(std::move(f));
    }
    return out;
}


bool parse_lociss_chromosomes(const std::string& json,
                              std::vector<LocissChrom>* out) {
    auto skip_ws = [&](size_t& p) {
        while (p < json.size() && std::isspace((unsigned char)json[p])) ++p;
    };
    // Locate the chromosomes array start.
    auto key = json.find("\"chromosomes\"");
    if (key == std::string::npos) return false;
    size_t p = json.find('[', key);
    if (p == std::string::npos) return false;
    ++p;  // step past '['
    out->clear();
    while (p < json.size()) {
        skip_ws(p);
        if (p < json.size() && json[p] == ']') return !out->empty();
        if (json[p] != '{') return false;
        // Parse one object; track nested braces in case of future nesting.
        size_t obj_start = p, depth = 0;
        for (; p < json.size(); ++p) {
            if (json[p] == '{') ++depth;
            else if (json[p] == '}') { if (--depth == 0) { ++p; break; } }
            else if (json[p] == '"') {
                // skip a string literal (handles backslash-escapes)
                ++p;
                while (p < json.size() && json[p] != '"') {
                    if (json[p] == '\\' && p + 1 < json.size()) p += 2;
                    else ++p;
                }
            }
        }
        std::string obj = json.substr(obj_start, p - obj_start);
        // Extract scalar fields by literal key search inside this object.
        LocissChrom c{};
        auto find_str = [&](const char* k, std::string* dst) {
            std::string needle = std::string("\"") + k + "\"";
            auto q = obj.find(needle);
            if (q == std::string::npos) return false;
            q = obj.find(':', q);
            if (q == std::string::npos) return false;
            ++q;
            while (q < obj.size() && std::isspace((unsigned char)obj[q])) ++q;
            if (q >= obj.size() || obj[q] != '"') return false;
            ++q;
            size_t end = q;
            while (end < obj.size() && obj[end] != '"') {
                if (obj[end] == '\\' && end + 1 < obj.size()) end += 2;
                else ++end;
            }
            *dst = obj.substr(q, end - q);
            return true;
        };
        auto find_int = [&](const char* k, int64_t* dst) {
            std::string needle = std::string("\"") + k + "\"";
            auto q = obj.find(needle);
            if (q == std::string::npos) return false;
            q = obj.find(':', q);
            if (q == std::string::npos) return false;
            ++q;
            while (q < obj.size() && std::isspace((unsigned char)obj[q])) ++q;
            try { *dst = std::stoll(obj.substr(q)); return true; }
            catch (...) { return false; }
        };
        if (!find_str("name",       &c.name))        return false;
        if (!find_int("rows",       &c.rows))        return false;
        if (!find_int("row_offset", &c.row_offset))  return false;
        out->push_back(std::move(c));
        // Skip commas / whitespace before the next entry.
        skip_ws(p);
        if (p < json.size() && json[p] == ',') ++p;
    }
    return !out->empty();
}

// Extract a top-level scalar (string or number) for `key` from the LociSSD
// manifest JSON, as a string. Returns false if the key is absent or null.
// The manifest's top-level keys (assembly / species / row_count) don't collide
// with the per-chromosome object keys, so a literal search is safe.
bool lociss_manifest_value(const std::string& json, const char* key,
                           std::string* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t q = json.find(needle);
    if (q == std::string::npos) return false;
    q = json.find(':', q + needle.size());
    if (q == std::string::npos) return false;
    ++q;
    while (q < json.size() && std::isspace((unsigned char)json[q])) ++q;
    if (q >= json.size()) return false;
    if (json[q] == '"') {                         // string value
        ++q; std::string v;
        while (q < json.size() && json[q] != '"') {
            if (json[q] == '\\' && q + 1 < json.size()) { v += json[q + 1]; q += 2; }
            else v += json[q++];
        }
        *out = std::move(v);
        return !out->empty();
    }
    if (json.compare(q, 4, "null") == 0) return false;
    size_t e = q;                                 // number / bare token
    while (e < json.size() && json[e] != ',' && json[e] != '}' &&
           json[e] != ']' && !std::isspace((unsigned char)json[e])) ++e;
    *out = json.substr(q, e - q);
    return e > q;
}

// Best-effort species name for a genome assembly, used when the LociSSD
// manifest leaves `species` null (common). Covers the usual model organisms;
// matched case-insensitively against the UCSC / Ensembl assembly aliases.
std::string assembly_to_species(const std::string& assembly) {
    std::string a;
    for (char c : assembly) a += (char)std::tolower((unsigned char)c);
    auto has = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys) if (a == k) return true;
        return false;
    };
    if (has({"hg38", "hg19", "hg18", "grch38", "grch37", "grch36", "t2t-chm13"}))
        return "Homo sapiens";
    if (has({"mm39", "mm10", "mm9", "grcm39", "grcm38"})) return "Mus musculus";
    if (has({"rn7", "rn6", "rn5", "mratbn7.2"}))          return "Rattus norvegicus";
    if (has({"danrer11", "danrer10", "grcz11", "grcz10"})) return "Danio rerio";
    if (has({"dm6", "dm3", "bdgp6"}))                     return "Drosophila melanogaster";
    if (has({"ce11", "ce10", "wbcel235"}))                return "Caenorhabditis elegans";
    if (has({"saccer3", "saccer2", "r64-1-1"}))           return "Saccharomyces cerevisiae";
    if (has({"galgal6", "galgal5", "grcg6a"}))            return "Gallus gallus";
    if (has({"susscr11", "susscr3"}))                     return "Sus scrofa";
    if (has({"bostau9", "bostau8", "ars-ucd1.2"}))        return "Bos taurus";
    if (has({"xentro10", "xentro9"}))                     return "Xenopus tropicalis";
    if (has({"tair10"}))                                  return "Arabidopsis thaliana";
    return "";
}

// ── LociSSD v4 "colblock" reader ─────────────────────────────────────────────
//
// A custom binary columnar container (NOT Parquet): data file magic "LSB1"
// (version 4) + a sidecar PATH.idx ("LSI1") with a block zone-map index and
// per-(block,column) chunk pointers. Each column chunk is
// zstd(has_nulls byte ‖ [validity bitmap] ‖ codec_payload). Site-level read only
// — the optional genotype matrix (spec §7) is never addressed (we read the index
// up to col_clen[] and ignore any trailing mat_* arrays / matrix section).
// Spec: /home/piotr/Sources/Loci1/docs/lociss_columnar_format_spec.md.
namespace lociss_v4 {

// Map a v4 schema type string to an Arrow type.
static std::shared_ptr<arrow::DataType> arrow_type_of(const std::string& t) {
    if (t == "int8")    return arrow::int8();
    if (t == "int16")   return arrow::int16();
    if (t == "int32")   return arrow::int32();
    if (t == "int64")   return arrow::int64();
    if (t == "uint8")   return arrow::uint8();
    if (t == "uint16")  return arrow::uint16();
    if (t == "uint32")  return arrow::uint32();
    if (t == "uint64")  return arrow::uint64();
    if (t == "float32") return arrow::float32();
    if (t == "float64") return arrow::float64();
    if (t == "large_utf8") return arrow::large_utf8();
    if (t == "bool")    return arrow::boolean();
    return arrow::utf8();   // "utf8" and unknown
}

// --- minimal JSON helpers for the index meta (a well-formed JSON object) ---

// Parse a JSON string array `"key": ["a","b",...]` into `out`.
static bool json_string_array(const std::string& json, const char* key,
                              std::vector<std::string>* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t q = json.find(needle);
    if (q == std::string::npos) return false;
    q = json.find('[', q + needle.size());
    if (q == std::string::npos) return false;
    ++q; out->clear();
    while (q < json.size()) {
        while (q < json.size() &&
               (std::isspace((unsigned char)json[q]) || json[q] == ',')) ++q;
        if (q >= json.size() || json[q] == ']') break;
        if (json[q] != '"') return false;
        ++q; std::string v;
        while (q < json.size() && json[q] != '"') {
            if (json[q] == '\\' && q + 1 < json.size()) { v += json[q + 1]; q += 2; }
            else v += json[q++];
        }
        if (q < json.size()) ++q;
        out->push_back(std::move(v));
    }
    return true;
}

// Byte span [*beg,*end) of the object value for `"obj_key": { ... }`.
static bool json_object_span(const std::string& json, const char* obj_key,
                             size_t* beg, size_t* end) {
    std::string needle = std::string("\"") + obj_key + "\"";
    size_t q = json.find(needle);
    if (q == std::string::npos) return false;
    q = json.find('{', q + needle.size());
    if (q == std::string::npos) return false;
    size_t depth = 0;
    for (size_t p = q; p < json.size(); ++p) {
        char c = json[p];
        if (c == '"') { ++p; while (p < json.size() && json[p] != '"') {
                              if (json[p] == '\\' && p + 1 < json.size()) ++p; ++p; } }
        else if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) { *beg = q; *end = p + 1; return true; } }
    }
    return false;
}

// Look up a scalar field inside the JSON object named `obj_key`.
static bool json_object_value(const std::string& json, const char* obj_key,
                              const std::string& field, std::string* out) {
    size_t beg, end;
    if (!json_object_span(json, obj_key, &beg, &end)) return false;
    std::string obj = json.substr(beg, end - beg);
    return lociss_manifest_value(obj, field.c_str(), out);
}

// Read a little-endian fixed-width int (cw = 4 or 8) and sign-extend to int64.
static inline int64_t rd_int(const uint8_t* p, int cw) {
    if (cw == 8) { int64_t v; std::memcpy(&v, p, 8); return v; }
    int32_t v; std::memcpy(&v, p, 4); return (int64_t)v;
}

// zstd-decompress a compressed buffer (decompressed size unknown up front) via
// Arrow's streaming decompressor, reading to end. `max_out` bounds the output so
// a tiny compressed chunk can't inflate to gigabytes (decompression bomb).
static arrow::Result<std::shared_ptr<arrow::Buffer>>
zstd_inflate(std::shared_ptr<arrow::Buffer> comp, int64_t max_out) {
    ARROW_ASSIGN_OR_RAISE(auto codec,
        arrow::util::Codec::Create(arrow::Compression::ZSTD));
    auto reader = std::make_shared<arrow::io::BufferReader>(comp);
    ARROW_ASSIGN_OR_RAISE(auto cis,
        arrow::io::CompressedInputStream::Make(codec.get(), reader));
    arrow::BufferBuilder bb;
    uint8_t tmp[64 * 1024];
    int64_t total = 0;
    for (;;) {
        ARROW_ASSIGN_OR_RAISE(int64_t got, cis->Read(sizeof tmp, tmp));
        if (got == 0) break;
        total += got;
        if (total > max_out)
            return arrow::Status::Invalid(
                "colblock: decompressed chunk exceeds size cap (corrupt or bomb)");
        ARROW_RETURN_NOT_OK(bb.Append(tmp, got));
    }
    std::shared_ptr<arrow::Buffer> out;
    ARROW_RETURN_NOT_OK(bb.Finish(&out));
    return out;
}

// Decode one (already zstd-decompressed) column chunk into an Arrow array.
// `n` = block row count, `cw` = coord width, `start` = decoded Start values for
// the LENGTH codec (else null). Splits has_nulls + validity bitmap, then decodes
// by codec_id. External linkage (not static) so the libFuzzer harness in
// tests/fuzz/ can call it directly; see include/vv/vvfuzz.hpp.
arrow::Result<std::shared_ptr<arrow::Array>>
decode_colblock(const uint8_t* buf, size_t blen, int codec_id,
                const arrow::DataType& type, int64_t n, int cw,
                const int64_t* start) {
    if (blen < 1) return arrow::Status::Invalid("colblock: empty chunk");
    bool has_nulls = buf[0] != 0;
    size_t p = 1;
    std::vector<uint8_t> valid;            // 1 = valid; empty = all valid
    if (has_nulls) {
        size_t nb = (size_t)((n + 7) / 8);
        if (p + nb > blen) return arrow::Status::Invalid("colblock: short bitmap");
        valid.assign((size_t)n, 0);
        for (int64_t i = 0; i < n; ++i)
            valid[(size_t)i] = (buf[p + (size_t)(i >> 3)] >> (i & 7)) & 1u;
        p += nb;
    }
    const uint8_t* pay = buf + p;
    size_t paylen = blen - p;
    const uint8_t* vb = valid.empty() ? nullptr : valid.data();
    std::shared_ptr<arrow::Array> arr;

    auto need = [&](size_t bytes) -> arrow::Status {
        return paylen >= bytes ? arrow::Status::OK()
             : arrow::Status::Invalid("colblock: payload too short");
    };

    // String result (DICT / FRONTCODE / ARENA): build per declared utf8 width.
    auto build_strings = [&](std::vector<std::string>& vals)
        -> arrow::Result<std::shared_ptr<arrow::Array>> {
        std::shared_ptr<arrow::Array> a;
        if (type.id() == arrow::Type::LARGE_STRING) {
            arrow::LargeStringBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(vals, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        } else {
            arrow::StringBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(vals, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        }
        return a;
    };
    // Integer-coordinate result (DELTA / LENGTH): build per the column type.
    auto build_coords = [&](const std::vector<int64_t>& v)
        -> arrow::Result<std::shared_ptr<arrow::Array>> {
        std::shared_ptr<arrow::Array> a;
        if (type.id() == arrow::Type::INT64) {
            arrow::Int64Builder b;
            if (n > 0) ARROW_RETURN_NOT_OK(b.AppendValues(v.data(), n, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        } else {
            std::vector<int32_t> w(v.size());
            for (size_t i = 0; i < v.size(); ++i) w[i] = (int32_t)v[i];
            arrow::Int32Builder b;
            if (n > 0) ARROW_RETURN_NOT_OK(b.AppendValues(w.data(), n, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        }
        return a;
    };

    switch (codec_id) {
        case 0: {  // RAW — fixed-width numeric, n × itemsize LE
            #define VV_RAW(BUILDER, CT)                                         \
                do { ARROW_RETURN_NOT_OK(need((size_t)n * sizeof(CT)));         \
                     std::vector<CT> v((size_t)n);                              \
                     BUILDER b;                                                 \
                     if (n > 0) {                                              \
                         std::memcpy(v.data(), pay, (size_t)n * sizeof(CT));    \
                         ARROW_RETURN_NOT_OK(b.AppendValues(v.data(), n, vb));  \
                     }                                                          \
                     ARROW_RETURN_NOT_OK(b.Finish(&arr)); } while (0)
            switch (type.id()) {
                case arrow::Type::INT8:   VV_RAW(arrow::Int8Builder,   int8_t);   break;
                case arrow::Type::INT16:  VV_RAW(arrow::Int16Builder,  int16_t);  break;
                case arrow::Type::INT32:  VV_RAW(arrow::Int32Builder,  int32_t);  break;
                case arrow::Type::INT64:  VV_RAW(arrow::Int64Builder,  int64_t);  break;
                case arrow::Type::UINT8:  VV_RAW(arrow::UInt8Builder,  uint8_t);  break;
                case arrow::Type::UINT16: VV_RAW(arrow::UInt16Builder, uint16_t); break;
                case arrow::Type::UINT32: VV_RAW(arrow::UInt32Builder, uint32_t); break;
                case arrow::Type::UINT64: VV_RAW(arrow::UInt64Builder, uint64_t); break;
                case arrow::Type::FLOAT:  VV_RAW(arrow::FloatBuilder,  float);    break;
                case arrow::Type::DOUBLE: VV_RAW(arrow::DoubleBuilder, double);   break;
                default: return arrow::Status::Invalid("colblock RAW: unsupported type");
            }
            #undef VV_RAW
            return arr;
        }
        case 1: {  // DELTA — cumsum (used for Start)
            ARROW_RETURN_NOT_OK(need((size_t)n * (size_t)cw));
            std::vector<int64_t> v((size_t)n);
            // Accumulate in uint64 — a crafted file's deltas can overflow int64
            // (signed overflow is UB); unsigned wraps deterministically and the
            // result is unchanged for legitimate (bounded) coordinates.
            uint64_t acc = 0;
            for (int64_t i = 0; i < n; ++i) {
                acc += (uint64_t)rd_int(pay + (size_t)i * cw, cw);
                v[(size_t)i] = (int64_t)acc;
            }
            return build_coords(v);
        }
        case 2: {  // LENGTH — Start + len (used for End)
            ARROW_RETURN_NOT_OK(need((size_t)n * (size_t)cw));
            if (!start) return arrow::Status::Invalid("colblock LENGTH: Start not decoded");
            std::vector<int64_t> v((size_t)n);
            for (int64_t i = 0; i < n; ++i)
                v[(size_t)i] = (int64_t)((uint64_t)start[i] +
                                         (uint64_t)rd_int(pay + (size_t)i * cw, cw));
            return build_coords(v);
        }
        case 3: {  // DICT
            ARROW_RETURN_NOT_OK(need(4));
            uint32_t n_dict; std::memcpy(&n_dict, pay, 4);
            size_t q = 4;
            // Widen before +1 so n_dict==UINT32_MAX can't wrap the length to 0.
            ARROW_RETURN_NOT_OK(need(q + ((size_t)n_dict + 1) * 4));
            const uint8_t* offp = pay + q; q += ((size_t)n_dict + 1) * 4;
            uint32_t blob_len; std::memcpy(&blob_len, offp + (size_t)n_dict * 4, 4);
            ARROW_RETURN_NOT_OK(need(q + blob_len));
            const char* blob = (const char*)(pay + q); q += blob_len;
            int code_w = (n_dict <= 256) ? 1 : 2;
            ARROW_RETURN_NOT_OK(need(q + (size_t)n * code_w));
            std::vector<std::string> dict((size_t)n_dict);
            for (uint32_t k = 0; k < n_dict; ++k) {
                uint32_t o0, o1; std::memcpy(&o0, offp + (size_t)k * 4, 4);
                std::memcpy(&o1, offp + ((size_t)k + 1) * 4, 4);
                // Offsets are attacker-controlled: reject non-monotone / OOB
                // (o1<o0 would underflow the length to ~4 GiB).
                if (o0 > o1 || o1 > blob_len)
                    return arrow::Status::Invalid("colblock DICT: bad dictionary offset");
                dict[k].assign(blob + o0, o1 - o0);
            }
            std::vector<std::string> vals((size_t)n);
            for (int64_t i = 0; i < n; ++i) {
                uint32_t code = (code_w == 1) ? pay[q + (size_t)i]
                              : (uint32_t)(pay[q + (size_t)i * 2] | (pay[q + (size_t)i * 2 + 1] << 8));
                if (code < n_dict) vals[(size_t)i] = dict[code];
            }
            return build_strings(vals);
        }
        case 4: {  // FRONTCODE — sequential lcp + suffix
            ARROW_RETURN_NOT_OK(need((size_t)n * 8));
            const uint8_t* lcpp = pay;
            const uint8_t* slenp = pay + (size_t)n * 4;
            const uint8_t* sufp = pay + (size_t)n * 8;
            size_t spos = 0; std::string prev;
            std::vector<std::string> vals((size_t)n);
            for (int64_t i = 0; i < n; ++i) {
                uint32_t lcp, sl;
                std::memcpy(&lcp, lcpp + (size_t)i * 4, 4);
                std::memcpy(&sl, slenp + (size_t)i * 4, 4);
                if (lcp > prev.size()) lcp = (uint32_t)prev.size();
                ARROW_RETURN_NOT_OK(need((size_t)n * 8 + spos + sl));
                std::string cur = prev.substr(0, lcp);
                cur.append((const char*)(sufp + spos), sl);
                spos += sl;
                vals[(size_t)i] = cur;
                prev = std::move(cur);
            }
            return build_strings(vals);
        }
        case 5: {  // ARENA — off[n+1] + utf8
            size_t hdr = ((size_t)n + 1) * 4;
            ARROW_RETURN_NOT_OK(need(hdr));
            const uint8_t* offp = pay;
            const char* blob = (const char*)(pay + hdr);
            size_t blob_avail = paylen - hdr;
            std::vector<std::string> vals((size_t)n);
            for (int64_t i = 0; i < n; ++i) {
                uint32_t o0, o1; std::memcpy(&o0, offp + (size_t)i * 4, 4);
                std::memcpy(&o1, offp + ((size_t)i + 1) * 4, 4);
                // Offsets are attacker-controlled: reject non-monotone / OOB.
                if (o0 > o1 || o1 > blob_avail)
                    return arrow::Status::Invalid("colblock ARENA: bad offset");
                vals[(size_t)i].assign(blob + o0, o1 - o0);
            }
            return build_strings(vals);
        }
        case 6: {  // BOOL — bit-packed LSB-first
            size_t nb = (size_t)((n + 7) / 8);
            ARROW_RETURN_NOT_OK(need(nb));
            std::vector<uint8_t> bits((size_t)n);
            for (int64_t i = 0; i < n; ++i)
                bits[(size_t)i] = (pay[(size_t)(i >> 3)] >> (i & 7)) & 1u;
            arrow::BooleanBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(bits.data(), n, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&arr));
            return arr;
        }
        default:
            return arrow::Status::Invalid("colblock: unknown codec id " +
                                          std::to_string(codec_id));
    }
}

// Select the rows of `a` where keep[i] is true (region overlap mask). Uses core
// Arrow scalars (no arrow_compute dependency); region results are small.
static arrow::Result<std::shared_ptr<arrow::Array>>
filter_array(const std::shared_ptr<arrow::Array>& a, const std::vector<bool>& keep) {
    std::unique_ptr<arrow::ArrayBuilder> bld;
    ARROW_RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(),
                                           a->type(), &bld));
    for (int64_t i = 0; i < a->length(); ++i) {
        if ((size_t)i < keep.size() && !keep[(size_t)i]) continue;
        ARROW_ASSIGN_OR_RAISE(auto sc, a->GetScalar(i));
        ARROW_RETURN_NOT_OK(bld->AppendScalar(*sc));
    }
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(bld->Finish(&out));
    return out;
}

}  // namespace lociss_v4

// A LociSSD v4 dataset, presented chunk = block. The sidecar .idx (zone-map +
// chunk pointers) is parsed at open; column chunks are zstd-decompressed and
// codec-decoded on demand in read_chunk. Region (-r) queries prune blocks via
// the index and mask rows per the §5 overlap test.
class LocissV4Source : public TabularSource {
    std::string                               path_;
    std::shared_ptr<arrow::io::ReadableFile>  data_;
    std::shared_ptr<arrow::Schema>            schema_;
    int       n_blocks_ = 0, n_cols_ = 0, cw_ = 4;
    int64_t   row_count_ = 0;
    std::vector<std::string>  stored_;
    std::vector<int>          codecs_;
    std::vector<std::shared_ptr<arrow::DataType>> col_types_;
    std::vector<int64_t>      cids_, min_start_, max_end_, prefix_max_end_;
    std::vector<int64_t>      n_rows_, block_first_row_;
    std::vector<uint64_t>     col_offset_;
    std::vector<uint32_t>     col_clen_;
    std::map<int64_t, std::string> rank_to_name_;
    std::map<std::string, int64_t> name_to_rank_;
    std::string assembly_, species_;
    int start_si_ = -1, end_si_ = -1;     // stored indices of Start / End

    bool region_mode_ = false;
    struct Slice { int block; Region win; };
    std::vector<Slice>   slices_;
    std::vector<int64_t> slice_first_row_, slice_count_;
    int64_t region_total_ = 0;
    mutable arrow::Status read_status_;

    // Region mode only: memoise decoded column arrays per (block, col) so the
    // open-time count pass and the subsequent display read don't decode the same
    // candidate blocks twice. Bounded; off for sequential scans (where each block
    // is read exactly once and a cache would be pure overhead).
    mutable std::map<int64_t, std::shared_ptr<arrow::Array>> region_col_cache_;
    static constexpr size_t kRegionCacheCap = 512;

    int stored_index(const std::string& nm) const {
        for (int i = 0; i < (int)stored_.size(); ++i) if (stored_[i] == nm) return i;
        return -1;
    }
    // Read + decompress + decode stored column `c` of block `b`.
    arrow::Result<std::shared_ptr<arrow::Array>>
    decode_col(int b, int c, const int64_t* start) const {
        if (c < 0 || c >= n_cols_)
            return arrow::Status::Invalid("colblock: column index out of range");
        int64_t key = (int64_t)b * n_cols_ + c;
        if (region_mode_) {
            auto it = region_col_cache_.find(key);
            if (it != region_col_cache_.end()) return it->second;
        }
        size_t rec = (size_t)b * n_cols_ + (size_t)c;
        ARROW_ASSIGN_OR_RAISE(auto comp, data_->ReadAt((int64_t)col_offset_[rec],
                                                       (int64_t)col_clen_[rec]));
        // Decompression-bomb guard: a block's column chunk can't legitimately
        // exceed a generous per-row bound (64 MiB base covers the dictionary
        // blob / bitmap; ~4 KiB/row is ample for genomic strings), capped at 2 GiB.
        int64_t nr = n_rows_[(size_t)b];
        int64_t max_out = (64LL << 20) + (nr > 0 ? nr * 4096 : 0);
        if (max_out > (2LL << 30)) max_out = 2LL << 30;
        ARROW_ASSIGN_OR_RAISE(auto raw, lociss_v4::zstd_inflate(comp, max_out));
        ARROW_ASSIGN_OR_RAISE(auto arr,
            lociss_v4::decode_colblock(raw->data(), (size_t)raw->size(),
                                       codecs_[(size_t)c], *col_types_[(size_t)c],
                                       nr, cw_, start));
        if (region_mode_) {
            if (region_col_cache_.size() >= kRegionCacheCap) region_col_cache_.clear();
            region_col_cache_[key] = arr;
        }
        return arr;
    }
    // Extract a decoded coordinate array (int32 or int64) as int64 values —
    // used for the region mask and as the LENGTH codec's Start input.
    static std::vector<int64_t> array_to_int64(const std::shared_ptr<arrow::Array>& a) {
        std::vector<int64_t> v((size_t)a->length());
        if (a->type_id() == arrow::Type::INT64) {
            auto ia = std::static_pointer_cast<arrow::Int64Array>(a);
            for (int64_t i = 0; i < a->length(); ++i) v[(size_t)i] = ia->Value(i);
        } else {
            auto ia = std::static_pointer_cast<arrow::Int32Array>(a);
            for (int64_t i = 0; i < a->length(); ++i) v[(size_t)i] = ia->Value(i);
        }
        return v;
    }
    std::shared_ptr<arrow::Array> chrom_array(int b, int64_t count) const {
        auto it = rank_to_name_.find(cids_[(size_t)b]);
        std::string nm = (it != rank_to_name_.end()) ? it->second : "?";
        arrow::StringBuilder bld;
        for (int64_t i = 0; i < count; ++i) (void)bld.Append(nm);
        std::shared_ptr<arrow::Array> a; (void)bld.Finish(&a); return a;
    }
    std::string build_region(const Config& cfg);

public:
    static std::string open(const std::string& path, const Config& cfg,
                            std::unique_ptr<LocissV4Source>* out);

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return region_mode_ ? region_total_ : row_count_; }
    int     num_chunks() const override { return region_mode_ ? (int)slices_.size() : n_blocks_; }
    arrow::Status read_status() const override { return read_status_; }
    bool region_applied() const override { return region_mode_; }
    ChunkMeta chunk_meta(int i) const override {
        if (region_mode_) return {slice_first_row_[(size_t)i], slice_count_[(size_t)i]};
        return {block_first_row_[(size_t)i], n_rows_[(size_t)i]};
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: LociSSD v4  |  Blocks: " + std::to_string(n_blocks_);
    }
    std::string top_banner() const override {
        std::string s = "LociSSD";
        if (!assembly_.empty()) {
            s += "  \xe2\x80\xa2  " + assembly_;
            if (!species_.empty()) s += " (" + species_ + ")";
        }
        s += "  \xe2\x80\xa2  " + digits_with_sep(std::to_string(row_count_)) + " elements";
        return s;
    }
    std::vector<std::string> preamble_above() const override {
        return {top_banner()};   // banner above the table in non-interactive views
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        // A decode failure (corrupt/hostile chunk) is sticky so the CLI exits
        // non-zero instead of silently skipping the block.
        arrow::Status st = read_chunk_impl(i, col_indices, out);
        if (!st.ok() && read_status_.ok()) read_status_ = st;
        return st;
    }
    arrow::Status read_chunk_impl(int i, const std::vector<int>& col_indices,
                                  std::shared_ptr<arrow::Table>* out) {
        int     b = region_mode_ ? slices_[(size_t)i].block : i;
        int64_t n = n_rows_[(size_t)b];

        // Decode Start once (as an array) when End is requested (LENGTH needs its
        // values) or for the region mask; the same array is reused as the Start
        // output column below instead of decoding it a second time.
        bool want_end = false;
        for (int f : col_indices) if (f == end_si_ + 1) want_end = true;
        std::shared_ptr<arrow::Array> start_arr;
        std::vector<int64_t> start;
        const int64_t* startp = nullptr;
        if ((region_mode_ || want_end) && start_si_ >= 0) {
            ARROW_ASSIGN_OR_RAISE(start_arr, decode_col(b, start_si_, nullptr));
            start = array_to_int64(start_arr);
            startp = start.empty() ? nullptr : start.data();
        }

        // Region overlap mask (Start < hi & End > lo); chrom is block-uniform.
        std::shared_ptr<arrow::Array> end_arr;
        std::vector<bool> keep;
        int64_t kept = n;
        if (region_mode_) {
            ARROW_ASSIGN_OR_RAISE(end_arr, decode_col(b, end_si_, startp));
            const Region& w = slices_[(size_t)i].win;
            bool e64 = (end_arr->type_id() == arrow::Type::INT64);
            auto e32 = e64 ? nullptr : std::static_pointer_cast<arrow::Int32Array>(end_arr);
            auto ei64 = e64 ? std::static_pointer_cast<arrow::Int64Array>(end_arr) : nullptr;
            keep.assign((size_t)n, false); kept = 0;
            for (int64_t r = 0; r < n; ++r) {
                int64_t st = start[(size_t)r];
                int64_t en = e64 ? ei64->Value(r) : e32->Value(r);
                bool ok = (w.end == INT64_MAX || st < w.end) &&
                          (w.start == INT64_MIN || en > w.start);
                keep[(size_t)r] = ok; if (ok) ++kept;
            }
        }

        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::Array>> cols;
        for (int f : col_indices) {
            std::shared_ptr<arrow::Array> a;
            if (f == 0) a = chrom_array(b, n);
            else if (f - 1 == end_si_ && end_arr) a = end_arr;
            else if (f - 1 == start_si_ && start_arr) a = start_arr;   // reuse decode
            else { ARROW_ASSIGN_OR_RAISE(a, decode_col(b, f - 1, startp)); }
            if (region_mode_ && kept != n) {
                ARROW_ASSIGN_OR_RAISE(a, lociss_v4::filter_array(a, keep));
            }
            cols.push_back(a);
            fields.push_back(schema_->field(f));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols,
                                  region_mode_ ? kept : n);
        return arrow::Status::OK();
    }
};

std::string open_lociss_v4_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<LocissV4Source> s;
    std::string e = LocissV4Source::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

std::string LocissV4Source::open(const std::string& path, const Config& cfg,
                                 std::unique_ptr<LocissV4Source>* out) {
    auto self = std::make_unique<LocissV4Source>();
    self->path_ = path;

    // Open the data file first — the LSI1 index may live inline (V4.1) or in a
    // sidecar (legacy V4); the column-chunk pointers are absolute into this file
    // either way.
    auto df = arrow::io::ReadableFile::Open(path);
    if (!df.ok()) return "LociSSD v4: cannot open '" + path + "': " + df.status().ToString();
    self->data_ = df.ValueOrDie();
    int64_t fsize = 0;
    if (auto sz = self->data_->GetSize(); sz.ok()) fsize = *sz;

    // ── Acquire the LSI1 index (V4.1 inline footer or legacy V4 .idx sidecar) ──
    // A V4.1 file ends in a 24-byte trailer: index_offset u64, index_len u64,
    // magic "LSIX", minor u8, reserved[3]. The trailer magic is authoritative
    // (the data header's offset-5 flags bit0 is a redundant fast-path hint).
    std::string idx;
    bool inline_idx = false;
    if (fsize >= 24) {
        if (auto tb = self->data_->ReadAt(fsize - 24, 24); tb.ok() && (*tb)->size() == 24) {
            const uint8_t* t = (*tb)->data();
            uint8_t hdr[8] = {0};
            if (auto hb = self->data_->ReadAt(0, 8); hb.ok() && (*hb)->size() >= 6)
                std::memcpy(hdr, (*hb)->data(), 6);
            bool hdr_flag = hdr[0]=='L' && hdr[1]=='S' && hdr[2]=='B' && hdr[3]=='1' && (hdr[5] & 1u);
            bool trailer_magic = t[16]=='L' && t[17]=='S' && t[18]=='I' && t[19]=='X';
            if (hdr_flag || trailer_magic) {
                if (!trailer_magic) return "LociSSD v4.1: inline-index trailer magic not found";
                uint64_t ioff, ilen;
                std::memcpy(&ioff, t, 8); std::memcpy(&ilen, t + 8, 8);
                if ((int64_t)ioff < 0 || (int64_t)ilen < 0 ||
                    (int64_t)(ioff + ilen) + 24 > fsize)
                    return "LociSSD v4.1: inline-index offset/length out of range";
                auto pb = self->data_->ReadAt((int64_t)ioff, (int64_t)ilen);
                if (!pb.ok()) return "LociSSD v4.1: cannot read inline index: " + pb.status().ToString();
                idx.assign((const char*)(*pb)->data(), (size_t)(*pb)->size());
                inline_idx = true;
            }
        }
    }
    if (!inline_idx) {
        std::string ip = path + ".idx";
        std::ifstream idxf(ip, std::ios::binary);
        if (!idxf) return "LociSSD: '" + path + "' is not a colblock file — no "
                          "inline index trailer and no sidecar '" + ip + "'";
        idx.assign((std::istreambuf_iterator<char>(idxf)),
                   std::istreambuf_iterator<char>());
    }
    if (idx.size() < 24 || idx.compare(0, 4, "LSI1") != 0)
        return "LociSSD v4: bad index magic"
               + std::string(inline_idx ? " (inline)" : " in sidecar '" + path + ".idx'");
    const uint8_t* ib = (const uint8_t*)idx.data();
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, ib + o, 4); return v; };
    uint32_t n_blocks = u32(8), n_cols = u32(12), flags = u32(16), meta_len = u32(20);
    if ((size_t)24 + meta_len > idx.size()) return "LociSSD v4: truncated index meta";
    std::string meta = idx.substr(24, meta_len);
    self->n_blocks_ = (int)n_blocks; self->n_cols_ = (int)n_cols;
    bool coords64 = (flags & 1u) != 0;
    self->cw_ = coords64 ? 8 : 4;
    int isz = coords64 ? 8 : 4;

    // ── meta JSON ────────────────────────────────────────────────────────────
    if (!lociss_v4::json_string_array(meta, "stored", &self->stored_) ||
        (int)self->stored_.size() != (int)n_cols)
        return "LociSSD v4: missing/mismatched 'stored' in index meta";
    self->codecs_.resize(n_cols);
    self->col_types_.resize(n_cols);
    for (int c = 0; c < (int)n_cols; ++c) {
        std::string ty, cd;
        lociss_v4::json_object_value(meta, "schema", self->stored_[(size_t)c], &ty);
        lociss_v4::json_object_value(meta, "codecs", self->stored_[(size_t)c], &cd);
        self->col_types_[(size_t)c] = lociss_v4::arrow_type_of(ty);
        self->codecs_[(size_t)c] = cd.empty() ? 0 : std::atoi(cd.c_str());
    }
    self->start_si_ = self->stored_index("Start");
    self->end_si_   = self->stored_index("End");
    // Start / End drive the region-overlap mask and the LENGTH codec, and are
    // read back through array_to_int64 / the End test, which cast the decoded
    // array to exactly Int32Array or Int64Array. decode_colblock honours
    // whatever Arrow type the file's own schema declares (int8, double, string,
    // …), so a file declaring Start or End as any other type would make those
    // casts reinterpret the buffer and read out of bounds. Require integer
    // coordinates up front; a column that is simply absent (index -1) never
    // reaches a cast.
    auto coord_is_int = [&](int si) {
        if (si < 0) return true;
        auto id = self->col_types_[(size_t)si]->id();
        return id == arrow::Type::INT32 || id == arrow::Type::INT64;
    };
    if (!coord_is_int(self->start_si_) || !coord_is_int(self->end_si_))
        return "'" + path + "': LociSSD Start/End columns must be int32 or int64";
    std::string rc;
    if (lociss_manifest_value(meta, "row_count", &rc)) self->row_count_ = std::atoll(rc.c_str());
    lociss_manifest_value(meta, "assembly", &self->assembly_);
    if (!lociss_manifest_value(meta, "species", &self->species_) && !self->assembly_.empty())
        self->species_ = assembly_to_species(self->assembly_);

    // ── fixed-stride index arrays ────────────────────────────────────────────
    size_t p = 24 + meta_len;
    auto take_coord = [&](std::vector<int64_t>& dst) -> bool {
        if (p + (size_t)n_blocks * isz > idx.size()) return false;
        dst.resize(n_blocks);
        for (uint32_t i = 0; i < n_blocks; ++i)
            dst[i] = lociss_v4::rd_int(ib + p + (size_t)i * isz, isz);
        p += (size_t)n_blocks * isz; return true;
    };
    if (!take_coord(self->cids_) || !take_coord(self->min_start_) ||
        !take_coord(self->max_end_))
        return "LociSSD v4: truncated index (coord arrays)";
    if (p + (size_t)n_blocks * 4 > idx.size()) return "LociSSD v4: truncated index (n_rows)";
    self->n_rows_.resize(n_blocks);
    for (uint32_t i = 0; i < n_blocks; ++i) self->n_rows_[i] = u32(p + (size_t)i * 4);
    p += (size_t)n_blocks * 4;
    if (!take_coord(self->prefix_max_end_))
        return "LociSSD v4: truncated index (prefix_max_end)";
    size_t npc = (size_t)n_blocks * n_cols;
    if (p + npc * 8 > idx.size()) return "LociSSD v4: truncated index (col_offset)";
    self->col_offset_.resize(npc);
    for (size_t i = 0; i < npc; ++i) std::memcpy(&self->col_offset_[i], ib + p + i * 8, 8);
    p += npc * 8;
    if (p + npc * 4 > idx.size()) return "LociSSD v4: truncated index (col_clen)";
    self->col_clen_.resize(npc);
    for (size_t i = 0; i < npc; ++i) std::memcpy(&self->col_clen_[i], ib + p + i * 4, 4);

    // block_first_row + chromosome name maps.
    self->block_first_row_.resize(n_blocks);
    int64_t acc = 0;
    for (uint32_t i = 0; i < n_blocks; ++i) { self->block_first_row_[i] = acc; acc += self->n_rows_[i]; }
    for (uint32_t b = 0; b < n_blocks; ++b) {
        int64_t cid = self->cids_[b];
        if (self->rank_to_name_.count(cid)) continue;
        std::string nm;
        lociss_v4::json_object_value(meta, "rank_to_name", std::to_string(cid), &nm);
        if (nm.empty()) nm = std::to_string(cid);
        self->rank_to_name_[cid] = nm;
        self->name_to_rank_[nm]  = cid;
    }

    // ── display schema: Chromosome (synthesized) + stored columns ───────────
    arrow::FieldVector fields;
    fields.push_back(arrow::field("Chromosome", arrow::utf8()));
    for (int c = 0; c < (int)n_cols; ++c)
        fields.push_back(arrow::field(self->stored_[(size_t)c], self->col_types_[(size_t)c]));
    self->schema_ = arrow::schema(fields);

    if (!cfg.region.empty()) {
        std::string err = self->build_region(cfg);
        if (!err.empty()) return err;
    }
    *out = std::move(self);
    return "";
}

// Region (-r) — block-prune via the index zone-map (§5), then per-row overlap
// in read_chunk. Mirrors ParquetSource's region_mode_ exact-count pass.
std::string LocissV4Source::build_region(const Config& cfg) {
    // A region query decodes Start/End to mask rows; without both stored columns
    // the mask/count path would index them out of range.
    if (start_si_ < 0 || end_si_ < 0)
        return "LociSSD v4: region query needs Start and End columns";
    region_mode_ = true;
    auto windows = parse_region_list(cfg.region, cfg.coords_one_based);
    resolve_region_chroms(windows,
        [&](const std::string& n){ return name_to_rank_.count(n) > 0; }, path_);
    for (const auto& w : windows) {
        auto it = name_to_rank_.find(w.chrom);
        if (it == name_to_rank_.end()) continue;          // chrom not present
        int64_t cid = it->second;
        int lo_c = (int)(std::lower_bound(cids_.begin(), cids_.end(), cid) - cids_.begin());
        int hi_c = (int)(std::upper_bound(cids_.begin(), cids_.end(), cid) - cids_.begin());
        if (lo_c >= hi_c) continue;
        int64_t hi = w.end, lo = w.start;
        int ub = (hi == INT64_MAX) ? hi_c
            : (int)(std::lower_bound(min_start_.begin() + lo_c,
                                     min_start_.begin() + hi_c, hi) - min_start_.begin());
        int lb = (lo == INT64_MIN) ? lo_c
            : (int)(std::upper_bound(prefix_max_end_.begin() + lo_c,
                                     prefix_max_end_.begin() + hi_c, lo) - prefix_max_end_.begin());
        for (int b = lb; b < ub; ++b)
            if (lo == INT64_MIN || max_end_[(size_t)b] > lo)
                slices_.push_back({b, w});
    }
    slice_first_row_.assign(slices_.size(), 0);
    slice_count_.assign(slices_.size(), 0);
    int64_t total = 0;
    for (size_t i = 0; i < slices_.size(); ++i) {
        slice_first_row_[i] = total;
        std::shared_ptr<arrow::Table> t;
        if (read_chunk((int)i, {1}, &t).ok() && t) slice_count_[i] = t->num_rows();
        total += slice_count_[i];
    }
    region_total_ = total;
    return "";
}
