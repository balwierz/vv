// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Oxford Nanopore POD5 (.pod5): Arrow IPC files embedded in one container.
//   signature (8) | section marker (16) | embedded files … | footer
//   (FlatBuffer) | footer length (int64) | section marker | signature
// The footer lists each embedded file's offset, length and content type
// (reads, signal, run info, indexes). Tabs: reads, signal, run_info. read_id
// (16-byte UUIDs) is shown as text; the signal tab decodes the VBZ-compressed
// samples (zstd over a 16-bit stream-VByte of zigzag deltas) to list<int16>.
// Record batches are read on demand.

#include "internal.hpp"

namespace {

const uint8_t kSignature[8] = {0x8b, 'P', 'O', 'D', '\r', '\n', 0x1a, '\n'};

// [offset, offset + length) of another file, as a RandomAccessFile.
class SliceFile : public arrow::io::RandomAccessFile {
public:
    SliceFile(std::shared_ptr<arrow::io::RandomAccessFile> f, int64_t off, int64_t len)
        : f_(std::move(f)), off_(off), len_(len) {}
    arrow::Status Close() override { closed_ = true; return arrow::Status::OK(); }
    bool closed() const override { return closed_; }
    arrow::Result<int64_t> Tell() const override { return pos_; }
    arrow::Status Seek(int64_t p) override {
        if (p < 0 || p > len_) return arrow::Status::IOError("seek outside the embedded file");
        pos_ = p;
        return arrow::Status::OK();
    }
    arrow::Result<int64_t> GetSize() override { return len_; }
    arrow::Result<int64_t> ReadAt(int64_t p, int64_t n, void* out) override {
        if (p < 0 || p > len_) return arrow::Status::IOError("read outside the embedded file");
        return f_->ReadAt(off_ + p, std::min(n, len_ - p), out);
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t p, int64_t n) override {
        if (p < 0 || p > len_) return arrow::Status::IOError("read outside the embedded file");
        return f_->ReadAt(off_ + p, std::min(n, len_ - p));
    }
    // Synchronous: the default goes through shared_from_this() and a
    // dynamic_cast to RandomAccessFile, which this slice need not support.
    arrow::Future<std::shared_ptr<arrow::Buffer>> ReadAsync(const arrow::io::IOContext&, int64_t p,
                                                            int64_t n) override {
        return arrow::Future<std::shared_ptr<arrow::Buffer>>::MakeFinished(ReadAt(p, n));
    }
    arrow::Result<int64_t> Read(int64_t n, void* out) override {
        ARROW_ASSIGN_OR_RAISE(int64_t got, ReadAt(pos_, n, out));
        pos_ += got;
        return got;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
        ARROW_ASSIGN_OR_RAISE(auto b, ReadAt(pos_, n));
        pos_ += b->size();
        return b;
    }
private:
    std::shared_ptr<arrow::io::RandomAccessFile> f_;
    int64_t off_, len_, pos_ = 0;
    bool closed_ = false;
};

// A minimal, bounds-checked FlatBuffers table reader for the footer.
struct Flat {
    const uint8_t* p;
    size_t n;
    bool ok(size_t at, size_t len) const { return at <= n && len <= n - at; }
    uint32_t u32(size_t at) const { uint32_t v = 0; if (ok(at, 4)) std::memcpy(&v, p + at, 4); return v; }
    int32_t  i32(size_t at) const { return (int32_t)u32(at); }
    uint16_t u16(size_t at) const { uint16_t v = 0; if (ok(at, 2)) std::memcpy(&v, p + at, 2); return v; }
    // Position of field `i` of the table at `t`, 0 when absent.
    size_t field(size_t t, int i) const {
        const int64_t vt = (int64_t)t - i32(t);
        if (vt < 0 || !ok((size_t)vt, 4)) return 0;
        const uint16_t vt_len = u16((size_t)vt);
        if (4 + 2 * (size_t)i + 2 > vt_len) return 0;
        const uint16_t fo = u16((size_t)vt + 4 + 2 * (size_t)i);
        return fo && ok(t + fo, 1) ? t + fo : 0;
    }
    size_t deref(size_t at) const { return at && ok(at, 4) ? at + u32(at) : 0; }
    std::string str(size_t t, int i) const {
        const size_t s = deref(field(t, i));
        if (!s || !ok(s, 4)) return "";
        const uint32_t len = u32(s);
        return ok(s + 4, len) ? std::string((const char*)p + s + 4, len) : "";
    }
    int64_t i64(size_t t, int i, int64_t dflt = 0) const {
        const size_t f = field(t, i);
        int64_t v = dflt;
        if (f && ok(f, 8)) std::memcpy(&v, p + f, 8);
        return v;
    }
    int16_t i16(size_t t, int i, int16_t dflt = 0) const {
        const size_t f = field(t, i);
        return f && ok(f, 2) ? (int16_t)u16(f) : dflt;
    }
};

enum Content : int16_t { ReadsTable = 0, SignalTable = 1, ReadIdIndex = 2, OtherIndex = 3, RunInfoTable = 4 };

struct Embedded { int64_t offset, length; int16_t content; };

std::string uuid_text(const uint8_t* b) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int k = 0; k < 16; ++k) {
        if (k == 4 || k == 6 || k == 8 || k == 10) s += '-';
        s += hex[b[k] >> 4];
        s += hex[b[k] & 15];
    }
    return s;
}

// Decompressed size a zstd frame header declares (RFC 8878 §3.1.1.1), -1
// when absent. Arrow's zstd codec needs the exact output length.
int64_t zstd_content_size(const uint8_t* in, int64_t n) {
    if (n < 6 || std::memcmp(in, "\x28\xb5\x2f\xfd", 4) != 0) return -1;
    const uint8_t fhd = in[4];
    const int fcs_flag = fhd >> 6, single = (fhd >> 5) & 1, dict_flag = fhd & 3;
    int64_t at = 5 + (single ? 0 : 1) + (dict_flag == 3 ? 4 : dict_flag);
    const int fcs_len = fcs_flag == 0 ? (single ? 1 : 0) : 1 << fcs_flag;
    if (fcs_len == 0 || at + fcs_len > n) return -1;
    uint64_t v = 0;
    for (int k = 0; k < fcs_len; ++k) v |= (uint64_t)in[at + k] << (8 * k);
    if (fcs_len == 2) v += 256;
    return v > (uint64_t)INT64_MAX ? -1 : (int64_t)v;
}

// VBZ (POD5's signal codec): zstd, then svb16 — one key bit per value (0: one
// byte, 1: two bytes little-endian) followed by the data bytes — of zigzag
// deltas. False when the bytes do not decode to `count` samples.
bool vbz_decode(const uint8_t* in, int64_t in_len, int64_t count, arrow::util::Codec* zstd,
                std::vector<uint8_t>& scratch, std::vector<int16_t>* out) {
    out->clear();
    // A signal row is a chunk of a read; 2^28 samples is ~15 h at 5 kHz. The
    // count and the frame's declared size come from the file.
    if (count < 0 || count > (int64_t)1 << 28) return false;
    const int64_t keys = (count + 7) / 8, cap = keys + 2 * count;
    const int64_t size = zstd_content_size(in, in_len);
    if (size < keys || size > cap) return false;
    scratch.resize((size_t)std::max<int64_t>(size, 1));
    auto got = zstd->Decompress(in_len, in, size, scratch.data());
    if (!got.ok()) return false;
    const uint8_t* d = scratch.data();
    const int64_t n = *got;
    if (n < keys) return false;
    int64_t pos = keys;
    int32_t prev = 0;
    out->reserve((size_t)count);
    for (int64_t i = 0; i < count; ++i) {
        const bool two = (d[i >> 3] >> (i & 7)) & 1;
        if (pos + (two ? 2 : 1) > n) return false;
        uint32_t v = d[pos];
        if (two) v |= (uint32_t)d[pos + 1] << 8;
        pos += two ? 2 : 1;
        const int32_t delta = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
        prev += delta;
        out->push_back((int16_t)prev);
    }
    return true;
}

struct TabSpec { int16_t content; std::string label; int64_t offset, length; };

// The container: the file, its footer line and the tabs found in it.
struct Pod5File {
    std::string path, footer;
    std::shared_ptr<arrow::io::RandomAccessFile> file;
    std::vector<TabSpec> tabs;
};

std::string open_tab(const std::shared_ptr<const Pod5File>& pf, size_t k, std::unique_ptr<TabularSource>* out);

// One embedded table as a tab. Batches are read when first needed; a 16-byte
// read_id becomes UUID text and (signal tab) VBZ signal becomes list<int16>.
class Pod5Tab : public TabularSource {
    std::shared_ptr<const Pod5File> pf_;
    size_t idx_;
    std::string path_, label_, footer_;
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> reader_;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<int64_t> rows_;            // per batch read so far
    mutable arrow::Status status_;
    std::unique_ptr<arrow::util::Codec> zstd_;
    bool signal_tab_;

    static bool is_uuid(const arrow::Field& f) {
        return f.type()->id() == arrow::Type::FIXED_SIZE_BINARY &&
               static_cast<const arrow::FixedSizeBinaryType&>(*f.type()).byte_width() == 16;
    }
    bool is_vbz(const arrow::Field& f) const {
        if (!signal_tab_ || f.name() != "signal") return false;
        auto md = f.metadata();
        return md && md->Contains("ARROW:extension:name") &&
               *md->Get("ARROW:extension:name") == "minknow.vbz" &&
               (f.type()->id() == arrow::Type::LARGE_BINARY || f.type()->id() == arrow::Type::BINARY);
    }
    arrow::Result<std::shared_ptr<arrow::Array>> convert(const arrow::Field& f,
                                                         const std::shared_ptr<arrow::Array>& a,
                                                         const std::shared_ptr<arrow::Array>& samples) {
        if (is_uuid(f)) {
            const auto& fb = static_cast<const arrow::FixedSizeBinaryArray&>(*a);
            arrow::StringBuilder b;
            for (int64_t i = 0; i < fb.length(); ++i) {
                if (fb.IsNull(i)) ARROW_RETURN_NOT_OK(b.AppendNull());
                else ARROW_RETURN_NOT_OK(b.Append(uuid_text(fb.GetValue(i))));
            }
            return b.Finish();
        }
        if (is_vbz(f)) {
            if (!zstd_) {
                ARROW_ASSIGN_OR_RAISE(zstd_, arrow::util::Codec::Create(arrow::Compression::ZSTD));
            }
            auto vb = std::make_shared<arrow::Int16Builder>();
            arrow::ListBuilder lb(arrow::default_memory_pool(), vb);
            std::vector<uint8_t> scratch;
            std::vector<int16_t> sig;
            for (int64_t i = 0; i < a->length(); ++i) {
                int64_t count = -1;
                if (samples && !samples->IsNull(i)) {
                    if (samples->type_id() == arrow::Type::UINT32)
                        count = static_cast<const arrow::UInt32Array&>(*samples).Value(i);
                    else if (samples->type_id() == arrow::Type::UINT64)
                        count = (int64_t)static_cast<const arrow::UInt64Array&>(*samples).Value(i);
                }
                std::string_view v;
                if (a->IsNull(i)) count = -1;
                else if (a->type_id() == arrow::Type::LARGE_BINARY)
                    v = static_cast<const arrow::LargeBinaryArray&>(*a).GetView(i);
                else
                    v = static_cast<const arrow::BinaryArray&>(*a).GetView(i);
                if (count < 0 || !vbz_decode((const uint8_t*)v.data(), (int64_t)v.size(), count, zstd_.get(),
                                             scratch, &sig)) {
                    ARROW_RETURN_NOT_OK(lb.AppendNull());
                    continue;
                }
                ARROW_RETURN_NOT_OK(lb.Append());
                ARROW_RETURN_NOT_OK(vb->AppendValues(sig.data(), (int64_t)sig.size()));
            }
            return lb.Finish();
        }
        return a;
    }
public:
    Pod5Tab(std::shared_ptr<const Pod5File> pf, size_t idx,
            std::shared_ptr<arrow::ipc::RecordBatchFileReader> r)
        : pf_(std::move(pf)), idx_(idx), path_(pf_->path), label_(pf_->tabs[idx_].label),
          footer_(pf_->footer + "  |  " + label_), reader_(std::move(r)),
          signal_tab_(pf_->tabs[idx_].content == SignalTable) {
        arrow::FieldVector fs;
        for (const auto& f : reader_->schema()->fields()) {
            if (is_uuid(*f)) fs.push_back(arrow::field(f->name(), arrow::utf8()));
            else if (is_vbz(*f)) fs.push_back(arrow::field(f->name(), arrow::list(arrow::int16())));
            else fs.push_back(f);
        }
        schema_ = arrow::schema(fs);
    }
    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        if ((int)rows_.size() < reader_->num_record_batches()) return -1;
        int64_t t = 0;
        for (int64_t r : rows_) t += r;
        return t;
    }
    int num_chunks() const override { return (int)rows_.size(); }
    void ensure(int i) override {
        while ((int)rows_.size() <= i && (int)rows_.size() < reader_->num_record_batches() && status_.ok()) {
            auto b = reader_->ReadRecordBatch((int)rows_.size());
            if (!b.ok()) { status_ = b.status(); return; }
            rows_.push_back((*b)->num_rows());
        }
    }
    ChunkMeta chunk_meta(int i) const override {
        int64_t first = 0;
        for (int k = 0; k < i && k < (int)rows_.size(); ++k) first += rows_[(size_t)k];
        return {first, i < (int)rows_.size() ? rows_[(size_t)i] : 0};
    }
    arrow::Status read_status() const override { return status_; }
    const std::string& path() const override { return path_; }
    std::string tab_label() const override { return label_; }
    std::string footer() const override { return footer_; }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        ARROW_RETURN_NOT_OK(status_);
        if (i < 0 || i >= (int)rows_.size()) return arrow::Status::IndexError("batch ", i);
        ARROW_ASSIGN_OR_RAISE(auto b, reader_->ReadRecordBatch(i));
        // Arrow validates a batch's structure on read but not its values.
        ARROW_RETURN_NOT_OK(b->ValidateFull());
        const int samples_col = b->schema()->GetFieldIndex("samples");
        arrow::FieldVector fs;
        arrow::ArrayVector as;
        for (int c : cols) {
            if (c < 0 || c >= b->num_columns()) continue;
            std::shared_ptr<arrow::Array> a;
            try {
                ARROW_ASSIGN_OR_RAISE(a, convert(*b->schema()->field(c), b->column(c),
                                                 samples_col >= 0 ? b->column(samples_col) : nullptr));
            } catch (const std::bad_alloc&) {
                return arrow::Status::OutOfMemory("decoding POD5 column '", b->schema()->field(c)->name(), "'");
            }
            fs.push_back(schema_->field(c));
            as.push_back(a);
        }
        *out = arrow::Table::Make(arrow::schema(fs), as, b->num_rows());
        return arrow::Status::OK();
    }
    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        std::vector<std::unique_ptr<TabularSource>> v;
        if (idx_ != 0) return v;
        for (size_t k = 1; k < pf_->tabs.size(); ++k) {
            std::unique_ptr<TabularSource> t;
            if (auto e = open_tab(pf_, k, &t); !e.empty()) {
                std::fprintf(stderr, "vv: %s\n", e.c_str());
                continue;
            }
            v.push_back(std::move(t));
        }
        return v;
    }
};

std::string open_tab(const std::shared_ptr<const Pod5File>& pf, size_t k, std::unique_ptr<TabularSource>* out) {
    const TabSpec& t = pf->tabs[k];
    auto slice = std::make_shared<SliceFile>(pf->file, t.offset, t.length);
    auto r = arrow::ipc::RecordBatchFileReader::Open(slice);
    if (!r.ok()) return "'" + pf->path + "': embedded " + t.label + " table: " + r.status().ToString();
    *out = std::make_unique<Pod5Tab>(pf, k, *r);
    return "";
}

}  // namespace

namespace {
std::string open_pod5(const std::string& path, std::shared_ptr<arrow::io::RandomAccessFile> f,
                      std::unique_ptr<TabularSource>* out) {
    auto size_r = f->GetSize();
    const int64_t size = size_r.ok() ? *size_r : 0;
    uint8_t head[8] = {0}, tail[32] = {0};
    if (size < 8 + 16 + 8 + 16 + 8 || !f->ReadAt(0, 8, head).ok() || !f->ReadAt(size - 32, 32, tail).ok() ||
        std::memcmp(head, kSignature, 8) != 0 || std::memcmp(tail + 24, kSignature, 8) != 0)
        return "'" + path + "': not a POD5 file (signature missing at the start or end)";
    int64_t footer_len = 0;
    std::memcpy(&footer_len, tail, 8);
    if (footer_len <= 0 || footer_len > size - 32 - 24) return "'" + path + "': POD5 footer length out of range";
    const int64_t footer_at = size - 32 - footer_len;
    std::vector<uint8_t> fb((size_t)footer_len);
    auto rd = f->ReadAt(footer_at, footer_len, fb.data());
    if (!rd.ok() || *rd != footer_len) return "'" + path + "': cannot read the POD5 footer";
    Flat flat{fb.data(), fb.size()};
    const size_t root = flat.u32(0);
    if (!flat.ok(root, 4)) return "'" + path + "': malformed POD5 footer";
    const std::string software = flat.str(root, 1), version = flat.str(root, 2);
    std::vector<Embedded> files;
    if (const size_t vec = flat.deref(flat.field(root, 3)); vec && flat.ok(vec, 4)) {
        const uint32_t n = flat.u32(vec);
        for (uint32_t k = 0; k < n && flat.ok(vec + 4 + 4 * (size_t)k, 4); ++k) {
            const size_t e = flat.deref(vec + 4 + 4 * (size_t)k);
            if (!e) continue;
            Embedded em{flat.i64(e, 0), flat.i64(e, 1), flat.i16(e, 3)};
            if (em.offset < 0 || em.length <= 0 || em.offset > footer_at || em.length > footer_at - em.offset)
                return "'" + path + "': POD5 footer points outside the file";
            files.push_back(em);
        }
    }
    const std::string fmt = "Format: POD5" + (version.empty() ? "" : " " + version) +
                            (software.empty() ? "" : "  |  written by " + software);
    auto pf = std::make_shared<Pod5File>();
    pf->path = path;
    pf->footer = fmt;
    pf->file = f;
    struct Want { int16_t content; const char* label; };
    for (const Want w : {Want{ReadsTable, "reads"}, Want{SignalTable, "signal"}, Want{RunInfoTable, "run_info"}})
        for (const Embedded& e : files)
            if (e.content == w.content) { pf->tabs.push_back({w.content, w.label, e.offset, e.length}); break; }
    if (pf->tabs.empty()) return "'" + path + "': the POD5 footer lists no reads, signal or run-info table";
    return open_tab(pf, 0, out);
}
}  // namespace

std::string open_pod5_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    auto rf = arrow::io::ReadableFile::Open(path);
    if (!rf.ok()) return "Cannot open '" + path + "': " + rf.status().ToString();
    return open_pod5(path, *rf, out);
}

#ifdef VV_FUZZ
// Open an untrusted buffer as POD5 and read every row of every tab (footer,
// embedded IPC files, UUID and VBZ conversion); then VBZ-decode the buffer
// itself with a sample count from its first bytes.
void pod5_fuzz_one(const uint8_t* buf, size_t n) {
    auto br = std::make_shared<arrow::io::BufferReader>(std::make_shared<arrow::Buffer>(buf, (int64_t)n));
    std::unique_ptr<TabularSource> first;
    if (open_pod5("fuzz.pod5", br, &first).empty()) {
        std::vector<std::unique_ptr<TabularSource>> tabs = first->expand_tabs();
        tabs.insert(tabs.begin(), std::move(first));
        for (auto& t : tabs) {
            std::vector<int> cols(t->schema()->num_fields());
            for (size_t k = 0; k < cols.size(); ++k) cols[k] = (int)k;
            for (int c = 0;; ++c) {
                t->ensure(c);
                if (c >= t->num_chunks()) break;
                std::shared_ptr<arrow::Table> tbl;
                if (!t->read_chunk(c, cols, &tbl).ok()) break;
                if (!tbl->ValidateFull().ok()) std::abort();
            }
        }
    }
    if (n >= 2) {
        auto zstd = arrow::util::Codec::Create(arrow::Compression::ZSTD);
        std::vector<uint8_t> scratch;
        std::vector<int16_t> sig;
        if (zstd.ok()) vbz_decode(buf + 2, (int64_t)n - 2, buf[0] | (buf[1] << 8), zstd->get(), scratch, &sig);
    }
}
#endif
