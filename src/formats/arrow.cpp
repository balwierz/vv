// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Arrow IPC / Feather, IPC streams and Apache ORC.

#include "internal.hpp"
#include <arrow/util/byte_size.h>
#include <zlib.h>

// ── Arrow IPC / Feather source ────────────────────────────────────────────────

// The IPC reader checks a record batch's buffer sizes only: offsets that run
// backwards or past their data (a corrupt or crafted file) reached the cell
// formatters and the writers, which read out of bounds. Each column a read
// asks for is checked in full the first time (check_column_structure: not
// UTF-8, which vv shows as stored); `checked` remembers which were.
static arrow::Status check_ipc_columns(const arrow::RecordBatch& b, const std::vector<int>& cols,
                                       std::vector<char>* checked) {
    checked->resize((size_t)b.num_columns(), 0);
    for (int c : cols) {
        if (c < 0 || c >= b.num_columns() || (*checked)[(size_t)c]) continue;
        ARROW_RETURN_NOT_OK(check_column_structure(*b.column(c), b.schema()->field(c)->name()));
        (*checked)[(size_t)c] = 1;
    }
    return arrow::Status::OK();
}

// A decoded record batch, before its rows are counted: its columns agree
// with its length and buffers (RecordBatch::Validate), and the length keeps
// the running row count in range — a batch without columns can claim any
// length, and a claimed 2^63 overflowed the count.
static arrow::Status check_ipc_batch(const arrow::RecordBatch& b, int64_t rows_before) {
    ARROW_RETURN_NOT_OK(b.Validate());
    if (b.num_rows() < 0 || rows_before > INT64_MAX - b.num_rows())
        return arrow::Status::Invalid("a record batch claims ", b.num_rows(), " rows (a corrupt file)");
    return arrow::Status::OK();
}

class IpcSource : public TabularSource {
    std::string                                       path_;
    bool                                              is_feather_ = false;
    bool                                              feather_v1_ = false;  // read whole
    std::shared_ptr<arrow::Schema>                    schema_;
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> rdr_;       // Arrow IPC only
    int                                               num_record_batches_ = 0; // Arrow IPC only
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>                      batch_first_row_;
    mutable std::vector<int64_t>                             batch_num_rows_;
    mutable std::list<int>                                   lru_;
    mutable int64_t                                          cached_bytes_ = 0;
    mutable int64_t                                   total_rows_ = 0;
    mutable bool                                      all_read_   = false;
    mutable arrow::Status                             read_status_;  // sticky error
    // The columns of each batch checked in full (check_ipc_columns).
    mutable std::map<int, std::vector<char>>          checked_;

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
            batch_num_rows_.push_back(batch->num_rows());
            total_rows_ += batch->num_rows();
            batches_.push_back(std::move(batch));
        }
        all_read_ = true;
        return arrow::Status::OK();
    }

    void cache_batch(int i, std::shared_ptr<arrow::RecordBatch> batch) const {
        lru_.remove(i);
        if (batches_[(size_t)i])
            cached_bytes_ -= arrow::util::TotalBufferSize(*batches_[(size_t)i]);
        cached_bytes_ += arrow::util::TotalBufferSize(*batch);
        batches_[(size_t)i] = std::move(batch);
        lru_.push_front(i);
        while (cached_bytes_ > (64 << 20) && lru_.size() > 1) {
            int victim = lru_.back();
            lru_.pop_back();
            cached_bytes_ -= arrow::util::TotalBufferSize(*batches_[(size_t)victim]);
            batches_[(size_t)victim].reset();
        }
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
        if (auto st = check_ipc_batch(*b, total_rows_); !st.ok()) {
            read_status_ = st;
            all_read_ = true;
            return st;
        }
        batch_first_row_.push_back(total_rows_);
        batch_num_rows_.push_back(b->num_rows());
        total_rows_ += b->num_rows();
        batches_.push_back(nullptr);
        cache_batch(i, std::move(b));
        if ((int)batches_.size() >= num_record_batches_) all_read_ = true;
        return arrow::Status::OK();
    }

public:
    static std::string open(const std::string& path, bool is_feather,
                             std::unique_ptr<IpcSource>* out) {
        auto self = std::make_unique<IpcSource>();
        self->path_       = path;
        self->is_feather_ = is_feather;

        auto maybe_file = open_input_file(path);
        if (!maybe_file.ok())
            return "Cannot open '" + path + "': " + maybe_file.status().ToString();
        auto file = maybe_file.ValueOrDie();

        // Feather V2 is an Arrow IPC file ("ARROW1"), read lazily below like
        // any .arrow. Only Feather V1 ("FEA1") needs feather::Reader, which
        // reads the whole table and is deprecated as of Arrow 26.
        if (is_feather)
            if (auto head = file->ReadAt(0, 4); head.ok() && (*head)->size() == 4)
                self->feather_v1_ = std::memcmp((*head)->data(), "FEA1", 4) == 0;
        if (self->feather_v1_) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            auto maybe_rdr = arrow::ipc::feather::Reader::Open(file);
#pragma GCC diagnostic pop
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
            // batch so the schema can still render, each column of its own type.
            std::vector<std::shared_ptr<arrow::Array>> empty;
            for (const auto& f : self->schema_->fields()) {
                auto a = arrow::MakeEmptyArray(f->type());
                if (!a.ok()) return "Cannot read '" + path + "': " + a.status().ToString();
                empty.push_back(*a);
            }
            self->batch_first_row_.push_back(0);
            self->batch_num_rows_.push_back(0);
            self->batches_.push_back(arrow::RecordBatch::Make(self->schema_, 0, std::move(empty)));
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
        // Arrow's public file reader does not expose individual metadata row
        // counts. Expose the discovered frontier, never decode a hidden prefix
        // from chunk_meta() just because the frontend binary-searches offsets.
        return (int)batches_.size();
    }
    arrow::Status read_status()                 const override { return read_status_; }
    ChunkMeta chunk_meta(int i)                 const override {
        // num_chunks() reports the declared record-batch count, but a batch
        // that fails to decode leaves batches_ short (load_next_ipc records the
        // sticky read_status_ and stops). Indexing batches_[i]/batch_first_row_
        // past the end read out of bounds and dereferenced a garbage pointer.
        // Report an empty chunk instead, keeping the caller's row math
        // monotonic; the rows render blank and read_status() surfaces the error.
        if (i < 0 || i >= (int)batches_.size()) {
            int64_t end =
                batch_first_row_.empty() ? 0 : batch_first_row_.back() + batch_num_rows_.back();
            return {end, 0};
        }
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void ensure(int i) override {
        if (feather_v1_) return;
        while (!all_read_ && (int)batches_.size() <= i)
            (void)load_next_ipc();
    }
    const std::string& path()                   const override { return path_; }
    std::string footer()                        const override {
        return std::string("Format: ") +
               (feather_v1_ ? "Feather v1" : is_feather_ ? "Feather v2" : "Arrow IPC");
    }
    // The IPC footer: format version, record batches, their rows (each
    // batch's message header, not its body) and the file's own metadata.
    void add_details(FileDetails& d) const override {
        if (!rdr_) return;   // Feather v1: one table, no footer to describe
        DetailItem ipc{"Arrow IPC", "", {}};
        auto add = [](DetailItem& s, std::string k, std::string v) {
            s.children.push_back({std::move(k), std::move(v), {}});
        };
        add(ipc, "Metadata version", "V" + std::to_string((int)rdr_->version() + 1));
        add(ipc, "Record batches", digits_with_sep(std::to_string(num_record_batches_)));
        if (auto n = rdr_->CountRows(); n.ok())
            add(ipc, "Rows", digits_with_sep(std::to_string(*n)));
        d.push_back(std::move(ipc));
        if (auto kv = rdr_->metadata(); kv && kv->size() > 0) {
            DetailItem fm{"File metadata", "", {}};
            for (int64_t i = 0; i < kv->size(); ++i) add(fm, kv->key(i), kv->value(i));
            d.push_back(std::move(fm));
        }
    }

    // A file vv wrote: the header it carried from its own source.
    std::vector<std::string> source_header() const override {
        auto kv = schema_ ? schema_->metadata() : nullptr;
        auto v = kv ? kv->Get("vv.source_header") : arrow::Result<std::string>(arrow::Status::KeyError(""));
        return v.ok() ? split_lines(*v) : std::vector<std::string>{};
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i < 0 || i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!feather_v1_) {
            if (!batches_[(size_t)i]) {
                auto b = rdr_->ReadRecordBatch(i);
                if (!b.ok()) return read_status_ = b.status();
                if (auto st = (*b)->Validate(); !st.ok()) return read_status_ = st;
                cache_batch(i, *b);
            } else {
                lru_.remove(i);
                lru_.push_front(i);
            }
        }
        if (auto st = check_ipc_columns(*batches_[(size_t)i], col_indices, &checked_[i]); !st.ok())
            return read_status_ = st;
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
    mutable std::map<int, std::vector<char>>             checked_;   // check_ipc_columns

    void advance() const {
        if (all_read_) return;
        std::shared_ptr<arrow::RecordBatch> b;
        auto st = reader_->ReadNext(&b);
        if (!st.ok()) { read_status_ = st; all_read_ = true; return; }
        if (!b) { all_read_ = true; return; }
        if (st = check_ipc_batch(*b, rows_so_far_); !st.ok()) { read_status_ = st; all_read_ = true; return; }
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
        auto f = open_input_file(path);
        if (!f.ok()) return "Cannot open '" + path + "': " + f.status().ToString();
        return open_stream(path, f.ValueOrDie(), out);
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override { return {batch_first_row_[i], batch_num_rows_[i]}; }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    int64_t chunk_bytes(int i) const override { return stream_chunk_bytes(batches_, i); }
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
        if (auto st = check_ipc_columns(*batches_[i], col_indices, &checked_[i]); !st.ok())
            return read_status_ = st;
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
// ── ORC section check ────────────────────────────────────────────────────────
//
// liborc parses the postscript, the footer and each stripe footer with
// protobuf. When one of those parses fails part-way (malformed wire data, or a
// decompression error thrown from inside the parse), protobuf built without
// NDEBUG (Arch's apache-orc is) aborts the process in the message's destructor
// (TcParser::CheckHasBitConsistency) before ORC's ParseError reaches Arrow:
// `vv bad.orc` exited 134. vv reads these sections first and refuses any that
// protobuf might fail on, by a stricter rule than protobuf's own: no group or
// reserved wire types, varints of at most 10 bytes, lengths inside the
// message, the message fields of orc_proto.proto checked recursively and
// packed repeated fields whole. Compressed sections must split into chunks
// that each decode, whole, to at most the compression block. A file a real
// writer produced passes; one that fails gets an error instead of an abort.
// LZO sections are left to ORC (no LZO decoder here).
namespace orc_check {

// The orc_proto.proto messages whose length-delimited fields are checked
// further: kPlain stands for every message holding only scalars and strings.
enum Msg : uint8_t {
    kPlain, kPostScript, kFooter, kType, kColStats, kBucket, kGeo, kEncryption,
    kDataMask, kEncVariant, kStripeFooter, kStripeEncVariant,
};
enum class Kind : uint8_t { Opaque, Packed, Message };
struct Sub { Kind kind; Msg msg; };

// What field `f` of message `m` holds when it arrives length-delimited:
// another message, a packed repeated varint, or bytes protobuf does not read.
static Sub sub(Msg m, uint32_t f) {
    const Sub opaque{Kind::Opaque, kPlain}, packed{Kind::Packed, kPlain};
    auto msg = [](Msg x) { return Sub{Kind::Message, x}; };
    switch (m) {
        case kPostScript:        if (f == 4) return packed; break;               // version
        case kFooter:
            switch (f) {
                case 3:  return msg(kPlain);       // StripeInformation
                case 4:  return msg(kType);
                case 5:  return msg(kPlain);       // UserMetadataItem
                case 7:  return msg(kColStats);
                case 10: return msg(kEncryption);
            }
            break;
        case kType:              if (f == 2) return packed;                      // subtypes
                                 if (f == 7) return msg(kPlain); break;          // StringPair
        case kColStats:          if (f == 5) return msg(kBucket);
                                 if (f == 13) return msg(kGeo);
                                 if ((f >= 2 && f <= 9) || f == 12) return msg(kPlain); break;
        case kBucket:            if (f == 1) return packed; break;
        case kGeo:               if (f == 1) return msg(kPlain);                 // BoundingBox
                                 if (f == 2) return packed; break;
        case kEncryption:        if (f == 1) return msg(kDataMask);
                                 if (f == 2) return msg(kPlain);                 // EncryptionKey
                                 if (f == 3) return msg(kEncVariant); break;
        case kDataMask:          if (f == 3) return packed; break;
        case kEncVariant:        if (f == 4) return msg(kPlain); break;          // Stream
        case kStripeFooter:      if (f == 1 || f == 2) return msg(kPlain);       // Stream, ColumnEncoding
                                 if (f == 4) return msg(kStripeEncVariant); break;
        case kStripeEncVariant:  if (f == 1 || f == 2) return msg(kPlain); break;
        case kPlain: break;
    }
    return opaque;
}

static bool varint(const uint8_t*& p, const uint8_t* end, uint64_t* v) {
    uint64_t r = 0;
    for (int i = 0; i < 10 && p < end; ++i) {
        const uint8_t b = *p++;
        if (i == 9 && b > 1) return false;
        r |= uint64_t(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) { *v = r; return true; }
    }
    return false;
}

// Calls fn(field, wire type, varint value, bytes, length) for each field of
// the message in [p, end); false when one is malformed or fn refuses it.
template <class Fn>
static bool fields(const uint8_t* p, const uint8_t* end, Fn&& fn) {
    while (p < end) {
        uint64_t tag = 0, v = 0;
        if (!varint(p, end, &tag) || tag > 0xffffffffu || (tag >> 3) == 0) return false;
        const uint32_t f = (uint32_t)(tag >> 3);
        const int wt = (int)(tag & 7);
        const uint8_t* data = nullptr;
        size_t len = 0;
        switch (wt) {
            case 0: if (!varint(p, end, &v)) return false; break;
            case 1: if (end - p < 8) return false; p += 8; break;
            case 5: if (end - p < 4) return false; p += 4; break;
            case 2:
                if (!varint(p, end, &v) || v > (uint64_t)(end - p)) return false;
                data = p; len = (size_t)v; p += len;
                break;
            default: return false;   // groups (3, 4) and the reserved 6, 7
        }
        if (!fn(f, wt, v, data, len)) return false;
    }
    return true;
}

static bool message(const uint8_t* p, const uint8_t* end, Msg m) {
    return fields(p, end, [m](uint32_t f, int wt, uint64_t, const uint8_t* d, size_t n) {
        if (wt != 2) return true;
        const Sub s = sub(m, f);
        if (s.kind == Kind::Message) return message(d, d + n, s.msg);
        if (s.kind == Kind::Packed) {
            const uint8_t* q = d;
            uint64_t v;
            while (q < d + n) if (!varint(q, d + n, &v)) return false;
        }
        return true;
    });
}

// Upper bound on a section's decoded size; a real footer is a few megabytes.
constexpr uint64_t kMaxSection = uint64_t(1) << 30;

// The decoded bytes of a section compressed with ORC kind `kind` (1 zlib,
// 2 snappy, 4 lz4, 5 zstd): chunks of a 3-byte little-endian header
// (length << 1 | original) followed by that many bytes, stored as they are
// or compressed, each decoding to at most `block` bytes. "" on success.
static std::string decode(int kind, uint64_t block, const uint8_t* p, size_t n, std::string* out) {
    std::unique_ptr<arrow::util::Codec> codec;
    if (kind != 1) {
        const auto t = kind == 2 ? arrow::Compression::SNAPPY
                     : kind == 4 ? arrow::Compression::LZ4 : arrow::Compression::ZSTD;
        auto c = arrow::util::Codec::Create(t);
        if (!c.ok()) return "skip";
        codec = std::move(*c);
    }
    out->clear();
    size_t at = 0;
    for (int chunk = 0; at < n; ++chunk) {
        if (n - at < 3) return "chunk " + std::to_string(chunk) + " header is cut short";
        const uint32_t h = (uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16;
        at += 3;
        const size_t len = h >> 1;
        if (len > n - at) return "chunk " + std::to_string(chunk) + " runs past the section";
        if (len > block) return "chunk " + std::to_string(chunk) + " exceeds the compression block";
        const uint8_t* src = p + at;
        at += len;
        const size_t have = out->size();
        if (have + block > kMaxSection) return "over 1 GiB decoded";
        if (h & 1) { out->append((const char*)src, len); continue; }
        out->resize(have + block);
        uint8_t* dst = (uint8_t*)out->data() + have;
        size_t got = 0;
        if (kind == 1) {
            z_stream z{};
            if (inflateInit2(&z, -15) != Z_OK) return "zlib: cannot start";
            z.next_in = const_cast<Bytef*>(src);
            z.avail_in = (uInt)len;
            z.next_out = dst;
            z.avail_out = (uInt)block;
            const int r = inflate(&z, Z_FINISH);
            got = block - z.avail_out;
            const bool whole = r == Z_STREAM_END && z.avail_in == 0;
            inflateEnd(&z);
            if (!whole) return "chunk " + std::to_string(chunk) + " does not decompress (zlib)";
        } else if (kind == 5) {
            // Arrow's one-shot ZSTD decode wants the exact output size, which
            // an ORC chunk does not record: stream it, one whole frame.
            auto dec = codec->MakeDecompressor();
            if (!dec.ok()) return "skip";
            size_t used = 0;
            bool whole = false;
            for (;;) {
                auto r = (*dec)->Decompress((int64_t)(len - used), src + used,
                                            (int64_t)(block - got), dst + got);
                if (!r.ok()) break;
                used += (size_t)r->bytes_read;
                got += (size_t)r->bytes_written;
                if ((*dec)->IsFinished()) { whole = used == len; break; }
                if (r->bytes_read == 0 && r->bytes_written == 0) break;
            }
            if (!whole) return "chunk " + std::to_string(chunk) + " does not decompress (zstd)";
        } else {
            auto r = codec->Decompress((int64_t)len, src, (int64_t)block, dst);
            if (!r.ok()) return "chunk " + std::to_string(chunk) + " does not decompress";
            got = (size_t)*r;
        }
        out->resize(have + got);
    }
    return "";
}

// The section [off, off + len) of `file` as message `m`; "" when it passes or
// cannot be checked here (LZO), else what is wrong with it.
static std::string section(arrow::io::RandomAccessFile& file, int64_t size, uint64_t off,
                           uint64_t len, int kind, uint64_t block, Msg m) {
    if (off > (uint64_t)size || len > (uint64_t)size - off) return "lies outside the file";
    if (len > kMaxSection) return "is over 1 GiB";
    auto buf = file.ReadAt((int64_t)off, (int64_t)len);
    if (!buf.ok()) return "cannot be read: " + buf.status().ToString();
    if ((uint64_t)(*buf)->size() != len) return "is cut short";
    const uint8_t* p = (*buf)->data();
    std::string decoded;
    if (kind != 0) {
        if (kind == 3) return "";
        std::string e = decode(kind, block, p, (size_t)len, &decoded);
        if (e == "skip") return "";
        if (!e.empty()) return e;
        p = (const uint8_t*)decoded.data();
        len = decoded.size();
    }
    return message(p, p + len, m) ? "" : "is not a well-formed protobuf message";
}

// Where a stripe's footer lies and how the file is compressed, from the tail.
struct Tail {
    int      kind = 0;
    uint64_t block = 256 * 1024;
    struct Stripe { uint64_t start = 0, length = 0; bool ok = true; };
    std::vector<Stripe> stripes;
};

// The postscript and footer of `file` checked as liborc will parse them, and
// the stripe footers located. "" when they pass.
static std::string tail(arrow::io::RandomAccessFile& file, Tail* t) {
    auto size_or = file.GetSize();
    if (!size_or.ok()) return "";                     // ORC reports it
    const int64_t size = *size_or;
    if (size < 4) return "";
    auto last = file.ReadAt(size - 1, 1);
    if (!last.ok() || (*last)->size() != 1) return "";
    const uint64_t ps_len = (*last)->data()[0];
    if (ps_len < 3 || 1 + ps_len > (uint64_t)std::min<int64_t>(size, 16 * 1024)) return "";
    auto ps_buf = file.ReadAt(size - 1 - (int64_t)ps_len, (int64_t)ps_len);
    if (!ps_buf.ok() || (uint64_t)(*ps_buf)->size() != ps_len) return "";
    const uint8_t* ps = (*ps_buf)->data();
    if (!message(ps, ps + ps_len, kPostScript)) return "malformed postscript";
    uint64_t footer_len = 0;
    bool has_kind = false;
    fields(ps, ps + ps_len, [&](uint32_t f, int wt, uint64_t v, const uint8_t*, size_t) {
        if (wt != 0) return true;
        if (f == 1) footer_len = v;
        // An enum value outside CompressionKind stays unset, as in protobuf.
        if (f == 2 && v <= 6) { t->kind = (int)v; has_kind = true; }
        if (f == 3) t->block = v;
        return true;
    });
    if (!has_kind) return "";                         // ORC: "Unknown compression type"
    if (t->kind != 0 && (t->block == 0 || t->block >= (1u << 23)))
        return "invalid compression block size " + std::to_string(t->block);
    if (t->kind == 6) return "";                      // brotli: ORC refuses it before parsing
    const uint64_t tail_len = 1 + ps_len + footer_len;
    if (footer_len >= (uint64_t)size || tail_len >= (uint64_t)size) return "";  // ORC reports it
    std::string e = section(file, size, (uint64_t)size - tail_len, footer_len, t->kind, t->block, kFooter);
    if (!e.empty()) return "footer " + e;
    if (t->kind == 3) return "";
    // The stripes, for their footers' ranges (StripeInformation 1-4).
    auto buf = file.ReadAt(size - (int64_t)tail_len, (int64_t)footer_len);
    if (!buf.ok()) return "";
    std::string decoded;
    const uint8_t* p = (*buf)->data();
    size_t n = (size_t)(*buf)->size();
    if (t->kind != 0) {
        if (!decode(t->kind, t->block, p, n, &decoded).empty()) return "";
        p = (const uint8_t*)decoded.data();
        n = decoded.size();
    }
    // Arrow's adapter turns a DECIMAL type into decimal128(precision, scale),
    // whose constructor aborts (ARROW_CHECK) on a precision outside [1, 38];
    // 0 stands for 38 (Hive 0.11). Type: kind 1, precision 5 (uint32).
    std::string bad_type;
    int type_index = 0;
    fields(p, p + n, [&](uint32_t f, int wt, uint64_t, const uint8_t* d, size_t dn) {
        if (f == 4 && wt == 2) {
            uint64_t kind = 0;
            uint32_t precision = 0;
            fields(d, d + dn, [&](uint32_t tf, int twt, uint64_t tv, const uint8_t*, size_t) {
                if (twt != 0) return true;
                if (tf == 1) kind = tv <= 20 ? tv : 0;   // outside Type.Kind: unset
                if (tf == 5) precision = (uint32_t)tv;
                return true;
            });
            if (kind == 14 && precision > 38 && bad_type.empty())
                bad_type = "type " + std::to_string(type_index) + " is a decimal of precision " +
                           std::to_string(precision) + " (at most 38)";
            ++type_index;
            return true;
        }
        if (f != 3 || wt != 2) return true;
        uint64_t v[5] = {0, 0, 0, 0, 0};
        fields(d, d + dn, [&](uint32_t sf, int swt, uint64_t sv, const uint8_t*, size_t) {
            if (swt == 0 && sf >= 1 && sf <= 4) v[sf] = sv;
            return true;
        });
        Tail::Stripe s;
        s.ok = !(v[1] > UINT64_MAX - v[2] || v[1] + v[2] > UINT64_MAX - v[3]);
        s.start = s.ok ? v[1] + v[2] + v[3] : 0;
        s.length = v[4];
        t->stripes.push_back(s);
        return true;
    });
    return bad_type.empty() ? "" : "footer " + bad_type;
}

}  // namespace orc_check

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
    // The file and its tail as orc_check read them: each stripe footer is
    // checked before ORC first parses it.
    std::shared_ptr<arrow::io::RandomAccessFile>             file_;
    orc_check::Tail                                          tail_;
    std::vector<char>                                        stripe_checked_;

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

    // Stripe `i`'s footer, checked once before ORC parses it.
    arrow::Status check_stripe(int i) {
        if (i < 0 || (size_t)i >= tail_.stripes.size()) return arrow::Status::OK();
        if (stripe_checked_.size() < tail_.stripes.size()) stripe_checked_.resize(tail_.stripes.size(), 0);
        if (stripe_checked_[(size_t)i]) return arrow::Status::OK();
        const auto& s = tail_.stripes[(size_t)i];
        std::string e = "lies outside the file";
        if (s.ok) {
            auto size = file_->GetSize();
            if (!size.ok()) return size.status();
            e = orc_check::section(*file_, *size, s.start, s.length, tail_.kind, tail_.block,
                                   orc_check::kStripeFooter);
        }
        if (!e.empty()) return arrow::Status::Invalid("ORC stripe ", i, " footer ", e);
        stripe_checked_[(size_t)i] = 1;
        return arrow::Status::OK();
    }

public:
    static std::string open(const std::string& path,
                             std::unique_ptr<OrcSource>* out) {
        auto self = std::make_unique<OrcSource>();
        self->path_ = path;

        auto maybe_file = open_input_file(path);
        if (!maybe_file.ok())
            return "Cannot open '" + path + "': " + maybe_file.status().ToString();
        auto file = maybe_file.ValueOrDie();
        if (std::string e = orc_check::tail(*file, &self->tail_); !e.empty())
            return "Not a valid ORC file: " + e;
        self->file_ = file;

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
        if (arrow::Status st = check_stripe(i); !st.ok()) {
            if (read_status_.ok()) read_status_ = st;
            return st;
        }
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
    bool random_access() const override { return true; }
    void add_details(FileDetails& d) const override {
        DetailItem orc{"ORC", "", {}};
        auto add = [](DetailItem& s, std::string k, std::string v) {
            s.children.push_back({std::move(k), std::move(v), {}});
        };
        add(orc, "File version", reader_->GetFileVersion().ToString());
        if (std::string sw = reader_->GetSoftwareVersion(); !sw.empty()) add(orc, "Software", sw);
        // The fix the writer includes, as ORC names it (WriterVersion).
        static const char* const kWriter[] = {"original", "HIVE-8732", "HIVE-4243", "HIVE-12055",
                                              "HIVE-13083", "ORC-101", "ORC-135", "ORC-517",
                                              "ORC-203", "ORC-14"};
        const int wv = (int)reader_->GetWriterVersion();
        add(orc, "Writer version", wv >= 0 && wv < (int)std::size(kWriter) ? kWriter[wv] : std::to_string(wv));
        add(orc, "Codec", codec_name(compression_));
        add(orc, "Compression block", detail_size(reader_->GetCompressionSize()));
        add(orc, "Stripes", digits_with_sep(std::to_string(num_stripes_)));
        add(orc, "Row index stride", digits_with_sep(std::to_string(reader_->GetRowIndexStride())));
        d.push_back(std::move(orc));
        if (auto kv = reader_->ReadMetadata(); kv.ok() && *kv && (*kv)->size() > 0) {
            DetailItem um{"User metadata", "", {}};
            for (int64_t i = 0; i < (*kv)->size(); ++i) add(um, (*kv)->key(i), (*kv)->value(i));
            d.push_back(std::move(um));
        }
    }
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
