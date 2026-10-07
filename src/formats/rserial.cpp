// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// R's serialization format, as written by saveRDS(), save() and serialize()
// (R's src/main/serialize.c is the reference): an optional "RDX2\n" /
// "RDX3\n" .RData magic, a format line (X = XDR big-endian binary, B = native
// binary, A = ASCII), the version header, then one item read recursively.
// Every item starts with a flags word (type, levels, object / attribute / tag
// bits); symbols, environments and a few others enter a reference table that
// later REFSXP items point back into.
//
// Input is untrusted: a declared length is never allocated up front — vectors
// grow as their elements arrive, so a short file fails at its end instead of
// asking for gigabytes — nesting is capped, and every index (reference,
// bytecode representation) is checked.

#include "rserial.hpp"

#include "internal.hpp"

namespace rser {

std::vector<std::string> Value::klass() const {
    std::vector<std::string> out;
    if (const Value* c = attr("class"); c && c->type == STRSXP)
        for (const auto& s : c->strs) if (s) out.push_back(*s);
    return out;
}
bool Value::inherits(std::string_view cls) const {
    for (const auto& c : klass()) if (c == cls) return true;
    return false;
}
int64_t Value::length() const {
    switch (type) {
        case LGLSXP: case INTSXP:  return (int64_t)ints.size();
        case REALSXP:              return (int64_t)reals.size();
        case CPLXSXP:              return (int64_t)reals.size() / 2;
        case STRSXP:               return (int64_t)strs.size();
        case RAWSXP:               return (int64_t)raw.size();
        case VECSXP: case EXPRSXP: case LISTSXP: case LANGSXP: case ENVSXP:
            return (int64_t)items.size();
        default:                   return type == NILSXP ? 0 : 1;
    }
}

namespace {

// Pseudo-types of the serialization stream (serialize.c).
enum : int {
    REFSXP = 255, NILVALUE_SXP = 254, GLOBALENV_SXP = 253, UNBOUNDVALUE_SXP = 252,
    MISSINGARG_SXP = 251, BASENAMESPACE_SXP = 250, NAMESPACESXP = 249, PACKAGESXP = 248,
    PERSISTSXP = 247, CLASSREFSXP = 246, GENERICREFSXP = 245, BCREPDEF = 244, BCREPREF = 243,
    EMPTYENV_SXP = 242, BASEENV_SXP = 241, ATTRLANGSXP = 240, ATTRLISTSXP = 239, ALTREP_SXP = 238,
};

#ifdef VV_FUZZ
constexpr int     kMaxDepth     = 256;
constexpr int64_t kMaxAltrepLen = int64_t(1) << 20;
#else
constexpr int     kMaxDepth     = 2048;
constexpr int64_t kMaxAltrepLen = int64_t(1) << 28;   // a compact 1:n expands to 4·n bytes
#endif
constexpr size_t kElemChunk = 1 << 16;     // elements read (and grown) at a time

struct ParseError { std::string msg; };
[[noreturn]] void fail(std::string m) { throw ParseError{std::move(m)}; }

class Parser {
public:
    Parser(std::shared_ptr<arrow::io::InputStream> in, File* f) : in_(std::move(in)), f_(f) {}

    void run() {
        char m[5];
        bytes(m, 2);
        if (m[0] == 'R' && m[1] == 'D') {            // .RData: RDX2 / RDX3 / RDA2 / RDB2 ...
            bytes(m + 2, 3);
            if (m[4] != '\n' || (m[3] != '2' && m[3] != '3') ||
                (m[2] != 'X' && m[2] != 'A' && m[2] != 'B'))
                fail("not an R data file (bad RD magic)");
            f_->rdata = true;
            bytes(m, 2);
        }
        if (m[1] != '\n') fail("not an R serialization (no format line)");
        switch (m[0]) {
            case 'X': fmt_ = 'X'; f_->format = "xdr"; break;
            case 'B': fmt_ = 'B'; f_->format = "binary"; break;
            case 'A': fmt_ = 'A'; f_->format = "ascii"; break;
            default:  fail(std::string("unsupported R serialization format '") + m[0] + "'");
        }
        const int32_t version = i32();
        const int32_t writer = i32();
        (void)i32();                                  // minimal reader version
        if (version != 2 && version != 3)
            fail("unsupported R serialization version " + std::to_string(version));
        f_->version = version;
        if (writer > 0)
            f_->writer = std::to_string(writer / 65536) + "." + std::to_string(writer / 256 % 256) + "." +
                         std::to_string(writer % 256);
        if (version == 3) {                           // native encoding name
            const int32_t n = i32();
            if (n < 0 || n > 4096) fail("bad native encoding length");
            (void)str(n);
        }
        const Value* top = item(0);
        if (!f_->rdata) {
            f_->objects.emplace_back("", top);
            return;
        }
        // .RData: a tagged pairlist of the saved objects.
        if (top->type != LISTSXP) fail("R data file without an object list");
        for (size_t k = 0; k < top->items.size(); ++k)
            f_->objects.emplace_back(k < top->tags.size() ? top->tags[k] : "", top->items[k]);
    }

private:
    std::shared_ptr<arrow::io::InputStream> in_;
    File* f_;
    char fmt_ = 'X';
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    std::vector<const Value*> refs_;
    const Value* nil_ = nullptr;

    // ── input ────────────────────────────────────────────────────────────────
    void need(size_t n) {
        if (buf_.size() - pos_ >= n) return;
        buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)pos_);
        pos_ = 0;
        while (buf_.size() < n) {
            const int64_t want = std::max<int64_t>((int64_t)(n - buf_.size()), 1 << 20);
            auto r = in_->Read(want);
            if (!r.ok()) fail("read error: " + r.status().message());
            const auto& b = *r;
            if (b->size() == 0) fail("unexpected end of file");
            buf_.insert(buf_.end(), b->data(), b->data() + b->size());
        }
    }
    void bytes(void* dst, size_t n) {
        need(n);
        std::memcpy(dst, buf_.data() + pos_, n);
        pos_ += n;
    }
    int getc_ascii() {
        need(1);
        return buf_[pos_++];
    }
    // ASCII format: one whitespace-separated word.
    std::string word() {
        int c;
        do c = getc_ascii(); while (std::isspace(c));
        std::string w;
        while (!std::isspace(c)) {
            w += (char)c;
            if (w.size() > 4096) fail("ASCII token too long");
            c = getc_ascii();
        }
        return w;
    }
    int32_t i32() {
        if (fmt_ == 'A') {
            const std::string w = word();
            if (w == "NA") return kNaInteger;
            errno = 0;
            char* end = nullptr;
            const long v = std::strtol(w.c_str(), &end, 10);
            if (errno || !end || *end || v < INT32_MIN || v > INT32_MAX) fail("bad ASCII integer '" + w + "'");
            return (int32_t)v;
        }
        uint8_t b[4];
        bytes(b, 4);
        if (fmt_ == 'X') return (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
        int32_t v;
        std::memcpy(&v, b, 4);
        return v;
    }
    double f64() {
        if (fmt_ == 'A') {
            const std::string w = word();
            if (w == "NA") {                          // R's NA_real_: a NaN with low word 1954
                const uint64_t bits = 0x7FF00000000007A2ULL;
                double d;
                std::memcpy(&d, &bits, 8);
                return d;
            }
            if (w == "NaN") return std::nan("");
            if (w == "Inf") return HUGE_VAL;
            if (w == "-Inf") return -HUGE_VAL;
            char* end = nullptr;
            const double d = std::strtod(w.c_str(), &end);
            if (!end || *end) fail("bad ASCII double '" + w + "'");
            return d;
        }
        uint8_t b[8];
        bytes(b, 8);
        if (fmt_ == 'X') std::reverse(b, b + 8);
        double d;
        std::memcpy(&d, b, 8);
        return d;
    }
    std::string str(int64_t n) {
        std::string s;
        if (fmt_ != 'A') {
            while ((int64_t)s.size() < n) {               // in pieces: n is untrusted
                const size_t k = (size_t)std::min<int64_t>(n - (int64_t)s.size(), 1 << 20);
                need(k);
                s.append((const char*)buf_.data() + pos_, k);
                pos_ += k;
            }
            return s;
        }
        if (n <= 0) return s;
        int c;
        do c = getc_ascii(); while (std::isspace(c));
        for (int64_t i = 0; i < n; ++i) {
            if (i) c = getc_ascii();
            if (c != '\\') { s += (char)c; continue; }
            c = getc_ascii();
            switch (c) {
                case 'n': s += '\n'; break;  case 't': s += '\t'; break;
                case 'v': s += '\v'; break;  case 'b': s += '\b'; break;
                case 'r': s += '\r'; break;  case 'f': s += '\f'; break;
                case 'a': s += '\a'; break;
                default:
                    if (c >= '0' && c <= '7') {
                        int d = 0, j = 0;
                        while (c >= '0' && c <= '7' && j < 3) {
                            d = d * 8 + (c - '0');
                            ++j;
                            if (j < 3) {
                                need(1);
                                const int nx = buf_[pos_];
                                if (nx < '0' || nx > '7') break;
                                c = getc_ascii();
                            }
                        }
                        s += (char)d;
                    } else {
                        s += (char)c;                // \\ \? \' \" and anything else
                    }
            }
        }
        return s;
    }
    int64_t length() {
        const int32_t n = i32();
        if (n >= 0) return n;
        if (n != -1) fail("negative vector length");
        const int64_t hi = i32(), lo = i32();       // a long vector: two words
        if (hi < 0 || lo < 0 || hi > (1 << 20)) fail("bad long vector length");
        return hi << 32 | (uint32_t)lo;
    }

    // ── values ───────────────────────────────────────────────────────────────
    Value* make(int type) {
        f_->arena.push_back(std::make_unique<Value>());
        f_->arena.back()->type = type;
        return f_->arena.back().get();
    }
    const Value* nil() {
        if (!nil_) nil_ = make(NILSXP);
        return nil_;
    }
    const Value* special_env(const char* name) {
        Value* v = make(ENVSXP);
        v->text = name;
        return v;
    }
    // A pairlist's cells as (tag, value) attributes.
    static void set_attrs(Value* v, const Value* pl) {
        if (!pl || pl->type != LISTSXP) return;
        for (size_t k = 0; k < pl->items.size(); ++k)
            v->attrs.emplace_back(k < pl->tags.size() ? pl->tags[k] : "", pl->items[k]);
    }
    std::string tag_name(const Value* t) {
        if (!t) return "";
        if (t->type == SYMSXP || t->type == CHARSXP) return t->text;
        return "";
    }
    // A CHARSXP's bytes as UTF-8 (R's latin1-marked strings are converted).
    std::string charsxp(int levels, int64_t n) {
        std::string s = str(n);
        if (levels & (1 << 2)) {                       // LATIN1_MASK
            std::string u;
            for (unsigned char c : s) {
                if (c < 0x80) u += (char)c;
                else { u += (char)(0xC0 | c >> 6); u += (char)(0x80 | (c & 0x3F)); }
            }
            return u;
        }
        return valid_utf8(s);
    }

    template <typename T, typename Read>
    void read_vec(std::vector<T>* out, int64_t n, Read rd) {
        out->clear();
        out->reserve((size_t)std::min<int64_t>(n, (int64_t)kElemChunk));
        for (int64_t k = 0; k < n; ++k) out->push_back(rd());
    }
    void read_numbers(Value* v, int64_t n) {
        if (v->type == REALSXP || v->type == CPLXSXP) {
            const int64_t m = v->type == CPLXSXP ? 2 * n : n;
            if (fmt_ == 'A') { read_vec(&v->reals, m, [&] { return f64(); }); return; }
            v->reals.clear();
            v->reals.reserve((size_t)std::min<int64_t>(m, (int64_t)kElemChunk));
            for (int64_t done = 0; done < m;) {
                const size_t k = (size_t)std::min<int64_t>(m - done, (int64_t)kElemChunk);
                need(k * 8);
                const uint8_t* p = buf_.data() + pos_;
                for (size_t j = 0; j < k; ++j) {
                    uint8_t b[8];
                    std::memcpy(b, p + j * 8, 8);
                    if (fmt_ == 'X') std::reverse(b, b + 8);
                    double d;
                    std::memcpy(&d, b, 8);
                    v->reals.push_back(d);
                }
                pos_ += k * 8;
                done += (int64_t)k;
            }
            return;
        }
        if (fmt_ == 'A') { read_vec(&v->ints, n, [&] { return i32(); }); return; }
        v->ints.clear();
        v->ints.reserve((size_t)std::min<int64_t>(n, (int64_t)kElemChunk));
        for (int64_t done = 0; done < n;) {
            const size_t k = (size_t)std::min<int64_t>(n - done, (int64_t)kElemChunk);
            need(k * 4);
            const uint8_t* p = buf_.data() + pos_;
            for (size_t j = 0; j < k; ++j) {
                const uint8_t* b = p + j * 4;
                int32_t x;
                if (fmt_ == 'X') x = (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
                else std::memcpy(&x, b, 4);
                v->ints.push_back(x);
            }
            pos_ += k * 4;
            done += (int64_t)k;
        }
    }

    const Value* item(int depth) { return with_flags(i32(), depth); }

    const Value* with_flags(int32_t flags, int depth) {
        if (depth > kMaxDepth) fail("objects nested deeper than " + std::to_string(kMaxDepth) + " levels");
        int type = flags & 0xFF;
        const int  levels  = flags >> 12;
        const bool is_obj  = flags & (1 << 8);
        bool       hasattr = flags & (1 << 9);
        bool       hastag  = flags & (1 << 10);
        if (type == ATTRLANGSXP) { type = LANGSXP; hasattr = true; }      // older encodings
        else if (type == ATTRLISTSXP) { type = LISTSXP; hasattr = true; }
        switch (type) {
            case NILVALUE_SXP:      return nil();
            case EMPTYENV_SXP:      return special_env("emptyenv");
            case BASEENV_SXP:       return special_env("baseenv");
            case GLOBALENV_SXP:     return special_env("globalenv");
            case UNBOUNDVALUE_SXP:  return nil();
            case MISSINGARG_SXP:    return nil();
            case BASENAMESPACE_SXP: return special_env("namespace:base");
            case REFSXP: {
                int64_t idx = (uint32_t)flags >> 8;
                if (idx == 0) idx = i32();
                if (idx < 1 || idx > (int64_t)refs_.size()) fail("bad reference index " + std::to_string(idx));
                return refs_[(size_t)idx - 1];
            }
            case PERSISTSXP: case PACKAGESXP: case NAMESPACESXP: {
                if (i32() != 0) fail("names in persistent strings are not supported");
                const int64_t n = i32();
                if (n < 0) fail("bad persistent string count");
                Value* v = make(type == PERSISTSXP ? STRSXP : ENVSXP);
                std::string name;
                for (int64_t k = 0; k < n; ++k) {
                    const Value* s = item(depth + 1);
                    if (s->type == CHARSXP) {
                        if (type == PERSISTSXP) v->strs.push_back(s->na ? std::nullopt
                                                                        : std::optional<std::string>(s->text));
                        else name += (name.empty() ? "" : " ") + s->text;
                    }
                }
                if (type != PERSISTSXP) v->text = (type == NAMESPACESXP ? "namespace:" : "package:") + name;
                refs_.push_back(v);
                return v;
            }
            case SYMSXP: {
                const Value* c = item(depth + 1);
                Value* v = make(SYMSXP);
                v->text = c->type == CHARSXP ? c->text : "";
                refs_.push_back(v);
                return v;
            }
            case ENVSXP: {
                (void)i32();                          // locked
                Value* v = make(ENVSXP);
                refs_.push_back(v);                   // before its contents, which may point back
                (void)item(depth + 1);                // enclosure
                const Value* frame = item(depth + 1);
                const Value* hash  = item(depth + 1);
                set_attrs(v, item(depth + 1));
                auto bind = [&](const Value* pl) {
                    if (!pl || pl->type != LISTSXP) return;
                    for (size_t k = 0; k < pl->items.size(); ++k) {
                        v->items.push_back(pl->items[k]);
                        v->tags.push_back(k < pl->tags.size() ? pl->tags[k] : "");
                    }
                };
                bind(frame);
                if (hash && hash->type == VECSXP)
                    for (const Value* b : hash->items) bind(b);
                return v;
            }
            case LISTSXP: case LANGSXP: case CLOSXP: case PROMSXP: case DOTSXP: {
                Value* v = make(type);
                v->obj = is_obj;
                bool first = true;
                for (;;) {
                    if (hasattr) {
                        const Value* a = item(depth + 1);
                        if (first) set_attrs(v, a);
                    }
                    const std::string tag = hastag ? tag_name(item(depth + 1)) : "";
                    v->items.push_back(item(depth + 1));
                    v->tags.push_back(tag);
                    first = false;
                    // A pairlist's CDR chain is read in this loop rather than
                    // recursively, so a long list does not deepen the stack.
                    const int32_t next = i32();
                    const int nt = next & 0xFF;
                    if (nt == NILVALUE_SXP) break;
                    if (nt == LISTSXP || nt == ATTRLISTSXP) {
                        hasattr = (next & (1 << 9)) || nt == ATTRLISTSXP;
                        hastag  = next & (1 << 10);
                        continue;
                    }
                    const Value* cdr = with_flags(next, depth + 1);   // dotted pair / body
                    v->items.push_back(cdr);
                    v->tags.push_back("");
                    break;
                }
                (void)levels;
                return v;
            }
            case ALTREP_SXP: {
                const Value* info  = item(depth + 1);
                const Value* state = item(depth + 1);
                const Value* attr  = item(depth + 1);
                Value* v = altrep(info, state);
                set_attrs(v, attr);
                v->obj = is_obj;
                return v;
            }
            case BCODESXP: {
                const int32_t nreps = i32();
                if (nreps < 0 || nreps > (1 << 20)) fail("bad bytecode representation count");
                std::vector<const Value*> reps((size_t)nreps, nullptr);
                Value* v = bc1(reps, depth + 1);
                if (hasattr) set_attrs(v, item(depth + 1));
                return v;
            }
            case CLASSREFSXP:   fail("class references are not supported");
            case GENERICREFSXP: fail("generic function references are not supported");
            default: break;
        }
        // The short-block types: body, then attributes.
        Value* v = nullptr;
        switch (type) {
            case EXTPTRSXP:
                v = make(EXTPTRSXP);
                refs_.push_back(v);
                (void)item(depth + 1);                // protected value
                (void)item(depth + 1);                // tag
                break;
            case WEAKREFSXP:
                v = make(WEAKREFSXP);
                refs_.push_back(v);
                break;
            case SPECIALSXP: case BUILTINSXP: {
                const int32_t n = i32();
                if (n < 0 || n > 4096) fail("bad primitive name length");
                v = make(type);
                v->text = str(n);
                break;
            }
            case CHARSXP: {
                const int32_t n = i32();
                v = make(CHARSXP);
                if (n == -1) v->na = true;
                else if (n < 0) fail("bad string length");
                else v->text = charsxp(levels, n);
                if (hasattr) (void)item(depth + 1);   // R reads and ignores it
                return v;
            }
            case LGLSXP: case INTSXP: case REALSXP: case CPLXSXP:
                v = make(type);
                read_numbers(v, length());
                break;
            case STRSXP: {
                v = make(STRSXP);
                const int64_t n = length();
                v->strs.reserve((size_t)std::min<int64_t>(n, (int64_t)kElemChunk));
                for (int64_t k = 0; k < n; ++k) {
                    const int32_t ef = i32();
                    if ((ef & 0xFF) == CHARSXP) {     // the usual case, without a Value per string
                        const int32_t len = i32();
                        if (len == -1) v->strs.emplace_back(std::nullopt);
                        else if (len < 0) fail("bad string length");
                        else v->strs.emplace_back(charsxp(ef >> 12, len));
                        if (ef & (1 << 9)) (void)item(depth + 1);
                    } else {
                        const Value* s = with_flags(ef, depth + 1);
                        v->strs.push_back(s->type == CHARSXP && !s->na ? std::optional<std::string>(s->text)
                                                                       : std::nullopt);
                    }
                }
                break;
            }
            case VECSXP: case EXPRSXP: {
                v = make(type);
                const int64_t n = length();
                v->items.reserve((size_t)std::min<int64_t>(n, (int64_t)kElemChunk));
                for (int64_t k = 0; k < n; ++k) v->items.push_back(item(depth + 1));
                break;
            }
            case RAWSXP: {
                v = make(RAWSXP);
                const int64_t n = length();
                if (fmt_ == 'A') {
                    read_vec(&v->raw, n, [&] {
                        const std::string w = word();
                        return (uint8_t)std::strtoul(w.c_str(), nullptr, 16);
                    });
                } else {
                    for (int64_t done = 0; done < n;) {
                        const size_t k = (size_t)std::min<int64_t>(n - done, 1 << 20);
                        need(k);
                        v->raw.insert(v->raw.end(), buf_.begin() + (ptrdiff_t)pos_,
                                      buf_.begin() + (ptrdiff_t)(pos_ + k));
                        pos_ += k;
                        done += (int64_t)k;
                    }
                }
                break;
            }
            case S4SXP:
                v = make(S4SXP);
                break;
            default:
                fail("unknown R object type " + std::to_string(type) + " (written by a later R?)");
        }
        if (hasattr) set_attrs(v, item(depth + 1));
        v->obj = is_obj;
        return v;
    }

    // ── bytecode (closures saved compiled): only walked, never used ─────────
    Value* bc1(std::vector<const Value*>& reps, int depth) {
        if (depth > kMaxDepth) fail("bytecode nested too deep");
        Value* v = make(BCODESXP);
        v->items.push_back(item(depth + 1));          // code
        const int32_t n = i32();
        if (n < 0) fail("bad bytecode constant count");
        for (int32_t k = 0; k < n; ++k) {
            const int32_t t = i32();
            switch (t) {
                case BCODESXP: v->items.push_back(bc1(reps, depth + 1)); break;
                case LANGSXP: case LISTSXP: case BCREPDEF: case BCREPREF: case ATTRLANGSXP: case ATTRLISTSXP:
                    v->items.push_back(bclang(t, reps, depth + 1));
                    break;
                default: v->items.push_back(item(depth + 1));
            }
        }
        return v;
    }
    const Value* bclang(int type, std::vector<const Value*>& reps, int depth) {
        if (depth > kMaxDepth) fail("bytecode nested too deep");
        if (type == BCREPREF) {
            const int32_t k = i32();
            if (k < 0 || k >= (int32_t)reps.size()) fail("bad bytecode reference");
            return reps[(size_t)k] ? reps[(size_t)k] : nil();
        }
        if (type != BCREPDEF && type != LANGSXP && type != LISTSXP && type != ATTRLANGSXP &&
            type != ATTRLISTSXP)
            return item(depth + 1);
        int pos = -1;
        if (type == BCREPDEF) {
            pos = i32();
            type = i32();
            if (pos < 0 || pos >= (int)reps.size()) fail("bad bytecode representation index");
        }
        bool hasattr = false;
        if (type == ATTRLANGSXP) { type = LANGSXP; hasattr = true; }
        else if (type == ATTRLISTSXP) { type = LISTSXP; hasattr = true; }
        Value* v = make(type == LANGSXP ? LANGSXP : LISTSXP);
        if (pos >= 0) reps[(size_t)pos] = v;
        if (hasattr) set_attrs(v, item(depth + 1));
        v->tags.push_back(tag_name(item(depth + 1)));
        v->items.push_back(bclang(i32(), reps, depth + 1));
        v->items.push_back(bclang(i32(), reps, depth + 1));
        return v;
    }

    // ── ALTREP: compact sequences, deferred strings, wrappers ───────────────
    Value* altrep(const Value* info, const Value* state) {
        std::string cls, pkg;
        if (info && info->type == LISTSXP && !info->items.empty()) {
            cls = info->items[0]->text;
            if (info->items.size() > 1) pkg = info->items[1]->text;
        }
        auto seq = [&](bool real) {
            if (!state || state->type != REALSXP || state->reals.size() < 3) fail("bad compact sequence state");
            const double n = state->reals[0], n1 = state->reals[1], inc = state->reals[2];
            if (!(n >= 0 && n <= (double)kMaxAltrepLen)) fail("compact sequence too long");
            Value* v = make(real ? REALSXP : INTSXP);
            for (int64_t k = 0; k < (int64_t)n; ++k) {
                const double x = n1 + (double)k * inc;
                if (real) v->reals.push_back(x);
                else v->ints.push_back(x >= INT32_MIN + 1.0 && x <= INT32_MAX ? (int32_t)x : kNaInteger);
            }
            return v;
        };
        if (cls == "compact_intseq") return seq(false);
        if (cls == "compact_realseq") return seq(true);
        if (cls == "deferred_string") {
            // state: (arg . info); arg is the integer / double vector as.character()
            // was applied to.
            const Value* arg = state && state->type == LISTSXP && !state->items.empty() ? state->items[0] : state;
            Value* v = make(STRSXP);
            if (arg && arg->type == INTSXP) {
                for (int32_t x : arg->ints)
                    v->strs.push_back(x == kNaInteger ? std::nullopt : std::optional<std::string>(std::to_string(x)));
            } else if (arg && arg->type == REALSXP) {
                for (double x : arg->reals) v->strs.push_back(r_double_string(x));
            } else if (arg && arg->type == STRSXP) {
                v->strs = arg->strs;
            } else {
                fail("bad deferred string state");
            }
            return v;
        }
        if (cls.rfind("wrap_", 0) == 0) {
            // state: (x . metadata)
            const Value* x = state && state->type == LISTSXP && !state->items.empty() ? state->items[0] : nullptr;
            if (!x) fail("bad ALTREP wrapper state");
            Value* v = make(x->type);
            *v = *x;
            v->attrs.clear();
            return v;
        }
        fail("ALTREP class '" + cls + "'" + (pkg.empty() ? "" : " (package " + pkg + ")") + " is not supported");
    }
    // as.character() of a double, as R prints it (15 significant digits).
    static std::optional<std::string> r_double_string(double x) {
        if (std::isnan(x)) {
            uint64_t bits;
            std::memcpy(&bits, &x, 8);
            if ((bits & 0xffffffffu) == 1954) return std::nullopt;
            return std::string("NaN");
        }
        if (std::isinf(x)) return std::string(x > 0 ? "Inf" : "-Inf");
        char b[64];
        std::snprintf(b, sizeof b, "%.15g", x);
        return std::string(b);
    }
};

}  // namespace

std::string read(std::shared_ptr<arrow::io::InputStream> in, File* out) {
    try {
        Parser(std::move(in), out).run();
    } catch (const ParseError& e) {
        return e.msg;
    } catch (const std::bad_alloc&) {
        return "out of memory";
    }
    return "";
}

}  // namespace rser
