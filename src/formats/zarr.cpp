// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Zarr stores — a directory (`.zarr`) or a zip of one (`.zarr.zip`) holding a
// hierarchy of groups and chunked, compressed N-d arrays described by JSON
// metadata. ZarrStore implements the Store interface (store.hpp), so AnnData /
// MuData stores written with `write_zarr` get the same tabs as their .h5ad /
// .h5mu, and any other store (OME-Zarr, xarray) the generic hierarchy +
// per-array tabs.
//
// Zarr v2: `.zgroup` / `.zarray` / `.zattrs` per node; chunk keys "i.j" (or
// "i/j" with dimension_separator "/"); a compressor (blosc, zstd, gzip, zlib,
// lz4, bz2) and filters (vlen-utf8 for text). A group may carry only
// `.zattrs` (anndata writes its sparse-matrix groups that way).
//
// Zarr v3: one `zarr.json` per node (node_type, attributes, data_type,
// chunk_grid, chunk_key_encoding "c/i/j" or v2-style, fill_value, codecs: an
// optional transpose, then bytes (endian) or vlen-utf8, then bytes-to-bytes
// codecs: blosc, gzip, zstd, crc32c). A sharded array (sharding_indexed)
// stores each outer chunk as one object: its inner chunks back to back and an
// index of (offset, length) pairs; reads fetch the index and the inner chunks
// they need.

#include "internal.hpp"
#include "store.hpp"

#include <array>
#include <functional>
#include <list>
#include <mutex>
#include <set>
#include <unordered_map>

#if VV_HAVE_BLOSC
#include <blosc.h>
#endif
#if VV_HAVE_BZ2
#include <bzlib.h>
#endif

namespace h5v {
namespace {

// ── A small JSON DOM for the metadata ───────────────────────────────────────
// Tolerant of what Python writes into attributes: NaN, Infinity, -Infinity.

struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double num = 0;
    bool integral = false;        // the number was written without . / e
    int64_t inum = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::string> keys;     // an object: keys[i] → a[i]

    const JVal* get(const std::string& k) const {
        if (t != Obj) return nullptr;
        for (size_t i = 0; i < keys.size() && i < a.size(); ++i) if (keys[i] == k) return &a[i];
        return nullptr;
    }
    std::string str(const std::string& k, const std::string& dflt = "") const {
        const JVal* v = get(k);
        return v && v->t == Str ? v->s : dflt;
    }
};

class JsonParser {
    const char* p_;
    const char* end_;
    int depth_ = 0;
    void ws() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_; }
    bool lit(const char* w) {
        const size_t n = std::strlen(w);
        if ((size_t)(end_ - p_) < n || std::memcmp(p_, w, n) != 0) return false;
        p_ += n;
        return true;
    }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                 out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
               out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t* v) {
        if (end_ - p_ < 4) return false;
        *v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = *p_++;
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string* out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ != '\\') { *out += *p_++; continue; }
            if (++p_ >= end_) return false;
            const char e = *p_++;
            switch (e) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        uint32_t lo;
                        if (!hex4(&lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(*out, cp);
                    break;
                }
                default: return false;
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }
public:
    JsonParser(const char* p, size_t n) : p_(p), end_(p + n) {}
    bool value(JVal* v) {
        if (++depth_ > 64) return false;
        ws();
        bool ok = false;
        if (p_ >= end_) ok = false;
        else if (*p_ == '{') {
            v->t = JVal::Obj;
            ++p_; ws();
            if (p_ < end_ && *p_ == '}') { ++p_; ok = true; }
            else for (;;) {
                std::string k;
                ws();
                if (!string(&k)) break;
                ws();
                if (p_ >= end_ || *p_ != ':') break;
                ++p_;
                JVal x;
                if (!value(&x)) break;
                v->keys.push_back(std::move(k));
                v->a.push_back(std::move(x));
                ws();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == '}') { ++p_; ok = true; }
                break;
            }
        } else if (*p_ == '[') {
            v->t = JVal::Arr;
            ++p_; ws();
            if (p_ < end_ && *p_ == ']') { ++p_; ok = true; }
            else for (;;) {
                JVal x;
                if (!value(&x)) break;
                v->a.push_back(std::move(x));
                ws();
                if (p_ < end_ && *p_ == ',') { ++p_; continue; }
                if (p_ < end_ && *p_ == ']') { ++p_; ok = true; }
                break;
            }
        } else if (*p_ == '"') {
            v->t = JVal::Str;
            ok = string(&v->s);
            if (ok) v->s = valid_utf8(v->s);
        } else if (lit("true")) { v->t = JVal::Bool; v->b = true; ok = true; }
        else if (lit("false")) { v->t = JVal::Bool; ok = true; }
        else if (lit("null")) { v->t = JVal::Null; ok = true; }
        else if (lit("NaN")) { v->t = JVal::Num; v->num = NAN; ok = true; }
        else if (lit("Infinity")) { v->t = JVal::Num; v->num = INFINITY; ok = true; }
        else if (lit("-Infinity")) { v->t = JVal::Num; v->num = -INFINITY; ok = true; }
        else {
            const char* s = p_;
            if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
            bool frac = false;
            while (p_ < end_ && (std::isdigit((unsigned char)*p_) || *p_ == '.' || *p_ == 'e' ||
                                 *p_ == 'E' || *p_ == '-' || *p_ == '+')) {
                if (*p_ == '.' || *p_ == 'e' || *p_ == 'E') frac = true;
                ++p_;
            }
            if (p_ > s) {
                const std::string t(s, p_);
                char* e = nullptr;
                v->t = JVal::Num;
                v->num = std::strtod(t.c_str(), &e);
                ok = e && *e == '\0';
                if (ok && !frac) {
                    errno = 0;
                    const long long i = std::strtoll(t.c_str(), &e, 10);
                    if (errno == 0 && e && *e == '\0') { v->integral = true; v->inum = i; }
                }
            }
        }
        --depth_;
        return ok;
    }
    bool parse(JVal* v) {
        if (!value(v)) return false;
        ws();
        return p_ == end_;
    }
};

// A JSON number as an integer: NaN, ±Infinity and values beyond int64 → `bad`.
int64_t to_i64(double d, int64_t bad = -1) {
    return std::isfinite(d) && std::fabs(d) < 9.2e18 ? (int64_t)d : bad;
}

bool parse_json(const std::string& text, JVal* v) {
    JsonParser p(text.data(), text.size());
    return p.parse(v);
}

// ── Storage: a directory or a zip ───────────────────────────────────────────

class ZarrIO {
public:
    virtual ~ZarrIO() = default;
    // The bytes stored under `key` ("obs/_index/0"); false when absent.
    virtual bool read(const std::string& key, std::string* out) const = 0;
    // Bytes [off, off + len) of a key; off < 0 counts from the end. False
    // when absent or shorter than asked.
    virtual bool read_range(const std::string& key, int64_t off, int64_t len, std::string* out) const {
        std::string all;
        if (!read(key, &all)) return false;
        if (off < 0) off += (int64_t)all.size();
        if (off < 0 || len < 0 || (uint64_t)off + (uint64_t)len > all.size()) return false;
        out->assign(all, (size_t)off, (size_t)len);
        return true;
    }
    virtual int64_t size(const std::string& key) const = 0;   // -1 when absent
    virtual bool exists(const std::string& key) const = 0;
    // Immediate children of a "directory" prefix ("" or "obs/"): names.
    virtual std::vector<std::string> list(const std::string& prefix) const = 0;
    virtual bool is_dir(const std::string& prefix) const = 0;
    // Bytes stored under a prefix (all keys below it), and in total.
    virtual uint64_t bytes_under(const std::string& prefix) const = 0;
    virtual int64_t total_bytes() const = 0;
};

class DirIO : public ZarrIO {
    std::string root_;   // ends with '/'
public:
    explicit DirIO(std::string root) : root_(std::move(root)) {
        if (root_.empty() || root_.back() != '/') root_ += '/';
    }
    bool read(const std::string& key, std::string* out) const override {
        std::ifstream f(root_ + key, std::ios::binary);
        if (!f) return false;
        std::error_code ec;
        if (std::filesystem::is_directory(root_ + key, ec)) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        *out = ss.str();
        return true;
    }
    bool read_range(const std::string& key, int64_t off, int64_t len, std::string* out) const override {
        const int64_t n = size(key);
        if (n < 0) return false;
        if (off < 0) off += n;
        if (off < 0 || len < 0 || off > n || len > n - off) return false;
        std::ifstream f(root_ + key, std::ios::binary);
        if (!f) return false;
        f.seekg(off);
        out->assign((size_t)len, '\0');
        f.read(out->data(), len);
        return f.gcount() == len;
    }
    int64_t size(const std::string& key) const override {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(root_ + key, ec)) return -1;
        const auto n = std::filesystem::file_size(root_ + key, ec);
        return ec ? -1 : (int64_t)n;
    }
    bool exists(const std::string& key) const override {
        std::error_code ec;
        return std::filesystem::is_regular_file(root_ + key, ec);
    }
    std::vector<std::string> list(const std::string& prefix) const override {
        std::vector<std::string> v;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(root_ + prefix, ec), e; !ec && it != e; it.increment(ec))
            v.push_back(it->path().filename().string());
        std::sort(v.begin(), v.end());
        return v;
    }
    bool is_dir(const std::string& prefix) const override {
        std::error_code ec;
        return std::filesystem::is_directory(root_ + prefix, ec);
    }
    uint64_t bytes_under(const std::string& prefix) const override {
        uint64_t n = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root_ + prefix, ec), e; !ec && it != e;
             it.increment(ec)) {
            std::error_code ec2;
            if (it->is_regular_file(ec2)) {
                const std::string name = it->path().filename().string();
                if (name.rfind(".z", 0) == 0 || name == "zarr.json") continue;
                n += (uint64_t)it->file_size(ec2);
            }
        }
        return n;
    }
    int64_t total_bytes() const override {
        uint64_t n = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root_, ec), e; !ec && it != e; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_regular_file(ec2)) n += (uint64_t)it->file_size(ec2);
        }
        return (int64_t)n;
    }
};

// A zipped store (zarr's ZipStore writes the keys at the zip root; a zipped
// directory has them under one top-level folder, which becomes the prefix).
class ZipIO : public ZarrIO {
    struct Entry { unz64_file_pos pos; uint64_t size, csize; };
    unzFile zf_ = nullptr;
    std::string prefix_;
    std::map<std::string, Entry> entries_;          // key (prefix stripped) → member
    std::set<std::string> dirs_;                    // every "a/", "a/b/" prefix
    int64_t file_size_ = -1;
    mutable std::mutex mu_;
public:
    ~ZipIO() override { if (zf_) unzClose(zf_); }
    std::string open(const std::string& path) {
        zf_ = unzOpen64(path.c_str());
        if (!zf_) return "not a zip file";
        std::error_code ec;
        file_size_ = (int64_t)std::filesystem::file_size(path, ec);
        std::map<std::string, Entry> all;
        for (int r = unzGoToFirstFile(zf_); r == UNZ_OK; r = unzGoToNextFile(zf_)) {
            unz_file_info64 fi;
            char name[4096];
            if (unzGetCurrentFileInfo64(zf_, &fi, name, sizeof name, nullptr, 0, nullptr, 0) != UNZ_OK) continue;
            std::string n(name);
            if (n.empty() || n.back() == '/') continue;
            Entry e{};
            if (unzGetFilePos64(zf_, &e.pos) != UNZ_OK) continue;
            e.size = fi.uncompressed_size;
            e.csize = fi.compressed_size;
            all.emplace(std::move(n), e);
            if (all.size() > 10000000) return "more than 10 million members";
        }
        // The store root: where .zgroup / .zarray / zarr.json sit.
        auto is_meta = [](const std::string& n) {
            return n == ".zgroup" || n == ".zarray" || n == ".zattrs" || n == "zarr.json";
        };
        bool at_root = false;
        std::set<std::string> tops;
        for (const auto& kv : all) {
            if (is_meta(kv.first)) at_root = true;
            const size_t sl = kv.first.find('/');
            if (sl != std::string::npos && is_meta(kv.first.substr(sl + 1))) tops.insert(kv.first.substr(0, sl + 1));
        }
        if (!at_root && tops.size() == 1) prefix_ = *tops.begin();
        for (auto& kv : all) {
            if (kv.first.compare(0, prefix_.size(), prefix_) != 0) continue;
            const std::string key = kv.first.substr(prefix_.size());
            for (size_t sl = key.find('/'); sl != std::string::npos; sl = key.find('/', sl + 1))
                dirs_.insert(key.substr(0, sl + 1));
            entries_.emplace(key, kv.second);
        }
        return "";
    }
    bool read(const std::string& key, std::string* out) const override {
        auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        unz64_file_pos pos = it->second.pos;
        if (unzGoToFilePos64(zf_, &pos) != UNZ_OK || unzOpenCurrentFile(zf_) != UNZ_OK) return false;
        out->clear();
        char buf[65536];
        int n;
        // The declared size is not trusted for the allocation: read until EOF.
        while ((n = unzReadCurrentFile(zf_, buf, sizeof buf)) > 0) {
            out->append(buf, (size_t)n);
            if (out->size() > ((size_t)1 << 32)) { n = -1; break; }
        }
        unzCloseCurrentFile(zf_);
        return n == 0;
    }
    bool exists(const std::string& key) const override { return entries_.count(key) > 0; }
    int64_t size(const std::string& key) const override {
        auto it = entries_.find(key);
        return it == entries_.end() ? -1 : (int64_t)it->second.size;
    }
    // Range reads of a member read it whole (members are decompressed
    // sequentially); the last member is kept for the next range.
    bool read_range(const std::string& key, int64_t off, int64_t len, std::string* out) const override {
        std::shared_ptr<const std::string> all;
        {
            std::lock_guard<std::mutex> lk(last_mu_);
            if (last_key_ == key) all = last_;
        }
        if (!all) {
            auto buf = std::make_shared<std::string>();
            if (!read(key, buf.get())) return false;
            all = buf;
            std::lock_guard<std::mutex> lk(last_mu_);
            last_key_ = key;
            last_ = all;
        }
        if (off < 0) off += (int64_t)all->size();
        if (off < 0 || len < 0 || (uint64_t)off + (uint64_t)len > all->size()) return false;
        out->assign(*all, (size_t)off, (size_t)len);
        return true;
    }
private:
    mutable std::mutex last_mu_;
    mutable std::string last_key_;
    mutable std::shared_ptr<const std::string> last_;
public:
    std::vector<std::string> list(const std::string& prefix) const override {
        std::set<std::string> names;
        for (auto it = entries_.lower_bound(prefix); it != entries_.end(); ++it) {
            if (it->first.compare(0, prefix.size(), prefix) != 0) break;
            const std::string rest = it->first.substr(prefix.size());
            names.insert(rest.substr(0, rest.find('/')));
        }
        return {names.begin(), names.end()};
    }
    bool is_dir(const std::string& prefix) const override {
        return prefix.empty() || dirs_.count(prefix) > 0;
    }
    uint64_t bytes_under(const std::string& prefix) const override {
        uint64_t n = 0;
        for (auto it = entries_.lower_bound(prefix); it != entries_.end(); ++it) {
            if (it->first.compare(0, prefix.size(), prefix) != 0) break;
            const std::string leaf = it->first.substr(it->first.rfind('/') + 1);
            if (leaf.rfind(".z", 0) == 0 || leaf == "zarr.json") continue;
            n += it->second.csize;
        }
        return n;
    }
    int64_t total_bytes() const override { return file_size_; }
};

// ── Array metadata ──────────────────────────────────────────────────────────

struct Codec {
    std::string id;              // blosc, zstd, gzip, zlib, lz4, bz2, vlen-utf8, ...
    JVal conf;
};

struct ArrayMeta {
    std::vector<int64_t> shape, chunks;
    char kind = 0;               // i u f b U S O M m  (0: unsupported dtype)
    int  itemsize = 0;           // bytes per element (O: 0)
    bool big_endian = false;
    bool fortran = false;
    std::string sep = ".";       // chunk key separator
    std::string key_prefix;      // v3 default encoding: "c" + sep
    JVal fill;
    // Decoding: the bytes-to-bytes codecs in reverse (decompression,
    // checksums), then the array-to-bytes step: vlen strings, or fixed-size
    // elements in `big_endian` order; `fortran` is the element order.
    std::vector<Codec> bytes_codecs;
    bool vlen = false;
    std::string codecs_label;    // the storage summary's "filters"
    std::string unsupported;     // a codec the decoder lacks: why reads fail
    // Sharded (v3 sharding_indexed): `chunks` are the inner chunks, a shard
    // holds `shard_ratio[d]` of them per dimension; its index is
    // n_inner × (offset, length) uint64 at the start or end of the shard.
    bool sharded = false;
    std::vector<int64_t> shard_ratio;
    std::vector<Codec> shard_codecs;   // bytes-to-bytes codecs over a whole shard
    bool index_at_end = true, index_crc32c = false, index_big_endian = false;
    std::string dtype_label;
    int64_t chunk_elems = 1;
};

bool parse_v2_dtype(const std::string& d, ArrayMeta* m) {
    if (d.size() < 2) return false;
    const char order = d[0];
    m->big_endian = order == '>';
    m->kind = d[1];
    const std::string num = d.substr(2);
    int n = 0;
    if (!num.empty()) {
        if (num.find('[') != std::string::npos) n = std::atoi(num.c_str());   // M8[ns]
        else n = std::atoi(num.c_str());
    }
    switch (m->kind) {
        case 'i': case 'u':
            if (n != 1 && n != 2 && n != 4 && n != 8) return false;
            m->itemsize = n;
            m->dtype_label = (m->kind == 'u' ? "uint" : "int") + std::to_string(n * 8);
            return true;
        case 'f':
            if (n != 2 && n != 4 && n != 8) return false;
            m->itemsize = n;
            m->dtype_label = "float" + std::to_string(n * 8);
            return true;
        case 'b':
            m->itemsize = 1;
            m->dtype_label = "bool";
            return true;
        case 'U':
            if (n <= 0 || n > (1 << 20)) return false;
            m->itemsize = 4 * n;
            m->dtype_label = "string[" + std::to_string(n) + "]";
            return true;
        case 'S':
            if (n <= 0 || n > (1 << 22)) return false;
            m->itemsize = n;
            m->dtype_label = "string[" + std::to_string(n) + "]";
            return true;
        case 'O':
            m->itemsize = 0;
            m->dtype_label = "string";
            return true;
        case 'M': case 'm':
            m->itemsize = 8;
            m->dtype_label = d.substr(1);
            return true;
        default:
            return false;
    }
}

std::vector<int64_t> int_list(const JVal* v) {
    std::vector<int64_t> out;
    if (!v || v->t != JVal::Arr) return out;
    for (const auto& x : v->a) out.push_back(x.t == JVal::Num ? to_i64(x.num) : -1);
    return out;
}

// The largest decoded chunk read (fixed by the dtype and chunk shape, or
// declared by a vlen / blosc stream). Fuzz builds read smaller chunks so the
// fuzzer explores decoding rather than the memory limit.
#ifndef VV_ZARR_MAX_CHUNK
#define VV_ZARR_MAX_CHUNK ((size_t)1 << 30)
#endif
constexpr size_t kMaxDecoded = VV_ZARR_MAX_CHUNK;

// Validates shape / chunks; "" or why the array is unreadable.
std::string finish_meta(ArrayMeta* m) {
    if (m->shape.size() != m->chunks.size()) return "shape and chunks differ in rank";
    if (m->shape.size() > 32) return "more than 32 dimensions";
    m->chunk_elems = 1;
    for (size_t d = 0; d < m->shape.size(); ++d) {
        if (m->shape[d] < 0) return "negative shape";
        if (m->chunks[d] <= 0) return "chunk size must be positive";
        if (m->chunk_elems > ((int64_t)1 << 31) / m->chunks[d]) return "chunks larger than 2^31 elements";
        m->chunk_elems *= m->chunks[d];
    }
    if (m->itemsize > 0 && (uint64_t)m->chunk_elems * (uint64_t)m->itemsize > kMaxDecoded)
        return "a chunk larger than " + std::to_string(kMaxDecoded >> 20) + " MiB";
    return "";
}

std::string codec_label(const Codec& c) {
    if (c.id == "blosc") {
        std::string s = "blosc " + c.conf.str("cname", "lz4");
        if (const JVal* l = c.conf.get("clevel"); l && l->t == JVal::Num) s += " " + std::to_string(to_i64(l->num, 0));
        if (const JVal* sh = c.conf.get("shuffle")) {
            if (sh->t == JVal::Num) s += sh->num == 1 ? " + shuffle" : sh->num == 2 ? " + bitshuffle" : "";
            else if (sh->t == JVal::Str && sh->s != "noshuffle") s += " + " + sh->s;
        }
        return s;
    }
    if (const JVal* l = c.conf.get("level"); l && l->t == JVal::Num && (c.id == "zstd" || c.id == "gzip" || c.id == "zlib"))
        return c.id + " " + std::to_string(to_i64(l->num, 0));
    return c.id;
}

// ── Decompression ───────────────────────────────────────────────────────────


std::string inflate_zlib(const std::string& in, size_t expect, std::string* out) {
    z_stream z{};
    if (inflateInit2(&z, 15 + 32) != Z_OK) return "zlib: cannot start";   // gzip or zlib header
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)std::min<size_t>(in.size(), UINT_MAX);
    out->clear();
    // Grown as output arrives (up to `expect` when known): a header cannot
    // make us allocate a size the data does not back.
    std::string buf(std::min(expect ? expect : kMaxDecoded, std::max<size_t>(in.size() * 4, 1 << 16)), '\0');
    size_t have = 0;
    int r = Z_OK;
    while (r != Z_STREAM_END) {
        if (have == buf.size()) {
            const size_t cap = expect ? expect : kMaxDecoded;
            if (buf.size() >= cap) { inflateEnd(&z); return "decoded chunk too large"; }
            buf.resize(std::min(buf.size() * 2, cap));
        }
        z.next_out = (Bytef*)buf.data() + have;
        z.avail_out = (uInt)std::min<size_t>(buf.size() - have, UINT_MAX);
        r = inflate(&z, Z_NO_FLUSH);
        have = (size_t)((char*)z.next_out - buf.data());
        if (r == Z_BUF_ERROR && z.avail_in == 0) break;
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) { inflateEnd(&z); return "gzip / zlib: corrupt data"; }
    }
    inflateEnd(&z);
    if (r != Z_STREAM_END) return "gzip / zlib: truncated data";
    buf.resize(have);
    *out = std::move(buf);
    return "";
}

std::string inflate_arrow(arrow::Compression::type t, const char* what, const std::string& in, size_t expect,
                          std::string* out) {
    auto codec = arrow::util::Codec::Create(t);
    if (!codec.ok()) return std::string(what) + ": not available in this Arrow build";
    auto dec = (*codec)->MakeDecompressor();
    if (!dec.ok()) return std::string(what) + ": cannot start";
    std::string buf(std::min(expect ? expect : kMaxDecoded, std::max<size_t>(in.size() * 4, 1 << 16)), '\0');
    size_t have = 0, used = 0;
    for (;;) {
        if (have == buf.size()) {
            const size_t cap = expect ? expect : kMaxDecoded;
            if (buf.size() >= cap) return "decoded chunk too large";
            buf.resize(std::min(buf.size() * 2, cap));
        }
        auto r = (*dec)->Decompress((int64_t)(in.size() - used), (const uint8_t*)in.data() + used,
                                    (int64_t)(buf.size() - have), (uint8_t*)buf.data() + have);
        if (!r.ok()) return std::string(what) + ": corrupt data";
        used += (size_t)r->bytes_read;
        have += (size_t)r->bytes_written;
        if ((*dec)->IsFinished()) break;
        // No progress with room left in the buffer: the input ended (or will
        // not decode further) before the frame did.
        if (r->bytes_read == 0 && r->bytes_written == 0 && (!r->need_more_output || have < buf.size()))
            return std::string(what) + ": truncated data";
    }
    buf.resize(have);
    *out = std::move(buf);
    return "";
}

// One compressor stage; `expect` is the decoded size when known (fixed-size
// dtypes), 0 otherwise.
uint32_t crc32c(const uint8_t* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string decompress(const Codec& c, const std::string& in, size_t expect, std::string* out) {
    if (c.id == "crc32c") {                      // v3: a little-endian CRC32C appended
        if (in.size() < 4) return "crc32c: truncated data";
        uint32_t want = 0;
        std::memcpy(&want, in.data() + in.size() - 4, 4);
        if (crc32c((const uint8_t*)in.data(), in.size() - 4) != want) return "crc32c: checksum mismatch";
        out->assign(in, 0, in.size() - 4);
        return "";
    }
    if (c.id == "gzip" || c.id == "zlib") return inflate_zlib(in, expect, out);
    if (c.id == "zstd") return inflate_arrow(arrow::Compression::ZSTD, "zstd", in, expect, out);
    if (c.id == "lz4") {                         // numcodecs: uint32 size + raw LZ4 block
        if (in.size() < 4) return "lz4: truncated data";
        uint32_t n = 0;
        std::memcpy(&n, in.data(), 4);
        if (expect && n != expect) return "lz4: unexpected decoded size";
        if (n > kMaxDecoded) return "decoded chunk too large";
        auto codec = arrow::util::Codec::Create(arrow::Compression::LZ4);
        if (!codec.ok()) return "lz4: not available in this Arrow build";
        out->assign(n, '\0');
        auto r = (*codec)->Decompress((int64_t)in.size() - 4, (const uint8_t*)in.data() + 4, (int64_t)n,
                                      (uint8_t*)out->data());
        if (!r.ok() || *r != (int64_t)n) return "lz4: corrupt data";
        return "";
    }
    if (c.id == "blosc") {
#if VV_HAVE_BLOSC
        size_t nbytes = 0;
        if (in.size() < BLOSC_MIN_HEADER_LENGTH || blosc_cbuffer_validate(in.data(), in.size(), &nbytes) != 0)
            return "blosc: corrupt header";
        if (expect && nbytes != expect) return "blosc: unexpected decoded size";
        if (nbytes > kMaxDecoded) return "decoded chunk too large";
        out->assign(nbytes, '\0');
        if (nbytes == 0) return "";
        const int r = blosc_decompress_ctx(in.data(), out->data(), nbytes, 1);
        if (r < 0 || (size_t)r != nbytes) return "blosc: corrupt data";
        return "";
#else
        (void)expect; (void)out;
        return "this vv was built without Blosc (c-blosc)";
#endif
    }
    if (c.id == "bz2") {
#if VV_HAVE_BZ2
        std::string buf(expect ? expect : std::max<size_t>(in.size() * 8, 1 << 16), '\0');
        for (;;) {
            unsigned int n = (unsigned int)std::min<size_t>(buf.size(), UINT_MAX);
            const int r = BZ2_bzBuffToBuffDecompress(buf.data(), &n, const_cast<char*>(in.data()),
                                                     (unsigned int)in.size(), 0, 0);
            if (r == BZ_OK) { buf.resize(n); *out = std::move(buf); return ""; }
            if (r != BZ_OUTBUFF_FULL || expect || buf.size() >= kMaxDecoded) return "bz2: corrupt data";
            buf.resize(std::min(buf.size() * 2, kMaxDecoded));
        }
#else
        (void)expect; (void)out;
        return "this vv was built without bzip2";
#endif
    }
    return "unsupported compressor '" + c.id + "'";
}

// numcodecs VLenUTF8: uint32 count, then per item uint32 length + bytes.
std::string decode_vlen(const std::string& in, int64_t n, std::vector<std::string>* out) {
    if (in.size() < 4) return "vlen-utf8: truncated data";
    uint32_t cnt = 0;
    std::memcpy(&cnt, in.data(), 4);
    if ((int64_t)cnt != n) return "vlen-utf8: " + std::to_string(cnt) + " items for a chunk of " + std::to_string(n);
    out->clear();
    out->reserve((size_t)std::min<int64_t>(n, (int64_t)in.size() / 4));   // each item takes >= 4 bytes
    size_t at = 4;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (in.size() - at < 4) return "vlen-utf8: truncated data";
        uint32_t len = 0;
        std::memcpy(&len, in.data() + at, 4);
        at += 4;
        if (in.size() - at < len) return "vlen-utf8: truncated data";
        out->emplace_back(in.data() + at, len);
        at += len;
    }
    return "";
}

// ── A decoded chunk ─────────────────────────────────────────────────────────

struct Chunk {
    bool missing = false;                 // not stored: every element is the fill value
    std::string bytes;                    // fixed-size elements, native byte order
    std::vector<std::string> strs;        // O / vlen strings
};

void byteswap(std::string& b, int itemsize) {
    if (itemsize <= 1) return;
    for (size_t i = 0; i + (size_t)itemsize <= b.size(); i += (size_t)itemsize)
        std::reverse(b.begin() + (long)i, b.begin() + (long)i + itemsize);
}

double half_to_double(uint16_t h) {
    const int s = h >> 15, e = (h >> 10) & 0x1F, f = h & 0x3FF;
    double v;
    if (e == 0) v = std::ldexp((double)f, -24);
    else if (e == 31) v = f ? NAN : INFINITY;
    else v = std::ldexp((double)(f | 0x400), e - 25);
    return s ? -v : v;
}

// The fill value of an array as a number / string.
double fill_f64(const ArrayMeta& m) {
    const JVal& f = m.fill;
    if (f.t == JVal::Num) return f.num;
    if (f.t == JVal::Bool) return f.b ? 1 : 0;
    if (f.t == JVal::Str) {
        if (f.s == "NaN") return NAN;
        if (f.s == "Infinity") return INFINITY;
        if (f.s == "-Infinity") return -INFINITY;
        if (f.s.rfind("0x", 0) == 0) {          // v3: the value's raw bits
            const uint64_t bits = std::strtoull(f.s.c_str() + 2, nullptr, 16);
            if (m.itemsize == 2) return half_to_double((uint16_t)bits);
            if (m.itemsize == 4) { float x; const uint32_t b = (uint32_t)bits; std::memcpy(&x, &b, 4); return x; }
            double x; std::memcpy(&x, &bits, 8); return x;
        }
    }
    return 0;
}
int64_t fill_i64(const ArrayMeta& m) {
    const JVal& f = m.fill;
    if (f.t == JVal::Num) return f.integral ? f.inum : to_i64(f.num, 0);
    if (f.t == JVal::Bool) return f.b ? 1 : 0;
    return 0;
}
std::string fill_str(const ArrayMeta& m) {
    return m.fill.t == JVal::Str && m.kind != 'S' ? m.fill.s : std::string();
}

// Element i (within the chunk) of a fixed-size chunk.
int64_t elem_i64(const ArrayMeta& m, const Chunk& c, int64_t i) {
    if (c.missing) return fill_i64(m);
    const char* p = c.bytes.data() + (size_t)i * (size_t)m.itemsize;
    switch (m.kind) {
        case 'b': return *p ? 1 : 0;
        case 'i': case 'M': case 'm':
            switch (m.itemsize) {
                case 1: return *(const int8_t*)p;
                case 2: { int16_t v; std::memcpy(&v, p, 2); return v; }
                case 4: { int32_t v; std::memcpy(&v, p, 4); return v; }
                default: { int64_t v; std::memcpy(&v, p, 8); return v; }
            }
        case 'u':
            switch (m.itemsize) {
                case 1: return *(const uint8_t*)p;
                case 2: { uint16_t v; std::memcpy(&v, p, 2); return v; }
                case 4: { uint32_t v; std::memcpy(&v, p, 4); return v; }
                default: { uint64_t v; std::memcpy(&v, p, 8); return (int64_t)v; }
            }
        case 'f': {
            double d;
            if (m.itemsize == 2) { uint16_t h; std::memcpy(&h, p, 2); d = half_to_double(h); }
            else if (m.itemsize == 4) { float f; std::memcpy(&f, p, 4); d = f; }
            else std::memcpy(&d, p, 8);
            return std::isfinite(d) && std::fabs(d) < 9.2e18 ? (int64_t)d : 0;
        }
        default: return 0;
    }
}
double elem_f64(const ArrayMeta& m, const Chunk& c, int64_t i) {
    if (c.missing) return fill_f64(m);
    if (m.kind != 'f') return (double)elem_i64(m, c, i);
    const char* p = c.bytes.data() + (size_t)i * (size_t)m.itemsize;
    if (m.itemsize == 2) { uint16_t h; std::memcpy(&h, p, 2); return half_to_double(h); }
    if (m.itemsize == 4) { float f; std::memcpy(&f, p, 4); return f; }
    double d;
    std::memcpy(&d, p, 8);
    return d;
}
std::string elem_str(const ArrayMeta& m, const Chunk& c, int64_t i) {
    if (c.missing) return fill_str(m);
    if (m.kind == 'O') return (size_t)i < c.strs.size() ? valid_utf8(c.strs[(size_t)i]) : std::string();
    const char* p = c.bytes.data() + (size_t)i * (size_t)m.itemsize;
    if (m.kind == 'S') return valid_utf8(std::string_view(p, strnlen(p, (size_t)m.itemsize)));
    // U: UTF-32 code points (native order after byteswap), NUL-padded
    std::string out;
    for (int k = 0; k < m.itemsize / 4; ++k) {
        uint32_t cp;
        std::memcpy(&cp, p + 4 * k, 4);
        if (cp == 0) break;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                 out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
               out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    return out;
}

VClass vclass_of(char kind) {
    switch (kind) {
        case 'i': case 'u': case 'M': case 'm': return VClass::Int;
        case 'f': return VClass::Float;
        case 'b': return VClass::Bool;
        case 'U': case 'S': case 'O': return VClass::String;
        default: return VClass::Other;
    }
}

// ── ZarrStore ───────────────────────────────────────────────────────────────

class ZarrStore : public Store {
    std::unique_ptr<ZarrIO> io_;
    int version_ = 2;
    // Metadata per node path, parsed once.
    mutable std::mutex mu_;
    mutable std::map<std::string, std::optional<JVal>> json_cache_;
    mutable std::map<std::string, std::shared_ptr<const ArrayMeta>> meta_cache_;
    mutable std::map<std::string, std::string> meta_err_;
    // Decoded chunks, least recently used dropped first.
    mutable std::list<std::pair<std::string, std::shared_ptr<const Chunk>>> lru_;
    mutable std::unordered_map<std::string, decltype(lru_)::iterator> lru_index_;
    mutable size_t lru_bytes_ = 0;
    static constexpr size_t kCacheBytes = (size_t)256 << 20;

    // "/obs/_index" → "obs/_index/" ("/" → "").
    static std::string prefix(const std::string& path) {
        std::string p = path;
        while (!p.empty() && p.front() == '/') p.erase(0, 1);
        if (!p.empty() && p.back() != '/') p += '/';
        return p;
    }
    const JVal* json(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = json_cache_.find(key);
        if (it == json_cache_.end()) {
            std::string text;
            std::optional<JVal> v;
            JVal j;
            if (io_->read(key, &text) && parse_json(text, &j)) v = std::move(j);
            it = json_cache_.emplace(key, std::move(v)).first;
        }
        return it->second ? &*it->second : nullptr;
    }
    const JVal* attrs(const std::string& path) const {
        if (version_ == 3) {
            const JVal* z = json(prefix(path) + "zarr.json");
            const JVal* a = z ? z->get("attributes") : nullptr;
            return a && a->t == JVal::Obj ? a : nullptr;
        }
        const JVal* a = json(prefix(path) + ".zattrs");
        return a && a->t == JVal::Obj ? a : nullptr;
    }
    // The node type a v3 zarr.json declares ("array" / "group" / "").
    std::string v3_node(const std::string& path) const {
        const JVal* z = json(prefix(path) + "zarr.json");
        return z ? z->str("node_type") : std::string();
    }
    std::string parse_v3(const std::string& path, ArrayMeta* m) const;
    std::shared_ptr<const ArrayMeta> meta(const std::string& path, std::string* why = nullptr) const {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (auto it = meta_cache_.find(path); it != meta_cache_.end()) return it->second;
            if (auto it = meta_err_.find(path); it != meta_err_.end()) {
                if (why) *why = it->second;
                return nullptr;
            }
        }
        std::string err;
        auto m = std::make_shared<ArrayMeta>();
        const JVal* z = version_ == 3 ? nullptr : json(prefix(path) + ".zarray");
        if (version_ == 3) err = parse_v3(path, m.get());
        else if (!z || z->t != JVal::Obj) err = "no readable .zarray";
        if (err.empty() && version_ == 2) {
            m->shape = int_list(z->get("shape"));
            m->chunks = int_list(z->get("chunks"));
            const JVal* dt = z->get("dtype");
            if (!dt || dt->t != JVal::Str || !parse_v2_dtype(dt->s, m.get()))
                err = "unsupported dtype " + (dt && dt->t == JVal::Str ? dt->s : std::string("(structured)"));
        }
        if (err.empty() && version_ == 2) {
            m->fortran = z->str("order", "C") == "F";
            m->sep = z->str("dimension_separator", ".");
            if (m->sep != "." && m->sep != "/") m->sep = ".";
            if (const JVal* f = z->get("fill_value")) m->fill = *f;
            // filters (applied before the compressor when writing): only the
            // vlen string encodings are read.
            std::vector<std::string> labels;
            if (const JVal* fs = z->get("filters"); fs && fs->t == JVal::Arr)
                for (const auto& f : fs->a) {
                    const std::string id = f.str("id");
                    if (id == "vlen-utf8" || id == "vlen-bytes") m->vlen = true;
                    else err = "unsupported filter '" + id + "'";
                    labels.push_back(id);
                }
            if (const JVal* c = z->get("compressor"); c && c->t == JVal::Obj) {
                m->bytes_codecs.push_back(Codec{c->str("id"), *c});
                labels.push_back(codec_label(m->bytes_codecs.back()));
            }
            for (const auto& l : labels) m->codecs_label += (m->codecs_label.empty() ? "" : " + ") + l;
            if (m->kind == 'O' && !m->vlen) err = "object array without a vlen-utf8 filter";
            if (err.empty()) err = finish_meta(m.get());
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (!err.empty()) {
            meta_err_[path] = err;
            if (why) *why = err;
            return nullptr;
        }
        meta_cache_[path] = m;
        return m;
    }
    // The bytes of inner chunk `idx` of a sharded array: false when the shard
    // or the chunk is not stored (fill value).
    arrow::Result<bool> shard_member(const std::string& path, const ArrayMeta& m, const std::vector<int64_t>& idx,
                                     std::string* out) const {
        std::vector<int64_t> sidx(idx.size()), inner(idx.size());
        int64_t n_inner = 1, lin = 0;
        for (size_t d = 0; d < idx.size(); ++d) {
            sidx[d] = idx[d] / m.shard_ratio[d];
            inner[d] = idx[d] % m.shard_ratio[d];
            n_inner *= m.shard_ratio[d];
            lin = lin * m.shard_ratio[d] + inner[d];
        }
        const std::string skey = chunk_key(path, m, sidx);   // the shard's object
        int64_t ssize = io_->size(skey);
        if (ssize < 0) return false;
        // A shard compressed as a whole is decoded (and cached) whole; index
        // offsets then refer to the decoded shard.
        std::shared_ptr<const Chunk> whole;
        if (!m.shard_codecs.empty()) {
            const std::string wkey = "#shard:" + skey;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (auto it = lru_index_.find(wkey); it != lru_index_.end()) {
                    lru_.splice(lru_.begin(), lru_, it->second);
                    whole = it->second->second;
                }
            }
            if (!whole) {
                std::string data;
                if (!io_->read(skey, &data)) return arrow::Status::IOError("'", path, "': cannot read shard ", skey);
                for (size_t k = m.shard_codecs.size(); k-- > 0;) {
                    std::string dec;
                    if (auto e = decompress(m.shard_codecs[k], data, 0, &dec); !e.empty())
                        return arrow::Status::IOError("'", path, "': shard ", skey, ": ", e);
                    data = std::move(dec);
                }
                auto c = std::make_shared<Chunk>();
                c->bytes = std::move(data);
                whole = c;
                std::lock_guard<std::mutex> lk(mu_);
                lru_.emplace_front(wkey, whole);
                lru_index_[wkey] = lru_.begin();
                lru_bytes_ += whole->bytes.size() + 64;
            }
            ssize = (int64_t)whole->bytes.size();
        }
        auto range = [&](int64_t off, int64_t len, std::string* o) {
            if (!whole) return io_->read_range(skey, off, len, o);
            if (off < 0 || len < 0 || off > ssize || len > ssize - off) return false;
            o->assign(whole->bytes, (size_t)off, (size_t)len);
            return true;
        };
        const int64_t isize = n_inner * 16 + (m.index_crc32c ? 4 : 0);
        if (n_inner > ((int64_t)1 << 26) || isize > ssize)
            return arrow::Status::IOError("'", path, "': shard ", skey, ": index larger than the shard");
        std::shared_ptr<const Chunk> index;
        const std::string ikey = "#index:" + skey;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (auto it = lru_index_.find(ikey); it != lru_index_.end()) {
                lru_.splice(lru_.begin(), lru_, it->second);
                index = it->second->second;
            }
        }
        if (!index) {
            std::string raw;
            if (!range(m.index_at_end ? ssize - isize : 0, isize, &raw))
                return arrow::Status::IOError("'", path, "': shard ", skey, ": cannot read its index");
            if (m.index_crc32c) {
                Codec crc{"crc32c", JVal{}};
                std::string body;
                if (auto e = decompress(crc, raw, 0, &body); !e.empty())
                    return arrow::Status::IOError("'", path, "': shard ", skey, " index: ", e);
                raw = std::move(body);
            }
            if (m.index_big_endian) byteswap(raw, 8);
            auto c = std::make_shared<Chunk>();
            c->bytes = std::move(raw);
            index = c;
            std::lock_guard<std::mutex> lk(mu_);
            lru_.emplace_front(ikey, index);
            lru_index_[ikey] = lru_.begin();
            lru_bytes_ += index->bytes.size() + 64;
        }
        uint64_t off = 0, len = 0;
        std::memcpy(&off, index->bytes.data() + lin * 16, 8);
        std::memcpy(&len, index->bytes.data() + lin * 16 + 8, 8);
        if (off == UINT64_MAX && len == UINT64_MAX) return false;    // not stored
        if (off > (uint64_t)ssize || len > (uint64_t)ssize - off || len > kMaxDecoded)
            return arrow::Status::IOError("'", path, "': shard ", skey, ": chunk outside the shard");
        if (!range((int64_t)off, (int64_t)len, out))
            return arrow::Status::IOError("'", path, "': shard ", skey, ": cannot read a chunk");
        return true;
    }
    std::string chunk_key(const std::string& path, const ArrayMeta& m, const std::vector<int64_t>& idx) const {
        std::string k = prefix(path);
        if (idx.empty()) return k + (m.key_prefix.empty() ? "0" : "c");
        k += m.key_prefix;
        for (size_t d = 0; d < idx.size(); ++d) {
            if (d) k += m.sep;
            k += std::to_string(idx[d]);
        }
        return k;
    }
    // The decoded chunk at grid index `idx`.
    arrow::Result<std::shared_ptr<const Chunk>> chunk(const std::string& path, const ArrayMeta& m,
                                                      const std::vector<int64_t>& idx) const {
        // (a sharded array's key names the inner chunk, which has no object
        // of its own: only a cache key)
        const std::string key = chunk_key(path, m, idx) + (m.sharded ? "#inner" : "");
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (auto it = lru_index_.find(key); it != lru_index_.end()) {
                lru_.splice(lru_.begin(), lru_, it->second);
                return it->second->second;
            }
        }
        auto c = std::make_shared<Chunk>();
        std::string raw;
        bool have = false;
        if (m.sharded) {
            auto r = shard_member(path, m, idx, &raw);
            if (!r.ok()) return r.status();
            have = *r;
        } else {
            have = io_->read(key, &raw);
        }
        if (!have) {
            c->missing = true;
        } else {
            std::string err;
            try {
                err = decode(m, std::move(raw), c.get());
            } catch (const std::bad_alloc&) {
                err = "out of memory decoding the chunk";
            }
            if (!err.empty()) return arrow::Status::IOError("'", path, "': chunk ", key, ": ", err);
        }
        std::lock_guard<std::mutex> lk(mu_);
        const size_t sz = c->bytes.size() + c->strs.size() * 32 + 64;
        lru_.emplace_front(key, c);
        lru_index_[key] = lru_.begin();
        lru_bytes_ += sz;
        while (lru_bytes_ > kCacheBytes && lru_.size() > 1) {
            auto& back = lru_.back();
            lru_bytes_ -= std::min(lru_bytes_, back.second->bytes.size() + back.second->strs.size() * 32 + 64);
            lru_index_.erase(back.first);
            lru_.pop_back();
        }
        return std::shared_ptr<const Chunk>(c);
    }
    static std::string decode(const ArrayMeta& m, std::string raw, Chunk* out) {
        if (!m.unsupported.empty()) return m.unsupported;
        const size_t expect = m.itemsize > 0 ? (size_t)m.chunk_elems * (size_t)m.itemsize : 0;
        std::string data = std::move(raw);
        for (size_t k = m.bytes_codecs.size(); k-- > 0;) {
            std::string dec;
            // the decoded size is known only at the last (first applied) stage
            const size_t want = (k == 0 && !m.vlen) ? expect : 0;
            if (auto e = decompress(m.bytes_codecs[k], data, want, &dec); !e.empty()) return e;
            data = std::move(dec);
        }
        if (m.vlen) return decode_vlen(data, m.chunk_elems, &out->strs);
        if (data.size() != expect)
            return "decoded " + std::to_string(data.size()) + " bytes, expected " + std::to_string(expect);
        if (m.big_endian && m.kind != 'S') byteswap(data, m.kind == 'U' ? 4 : m.itemsize);
        out->bytes = std::move(data);
        return "";
    }
    // Visit elements [r0, r0 + nr) × [c0, c0 + nc) (1-D: c0 = 0, nc = 1 with
    // rank 1; 0-D: the one element), chunk by chunk: f(out_index, chunk, i).
    template <typename F>
    arrow::Status visit(const std::string& path, int64_t r0, int64_t nr, int64_t c0, int64_t nc, int rank,
                        F&& f) const {
        std::string why;
        auto m = meta(path, &why);
        if (!m) return arrow::Status::IOError("'", path, "': cannot read Zarr array: ", why);
        if ((int)m->shape.size() != rank && !(rank == 1 && m->shape.empty()))
            return arrow::Status::Invalid("'", path, "': expected a ", rank, "-D array");
        if (m->shape.empty()) {                      // 0-D
            ARROW_ASSIGN_OR_RAISE(auto c, chunk(path, *m, {}));
            if (nr > 0) f(0, *m, *c, 0);
            return arrow::Status::OK();
        }
        if (r0 < 0 || nr < 0 || r0 + nr > m->shape[0]) return arrow::Status::IndexError("'", path, "': rows out of range");
        if (rank == 2 && (c0 < 0 || nc < 0 || c0 + nc > m->shape[1]))
            return arrow::Status::IndexError("'", path, "': columns out of range");
        const int64_t cr = m->chunks[0], cc = rank == 2 ? m->chunks[1] : 1;
        for (int64_t gr = r0 / cr; gr * cr < r0 + nr; ++gr) {
            const int64_t ra = std::max(r0, gr * cr), rb = std::min(r0 + nr, (gr + 1) * cr);
            const int64_t gc_end = rank == 2 ? (c0 + nc + cc - 1) / cc : 1;
            for (int64_t gc = rank == 2 ? c0 / cc : 0; gc < gc_end; ++gc) {
                std::vector<int64_t> idx = rank == 2 ? std::vector<int64_t>{gr, gc} : std::vector<int64_t>{gr};
                ARROW_ASSIGN_OR_RAISE(auto c, chunk(path, *m, idx));
                const int64_t ca = rank == 2 ? std::max(c0, gc * cc) : 0;
                const int64_t cb = rank == 2 ? std::min(c0 + nc, (gc + 1) * cc) : 1;
                // The element's index inside its chunk: chunk-local row lr,
                // column lc, in C (row-major) or Fortran (column-major) order.
                for (int64_t r = ra; r < rb; ++r)
                    for (int64_t col = ca; col < cb; ++col) {
                        const int64_t lr = r - gr * cr, lc = col - gc * cc;
                        const int64_t li = rank == 2 ? (m->fortran ? lc * cr + lr : lr * cc + lc) : lr;
                        const int64_t out = rank == 2 ? (r - r0) * nc + (col - c0) : r - r0;
                        f(out, *m, *c, li);
                    }
            }
        }
        return arrow::Status::OK();
    }

public:
    ZarrStore(std::unique_ptr<ZarrIO> io, int version) : io_(std::move(io)), version_(version) {}

    NodeKind kind(const std::string& path) const override {
        const std::string p = prefix(path);
        if (version_ == 3) {
            const std::string t = v3_node(path);
            return t == "array" ? NodeKind::Array : t == "group" || p.empty() ? NodeKind::Group
                                                                                : NodeKind::Missing;
        }
        if (io_->exists(p + ".zarray")) return NodeKind::Array;
        if (io_->exists(p + ".zgroup") || io_->exists(p + ".zattrs") || p.empty()) return NodeKind::Group;
        // A directory without metadata that holds nodes (an implicit group).
        if (io_->is_dir(p))
            for (const auto& c : io_->list(p))
                if (c.rfind(".", 0) != 0 && kind(path + (path.size() > 1 ? "/" : "") + c) != NodeKind::Missing)
                    return NodeKind::Group;
        return NodeKind::Missing;
    }
    std::vector<std::string> children(const std::string& group) const override {
        std::vector<std::string> out;
        const std::string p = prefix(group);
        if (kind(group) == NodeKind::Array) return out;
        for (const auto& c : io_->list(p)) {
            if (c.empty() || c[0] == '.') continue;
            // Names become tab labels and table cells, which must be UTF-8; a
            // node whose name is not is left out (rather than renamed, which
            // would no longer find its key).
            if (valid_utf8(c) != c) continue;
            const std::string child = (group == "/" || group.empty() ? "/" : group + "/") + c;
            if (io_->is_dir(prefix(child)) && kind(child) != NodeKind::Missing) out.push_back(c);
        }
        return out;
    }
    bool has_attr(const std::string& path, const char* name) const override {
        const JVal* a = attrs(path);
        return a && a->get(name);
    }
    std::string attr_string(const std::string& path, const char* name) const override {
        const JVal* a = attrs(path);
        return a ? a->str(name) : std::string();
    }
    std::vector<int64_t> attr_ints(const std::string& path, const char* name) const override {
        const JVal* a = attrs(path);
        const JVal* v = a ? a->get(name) : nullptr;
        std::vector<int64_t> out;
        if (!v) return out;
        if (v->t == JVal::Num) return {to_i64(v->num, 0)};
        if (v->t == JVal::Arr && v->a.size() <= 1024)
            for (const auto& x : v->a) out.push_back(x.t == JVal::Num ? to_i64(x.num, 0) : 0);
        return out;
    }
    bool attr_bool(const std::string& path, const char* name) const override {
        const JVal* a = attrs(path);
        const JVal* v = a ? a->get(name) : nullptr;
        return v && ((v->t == JVal::Bool && v->b) || (v->t == JVal::Num && v->num != 0));
    }
    std::optional<ArrayInfo> info(const std::string& path) const override {
        auto m = meta(path);
        if (!m) {
            if (kind(path) != NodeKind::Array) return std::nullopt;
            ArrayInfo ai;                      // an array we cannot decode
            const JVal* z = json(prefix(path) + (version_ == 3 ? "zarr.json" : ".zarray"));
            if (z) ai.shape = int_list(z->get("shape"));
            ai.dtype = "?";
            return ai;
        }
        ArrayInfo ai;
        ai.shape = m->shape;
        ai.cls = vclass_of(m->kind);
        ai.dtype = m->dtype_label;
        ai.itemsize = (size_t)m->itemsize;
        return ai;
    }
    arrow::Result<std::shared_ptr<arrow::Array>> read_column(const std::string& path, int64_t off,
                                                             int64_t len) const override {
        std::string why;
        auto m = meta(path, &why);
        if (!m) return arrow::Status::IOError("'", path, "': cannot read Zarr array: ", why);
        if (m->shape.size() > 1) return arrow::Status::Invalid("expected 1-D dataset");
        const int64_t n = m->shape.empty() ? 1 : std::max<int64_t>(len, 0);
        const VClass cls = vclass_of(m->kind);
        std::shared_ptr<arrow::Array> arr;
        if (cls == VClass::String) {
            std::vector<std::string> v((size_t)n);
            ARROW_RETURN_NOT_OK(visit(path, off, n, 0, 1, 1, [&](int64_t o, const ArrayMeta& mm, const Chunk& c,
                                                                 int64_t i) { v[(size_t)o] = elem_str(mm, c, i); }));
            arrow::StringBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(v));
            ARROW_RETURN_NOT_OK(b.Finish(&arr));
        } else if (cls == VClass::Float) {
            std::vector<double> v((size_t)n);
            ARROW_RETURN_NOT_OK(visit(path, off, n, 0, 1, 1, [&](int64_t o, const ArrayMeta& mm, const Chunk& c,
                                                                 int64_t i) { v[(size_t)o] = elem_f64(mm, c, i); }));
            arrow::DoubleBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(v));
            ARROW_RETURN_NOT_OK(b.Finish(&arr));
        } else if (cls == VClass::Bool) {
            std::vector<bool> v((size_t)n);
            ARROW_RETURN_NOT_OK(visit(path, off, n, 0, 1, 1, [&](int64_t o, const ArrayMeta& mm, const Chunk& c,
                                                                 int64_t i) { v[(size_t)o] = elem_i64(mm, c, i) != 0; }));
            arrow::BooleanBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(v));
            ARROW_RETURN_NOT_OK(b.Finish(&arr));
        } else if (cls == VClass::Int) {
            std::vector<int64_t> v((size_t)n);
            ARROW_RETURN_NOT_OK(visit(path, off, n, 0, 1, 1, [&](int64_t o, const ArrayMeta& mm, const Chunk& c,
                                                                 int64_t i) { v[(size_t)o] = elem_i64(mm, c, i); }));
            arrow::Int64Builder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(v));
            ARROW_RETURN_NOT_OK(b.Finish(&arr));
        } else {
            arrow::StringBuilder b;
            for (int64_t i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(b.Append("?"));
            ARROW_RETURN_NOT_OK(b.Finish(&arr));
        }
        return arr;
    }
    arrow::Status read_i64(const std::string& path, int64_t off, int64_t len, int64_t* out) const override {
        if (len <= 0) return arrow::Status::OK();
        return visit(path, off, len, 0, 1, 1, [&](int64_t o, const ArrayMeta& m, const Chunk& c, int64_t i) {
            out[o] = elem_i64(m, c, i);
        });
    }
    arrow::Status read_f64(const std::string& path, int64_t off, int64_t len, double* out) const override {
        if (len <= 0) return arrow::Status::OK();
        return visit(path, off, len, 0, 1, 1, [&](int64_t o, const ArrayMeta& m, const Chunk& c, int64_t i) {
            out[o] = elem_f64(m, c, i);
        });
    }
    arrow::Status read_block_i64(const std::string& path, int64_t r0, int64_t nr, int64_t c0, int64_t nc,
                                 int64_t* out) const override {
        if (nr <= 0 || nc <= 0) return arrow::Status::OK();
        return visit(path, r0, nr, c0, nc, 2, [&](int64_t o, const ArrayMeta& m, const Chunk& c, int64_t i) {
            out[o] = elem_i64(m, c, i);
        });
    }
    arrow::Status read_block_f64(const std::string& path, int64_t r0, int64_t nr, int64_t c0, int64_t nc,
                                 double* out) const override {
        if (nr <= 0 || nc <= 0) return arrow::Status::OK();
        return visit(path, r0, nr, c0, nc, 2, [&](int64_t o, const ArrayMeta& m, const Chunk& c, int64_t i) {
            out[o] = elem_f64(m, c, i);
        });
    }
    std::optional<StorageInfo> storage(const std::string& path) const override {
        auto m = meta(path);
        if (!m) return std::nullopt;
        StorageInfo si;
        si.dtype = m->dtype_label;
        si.n = 1;
        for (int64_t x : m->shape) si.n *= x;
        si.itemsize = (size_t)(m->itemsize ? m->itemsize : 8);
        si.chunked = !m->chunks.empty();
        si.chunk = m->chunks;
        si.chunk_elems = m->chunk_elems;
        si.filters = m->codecs_label.empty() ? "no compression" : m->codecs_label;
        si.stored = io_->bytes_under(prefix(path));
        return si;
    }
    std::vector<HierarchyRow> hierarchy() const override {
        std::vector<HierarchyRow> rows;
        std::function<void(const std::string&)> walk = [&](const std::string& path) {
            if (rows.size() > 1000000) return;
            HierarchyRow r;
            r.path = path;
            const JVal* a = attrs(path);
            r.n_attrs = a ? (int)a->keys.size() : 0;
            if (kind(path) == NodeKind::Array) {
                r.kind = "Dataset";
                if (auto ai = info(path)) {
                    std::vector<hsize_t> dims;
                    for (int64_t x : ai->shape) { dims.push_back((hsize_t)x); r.dims.push_back(x); }
                    std::string s;
                    for (size_t i = 0; i < dims.size(); ++i) s += (i ? " \xc3\x97 " : "") + std::to_string(dims[i]);
                    r.shape = s;
                    r.dtype = ai->dtype;
                }
                rows.push_back(std::move(r));
                return;
            }
            r.kind = "Group";
            rows.push_back(std::move(r));
            for (const auto& c : children(path)) walk((path == "/" ? "/" : path + "/") + c);
        };
        walk("/");
        return rows;
    }
    int64_t total_bytes() const override { return io_->total_bytes(); }
    std::string format_name() const override { return "Zarr v" + std::to_string(version_); }
    std::string read_why(const std::string& path) const override {
        std::string why;
        if (!meta(path, &why)) return why;
        return "a chunk cannot be decoded";
    }
};

// A v3 data_type: a name ("int32", "string") or an object for the
// extension types zarr-python writes (fixed-length strings, datetimes).
bool parse_v3_dtype(const JVal& dt, ArrayMeta* m) {
    std::string name = dt.t == JVal::Str ? dt.s : dt.str("name");
    const JVal* conf = dt.t == JVal::Obj ? dt.get("configuration") : nullptr;
    auto num = [&](const char* k) {
        const JVal* v = conf ? conf->get(k) : nullptr;
        return v && v->t == JVal::Num ? (int)std::clamp<int64_t>(to_i64(v->num, 0), 0, INT32_MAX) : 0;
    };
    m->dtype_label = name;
    if (name == "bool") { m->kind = 'b'; m->itemsize = 1; return true; }
    for (int bits : {8, 16, 32, 64}) {
        if (name == "int" + std::to_string(bits)) { m->kind = 'i'; m->itemsize = bits / 8; return true; }
        if (name == "uint" + std::to_string(bits)) { m->kind = 'u'; m->itemsize = bits / 8; return true; }
    }
    if (name == "float16" || name == "float32" || name == "float64") {
        m->kind = 'f';
        m->itemsize = std::atoi(name.c_str() + 5) / 8;
        return true;
    }
    if (name == "string" || name == "bytes" || name == "variable_length_utf8") {
        m->kind = 'O'; m->itemsize = 0; m->dtype_label = "string"; return true;
    }
    if (name == "fixed_length_utf32") {
        const int lb = num("length_bytes");
        if (lb <= 0 || lb % 4 || lb > (1 << 22)) return false;
        m->kind = 'U'; m->itemsize = lb; m->dtype_label = "string[" + std::to_string(lb / 4) + "]";
        return true;
    }
    if (name == "null_terminated_bytes" || name == "fixed_length_bytes" || name == "fixed_length_ascii") {
        const int lb = num("length_bytes");
        if (lb <= 0 || lb > (1 << 22)) return false;
        m->kind = 'S'; m->itemsize = lb; m->dtype_label = "string[" + std::to_string(lb) + "]";
        return true;
    }
    if (name == "numpy.datetime64" || name == "numpy.timedelta64") {
        m->kind = 'M'; m->itemsize = 8; return true;
    }
    return false;
}

// sharding_indexed: the outer chunk (shard) shape is the chunk grid; the
// inner chunks and their codecs come from the configuration.
std::string parse_sharding(const JVal& c, ArrayMeta* m) {
    const JVal* conf = c.get("configuration");
    if (!conf) return "sharding_indexed without a configuration";
    const std::vector<int64_t> inner = int_list(conf->get("chunk_shape"));
    if (inner.size() != m->chunks.size()) return "sharding_indexed: inner chunk rank differs";
    m->shard_ratio.clear();
    std::string shape;
    for (size_t d = 0; d < inner.size(); ++d) {
        if (inner[d] <= 0 || m->chunks[d] <= 0 || m->chunks[d] % inner[d] != 0)
            return "sharding_indexed: shards must hold whole inner chunks";
        m->shard_ratio.push_back(m->chunks[d] / inner[d]);
        shape += (d ? " \xc3\x97 " : "") + std::to_string(m->chunks[d]);
    }
    m->chunks = inner;
    m->sharded = true;
    m->index_at_end = conf->str("index_location", "end") != "start";
    if (const JVal* ic = conf->get("index_codecs"); ic && ic->t == JVal::Arr)
        for (const JVal& x : ic->a) {
            const std::string n = x.t == JVal::Str ? x.s : x.str("name");
            const JVal* xc = x.get("configuration");
            if (n == "bytes") m->index_big_endian = xc && xc->str("endian", "little") == "big";
            else if (n == "crc32c") m->index_crc32c = true;
            else return "sharding_indexed: unsupported index codec '" + n + "'";
        }
    // The inner codec chain, as for an unsharded array.
    const JVal* codecs = conf->get("codecs");
    if (!codecs || codecs->t != JVal::Arr) return "sharding_indexed without inner codecs";
    bool a2b = false;
    std::vector<std::string> labels;
    for (const JVal& x : codecs->a) {
        const std::string name = x.t == JVal::Str ? x.s : x.str("name");
        const JVal* xc = x.get("configuration");
        const JVal empty;
        if (!a2b) {
            if (name == "transpose") {
                std::vector<int64_t> ord = int_list(xc ? xc->get("order") : nullptr);
                bool identity = true;
                for (size_t k = 0; k < ord.size(); ++k) identity = identity && ord[k] == (int64_t)k;
                if (ord.size() == 2 && ord[0] == 1 && ord[1] == 0) m->fortran = !m->fortran;
                else if (!identity) m->unsupported = "unsupported transpose order";
                continue;
            }
            a2b = true;
            if (name == "bytes") m->big_endian = xc && xc->str("endian", "little") == "big";
            else if (name == "vlen-utf8" || name == "vlen-bytes") m->vlen = true;
            else m->unsupported = "unsupported inner codec '" + name + "' (nested sharding?)";
            continue;
        }
        m->bytes_codecs.push_back(Codec{name, xc ? *xc : empty});
        labels.push_back(codec_label(m->bytes_codecs.back()));
    }
    m->codecs_label = "sharded (shards of " + shape + ")";
    for (const auto& l : labels) m->codecs_label += " + " + l;
    return "";
}

std::string ZarrStore::parse_v3(const std::string& path, ArrayMeta* m) const {
    const JVal* z = json(prefix(path) + "zarr.json");
    if (!z || z->t != JVal::Obj) return "no readable zarr.json";
    if (z->str("node_type") != "array") return "not an array";
    m->shape = int_list(z->get("shape"));
    const JVal* dt = z->get("data_type");
    if (!dt || !parse_v3_dtype(*dt, m))
        return "unsupported data_type " + (dt && dt->t == JVal::Str ? dt->s : dt ? dt->str("name") : "");
    const JVal* grid = z->get("chunk_grid");
    if (!grid || grid->str("name") != "regular") return "unsupported chunk_grid";
    const JVal* gc = grid->get("configuration");
    m->chunks = int_list(gc ? gc->get("chunk_shape") : nullptr);
    if (const JVal* ke = z->get("chunk_key_encoding")) {
        const JVal* kc = ke->get("configuration");
        const std::string name = ke->str("name", "default");
        m->sep = kc ? kc->str("separator", name == "v2" ? "." : "/") : (name == "v2" ? "." : "/");
        if (m->sep != "." && m->sep != "/") m->sep = "/";
        m->key_prefix = name == "v2" ? "" : "c" + m->sep;
    } else {
        m->sep = "/";
        m->key_prefix = "c/";
    }
    if (const JVal* f = z->get("fill_value")) m->fill = *f;
    const JVal* codecs = z->get("codecs");
    if (!codecs || codecs->t != JVal::Arr) return "no codecs";
    bool a2b = false;
    std::vector<std::string> labels;
    for (const JVal& c : codecs->a) {
        const std::string name = c.t == JVal::Str ? c.s : c.str("name");
        const JVal* conf = c.get("configuration");
        const JVal empty;
        Codec codec{name, conf ? *conf : empty};
        if (!a2b) {
            if (name == "transpose") {
                // order [1, 0] on a 2-D array is Fortran order; the identity is
                // nothing; other permutations are not read.
                std::vector<int64_t> ord = int_list(conf ? conf->get("order") : nullptr);
                bool identity = true;
                for (size_t k = 0; k < ord.size(); ++k) identity = identity && ord[k] == (int64_t)k;
                if (ord.size() == 2 && ord[0] == 1 && ord[1] == 0) m->fortran = !m->fortran;
                else if (!identity) m->unsupported = "unsupported transpose order";
                labels.push_back("transpose");
                continue;
            }
            a2b = true;
            if (name == "bytes") {
                m->big_endian = conf && conf->str("endian", "little") == "big";
            } else if (name == "vlen-utf8" || name == "vlen-bytes") {
                m->vlen = true;
                labels.push_back(name);
            } else if (name == "sharding_indexed") {
                // A transpose outside the shards would permute the shard
                // layout itself; zarr-python puts it inside (inner codecs).
                if (m->fortran) return "a transpose outside sharding_indexed is not supported";
                if (auto e = parse_sharding(c, m); !e.empty()) return e;
                labels.push_back(m->codecs_label);
                m->codecs_label.clear();
            } else {
                m->unsupported = "unsupported array-to-bytes codec '" + name + "'";
            }
            continue;
        }
        // after sharding_indexed: codecs over the whole shard
        (m->sharded ? m->shard_codecs : m->bytes_codecs).push_back(codec);
        labels.push_back(codec_label(codec));
    }
    if (!a2b) return "no array-to-bytes codec";
    if (m->kind == 'O' && !m->vlen) m->unsupported = "a string array without a vlen-utf8 codec";
    for (const auto& l : labels) m->codecs_label += (m->codecs_label.empty() ? "" : " + ") + l;
    return finish_meta(m);
}

std::string strip_slash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

}  // namespace

// A directory holding a Zarr store: named *.zarr, or with root metadata.
bool is_zarr_dir(const std::string& path) {
    std::error_code ec;
    const std::string p = strip_slash(path);
    if (!std::filesystem::is_directory(p, ec)) return false;
    if (fends_ci(p, ".zarr")) return true;
    for (const char* m : {".zgroup", ".zarray", "zarr.json"})
        if (std::filesystem::is_regular_file(p + "/" + m, ec)) return true;
    return false;
}

std::string open_zarr_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    const std::string p = strip_slash(path);
    std::unique_ptr<ZarrIO> io;
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        io = std::make_unique<DirIO>(p);
    } else {
        auto z = std::make_unique<ZipIO>();
        if (auto e = z->open(p); !e.empty()) return "'" + path + "': " + e;
        io = std::move(z);
    }
    const bool v2 = io->exists(".zgroup") || io->exists(".zarray") || io->exists(".zattrs");
    const bool v3 = io->exists("zarr.json");
    if (!v2 && !v3)
        return "'" + path + "': not a Zarr store (no .zgroup / .zarray / zarr.json at its root)";
    auto store = std::make_shared<ZarrStore>(std::move(io), v3 ? 3 : 2);
    return open_store_source(std::move(store), path, out, anndata_df_row_cap(cfg), cfg.matrix == "long");
}

#ifdef VV_FUZZ
namespace {
// A store held in memory: the fuzz input is a run of records
// [u16 key length][key][u32 value length][value].
class MemIO : public ZarrIO {
    std::map<std::string, std::string> kv_;
    std::set<std::string> dirs_;
public:
    void put(std::string k, std::string v) {
        for (size_t sl = k.find('/'); sl != std::string::npos; sl = k.find('/', sl + 1)) dirs_.insert(k.substr(0, sl + 1));
        kv_[std::move(k)] = std::move(v);
    }
    bool read(const std::string& key, std::string* out) const override {
        auto it = kv_.find(key);
        if (it == kv_.end()) return false;
        *out = it->second;
        return true;
    }
    int64_t size(const std::string& key) const override {
        auto it = kv_.find(key);
        return it == kv_.end() ? -1 : (int64_t)it->second.size();
    }
    bool exists(const std::string& key) const override { return kv_.count(key) > 0; }
    std::vector<std::string> list(const std::string& prefix) const override {
        std::set<std::string> names;
        for (auto it = kv_.lower_bound(prefix); it != kv_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
            const std::string rest = it->first.substr(prefix.size());
            names.insert(rest.substr(0, rest.find('/')));
        }
        return {names.begin(), names.end()};
    }
    bool is_dir(const std::string& prefix) const override { return prefix.empty() || dirs_.count(prefix) > 0; }
    uint64_t bytes_under(const std::string& prefix) const override {
        uint64_t n = 0;
        for (auto it = kv_.lower_bound(prefix); it != kv_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
            n += it->second.size();
        return n;
    }
    int64_t total_bytes() const override { return (int64_t)bytes_under(""); }
};
}  // namespace

// Build a store from an untrusted buffer, read every array through the Store
// interface and every tab through the AnnData / generic tab code; abort when a
// built table is not valid.
void zarr_fuzz_one(const uint8_t* buf, size_t n) {
    auto io = std::make_unique<MemIO>();
    size_t at = 0;
    while (at + 2 <= n) {
        const size_t kl = (size_t)buf[at] | ((size_t)buf[at + 1] << 8);
        at += 2;
        if (at + kl + 4 > n) break;
        std::string key((const char*)buf + at, kl);
        at += kl;
        const size_t vl = (size_t)buf[at] | ((size_t)buf[at + 1] << 8) | ((size_t)buf[at + 2] << 16) |
                          ((size_t)buf[at + 3] << 24);
        at += 4;
        if (vl > n - at) break;
        io->put(std::move(key), std::string((const char*)buf + at, vl));
        at += vl;
    }
    const int version = io->exists("zarr.json") ? 3 : 2;
    auto store = std::make_shared<ZarrStore>(std::move(io), version);
    int nodes = 0;
    for (const HierarchyRow& r : store->hierarchy()) {
        if (++nodes > 200) break;
        if (r.kind != "Dataset") continue;
        auto ai = store->info(r.path);
        (void)store->storage(r.path);
        if (!ai) continue;
        if (ai->shape.size() <= 1) {
            const int64_t len = ai->shape.empty() ? 1 : std::min<int64_t>(ai->shape[0], 4096);
            auto col = store->read_column(r.path, 0, len);
            if (col.ok()) {
                auto st = (*col)->ValidateFull();
                if (!st.ok()) { std::fprintf(stderr, "invalid column %s: %s\n", r.path.c_str(), st.ToString().c_str()); std::abort(); }
            }
            std::vector<double> d((size_t)std::max<int64_t>(len, 0));
            (void)store->read_f64(r.path, 0, ai->shape.empty() ? 0 : len, d.data());
        } else if (ai->shape.size() == 2) {
            const int64_t nr = std::min<int64_t>(ai->shape[0], 64), nc = std::min<int64_t>(ai->shape[1], 64);
            std::vector<double> d((size_t)(nr * nc));
            (void)store->read_block_f64(r.path, 0, nr, 0, nc, d.data());
        }
    }
    std::unique_ptr<TabularSource> first;
    if (!open_store_source(store, "fuzz.zarr", &first, kDataFrameRowCap, false).empty()) return;
    std::vector<std::unique_ptr<TabularSource>> tabs = first->expand_tabs();
    tabs.insert(tabs.begin(), std::move(first));
    for (auto& t : tabs) {
        std::vector<int> cols(t->schema()->num_fields());
        for (size_t k = 0; k < cols.size(); ++k) cols[k] = (int)k;
        for (int c = 0; c < 4; ++c) {
            t->ensure(c);
            if (c >= t->num_chunks()) break;
            std::shared_ptr<arrow::Table> tbl;
            if (!t->read_chunk(c, cols, &tbl).ok()) break;
            auto st = tbl->ValidateFull();
            if (!st.ok()) { std::fprintf(stderr, "invalid tab %s: %s\n", t->tab_label().c_str(), st.ToString().c_str()); std::abort(); }
        }
    }
}
#endif

}  // namespace h5v
