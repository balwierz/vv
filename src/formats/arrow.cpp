// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Arrow IPC / Feather, IPC streams and Apache ORC.

#include "internal.hpp"

// ── Arrow IPC / Feather source ────────────────────────────────────────────────

class IpcSource : public TabularSource {
    std::string                                       path_;
    bool                                              is_feather_ = false;
    std::shared_ptr<arrow::Schema>                    schema_;
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> rdr_;       // Arrow IPC only
    int                                               num_record_batches_ = 0; // Arrow IPC only
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>                      batch_first_row_;
    mutable int64_t                                   total_rows_ = 0;
    mutable bool                                      all_read_   = false;
    mutable arrow::Status                             read_status_;  // sticky error

    static constexpr int64_t BATCH_ROWS = 65536;

    // Slice a Table into BATCH_ROWS-sized RecordBatches and append to batches_
    // (Feather v1 path: the reader returns a single Table).
    arrow::Status ingest_table(const std::shared_ptr<arrow::Table>& table) {
        arrow::TableBatchReader rdr(*table);
        rdr.set_chunksize(BATCH_ROWS);
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            ARROW_RETURN_NOT_OK(rdr.ReadNext(&batch));
            if (!batch) break;
            batch_first_row_.push_back(total_rows_);
            total_rows_ += batch->num_rows();
            batches_.push_back(std::move(batch));
        }
        all_read_ = true;
        return arrow::Status::OK();
    }

    // Decode the next not-yet-loaded record batch (Arrow IPC only).
    arrow::Status load_next_ipc() const {
        if (all_read_) return arrow::Status::OK();
        int i = (int)batches_.size();
        if (i >= num_record_batches_) { all_read_ = true; return arrow::Status::OK(); }
        auto maybe_b = rdr_->ReadRecordBatch(i);
        if (!maybe_b.ok()) {
            // A batch that fails to decode (truncated/corrupt IPC) must not
            // leave ensure() spinning: record the error stickily and stop.
            read_status_ = maybe_b.status();
            all_read_ = true;
            return maybe_b.status();
        }
        auto b = maybe_b.ValueOrDie();
        batch_first_row_.push_back(total_rows_);
        total_rows_ += b->num_rows();
        batches_.push_back(std::move(b));
        if ((int)batches_.size() >= num_record_batches_) all_read_ = true;
        return arrow::Status::OK();
    }

public:
    static std::string open(const std::string& path, bool is_feather,
                             std::unique_ptr<IpcSource>* out) {
        auto self = std::make_unique<IpcSource>();
        self->path_       = path;
        self->is_feather_ = is_feather;

        auto maybe_file = arrow::io::ReadableFile::Open(path);
        if (!maybe_file.ok())
            return "Cannot open '" + path + "': " + maybe_file.status().ToString();
        auto file = maybe_file.ValueOrDie();

        if (is_feather) {
            auto maybe_rdr = arrow::ipc::feather::Reader::Open(file);
            if (!maybe_rdr.ok())
                return "Not a valid Feather file: " + maybe_rdr.status().ToString();
            auto rdr = maybe_rdr.ValueOrDie();
            self->schema_ = rdr->schema();
            std::shared_ptr<arrow::Table> table;
            auto st = rdr->Read(&table);
            if (!st.ok()) return "Error reading Feather: " + st.ToString();
            st = self->ingest_table(table);
            if (!st.ok()) return "Error batching Feather: " + st.ToString();
        } else {
            // Arrow IPC: open the footer only — batches decoded lazily by ensure().
            auto maybe_rdr = arrow::ipc::RecordBatchFileReader::Open(file);
            if (!maybe_rdr.ok())
                return "Not a valid Arrow IPC file: " + maybe_rdr.status().ToString();
            self->rdr_                 = maybe_rdr.ValueOrDie();
            self->schema_              = self->rdr_->schema();
            self->num_record_batches_  = self->rdr_->num_record_batches();
            if (self->num_record_batches_ == 0) self->all_read_ = true;
        }

        if (self->batches_.empty() && self->all_read_) {
            // Empty file (Feather with 0 rows, or 0-batch IPC): seed a zero-row
            // batch so the schema can still render.
            self->batch_first_row_.push_back(0);
            self->batches_.push_back(arrow::RecordBatch::Make(
                self->schema_, 0, std::vector<std::shared_ptr<arrow::Array>>(
                    self->schema_->num_fields(),
                    arrow::MakeArrayOfNull(arrow::utf8(), 0).ValueOrDie())));
        }

        *out = std::move(self);
        return "";
    }

public:
    std::shared_ptr<arrow::Schema> schema()    const override { return schema_; }
    int64_t total_rows()                        const override {
        return all_read_ ? total_rows_ : -1;
    }
    int     num_chunks()                        const override {
        if (is_feather_) return (int)batches_.size();
        // A 0-batch Arrow IPC seeds one zero-row batch (see open) so its schema
        // still renders; surface that instead of the raw 0, which would make
        // the seeded batch unreachable and the table view draw nothing.
        return num_record_batches_ > 0 ? num_record_batches_
                                       : (int)batches_.size();
    }
    arrow::Status read_status()                 const override { return read_status_; }
    ChunkMeta chunk_meta(int i)                 const override {
        if (!is_feather_ && i >= (int)batches_.size())
            const_cast<IpcSource*>(this)->ensure(i);
        // num_chunks() reports the declared record-batch count, but a batch
        // that fails to decode leaves batches_ short (load_next_ipc records the
        // sticky read_status_ and stops). Indexing batches_[i]/batch_first_row_
        // past the end read out of bounds and dereferenced a garbage pointer.
        // Report an empty chunk instead, keeping the caller's row math
        // monotonic; the rows render blank and read_status() surfaces the error.
        if (i < 0 || i >= (int)batches_.size()) {
            int64_t end = batch_first_row_.empty()
                ? 0 : batch_first_row_.back() + batches_.back()->num_rows();
            return {end, 0};
        }
        return {batch_first_row_[i], batches_[i]->num_rows()};
    }
    void ensure(int i) override {
        if (is_feather_) return;
        while (!all_read_ && (int)batches_.size() <= i)
            (void)load_next_ipc();
    }
    const std::string& path()                   const override { return path_; }
    std::string footer()                        const override {
        return std::string("Format: ") + (is_feather_ ? "Feather v2" : "Arrow IPC");
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
};

std::string open_ipc_source(const std::string& path, bool is_feather, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<IpcSource> s;
    std::string e = IpcSource::open(path, is_feather, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── Apache ORC source ────────────────────────────────────────────────────────
//
// Compiled in only when the Arrow build we link against has the ORC adapter
// (`arrow/adapters/orc/adapter.h`). CMake detects the header at configure
// time and defines VV_HAVE_ORC. The Apache Arrow apt repo and Homebrew's
// apache-arrow ship Arrow with ORC enabled; the AlmaLinux 8 static build
// passes -DARROW_ORC=ON to the arrow-build stage. Local dev builds need
// the same flag — without it `vv file.orc` reports a build-time message.

// ── Arrow IPC stream format (.arrows, or a stream on stdin / a pipe) ────────
//
// The stream format is the IPC file format without the footer: a schema
// message, then record batches, read front to back. There is nothing to seek
// to, so it streams like the text readers — each record batch is one chunk,
// kept through the shared bounded-window helper — and works on a pipe
// without the temporary copy the footer-based formats need.
class IpcStreamSource : public TabularSource {
    std::string                                          path_;
    std::shared_ptr<arrow::Schema>                       schema_;
    mutable std::shared_ptr<arrow::ipc::RecordBatchReader> reader_;
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>                         batch_first_row_;
    mutable std::vector<int64_t>                         batch_num_rows_;
    mutable int64_t                                      rows_so_far_ = 0;
    mutable bool                                         all_read_    = false;
    mutable bool                                         retain_all_  = false;
    mutable bool                                         evicted_any_ = false;
    mutable arrow::Status                                read_status_;

    void advance() const {
        if (all_read_) return;
        std::shared_ptr<arrow::RecordBatch> b;
        auto st = reader_->ReadNext(&b);
        if (!st.ok()) { read_status_ = st; all_read_ = true; return; }
        if (!b) { all_read_ = true; return; }
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(b));
    }

public:
    // True when `head` starts like an IPC stream: the 0xFFFFFFFF continuation
    // marker, then a metadata length.
    static bool looks_like_stream(const uint8_t* m, size_t n) {
        if (n < 8 || m[0] != 0xff || m[1] != 0xff || m[2] != 0xff || m[3] != 0xff) return false;
        const uint32_t len = (uint32_t)m[4] | ((uint32_t)m[5] << 8) |
                             ((uint32_t)m[6] << 16) | ((uint32_t)m[7] << 24);
        return len > 0 && len < (1u << 26);
    }

    static std::string open_stream(const std::string& label,
                                   std::shared_ptr<arrow::io::InputStream> in,
                                   std::unique_ptr<IpcStreamSource>* out) {
        auto self = std::make_unique<IpcStreamSource>();
        self->path_ = label;
        auto r = arrow::ipc::RecordBatchStreamReader::Open(std::move(in));
        if (!r.ok()) return "Not a valid Arrow IPC stream: " + r.status().ToString();
        self->reader_ = r.ValueOrDie();
        self->schema_ = self->reader_->schema();
        self->advance();
        if (!self->read_status_.ok() && self->batches_.empty())
            return "Error reading Arrow IPC stream: " + self->read_status_.ToString();
        *out = std::move(self);
        return "";
    }
    static std::string open(const std::string& path, std::unique_ptr<IpcStreamSource>* out) {
        auto f = arrow::io::ReadableFile::Open(path);
        if (!f.ok()) return "Cannot open '" + path + "': " + f.status().ToString();
        return open_stream(path, f.ValueOrDie(), out);
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override { return {batch_first_row_[i], batch_num_rows_[i]}; }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i) advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError("chunk ", i, " was released by the streaming window");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
    arrow::Status read_status() const override { return read_status_; }
    const std::string& path() const override { return path_; }
    std::string footer() const override { return "Format: Arrow IPC stream"; }
};

std::string open_ipc_stream_file(const std::string& path, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<IpcStreamSource> s;
    std::string e = IpcStreamSource::open(path, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
std::string open_ipc_stream(const std::string& label, std::shared_ptr<arrow::io::InputStream> in,
                            std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<IpcStreamSource> s;
    std::string e = IpcStreamSource::open_stream(label, std::move(in), &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
bool looks_like_ipc_stream(const uint8_t* m, size_t n) {
    return IpcStreamSource::looks_like_stream(m, n);
}

#if VV_HAVE_ORC
class OrcSource : public TabularSource {
    std::string                                              path_;
    std::shared_ptr<arrow::Schema>                           schema_;
    std::unique_ptr<arrow::adapters::orc::ORCFileReader>     reader_;
    int64_t                                                  num_stripes_ = 0;
    int64_t                                                  num_rows_total_ = 0;
    arrow::Compression::type                                 compression_ =
        arrow::Compression::UNCOMPRESSED;
    int64_t                                                  file_size_ = 0;
    // Each stripe's first row and row count, from the file footer: stripes are
    // located without decoding, and read_chunk() decodes only the requested
    // columns of one stripe (nothing is cached; the callers cache chunks).
    std::vector<ChunkMeta>                                   stripes_;
    mutable arrow::Status                                    read_status_;  // sticky error

    static std::string fmt_size(int64_t sz) {
        char buf[32];
        if      (sz < 1024)             std::snprintf(buf,sizeof(buf),"%lld B",(long long)sz);
        else if (sz < 1024*1024)        std::snprintf(buf,sizeof(buf),"%.1f KiB",sz/1024.0);
        else if (sz < 1024LL*1024*1024) std::snprintf(buf,sizeof(buf),"%.2f MiB",sz/(1024.0*1024));
        else                            std::snprintf(buf,sizeof(buf),"%.2f GiB",sz/(1024.0*1024*1024));
        return buf;
    }
    static const char* codec_name(arrow::Compression::type c) {
        switch (c) {
            case arrow::Compression::UNCOMPRESSED: return "uncompressed";
            case arrow::Compression::SNAPPY:       return "snappy";
            case arrow::Compression::GZIP:         return "gzip";
            case arrow::Compression::LZO:          return "lzo";
            case arrow::Compression::LZ4:          return "lz4";
            case arrow::Compression::LZ4_FRAME:    return "lz4_frame";
            case arrow::Compression::ZSTD:         return "zstd";
            default:                                return "?";
        }
    }

public:
    static std::string open(const std::string& path,
                             std::unique_ptr<OrcSource>* out) {
        auto self = std::make_unique<OrcSource>();
        self->path_ = path;

        auto maybe_file = arrow::io::ReadableFile::Open(path);
        if (!maybe_file.ok())
            return "Cannot open '" + path + "': " + maybe_file.status().ToString();
        auto file = maybe_file.ValueOrDie();

        auto maybe_rdr = arrow::adapters::orc::ORCFileReader::Open(
            file, arrow::default_memory_pool());
        if (!maybe_rdr.ok())
            return "Not a valid ORC file: " + maybe_rdr.status().ToString();
        self->reader_ = std::move(*maybe_rdr);

        auto sch_or = self->reader_->ReadSchema();
        if (!sch_or.ok())
            return "ORC schema read failed: " + sch_or.status().ToString();
        self->schema_         = *sch_or;
        self->num_stripes_    = self->reader_->NumberOfStripes();
        self->num_rows_total_ = self->reader_->NumberOfRows();
        self->file_size_      = self->reader_->GetFileLength();
        auto cmp_or = self->reader_->GetCompression();
        if (cmp_or.ok()) self->compression_ = *cmp_or;

        for (int64_t i = 0; i < self->num_stripes_; ++i) {
            const auto si = self->reader_->GetStripeInformation(i);
            self->stripes_.push_back({si.first_row_id, si.num_rows});
        }
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return num_rows_total_; }
    // An empty file still has one (empty) chunk, so schema-only views work.
    int     num_chunks() const override {
        return num_stripes_ == 0 ? 1 : (int)num_stripes_;
    }
    ChunkMeta chunk_meta(int i) const override {
        if (i < 0 || i >= (int)stripes_.size()) return {0, 0};
        return stripes_[(size_t)i];
    }
    arrow::Status read_status() const override { return read_status_; }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        if (num_stripes_ == 0 && i == 0) {          // empty file
            arrow::FieldVector fields;
            std::vector<std::shared_ptr<arrow::Array>> cols;
            for (int c : col_indices) {
                fields.push_back(schema_->field(c));
                ARROW_ASSIGN_OR_RAISE(auto a, arrow::MakeArrayOfNull(schema_->field(c)->type(), 0));
                cols.push_back(std::move(a));
            }
            *out = arrow::Table::Make(arrow::schema(fields), cols, 0);
            return arrow::Status::OK();
        }
        if (i < 0 || i >= (int)num_stripes_)
            return arrow::Status::IndexError("chunk ", i, " out of range");
        // ORC decodes the selected top-level fields, returned in schema
        // order; map each requested column (any order, repeats allowed) to
        // its place in that batch.
        std::vector<int> wanted(col_indices.begin(), col_indices.end());
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        auto b = reader_->ReadStripe(i, wanted);
        if (!b.ok()) {
            // A failed stripe read must not become a short table with exit 0.
            if (read_status_.ok()) read_status_ = b.status();
            return b.status();
        }
        const auto& batch = *b;
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::Array>> cols;
        for (int c : col_indices) {
            const int pos = (int)(std::lower_bound(wanted.begin(), wanted.end(), c) - wanted.begin());
            if (pos >= batch->num_columns())
                return arrow::Status::Invalid("ORC stripe ", i, " lacks column ", c);
            fields.push_back(schema_->field(c));
            cols.push_back(batch->column(pos));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, batch->num_rows());
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        std::string s = "Format: ORC  |  Stripes: " +
            std::to_string(num_stripes_);
        s += "  |  Compressed: " + fmt_size(file_size_);
        s += "  |  Codec: " + std::string(codec_name(compression_));
        return s;
    }
};

std::string open_orc_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<OrcSource> s;
    std::string e = OrcSource::open(path, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
#endif  // VV_HAVE_ORC
