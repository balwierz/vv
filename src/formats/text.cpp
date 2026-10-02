// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Plain text (and the text fallback for unknown files).

#include "internal.hpp"

// ── Plain-text source ────────────────────────────────────────────────────────
//
// A text file is modelled as one utf8 column named `line`, one row per line.
// That is what buys tabs, `/` search, `&` filter, themes, per-tab state, the
// status bar and the multi-file loop for free: every one of those walks Arrow
// columns through cell_to_string(), which works unchanged over a single string
// column. The TUI then renders a text tab without the header row and without
// truncation (see TableTUI::text_view_).
//
// Streaming and forward-only, like DelimitedSource — logs are the point, so
// slurping (md::slurp_file, which has no size cap) is not an option.
//
// Deliberately NOT LineReader: that strips '\r' anywhere in a line, so a CRLF
// file would not round-trip. This splits on '\n' only and keeps every other
// byte, including a trailing '\r'.
//
// Line terminators are dropped from the stored value and re-added on output,
// so `vv f.txt > copy` is byte-identical to f.txt. A file whose last line has
// no trailing newline is remembered (final_newline_ = false) and printed
// without one.


// Decide whether a buffer (the first ~8 KiB of a file) is plain text.
//
// Order matters. A UTF-16 file is full of NUL bytes, so the BOM test has to
// come before the NUL test or every Windows-exported file gets the generic
// "binary" message instead of one naming iconv.
TextSniffResult sniff_text(const char* p, size_t n) {
    const unsigned char* d = (const unsigned char*)p;
    if (n == 0) return TextSniffResult::Text;          // empty file is text
    // 1. Byte-order marks, longest first (UTF-32LE starts with the UTF-16LE
    //    BOM, so testing UTF-16 first would misreport it).
    if (n >= 4 && d[0]==0xFF && d[1]==0xFE && d[2]==0x00 && d[3]==0x00)
        return TextSniffResult::Utf32;
    if (n >= 4 && d[0]==0x00 && d[1]==0x00 && d[2]==0xFE && d[3]==0xFF)
        return TextSniffResult::Utf32;
    if (n >= 3 && d[0]==0xEF && d[1]==0xBB && d[2]==0xBF)
        return TextSniffResult::Text;                  // UTF-8 BOM
    if (n >= 2 && ((d[0]==0xFF && d[1]==0xFE) || (d[0]==0xFE && d[1]==0xFF)))
        return TextSniffResult::Utf16;
    // 2. Any NUL → binary.
    if (std::memchr(p, 0, n) != nullptr) return TextSniffResult::Binary;
    // 3. Too many C0 control bytes → binary. \t \n \r \f and ESC are all
    //    normal in text; excluding ESC matters or an ANSI-coloured log gets
    //    refused. DEL (0x7F) counts as a control byte.
    size_t ctrl = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = d[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0x1b)
            continue;
        if (c < 0x20 || c == 0x7f) ++ctrl;
    }
    return (ctrl * 20 > n) ? TextSniffResult::Binary   // > 5%
                           : TextSniffResult::Text;
}

// The error text for a file the sniffer refused. Split out so stdin and the
// path ladder word it identically. The caller already prefixes "vv: <path>: ",
// so `what` appears only inside the suggested command line.
std::string text_binary_error(const std::string& what,
                              TextSniffResult r) {
    if (r == TextSniffResult::Utf16 || r == TextSniffResult::Utf32) {
        const char* enc = (r == TextSniffResult::Utf16) ? "UTF-16" : "UTF-32";
        return std::string(enc) + " text is not supported. Convert it first: "
               "`iconv -f " + enc + " -t UTF-8 " + what + " | vv -`.";
    }
    return "binary file, not shown. vv views text and the formats listed by "
           "`vv --formats`; it has no hex view.";
}

// Return the compression codec a file starts with — gzip (magic 1f 8b) or
// zstandard (magic 28 b5 2f fd) — else UNCOMPRESSED. Detects by content, not
// suffix, so `syslog.1.gz` or a bare `dump.zst` work too. Rewinds `rf`.
arrow::Compression::type sniff_stream_codec(
 const std::shared_ptr<arrow::io::ReadableFile>& rf) {
    auto head = rf->Read(4);
    arrow::Compression::type comp = arrow::Compression::UNCOMPRESSED;
    if (head.ok() && (*head)->size() >= 2) {
        const uint8_t* m = (*head)->data();
        int64_t n = (*head)->size();
        if (m[0] == 0x1f && m[1] == 0x8b) comp = arrow::Compression::GZIP;
        else if (n >= 4 && m[0] == 0x28 && m[1] == 0xB5 &&
                 m[2] == 0x2F && m[3] == 0xFD) comp = arrow::Compression::ZSTD;
    }
    (void)rf->Seek(0);
    return comp;
}

class TextSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    std::shared_ptr<arrow::io::InputStream> in_;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>           batch_first_row_;
    mutable std::vector<int64_t>           batch_num_rows_;
    mutable int64_t                        rows_so_far_   = 0;
    mutable bool                           all_read_      = false;
    mutable bool                           retain_all_    = false;
    mutable bool                           evicted_any_   = false;
    mutable bool                           final_newline_ = true;
    mutable std::string                    pending_;      // partial last line
    mutable arrow::Status                  read_status_;
    bool                                   gz_ = false;   // compressed stream
    arrow::Compression::type               comp_ =
        arrow::Compression::UNCOMPRESSED;                 // gzip or zstd, if gz_

    static constexpr int    BATCH_SIZE = 4096;
    static constexpr size_t READ_SIZE  = 64 * 1024;

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder b;
        int count = 0;
        std::string buf;
        buf.resize(READ_SIZE);
        while (count < BATCH_SIZE) {
            // Emit every complete line already buffered.
            size_t start = 0;
            for (;;) {
                size_t nl = pending_.find('\n', start);
                if (nl == std::string::npos) break;
                ARROW_RETURN_NOT_OK(b.Append(pending_.data() + start,
                                             (int32_t)(nl - start)));
                start = nl + 1;
                if (++count >= BATCH_SIZE) break;
            }
            if (start) pending_.erase(0, start);
            if (count >= BATCH_SIZE) break;

            auto got = in_->Read((int64_t)READ_SIZE, buf.data());
            if (!got.ok()) {
                read_status_ = got.status();
                all_read_ = true;
                break;
            }
            if (*got == 0) {
                // EOF. A non-empty remainder is a final line with no newline.
                if (!pending_.empty()) {
                    ARROW_RETURN_NOT_OK(b.Append(pending_.data(),
                                                 (int32_t)pending_.size()));
                    ++count;
                    pending_.clear();
                    final_newline_ = false;
                }
                all_read_ = true;
                break;
            }
            pending_.append(buf.data(), (size_t)*got);
        }
        if (count == 0) { all_read_ = true; return read_status_; }

        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(b.Finish(&arr));
        auto batch = arrow::RecordBatch::Make(schema_, count, {arr});
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    static std::string open(const std::string& path, bool gz,
                            const Config& /*cfg*/,
                            std::unique_ptr<TextSource>* out) {
        auto self = std::make_unique<TextSource>();
        self->path_ = path;
        self->schema_ = arrow::schema({arrow::field("line", arrow::utf8())});

        auto rf = arrow::io::ReadableFile::Open(path);
        if (!rf.ok())
            return "Cannot open '" + path + "': " + rf.status().ToString();
        auto raw = rf.ValueOrDie();
        // Detect gzip / zstd by magic (the `gz` hint from the caller is
        // suffix-based and may be blank for a magic-only match).
        self->comp_ = sniff_stream_codec(raw);
        (void)gz;
        self->gz_ = (self->comp_ != arrow::Compression::UNCOMPRESSED);
        std::shared_ptr<arrow::io::InputStream> in = raw;
        if (self->gz_) {
            auto codec = arrow::util::Codec::Create(self->comp_);
            if (!codec.ok()) return codec.status().ToString();
            auto ci = arrow::io::CompressedInputStream::Make(codec->get(), in);
            if (!ci.ok()) return ci.status().ToString();
            in = ci.ValueOrDie();
        }
        self->in_ = std::move(in);

        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();
        *out = std::move(self);
        return "";
    }

    // Same, over an already-open stream (stdin).
    static std::string open_stream(const std::string& label,
                                   std::shared_ptr<arrow::io::InputStream> in,
                                   std::unique_ptr<TextSource>* out) {
        auto self = std::make_unique<TextSource>();
        self->path_   = label;
        self->in_     = std::move(in);
        self->schema_ = arrow::schema({arrow::field("line", arrow::utf8())});
        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + label + "': " + st.ToString();
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
        while (!all_read_ && (int)batches_.size() <= i) (void)advance();
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
        return std::string("Format: text") +
               (comp_ == arrow::Compression::GZIP ? " (gzip)" :
                comp_ == arrow::Compression::ZSTD ? " (zstd)" : "");
    }
    // True when the file's last line carries no terminator, so a verbatim
    // dump can reproduce that.
    bool final_newline() const { return final_newline_; }
    bool is_text() const override { return true; }
};

std::string open_text_stream(const std::string& label, std::shared_ptr<arrow::io::InputStream> in,
                             std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<TextSource> s;
    std::string e = TextSource::open_stream(label, std::move(in), &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
// Whether a text source's last line had a terminator (true for any other source).
bool text_final_newline(const TabularSource& src) {
    auto* ts = dynamic_cast<const TextSource*>(&src);
    return !ts || ts->final_newline();
}

// ── Plain-text entry points ──────────────────────────────────────────────────
//
// The extension list is deliberately SHORT. `.py`, `.c`, `.conf`, `.toml`,
// `.rst` need no entry: the content sniff at the bottom of the ladder routes
// them to text identically, and claiming them in `--formats` would advertise
// vv as a code viewer with no highlighting. `.json` is absent here because it
// has its own tabular reader (JsonSource, dispatched above); `vv --text f.json`
// still shows the raw source.
static const char* kTextExts[] = { ".txt", ".text", ".log" };

bool text_ext(const std::string& p) {
    for (const char* e : kTextExts) {
        if (fends_ci(p, e)) return true;
        if (fends_ci(p, std::string(e) + ".gz")) return true;
    }
    return false;
}

// True when `path` has no extension at all (a bare `README`, `Makefile`,
// `CHANGELOG`). Such files get no "sniffed as text" note — a note on every
// README would be nagging. A dotfile (`.bashrc`) counts as extension-less.
bool has_no_extension(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path
                                                    : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos || dot == 0;
}

// Sniff `path` and, if it is text, open it as a TextSource. Returns an error
// string otherwise. `note` asks for the one-line stderr note that says text
// mode was reached by content-sniffing rather than by extension.
std::string open_text(const std::string& path, const Config& cfg,
                      std::unique_ptr<TabularSource>* out,
                      bool note) {
    auto rf = arrow::io::ReadableFile::Open(path);
    if (!rf.ok())
        return "Cannot open '" + path + "': " + rf.status().ToString();
    std::shared_ptr<arrow::io::InputStream> in = rf.ValueOrDie();

    // Compression is detected by magic, not by suffix, so `syslog.1.gz` and a
    // bare `dump.zst` work as well as `notes.txt.gz`. Decompress before sniffing
    // so the text/binary check sees the real content.
    arrow::Compression::type comp = sniff_stream_codec(rf.ValueOrDie());
    bool gz = (comp != arrow::Compression::UNCOMPRESSED);
    if (gz) {
        auto codec = arrow::util::Codec::Create(comp);
        if (!codec.ok()) return codec.status().ToString();
        auto ci = arrow::io::CompressedInputStream::Make(codec->get(), in);
        if (!ci.ok()) return ci.status().ToString();
        in = ci.ValueOrDie();
    }

    std::string buf(8192, '\0');
    auto got = in->Read(8192, buf.data());
    if (!got.ok()) return "Cannot read '" + path + "': " + got.status().ToString();
    (void)rf.ValueOrDie()->Close();

    TextSniffResult r = sniff_text(buf.data(), (size_t)*got);
    if (r != TextSniffResult::Text) return text_binary_error(path, r);

    if (note)
        std::fprintf(stderr,
                     "vv: %s: no known format claims this extension; "
                     "shown as plain text\n", path.c_str());

    std::unique_ptr<TextSource> src;
    std::string err = TextSource::open(path, gz, cfg, &src);
    if (!err.empty()) return err;
    *out = std::move(src);
    return "";
}


// Opens a single file — declared here so DatasetSource (below) can open its
// member files, and defined further down with the directory branch that reaches
// DatasetSource. A member file is never itself a directory, so there is no
// recursion.
// A FASTA / FASTQ extension, plain or .gz / .bgz (checked on `det`, where
// .bgz reads as .gz). The reader decompresses through BGZF, which handles
// gzip but not zstandard, so a .zst wrapper does not count.
bool fastx_ext(const std::string& path, const std::string& det,
               std::initializer_list<const char*> exts) {
    if (fends_ci(path, ".zst") || fends_ci(path, ".zstd")) return false;
    for (const char* e : exts)
        if (fends_ci(det, e) || fends_ci(det, (std::string(e) + ".gz").c_str())) return true;
    return false;
}
