// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// The format dispatch behind open_source(), stdin / pipe input, and
// directory datasets (incl. 10x matrix directories).

#include "internal.hpp"

// ── Format detection + source factory ────────────────────────────────────────
// (fends is defined above, near the preamble helpers)

// Open any supported file.  Returns empty string on success; error message otherwise.
// Read up to `n` bytes from stdin into a buffer. Used to sniff format magic
// bytes and the first line for delimiter detection.
static std::string sniff_fd(int fd, size_t n, std::string* out_buf) {
    out_buf->clear();
    out_buf->resize(n);
    size_t total = 0;
    while (total < n) {
        ssize_t got = ::read(fd, out_buf->data() + total, n - total);
        if (got == 0) break;            // EOF
        if (got < 0) {
            if (errno == EINTR) continue;
            return std::string("stdin read error: ") + std::strerror(errno);
        }
        total += (size_t)got;
    }
    out_buf->resize(total);
    return "";
}

// ── Binary input on a pipe ───────────────────────────────────────────────────
//
// Stdin and process substitution (`vv <(zcat x.parquet.gz)`) are pipes: they
// cannot seek, and every binary format vv reads needs to (a Parquet footer, an
// Arrow IPC footer, a BAM index, an HDF5 superblock). Such input is copied to a
// temporary file, named with the format's extension so the normal dispatch
// opens it, and the file is removed when vv exits — normally, or on SIGINT /
// SIGTERM / SIGHUP (the TUI's handler calls remove_spooled_files too).
static char                  g_spooled[8][4096];
static volatile sig_atomic_t g_n_spooled = 0;

void remove_spooled_files() {             // async-signal-safe: unlink only
    for (int i = 0; i < g_n_spooled; ++i) ::unlink(g_spooled[i]);
}
static void spool_signal(int sig) {
    remove_spooled_files();
    signal(sig, SIG_DFL);
    raise(sig);
}
static void spool_register(const std::string& path) {
    if (g_n_spooled == 0) {
        std::atexit(remove_spooled_files);
        for (int sig : {SIGINT, SIGTERM, SIGHUP}) {
            auto prev = signal(sig, spool_signal);
            if (prev != SIG_DFL) signal(sig, prev);   // someone else handles it
        }
    }
    if (g_n_spooled < 8 && path.size() < sizeof g_spooled[0]) {
        std::memcpy(g_spooled[g_n_spooled], path.c_str(), path.size() + 1);
        g_n_spooled = g_n_spooled + 1;
    }
}

// The extension of the binary format `head` (the first bytes of the input)
// starts with, for the formats whose magic is unambiguous; "bgzf" for a BGZF
// stream (BAM, BCF or bgzipped text — decided after copying); "" otherwise.
static std::string pipe_binary_ext(const std::string& head) {
    auto at = [&](size_t off, const char* m, size_t n) {
        return head.size() >= off + n && std::memcmp(head.data() + off, m, n) == 0;
    };
    if (at(0, "PAR1", 4))                       return ".parquet";
    if (at(0, "ARROW1\0\0", 8))                 return ".arrow";
    if (at(0, "FEA1", 4))                       return ".feather";
    if (at(0, "ORC", 3))                        return ".orc";
    if (at(0, "LSB1", 4))                       return ".lociss";
    if (at(0, "CRAM", 4))                       return ".cram";
    if (at(0, "SQLite format 3\0", 16))         return ".sqlite";
    if (at(0, "\x89HDF\r\n\x1a\n", 8))           return ".h5";
    if (at(0, "\x93NUMPY", 6))                  return ".npy";
    if (at(0, "\x26\xfc\x8f\x88", 4))            return ".bw";     // 0x888FFC26 LE
    if (at(0, "\xeb\xf2\x89\x87", 4))            return ".bb";     // 0x8789F2EB LE
    if (at(0, "\x43\x27\x41\x1a", 4))            return ".2bit";   // 0x1A412743 LE
    if (at(0, "\x1f\x8b\x08\x04", 4))            return "bgzf";
    if (at(0, "PK\x03\x04", 4) && head.size() >= 30) {
        // A zip: the first entry names the container.
        const uint16_t n = (uint8_t)head[26] | ((uint8_t)head[27] << 8);
        const std::string first = head.substr(30, n);
        if (first == "mimetype")                  return ".ods";
        if (first.size() > 4 && first.compare(first.size() - 4, 4, ".npy") == 0)
                                                  return ".npz";
        if (first == "[Content_Types].xml" || first.rfind("xl/", 0) == 0 ||
            first.rfind("_rels/", 0) == 0 || first.rfind("docProps/", 0) == 0)
                                                  return ".xlsx";
    }
    return "";
}

// Copy `head` and then the rest of `fd` into a new temporary file, and return
// its path (renamed to end in `ext`, or, for "bgzf", in the extension htslib's
// content detection gives: .bam, .bcf, .cram, .vcf.gz, .bed.gz, .fq.gz, .fa.gz,
// else .tsv.gz). The file is removed at exit.
static std::string spool_pipe(int fd, const std::string& head, std::string ext,
                              std::string* path_out, int64_t* bytes_out) {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/vv-pipe-XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    int out = ::mkstemp(name.data());
    if (out < 0) return "cannot create a temporary file in " + tmpl.substr(0, tmpl.rfind('/')) +
                        ": " + std::strerror(errno) + " (set TMPDIR)";
    std::string path(name.data());
    spool_register(path);
    int64_t total = 0;
    auto put = [&](const char* p, size_t n) -> bool {
        while (n) {
            ssize_t w = ::write(out, p, n);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            p += w; n -= (size_t)w; total += w;
        }
        return true;
    };
    std::string err;
    if (!put(head.data(), head.size())) err = std::strerror(errno);
    std::vector<char> buf(1 << 20);
    while (err.empty()) {
        ssize_t r = ::read(fd, buf.data(), buf.size());
        if (r == 0) break;
        if (r < 0) { if (errno == EINTR) continue; err = std::strerror(errno); break; }
        if (!put(buf.data(), (size_t)r)) err = std::strerror(errno);
    }
    ::close(out);
    if (!err.empty()) return "copying the input to " + path + " failed: " + err;
    if (ext == "bgzf") {
        ext = ".tsv.gz";
        if (htsFile* hf = hts_open(path.c_str(), "r")) {
            switch (hts_get_format(hf)->format) {
                case bam:          ext = ".bam";    break;
                case bcf:          ext = ".bcf";    break;
                case cram:         ext = ".cram";   break;
                case vcf:          ext = ".vcf.gz"; break;
                case bed:          ext = ".bed.gz"; break;
                case fastq_format: ext = ".fq.gz";  break;
                case fasta_format: ext = ".fa.gz";  break;
                default: break;
            }
            hts_close(hf);
        }
    }
    const std::string named = path + ext;
    if (::rename(path.c_str(), named.c_str()) != 0)
        return "renaming " + path + " failed: " + std::strerror(errno);
    spool_register(named);
    *path_out = named;
    *bytes_out = total;
    return "";
}

std::string human_bytes(int64_t sz) {
    char buf[32];
    if      (sz < 1024)             std::snprintf(buf, sizeof(buf), "%lld B", (long long)sz);
    else if (sz < 1024 * 1024)      std::snprintf(buf, sizeof(buf), "%.1f KiB", sz / 1024.0);
    else if (sz < 1024LL * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MiB", sz / (1024.0 * 1024));
    else                            std::snprintf(buf, sizeof(buf), "%.2f GiB", sz / (1024.0 * 1024 * 1024));
    return buf;
}

// Copy an already-decoded stream (stdin after gunzip) into a new temporary
// file ending in `ext`, removed at exit like spool_pipe's copies. With a
// `progress` label and stderr on a terminal, the bytes written so far are
// shown on one stderr line ("vv: decompressing x.json.gz: 1.2 GiB"),
// cleared at the end.
std::string spool_stream(const std::shared_ptr<arrow::io::InputStream>& in,
                         const std::string& ext, std::string* path_out,
                         int64_t* bytes_out, const std::string& progress) {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/vv-pipe-XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back('\0');
    int out = ::mkstemp(name.data());
    if (out < 0) return "cannot create a temporary file: " + std::string(std::strerror(errno)) +
                        " (set TMPDIR)";
    std::string path(name.data());
    spool_register(path);
    int64_t total = 0, shown_at = 0;
    const bool show = !progress.empty() && isatty(STDERR_FILENO);
    const std::string shown_name = progress.substr(progress.find_last_of('/') + 1);
    std::string err;
    for (;;) {
        auto buf = in->Read(1 << 20);
        if (!buf.ok()) { err = buf.status().ToString(); break; }
        if ((*buf)->size() == 0) break;
        const char* p = reinterpret_cast<const char*>((*buf)->data());
        size_t n = (size_t)(*buf)->size();
        while (n) {
            ssize_t w = ::write(out, p, n);
            if (w < 0) { if (errno == EINTR) continue; err = std::strerror(errno); break; }
            p += w; n -= (size_t)w; total += w;
        }
        if (!err.empty()) break;
        if (show && total - shown_at >= (32 << 20)) {
            shown_at = total;
            std::fprintf(stderr, "\rvv: decompressing %s: %s ", shown_name.c_str(), human_bytes(total).c_str());
            std::fflush(stderr);
        }
    }
    if (show && shown_at > 0) { std::fprintf(stderr, "\r\033[K"); std::fflush(stderr); }
    ::close(out);
    if (!err.empty()) return "copying the input to " + path + " failed: " + err;
    const std::string named = path + ext;
    if (::rename(path.c_str(), named.c_str()) != 0)
        return "renaming " + path + " failed: " + std::strerror(errno);
    spool_register(named);
    *path_out = named;
    *bytes_out = total;
    return "";
}

// The genomics text format the first bytes of piped text are in, as the
// file readers would take it: FASTA ('>'), FASTQ ('@' with a '+' third
// line), SAM (an @HD / @SQ / @RG / @PG / @CO header line), VCF
// (##fileformat=VCF), GFF (##gff-version). "" for anything else (TSV / CSV).
static std::string sniff_text_format(const std::string& head) {
    auto starts = [&](const char* p) { return head.rfind(p, 0) == 0; };
    if (starts("##fileformat=VCF")) return "vcf";
    if (starts("##gff-version"))     return "gff";
    for (const char* t : {"@HD\t", "@SQ\t", "@RG\t", "@PG\t", "@CO\t"})
        if (starts(t)) return "sam";
    if (starts(">")) return "fasta";
    if (vvjson::looks_like_json(head)) return "json";
    if (starts("@")) {
        size_t a = head.find('\n');
        size_t b = a == std::string::npos ? a : head.find('\n', a + 1);
        if (b != std::string::npos && b + 1 < head.size() && head[b + 1] == '+') return "fastq";
    }
    return "";
}

// True when the regular file at `path` begins like an Arrow IPC stream.
static bool file_is_ipc_stream(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    uint8_t m[8] = {0};
    return f.read(reinterpret_cast<char*>(m), 8) && looks_like_ipc_stream(m, 8);
}

// True for a path that names a pipe, FIFO, socket or character device —
// process substitution's /dev/fd/N — rather than a regular file.
bool path_is_pipe(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISSOCK(st.st_mode);
}

// ── Directory / partitioned-dataset source ────────────────────────────────────
//
// `vv DIR` concatenates the data files under DIR (recursively) into one table,
// the way Spark / Arrow / DuckDB write a "dataset": many `part-*.parquet` files,
// often under Hive-style `key=value/` directories. It reuses the per-file
// readers (Parquet, Arrow/Feather, ORC, CSV/TSV, JSON) — one child open at a
// time — and streams their chunks in filename order, so a dataset larger than
// memory still previews. Hive `key=value` path components become columns,
// constant within each file. Scope is the columnar / delimited formats where
// concatenation is meaningful; the genomics formats (with per-file headers and
// indexes) are not treated as datasets.

namespace dataset {

// Coarse format group of a data-file name, ignoring a .gz/.zst wrapper. Empty
// for a file that is not dataset data (sidecars, _SUCCESS, README, …), so it is
// skipped during discovery. Also the guard against concatenating mixed formats.
static std::string format_group(const std::string& name) {
    std::string s = name;
    s = strip_compression_suffix(s);
    if (fends_ci(s, ".parquet"))                          return "parquet";
    if (fends_ci(s, ".arrow") || fends_ci(s, ".feather")) return "arrow";
    if (fends_ci(s, ".orc"))                              return "orc";
    if (fends_ci(s, ".csv"))                              return "csv";
    if (fends_ci(s, ".tsv"))                              return "tsv";
    if (fends_ci(s, ".json") || fends_ci(s, ".ndjson") ||
        fends_ci(s, ".jsonl"))                            return "json";
    return "";
}

// A partition value is treated as an integer column only when it is a canonical
// integer — no leading zeros (so `month=01` stays the string "01" rather than
// becoming 1 and losing the zero), fitting int64. Mirrors vv's leading-zero-ID
// handling elsewhere.
static bool is_canonical_int(const std::string& s) {
    size_t i = 0;
    if (i < s.size() && s[i] == '-') ++i;
    if (i >= s.size()) return false;                 // "" or "-"
    if (s[i] == '0' && s.size() - i > 1) return false;  // leading zero
    for (size_t j = i; j < s.size(); ++j)
        if (s[j] < '0' || s[j] > '9') return false;
    errno = 0;
    char* end = nullptr;
    (void)std::strtoll(s.c_str(), &end, 10);
    return errno == 0 && end && *end == '\0';
}

struct DsFile {
    std::string                                       path;
    std::vector<std::pair<std::string, std::string>>  parts;  // (key,value), in path order
};

// Two files belong to the same dataset when their schemas agree on column names
// and types, in order. Nullability and field metadata are ignored: parts written
// by the same job commonly differ there (an all-present column in one file,
// with a null in another) while being the same table.
static bool schemas_compatible(const arrow::Schema& a, const arrow::Schema& b) {
    if (a.num_fields() != b.num_fields()) return false;
    for (int i = 0; i < a.num_fields(); ++i) {
        if (a.field(i)->name() != b.field(i)->name()) return false;
        if (!a.field(i)->type()->Equals(*b.field(i)->type())) return false;
    }
    return true;
}

}  // namespace dataset

class DatasetSource : public TabularSource {
    std::string                            label_;
    std::string                            group_;
    std::vector<dataset::DsFile>           files_;
    std::vector<std::string>               part_keys_;
    std::vector<bool>                      part_is_int_;
    std::shared_ptr<arrow::Schema>         data_schema_;   // the child files' schema
    std::shared_ptr<arrow::Schema>         schema_;        // data + partition columns
    int                                    ndata_ = 0;
    Config                                 child_cfg_;

    mutable std::unique_ptr<TabularSource> child_;
    mutable size_t                         cur_file_  = 0;
    mutable int                            cur_local_ = 0;
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>           batch_first_row_;
    mutable std::vector<int64_t>           batch_num_rows_;
    mutable int64_t                        rows_so_far_ = 0;
    mutable bool                           all_read_    = false;
    mutable bool                           retain_all_  = false;
    mutable bool                           evicted_any_ = false;
    mutable arrow::Status                  read_status_;

    // Build the constant partition-value array for the current file.
    arrow::Result<std::shared_ptr<arrow::Array>>
    partition_array(size_t key_idx, int64_t n) const {
        const std::string& val = files_[cur_file_].parts[key_idx].second;
        if (part_is_int_[key_idx]) {
            arrow::Int64Scalar s(std::strtoll(val.c_str(), nullptr, 10));
            return arrow::MakeArrayFromScalar(s, n);
        }
        arrow::StringScalar s(val);
        return arrow::MakeArrayFromScalar(s, n);
    }

    arrow::Status advance() const {
        for (;;) {
            if (all_read_) return arrow::Status::OK();
            if (cur_file_ >= files_.size()) { all_read_ = true; return arrow::Status::OK(); }

            if (!child_) {
                std::unique_ptr<TabularSource> c;
                std::string e = open_source_dispatch(files_[cur_file_].path, child_cfg_, &c);
                if (!e.empty()) {
                    read_status_ = arrow::Status::IOError(e);
                    all_read_ = true; return read_status_;
                }
                if (!dataset::schemas_compatible(*c->schema(), *data_schema_)) {
                    read_status_ = arrow::Status::Invalid(
                        "schema of '", files_[cur_file_].path,
                        "' differs from '", files_[0].path,
                        "' — every file in a dataset must share one schema "
                        "(same column names and types)");
                    all_read_ = true; return read_status_;
                }
                child_ = std::move(c);
                cur_local_ = 0;
            }

            child_->ensure(cur_local_);
            if (cur_local_ < child_->num_chunks()) {
                std::vector<int> allcols(ndata_);
                std::iota(allcols.begin(), allcols.end(), 0);
                std::shared_ptr<arrow::Table> t;
                arrow::Status st = child_->read_chunk(cur_local_, allcols, &t);
                ++cur_local_;
                if (!st.ok()) { read_status_ = st; all_read_ = true; return st; }

                auto combined = t->CombineChunks();
                if (!combined.ok()) {
                    read_status_ = combined.status(); all_read_ = true;
                    return read_status_;
                }
                int64_t n = (*combined)->num_rows();
                if (n == 0) continue;   // empty chunk — nothing to retain

                std::vector<std::shared_ptr<arrow::Array>> arrs;
                arrs.reserve(ndata_ + part_keys_.size());
                for (int c = 0; c < ndata_; ++c)
                    arrs.push_back((*combined)->column(c)->chunk(0));
                for (size_t k = 0; k < part_keys_.size(); ++k) {
                    auto pa = partition_array(k, n);
                    if (!pa.ok()) { read_status_ = pa.status(); all_read_ = true;
                                    return read_status_; }
                    arrs.push_back(*pa);
                }
                auto batch = arrow::RecordBatch::Make(schema_, n, arrs);
                stream_retain(batches_, batch_first_row_, batch_num_rows_,
                              rows_so_far_, retain_all_, evicted_any_, std::move(batch));
                return arrow::Status::OK();
            }

            // Current file drained — surface any read error, then move on.
            if (!child_->read_status().ok()) {
                read_status_ = child_->read_status();
                all_read_ = true; return read_status_;
            }
            child_.reset();
            ++cur_file_;
        }
    }

    // Walk DIR, collecting data files and their Hive partitions. Populates the
    // members; returns an error string on a bad layout.
    std::string discover(const std::string& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (fs::recursive_directory_iterator
                 it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string base = it->path().filename().string();
            if (base.empty() || base[0] == '.' || base[0] == '_') continue;  // hidden / _SUCCESS
            std::string g = dataset::format_group(base);
            if (g.empty()) continue;                       // sidecar / non-data file
            if (group_.empty()) group_ = g;
            else if (g != group_)
                return "'" + dir + "': directory mixes " + group_ + " and " + g +
                       " files; a dataset must be a single format";
            dataset::DsFile f;
            f.path = it->path().string();
            fs::path rel = fs::relative(it->path(), dir, ec);
            for (const auto& comp : rel.parent_path()) {
                std::string c = comp.string();
                auto eq = c.find('=');
                if (eq != std::string::npos && eq > 0)
                    f.parts.emplace_back(c.substr(0, eq), c.substr(eq + 1));
            }
            files_.push_back(std::move(f));
        }
        if (files_.empty())
            return "'" + dir + "': no supported data files found "
                   "(.parquet / .arrow / .feather / .orc / .csv / .tsv / .json / "
                   ".ndjson / .jsonl, optionally .gz / .zst)";
        std::sort(files_.begin(), files_.end(),
                  [](const dataset::DsFile& a, const dataset::DsFile& b) {
                      return a.path < b.path;
                  });
        // Partition keys must be identical across every file (Hive guarantees
        // this); the first file's keys are canonical.
        for (const auto& kv : files_[0].parts) part_keys_.push_back(kv.first);
        for (const auto& f : files_) {
            std::vector<std::string> fk;
            for (const auto& kv : f.parts) fk.push_back(kv.first);
            if (fk != part_keys_)
                return "'" + dir + "': inconsistent partition layout — '" +
                       f.path + "' does not carry the same key=value directories "
                       "as the other files";
        }
        // A partition key is an integer column only if every value is a canonical
        // integer; otherwise it stays a string.
        part_is_int_.assign(part_keys_.size(), true);
        for (const auto& f : files_)
            for (size_t k = 0; k < part_keys_.size(); ++k)
                if (!dataset::is_canonical_int(f.parts[k].second))
                    part_is_int_[k] = false;
        return "";
    }

public:
    static std::string open(const std::string& dir, const Config& cfg,
                            std::unique_ptr<DatasetSource>* out) {
        auto self = std::make_unique<DatasetSource>();
        self->label_ = dir;

        std::string derr = self->discover(dir);
        if (!derr.empty()) return derr;

        // Children are opened raw: the row/range operators apply to the dataset
        // as a whole, above this source, not per file.
        self->child_cfg_               = cfg;
        self->child_cfg_.region.clear();
        self->child_cfg_.regions_file.clear();
        self->child_cfg_.head_rows     = 0;
        self->child_cfg_.head_rows_set = false;
        self->child_cfg_.bam_tags.clear();

        // The first file fixes the data schema; the unified schema appends one
        // column per partition key.
        {
            std::unique_ptr<TabularSource> first;
            std::string e = open_source_dispatch(self->files_[0].path,
                                                 self->child_cfg_, &first);
            if (!e.empty())
                return "'" + self->files_[0].path + "': " + e;
            self->data_schema_ = first->schema();
            self->ndata_ = self->data_schema_->num_fields();
            self->child_ = std::move(first);   // reused as the first child to read
        }
        // Partition columns are appended after the data columns, one per key.
        // (A key that also names a data column is rare — Hive keeps the partition
        // value in the path, not the file — and would just appear twice; the
        // schema and each batch stay in lock-step either way.)
        arrow::FieldVector fields = self->data_schema_->fields();
        for (size_t k = 0; k < self->part_keys_.size(); ++k)
            fields.push_back(arrow::field(
                self->part_keys_[k],
                self->part_is_int_[k] ? arrow::int64() : arrow::utf8()));
        self->schema_ = arrow::schema(fields);

        auto st = self->advance();
        if (!st.ok()) return st.ToString();
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
        while (!all_read_ && (int)batches_.size() <= i) {
            auto st = advance();
            if (!st.ok()) { if (read_status_.ok()) read_status_ = st; break; }
        }
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
    const std::string& path() const override { return label_; }
    std::string footer() const override {
        std::string f = "Format: dataset (" + group_ + ")  |  Files: " +
                        std::to_string(files_.size());
        if (!part_keys_.empty()) {
            f += "  |  Partitions: ";
            for (size_t k = 0; k < part_keys_.size(); ++k)
                f += (k ? ", " : "") + part_keys_[k];
        }
        return f;
    }
};

static int tenx_sidecar_kind(const std::string& path);

// The files of a 10x Genomics / STARsolo matrix directory, found by basename in
// `dir` itself (not recursively): matrix.mtx, barcodes.tsv and features.tsv
// (Cell Ranger v3+) or genes.tsv (v2), each optionally .gz / .zst. False unless
// the matrix, the barcodes and one feature file are all present.
struct TenxDirFiles { std::string matrix, barcodes, features; bool v2 = false; };
static bool find_tenx_dir(const std::string& dir, TenxDirFiles* f) {
    std::error_code ec;
    std::string genes;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string p = e.path().string();
        std::string b = e.path().filename().string();
        for (char& c : b) c = (char)std::tolower((unsigned char)c);
        if (b == "matrix.mtx" || b == "matrix.mtx.gz" || b == "matrix.mtx.zst" ||
            b == "matrix.mtx.zstd") { f->matrix = p; continue; }
        switch (tenx_sidecar_kind(p)) {
            case 1: f->barcodes = p; break;
            case 2: f->features = p; break;
            case 3: genes = p;       break;
            default: break;
        }
    }
    if (f->features.empty() && !genes.empty()) { f->features = genes; f->v2 = true; }
    return !f->matrix.empty() && !f->barcodes.empty() && !f->features.empty();
}

// A 10x Genomics / STARsolo matrix directory opened as one source. The first
// tab streams matrix.mtx's stored entries (row, col, value — 0-based) with the
// feature and barcode of each entry appended as label columns (feature_id,
// feature_name[, feature_type], barcode); features and barcodes are sibling
// tabs. The sidecars are read in full once — they label the entries and are
// small next to the matrix. Cell Ranger writes features × barcodes; a matrix
// written the other way round (barcodes × features) is recognised by its
// shape. A shape that matches neither is an error rather than wrong labels.
class TenxDirSource : public TabularSource {
    std::unique_ptr<TabularSource>             inner_;
    std::shared_ptr<arrow::Schema>             schema_;
    int                                        n_inner_ = 0;
    bool                                       rows_are_features_ = true;
    std::shared_ptr<arrow::Table>              features_, barcodes_;
    std::vector<std::shared_ptr<arrow::StringArray>> feat_labels_;  // id, name[, type]
    std::shared_ptr<arrow::StringArray>        barcode_label_;
    TenxDirFiles                               files_;
    std::string                                note_;

    // A sidecar as a sibling tab.
    class Sidecar : public MemoryTableSource {
        std::string label_;
    public:
        Sidecar(std::shared_ptr<arrow::Table> t, const std::string& path,
                std::string label, std::string footer)
            : MemoryTableSource(std::move(t), path, std::move(footer)),
              label_(std::move(label)) {}
        std::string tab_label() const override { return label_; }
    };

    static std::string load_table(const std::string& path, const Config& cfg,
                                  std::shared_ptr<arrow::Table>* out);

    // A label column as one StringArray (non-string columns rendered as text).
    static std::shared_ptr<arrow::StringArray>
    as_strings(const std::shared_ptr<arrow::ChunkedArray>& c) {
        arrow::StringBuilder b;
        for (const auto& ch : c->chunks()) {
            if (ch->type_id() == arrow::Type::STRING) {
                const auto& s = static_cast<const arrow::StringArray&>(*ch);
                for (int64_t i = 0; i < s.length(); ++i) {
                    if (s.IsNull(i)) (void)b.AppendNull(); else (void)b.Append(s.GetView(i));
                }
            } else {
                for (int64_t i = 0; i < ch->length(); ++i) {
                    if (ch->IsNull(i)) (void)b.AppendNull(); else (void)b.Append(cell_to_string(*ch, i));
                }
            }
        }
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return std::static_pointer_cast<arrow::StringArray>(a);
    }

    // Look `lab` up at each (0-based, already range-checked) index in `idx`.
    static std::shared_ptr<arrow::ChunkedArray>
    gather(const arrow::StringArray& lab, const arrow::ChunkedArray& idx) {
        arrow::ArrayVector out;
        for (const auto& ch : idx.chunks()) {
            const auto& ix = static_cast<const arrow::Int64Array&>(*ch);
            arrow::StringBuilder b;
            for (int64_t i = 0; i < ix.length(); ++i) {
                int64_t v = ix.Value(i);
                if (ix.IsNull(i) || v < 0 || v >= lab.length() || lab.IsNull(v))
                    (void)b.AppendNull();
                else
                    (void)b.Append(lab.GetView(v));
            }
            std::shared_ptr<arrow::Array> a;
            (void)b.Finish(&a);
            out.push_back(a);
        }
        return std::make_shared<arrow::ChunkedArray>(out, arrow::utf8());
    }

public:
    static std::string open(const TenxDirFiles& files, const Config& cfg,
                            std::unique_ptr<TabularSource>* out);

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    std::string tab_label() const override { return "matrix"; }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        const int n_feat = (int)feat_labels_.size();
        std::vector<int> inner_req;
        bool want_labels = false;
        for (int c : col_indices) {
            if (c < n_inner_) inner_req.push_back(c); else want_labels = true;
        }
        if (want_labels)
            for (int rc : {0, 1})
                if (std::find(inner_req.begin(), inner_req.end(), rc) == inner_req.end())
                    inner_req.push_back(rc);
        std::shared_ptr<arrow::Table> in_tbl;
        ARROW_RETURN_NOT_OK(inner_->read_chunk(i, inner_req, &in_tbl));
        if (!in_tbl) { *out = nullptr; return arrow::Status::OK(); }
        auto pos_of = [&](int c) {
            for (size_t k = 0; k < inner_req.size(); ++k) if (inner_req[k] == c) return (int)k;
            return -1;
        };
        const int feat_axis = rows_are_features_ ? 0 : 1;
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int c : col_indices) {
            fields.push_back(schema_->field(c));
            if (c < n_inner_) { cols.push_back(in_tbl->column(pos_of(c))); continue; }
            int k = c - n_inner_;
            if (k < n_feat)
                cols.push_back(gather(*feat_labels_[(size_t)k], *in_tbl->column(pos_of(feat_axis))));
            else
                cols.push_back(gather(*barcode_label_, *in_tbl->column(pos_of(1 - feat_axis))));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, in_tbl->num_rows());
        return arrow::Status::OK();
    }

    int64_t total_rows()      const override { return inner_->total_rows(); }
    int     num_chunks()      const override { return inner_->num_chunks(); }
    ChunkMeta chunk_meta(int i) const override { return inner_->chunk_meta(i); }
    void    ensure(int i)           override { inner_->ensure(i); }
    void    set_retain_all(bool b)  override { inner_->set_retain_all(b); }
    bool    evicted_any()     const override { return inner_->evicted_any(); }
    arrow::Status read_status() const override { return inner_->read_status(); }
    const std::string& path() const override { return inner_->path(); }
    std::string footer() const override { return inner_->footer() + "  |  " + note_; }

    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        std::vector<std::unique_ptr<TabularSource>> out;
        out.push_back(std::make_unique<Sidecar>(
            features_, files_.features, files_.v2 ? "genes" : "features",
            "10x Genomics " + std::string(files_.v2 ? "genes" : "features") + ": " +
                std::to_string(features_->num_rows()) + " rows"));
        out.push_back(std::make_unique<Sidecar>(
            barcodes_, files_.barcodes, "barcodes",
            "10x Genomics barcodes: " + std::to_string(barcodes_->num_rows()) + " rows"));
        return out;
    }
};

// Which 10x Genomics matrix-directory sidecar `path` is, by basename (any
// directory; optional .gz / .zst / .zstd): 1 = barcodes.tsv, 2 = features.tsv
// (Cell Ranger v3+: id, name, feature_type[, chrom, start, end]), 3 = genes.tsv
// (Cell Ranger v2: id, name). 0 = neither.
static int tenx_sidecar_kind(const std::string& path) {
    std::string b = std::filesystem::path(path).filename().string();
    for (char& c : b) c = (char)std::tolower((unsigned char)c);
    for (const char* sfx : {".gz", ".zstd", ".zst"}) {
        size_t n = std::strlen(sfx);
        if (b.size() > n && b.compare(b.size() - n, n, sfx) == 0) { b.resize(b.size() - n); break; }
    }
    if (b == "barcodes.tsv") return 1;
    if (b == "features.tsv") return 2;
    if (b == "genes.tsv")    return 3;
    return 0;
}

// PLINK files. The genotypes (.bed, PLINK 1; .pgen, PLINK 2) are binary and
// vv does not decode them; their variant and sample tables are text:
//   .bim   CHROM ID CM POS ALT REF          no header (allele 1 is ALT to plink2)
//   .fam   FID IID PAT MAT SEX PHENO1       no header
//   .pvar  a VCF-like #CHROM header line; without one, .bim column order
//   .psam  a #FID / #IID header line; without one, .fam column order
enum class PlinkTable { None, Bim, Fam, Pvar, Psam };
static PlinkTable plink_table_kind(const std::string& det) {
    auto is = [&](const char* ext) {
        return fends_ci(det, ext) || fends_ci(det, (std::string(ext) + ".gz").c_str());
    };
    if (is(".bim"))  return PlinkTable::Bim;
    if (is(".fam"))  return PlinkTable::Fam;
    if (is(".pvar")) return PlinkTable::Pvar;
    if (is(".psam")) return PlinkTable::Psam;
    return PlinkTable::None;
}

// BLAST / DIAMOND tabular output (-outfmt 6, DIAMOND's default format): no
// header row; the standard 12 columns are named when the first line fits them
// (a custom -outfmt "6 ..." can hold any fields, in any order).
static bool is_blast_tabular(const std::string& det) {
    for (const char* ext : {".m8", ".blast6", ".outfmt6"})
        if (fends_ci(det, ext) || fends_ci(det, (std::string(ext) + ".gz").c_str())) return true;
    return false;
}
static const std::vector<std::string> kBlastColumns = {
    "qseqid", "sseqid", "pident", "length", "mismatch", "gapopen",
    "qstart", "qend", "sstart", "send", "evalue", "bitscore"};
// The first line has the standard layout: percent identity, then six integers
// (length, mismatches, gap opens, query and subject start / end — two of which
// may be given as floats by some tools), e-value and bit score.
static bool blast_standard_layout(const std::string& line) {
    std::vector<std::string> f;
    split_delimited_line(line, '\t', &f);
    if (f.size() < 12) return false;
    auto num = [](const std::string& s, bool integer) {
        if (s.empty()) return false;
        char* end = nullptr;
        if (integer) { (void)std::strtoll(s.c_str(), &end, 10); }
        else         { (void)std::strtod(s.c_str(), &end); }
        return end && *end == '\0';
    };
    if (!num(f[2], false) || !num(f[10], false) || !num(f[11], false)) return false;
    for (int i = 3; i <= 9; ++i) if (!num(f[(size_t)i], true)) return false;
    return true;
}

// The TSV layout a file's extension names (.gz allowed): .bedpe, .pairs
// (4DN), .gct (GenePattern), .maf (mutation annotation format).
static TsvDialect tsv_dialect_of(const std::string& det) {
    auto is = [&](const char* ext) {
        return fends_ci(det, ext) || fends_ci(det, (std::string(ext) + ".gz").c_str());
    };
    if (is(".bedpe")) return TsvDialect::Bedpe;
    if (is(".pairs")) return TsvDialect::Pairs;
    if (is(".gct"))   return TsvDialect::Gct;
    if (is(".maf"))   return TsvDialect::Maf;
    return TsvDialect::None;
}

// True when `path` starts with PLINK 1's .bed magic 0x6c 0x1b (then 0x01,
// variant-major, or 0x00, sample-major). A text BED cannot start with it:
// 0x1b is ESC.
static bool is_plink_bed(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    unsigned char m[2] = {0, 0};
    return f.read(reinterpret_cast<char*>(m), 2) && m[0] == 0x6c && m[1] == 0x1b;
}

// The refusal for a PLINK genotype file, with the plink2 command that
// exports it to a VCF vv reads and the sidecars that vv reads as they are.
static std::string plink_genotype_refusal(const std::string& path, bool pgen) {
    std::filesystem::path p(path);
    std::string stem = (p.parent_path() / p.stem()).string();
    if (stem.find_first_of(" '\"$`\\") != std::string::npos) stem = "'" + stem + "'";
    const std::string name = p.stem().string();
    return std::string("a PLINK ") + (pgen ? "2 .pgen" : "1 .bed") + " genotype file (binary), " +
           (pgen ? "not a table" : "not a BED interval file") +
           "; vv does not decode genotypes. Export them with\n  plink2 " +
           (pgen ? "--pfile " : "--bfile ") + stem + " --export vcf bgz --out " + stem +
           "\nand open the .vcf.gz. Its variants (" + name + (pgen ? ".pvar" : ".bim") +
           ") and samples (" + name + (pgen ? ".psam" : ".fam") + ") are tables vv opens.";
}

std::string TenxDirSource::load_table(const std::string& path, const Config& cfg,
                                      std::shared_ptr<arrow::Table>* out) {
    std::unique_ptr<TabularSource> src;
    std::string e = open_source_dispatch(path, cfg, &src);
    if (!e.empty()) return e;
    src->set_retain_all(true);
    std::vector<int> all((size_t)src->schema()->num_fields());
    std::iota(all.begin(), all.end(), 0);
    std::vector<std::shared_ptr<arrow::Table>> parts;
    for (int c = 0;; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        std::shared_ptr<arrow::Table> t;
        arrow::Status st = src->read_chunk(c, all, &t);
        if (!st.ok()) return "'" + path + "': " + st.ToString();
        if (t && t->num_rows() > 0) parts.push_back(std::move(t));
    }
    if (!src->read_status().ok()) return "'" + path + "': " + src->read_status().ToString();
    if (parts.empty()) return "'" + path + "' is empty";
    auto cat = arrow::ConcatenateTables(parts);
    if (!cat.ok()) return "'" + path + "': " + cat.status().ToString();
    auto comb = (*cat)->CombineChunks();
    if (!comb.ok()) return "'" + path + "': " + comb.status().ToString();
    *out = *comb;
    return "";
}

std::string TenxDirSource::open(const TenxDirFiles& files, const Config& cfg,
                                std::unique_ptr<TabularSource>* out) {
    // Sidecars and matrix are read with the defaults that make them parse:
    // header detection on (the sidecars are recognised by name), no input
    // delimiter override, no region.
    Config sub = cfg;
    sub.header = HeaderMode::Auto;
    sub.in_delimiter = 0;
    sub.region.clear();
    auto self = std::unique_ptr<TenxDirSource>(new TenxDirSource());
    self->files_ = files;
    std::string e = load_table(files.features, sub, &self->features_);
    if (e.empty()) e = load_table(files.barcodes, sub, &self->barcodes_);
    if (!e.empty()) return e;
    e = open_source_dispatch(files.matrix, sub, &self->inner_);
    if (!e.empty()) return e;

    int64_t mr = 0, mc = 0;
    if (!delimited_mtx_shape(self->inner_.get(), &mr, &mc))
        return "'" + files.matrix + "' did not open as a MatrixMarket matrix";
    const int64_t nf = self->features_->num_rows(), nb = self->barcodes_->num_rows();
    if (mr == nf && mc == nb)      self->rows_are_features_ = true;
    else if (mr == nb && mc == nf) self->rows_are_features_ = false;
    else
        return "10x matrix directory does not fit together: " +
               std::filesystem::path(files.matrix).filename().string() + " is " +
               std::to_string(mr) + " x " + std::to_string(mc) + ", but " +
               std::filesystem::path(files.features).filename().string() + " has " +
               std::to_string(nf) + " rows and " +
               std::filesystem::path(files.barcodes).filename().string() + " has " +
               std::to_string(nb);

    static const char* kFeatNames[] = {"feature_id", "feature_name", "feature_type"};
    const int nfc = std::min(self->features_->num_columns(), files.v2 ? 2 : 3);
    for (int k = 0; k < nfc; ++k)
        self->feat_labels_.push_back(as_strings(self->features_->column(k)));
    self->barcode_label_ = as_strings(self->barcodes_->column(0));

    auto in_schema = self->inner_->schema();
    self->n_inner_ = in_schema->num_fields();
    arrow::FieldVector fields = in_schema->fields();
    for (int k = 0; k < nfc; ++k) fields.push_back(arrow::field(kFeatNames[k], arrow::utf8()));
    fields.push_back(arrow::field("barcode", arrow::utf8()));
    self->schema_ = arrow::schema(fields);
    self->note_ = std::string("10x matrix, ") +
                  (self->rows_are_features_ ? "features × barcodes" : "barcodes × features") +
                  "; labels from " + std::filesystem::path(files.features).filename().string() +
                  " and " + std::filesystem::path(files.barcodes).filename().string();
    *out = std::move(self);
    return "";
}

std::string open_source_dispatch(const std::string& path, const Config& cfg,
                         std::unique_ptr<TabularSource>* out) {
    // ── Determine file kind ──────────────────────────────────────────────────
    bool        is_parquet = false;
    DelimKind   dk         = DelimKind::TSV;

    // A trailing .zst / .zstd / .bz2 / .xz is a compression wrapper, not a
    // format: strip it so a foo.csv.xz dispatches like foo.csv (the readers
    // decode by magic bytes). A .gz wrapper stays and is matched explicitly
    // below.
    std::string det = path;
    for (const char* z : {".zstd", ".zst", ".bz2", ".xz"})
        if (fends_ci(det, z)) { det.resize(det.size() - std::strlen(z)); break; }
    // `.bgz` (gnomAD's *.vcf.bgz) is the bgzip suffix: read it as `.gz`.
    if (fends_ci(det, ".bgz")) det = det.substr(0, det.size() - 4) + ".gz";

    // --tags names aux-tag columns and applies only to the alignment formats
    // read through htslib (BamSource). Reject it elsewhere rather than let it be
    // a silent no-op, and reject the pileup combination (pileup rows are not
    // alignment records).
    if (!cfg.bam_tags.empty()) {
        bool is_aln = fends_ci(path, ".bam") || fends_ci(path, ".cram") ||
                      fends_ci(path, ".sam") || fends_ci(det, ".paf") || fends_ci(det, ".paf.gz");
        if (!is_aln)
            return "'" + path + "': --tags applies to BAM/CRAM/SAM alignment "
                   "files and PAF (a record's optional NM/AS/tp/… tags)";
        if (cfg.pileup)
            return "--tags cannot be combined with --pileup — pileup rows are "
                   "per-base counts, not alignment records";
    }

    // A directory is a dataset: concatenate the data files under it. Checked
    // before the format ladder (a directory has no extension to match) and
    // before --text (which has nothing to sniff on a directory).
    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            if (!cfg.region.empty())
                return "'" + path + "': -r/--region is not supported on a "
                       "directory dataset";
            // A 10x Genomics / STARsolo matrix directory is one matrix with
            // its labels, not a dataset of like-shaped files.
            TenxDirFiles tenx;
            if (find_tenx_dir(path, &tenx))
                return TenxDirSource::open(tenx, cfg, out);
            std::unique_ptr<DatasetSource> src;
            std::string err = DatasetSource::open(path, cfg, &src);
            if (!err.empty()) return err;
            *out = std::move(src);
            return "";
        }
    }

    // --text: read it as plain text whatever the extension says. The escape
    // hatch for a textual-but-tabular file — `vv --text notes.md` shows the
    // markdown source, `vv --text data.csv` shows the raw lines. Still
    // sniffed, so `vv --text foo.bam` is refused rather than dumped.
    // (stdin is handled inside the `-` branch below: it has no path to
    // sniff and its stream is already open.)
    if (cfg.force_text && path != "-") return open_text(path, cfg, out);

    // -d/--in-delimiter: read the file as delimited text with the given field
    // separator, overriding the extension. Turns a space-, semicolon- or
    // pipe-separated file (e.g. R's default write.table() .txt) into a table.
    // A comma routes through the CSV path, anything else through TSV — both run
    // the same header auto-detection; only the separator char differs. Trusts
    // the user like the .csv/.tsv extensions do (no binary sniff). Directory
    // datasets and --text are handled above and still win.
    if (cfg.in_delimiter != 0 && path != "-") {
        DelimKind dk2 = (cfg.in_delimiter == ',') ? DelimKind::CSV : DelimKind::TSV;
        std::unique_ptr<TabularSource> src;
        std::string err = open_delimited_source(path, dk2, cfg.region, &src,
                                                cfg.in_delimiter, cfg.header);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    }

    // ── Stdin (`-`) and pipes (`vv <(zcat x.gz)`, FIFOs) ─────────────────────
    // A pipe cannot seek. Text (plain, gzip, zstd) is read as it streams in;
    // a binary format is copied to a temporary file first (spool_pipe).
    if (path == "-" || path_is_pipe(path)) {
        int fd = STDIN_FILENO;
        if (path == "-") {
            if (isatty(STDIN_FILENO))
                return "Refusing to read from a terminal on stdin. "
                       "Did you mean to pipe data in (`cat foo.tsv | vv -`)?";
        } else {
            fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) return "Cannot open '" + path + "': " + std::strerror(errno);
            if (isatty(fd)) { ::close(fd); return "'" + path + "' is a terminal, not a file"; }
        }

        std::string sniff;
        std::string err = sniff_fd(fd, 64, &sniff);
        if (!err.empty()) return err;
        auto starts_with = [&](const char* m, size_t l) {
            return sniff.size() >= l && std::memcmp(sniff.data(), m, l) == 0;
        };

        // An Arrow IPC stream needs no seeking: read it as it arrives.
        if (looks_like_ipc_stream((const uint8_t*)sniff.data(), sniff.size())) {
            std::shared_ptr<arrow::io::InputStream> in = std::make_shared<PrependInputStream>(
                std::move(sniff), std::make_shared<FdInputStream>(fd));
            std::unique_ptr<TabularSource> src;
            std::string e = open_ipc_stream(path, std::move(in), &src);
            if (!e.empty()) return e;
            *out = std::move(src);
            return "";
        }
        const std::string bin_ext = pipe_binary_ext(sniff);
        if (!bin_ext.empty()) {
            std::string tmp;
            int64_t bytes = 0;
            std::string serr = spool_pipe(fd, sniff, bin_ext, &tmp, &bytes);
            if (fd != STDIN_FILENO) ::close(fd);
            if (!serr.empty()) return serr;
            std::fprintf(stderr, "vv: %s: binary input needs random access; copied it to %s "
                         "(%s), removed when vv exits\n",
                         path.c_str(), tmp.c_str(), human_bytes(bytes).c_str());
            return open_source_dispatch(tmp, cfg, out);
        }

        // Wrap the pipe as a sequential-only InputStream that reads via
        // read(2). Avoid arrow::io::StdinStream — it uses std::cin which
        // conflicts with the preceding raw read(2) sniff.
        std::shared_ptr<arrow::io::InputStream> input =
            std::make_shared<FdInputStream>(fd);

        // Compressed stdin: gzip, zstandard, bzip2 or xz, by magic. BGZF (BAM /
        // BCF) shares the gzip magic but was already rejected above by its
        // 1f 8b 08 04 header.
        const StreamCodec comp = sniff_codec(reinterpret_cast<const uint8_t*>(sniff.data()), sniff.size());
        bool is_gz = (comp != StreamCodec::None);
        // Reattach the sniffed bytes BEFORE the decompressor sees the stream.
        input = std::make_shared<PrependInputStream>(std::move(sniff), input);
        if (auto e = decode_stream(comp, input, &input); !e.empty()) return e;

        // Binary refusal, at the second entry point. Piped binary used to
        // reach Arrow's CSV reader, which echoed the raw bytes back inside a
        // parse error ("Expected 2 columns, got 1: Mu\xef\xbf\xbdm…") —
        // control characters and all, straight at the user's terminal.
        // Sniffing here rather than on the raw fd covers gzip'd input too.
        {
            std::string head(8192, '\0');
            auto got = input->Read(8192, head.data());
            if (!got.ok()) return got.status().ToString();
            head.resize((size_t)*got);
            TextSniffResult tr = sniff_text(head.data(), head.size());
            if (tr != TextSniffResult::Text)
                return text_binary_error(path == "-" ? "stdin" : path, tr);
            // A genomics text format read as its file reader would read it,
            // not as TSV — unless --text or -d asks for something else.
            if (!cfg.force_text && cfg.in_delimiter == 0) {
                const std::string fmt = sniff_text_format(head);
                input = std::make_shared<PrependInputStream>(std::move(head), input);
                if (fmt == "vcf" || fmt == "gff" || fmt == "sam") {
                    const DelimKind k = fmt == "vcf" ? DelimKind::VCF
                                      : fmt == "gff" ? DelimKind::GFF : DelimKind::SAM;
                    std::unique_ptr<TabularSource> src;
                    std::string e = open_delimited_stream(
                        std::move(input), path, k, is_gz, cfg.region, &src);
                    if (!e.empty()) return e;
                    *out = std::move(src);
                    return "";
                }
                if (fmt == "json") {
                    // As a document (the CLI's pretty print / viewer), or as
                    // records for the table modes and the GUI.
                    if (cfg.json_document) {
                        make_json_stream_source(path, std::move(input), out);
                        return "";
                    }
                    std::unique_ptr<TabularSource> src;
                    std::string e = open_json_stream(path, std::move(input), comp, &src);
                    if (!e.empty()) return e;
                    *out = std::move(src);
                    return "";
                }
                if (fmt == "fasta" || fmt == "fastq") {
                    // The FASTA / FASTQ reader opens a file: copy the text to one.
                    std::string tmp;
                    int64_t bytes = 0;
                    std::string serr = spool_stream(input, fmt == "fasta" ? ".fa" : ".fq", &tmp, &bytes);
                    if (!serr.empty()) return serr;
                    return open_source_dispatch(tmp, cfg, out);
                }
            } else {
                input = std::make_shared<PrependInputStream>(std::move(head), input);
            }
        }

        // --text on stdin: `cat server.log | vv --text -` must behave like
        // `vv server.log`. Without this the flag was a silent no-op and the
        // stream went to the CSV reader, which promotes line 1 to a column
        // header and drops it from the data — the exact lossy rendering the
        // plain-text feature exists to remove. TextSource reads any
        // InputStream, so the already-sniffed, already-gunzipped stream goes
        // straight in.
        if (cfg.force_text) {
            std::unique_ptr<TabularSource> tsrc;
            std::string terr = open_text_stream(path, std::move(input), &tsrc);
            if (!terr.empty()) return terr;
            *out = std::move(tsrc);
            return "";
        }

        // Choose CSV/TSV from the user-provided flag (none → assume TSV; the
        // header-line auto-detect in DelimitedSource will catch obvious CSV).
        // -d/--in-delimiter overrides the field separator outright.
        DelimKind kind = DelimKind::TSV;
        if (cfg.in_delimiter == ',' ||
            (cfg.in_delimiter == 0 && cfg.delimiter == ',')) kind = DelimKind::CSV;
        std::unique_ptr<TabularSource> src;
        std::string e = open_delimited_stream(
            std::move(input), path, kind, /*is_gz=*/is_gz, cfg.region, &src,
            cfg.in_delimiter, cfg.header);
        if (!e.empty()) return e;
        *out = std::move(src);
        return "";
    }

    if (fends_ci(path, ".parquet")) {
        is_parquet = true;
    } else if (fends_ci(path, ".arrows") || (fends_ci(path, ".arrow") && file_is_ipc_stream(path))) {
        // The IPC stream format: .arrows, or a stream saved as .arrow.
        std::unique_ptr<TabularSource> src;
        std::string err = open_ipc_stream_file(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".arrow")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_ipc_source(path, false, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".feather")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_ipc_source(path, true, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".orc")) {
#if VV_HAVE_ORC
        std::unique_ptr<TabularSource> src;
        std::string err = open_orc_source(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
#else
        return "'" + path + "': vv was built without Apache ORC support "
               "(Arrow needs -DARROW_ORC=ON; rebuild Arrow and vv)";
#endif
        return "";
    } else if (fends_ci(path, ".bam") || fends_ci(path, ".cram") ||
               (fends_ci(path, ".sam") && !cfg.bam_tags.empty())) {
        // .sam normally reads through the delimited text reader; with --tags it
        // is routed here so htslib decodes the aux fields into typed columns.
        if (cfg.pileup) {
            // --pileup: walk the alignments through htslib's bam_plp engine
            // and emit mpileup-style per-base rows instead of alignment
            // records. Run the decoded view on top if --decode-pileup is
            // also set.
            std::unique_ptr<TabularSource> src;
            std::string err = open_bam_pileup_source(path, cfg, &src);
            if (!err.empty()) return err;
            if (cfg.decode_pileup) {
                std::unique_ptr<TabularSource> decoded;
                std::string err2 = decode_mpileup_to_memory(*src, path, &decoded);
                if (!err2.empty()) return err2;
                *out = std::move(decoded);
            } else {
                *out = std::move(src);
            }
            return "";
        }
        std::unique_ptr<TabularSource> src;
        std::string err = open_bam_source(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".bcf")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_bcf_source(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".bb") || fends_ci(path, ".bigBed") ||
               fends_ci(path, ".bw") || fends_ci(path, ".bigWig")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_big_source(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".2bit")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_twobit_source(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".sqlite")  || fends_ci(path, ".sqlite3") ||
               fends_ci(path, ".db")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_sqlite_source(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".xlsx") || fends_ci(path, ".xlsm")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_xlsx_source(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".ods") || fends_ci(path, ".fods")) {
        // .fods (flat ODF) is the .ods content.xml as a plain XML file.
        std::unique_ptr<TabularSource> src;
        std::string err = open_ods_source(path, &src, fends_ci(path, ".fods"));
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".h5ad") || fends_ci(path, ".h5") ||
               fends_ci(path, ".hdf5") || fends_ci(path, ".loom") ||
               // HDF5 underneath: MuData, Seurat's h5Seurat, Oxford Nanopore
               // FAST5, NetCDF-4.
               fends_ci(path, ".h5mu") || fends_ci(path, ".h5seurat") ||
               fends_ci(path, ".fast5") || fends_ci(path, ".nc") || fends_ci(path, ".nc4")) {
        // NetCDF-3 ("classic": CDF\x01 / \x02 / \x05) is not HDF5.
        if (fends_ci(path, ".nc") || fends_ci(path, ".nc4")) {
            char m[4] = {0, 0, 0, 0};
            std::ifstream f(path, std::ios::binary);
            f.read(m, 4);
            if (f.gcount() == 4 && m[0] == 'C' && m[1] == 'D' && m[2] == 'F')
                return "'" + path + "': NetCDF-3 (classic) is not supported; NetCDF-4 is "
                       "(convert with `nccopy -k nc4 " + path + " out.nc`)";
        }
        std::unique_ptr<TabularSource> src;
        // The 1000-row cap exists so opening a 10 GB .h5ad in the TUI does not
        // read 310k rows of obs up front. It must apply to THAT and nothing
        // else: every mode that produces a complete answer — a count, an
        // aggregate, an export, a delimited dump — has to see all the rows.
        //
        // Only --tsv/--csv used to escape it, so `--count` on a 310,385-row
        // obs answered "1000", `--unique` reported "of 1000", and
        // `--parquet out.parquet` wrote 1000 of 8563 rows. That last one is
        // data loss during a format conversion.
        //
        // Sparse / dense X and generic datasets stay capped regardless (see
        // build_table) — those are genuinely previews of a matrix.
        // A filter or a sort ranges over every row: neither the preview cap
        // nor -n may limit what is read (-n then cuts the result).
        const bool whole_frame = !cfg.filter_expr.empty() || !cfg.sort_col.empty();
        const bool df_preview_only =
            !cfg.delimiter && !cfg.count && !cfg.describe &&
            cfg.unique_cols.empty() && cfg.sample_n <= 0 &&
            !cfg.tail_rows_set && !cfg.json_array && !cfg.json_lines &&
            !cfg.md && cfg.parquet_out.empty() && cfg.arrow_out.empty() &&
            !whole_frame;
        int64_t df_cap = df_preview_only
            ? h5v::kDataFrameRowCap
            : ((cfg.head_rows_set && !whole_frame) ? (int64_t)cfg.head_rows : -1);
        std::string err = h5v::open_hdf5_source(path, &src, df_cap, cfg.matrix == "long");
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".npz")) {
        std::unique_ptr<TabularSource> src;
        std::string err = npz::open_npz_source(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".npy")) {
        std::unique_ptr<TabularSource> src;
        std::string err = npz::open_npy_source(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fastx_ext(path, det, {".fa", ".fasta", ".fna", ".faa", ".ffn", ".frn"})) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_fastx_source(path, /*is_fastq=*/false, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fastx_ext(path, det, {".fq", ".fastq"})) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_fastx_source(path, /*is_fastq=*/true, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(det, ".json")   || fends_ci(det, ".json.gz")   ||
               fends_ci(det, ".ndjson") || fends_ci(det, ".ndjson.gz") ||
               fends_ci(det, ".jsonl")  || fends_ci(det, ".jsonl.gz")  ||
               fends_ci(det, ".geojson") || fends_ci(det, ".geojson.gz") ||
               fends_ci(det, ".ipynb")  || fends_ci(det, ".ipynb.gz")   ||
               fends_ci(det, ".har")    || fends_ci(det, ".har.gz")) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_json_source(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".md")    || fends_ci(path, ".markdown") ||
               fends_ci(path, ".mdown") || fends_ci(path, ".mkd")) {
        // The CLI renders markdown in main() before reaching open_source(); a
        // caller that gets here (the GUI, the KDE plugins) is a grid viewer, so
        // surface the file's GFM tables — each a tab. A markdown file with no
        // table (or a parse error) falls through to the text reader below, which
        // shows its source, as it did before.
        md::MarkdownDoc doc;
        std::string merr = md::parse_markdown_file(path, /*term_w=*/80, &doc);
        if (merr.empty() && !doc.tables.empty()) {
            *out = std::make_unique<MarkdownTablesSource>(
                doc.tables, doc.table_captions, path, 0);
            return "";
        }
        return open_text(path, cfg, out);
    } else if (fends_ci(det, ".vcf")   || fends_ci(det, ".vcf.gz")) {
        dk = DelimKind::VCF;
    } else if (fends_ci(det, ".gff")   || fends_ci(det, ".gff.gz")  ||
               fends_ci(det, ".gff3")  || fends_ci(det, ".gff3.gz") ||
               fends_ci(det, ".gtf")   || fends_ci(det, ".gtf.gz")) {
        dk = DelimKind::GFF;
    } else if (fends_ci(det, ".sam")) {
        dk = DelimKind::SAM;
    } else if (fends_ci(det, ".mtx") || fends_ci(det, ".mtx.gz")) {
        if (!cfg.region.empty())
            return "-r/--region does not apply to a MatrixMarket file";
        dk = DelimKind::Mtx;
    } else if (fends_ci(det, ".paf") || fends_ci(det, ".paf.gz")) {
        dk = DelimKind::PAF;
    } else if (fends_ci(det, ".pgen")) {
        return plink_genotype_refusal(path, /*pgen=*/true);
    } else if (fends_ci(det, ".bed") && is_plink_bed(path)) {
        return plink_genotype_refusal(path, /*pgen=*/false);
    } else if (plink_table_kind(det) != PlinkTable::None || is_blast_tabular(det)) {
        dk = DelimKind::TSV;
    } else if (tsv_dialect_of(det) != TsvDialect::None) {
        // A UCSC multiple-alignment file shares .maf with the mutation
        // annotation format; it starts with "##maf" and is not a table.
        if (tsv_dialect_of(det) == TsvDialect::Maf &&
            delimited_first_line_after_meta_raw(path).rfind("##maf", 0) == 0)
            return open_text(path, cfg, out);
        dk = DelimKind::TSV;
    } else if (fends_ci(path, ".rds") || fends_ci(path, ".rdata") || fends_ci(path, ".rda")) {
        return open_rdata_source(path, out);
    } else if (fends_ci(det, ".wig") || fends_ci(det, ".wig.gz")) {
        return open_wig_source(path, cfg, out);
    } else if (fends_ci(det, ".bed")        || fends_ci(det, ".bed.gz")
            || fends_ci(det, ".narrowPeak") || fends_ci(det, ".narrowPeak.gz")
            || fends_ci(det, ".broadPeak")  || fends_ci(det, ".broadPeak.gz")
            || fends_ci(det, ".gappedPeak") || fends_ci(det, ".gappedPeak.gz")
            || fends_ci(det, ".bedGraph")   || fends_ci(det, ".bedGraph.gz")
            || fends_ci(det, ".bg")         || fends_ci(det, ".bg.gz")
            || fends_ci(det, ".tagAlign")   || fends_ci(det, ".tagAlign.gz")) {
        dk = DelimKind::BED;
    } else if (fends_ci(det, ".pileup")  || fends_ci(det, ".pileup.gz")
            || fends_ci(det, ".mpileup") || fends_ci(det, ".mpileup.gz")
            || fends_ci(det, ".pile")    || fends_ci(det, ".pile.gz")) {
        dk = DelimKind::Mpileup;
    } else if (fends_ci(det, ".tsv")   || fends_ci(det, ".tsv.gz")) {
        dk = DelimKind::TSV;
    } else if (fends_ci(det, ".csv")   || fends_ci(det, ".csv.gz")) {
        dk = DelimKind::CSV;
    } else if (text_ext(det)) {
        // Known text extension. Still sniffed: a `.log` holding a core dump
        // is binary whatever it is called, and the sniff is what keeps the
        // "vv never dumps binary to your terminal" promise true.
        return open_text(path, cfg, out);
    } else {
        // Unknown extension: only accept it if the magic bytes positively
        // identify a known binary format. The previous fallback ran a
        // tabs-vs-commas count on the first 4 KiB and routed everything
        // else through the CSV reader — which produced cryptic parse
        // errors on binary files (`Expected 1 columns, got 2: …<garbage>`)
        // when the user's file was e.g. `.bigwig` (case mismatch) or a
        // format we just don't know.
        auto rf = arrow::io::ReadableFile::Open(path);
        if (!rf.ok()) return "Cannot open '" + path + "': " + rf.status().ToString();
        auto buf = rf.ValueOrDie()->Read(8);
        (void)rf.ValueOrDie()->Close();
        bool is_ipc = false, is_feather = false;
        if (buf.ok() && (*buf)->size() >= 4) {
            const uint8_t* m = (*buf)->data();
            is_parquet = ((*buf)->size() >= 4 &&
                          m[0]=='P' && m[1]=='A' && m[2]=='R' && m[3]=='1');
            is_ipc     = ((*buf)->size() >= 8 &&
                          m[0]=='A' && m[1]=='R' && m[2]=='R' && m[3]=='O' &&
                          m[4]=='W' && m[5]=='1' && m[6]==0   && m[7]==0);
            is_feather = ((*buf)->size() >= 4 &&
                          m[0]=='F' && m[1]=='E' && m[2]=='A' && m[3]=='1');
        }
        if (buf.ok() && looks_like_ipc_stream((*buf)->data(), (size_t)(*buf)->size())) {
            std::unique_ptr<TabularSource> src;
            std::string err = open_ipc_stream_file(path, &src);
            if (!err.empty()) return err;
            *out = std::move(src);
            return "";
        }
        if (is_ipc || is_feather) {
            std::unique_ptr<TabularSource> src;
            std::string err = open_ipc_source(path, is_feather, &src);
            if (!err.empty()) return err;
            *out = std::move(src);
            return "";
        }
        // LociSSD v4 "colblock": data magic "LSB1". (v3 .lociss is Parquet → the
        // PAR1 sniff above; dispatch is by magic, per the spec.)
        if (buf.ok() && (*buf)->size() >= 4) {
            const uint8_t* mm = (*buf)->data();
            if (mm[0]=='L' && mm[1]=='S' && mm[2]=='B' && mm[3]=='1') {
                std::unique_ptr<TabularSource> src;
                std::string err = open_lociss_v4_source(path, cfg, &src);
                if (!err.empty()) return err;
                *out = std::move(src);
                return "";
            }
        }
        if (!is_parquet) {
            // No known format claims it. Before giving up, look at the
            // content: if it is text, show it as text. Binary is refused —
            // deliberately unlike less, which offers to dump it anyway.
            return open_text(path, cfg, out, !has_no_extension(path));
        }
    }

    // ── Open appropriate source ───────────────────────────────────────────────
    if (is_parquet) {
        std::unique_ptr<TabularSource> src;
        std::string err = open_parquet_source(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    }

    // 10x Genomics sidecars (Cell Ranger / STARsolo matrix directories) have no
    // header row. Their data is all text, so header detection keeps row 0 as the
    // header and the first barcode / feature would vanish into the column name.
    // Read them headerless and name the columns, unless --header was given.
    const int tenx = (dk == DelimKind::TSV && cfg.header == HeaderMode::Auto)
                         ? tenx_sidecar_kind(path) : 0;
    // PLINK tables: .bim / .fam have no header row, and neither has a .pvar /
    // .psam whose first line (after ## meta lines) is not a # header. PLINK 1
    // writes .fam space-separated, so the separator follows the first line.
    const PlinkTable plink = dk == DelimKind::TSV ? plink_table_kind(det) : PlinkTable::None;
    std::vector<std::string> plink_names;
    std::string plink_note;
    char delim_override = 0;
    if (plink != PlinkTable::None) {
        const std::string first = delimited_first_line_after_meta(path);
        const bool headed = (plink == PlinkTable::Pvar || plink == PlinkTable::Psam) &&
                            !first.empty() && first[0] == '#';
        if (!headed) {
            delim_override = first.find('\t') != std::string::npos ? '\t' : ' ';
            const bool bim_order = plink == PlinkTable::Bim || plink == PlinkTable::Pvar;
            if (bim_order) {
                std::vector<std::string> f;
                split_delimited_line(first, delim_override, &f);
                plink_names = f.size() == 5
                    ? std::vector<std::string>{"CHROM", "ID", "POS", "ALT", "REF"}
                    : std::vector<std::string>{"CHROM", "ID", "CM", "POS", "ALT", "REF"};
                plink_note = std::string("PLINK ") + (plink == PlinkTable::Bim ? ".bim" : ".pvar") +
                             " (no header row; allele 1 is ALT, allele 2 REF, as plink2 reads them)";
            } else {
                plink_names = {"FID", "IID", "PAT", "MAT", "SEX", "PHENO1"};
                plink_note = std::string("PLINK ") + (plink == PlinkTable::Fam ? ".fam" : ".psam") +
                             " (no header row)";
            }
        }
    }
    // BLAST tabular: no header row either; the column names when the layout
    // is the standard one (they go through the same renaming as PLINK's).
    const bool blast = dk == DelimKind::TSV && is_blast_tabular(det);
    if (blast) {
        if (blast_standard_layout(delimited_first_line_after_meta(path))) {
            plink_names = kBlastColumns;
            plink_note = "BLAST tabular (-outfmt 6; no header row)";
        } else {
            plink_note = "BLAST tabular with custom columns (no header row; names unknown)";
        }
    }
    const bool named_headerless = !plink_names.empty() || blast;
    const bool headerless = tenx || (named_headerless && cfg.header == HeaderMode::Auto);
    const TsvDialect dialect = dk != DelimKind::TSV ? TsvDialect::None
                             : blast && !plink_names.empty() ? TsvDialect::Blast
                             : tsv_dialect_of(det);
    std::vector<std::string> paf_tags;
    if (dk == DelimKind::PAF && !cfg.bam_tags.empty()) {
        std::string terr;
        paf_tags = parse_bam_tag_list(cfg.bam_tags, &terr);
        if (!terr.empty()) return terr;
    }
    std::unique_ptr<TabularSource> src;
    std::string err = open_delimited_source(path, dk, cfg.region, &src, delim_override,
                                            headerless ? HeaderMode::Off : cfg.header, dialect,
                                            paf_tags);
    if (!err.empty()) return err;
    // BEDPE has no header row unless it starts with a "#chrom1 …" line (read
    // as the header); without one, name the bedtools columns.
    if (dialect == TsvDialect::Bedpe &&
        static_cast<TabularSource&>(*src).schema()->num_fields() > 0 &&
        static_cast<TabularSource&>(*src).schema()->field(0)->name() == "f0")
        delimited_apply_column_names(*src, {"chrom1", "start1", "end1", "chrom2", "start2", "end2",
                                            "name", "score", "strand1", "strand2"},
                                     "BEDPE (no header row)");
    if (tenx) delimited_apply_tenx_sidecar(*src, tenx);
    if (headerless && named_headerless) delimited_apply_column_names(*src, plink_names, plink_note);
    // ENCODE peak-family variants ride on top of DelimKind::BED — the
    // dispatch detected them by extension; apply variant-specific naming
    // now that the schema is materialised.
    if (dk == DelimKind::BED) {
        BedVariant bv = BedVariant::None;
        if      (fends_ci(path, ".narrowPeak") || fends_ci(path, ".narrowPeak.gz")) bv = BedVariant::NarrowPeak;
        else if (fends_ci(path, ".broadPeak")  || fends_ci(path, ".broadPeak.gz"))  bv = BedVariant::BroadPeak;
        else if (fends_ci(path, ".gappedPeak") || fends_ci(path, ".gappedPeak.gz")) bv = BedVariant::GappedPeak;
        else if (fends_ci(path, ".bedGraph")   || fends_ci(path, ".bedGraph.gz")
              || fends_ci(path, ".bg")         || fends_ci(path, ".bg.gz"))         bv = BedVariant::BedGraph;
        else if (fends_ci(path, ".tagAlign")   || fends_ci(path, ".tagAlign.gz"))   bv = BedVariant::TagAlign;
        if (bv != BedVariant::None) delimited_apply_bed_variant(*src, bv);
    }
    // --decode-pileup: materialise the typed allele-count view from the
    // underlying mpileup source. The original streaming source is consumed
    // in full, decoded row by row, and replaced with a MemoryTableSource
    // over the resulting Arrow table.
    if (dk == DelimKind::Mpileup && cfg.decode_pileup) {
        std::unique_ptr<TabularSource> decoded;
        std::string err2 = decode_mpileup_to_memory(*src, path, &decoded);
        if (!err2.empty()) return err2;
        *out = std::move(decoded);
        return "";
    }
    *out = std::move(src);
    return "";
}
