// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// JSON: records as a table, the document lexer / writers, the tree index.

#include "internal.hpp"

class JsonArrayUnwrapStream : public arrow::io::InputStream {
    std::shared_ptr<arrow::io::InputStream> inner_;
    std::string  out_buf_;
    size_t       out_pos_    = 0;
    bool         inner_done_ = false;
    // First non-space byte decides the shape: `[` is an array to unwrap, any
    // other byte is a passthrough stream. Until then the mode is UNKNOWN.
    enum Mode { UNKNOWN, ARRAY, PASSTHROUGH } mode_ = UNKNOWN;
    int   depth_      = 0;      // brace/bracket nesting inside the outer array
    bool  in_string_  = false;
    bool  escaped_    = false;
    bool  array_done_ = false;  // the outer `]` was consumed

    static constexpr int64_t BLK = 64 * 1024;

    void feed(const uint8_t* p, int64_t n) {
        for (int64_t i = 0; i < n; ++i) {
            char c = (char)p[i];
            if (mode_ == UNKNOWN) {
                if (c==' '||c=='\t'||c=='\r'||c=='\n') { out_buf_.push_back(c); continue; }
                if (c=='[') { mode_ = ARRAY; depth_ = 0; continue; }  // drop outer [
                mode_ = PASSTHROUGH; out_buf_.push_back(c); continue;
            }
            if (mode_ == PASSTHROUGH) { out_buf_.push_back(c); continue; }
            // ARRAY mode.
            if (array_done_) continue;              // trailing bytes after outer ]
            if (in_string_) {
                out_buf_.push_back(c);
                if (escaped_)        escaped_ = false;
                else if (c=='\\')    escaped_ = true;
                else if (c=='"')     in_string_ = false;
                continue;
            }
            switch (c) {
                case '"': in_string_ = true; out_buf_.push_back(c); break;
                case '{': case '[': ++depth_; out_buf_.push_back(c); break;
                case '}': if (depth_ > 0) --depth_; out_buf_.push_back(c); break;
                case ']':
                    if (depth_ == 0) array_done_ = true;   // outer close → drop
                    else { --depth_; out_buf_.push_back(c); }
                    break;
                case ',':
                    if (depth_ == 0) out_buf_.push_back('\n');  // record separator
                    else out_buf_.push_back(c);
                    break;
                default: out_buf_.push_back(c); break;
            }
        }
    }

    bool refill() {
        out_buf_.clear(); out_pos_ = 0;
        while (out_buf_.empty() && !inner_done_) {
            auto r = inner_->Read(BLK);
            if (!r.ok()) { inner_done_ = true; return false; }
            auto buf = *r;
            if (!buf || buf->size() == 0) { inner_done_ = true; break; }
            feed(buf->data(), buf->size());
        }
        return !out_buf_.empty();
    }

public:
    explicit JsonArrayUnwrapStream(std::shared_ptr<arrow::io::InputStream> inner)
        : inner_(std::move(inner)) {}

    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("JsonArrayUnwrapStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* buf) override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        int64_t total = 0;
        while (n > 0) {
            if (out_pos_ >= out_buf_.size()) {
                if (!refill()) break;
            }
            int64_t avail = (int64_t)(out_buf_.size() - out_pos_);
            int64_t take  = std::min(n, avail);
            std::memcpy(p, out_buf_.data() + out_pos_, (size_t)take);
            p += take; out_pos_ += (size_t)take; n -= take; total += take;
        }
        return total;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(n));
        ARROW_ASSIGN_OR_RAISE(int64_t actual, Read(n, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(actual, false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }
};
// ── JSON / NDJSON source ──────────────────────────────────────────────────────
//
// Reads newline-delimited JSON (`.ndjson` / `.jsonl`, one object per line) and
// ordinary JSON (`.json`) through Arrow's streaming JSON reader. A top-level
// array is unwrapped into records by JsonArrayUnwrapStream first; NDJSON and
// concatenated / pretty-printed objects pass through it untouched. Records need
// not share a schema — Arrow infers the union of fields and fills the gaps with
// nulls (`unexpected_field_behavior = InferType`). Nested objects and arrays
// become struct / list columns, rendered by the same cell formatters as Parquet.
//
// Streaming and forward-only, modelled on FastxSource: each block Arrow yields
// is one chunk, retained through the shared bounded-window helper so a file
// larger than memory still previews. Compression (gzip / zstd) is detected by
// magic, like the plain-text reader. Range queries and tabix do not apply.
class JsonSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    StreamCodec                            comp_ = StreamCodec::None;

    mutable std::shared_ptr<arrow::json::StreamingReader> reader_;
    std::shared_ptr<ReadGate>             gate_ = std::make_shared<ReadGate>();
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>           batch_first_row_;
    mutable std::vector<int64_t>           batch_num_rows_;
    mutable int64_t                        rows_so_far_ = 0;
    mutable bool                           all_read_    = false;
    mutable bool                           retain_all_  = false;
    mutable bool                           evicted_any_ = false;
    mutable arrow::Status                  read_status_;

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        std::shared_ptr<arrow::RecordBatch> batch;
        arrow::Status st = reader_->ReadNext(&batch);
        if (!st.ok()) {
            // A record with a type conflict (a field that is a number in one
            // row and an object in another) surfaces here past the first block.
            // Record it stickily and stop so ensure()'s loop can't spin.
            read_status_ = st;
            all_read_ = true;
            return st;
        }
        if (!batch) { all_read_ = true; return arrow::Status::OK(); }
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~JsonSource() override { close_stream_reader(reader_, gate_); }

    static std::string open(const std::string& path, const Config& /*cfg*/,
                            std::unique_ptr<JsonSource>* out) {
        std::shared_ptr<arrow::io::InputStream> input;
        StreamCodec comp = StreamCodec::None;
        if (auto e = open_decoded_file(path, &input, &comp); !e.empty()) return e;
        return open_stream(path, std::move(input), comp, out);
    }

    // Records from an already-decoded stream (JSON on stdin); `label` names it
    // in messages, `comp` is the compression it arrived with.
    static std::string open_stream(const std::string& label,
                                   std::shared_ptr<arrow::io::InputStream> input,
                                   StreamCodec comp,
                                   std::unique_ptr<JsonSource>* out) {
        auto self = std::make_unique<JsonSource>();
        self->path_ = label;
        self->comp_ = comp;
        const std::string& path = label;
        std::shared_ptr<arrow::io::InputStream> unwrapped =
            std::make_shared<GatedInputStream>(
                std::make_shared<JsonArrayUnwrapStream>(std::move(input)), self->gate_);

        auto ropts = arrow::json::ReadOptions::Defaults();
        ropts.block_size  = (int32_t)std::min<int64_t>(csv_block_bytes(), INT32_MAX);
        ropts.use_threads = true;
        auto popts = arrow::json::ParseOptions::Defaults();
        popts.newlines_in_values         = true;   // tolerate pretty-printed records
        popts.unexpected_field_behavior  =
            arrow::json::UnexpectedFieldBehavior::InferType;

        auto r = arrow::json::StreamingReader::Make(
            std::move(unwrapped), ropts, popts);
        if (!r.ok()) {
            std::string m = r.status().ToString();
            if (m.find("Empty JSON") != std::string::npos)
                return "'" + path + "': no JSON records to display";
            return "'" + path + "': " + m +
                   " (if this is not tabular JSON, view it raw with `--text`)";
        }
        self->reader_ = *r;
        self->schema_ = self->reader_->schema();
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
    arrow::Status read_status() const override { return read_status_; }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return std::string("Format: JSON") +
               (comp_ != StreamCodec::None ? std::string(" (") + codec_label(comp_) + ")" : "");
    }
};

std::string open_json_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<JsonSource> s;
    std::string e = JsonSource::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
std::string open_json_stream(const std::string& label, std::shared_ptr<arrow::io::InputStream> input,
                             StreamCodec comp, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<JsonSource> s;
    std::string e = JsonSource::open_stream(label, std::move(input), comp, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
bool is_json_source(const TabularSource* src) {
    return dynamic_cast<const JsonSource*>(src) != nullptr;
}

std::string open_json_records(std::shared_ptr<arrow::io::InputStream> in,
                              const std::string& label,
                              std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<JsonSource> js;
    std::string e = JsonSource::open_stream(label, std::move(in), StreamCodec::None, &js);
    if (e.empty()) *out = std::move(js);
    return e;
}

// ── JSON documents: strict streaming lexer, pretty print, paths ─────────────
//
// `vv x.json` on a pipe prints the document re-indented (like `jq .`) and
// --json-paths prints one `path = value` line per leaf (like gron, with jq
// paths). Both run on a pull lexer over any InputStream (a file, gzip / zstd,
// stdin): iterative with an explicit container stack, numbers kept as the
// bytes in the file, strings handed on in pieces — memory is constant in
// nesting depth and value size. RFC 8259 strict: no comments, no trailing
// commas, no NaN; several top-level values are allowed (NDJSON, concatenated).

namespace vvjson {

std::string JsonError::describe() const {
    return "invalid JSON at byte " + std::to_string(offset) + " (line " +
           std::to_string(line) + ", column " + std::to_string(col) + "): " + msg;
}

namespace {

// Buffered reader with the byte offset and the line count of what it has
// consumed, so an error can name its line and column without a second pass.
class ByteReader {
    arrow::io::InputStream& in_;
    std::vector<char>       buf_;
    size_t                  pos_ = 0, len_ = 0;
    int64_t                 base_ = 0;          // stream offset of buf_[0]
    int64_t                 lines_ = 0;         // '\n' before buf_[0]
    int64_t                 last_nl_ = -1;      // offset of the last '\n' before buf_[0]
    bool                    eof_ = false;
public:
    arrow::Status           status;
    explicit ByteReader(arrow::io::InputStream& in, size_t buf = (size_t)1 << 20)
        : in_(in), buf_(buf) {}
    bool fill() {
        if (eof_) return false;
        for (size_t k = 0; k < len_; ++k)
            if (buf_[k] == '\n') { ++lines_; last_nl_ = base_ + (int64_t)k; }
        base_ += (int64_t)len_;
        pos_ = len_ = 0;
        auto r = in_.Read((int64_t)buf_.size(), buf_.data());
        if (!r.ok()) { status = r.status(); eof_ = true; return false; }
        len_ = (size_t)*r;
        if (len_ == 0) { eof_ = true; return false; }
        return true;
    }
    int peek() { return (pos_ < len_ || fill()) ? (unsigned char)buf_[pos_] : -1; }
    int get()  { int c = peek(); if (c >= 0) ++pos_; return c; }
    // The bytes available without another read (empty at end of input).
    const char* span(size_t* n) {
        if (pos_ == len_) fill();
        *n = len_ - pos_;
        return buf_.data() + pos_;
    }
    void skip(size_t n) { pos_ += n; }
    int64_t offset() const { return base_ + (int64_t)pos_; }
    void locate(int64_t* line, int64_t* col) const {
        int64_t l = lines_, nl = last_nl_;
        for (size_t k = 0; k < pos_ && k < len_; ++k)
            if (buf_[k] == '\n') { ++l; nl = base_ + (int64_t)k; }
        *line = l + 1;
        *col = offset() - nl;
    }
};

// What the lexer reports. String bytes come between str_begin / str_end (or
// key_begin / key_end) as raw pieces: the text between the quotes, escapes
// kept. `empty` on open / close: the container is {} or [].
struct Visitor {
    virtual ~Visitor() = default;
    virtual void doc_begin(int64_t /*index*/) {}
    virtual void doc_end() {}
    virtual void open(bool /*obj*/, bool /*empty*/) {}
    virtual void close(bool /*obj*/, bool /*empty*/) {}
    virtual void key_begin() {}
    virtual void key_end() {}
    virtual void str_begin() {}
    virtual void str_end() {}
    virtual void piece(const char* /*p*/, size_t /*n*/) {}
    // A number ('n') or a literal ('t' true, 'f' false, 'z' null), as written.
    virtual void scalar(const char* /*p*/, size_t /*n*/, char /*kind*/) {}
};

class Lexer {
    ByteReader&           r_;
    Visitor&              v_;
    JsonError             err_;
    std::vector<uint8_t>  stack_;   // 1 = object, 0 = array
    std::string           num_;
    int64_t               docs_ = 0;  // complete top-level values

    bool fail(const std::string& msg) {
        if (err_.ok()) {
            err_.offset = r_.offset();
            r_.locate(&err_.line, &err_.col);
            err_.msg = msg;
        }
        return false;
    }
    static bool ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    void skip_ws() { while (ws(r_.peek())) r_.get(); }
    static bool hex(int c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    static bool alnum(int c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    // After the opening quote: hand the raw bytes on, validating escapes and
    // rejecting control characters, up to and including the closing quote.
    bool string_body() {
        for (;;) {
            size_t n = 0;
            const char* p = r_.span(&n);
            if (n == 0) return fail("unterminated string");
            size_t k = 0;
            while (k < n && p[k] != '"' && p[k] != '\\' && (unsigned char)p[k] >= 0x20) ++k;
            if (k) v_.piece(p, k);
            r_.skip(k);
            if (k == n) continue;
            const char c = p[k];
            if (c == '"') { r_.skip(1); return true; }
            if (c != '\\') return fail("control character in a string (must be escaped)");
            r_.skip(1);
            const int e = r_.get();
            char esc[6] = {'\\', (char)e, 0, 0, 0, 0};
            if (e == 'u') {
                for (int d = 0; d < 4; ++d) {
                    const int h = r_.get();
                    if (!hex(h)) return fail("bad \\u escape (four hex digits expected)");
                    esc[2 + d] = (char)h;
                }
                v_.piece(esc, 6);
            } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' ||
                       e == 'n' || e == 'r' || e == 't') {
                v_.piece(esc, 2);
            } else {
                return fail(e < 0 ? "unterminated string" : "bad escape in a string");
            }
        }
    }
    bool number() {
        num_.clear();
        auto digits = [&]() {
            size_t before = num_.size();
            while (r_.peek() >= '0' && r_.peek() <= '9') num_ += (char)r_.get();
            return num_.size() > before;
        };
        if (r_.peek() == '-') num_ += (char)r_.get();
        if (r_.peek() == '0') num_ += (char)r_.get();
        else if (!digits()) return fail("bad number");
        if (r_.peek() == '.') {
            num_ += (char)r_.get();
            if (!digits()) return fail("bad number: digits expected after '.'");
        }
        if (r_.peek() == 'e' || r_.peek() == 'E') {
            num_ += (char)r_.get();
            if (r_.peek() == '+' || r_.peek() == '-') num_ += (char)r_.get();
            if (!digits()) return fail("bad number: digits expected in the exponent");
        }
        const int next = r_.peek();
        if (alnum(next) || next == '.' || next == '-' || next == '+') return fail("bad number");
        v_.scalar(num_.data(), num_.size(), 'n');
        return true;
    }
    bool literal(const char* word, char kind) {
        const size_t n = std::strlen(word);
        for (size_t k = 0; k < n; ++k)
            if (r_.get() != word[k]) return fail("expected a value");
        const int next = r_.peek();
        if (alnum(next) || next == '-' || next == '+' || next == '.') return fail("expected a value");
        v_.scalar(word, n, kind);
        return true;
    }

    // One complete top-level value.
    bool value() {
        enum { VALUE, KEY, AFTER } st = VALUE;
        stack_.clear();
        for (;;) {
            skip_ws();
            const int c = r_.peek();
            if (st == VALUE) {
                if (c == '{' || c == '[') {
                    const bool obj = c == '{';
                    r_.get();
                    skip_ws();
                    if (r_.peek() == (obj ? '}' : ']')) {
                        r_.get();
                        v_.open(obj, true);
                        v_.close(obj, true);
                        st = AFTER;
                    } else {
                        v_.open(obj, false);
                        stack_.push_back(obj ? 1 : 0);
                        st = obj ? KEY : VALUE;
                    }
                } else if (c == '"') {
                    r_.get();
                    v_.str_begin();
                    if (!string_body()) return false;
                    v_.str_end();
                    st = AFTER;
                } else if (c == '-' || (c >= '0' && c <= '9')) {
                    if (!number()) return false;
                    st = AFTER;
                } else if (c == 't') { if (!literal("true", 't')) return false; st = AFTER; }
                else if (c == 'f')   { if (!literal("false", 'f')) return false; st = AFTER; }
                else if (c == 'n')   { if (!literal("null", 'z')) return false; st = AFTER; }
                else return fail(c < 0 ? "unexpected end of input" : "expected a value");
            } else if (st == KEY) {
                if (c != '"') return fail(c < 0 ? "unexpected end of input" : "expected a string key");
                r_.get();
                v_.key_begin();
                if (!string_body()) return false;
                v_.key_end();
                skip_ws();
                if (r_.peek() != ':') return fail(r_.peek() < 0 ? "unexpected end of input"
                                                                : "expected ':' after a key");
                r_.get();
                st = VALUE;
            } else {                                    // AFTER a value
                if (stack_.empty()) return true;
                const bool obj = stack_.back() == 1;
                if (c == ',') { r_.get(); st = obj ? KEY : VALUE; }
                else if (c == (obj ? '}' : ']')) {
                    r_.get();
                    stack_.pop_back();
                    v_.close(obj, false);
                } else if (c < 0) return fail("unexpected end of input");
                else return fail(obj ? "expected ',' or '}'" : "expected ',' or ']'");
            }
        }
    }

public:
    Lexer(ByteReader& r, Visitor& v) : r_(r), v_(v) {}
    int64_t docs() const { return docs_; }
    // Every top-level value to the end of the input; `max_docs` > 0 stops
    // after that many.
    JsonError run(int64_t max_docs = 0) {
        if (r_.peek() == 0xEF) {                        // UTF-8 byte order mark
            r_.get();
            if (r_.get() != 0xBB || r_.get() != 0xBF) { fail("expected a value"); return err_; }
        }
        for (int64_t i = 0; max_docs <= 0 || i < max_docs; ++i) {
            skip_ws();
            if (r_.peek() < 0) break;
            v_.doc_begin(i);
            if (!value()) break;
            v_.doc_end();
            ++docs_;
        }
        if (err_.ok() && !r_.status.ok()) {
            err_.offset = r_.offset();
            r_.locate(&err_.line, &err_.col);
            err_.msg = "read error: " + r_.status.ToString();
        }
        return err_;
    }
};

// Buffered output to a FILE*.
class Out {
    std::FILE*  f_;
    std::string b_;
    char        last_ = '\n';     // the last byte written
public:
    // End a line cut off by an error, so the message starts on its own line.
    void end_line() { if (!b_.empty() ? b_.back() != '\n' : last_ != '\n') put('\n'); }
    explicit Out(std::FILE* f) : f_(f) { b_.reserve(1 << 16); }
    ~Out() { flush(); }
    void put(const char* p, size_t n) {
        b_.append(p, n);
        if (b_.size() >= (1 << 16)) flush();
    }
    void put(const std::string& s) { put(s.data(), s.size()); }
    void put(char c) { b_ += c; if (b_.size() >= (1 << 16)) flush(); }
    void flush() {
        if (!b_.empty()) {
            std::fwrite(b_.data(), 1, b_.size(), f_);
            last_ = b_.back();
            b_.clear();
        }
        std::fflush(f_);
    }
};

// The theme's colours for JSON tokens (all empty without colour).
struct Palette {
    std::string key, str, num, t, f, null, reset;
    explicit Palette(bool on) {
        if (!on || !*g_color.reset) return;
        key = g_color.header; str = g_color.type_str; num = g_color.number;
        t = g_color.bool_true; f = g_color.bool_false; null = g_color.null_val;
        reset = g_color.reset;
    }
    const std::string& of(char kind) const {
        return kind == 'n' ? num : kind == 't' ? t : kind == 'f' ? f : null;
    }
};

class PrettySink : public Visitor {
    Out&                 o_;
    Palette              c_;
    struct Level { bool obj; int64_t n; };
    std::vector<Level>   st_;
    void indent() {
        o_.put('\n');
        for (size_t k = 0; k < st_.size(); ++k) o_.put("  ", 2);
    }
    // A value about to start: in an array, its separator and indent (an
    // object member's indent was written with its key).
    void before_value() {
        if (st_.empty() || st_.back().obj) return;
        if (st_.back().n++) o_.put(',');
        indent();
    }
public:
    PrettySink(Out& o, bool color) : o_(o), c_(color) {}
    void doc_end() override { o_.put('\n'); }
    void open(bool obj, bool empty) override {
        before_value();
        if (empty) { o_.put(obj ? "{}" : "[]", 2); return; }
        o_.put(obj ? '{' : '[');
        st_.push_back({obj, 0});
    }
    void close(bool obj, bool empty) override {
        if (empty) return;
        st_.pop_back();
        indent();
        o_.put(obj ? '}' : ']');
    }
    void key_begin() override {
        if (st_.back().n++) o_.put(',');
        indent();
        o_.put(c_.key);
        o_.put('"');
    }
    void key_end() override { o_.put('"'); o_.put(c_.reset); o_.put(": ", 2); }
    void str_begin() override { before_value(); o_.put(c_.str); o_.put('"'); }
    void str_end() override { o_.put('"'); o_.put(c_.reset); }
    void piece(const char* p, size_t n) override { o_.put(p, n); }
    void scalar(const char* p, size_t n, char kind) override {
        before_value();
        o_.put(c_.of(kind));
        o_.put(p, n);
        o_.put(c_.reset);
    }
};

// `path = value` per leaf; an empty container is a leaf ({} / []), so the
// lines describe the document completely. Paths are jq paths: .key for an
// identifier key, ["key"] (the key as written, escapes kept) otherwise, [i]
// for an array element; the root is ".". NDJSON records are .[i].
class PathsSink : public Visitor {
    Out&                     o_;
    Palette                  c_;
    bool                     lines_;
    struct Level { bool obj; int64_t n; size_t path_len; };
    std::vector<Level>       st_;
    std::string              path_;       // the current member's path
    std::string              key_;        // the pending key (raw bytes)
    bool                     in_key_ = false;
    size_t                   member_len_ = 0;
    static bool ident(const std::string& k) {
        if (k.empty() || !(std::isalpha((unsigned char)k[0]) || k[0] == '_')) return false;
        for (char ch : k)
            if (!(std::isalnum((unsigned char)ch) || ch == '_')) return false;
        return true;
    }
    // Point path_ at the next member of the current container.
    void lead() {
        if (!st_.empty()) {
            Level& lv = st_.back();
            path_.resize(lv.path_len);
            if (lv.obj) {
                if (ident(key_)) { path_ += '.'; path_ += key_; }
                else {
                    if (path_.empty()) path_ += '.';
                    path_ += "[\""; path_ += key_; path_ += "\"]";
                }
            } else {
                if (path_.empty()) path_ += '.';
                path_ += '[' + std::to_string(lv.n) + ']';
            }
            ++lv.n;
        }
        member_len_ = path_.size();
    }
    void write_lead() {
        o_.put(c_.key);
        if (path_.empty()) o_.put('.'); else o_.put(path_);
        o_.put(c_.reset);
        o_.put(" = ", 3);
    }
public:
    PathsSink(Out& o, bool color, bool lines) : o_(o), c_(color), lines_(lines) {}
    void doc_begin(int64_t i) override {
        st_.clear();
        path_ = lines_ ? ".[" + std::to_string(i) + "]" : std::string();
    }
    void open(bool obj, bool empty) override {
        lead();
        if (empty) {
            write_lead();
            o_.put(obj ? "{}\n" : "[]\n", 3);
            return;
        }
        st_.push_back({obj, 0, member_len_});
    }
    void close(bool, bool empty) override { if (!empty) st_.pop_back(); }
    void key_begin() override { key_.clear(); in_key_ = true; }
    void key_end() override { in_key_ = false; }
    void str_begin() override { lead(); write_lead(); o_.put(c_.str); o_.put('"'); }
    void str_end() override { o_.put('"'); o_.put(c_.reset); o_.put('\n'); }
    void piece(const char* p, size_t n) override {
        if (in_key_) key_.append(p, n); else o_.put(p, n);
    }
    void scalar(const char* p, size_t n, char kind) override {
        lead();
        write_lead();
        o_.put(c_.of(kind));
        o_.put(p, n);
        o_.put(c_.reset);
        o_.put('\n');
    }
};

}  // namespace

JsonError write_json_document(arrow::io::InputStream& in, std::FILE* out,
                              JsonOut mode, bool lines, bool color) {
    ByteReader r(in);
    Out o(out);
    std::unique_ptr<Visitor> v;
    if (mode == JsonOut::Pretty) v = std::make_unique<PrettySink>(o, color);
    else                         v = std::make_unique<PathsSink>(o, color, lines);
    Lexer lx(r, *v);
    JsonError e = lx.run();
    if (!e.ok()) o.end_line();
    o.flush();
    return e;
}

// Does this decoded head of a stream look like JSON? The first token must
// open an object with a key (or `{}`) or an array with a value, and the bytes
// must lex cleanly up to where the head was cut off — or break inside that
// first value, which is malformed JSON (reported as such) rather than text.
// A complete first value followed by something that is not JSON — a TSV
// whose first cell is "[1]" — is not taken for JSON.
bool looks_like_json(const std::string& head) {
    size_t i = 0;
    if (head.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    auto skip = [&]() { while (i < head.size() && std::strchr(" \t\r\n", head[i]) && head[i]) ++i; };
    skip();
    if (i >= head.size()) return false;
    const char open = head[i++];
    skip();
    if (i >= head.size()) return false;
    const char next = head[i];
    if (open == '{') { if (next != '"' && next != '}') return false; }
    else if (open == '[') {
        if (!(std::strchr("{[\"-tfn]", next) || (next >= '0' && next <= '9'))) return false;
    } else return false;
    arrow::io::BufferReader br(arrow::Buffer::FromString(head));
    ByteReader r(br, 1 << 16);
    Visitor none;
    Lexer lx(r, none);
    JsonError e = lx.run();
    return e.ok() || lx.docs() == 0 || e.offset >= (int64_t)head.size() ||
           e.msg == "unexpected end of input" || e.msg == "unterminated string";
}


// ── JsonDoc: a lazy structural index over an mmap'ed JSON file ───────────────
//
// The tree viewer's model. Nothing is parsed up front beyond a pass over the
// top level: a container's children are indexed the first time they are
// asked for, a page of K at a time, with a checkpoint (the byte offset) every
// K children so a page evicted from the cache is re-read from its checkpoint
// rather than from the container's start. Scalars are never parsed — they
// are shown as the bytes in the file. The structural scanner is lenient (it
// tracks brackets, strings and commas); the strict Lexer validates the file
// on a background thread and reports the first error. Memory: the mapped
// file (not copied), 8 bytes per K children for checkpoints, and at most
// kMaxPages pages of nodes.
//
// Root modes: a single value; several top-level values (concatenated JSON),
// shown as a virtual array; NDJSON / JSON Lines, one element per line.

// Strict validation of a whole stream with the lexer (no output): the
// first error, or ok(). Used by JsonDoc's background validator.
JsonError lex_validate(arrow::io::InputStream& in) {
    ByteReader r(in, 1 << 20);
    Visitor none;
    Lexer lx(r, none);
    return lx.run();
}

void JsonDoc::start_validation() {
    if (size_ == 0) { valid_state_ = 1; return; }
    validator_ = std::thread([this]() {
        auto buf = std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t*>(data_),
                                                   (int64_t)size_);
        // Feeds the lexer 1 MiB at a time, counting progress and stopping
        // when the document closes.
        struct Feed : arrow::io::InputStream {
            arrow::io::BufferReader in;
            JsonDoc& d;
            Feed(std::shared_ptr<arrow::Buffer> b, JsonDoc& doc) : in(std::move(b)), d(doc) {}
            arrow::Status Close() override { return in.Close(); }
            arrow::Result<int64_t> Tell() const override { return in.Tell(); }
            bool closed() const override { return in.closed(); }
            arrow::Result<int64_t> Read(int64_t n, void* out) override {
                if (d.stop_) return 0;
                auto r = in.Read(std::min<int64_t>(n, 1 << 20), out);
                if (r.ok()) d.validated_ += *r;
                return r;
            }
            arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
                return in.Read(n);
            }
        } feed(buf, *this);
        const JsonError e = lex_validate(feed);
        if (stop_) return;
        if (e.ok()) { valid_state_ = 1; return; }
        std::lock_guard<std::mutex> g(err_mu_);
        valid_err_ = e;
        valid_state_ = 2;
    });
}

#ifdef VV_FUZZ
void fuzz_one(const uint8_t* buf, size_t n) {
    auto run = [](const std::string& in, JsonOut mode, std::string* out) {
        char* mem = nullptr;
        size_t len = 0;
        std::FILE* f = open_memstream(&mem, &len);
        arrow::io::BufferReader br(arrow::Buffer::FromString(in));
        JsonError e = write_json_document(br, f, mode, false, false);
        std::fclose(f);
        out->assign(mem, len);
        std::free(mem);
        return e;
    };
    const std::string in(reinterpret_cast<const char*>(buf), n);
    std::string p1, p2, paths;
    const JsonError e = run(in, JsonOut::Pretty, &p1);
    if (!e.ok() && (e.offset < 0 || e.offset > (int64_t)n)) std::abort();
    run(in, JsonOut::Paths, &paths);
    if (e.ok()) {
        const JsonError e2 = run(p1, JsonOut::Pretty, &p2);
        if (!e2.ok() || p1 != p2) std::abort();
    }
    (void)looks_like_json(in);

    // The lazy index over the same bytes, walked completely with tiny pages
    // (so checkpoints and evicted pages are exercised): children lie inside
    // their parent in increasing order, and an accepted document has as many
    // scalars in the tree as the lexer reported.
    setenv("VV_JSON_CHECKPOINT", "4", 1);
    JsonDoc doc;
    doc.open_buffer(in.data(), in.size(), /*lines=*/false, /*validate=*/false);
    struct Count : Visitor {
        int64_t n = 0;
        void str_end() override { ++n; }
        void scalar(const char*, size_t, char) override { ++n; }
        void open(bool, bool empty) override { if (empty) ++n; }
    } lexed;
    arrow::io::BufferReader br(arrow::Buffer::FromString(in));
    ByteReader r(br, 1 << 12);
    Lexer lx(r, lexed);
    const bool accepted = lx.run().ok();
    int64_t leaves = 0, visited = 0;
    std::vector<JNode> stack{doc.root()};
    while (!stack.empty() && visited < 200000) {
        const JNode cur = stack.back();
        stack.pop_back();
        ++visited;
        if (!cur.container()) { if (cur.kind != JKind::Error) ++leaves; continue; }
        if (cur.count == 0 && !cur.broken) { ++leaves; continue; }
        uint64_t prev = 0;
        for (int64_t i = 0; i < cur.count; ++i) {
            JNode c;
            if (!doc.child(cur, i, &c)) break;
            if (!cur.virt && (c.off < cur.off || c.off > cur.end)) std::abort();
            if (i > 0 && c.start() < prev) std::abort();
            prev = c.start();
            stack.push_back(c);
        }
    }
    if (accepted && visited < 200000 && leaves != lexed.n) std::abort();
}
#endif

}  // namespace vvjson

// JSON on stdin wanted as a document (Config::json_document, set by the CLI):
// carries the decoded stream to main(), which prints or views it. It reads
// no records itself.
class JsonStreamSource : public TabularSource {
    std::string                              label_;
    std::shared_ptr<arrow::io::InputStream>  in_;
    std::shared_ptr<arrow::Schema>           schema_ =
        arrow::schema({arrow::field("json", arrow::utf8())});
public:
    JsonStreamSource(std::string label, std::shared_ptr<arrow::io::InputStream> in)
        : label_(std::move(label)), in_(std::move(in)) {}
    std::shared_ptr<arrow::io::InputStream> stream() const { return in_; }
    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return 0; }
    int     num_chunks() const override { return 0; }
    ChunkMeta chunk_meta(int) const override { return {0, 0}; }
    arrow::Status read_chunk(int, const std::vector<int>&, std::shared_ptr<arrow::Table>*) override {
        return arrow::Status::Invalid("a JSON document stream has no table");
    }
    const std::string& path() const override { return label_; }
    std::string footer() const override { return "Format: JSON document"; }
};

// The decoded stream of JSON on stdin wanted as a document, or nullptr.
std::shared_ptr<arrow::io::InputStream> json_document_stream(TabularSource* src) {
    auto* js = dynamic_cast<JsonStreamSource*>(src);
    return js ? js->stream() : nullptr;
}
void make_json_stream_source(const std::string& label, std::shared_ptr<arrow::io::InputStream> in,
                             std::unique_ptr<TabularSource>* out) {
    *out = std::make_unique<JsonStreamSource>(label, std::move(in));
}
