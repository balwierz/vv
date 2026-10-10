// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// TSV / CSV and the text genomics formats read as delimited text
// (BED, VCF, GFF / GTF, SAM, PAF, mpileup, MatrixMarket, PLINK, ...).

#include "internal.hpp"

class TabixInputStream : public arrow::io::InputStream {
    htsFile*               fp_     = nullptr;
    tbx_t*                 tbx_    = nullptr;
    std::vector<hts_itr_t*> iters_;
    size_t                 cur_iter_ = 0;
    std::string            buf_;       // current line + '\n', drained byte-by-byte
    size_t                 pos_     = 0;
    bool                   eof_     = false;
    bool                   closed_  = false;
    // Reused across records: htslib's line reader (bgzf_getline) resets `l` and
    // reallocs `s` only when a line outgrows it, so keeping one buffer for the
    // whole scan avoids a malloc+free per record. Freed once in the destructor.
    kstring_t              ks_      = {0, 0, nullptr};
public:
    static std::string open(const std::string& path,
                            const std::string& region,
                            std::shared_ptr<TabixInputStream>* out)
    {
        auto self = std::shared_ptr<TabixInputStream>(new TabixInputStream());
        self->fp_  = hts_open(path.c_str(), "r");
        if (!self->fp_) return "Cannot open '" + path + "' for tabix";
        self->tbx_ = tbx_index_load(path.c_str());
        if (!self->tbx_) {
            hts_close(self->fp_); self->fp_ = nullptr;
            return "No tabix index (.tbi) found for '" + path + "'. "
                   "Index it with: tabix -p <type> '" + path + "'";
        }
        // Comma-separated list of regions
        std::vector<std::string> regs;
        size_t start = 0;
        while (start <= region.size()) {
            size_t comma = region.find(',', start);
            std::string r = region.substr(start,
                comma == std::string::npos ? std::string::npos : comma - start);
            if (!r.empty()) regs.push_back(r);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (regs.empty()) regs.push_back(region);
        // UCSC<->Ensembl human/mouse chrom aliasing: if a region's chromosome
        // isn't among the tabix index's sequence names but its alias is, swap it.
        {
            int nseq = 0;
            const char** seqs = tbx_seqnames(self->tbx_, &nseq);
            std::set<std::string> have;
            for (int i = 0; seqs && i < nseq; ++i) have.insert(seqs[i]);
            free(seqs);
            bool noted = false;
            auto havef = [&](const std::string& n){ return have.count(n) > 0; };
            for (auto& r : regs) {
                size_t colon = r.find(':');
                std::string chrom = (colon == std::string::npos) ? r : r.substr(0, colon);
                std::string res = resolve_chrom(chrom, havef, path, noted);
                if (res != chrom)
                    r = res + (colon == std::string::npos ? std::string() : r.substr(colon));
            }
        }
        for (const auto& r : regs) {
            hts_itr_t* it = tbx_itr_querys(self->tbx_, r.c_str());
            if (!it) return "Cannot query region '" + r + "' in '" + path + "'";
            self->iters_.push_back(it);
        }
        *out = std::move(self);
        return "";
    }

    ~TabixInputStream() override {
        for (auto* it : iters_) if (it) tbx_itr_destroy(it);
        if (tbx_) tbx_destroy(tbx_);
        if (fp_)  hts_close(fp_);
        if (ks_.s) free(ks_.s);
    }

    arrow::Status Close() override { closed_ = true; return arrow::Status::OK(); }
    bool closed() const override { return closed_; }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("TabixInputStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* out) override {
        uint8_t* p = static_cast<uint8_t*>(out);
        int64_t total = 0;
        while (n > 0 && !eof_) {
            if (pos_ >= buf_.size()) {
                if (!fetch_next_line()) break;
            }
            int64_t avail = (int64_t)(buf_.size() - pos_);
            int64_t take  = std::min(n, avail);
            std::memcpy(p, buf_.data() + pos_, (size_t)take);
            pos_ += (size_t)take;
            p += take; n -= take; total += take;
        }
        return total;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(n));
        ARROW_ASSIGN_OR_RAISE(int64_t actual, Read(n, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(actual, false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }
private:
    bool fetch_next_line() {
        while (cur_iter_ < iters_.size()) {
            int r = tbx_itr_next(fp_, tbx_, iters_[cur_iter_], &ks_);
            if (r >= 0) {
                buf_.assign(ks_.s, ks_.l);
                buf_ += '\n';
                pos_ = 0;
                return true;
            }
            ++cur_iter_;
        }
        eof_ = true;
        return false;
    }
};
class RegionScanStream : public arrow::io::InputStream {
public:
    enum class Kind { BED, GFF, VCF, SAM, PAF, Mpileup };
private:
    std::shared_ptr<arrow::io::InputStream> inner_;
    LineReader          lr_;
    Kind                kind_;
    char                delim_;
    std::vector<Region> windows_;
    std::vector<std::string> aliases_;   // chrom_alias of each window's chrom
    std::string         out_buf_;
    size_t              out_pos_    = 0;
    bool                inner_done_ = false;

    static bool to_int(std::string_view v, int64_t* out) {
        if (v.empty()) return false;
        auto r = std::from_chars(v.data(), v.data() + v.size(), *out);
        return r.ec == std::errc() && r.ptr == v.data() + v.size();
    }
    static int64_t cigar_ref_len(std::string_view c) {
        if (c == "*") return 1;
        int64_t len = 0, n = 0;
        for (char ch : c) {
            if (ch >= '0' && ch <= '9') { n = n * 10 + (ch - '0'); continue; }
            if (ch == 'M' || ch == 'D' || ch == 'N' || ch == '=' || ch == 'X') len += n;
            n = 0;
        }
        return len > 0 ? len : 1;
    }
    // The line's span; false when its coordinates do not parse.
    bool span(std::string_view line, std::string_view* chrom,
              int64_t* beg, int64_t* end) const {
        std::string_view f[9];
        int nf = 0;
        size_t start = 0;
        const int want = kind_ == Kind::VCF ? 8 : kind_ == Kind::PAF ? 9
                       : kind_ == Kind::GFF ? 5 : kind_ == Kind::SAM ? 6
                       : kind_ == Kind::BED ? 3 : 2;
        while (nf < want) {
            size_t d = line.find(delim_, start);
            f[nf++] = line.substr(start, d == std::string_view::npos ? d : d - start);
            if (d == std::string_view::npos) break;
            start = d + 1;
        }
        if (nf < want) return false;
        switch (kind_) {
            case Kind::BED:
                *chrom = f[0];
                if (!to_int(f[1], beg) || !to_int(f[2], end)) return false;
                if (*end <= *beg) *end = *beg + 1;
                return true;
            case Kind::GFF:
                *chrom = f[0];
                if (!to_int(f[3], beg) || !to_int(f[4], end)) return false;
                *beg -= 1;
                return true;
            case Kind::VCF: {
                *chrom = f[0];
                if (!to_int(f[1], beg)) return false;
                *beg -= 1;
                *end = *beg + (int64_t)std::max<size_t>(f[3].size(), 1);
                // INFO END= (a symbolic allele's extent), 1-based inclusive.
                std::string_view info = f[7];
                for (size_t p = 0; p < info.size();) {
                    size_t sc = info.find(';', p);
                    std::string_view kv = info.substr(p, sc == std::string_view::npos ? sc : sc - p);
                    int64_t e;
                    if (kv.size() > 4 && kv.substr(0, 4) == "END=" && to_int(kv.substr(4), &e))
                        *end = e;
                    if (sc == std::string_view::npos) break;
                    p = sc + 1;
                }
                return true;
            }
            case Kind::SAM:
                *chrom = f[2];
                if (*chrom == "*" || !to_int(f[3], beg)) return false;
                *beg -= 1;
                *end = *beg + cigar_ref_len(f[5]);
                return true;
            case Kind::PAF:
                *chrom = f[5];
                return to_int(f[7], beg) && to_int(f[8], end);
            case Kind::Mpileup:
                *chrom = f[0];
                if (!to_int(f[1], beg)) return false;
                *end = *beg; *beg -= 1;
                return true;
        }
        return false;
    }
    bool keep(std::string_view line) const {
        if (line.empty()) return true;
        std::string_view chrom;
        int64_t beg, end;
        if (!span(line, &chrom, &beg, &end)) {
            // An unmapped SAM read (RNAME '*') lies in no window.
            return !(kind_ == Kind::SAM && line.find(delim_) != std::string_view::npos &&
                     chrom == "*");
        }
        for (size_t w = 0; w < windows_.size(); ++w) {
            const Region& r = windows_[w];
            if (chrom != r.chrom && (aliases_[w].empty() || chrom != aliases_[w])) continue;
            if (beg < r.end && end > r.start) return true;
        }
        return false;
    }
    bool refill() {
        out_buf_.clear(); out_pos_ = 0;
        std::string line;
        while (out_buf_.empty()) {
            bool ok = lr_.read_line(&line);
            if (!ok && line.empty()) { inner_done_ = true; return false; }
            if (!ok) inner_done_ = true;
            if (keep(line)) { out_buf_ = line; out_buf_ += '\n'; }
            else if (inner_done_) return false;
        }
        return true;
    }

public:
    RegionScanStream(std::shared_ptr<arrow::io::InputStream> inner, Kind kind, char delim,
                     std::vector<Region> windows)
        : inner_(inner), lr_(inner), kind_(kind), delim_(delim), windows_(std::move(windows)) {
        for (const auto& w : windows_) aliases_.push_back(chrom_alias(w.chrom));
    }

    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("RegionScanStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* buf) override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        int64_t total = 0;
        while (n > 0) {
            if (out_pos_ >= out_buf_.size()) {
                if (inner_done_ || !refill()) break;
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
class TruncateFieldsStream : public arrow::io::InputStream {
    std::shared_ptr<arrow::io::InputStream> inner_;
    LineReader   lr_;          // buffered reader — avoids one-byte-at-a-time reads
    int          max_fields_;
    std::string  out_buf_;
    size_t       out_pos_    = 0;
    bool         inner_done_ = false;

    bool refill() {
        out_pos_ = 0;
        do {
            bool ok = lr_.read_line(&out_buf_);
            if (!ok && out_buf_.empty()) { inner_done_ = true; return false; }
            if (!ok) inner_done_ = true;
        } while (out_buf_.empty());
        // Truncate to at most max_fields tab-separated fields
        const char* b = out_buf_.data();
        const char* e = b + out_buf_.size();
        const char* p = b;
        for (int fields = 0; ; ) {
            const char* t = static_cast<const char*>(std::memchr(p, '\t', (size_t)(e - p)));
            if (!t) break;
            if (++fields == max_fields_) { out_buf_.resize((size_t)(t - b)); break; }
            p = t + 1;
        }
        out_buf_ += '\n';
        return true;
    }

public:
    TruncateFieldsStream(std::shared_ptr<arrow::io::InputStream> inner, int max_fields)
        : inner_(inner), lr_(inner), max_fields_(max_fields) {}

    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("TruncateFieldsStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* buf) override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        int64_t total = 0;
        while (n > 0) {
            if (out_pos_ >= out_buf_.size()) {
                if (inner_done_ || !refill()) break;
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
class PafTagsStream : public arrow::io::InputStream {
    std::shared_ptr<arrow::io::InputStream> inner_;
    LineReader               lr_;
    std::vector<std::string> tags_;
    std::string              out_buf_;
    size_t                   out_pos_    = 0;
    bool                     inner_done_ = false;

    bool refill() {
        out_buf_.clear(); out_pos_ = 0;
        std::string line;
        while (line.empty()) {
            bool ok = lr_.read_line(&line);
            if (!ok && line.empty()) { inner_done_ = true; return false; }
            if (!ok) inner_done_ = true;
        }
        std::vector<std::string_view> f;
        for (size_t a = 0;;) {
            size_t b = line.find('\t', a);
            f.emplace_back(std::string_view(line).substr(a, b == std::string::npos ? b : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        for (size_t i = 0; i < 12; ++i) {
            if (i) out_buf_ += '\t';
            if (i < f.size()) out_buf_ += f[i];
        }
        for (const auto& t : tags_) {
            out_buf_ += '\t';
            for (size_t i = 12; i < f.size(); ++i)
                if (f[i].size() >= 5 && f[i].substr(0, 2) == t && f[i][2] == ':' && f[i][4] == ':') {
                    out_buf_ += f[i].substr(5);
                    break;
                }
        }
        out_buf_ += '\n';
        return true;
    }

public:
    PafTagsStream(std::shared_ptr<arrow::io::InputStream> inner, std::vector<std::string> tags)
        : inner_(inner), lr_(inner), tags_(std::move(tags)) {}
    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("PafTagsStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* buf) override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        int64_t total = 0;
        while (n > 0) {
            if (out_pos_ >= out_buf_.size()) {
                if (inner_done_ || !refill()) break;
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
class CollapseWhitespaceStream : public arrow::io::InputStream {
    std::shared_ptr<arrow::io::InputStream> inner_;
    LineReader   lr_;
    std::string  out_buf_;
    size_t       out_pos_    = 0;
    bool         inner_done_ = false;

    bool refill() {
        out_buf_.clear(); out_pos_ = 0;
        std::string line;
        while (out_buf_.empty()) {
            bool ok = lr_.read_line(&line);
            if (!ok && line.empty()) { inner_done_ = true; return false; }
            if (!ok) inner_done_ = true;
            bool gap = false;
            for (char c : line) {
                if (c == ' ' || c == '\t') { gap = !out_buf_.empty(); continue; }
                if (gap) { out_buf_ += ' '; gap = false; }
                out_buf_ += c;
            }
            if (out_buf_.empty() && inner_done_) return false;
        }
        out_buf_ += '\n';
        return true;
    }

public:
    explicit CollapseWhitespaceStream(std::shared_ptr<arrow::io::InputStream> inner)
        : inner_(inner), lr_(inner) {}

    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("CollapseWhitespaceStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* buf) override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        int64_t total = 0;
        while (n > 0) {
            if (out_pos_ >= out_buf_.size()) {
                if (inner_done_ || !refill()) break;
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
// ── Delimited source (CSV / TSV / BED / VCF / GFF3+GTF / SAM, plain or gzip) ──



// Wrap a single RecordBatch column slice as a single-chunk Table.
std::shared_ptr<arrow::Table> batch_slice_to_table(
    const arrow::RecordBatch& batch,
    const std::vector<int>& col_indices,
    const std::shared_ptr<arrow::Schema>& full_schema)
{
    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    arrow::FieldVector fields;
    for (int ci : col_indices) {
        cols.push_back(std::make_shared<arrow::ChunkedArray>(
            arrow::ArrayVector{batch.column(ci)}));
        fields.push_back(full_schema->field(ci));
    }
    return arrow::Table::Make(arrow::schema(fields), cols, batch.num_rows());
}



// Block size for Arrow's streaming CSV / JSON readers: 16 MiB, or
// VV_CSV_BLOCK_MB. A record longer than a block fails with "straddling object
// straddles two block boundaries"; opening retries with larger blocks
// (csv_block_override), and a failure later in the stream names the setting.
static thread_local int64_t csv_block_override = 0;
int64_t csv_block_bytes() {
    if (csv_block_override > 0) return csv_block_override;
    static const int64_t env = [] {
        if (const char* e = std::getenv("VV_CSV_BLOCK_MB")) {
            long long mb = std::atoll(e);
            if (mb > 0) return (int64_t)mb << 20;
        }
        return (int64_t)16 << 20;
    }();
    return env;
}


// True for a leading-zero integer token like "007" / "00" / "012" — a code or
// ID where the zeros are meaningful. "0", "10", "0.5" and "" are NOT flagged, so
// ordinary numeric data is never forced to string. Used to keep such columns as
// utf8 instead of letting Arrow's CSV inference drop the zeros ("007" -> 7).
static bool is_leading_zero_int(const std::string& s) {
    if (s.size() < 2 || s[0] != '0') return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

// True if `s` is a plain decimal / scientific number token (digits, sign, dot,
// e/E only) that strtod fully consumes — rejects the word-like / hex forms strtod
// also accepts (nan, inf, 0x…). Shared by the headerless-CSV heuristic and the
// leading-zero pre-scan so both agree on whether row 0 is a header.
static bool looks_like_plain_number(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isdigit(c) && c != '+' && c != '-' &&
            c != '.' && c != 'e' && c != 'E')
            return false;
    char* ep; std::strtod(s.c_str(), &ep);
    return *ep == '\0';
}

// The boolean spellings Arrow's CSV type inference accepts. Used by header
// detection: a value like this in a column Arrow inferred as bool is a datum,
// not a column label.
static bool is_bool_token(const std::string& s) {
    return s == "true" || s == "false" || s == "TRUE" ||
           s == "FALSE" || s == "True" || s == "False";
}

// Split one delimited line into fields, honouring Arrow's default CSV quoting: a
// field that starts with '"' is quoted until the next unescaped '"', a doubled
// '""' inside is a literal quote, and the delimiter is literal inside quotes.
// So a quoted field containing the delimiter doesn't shift column positions.
void split_delimited_line(const std::string& line, char delim,
                          std::vector<std::string>* out) {
    out->clear();
    std::string field;
    size_t i = 0, n = line.size();
    while (i < n) {
        field.clear();
        if (line[i] == '"') {                 // quoted field
            ++i;
            while (i < n) {
                if (line[i] == '"') {
                    if (i + 1 < n && line[i + 1] == '"') { field += '"'; i += 2; }
                    else { ++i; break; }       // closing quote
                } else field += line[i++];
            }
            while (i < n && line[i] != delim) ++i;   // skip to delimiter
        } else {
            while (i < n && line[i] != delim) field += line[i++];
        }
        out->push_back(field);
        if (i < n && line[i] == delim) {
            ++i;
            if (i == n) out->push_back("");   // trailing delimiter → empty field
        }
    }
}

// From a sample of delimited data lines + column names, return the names of
// columns that contain a leading-zero integer value (so they should be read as
// utf8, not inferred numeric). Lines are tokenised quote-aware; a column is
// flagged if any sampled value is is_leading_zero_int().
std::vector<std::string>
leading_zero_columns(const std::vector<std::string>& sample_lines, char delim,
              const std::vector<std::string>& col_names) {
    std::vector<char> flagged(col_names.size(), 0);
    std::vector<std::string> fields;
    for (const auto& line : sample_lines) {
        split_delimited_line(line, delim, &fields);
        for (size_t c = 0; c < fields.size() && c < flagged.size(); ++c)
            if (!flagged[c] && is_leading_zero_int(fields[c])) flagged[c] = 1;
    }
    std::vector<std::string> out;
    for (size_t c = 0; c < col_names.size(); ++c)
        if (flagged[c]) out.push_back(col_names[c]);
    return out;
}

class DelimitedSource : public TabularSource {
    // Name given to R's unnamed leading index / row-names column (see
    // detect_rstyle_index_header).
    static constexpr const char* kRowIndexColName = "index";

    std::string                           path_;
    char                                  delimiter_;
    DelimKind                             kind_;
    TsvDialect                            dialect_ = TsvDialect::None;
    std::vector<std::string>              paf_tags_;     // --tags on a PAF
    std::vector<char>                     paf_tag_types_; // SAM type of each (i f A Z …)
    std::shared_ptr<arrow::Schema>        schema_;
    std::vector<std::string>              preamble_lines_;
    int                                   bed_level_ = 3; // detected BED standard cols (3..9)
    BedVariant                            bed_variant_ = BedVariant::None;
    int                                   mpileup_samples_ = 0; // samtools mpileup samples (>=1)
    // MatrixMarket (DelimKind::Mtx): banner field (integer / real / pattern) and
    // the size line. Entries are checked against these as they stream.
    std::string                           mtx_field_;
    int64_t                               mtx_rows_ = 0, mtx_cols_ = 0, mtx_nnz_ = 0;
    HeaderMode                            header_mode_ = HeaderMode::Auto; // -d/--header
    bool                                  auto_headerless_ = false; // Auto-detected: row 0 is data
    std::string                           format_note_;   // extra footer text (e.g. 10x sidecar)

    // Decoded batches in a bounded trailing window: older entries are freed
    // (set null) once retain_all_ is false and the window overflows. Per-batch
    // metadata below is kept for every batch, evicted or not.
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>          batch_first_row_;
    mutable std::vector<int64_t>          batch_num_rows_;  // retained after eviction
    mutable int64_t                       rows_so_far_ = 0;
    mutable bool                          all_read_    = false;
    mutable bool                          retain_all_  = false; // pinned by full-pass ops
    mutable bool                          evicted_any_ = false; // any batch freed?
    // True when the data stream was replaced by a tabix iterator for -r.
    bool                                  region_applied_ = false;
    mutable arrow::Status                 read_status_;   // sticky stream error

    // Append a freshly-decoded batch via the shared bounded-window helper.
    void retain_(std::shared_ptr<arrow::RecordBatch> batch) const {
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
    }

    mutable std::shared_ptr<arrow::csv::StreamingReader> reader_;
    std::shared_ptr<ReadGate>             gate_;   // stops reader_'s read-ahead on close

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        std::shared_ptr<arrow::RecordBatch> batch;
        arrow::Status st = reader_->ReadNext(&batch);
        if (!st.ok()) {
            // A malformed row (bad column count, encoding, …) anywhere past the
            // first block surfaces here. Record it stickily and stop: callers
            // that ignore advance()'s result (ensure()) must not spin forever,
            // and the CLI can report the truncation and exit non-zero instead
            // of silently emitting a partial result with status 0.
            read_status_ = st;
            all_read_ = true;
            return st;
        }
        if (!batch) {
            all_read_ = true;
            if (kind_ == DelimKind::Mtx && rows_so_far_ != mtx_nnz_) {
                read_status_ = arrow::Status::Invalid(
                    "MatrixMarket size line declares ", mtx_nnz_,
                    " entries, but the file holds ", rows_so_far_);
                return read_status_;
            }
            return arrow::Status::OK();
        }
        if (kind_ == DelimKind::Mtx) {
            auto shifted = mtx_to_zero_based(batch);
            if (!shifted.ok()) {
                read_status_ = shifted.status();
                all_read_ = true;
                return read_status_;
            }
            batch = *shifted;
        }
        retain_(std::move(batch));
        return arrow::Status::OK();
    }

    // MatrixMarket indices are 1-based; present them 0-based (like scipy.io.mmread)
    // and reject any entry outside the declared shape rather than show it.
    arrow::Result<std::shared_ptr<arrow::RecordBatch>>
    mtx_to_zero_based(const std::shared_ptr<arrow::RecordBatch>& b) const {
        std::vector<std::shared_ptr<arrow::Array>> cols = b->columns();
        const int64_t limit[2] = {mtx_rows_, mtx_cols_};
        const char* axis[2] = {"row", "col"};
        for (int c = 0; c < 2; ++c) {
            if (cols[(size_t)c]->type_id() != arrow::Type::INT64)
                return arrow::Status::Invalid("MatrixMarket ", axis[c],
                                              " index is not an integer");
            const auto& in = static_cast<const arrow::Int64Array&>(*cols[(size_t)c]);
            arrow::Int64Builder bld;
            ARROW_RETURN_NOT_OK(bld.Reserve(in.length()));
            for (int64_t i = 0; i < in.length(); ++i) {
                int64_t v = in.IsNull(i) ? 0 : in.Value(i);
                if (v < 1 || v > limit[c])
                    return arrow::Status::Invalid(
                        "MatrixMarket entry ", rows_so_far_ + i + 1, ": ", axis[c],
                        " index ", in.IsNull(i) ? std::string("missing") : std::to_string(v),
                        " is outside 1..", limit[c]);
                bld.UnsafeAppend(v - 1);
            }
            ARROW_RETURN_NOT_OK(bld.Finish(&cols[(size_t)c]));
        }
        return arrow::RecordBatch::Make(b->schema(), b->num_rows(), cols);
    }

    // Open the file as a (possibly decompressed) InputStream.
    static std::string open_stream(const std::string& path, bool is_gz,
                                    std::shared_ptr<arrow::io::ReadableFile>*  raw_out,
                                    std::shared_ptr<arrow::io::InputStream>*   input_out) {
        auto maybe_raw = arrow::io::ReadableFile::Open(path);
        if (!maybe_raw.ok())
            return "Cannot open '" + path + "': " + maybe_raw.status().ToString();
        auto raw = maybe_raw.ValueOrDie();
        std::shared_ptr<arrow::io::InputStream> input = raw;
        if (is_gz) {   // "is_gz" == the stream is compressed; the codec is in its magic.
            if (auto e = decode_stream(sniff_file_codec(raw), raw, &input); !e.empty())
                return "'" + path + "': " + e;
        }
        *raw_out   = raw;
        *input_out = input;
        return "";
    }

    // Create a StreamingReader from an already-open stream.
    static arrow::Result<std::shared_ptr<arrow::csv::StreamingReader>>
    make_reader(std::shared_ptr<arrow::io::InputStream> input, char delim,
                bool autogen_names, const std::vector<std::string>& col_names,
                const std::vector<std::string>& force_string_cols = {},
                bool strings_nullable = false,
                const std::vector<std::pair<std::string, std::shared_ptr<arrow::DataType>>>&
                    column_types = {},
                std::shared_ptr<ReadGate>* gate_out = nullptr) {
        if (gate_out) {
            *gate_out = std::make_shared<ReadGate>();
            input = std::make_shared<GatedInputStream>(std::move(input), *gate_out);
        }
        auto ropts = arrow::csv::ReadOptions::Defaults();
        // 16 MiB blocks + per-block parsing on the CPU pool. The default
        // (~1 MiB) is too small for multi-GB files; raising it amortises
        // tokenizer overhead and lets parsing parallelise across threads.
        ropts.block_size  = (int32_t)std::min<int64_t>(csv_block_bytes(), INT32_MAX);
        ropts.use_threads = true;
        if (!col_names.empty())
            ropts.column_names = col_names;
        else
            ropts.autogenerate_column_names = autogen_names;
        auto popts = arrow::csv::ParseOptions::Defaults();
        popts.delimiter = delim;
        auto copts = arrow::csv::ConvertOptions::Defaults();
        // Missing values in string columns. Arrow only nulls a string cell when
        // strings_can_be_null is set; otherwise a bare NA / NULL / empty field
        // is kept as that literal text. R (and pandas) write a missing value as
        // an unquoted token and quote a genuine string, so honour that: an
        // *unquoted* null token (NA, NULL, NaN, empty, …) becomes null while a
        // *quoted* one ("NA") stays the literal string — a column whose real
        // value is "NA" (e.g. Namibia's country code) survives as long as it is
        // quoted. Only for user CSV/TSV; the fixed-schema genomics formats
        // (BED/VCF/GFF/SAM/PAF) keep their own missing conventions.
        if (strings_nullable) {
            copts.strings_can_be_null        = true;
            copts.quoted_strings_can_be_null = false;
        }
        // Force the detected leading-zero-ID columns to utf8 so inference can't
        // drop the zeros ("007" -> 7). Keyed by name (Arrow has no by-index
        // override); a no-op for a column Arrow would have made string anyway.
        for (const auto& ct : column_types) copts.column_types[ct.first] = ct.second;
        for (const auto& name : force_string_cols)
            copts.column_types[name] = arrow::utf8();
        return arrow::csv::StreamingReader::Make(
            arrow::io::default_io_context(), input, ropts, popts, copts);
    }

    // Re-read a small sample of the file and return the names of columns that
    // hold a leading-zero integer (so they can be forced to utf8 — Arrow's CSV
    // inference would otherwise turn "007" into 7). Mirrors the primary reader's
    // naming so the forced-string names line up: a `#`-header supplies the names
    // (header_names), otherwise the first non-comment line is the header. The
    // leading-zero scan reads the data lines after it. Best-effort: {} on any
    // I/O / open problem; column_types entries that don't match are simply
    // ignored by Arrow, never an error.
    static std::vector<std::string>
    detect_leading_zero_columns(const std::string& path, bool is_gz, char delim,
                                const std::vector<std::string>& header_names,
                                bool headerless = false) {
        std::shared_ptr<arrow::io::ReadableFile> raw;
        std::shared_ptr<arrow::io::InputStream>  input;
        if (!open_stream(path, is_gz, &raw, &input).empty()) return {};
        LineReader lr(input);
        std::vector<std::string> names = header_names;   // from a `#`-header, if any
        // Headerless mode: every data line (including the first) is data, and
        // columns are named f0, f1, … to match Arrow's autogenerate_column_names
        // so the forced-string set lines up with the retried reader's schema.
        bool have_names = !names.empty() || headerless;
        std::vector<std::string> sample;
        std::string line;
        const int kMaxData = 200;
        for (;;) {
            bool ok = lr.read_line(&line);
            if (!ok && line.empty()) break;              // true EOF
            if (!line.empty() && line[0] != '#') {       // skip blank + comment
                if (!have_names) {                       // first data line = header
                    split_delimited_line(line, delim, &names);
                    have_names = true;
                } else {
                    sample.push_back(line);
                    if ((int)sample.size() >= kMaxData) break;
                }
            }
            if (!ok) break;
        }
        if (headerless) {
            size_t ncols = 0;
            std::vector<std::string> f;
            for (const auto& s : sample) {
                split_delimited_line(s, delim, &f);
                ncols = std::max(ncols, f.size());
            }
            names.clear();
            for (size_t i = 0; i < ncols; ++i)
                names.push_back("f" + std::to_string(i));
        }
        if (names.empty()) return {};
        return leading_zero_columns(sample, delim, names);
    }

    // R's write.table(sep="\t") / write.csv() emit an index (row-names) column
    // with no matching header field, so the header row has exactly one fewer
    // field than every data row ("x\ty\tz" over "row1\t1\t2\t3"). Arrow's CSV
    // reader takes the short row as the column list and then rejects the wider
    // data rows ("Expected N columns, got N+1"), making the file unreadable.
    // Detect that shape by reading the header and the first data line: return
    // true when the header has one fewer field than the data (and at least one
    // field), so the caller can inject a name for the leading index column.
    // Best-effort: false on any I/O problem, EOF before two lines, or a header
    // that already matches the data width. (This only runs when no explicit
    // header was found, so a '#'-header or an already-empty leading name — R's
    // write.csv writes `""` there, which Arrow reads fine — never reaches here.)
    static bool detect_rstyle_index_header(const std::string& path, bool is_gz,
                                           char delim) {
        std::shared_ptr<arrow::io::ReadableFile> raw;
        std::shared_ptr<arrow::io::InputStream>  input;
        if (!open_stream(path, is_gz, &raw, &input).empty()) return false;
        LineReader lr(input);
        std::string line;
        auto next_content_line = [&](std::string* dst) -> bool {
            for (;;) {
                bool ok = lr.read_line(&line);
                if (!ok && line.empty()) return false;   // true EOF
                if (!line.empty() && line[0] != '#') { *dst = line; return true; }
                if (!ok) return false;                   // EOF after only comments
            }
        };
        std::string header, data;
        if (!next_content_line(&header)) return false;
        if (!next_content_line(&data))   return false;
        std::vector<std::string> hf, df;
        split_delimited_line(header, delim, &hf);
        split_delimited_line(data,   delim, &df);
        return hf.size() >= 1 && df.size() == hf.size() + 1;
    }

public:
    static std::string open(const std::string& path, DelimKind kind,
                             std::unique_ptr<DelimitedSource>* out) {
        return open(path, kind, /*region=*/"", out);
    }
    // The first line of `path` that does not start with "##" (decompressed
    // for .gz / .zst); "" when there is none or the file cannot be read.
    static std::string first_line_after_meta(const std::string& path) {
        const bool is_gz = has_compression_suffix(path);
        std::shared_ptr<arrow::io::ReadableFile> raw;
        std::shared_ptr<arrow::io::InputStream>  input;
        if (!open_stream(path, is_gz, &raw, &input).empty()) return "";
        LineReader lr(input);
        std::string line;
        for (;;) {
            bool ok = lr.read_line(&line);
            if (line.rfind("##", 0) != 0) return line;
            if (!ok) return "";
        }
    }
    // The SAM type character of each tag in `tags`, from its first occurrence
    // in the first 1000 lines of a PAF ('Z' when it does not occur there).
    static std::vector<char> paf_tag_types(const std::string& path,
                                           const std::vector<std::string>& tags) {
        std::vector<char> types(tags.size(), 0);
        const bool is_gz = has_compression_suffix(path);
        std::shared_ptr<arrow::io::ReadableFile> raw;
        std::shared_ptr<arrow::io::InputStream>  input;
        if (open_stream(path, is_gz, &raw, &input).empty()) {
            LineReader lr(input);
            std::string line;
            for (int n = 0; n < 1000 && (lr.read_line(&line) || !line.empty()); ++n) {
                for (size_t a = 0, k = 0;; ++k) {
                    size_t b = line.find('\t', a);
                    if (k >= 12) {
                        std::string_view f = std::string_view(line).substr(a, b == std::string::npos ? b : b - a);
                        for (size_t t = 0; t < tags.size(); ++t)
                            if (!types[t] && f.size() >= 5 && f.substr(0, 2) == tags[t] && f[2] == ':' && f[4] == ':')
                                types[t] = f[3];
                    }
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
                line.clear();
            }
        }
        for (auto& t : types) if (!t) t = 'Z';
        return types;
    }
    // The first line of `path` (decompressed for .gz / .zst); "" if unreadable.
    static std::string first_line_after_meta_raw(const std::string& path) {
        const bool is_gz = has_compression_suffix(path);
        std::shared_ptr<arrow::io::ReadableFile> raw;
        std::shared_ptr<arrow::io::InputStream>  input;
        if (!open_stream(path, is_gz, &raw, &input).empty()) return "";
        LineReader lr(input);
        std::string line;
        lr.read_line(&line);
        return line;
    }
    // Give the leading columns of a file read headerless their names (the
    // rest keep f<i>) and note in the footer what the file is.
    void apply_column_names(const std::vector<std::string>& names, std::string note) {
        arrow::FieldVector fields = schema_->fields();
        for (size_t i = 0; i < fields.size() && i < names.size(); ++i)
            fields[i] = fields[i]->WithName(names[i]);
        schema_ = arrow::schema(fields);
        format_note_ = std::move(note);
    }
    // Open a delimited source from an already-built InputStream.
    // `path_label` is used for error messages and as the displayed file name
    // (typically "-" for stdin). `is_gz` controls whether the gzip-decompression
    // wrapper has *already* been applied — set to true so the seekability
    // checks behave like for `.gz` files.
    static std::string open_from_stream(
        std::shared_ptr<arrow::io::InputStream> input,
        const std::string& path_label, DelimKind kind, bool is_gz,
        const std::string& region,
        std::unique_ptr<DelimitedSource>* out, char delim_override = 0,
        HeaderMode header_mode = HeaderMode::Auto) {
        auto self = std::make_unique<DelimitedSource>();
        self->path_      = path_label;
        self->kind_      = kind;
        self->header_mode_ = header_mode;
        self->delimiter_ = delim_override ? delim_override
                                          : (kind == DelimKind::CSV ? ',' : kind == DelimKind::Mtx ? ' ' : '\t');
        return continue_open(std::move(self), std::move(input),
                             /*raw=*/nullptr, is_gz, kind, region, out);
    }
    // `delim_override` (non-zero) forces the input field separator, overriding
    // the kind's default (used by -d/--in-delimiter); pass kind CSV or TSV so
    // the CSV/TSV code paths — header auto-detect, leading-zero guard — apply.
    // `header_mode` overrides CSV/TSV header detection (On/Off) or leaves it Auto.
    // Open, retrying with 4x larger read blocks (up to 1 GiB) while the first
    // block cannot hold a whole record — a line longer than the block.
    static std::string open(const std::string& path, DelimKind kind,
                             const std::string& region,
                             std::unique_ptr<DelimitedSource>* out,
                             char delim_override = 0,
                             HeaderMode header_mode = HeaderMode::Auto,
                             TsvDialect dialect = TsvDialect::None,
                             const std::vector<std::string>& paf_tags = {}) {
        std::string err;
        for (int64_t block = csv_block_bytes();; block *= 4) {
            const int64_t saved = csv_block_override;
            csv_block_override = block;
            err = open_once(path, kind, region, out, delim_override, header_mode, dialect,
                            paf_tags);
            csv_block_override = saved;
            if (err.find("straddling object") == std::string::npos || block >= ((int64_t)1 << 30))
                break;
            out->reset();
        }
        return err;
    }
    static std::string open_once(const std::string& path, DelimKind kind,
                             const std::string& region,
                             std::unique_ptr<DelimitedSource>* out,
                             char delim_override, HeaderMode header_mode,
                             TsvDialect dialect,
                             const std::vector<std::string>& paf_tags) {
        auto self = std::make_unique<DelimitedSource>();
        self->path_      = path;
        self->kind_      = kind;
        self->dialect_   = dialect;
        self->paf_tags_  = paf_tags;
        if (!paf_tags.empty()) self->paf_tag_types_ = paf_tag_types(path, paf_tags);
        self->header_mode_ = header_mode;
        self->delimiter_ = delim_override ? delim_override
                                          : (kind == DelimKind::CSV ? ',' : kind == DelimKind::Mtx ? ' ' : '\t');

        // "is_gz" means the byte stream is compressed (non-seekable). Both a
        // gzip (.gz) and a zstandard (.zst / .zstd) wrapper qualify; open_stream
        // selects the matching codec.
        bool is_gz = has_compression_suffix(path);

        std::shared_ptr<arrow::io::ReadableFile>  raw;
        std::shared_ptr<arrow::io::InputStream>   input;
        {
            std::string err = open_stream(path, is_gz, &raw, &input);
            if (!err.empty()) return err;
        }
        return continue_open(std::move(self), std::move(input),
                             std::move(raw), is_gz, kind, region, out);
    }

    // Stop and drain the reader's read-ahead (see ReadGate).
    ~DelimitedSource() override { close_stream_reader(reader_, gate_); }

    // Apply ENCODE peak-family naming on top of the vanilla BED schema.
    // The MatrixMarket size line (rows, cols); false for any other kind.
    bool mtx_shape(int64_t* rows, int64_t* cols) const {
        if (kind_ != DelimKind::Mtx) return false;
        *rows = mtx_rows_; *cols = mtx_cols_;
        return true;
    }

    // Name the columns of a 10x Genomics sidecar file read headerless (see
    // tenx_sidecar_kind): f0, f1, … become barcode / id, name, feature_type,
    // … ; any extra columns keep their f<i> names. Rewrites schema_ in place.
    void apply_tenx_sidecar(int kind) {
        static const char* const kBarcode[]  = {"barcode"};
        static const char* const kFeatures[] = {"id", "name", "feature_type",
                                                "chrom", "start", "end"};
        const char* const* names = (kind == 1) ? kBarcode : kFeatures;
        const int n_names = (kind == 1) ? 1 : (kind == 3 ? 2 : 6);
        arrow::FieldVector fields = schema_->fields();
        for (int i = 0; i < (int)fields.size() && i < n_names; ++i)
            fields[(size_t)i] = fields[(size_t)i]->WithName(names[i]);
        schema_ = arrow::schema(fields);
        format_note_ = std::string("10x Genomics ") +
                       (kind == 1 ? "barcodes" : kind == 2 ? "features" : "genes") +
                       " file (no header row)";
    }

    // Called by open_source's BED dispatch when the file extension picks
    // a specific variant (narrowPeak / broadPeak / gappedPeak / bedGraph
    // / tagAlign). Rewrites schema_ in place; safe to call after open().
    void apply_bed_variant(BedVariant v) {
        bed_variant_ = v;
        if (v == BedVariant::None) return;
        const int nf = schema_->num_fields();
        std::vector<std::string> names;
        switch (v) {
            case BedVariant::NarrowPeak:
                names = {"Chr","[Beg","End)","Name","Score","Str",
                         "signalValue","pValue","qValue","peak"};
                break;
            case BedVariant::BroadPeak:
                names = {"Chr","[Beg","End)","Name","Score","Str",
                         "signalValue","pValue","qValue"};
                break;
            case BedVariant::GappedPeak:
                names = {"Chr","[Beg","End)","Name","Score","Str",
                         "ThBeg","ThEnd","RGB","NBlk","BlkSz","BlkSt",
                         "signalValue","pValue","qValue"};
                break;
            case BedVariant::BedGraph:
                names = {"Chr","[Beg","End)","value"};
                break;
            case BedVariant::TagAlign:
                names = {"Chr","[Beg","End)","sequence","Score","Str"};
                break;
            case BedVariant::None: return;
        }
        arrow::FieldVector fields;
        fields.reserve(nf);
        for (int i = 0; i < nf; ++i) {
            auto f = schema_->field(i);
            std::string nm = (i < (int)names.size())
                ? names[i]
                : ("+" + std::to_string(i - (int)names.size() + 1));
            fields.push_back(arrow::field(nm, f->type(), f->nullable()));
        }
        schema_ = arrow::schema(fields);
        // bed_level_ drives footer formatting; track the "standard cols" count.
        bed_level_ = (int)names.size();
    }
private:
    // Validate a MatrixMarket banner (preamble_lines_[0]) and consume the size
    // line from the front of *put_back. Only the layout vv presents faithfully is
    // accepted: `matrix coordinate`, field integer / real / pattern, symmetry
    // general. Symmetric files store one triangle, so a coordinate listing of
    // them would silently show half the entries.
    static std::string parse_mtx_header(DelimitedSource* self, std::string* put_back) {
        if (self->preamble_lines_.empty())
            return "not a MatrixMarket file (no %%MatrixMarket banner)";
        std::istringstream bs(self->preamble_lines_.front());
        std::string tag, object, format, field, symmetry;
        bs >> tag >> object >> format >> field >> symmetry;
        auto lc = [](std::string v) {
            for (char& c : v) c = (char)std::tolower((unsigned char)c);
            return v;
        };
        object = lc(object); format = lc(format); field = lc(field); symmetry = lc(symmetry);
        if (tag != "%%MatrixMarket" || object != "matrix")
            return "not a MatrixMarket matrix (banner: '" + self->preamble_lines_.front() + "')";
        if (format != "coordinate")
            return "MatrixMarket '" + format + "' (dense) format is not supported; "
                   "only 'coordinate' (sparse) files are read";
        if (field != "integer" && field != "real" && field != "pattern")
            return "MatrixMarket field '" + field + "' is not supported "
                   "(integer, real and pattern are)";
        if (symmetry != "general")
            return "MatrixMarket symmetry '" + symmetry + "' is not supported: the file "
                   "stores one triangle, so listing its entries would show half the "
                   "matrix; only 'general' is read";
        self->mtx_field_ = field;
        // The size line: the first non-blank line after the banner and comments.
        std::string& pb = *put_back;
        for (;;) {
            size_t nl = pb.find('\n');
            std::string line = pb.substr(0, nl);
            pb.erase(0, nl == std::string::npos ? pb.size() : nl + 1);
            if (line.find_first_not_of(" \t\r") == std::string::npos) {
                if (pb.empty()) return "MatrixMarket file has no size line";
                continue;
            }
            std::istringstream ls(line);
            std::string extra;
            if (!(ls >> self->mtx_rows_ >> self->mtx_cols_ >> self->mtx_nnz_) || (ls >> extra) ||
                self->mtx_rows_ < 0 || self->mtx_cols_ < 0 || self->mtx_nnz_ < 0)
                return "malformed MatrixMarket size line '" + line +
                       "' (expected: rows cols entries)";
            return "";
        }
    }

    // Shared body of open() / open_from_stream(). Strips per-format preamble,
    // optionally swaps the data stream for a tabix iterator, builds the
    // streaming CSV reader, and finalises the schema.
    static std::string continue_open(
        std::unique_ptr<DelimitedSource> self,
        std::shared_ptr<arrow::io::InputStream> input,
        std::shared_ptr<arrow::io::ReadableFile> /*raw*/,
        bool is_gz, DelimKind kind, const std::string& region,
        std::unique_ptr<DelimitedSource>* out) {
        const std::string& path = self->path_;
        // Format-specific preamble stripping and column-name determination.
        // GFF and SAM wrap the stream in TruncateFieldsStream to handle variable columns.
        std::vector<std::string> col_names;
        std::string put_back;

        switch (kind) {
            case DelimKind::BED:
                self->preamble_lines_ = strip_bed_preamble(input, &put_back);
                break;
            case DelimKind::VCF:
                self->preamble_lines_ = strip_vcf_preamble(input, &col_names, &put_back);
                break;
            case DelimKind::GFF: {
                self->preamble_lines_ = strip_prefix_preamble(input, '#', &put_back);
                col_names = {"seqname","source","feature","start","end",
                             "score","strand","frame","attributes"};
                // Rebuild input with put_back then truncate to 9 fields
                std::shared_ptr<arrow::io::InputStream> base =
                    put_back.empty() ? input
                    : std::shared_ptr<arrow::io::InputStream>(
                        std::make_shared<PrependInputStream>(std::move(put_back), input));
                input = std::make_shared<TruncateFieldsStream>(base, 9);
                put_back.clear();
                break;
            }
            case DelimKind::SAM: {
                self->preamble_lines_ = strip_prefix_preamble(input, '@', &put_back);
                col_names = {"QNAME","FLAG","RNAME","POS","MAPQ",
                             "CIGAR","RNEXT","PNEXT","TLEN","SEQ","QUAL"};
                std::shared_ptr<arrow::io::InputStream> base =
                    put_back.empty() ? input
                    : std::shared_ptr<arrow::io::InputStream>(
                        std::make_shared<PrependInputStream>(std::move(put_back), input));
                input = std::make_shared<TruncateFieldsStream>(base, 11);
                put_back.clear();
                break;
            }
            case DelimKind::Mtx: {
                // MatrixMarket: `%%MatrixMarket matrix coordinate <field>
                // <symmetry>`, `%` comment lines, a `rows cols nnz` size line,
                // then one whitespace-separated `i j [value]` entry per line.
                self->preamble_lines_ = strip_prefix_preamble(input, '%', &put_back);
                std::string err = parse_mtx_header(self.get(), &put_back);
                if (!err.empty()) return err;
                col_names = {"row", "col"};
                if (self->mtx_field_ != "pattern") col_names.push_back("value");
                std::shared_ptr<arrow::io::InputStream> base =
                    put_back.empty() ? input
                    : std::shared_ptr<arrow::io::InputStream>(
                        std::make_shared<PrependInputStream>(std::move(put_back), input));
                input = std::make_shared<CollapseWhitespaceStream>(base);
                put_back.clear();
                break;
            }
            case DelimKind::PAF: {
                // PAF (minimap2): 12 mandatory tab-separated columns followed
                // by zero or more optional `tag:type:value` tokens. No header.
                col_names = {"qname","qlen","qstart","qend","strand",
                             "tname","tlen","tstart","tend",
                             "nmatch","alen","mapq"};
                if (self->paf_tags_.empty()) {
                    input = std::make_shared<TruncateFieldsStream>(input, 12);
                } else {
                    input = std::make_shared<PafTagsStream>(input, self->paf_tags_);
                    for (const auto& t : self->paf_tags_) col_names.push_back(t);
                }
                put_back.clear();
                break;
            }
            case DelimKind::CSV:
            case DelimKind::TSV:
                if (self->dialect_ == TsvDialect::Gct) {
                    self->preamble_lines_ = strip_gct_preamble(input, &put_back, &col_names);
                    self->format_note_ = "GenePattern GCT" +
                        (self->preamble_lines_.empty() ? std::string()
                                                       : " " + self->preamble_lines_[0].substr(1));
                } else if (self->dialect_ == TsvDialect::Pairs) {
                    self->preamble_lines_ = strip_pairs_preamble(input, &put_back, &col_names);
                    self->format_note_ = "4DN pairs";
                } else {
                    if (self->dialect_ == TsvDialect::Maf) self->format_note_ = "mutation annotation format (MAF)";
                    self->preamble_lines_ = strip_tsv_csv_preamble(
                        input, self->delimiter_, &put_back, &col_names);
                }
                break;
            default:
                break;
        }

        // BED non-gz: put_back still handled here; GFF/SAM already cleared it above.
        if (!put_back.empty())
            input = std::make_shared<PrependInputStream>(std::move(put_back), input);

        // Tabix range query: drop the file-driven data stream and replace it
        // with a tabix iterator that yields only records overlapping `region`.
        // Preamble + column names already came from the original file above.
        if (!region.empty() && !tabix_index_exists(path)) {
            // No index: scan the whole file and keep the lines a tabix query
            // would return. CSV / TSV name their coordinate columns only in
            // the header Arrow reads later, so they still need an index.
            std::optional<RegionScanStream::Kind> scan;
            switch (kind) {
                case DelimKind::BED:     scan = RegionScanStream::Kind::BED; break;
                case DelimKind::GFF:     scan = RegionScanStream::Kind::GFF; break;
                case DelimKind::VCF:     scan = RegionScanStream::Kind::VCF; break;
                case DelimKind::SAM:     scan = RegionScanStream::Kind::SAM; break;
                case DelimKind::PAF:     scan = RegionScanStream::Kind::PAF; break;
                case DelimKind::Mpileup: scan = RegionScanStream::Kind::Mpileup; break;
                default: break;
            }
            if (!scan)
                return "No tabix index (.tbi / .csi) found for '" + path + "'; -r on a "
                       "CSV / TSV file needs one (bgzip it, then tabix -s <col> -b <col> "
                       "-e <col>), or select rows with --filter";
            std::fprintf(stderr, "vv: %s: no tabix index; -r reads the whole file\n",
                         path.c_str());
            input = std::make_shared<RegionScanStream>(
                input, *scan, self->delimiter_, parse_region_list(region));
            self->region_applied_ = true;
        } else if (!region.empty()) {
            // `region` is canonical 0-based half-open; tbx_itr_querys reads its
            // string argument as 1-based inclusive, so convert at the boundary.
            std::shared_ptr<TabixInputStream> tabix;
            std::string err =
                TabixInputStream::open(path, regions_to_htslib(region), &tabix);
            if (!err.empty()) return err;
            std::shared_ptr<arrow::io::InputStream> ti = tabix;
            if (kind == DelimKind::GFF) ti = std::make_shared<TruncateFieldsStream>(ti, 9);
            if (kind == DelimKind::SAM) ti = std::make_shared<TruncateFieldsStream>(ti, 11);
            input = ti;
            self->region_applied_ = true;
        }

        // R row-names / index convention: header shorter than the data by one
        // field (the leading index column is unnamed). Inject a name for that
        // column so Arrow's header row matches the data width, instead of
        // failing with "Expected N columns, got N+1". Only for a plain CSV/TSV
        // whose header Arrow auto-detects (col_names still empty) and not under
        // a tabix region query. The injected `<name><delim>` sits at the front
        // of the stream, so the first (header) line gains the extra field.
        if ((kind == DelimKind::CSV || kind == DelimKind::TSV) &&
            col_names.empty() && region.empty() &&
            detect_rstyle_index_header(path, is_gz, self->delimiter_)) {
            std::string inject = std::string(kRowIndexColName) + self->delimiter_;
            input = std::make_shared<PrependInputStream>(std::move(inject), input);
        }

        bool autogen = (kind == DelimKind::BED) ||
                       (kind == DelimKind::Mpileup);
        // Leading-zero IDs: a CSV/TSV column like "007" would otherwise be
        // inferred as int and lose the zeros. Pre-scan a sample (keyed by the
        // same column names the reader uses) and force those columns to utf8.
        // Other formats have fixed schemas where this can't arise.
        std::vector<std::string> force_string;
        if (kind == DelimKind::CSV || kind == DelimKind::TSV)
            force_string = detect_leading_zero_columns(path, is_gz,
                                                       self->delimiter_, col_names);
        bool strings_nullable = (kind == DelimKind::CSV || kind == DelimKind::TSV) ||
                                !self->paf_tags_.empty();    // a PAF record without the tag
        std::vector<std::pair<std::string, std::shared_ptr<arrow::DataType>>> col_types;
        // PAF tag columns take the tag's SAM type: i -> int64, f -> double, else text.
        for (size_t t = 0; t < self->paf_tags_.size(); ++t) {
            const char ty = t < self->paf_tag_types_.size() ? self->paf_tag_types_[t] : 'Z';
            col_types.push_back({self->paf_tags_[t], ty == 'i' ? arrow::int64()
                                                    : ty == 'f' ? arrow::float64() : arrow::utf8()});
        }
        // BLAST tabular (standard 12 columns, read headerless as f0..): percent
        // identity, e-value and bit score are reals even when the first block
        // happens to hold only whole numbers ("259" before "45.8").
        if (self->dialect_ == TsvDialect::Blast)
            for (const char* f : {"f2", "f10", "f11"}) col_types.push_back({f, arrow::float64()});
        if (kind == DelimKind::Mtx) {
            col_types = {{"row", arrow::int64()}, {"col", arrow::int64()}};
            if (self->mtx_field_ == "integer") col_types.push_back({"value", arrow::int64()});
            if (self->mtx_field_ == "real")    col_types.push_back({"value", arrow::float64()});
        }
        auto r = make_reader(input, self->delimiter_, autogen, col_names,
                             force_string, strings_nullable, col_types, &self->gate_);
        if (!r.ok()) {
            // A region query whose window overlaps no records leaves the tabix
            // stream empty, and Arrow's CSV reader rejects empty input with
            // "Empty CSV file". The Parquet/BCF/BAM paths return a valid empty
            // result (exit 0) in this situation, so match them: recover the
            // column layout from the full file (region-free) and present zero
            // rows. Reading the whole file region-free is unnecessary — the
            // probe only needs the first block to settle names/types (and, for
            // BED, the column level). Falls through to the original error if
            // the file itself cannot be opened.
            if (!region.empty() &&
                r.status().ToString().find("Empty CSV") != std::string::npos) {
                std::unique_ptr<DelimitedSource> probe;
                if (open(path, kind, /*region=*/"", &probe).empty()) {
                    self->schema_          = probe->schema_;
                    self->bed_level_       = probe->bed_level_;
                    self->bed_variant_     = probe->bed_variant_;
                    self->mpileup_samples_ = probe->mpileup_samples_;
                    self->all_read_        = true;   // zero matching rows
                    *out = std::move(self);
                    return "";
                }
            }
            return "Cannot open '" + path + "': " + r.status().ToString();
        }
        self->reader_ = r.ValueOrDie();
        self->schema_ = self->reader_->schema();

        // Header detection for CSV/TSV without an explicit header (no `#`-line;
        // Arrow took row 0 as the column names, so schema_ now has row 0's
        // values as names and the *body* types (rows 1..N) as the column types).
        //
        // Decide whether row 0 is actually a header or just the first data row
        // by testing row 0 against those body types: a column Arrow inferred as
        // numeric or boolean is "informative"; row 0 *breaks* it when its value
        // there is not a valid token of that type (a word over a number column
        // is a label, i.e. a header). Treat the file as headerless only when at
        // least one informative column exists and row 0 breaks none of them —
        // then re-read auto-naming the columns f0, f1, …. A row-0 word over a
        // numeric column (`gene count value`) keeps the header; a clean data row
        // (`chr1 100 200`) drops it. All-string data gives no signal, so the
        // header is kept (the safe default). --header on/off forces the choice.
        // The all-numeric case (every column numeric, row 0 all numbers) falls
        // out of this as one instance and stays headerless as before.
        if ((kind == DelimKind::CSV || kind == DelimKind::TSV) &&
            col_names.empty() && self->header_mode_ != HeaderMode::On) {
            bool headerless;
            if (self->header_mode_ == HeaderMode::Off) {
                headerless = true;                       // user forced --header off
            } else {                                     // HeaderMode::Auto
                int informative = 0, broken = 0;
                for (int i = 0; i < self->schema_->num_fields(); ++i) {
                    auto id = self->schema_->field(i)->type()->id();
                    const std::string& nm = self->schema_->field(i)->name();
                    bool numeric =
                        id == arrow::Type::INT8   || id == arrow::Type::INT16  ||
                        id == arrow::Type::INT32  || id == arrow::Type::INT64  ||
                        id == arrow::Type::UINT8  || id == arrow::Type::UINT16 ||
                        id == arrow::Type::UINT32 || id == arrow::Type::UINT64 ||
                        id == arrow::Type::HALF_FLOAT || id == arrow::Type::FLOAT ||
                        id == arrow::Type::DOUBLE;
                    if (numeric) {
                        ++informative;
                        if (!looks_like_plain_number(nm)) ++broken;
                    } else if (id == arrow::Type::BOOL) {
                        ++informative;
                        if (!is_bool_token(nm)) ++broken;
                    }
                }
                headerless = (informative > 0) && (broken == 0);
            }
            if (headerless) {
                std::shared_ptr<arrow::io::ReadableFile>  raw2;
                std::shared_ptr<arrow::io::InputStream>   input2;
                if (open_stream(path, is_gz, &raw2, &input2).empty()) {
                    // The first reader keyed force_string to the header-as-data
                    // names; the retry autogenerates f0, f1, …, so recompute the
                    // leading-zero columns under that naming — otherwise a
                    // headerless "007" column loses its zeros (inferred as int).
                    std::vector<std::string> force2 = detect_leading_zero_columns(
                        path, is_gz, self->delimiter_, {}, /*headerless=*/true);
                    std::shared_ptr<ReadGate> gate2;
                    auto r2 = make_reader(input2, self->delimiter_,
                                          /*autogen=*/true, {}, force2,
                                          /*strings_nullable=*/true, {}, &gate2);
                    if (r2.ok()) {
                        close_stream_reader(self->reader_, self->gate_);
                        self->gate_ = std::move(gate2);
                        self->reader_ = r2.ValueOrDie();
                        self->schema_ = self->reader_->schema();
                        if (self->header_mode_ == HeaderMode::Auto)
                            self->auto_headerless_ = true;
                    }
                }
            }
        }

        // R write.csv() writes the index column with an empty header field
        // ("","x","y",…); Arrow reads it as a column literally named "". Give it
        // the same placeholder as the write.table() form (which has no field at
        // all — handled by the stream injection above) so both round-trip to a
        // clear header instead of a blank one. Only the leading column, only
        // when Arrow auto-detected the header (no explicit col_names / #-header).
        if ((kind == DelimKind::CSV || kind == DelimKind::TSV) &&
            col_names.empty() && self->schema_->num_fields() >= 2 &&
            self->schema_->field(0)->name().empty()) {
            auto fields = self->schema_->fields();
            fields[0] = fields[0]->WithName(kRowIndexColName);
            self->schema_ = arrow::schema(fields);
        }

        // Eagerly read first batch so schema + first rows are immediately available.
        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();

        // BED: detect level (BED3..BED12) by checking column types sequentially,
        // then assign standard names. Cols beyond the detected level get "+1", "+2", ...
        // Allele disambiguation: if col 3 is string but values are very short (≤2 chars,
        // typical of allele columns like "A", "CG") AND col 4 isn't numeric, treat col 3
        // as an extra rather than BED4 Name.
        if (kind == DelimKind::BED) {
            int nf = self->schema_->num_fields();
            if (nf >= 3) {
                auto tt = [&](int c) { return self->schema_->field(c)->type()->id(); };
                auto is_str_t = [](arrow::Type::type t) {
                    return t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING;
                };
                auto is_num_t = [](arrow::Type::type t) {
                    return t == arrow::Type::INT8   || t == arrow::Type::INT16  ||
                           t == arrow::Type::INT32  || t == arrow::Type::INT64  ||
                           t == arrow::Type::UINT8  || t == arrow::Type::UINT16 ||
                           t == arrow::Type::UINT32 || t == arrow::Type::UINT64 ||
                           t == arrow::Type::FLOAT  || t == arrow::Type::DOUBLE ||
                           t == arrow::Type::HALF_FLOAT;
                };
                auto is_int_t = [](arrow::Type::type t) {
                    return t == arrow::Type::INT8   || t == arrow::Type::INT16  ||
                           t == arrow::Type::INT32  || t == arrow::Type::INT64  ||
                           t == arrow::Type::UINT8  || t == arrow::Type::UINT16 ||
                           t == arrow::Type::UINT32 || t == arrow::Type::UINT64;
                };
                int lvl = 3;
                do {
                    if (nf < 4  || !is_str_t(tt(3)))  break; lvl = 4;   // Name: string
                    if (nf < 5  || !is_num_t(tt(4)))  break; lvl = 5;   // Score: numeric
                    if (nf < 6  || !is_str_t(tt(5)))  break; lvl = 6;   // Strand: string
                    if (nf < 7  || !is_int_t(tt(6)))  break; lvl = 7;   // ThickStart: int
                    if (nf < 8  || !is_int_t(tt(7)))  break; lvl = 8;   // ThickEnd: int
                    if (nf < 9  || !is_str_t(tt(8)))  break; lvl = 9;   // itemRgb: string
                    if (nf < 10 || !is_int_t(tt(9)))  break; lvl = 10;  // blockCount: int
                    if (nf < 11 || !is_str_t(tt(10))) break; lvl = 11;  // blockSizes: string
                    if (nf < 12 || !is_str_t(tt(11))) break; lvl = 12;  // blockStarts: string
                } while (false);

                // Allele disambiguation: only trigger if detection stopped at BED4
                // (col 3 string, col 4 not numeric) — in BED5+ layouts the presence
                // of Score/Strand/etc. in the right slots is strong signal.
                if (lvl == 4 && !self->batches_.empty() && self->batches_[0]) {
                    auto col3 = self->batches_[0]->column(3);
                    int64_t n_rows = std::min<int64_t>(self->batches_[0]->num_rows(), 100);
                    int n_nonnull = 0;
                    bool all_short = true;
                    auto check = [&](auto* arr) {
                        for (int64_t i = 0; i < n_rows; ++i) {
                            if (arr->IsNull(i)) continue;
                            ++n_nonnull;
                            if (arr->GetView(i).size() > 2) { all_short = false; return; }
                        }
                    };
                    if (auto sa = std::dynamic_pointer_cast<arrow::StringArray>(col3))
                        check(sa.get());
                    else if (auto la = std::dynamic_pointer_cast<arrow::LargeStringArray>(col3))
                        check(la.get());
                    if (all_short && n_nonnull > 0) lvl = 3;
                }
                self->bed_level_ = lvl;

                static const char* kBedNames[] = {
                    "Chr", "[Beg", "End)", "Name", "Score", "Str",
                    "ThBeg", "ThEnd", "RGB", "NBlk", "BlkSz", "BlkSt"
                };
                arrow::FieldVector fields;
                fields.reserve(nf);
                for (int i = 0; i < nf; ++i) {
                    auto f = self->schema_->field(i);
                    std::string nm = (i < lvl)
                        ? kBedNames[i]
                        : ("+" + std::to_string(i - lvl + 1));
                    fields.push_back(arrow::field(nm, f->type(), f->nullable()));
                }
                self->schema_ = arrow::schema(fields);
            }
        }

        // samtools mpileup: per-sample triplet (depth, bases, qualities)
        // after the fixed chrom/pos/ref prefix. Single-sample files get
        // unsuffixed names; multi-sample files get `_1`, `_2`, … so the
        // user can still --select / --filter by column.
        if (kind == DelimKind::Mpileup) {
            int nf = self->schema_->num_fields();
            if (nf >= 6 && (nf - 3) % 3 == 0) {
                int samples = (nf - 3) / 3;
                self->mpileup_samples_ = samples;
                arrow::FieldVector fields;
                fields.reserve(nf);
                auto add = [&](const std::string& nm, int i) {
                    auto t = self->schema_->field(i)->type();
                    bool n = self->schema_->field(i)->nullable();
                    fields.push_back(arrow::field(nm, t, n));
                };
                add("chrom", 0);
                add("pos",   1);
                add("ref",   2);
                for (int s = 0; s < samples; ++s) {
                    std::string sfx = (samples == 1) ? std::string{}
                                        : "_" + std::to_string(s + 1);
                    add("depth"  + sfx, 3 + s * 3);
                    add("bases"  + sfx, 4 + s * 3);
                    add("quals"  + sfx, 5 + s * 3);
                }
                self->schema_ = arrow::schema(fields);
            }
        }

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    arrow::Status read_status() const override { return read_status_; }
    bool region_applied() const override { return region_applied_; }
    ChunkMeta chunk_meta(int i) const override {
        // num_rows is read from retained metadata, not the batch, so it stays
        // valid after the batch's data is evicted from the trailing window.
        return {batch_first_row_[i], batch_num_rows_[i]};
    }

    // Pin retention: stop evicting (used by full-pass ops — search / sort /
    // filter / stats — that must read every row). Already-evicted batches can't
    // be recovered (forward-only stream); evicted_any() reports that case.
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

    const std::string& path() const override { return path_; }

    std::string footer() const override {
        switch (kind_) {
            case DelimKind::VCF: return "Format: VCF";
            case DelimKind::GFF: return "Format: GFF3/GTF";
            case DelimKind::SAM: return "Format: SAM";
            case DelimKind::PAF: return "Format: PAF";
            case DelimKind::Mtx:
                return "Format: MatrixMarket coordinate " + mtx_field_ +
                       "  |  shape: " + std::to_string(mtx_rows_) + " \xc3\x97 " +
                       std::to_string(mtx_cols_) + "  |  entries: " +
                       std::to_string(mtx_nnz_) + "  |  row / col are 0-based";
            case DelimKind::Mpileup: {
                std::string s = "Format: mpileup";
                if (mpileup_samples_ > 0) {
                    s += "  |  Samples: " + std::to_string(mpileup_samples_);
                }
                return s;
            }
            case DelimKind::BED: {
                int nf = schema_->num_fields();
                if (bed_variant_ != BedVariant::None) {
                    switch (bed_variant_) {
                        case BedVariant::NarrowPeak: return "Format: narrowPeak (BED6+4)";
                        case BedVariant::BroadPeak:  return "Format: broadPeak (BED6+3)";
                        case BedVariant::GappedPeak: return "Format: gappedPeak (BED12+3)";
                        case BedVariant::BedGraph:
                            return format_note_.empty() ? "Format: bedGraph" : "Format: " + format_note_;
                        case BedVariant::TagAlign:   return "Format: tagAlign (BED6)";
                        case BedVariant::None: break;
                    }
                }
                std::string s = "Format: BED" + std::to_string(bed_level_);
                if (nf > bed_level_) {
                    int extra = nf - bed_level_;
                    s += " + " + std::to_string(extra) + " extra col" + (extra == 1 ? "" : "s");
                }
                return s;
            }
            default:
                std::string d(1, delimiter_);
                if (delimiter_ == '\t') d = "tab";
                std::string s = "Format: delimited (separator: " + d + ")";
                if (!format_note_.empty()) s += "  |  " + format_note_;
                if (auto_headerless_)
                    s += "  |  no header row detected — columns auto-named "
                         "(--header on to override)";
                return s;
        }
    }

    // BED track/browser lines are shown above the table; all other format
    // preambles (VCF ##INFO/##FILTER, SAM @SQ/@PG, GFF ##) go below the schema.
    std::vector<std::string> preamble_above() const override {
        return (kind_ == DelimKind::BED) ? preamble_lines_ : std::vector<std::string>{};
    }
    std::vector<std::string> preamble_below() const override {
        if (kind_ == DelimKind::BED) return {};
        return preamble_lines_;
    }

    // Per-format: display coordinate columns with '_' digit grouping.
    std::string format_cell(int col_idx, std::string val) const override {
        switch (kind_) {
            case DelimKind::BED:
                if (col_idx == 1 || col_idx == 2) return digits_with_sep(val);
                break;
            case DelimKind::VCF:
                if (col_idx == 1) return digits_with_sep(val);          // POS
                break;
            case DelimKind::GFF:
                if (col_idx == 3 || col_idx == 4) return digits_with_sep(val); // start, end
                break;
            case DelimKind::SAM:
                if (col_idx == 3 || col_idx == 7) return digits_with_sep(val); // POS, PNEXT
                break;
            default: break;
        }
        return val;
    }

    int min_col_width(int col_idx) const override {
        switch (kind_) {
            case DelimKind::BED:
                if (col_idx >= bed_level_) return 4;
                switch (col_idx) {
                    case 0:         return 6;   // Chr: "chrXII"
                    case 1: case 2: return 11;  // [Beg / End): "999_999_999"
                    case 3:         return 6;   // Name
                    case 5:         return 3;   // Str (strand: +/-/.)
                    case 8:         return 5;   // RGB bar
                    default:        return 4;
                }
            case DelimKind::VCF:
                switch (col_idx) {
                    case 0:  return 6;   // CHROM: "chrXII"
                    case 1:  return 9;   // POS
                    case 7:  return 12;  // INFO
                    default: return 4;
                }
            case DelimKind::GFF:
                switch (col_idx) {
                    case 0:         return 6;   // seqname: "chrXII"
                    case 3: case 4: return 9;   // start / end
                    case 6:         return 3;   // strand (+/-/.)
                    case 8:         return 15;  // attributes
                    default:        return 4;
                }
            case DelimKind::SAM:
                switch (col_idx) {
                    case 0:  return 10;  // QNAME
                    case 2:  return 6;   // RNAME: "chrXII"
                    case 3:  return 9;   // POS
                    case 5:  return 8;   // CIGAR
                    case 7:  return 9;   // PNEXT
                    case 9:  return 10;  // SEQ
                    case 10: return 10;  // QUAL
                    default: return 4;
                }
            default: return 4;
        }
    }
};

std::string open_delimited_source(const std::string& path, DelimKind kind, const std::string& region,
                                  std::unique_ptr<TabularSource>* out, char delim_override,
                                  HeaderMode header_mode,
                                  TsvDialect dialect,
                                  const std::vector<std::string>& paf_tags) {
    std::unique_ptr<DelimitedSource> s;
    std::string e = DelimitedSource::open(path, kind, region, &s, delim_override, header_mode,
                                            dialect, paf_tags);
    if (e.empty()) *out = std::move(s);
    return e;
}
std::string open_delimited_stream(std::shared_ptr<arrow::io::InputStream> input,
                                  const std::string& path_label, DelimKind kind, bool is_gz,
                                  const std::string& region, std::unique_ptr<TabularSource>* out,
                                  char delim_override, HeaderMode header_mode) {
    std::unique_ptr<DelimitedSource> s;
    std::string e = DelimitedSource::open_from_stream(std::move(input), path_label, kind, is_gz,
                                                        region, &s, delim_override, header_mode);
    if (e.empty()) *out = std::move(s);
    return e;
}
// Post-open adjustments of a DelimitedSource made by the dispatcher; no-ops
// on any other source.
void delimited_apply_column_names(TabularSource& src, const std::vector<std::string>& names,
                                  std::string note) {
    if (auto* d = dynamic_cast<DelimitedSource*>(&src)) d->apply_column_names(names, std::move(note));
}
// A MatrixMarket source's declared shape; false for anything else.
bool delimited_mtx_shape(TabularSource* src, int64_t* rows, int64_t* cols) {
    auto* d = dynamic_cast<DelimitedSource*>(src);
    return d && d->mtx_shape(rows, cols);
}
void delimited_apply_tenx_sidecar(TabularSource& src, int kind) {
    if (auto* d = dynamic_cast<DelimitedSource*>(&src)) d->apply_tenx_sidecar(kind);
}
void delimited_apply_bed_variant(TabularSource& src, BedVariant v) {
    if (auto* d = dynamic_cast<DelimitedSource*>(&src)) d->apply_bed_variant(v);
}
std::string delimited_first_line_after_meta(const std::string& path) {
    return DelimitedSource::first_line_after_meta(path);
}
std::string delimited_first_line_after_meta_raw(const std::string& path) {
    return DelimitedSource::first_line_after_meta_raw(path);
}
