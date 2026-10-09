// vvjson.hpp — JSON documents in libvvcore: a strict streaming lexer and the
// writers behind `vv x.json` on a pipe (pretty print) and --json-paths.
//
// The lexer is iterative (an explicit container stack, never recursion), keeps
// numbers as the bytes the file holds, and passes strings through in pieces,
// so memory does not depend on nesting depth or value size. A file may hold
// several top-level values (NDJSON, concatenated JSON).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <functional>
#include <regex>
#include <ctime>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace arrow { namespace io { class InputStream; } }

namespace vvjson {

// The first error in a document: byte offset (from the start of the decoded
// stream), 1-based line and column, and what was expected. offset < 0 = none.
struct JsonError {
    int64_t     offset = -1;
    int64_t     line   = 0;
    int64_t     col    = 0;
    std::string msg;
    bool ok() const { return offset < 0; }
    // "invalid JSON at byte N (line L, column C): msg"
    std::string describe() const;
};

enum class JsonOut {
    Pretty,   // re-indented, 2 spaces; scalars as written in the file
    Paths,    // one `path = value` line per leaf (and per empty container)
};

// Read every top-level value from `in` and write it to `out`. `lines`: the
// input is NDJSON / JSON Lines, so --json-paths prefixes each record's paths
// with `.[i]`. `color`: colour keys / strings / numbers / literals with the
// active theme. Everything before an error is written; the error is returned.
JsonError write_json_document(arrow::io::InputStream& in, std::FILE* out,
                              JsonOut mode, bool lines, bool color);

// Strict validation of a whole stream (no output): the first error, or ok().
JsonError lex_validate(arrow::io::InputStream& in);

// The tree viewers' search pattern: a case-insensitive substring, or an
// ECMAScript regex (case-insensitive) when the query uses regex syntax and
// compiles; a query that does not compile is taken literally.
struct JsonSearch {
    std::string lit;
    std::regex  re;
    bool        is_re = false;
    void compile(const std::string& q) {
        is_re = q.find_first_of(".^$|?*+()[]{}\\") != std::string::npos;
        lit.clear();
        if (is_re) {
            try { re = std::regex(q, std::regex::ECMAScript | std::regex::icase); }
            catch (...) { is_re = false; }
        }
        if (!is_re) for (char c : q) lit += (char)std::tolower((unsigned char)c);
    }
    bool match(std::string_view s) const {
        if (is_re) return std::regex_search(s.begin(), s.end(), re);
        if (lit.empty() || s.size() < lit.size()) return false;
        for (size_t i = 0; i + lit.size() <= s.size(); ++i) {
            size_t k = 0;
            while (k < lit.size() && std::tolower((unsigned char)s[i + k]) == lit[k]) ++k;
            if (k == lit.size()) return true;
        }
        return false;
    }
};

// ── The tree viewers' document: a lazy index over a mapped file ─────────────
//
// A container's children are found by scanning forward from a checkpoint
// kept every K children (VV_JSON_CHECKPOINT, default 1024) and cached a page
// of K at a time (an LRU of 64 pages), so a multi-GB document opens after one
// pass over its top level and memory stays bounded. The scan is lenient
// (brackets must match); a background thread validates the whole document
// with the strict lexer. Used by the terminal tree (vv) and vvg's tree tab.

enum class JKind : uint8_t { Object, Array, String, Number, True, False, Null, Error };

struct JNode {
    uint64_t off = 0;                // first byte (the error position for Error)
    uint64_t end = 0;                // one past the value
    uint64_t key_off = UINT64_MAX;   // object member: the key's opening quote
    int64_t  count = 0;              // containers: direct children
    JKind    kind = JKind::Null;
    bool     broken = false;         // the value's bytes end in an error
    bool     virt = false;           // the virtual root of a sequence / NDJSON
    bool     container() const { return kind == JKind::Object || kind == JKind::Array; }
    uint64_t start() const { return key_off != UINT64_MAX ? key_off : off; }
};

// A mapped file that shrinks while it is open (rewritten by another
// program) makes every read past its new end a SIGBUS. Mappings register
// here; the handler maps a zero-filled page over a faulting page inside one
// of them and notes it, so the read sees NUL bytes (which the scanners treat
// as an error) instead of the process dying. A fault anywhere else gets the
// default action.
struct MapGuardSlot {
    std::atomic<uintptr_t> lo{0}, hi{0};
    std::atomic<int>       hit{0};
};
inline MapGuardSlot g_map_guard[16];
inline long         g_map_guard_page = 4096;

#ifdef _WIN32
// Windows refuses to truncate a file while it is mapped, so there is nothing
// to guard. map_file / unmap_file (src/core/platform.cpp) map a whole file
// read-only; an empty file gives no view and size 0.
inline int  map_guard_add(const void*, size_t) { return -1; }
inline void map_guard_remove(int) {}
std::string map_file(const std::string& path, void** view, size_t* size);
void        unmap_file(void* view);
#else

inline void map_guard_handler(int, siginfo_t* si, void*) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
    for (MapGuardSlot& sl : g_map_guard) {
        const uintptr_t lo = sl.lo.load(), hi = sl.hi.load();
        if (lo == 0 || a < lo || a >= hi) continue;
        void* page = reinterpret_cast<void*>(a & ~static_cast<uintptr_t>(g_map_guard_page - 1));
        if (mmap(page, (size_t)g_map_guard_page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                 -1, 0) != MAP_FAILED) {
            sl.hit.store(1);
            return;                                   // the read is retried, now on zeros
        }
        break;
    }
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGBUS, &dfl, nullptr);                 // not ours: re-fault with the default
}

// Register [p, p + n); the slot index, or -1 when all slots are taken.
inline int map_guard_add(const void* p, size_t n) {
    static std::once_flag once;
    std::call_once(once, [] {
        g_map_guard_page = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
        struct sigaction sa {};
        sa.sa_sigaction = map_guard_handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGBUS, &sa, nullptr);
    });
    for (int i = 0; i < 16; ++i) {
        uintptr_t expect = 0;
        if (g_map_guard[i].lo.compare_exchange_strong(expect, reinterpret_cast<uintptr_t>(p))) {
            g_map_guard[i].hit.store(0);
            g_map_guard[i].hi.store(reinterpret_cast<uintptr_t>(p) + n);
            return i;
        }
    }
    return -1;
}
inline void map_guard_remove(int i) {
    if (i < 0) return;
    g_map_guard[i].hi.store(0);
    g_map_guard[i].lo.store(0);
}
#endif  // _WIN32

class JsonDoc {
public:
    enum class Root { Value, Sequence, Lines };

    JsonDoc() = default;
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;
    ~JsonDoc() {
        stop_ = true;
        if (validator_.joinable()) validator_.join();
        map_guard_remove(guard_);
#ifdef _WIN32
        if (map_) unmap_file(map_);
#else
        if (map_) munmap(map_, size_);
#endif
    }

    // Map `path` (a regular, uncompressed file). `lines`: NDJSON / JSON Lines.
    std::string open(const std::string& path, bool lines, bool validate = true) {
        path_ = path;
#ifdef _WIN32
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return "Cannot open '" + path + "': " + std::strerror(errno);
        mtime_ = st.st_mtime;
        if (auto e = map_file(path, &map_, &size_); !e.empty()) return e;
        data_ = static_cast<const char*>(map_);
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return "Cannot open '" + path + "': " + std::strerror(errno);
        struct stat st;
        if (fstat(fd, &st) != 0) { ::close(fd); return "Cannot stat '" + path + "'"; }
        size_ = (size_t)st.st_size;
        mtime_ = st.st_mtime;
        if (size_ > 0) {
            void* m = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m == MAP_FAILED) {
                ::close(fd);
                return "Cannot map '" + path + "': " + std::strerror(errno);
            }
            map_ = m;
            data_ = static_cast<const char*>(m);
            guard_ = map_guard_add(m, size_);
        }
        ::close(fd);
#endif
        init(lines, validate);
        return "";
    }
    // An in-memory buffer (fuzzing). The buffer must outlive the document.
    void open_buffer(const char* data, size_t n, bool lines, bool validate) {
        data_ = data;
        size_ = n;
        init(lines, validate);
    }

    const std::string& path() const { return path_; }
    size_t size() const { return size_; }
    const char* data() const { return data_; }
    Root root_mode() const { return mode_; }
    const JNode& root() const { return root_; }
    int64_t page_size() const { return K_; }

    // A node's raw bytes.
    std::string_view bytes(const JNode& n) const {
        if (n.off >= size_ || n.end <= n.off) return {};
        return std::string_view(data_ + n.off, (size_t)(std::min<uint64_t>(n.end, size_) - n.off));
    }
    // An object member's key, as written between its quotes.
    std::string_view key(const JNode& n) const {
        if (n.key_off == UINT64_MAX || n.key_off >= size_) return {};
        const uint64_t e = skip_string(n.key_off, size_);
        if (e == UINT64_MAX || e < n.key_off + 2) return {};
        return std::string_view(data_ + n.key_off + 1, (size_t)(e - n.key_off - 2));
    }

    // Child i of container `parent`; false past the end (or after an error).
    bool child(const JNode& parent, int64_t i, JNode* out) {
        if (!parent.container() || i < 0) return false;
        Index& ix = index_of(parent);
        const int64_t page = i / K_;
        if (!ensure_page(parent, ix, page)) return false;
        const std::vector<JNode>& nodes = page_nodes(parent, ix, page);
        const int64_t k = i - page * K_;
        if (k >= (int64_t)nodes.size()) return false;
        *out = nodes[(size_t)k];
        return true;
    }

    // Path from the root to the deepest node whose bytes (key included)
    // contain `target`: (node, index in its parent); the root's index is -1.
    std::vector<std::pair<JNode, int64_t>> path_to(uint64_t target) {
        std::vector<std::pair<JNode, int64_t>> p;
        p.push_back({root_, -1});
        for (;;) {
            const JNode cur = p.back().first;
            if (!cur.container() || cur.count == 0) break;
            int64_t lo = 0, hi = cur.count - 1, found = -1;
            JNode fn;
            while (lo <= hi) {
                const int64_t mid = lo + (hi - lo) / 2;
                JNode c;
                if (!child(cur, mid, &c)) { hi = mid - 1; continue; }
                if (c.start() <= target) { found = mid; fn = c; lo = mid + 1; }
                else hi = mid - 1;
            }
            if (found < 0 || target >= std::max(fn.end, fn.off + 1)) break;
            p.push_back({fn, found});
        }
        return p;
    }

    // A one-line compact rendering of a node's bytes (whitespace outside
    // strings dropped, ", " and ": " spaced), cut at `max_bytes` of output.
    std::string compact(const JNode& n, size_t max_bytes) const {
        std::string out;
        uint64_t i = n.off;
        const uint64_t e = std::min<uint64_t>(n.end, size_);
        bool in_str = false, esc = false;
        while (i < e && out.size() < max_bytes) {
            const char c = data_[i++];
            if (in_str) {
                out += c;
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
            } else if (c == '"') { in_str = true; out += c; }
            else if (ws(c)) continue;
            else { out += c; if (c == ',' || c == ':') out += ' '; }
        }
        return out;
    }

    // The file was rewritten since it was mapped (size or modification time
    // differ); `zeroed`: a read past its new end was answered with zeros.
    bool file_changed(bool* zeroed = nullptr) const {
        if (zeroed) *zeroed = guard_ >= 0 && g_map_guard[guard_].hit.load() != 0;
        if (path_.empty() || !map_) return false;
        struct stat st;
        if (::stat(path_.c_str(), &st) != 0) return true;
        return (size_t)st.st_size != size_ || st.st_mtime != mtime_;
    }

    // Search the bytes for a key, string (matched without its quotes) or
    // scalar matching `pat`. Forward: from `from` to the end, then wrapping
    // from the start; backward: the last hit before `before`, then the last
    // in the rest. The hit's offset (where its token starts), or UINT64_MAX.
    // `tick(pos)` is called every 4 MiB scanned; returning false cancels.
    // Reads only the mapping, so it may run on another thread while the
    // index is used (not while the document is destroyed).
    uint64_t search(const JsonSearch& pat, bool forward, uint64_t from, uint64_t before,
                    const std::function<bool(uint64_t)>& tick, bool* wrapped,
                    bool* cancelled) const {
        *wrapped = *cancelled = false;
        uint64_t hit;
        if (forward) {
            hit = scan(pat, std::min<uint64_t>(from, size_), size_, true, tick, cancelled);
            if (hit == UINT64_MAX && !*cancelled) {
                hit = scan(pat, 0, std::min<uint64_t>(from, size_), true, tick, cancelled);
                *wrapped = true;
            }
        } else {
            hit = scan(pat, 0, std::min<uint64_t>(before, size_), false, tick, cancelled);
            if (hit == UINT64_MAX && !*cancelled) {
                hit = scan(pat, std::min<uint64_t>(before, size_), size_, false, tick, cancelled);
                *wrapped = true;
            }
        }
        return *cancelled ? UINT64_MAX : hit;
    }
    // Where a search continues from a node: after a scalar (or at it, with
    // `include`), inside a container after its opening bracket.
    static uint64_t search_from(const JNode& n, bool include) {
        if (n.virt) return 0;
        if (n.container()) return n.off + 1;
        return include ? n.start() : n.end;
    }

    // The first structural error (where the scanner gave up), if any.
    bool has_struct_err() const { return !struct_err_.empty(); }
    uint64_t struct_err_off() const { return struct_err_off_; }
    const std::string& struct_err() const { return struct_err_; }

    // Background validation: 0 running, 1 valid, 2 invalid.
    int validation() const { return valid_state_.load(); }
    int64_t validated_bytes() const { return validated_.load(); }
    JsonError validation_error() const {
        std::lock_guard<std::mutex> g(err_mu_);
        return valid_err_;
    }
    // Wait for the validator (tests, the fuzzer).
    void wait_validation() { if (validator_.joinable()) validator_.join(); }

private:
    struct Index {
        std::vector<uint64_t> ckpt;     // start of child k*K (an object member's key)
        uint64_t scan_pos = 0;          // start of the next unscanned child
        int64_t  scanned = 0;           // children scanned so far
        bool     complete = false;
    };
    std::string           path_;
    time_t                mtime_ = 0;
    int                   guard_ = -1;      // map_guard slot
    void*                 map_ = nullptr;
    const char*           data_ = nullptr;
    size_t                size_ = 0;
    Root                  mode_ = Root::Value;
    JNode                 root_;
    int64_t               K_ = 1024;
    std::vector<uint64_t> top_, top_end_;   // top-level values (Sequence / Lines)
    std::unordered_map<uint64_t, Index> index_;
    static constexpr size_t kMaxPages = 64;
    using PageKey = std::pair<uint64_t, int64_t>;
    std::list<std::pair<PageKey, std::vector<JNode>>> pages_;   // most recent first
    std::map<PageKey, decltype(pages_)::iterator> page_at_;
    std::string           struct_err_;
    uint64_t              struct_err_off_ = 0;
    std::thread           validator_;
    std::atomic<bool>     stop_{false};
    std::atomic<int>      valid_state_{0};
    std::atomic<int64_t>  validated_{0};
    mutable std::mutex    err_mu_;
    JsonError             valid_err_;

    static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    // One direction of search(): the first (forward) or last (backward) hit
    // whose token starts in [a, b).
    uint64_t scan(const JsonSearch& pat, uint64_t a, uint64_t b, bool forward,
                  const std::function<bool(uint64_t)>& tick, bool* cancelled) const {
        const char* d = data_;
        uint64_t i = a, last = UINT64_MAX, next_tick = a + (4u << 20);
        while (i < b) {
            if (i >= next_tick) {
                next_tick = i + (4u << 20);
                if (tick && !tick(i)) { *cancelled = true; return UINT64_MAX; }
            }
            const char c = d[i];
            if (c == '"') {
                const uint64_t e0 = skip_string(i, size_);
                const uint64_t e = e0 == UINT64_MAX ? size_ : e0 - 1;   // the closing quote
                if (pat.match(std::string_view(d + i + 1, (size_t)(e - i - 1)))) {
                    if (forward) return i;
                    last = i;
                }
                i = e + 1;
                continue;
            }
            if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
                uint64_t e = i;
                while (e < b && !std::strchr(" \t\r\n,:[]{}\"", d[e])) ++e;
                if (pat.match(std::string_view(d + i, (size_t)(e - i)))) {
                    if (forward) return i;
                    last = i;
                }
                i = std::max(e, i + 1);
                continue;
            }
            ++i;
        }
        return last;
    }
    uint64_t skip_ws(uint64_t i, uint64_t lim) const {
        while (i < lim && ws(data_[i])) ++i;
        return i;
    }
    // Past a string that opens at `i`; UINT64_MAX when it is not closed
    // before `lim`.
    uint64_t skip_string(uint64_t i, uint64_t lim) const {
        uint64_t k = i + 1;
        while (k < lim) {
            const void* q = std::memchr(data_ + k, '"', (size_t)(lim - k));
            if (!q) return UINT64_MAX;
            const uint64_t at = (uint64_t)(static_cast<const char*>(q) - data_);
            uint64_t j = at;
            while (j > i + 1 && data_[j - 1] == '\\') --j;
            if ((at - j) % 2 == 0) return at + 1;
            k = at + 1;
        }
        return UINT64_MAX;
    }
    void note_err(uint64_t off, const char* msg) {
        if (struct_err_.empty() || off < struct_err_off_) {
            struct_err_ = msg;
            struct_err_off_ = off;
        }
    }
    static JKind scalar_kind(char c) {
        if (c == '"') return JKind::String;
        if (c == 't') return JKind::True;
        if (c == 'f') return JKind::False;
        if (c == 'n') return JKind::Null;
        if (c == '-' || (c >= '0' && c <= '9')) return JKind::Number;
        return JKind::Error;
    }
    JNode error_at(uint64_t off, const char* msg) {
        JNode n;
        n.kind = JKind::Error;
        n.off = n.end = off;
        note_err(off, msg);
        return n;
    }
    // The value at `i` (bytes before `lim`): end, kind, and for containers
    // the number of direct children. Lenient: brackets must match, nothing
    // else is checked. A structural break marks the node broken and ends it
    // there.
    JNode scan_value(uint64_t i, uint64_t lim) {
        if (i >= lim) return error_at(i, "unexpected end of input");
        JNode n;
        n.off = i;
        const char c = data_[i];
        if (c == '{' || c == '[') {
            n.kind = c == '{' ? JKind::Object : JKind::Array;
            std::vector<char> st(1, c == '{' ? '}' : ']');
            uint64_t k = i + 1;
            bool any = false;
            int64_t commas = 0;
            while (k < lim && !st.empty()) {
                const char ch = data_[k];
                if (ch == '"') {
                    if (st.size() == 1) any = true;
                    k = skip_string(k, lim);
                    if (k == UINT64_MAX) break;
                    continue;
                }
                if (ch == '{' || ch == '[') {
                    if (st.size() == 1) any = true;
                    st.push_back(ch == '{' ? '}' : ']');
                } else if (ch == '}' || ch == ']') {
                    if (ch != st.back()) {
                        n.count = commas + (any ? 1 : 0);
                        n.broken = true;
                        n.end = k;
                        note_err(k, "mismatched bracket");
                        return n;
                    }
                    st.pop_back();
                } else if (ch == ',') {
                    if (st.size() == 1) ++commas;
                } else if (!ws(ch) && st.size() == 1) {
                    any = true;
                }
                ++k;
            }
            n.count = commas + (any ? 1 : 0);
            if (!st.empty()) {
                n.broken = true;
                n.end = lim;
                note_err(lim, k == UINT64_MAX ? "unterminated string" : "unexpected end of input");
                return n;
            }
            n.end = k;
            return n;
        }
        n.kind = scalar_kind(c);
        if (n.kind == JKind::Error) return error_at(i, "expected a value");
        if (n.kind == JKind::String) {
            const uint64_t e = skip_string(i, lim);
            if (e == UINT64_MAX) { n.broken = true; n.end = lim; note_err(lim, "unterminated string"); }
            else n.end = e;
            return n;
        }
        uint64_t k = i + 1;            // a number or literal ends at any structural byte
        while (k < lim && !ws(data_[k]) && !std::strchr(",:[]{}\"", data_[k])) ++k;
        n.end = k;
        return n;
    }

    void init(bool lines, bool validate) {
        if (const char* e = std::getenv("VV_JSON_CHECKPOINT")) {
            const long v = std::atol(e);
            if (v >= 2 && v <= (1L << 20)) K_ = v;
        }
        uint64_t i = 0;
        if (size_ >= 3 && std::memcmp(data_, "\xEF\xBB\xBF", 3) == 0) i = 3;
        i = skip_ws(i, size_);
        if (lines) {
            mode_ = Root::Lines;
            uint64_t p = i;
            while (p < size_) {
                p = skip_ws(p, size_);
                if (p >= size_) break;
                const void* nl = std::memchr(data_ + p, '\n', size_ - p);
                const uint64_t e = nl ? (uint64_t)(static_cast<const char*>(nl) - data_) : size_;
                top_.push_back(p);
                top_end_.push_back(e);
                p = e + 1;
            }
        } else {
            uint64_t p = i;
            while (p < size_) {
                const JNode v = scan_value(p, size_);
                top_.push_back(p);
                top_end_.push_back(size_);
                if (v.kind == JKind::Error || v.broken) break;
                p = skip_ws(v.end, size_);
            }
            mode_ = top_.size() > 1 ? Root::Sequence : Root::Value;
        }
        if (mode_ == Root::Value) {
            if (top_.empty()) root_ = error_at(0, "no JSON value (empty input)");
            else root_ = scan_value(top_[0], size_);
            if (root_.kind == JKind::Error && size_ == 0) struct_err_.clear();
        } else {
            root_.kind = JKind::Array;
            root_.virt = true;
            root_.off = 0;
            root_.end = size_;
            root_.count = (int64_t)top_.size();
        }
        if (validate) start_validation();
        else valid_state_ = struct_err_.empty() ? 1 : 2;
    }

    // Validate the mapping on a background thread with the strict lexer
    // (defined in src/formats/json.cpp, with the lexer).
    void start_validation();

    static uint64_t key_of(const JNode& n) { return n.virt ? UINT64_MAX : n.off; }

    Index& index_of(const JNode& parent) {
        const uint64_t k = key_of(parent);
        auto it = index_.find(k);
        if (it != index_.end()) return it->second;
        Index ix;
        if (parent.virt) {
            for (size_t j = 0; j < top_.size(); j += (size_t)K_) ix.ckpt.push_back(j);   // indexes
            ix.scanned = (int64_t)top_.size();
            ix.complete = true;
        } else {
            const uint64_t lim = std::min<uint64_t>(parent.end, size_);
            ix.scan_pos = skip_ws(parent.off + 1, lim);
            const char close = parent.kind == JKind::Object ? '}' : ']';
            if (parent.count == 0 || ix.scan_pos >= lim || data_[ix.scan_pos] == close)
                ix.complete = true;
            else ix.ckpt.push_back(ix.scan_pos);
        }
        return index_.emplace(k, std::move(ix)).first->second;
    }

    // One member at `pos` of a non-virtual container: the node, and where
    // the next member starts (UINT64_MAX after the last one or an error).
    JNode read_member(uint64_t pos, const JNode& parent, uint64_t* next) {
        *next = UINT64_MAX;
        const uint64_t lim = std::min<uint64_t>(parent.end, size_);
        uint64_t p = pos;
        JNode c;
        if (parent.kind == JKind::Object) {
            if (p >= lim || data_[p] != '"')
                return error_at(p, p >= lim ? "unexpected end of input" : "expected a string key");
            const uint64_t key = p;
            p = skip_string(p, lim);
            if (p == UINT64_MAX) return error_at(lim, "unterminated string");
            p = skip_ws(p, lim);
            if (p >= lim || data_[p] != ':')
                return error_at(p, p >= lim ? "unexpected end of input" : "expected ':' after a key");
            p = skip_ws(p + 1, lim);
            c = scan_value(p, lim);
            c.key_off = key;
        } else {
            c = scan_value(p, lim);
        }
        if (c.kind == JKind::Error || c.broken) return c;
        p = skip_ws(c.end, lim);
        if (p < lim && data_[p] == ',') *next = skip_ws(p + 1, lim);
        return c;
    }

    // Checkpoints up to `page`, scanning forward one child at a time.
    bool ensure_page(const JNode& parent, Index& ix, int64_t page) {
        while ((int64_t)ix.ckpt.size() <= page && !ix.complete) {
            uint64_t next;
            const JNode c = read_member(ix.scan_pos, parent, &next);
            ++ix.scanned;
            if (c.kind == JKind::Error || c.broken || next == UINT64_MAX) { ix.complete = true; break; }
            ix.scan_pos = next;
            if (ix.scanned % K_ == 0) ix.ckpt.push_back(next);
        }
        return page < (int64_t)ix.ckpt.size();
    }

    const std::vector<JNode>& page_nodes(const JNode& parent, Index& ix, int64_t page) {
        const PageKey key(key_of(parent), page);
        auto it = page_at_.find(key);
        if (it != page_at_.end()) {
            pages_.splice(pages_.begin(), pages_, it->second);
            return it->second->second;
        }
        std::vector<JNode> nodes;
        if (parent.virt) {
            const size_t a = (size_t)ix.ckpt[(size_t)page];
            const size_t b = std::min(top_.size(), a + (size_t)K_);
            for (size_t j = a; j < b; ++j) {
                JNode v = scan_value(top_[j], top_end_[j]);
                if (mode_ == Root::Lines && v.kind != JKind::Error && !v.broken) {
                    const uint64_t p = skip_ws(v.end, top_end_[j]);
                    if (p < top_end_[j]) { v.broken = true; note_err(p, "unexpected text after the value"); }
                }
                nodes.push_back(v);
            }
        } else {
            uint64_t pos = ix.ckpt[(size_t)page];
            for (int64_t k = 0; k < K_ && pos != UINT64_MAX; ++k) {
                uint64_t next;
                const JNode c = read_member(pos, parent, &next);
                nodes.push_back(c);
                if (c.kind == JKind::Error || c.broken) break;
                pos = next;
            }
        }
        pages_.emplace_front(key, std::move(nodes));
        page_at_[key] = pages_.begin();
        while (pages_.size() > kMaxPages) {
            page_at_.erase(pages_.back().first);
            pages_.pop_back();
        }
        return pages_.front().second;
    }
};

}  // namespace vvjson
