// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// htslib readers: BAM / CRAM / SAM, pileup, BCF, FASTA / FASTQ, --contigs.

#include "internal.hpp"

// ── BAM aux-tag support (--tags) ──────────────────────────────────────────────
//
// SAM/BAM optional fields (`NM:i:2`, `AS:i:100`, `RG:Z:grp`, `MD:Z:…`) are
// per-record and are not part of the mandatory 11-column schema. `--tags LIST`
// adds one column per named tag. The Arrow column type is fixed at open() from
// the tag's SAM type code, so a numeric tag stays numeric and `--filter 'NM<=2'`
// compares numbers rather than strings.

// Float: SAM type 'f' (single precision) — a float32 column, so the stored
// value (0.9f) prints as 0.9, not as its widened double 0.8999999761581421.
// Double: htslib's 'd' extension.
enum class BamTagKind { Int, Float, Double, Str };

// SAM type code (the first byte a bam_aux_get() pointer addresses) → column kind.
static BamTagKind bam_tag_kind_from_code(char c) {
    switch (c) {
        case 'c': case 'C': case 's': case 'S': case 'i': case 'I':
            return BamTagKind::Int;
        case 'f':
            return BamTagKind::Float;
        case 'd':
            return BamTagKind::Double;
        default:   // 'A' (char), 'Z' (string), 'H' (hex), 'B' (array)
            return BamTagKind::Str;
    }
}
static bool bam_tag_is_int_code(char c) {
    return c=='c'||c=='C'||c=='s'||c=='S'||c=='i'||c=='I';
}

// Render one aux value as text — for a string tag column, and for any tag whose
// type doesn't match the column kind resolved at open().
static std::string bam_aux_to_string(const uint8_t* s) {
    char t = (char)*s;
    switch (t) {
        case 'A': return std::string(1, (char)*(s + 1));
        case 'c': case 'C': case 's': case 'S': case 'i': case 'I':
            return std::to_string(bam_aux2i(s));
        case 'f': case 'd': {
            char b[32];
            std::snprintf(b, sizeof(b), "%g", bam_aux2f(s));
            return b;
        }
        case 'Z': case 'H': {
            const char* z = bam_aux2Z(s);
            return z ? std::string(z) : std::string();
        }
        case 'B': {
            char sub = (char)*(s + 1);
            uint32_t n = bam_auxB_len(s);
            std::string out(1, sub);
            out.push_back(':');
            for (uint32_t i = 0; i < n; ++i) {
                if (i) out.push_back(',');
                if (sub == 'f') {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%g", bam_auxB2f(s, i));
                    out += b;
                } else {
                    out += std::to_string(bam_auxB2i(s, i));
                }
            }
            return out;
        }
        default: return std::string();
    }
}

// Split the --tags value into distinct two-character SAM tags, preserving order.
// Returns an error string in *err on a malformed tag.
std::vector<std::string> parse_bam_tag_list(const std::string& spec,
                                            std::string* err) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        std::string tok = spec.substr(i, j == std::string::npos ? j : j - i);
        i = (j == std::string::npos) ? spec.size() : j + 1;
        // trim spaces
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a == std::string::npos) continue;   // empty token (e.g. trailing comma)
        tok = tok.substr(a, b - a + 1);
        if (tok.size() != 2) {
            *err = "invalid SAM tag '" + tok + "' (tags are exactly two characters)";
            return {};
        }
        if (seen.insert(tok).second) out.push_back(tok);
    }
    if (out.empty()) *err = "--tags needs at least one two-character tag";
    return out;
}

// ── BAM source ────────────────────────────────────────────────────────────────

class BamSource : public TabularSource {
    std::string                           path_;
    std::shared_ptr<arrow::Schema>        schema_;
    std::vector<std::string>              preamble_lines_;
    int                                   num_refs_   = 0;
    std::string                           fmt_name_;   // "BAM", "CRAM", or "SAM"
    std::vector<std::string>              tag_names_;  // --tags: aux tag columns
    std::vector<BamTagKind>               tag_kinds_;  // resolved column type per tag

    mutable htsFile*   hts_ = nullptr;
    mutable sam_hdr_t* hdr_ = nullptr;
    mutable bam1_t*    rec_ = nullptr;   // reused across advance() calls
    // -r: index + multi-region iterator. When iter_ is set, advance() walks
    // only the records overlapping the requested windows.
    mutable hts_idx_t* idx_  = nullptr;
    mutable hts_itr_t* iter_ = nullptr;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>          batch_first_row_;
    mutable std::vector<int64_t>          batch_num_rows_;   // kept after eviction
    mutable int64_t                       rows_so_far_ = 0;
    mutable bool                          all_read_    = false;
    mutable bool                          retain_all_  = false;
    mutable bool                          evicted_any_ = false;
    mutable arrow::Status                 read_status_;      // sticky stream error

    static constexpr int BATCH_SIZE = 32768;

    static constexpr const char NT16[] = "=ACMGRSVTWYHKDBN";

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();

        arrow::StringBuilder qname_b, rname_b, cigar_b, rnext_b, seq_b, qual_b;
        arrow::Int32Builder  flag_b, mapq_b;
        arrow::Int64Builder  pos_b, pnext_b, tlen_b;

        // One builder per --tags column, of the type resolved at open().
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> tag_b;
        for (BamTagKind kind : tag_kinds_) {
            std::shared_ptr<arrow::DataType> dt =
                kind == BamTagKind::Int    ? arrow::int64()
              : kind == BamTagKind::Float  ? arrow::float32()
              : kind == BamTagKind::Double ? arrow::float64()
                                           : arrow::utf8();
            std::unique_ptr<arrow::ArrayBuilder> bld;
            ARROW_RETURN_NOT_OK(
                arrow::MakeBuilder(arrow::default_memory_pool(), dt, &bld));
            tag_b.push_back(std::move(bld));
        }

        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        int count = 0, ret = 0;
        // In region mode the multi-region iterator yields only the records
        // overlapping the requested windows; otherwise walk the whole file.
        auto next_record = [&]() {
            return iter_ ? sam_itr_multi_next(hts_, iter_, rec_)
                         : sam_read1(hts_, hdr_, rec_);
        };
        while (count < cap && (ret = next_record()) >= 0) {
            ARROW_RETURN_NOT_OK(qname_b.Append(bam_get_qname(rec_)));
            ARROW_RETURN_NOT_OK(flag_b.Append((int32_t)rec_->core.flag));

            // RNAME / POS
            if (rec_->core.tid < 0) {
                ARROW_RETURN_NOT_OK(rname_b.Append("*"));
                ARROW_RETURN_NOT_OK(pos_b.Append((int64_t)0));
            } else {
                ARROW_RETURN_NOT_OK(rname_b.Append(
                    sam_hdr_tid2name(hdr_, rec_->core.tid)));
                ARROW_RETURN_NOT_OK(pos_b.Append((int64_t)rec_->core.pos + 1));
            }

            ARROW_RETURN_NOT_OK(mapq_b.Append((int32_t)rec_->core.qual));

            // CIGAR
            {
                uint32_t nc = rec_->core.n_cigar;
                if (nc == 0) {
                    ARROW_RETURN_NOT_OK(cigar_b.Append("*"));
                } else {
                    std::string cig;
                    cig.reserve(nc * 5);
                    const uint32_t* cr = bam_get_cigar(rec_);
                    for (uint32_t i = 0; i < nc; ++i) {
                        char buf[16];
                        int n = std::snprintf(buf, sizeof(buf), "%u%c",
                                              bam_cigar_oplen(cr[i]),
                                              bam_cigar_opchr(cr[i]));
                        cig.append(buf, n);
                    }
                    ARROW_RETURN_NOT_OK(cigar_b.Append(cig));
                }
            }

            // RNEXT / PNEXT
            if (rec_->core.mtid < 0) {
                ARROW_RETURN_NOT_OK(rnext_b.Append("*"));
                ARROW_RETURN_NOT_OK(pnext_b.Append((int64_t)0));
            } else if (rec_->core.mtid == rec_->core.tid) {
                ARROW_RETURN_NOT_OK(rnext_b.Append("="));
                ARROW_RETURN_NOT_OK(pnext_b.Append((int64_t)rec_->core.mpos + 1));
            } else {
                ARROW_RETURN_NOT_OK(rnext_b.Append(
                    sam_hdr_tid2name(hdr_, rec_->core.mtid)));
                ARROW_RETURN_NOT_OK(pnext_b.Append((int64_t)rec_->core.mpos + 1));
            }

            ARROW_RETURN_NOT_OK(tlen_b.Append((int64_t)rec_->core.isize));

            // SEQ
            {
                int lq = rec_->core.l_qseq;
                if (lq == 0) {
                    ARROW_RETURN_NOT_OK(seq_b.Append("*"));
                } else {
                    std::string seq((size_t)lq, ' ');
                    const uint8_t* s = bam_get_seq(rec_);
                    for (int i = 0; i < lq; ++i)
                        seq[i] = NT16[bam_seqi(s, i)];
                    ARROW_RETURN_NOT_OK(seq_b.Append(seq));
                }
            }

            // QUAL (Phred+33; '*' if not stored)
            {
                int lq = rec_->core.l_qseq;
                const uint8_t* q = bam_get_qual(rec_);
                if (lq == 0 || q[0] == 0xff) {
                    ARROW_RETURN_NOT_OK(qual_b.Append("*"));
                } else {
                    std::string qual((size_t)lq, ' ');
                    for (int i = 0; i < lq; ++i)
                        qual[i] = (char)(q[i] + 33);
                    ARROW_RETURN_NOT_OK(qual_b.Append(qual));
                }
            }

            // Aux tags (--tags): one typed column each, null where the read has
            // no such tag (or its stored type doesn't fit the resolved column).
            for (size_t k = 0; k < tag_names_.size(); ++k) {
                const uint8_t* aux = bam_aux_get(rec_, tag_names_[k].c_str());
                arrow::ArrayBuilder* bld = tag_b[k].get();
                if (!aux) { ARROW_RETURN_NOT_OK(bld->AppendNull()); continue; }
                char t = (char)*aux;
                switch (tag_kinds_[k]) {
                    case BamTagKind::Int:
                        if (bam_tag_is_int_code(t))
                            ARROW_RETURN_NOT_OK(static_cast<arrow::Int64Builder*>(bld)
                                                    ->Append(bam_aux2i(aux)));
                        else ARROW_RETURN_NOT_OK(bld->AppendNull());
                        break;
                    case BamTagKind::Float:
                        if (t == 'f' || t == 'd')
                            ARROW_RETURN_NOT_OK(static_cast<arrow::FloatBuilder*>(bld)
                                                    ->Append((float)bam_aux2f(aux)));
                        else if (bam_tag_is_int_code(t))
                            ARROW_RETURN_NOT_OK(static_cast<arrow::FloatBuilder*>(bld)
                                                    ->Append((float)bam_aux2i(aux)));
                        else ARROW_RETURN_NOT_OK(bld->AppendNull());
                        break;
                    case BamTagKind::Double:
                        if (t == 'f' || t == 'd')
                            ARROW_RETURN_NOT_OK(static_cast<arrow::DoubleBuilder*>(bld)
                                                    ->Append(bam_aux2f(aux)));
                        else if (bam_tag_is_int_code(t))
                            ARROW_RETURN_NOT_OK(static_cast<arrow::DoubleBuilder*>(bld)
                                                    ->Append((double)bam_aux2i(aux)));
                        else ARROW_RETURN_NOT_OK(bld->AppendNull());
                        break;
                    case BamTagKind::Str:
                        ARROW_RETURN_NOT_OK(static_cast<arrow::StringBuilder*>(bld)
                                                ->Append(bam_aux_to_string(aux)));
                        break;
                }
            }

            ++count;
        }

        // A read error (ret < -1) is distinct from EOF (-1). ensure() discards
        // advance()'s return, so record it stickily too or a truncated /
        // corrupt file yields a partial result with exit 0.
        if (ret < -1) {
            all_read_ = true;
            if (read_status_.ok())
                read_status_ = arrow::Status::IOError(
                    "Error reading ", fmt_name_, " record from ", path_);
            return read_status_;
        }
        if (count == 0) { all_read_ = true; return arrow::Status::OK(); }
        if (ret < 0) all_read_ = true;   // EOF hit during this batch

        std::vector<std::shared_ptr<arrow::Array>> arrs(11 + tag_b.size());
        ARROW_RETURN_NOT_OK(qname_b.Finish(&arrs[0]));
        ARROW_RETURN_NOT_OK(flag_b.Finish(&arrs[1]));
        ARROW_RETURN_NOT_OK(rname_b.Finish(&arrs[2]));
        ARROW_RETURN_NOT_OK(pos_b.Finish(&arrs[3]));
        ARROW_RETURN_NOT_OK(mapq_b.Finish(&arrs[4]));
        ARROW_RETURN_NOT_OK(cigar_b.Finish(&arrs[5]));
        ARROW_RETURN_NOT_OK(rnext_b.Finish(&arrs[6]));
        ARROW_RETURN_NOT_OK(pnext_b.Finish(&arrs[7]));
        ARROW_RETURN_NOT_OK(tlen_b.Finish(&arrs[8]));
        ARROW_RETURN_NOT_OK(seq_b.Finish(&arrs[9]));
        ARROW_RETURN_NOT_OK(qual_b.Finish(&arrs[10]));
        for (size_t k = 0; k < tag_b.size(); ++k)
            ARROW_RETURN_NOT_OK(tag_b[k]->Finish(&arrs[11 + k]));

        auto batch = arrow::RecordBatch::Make(schema_, count, arrs);
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

    // Resolve each requested tag's column type from the SAM type code it carries
    // in the first records that have it. Reopens the file for a bounded scan
    // (up to kScan records, or until every tag's type is known) so the schema is
    // fixed before the main read starts. A tag never seen defaults to string.
    static std::vector<BamTagKind> scan_tag_types(
            const std::string& path, const Config& cfg,
            const std::vector<std::string>& tags) {
        std::vector<BamTagKind> kinds(tags.size(), BamTagKind::Str);
        std::vector<bool>       found(tags.size(), false);
        int remaining = (int)tags.size();
        if (remaining == 0) return kinds;

        htsFile* fp = hts_open(path.c_str(), "r");
        if (!fp) return kinds;
        if (!cfg.pileup_ref.empty())
            (void)hts_set_fai_filename(fp, cfg.pileup_ref.c_str());
        sam_hdr_t* h = sam_hdr_read(fp);
        if (!h) { hts_close(fp); return kinds; }
        bam1_t* b = bam_init1();

        const int kScan = 4096;
        for (int r = 0; r < kScan && remaining > 0; ++r) {
            if (sam_read1(fp, h, b) < 0) break;
            for (size_t k = 0; k < tags.size(); ++k) {
                if (found[k]) continue;
                const uint8_t* aux = bam_aux_get(b, tags[k].c_str());
                if (!aux) continue;
                kinds[k] = bam_tag_kind_from_code((char)*aux);
                found[k] = true;
                --remaining;
            }
        }
        bam_destroy1(b);
        sam_hdr_destroy(h);
        hts_close(fp);
        return kinds;
    }

public:
    ~BamSource() {
        // sam_itr_regarray() builds a MULTI iterator — it must be freed with
        // hts_itr_multi_destroy, not hts_itr_destroy.
        if (iter_) { hts_itr_multi_destroy(iter_); iter_ = nullptr; }
        if (idx_)  { hts_idx_destroy(idx_);  idx_  = nullptr; }
        if (rec_) { bam_destroy1(rec_); rec_ = nullptr; }
        if (hdr_) { sam_hdr_destroy(hdr_); hdr_ = nullptr; }
        if (hts_) { hts_close(hts_); hts_ = nullptr; }
    }

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BamSource>* out) {
        auto self = std::make_unique<BamSource>();
        self->path_ = path;

        self->hts_ = hts_open(path.c_str(), "r");
        if (!self->hts_)
            return "Cannot open '" + path + "'";

        // Multi-threaded BGZF / CRAM slice decompression.
        int n = effective_threads(cfg);
        if (n > 1) hts_set_threads(self->hts_, n);

        // CRAM stores bases as differences from a reference, so decoding needs
        // one. htslib otherwise falls back to $REF_PATH / $REF_CACHE (often a
        // network fetch, or nothing at all). -f/--fasta points it at a local
        // FASTA. Must be set before the header read triggers any decode.
        if (!cfg.pileup_ref.empty()) {
            if (hts_set_fai_filename(self->hts_, cfg.pileup_ref.c_str()) < 0)
                return "Cannot use reference '" + cfg.pileup_ref +
                       "' for '" + path + "' (need a .fai index; run "
                       "`samtools faidx`)";
        }

        self->hdr_ = sam_hdr_read(self->hts_);
        if (!self->hdr_)
            return "Cannot read BAM/SAM header from '" + path + "'";

        self->rec_ = bam_init1();
        if (!self->rec_)
            return "Out of memory allocating BAM record";

        self->num_refs_ = sam_hdr_nref(self->hdr_);

        // Detect exact format (BAM / CRAM / SAM) for the footer
        {
            const htsFormat* fmt = hts_get_format(self->hts_);
            switch (fmt ? fmt->format : unknown_format) {
                case cram: self->fmt_name_ = "CRAM"; break;
                case sam:  self->fmt_name_ = "SAM";  break;
                default:   self->fmt_name_ = "BAM";  break;
            }
        }

        // Collect preamble lines from the embedded SAM header text (cap at 20)
        {
            int total = 0;
            std::istringstream ss(std::string(self->hdr_->text,
                                              (size_t)self->hdr_->l_text));
            std::string line;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                ++total;
                if (total <= 20) self->preamble_lines_.push_back(line);
            }
            if (total > 20)
                self->preamble_lines_.push_back(
                    "... (" + std::to_string(total - 20) + " more header lines)");
        }

        arrow::FieldVector fields = {
            arrow::field("QNAME", arrow::utf8()),
            arrow::field("FLAG",  arrow::int32()),
            arrow::field("RNAME", arrow::utf8()),
            arrow::field("POS",   arrow::int64()),
            arrow::field("MAPQ",  arrow::int32()),
            arrow::field("CIGAR", arrow::utf8()),
            arrow::field("RNEXT", arrow::utf8()),
            arrow::field("PNEXT", arrow::int64()),
            arrow::field("TLEN",  arrow::int64()),
            arrow::field("SEQ",   arrow::utf8()),
            arrow::field("QUAL",  arrow::utf8()),
        };

        // --tags: append one column per requested aux tag. Its Arrow type is
        // fixed here from the tag's SAM type code (scanned from the first records
        // that carry it), so a numeric tag stays numeric for --filter and stats.
        if (!cfg.bam_tags.empty()) {
            std::string terr;
            self->tag_names_ = parse_bam_tag_list(cfg.bam_tags, &terr);
            if (!terr.empty()) return "'" + path + "': " + terr;
            self->tag_kinds_ = scan_tag_types(path, cfg, self->tag_names_);
            for (size_t k = 0; k < self->tag_names_.size(); ++k) {
                std::shared_ptr<arrow::DataType> dt =
                    self->tag_kinds_[k] == BamTagKind::Int    ? arrow::int64()
                  : self->tag_kinds_[k] == BamTagKind::Float  ? arrow::float32()
                  : self->tag_kinds_[k] == BamTagKind::Double ? arrow::float64()
                                                              : arrow::utf8();
                fields.push_back(arrow::field(self->tag_names_[k], dt));
            }
        }
        self->schema_ = arrow::schema(fields);

        // -r: restrict the scan to the requested windows. Needs a
        // coordinate-sorted file with an index (.bai / .csi / .crai). Without
        // this the flag was silently ignored and vv answered with the whole
        // file — including for a contig the file doesn't even have.
        if (!cfg.region.empty()) {
            if (self->fmt_name_ == "SAM")
                return "'" + path + "': -r needs an indexed BAM/CRAM; plain "
                       "SAM has no index (convert with `samtools view -b`)";
            self->idx_ = sam_index_load(self->hts_, path.c_str());
            if (!self->idx_)
                return "'" + path + "': -r needs an index (.bai/.csi/.crai); "
                       "build one with `samtools index`";
            // cfg.region is canonical 0-based half-open; sam_itr_regarray reads
            // region strings as 1-based inclusive, so convert at the boundary.
            std::vector<Region> windows = parse_region_list(cfg.region);
            resolve_region_chroms(windows,
                [&](const std::string& nm) {
                    return bam_name2id(self->hdr_, nm.c_str()) >= 0;
                }, path);
            std::vector<std::string> regs;
            regs.reserve(windows.size());
            for (const auto& w : windows) regs.push_back(region_to_htslib(w));
            std::vector<const char*> regp;
            regp.reserve(regs.size());
            for (auto& r : regs) regp.push_back(r.c_str());
            self->iter_ = sam_itr_regarray(self->idx_, self->hdr_,
                                            const_cast<char**>(regp.data()),
                                            (unsigned)regp.size());
            if (!self->iter_)
                return "'" + path + "': cannot build iterator for region '" +
                       cfg.region + "'";
        }

        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();

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
    bool region_applied() const override { return iter_ != nullptr; }
    arrow::Status read_status() const override { return read_status_; }
    void ensure(int i) override {
        // advance()'s error used to be discarded here, so a truncated or
        // corrupt file produced a partial result with exit 0. Keep it.
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
    // Cap the first-batch decode to `rows` so a small `-n` preview doesn't
    // decode 32 768 BAM records.
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        if (batches_.empty() && !all_read_ && rows > 0)
            ARROW_RETURN_NOT_OK(advance(rows));
        return TabularSource::read_first(rows, col_indices, out);
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: " + fmt_name_ + "  |  References: " + std::to_string(num_refs_);
    }
    std::vector<std::string> preamble_below() const override { return preamble_lines_; }
    std::string format_cell(int col_idx, std::string val) const override {
        if (col_idx == 3 || col_idx == 7) return digits_with_sep(val);  // POS, PNEXT
        return val;
    }
    int min_col_width(int col_idx) const override {
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
    }
};

std::string open_bam_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<BamSource> s;
    std::string e = BamSource::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── BAM/CRAM pileup source (`vv x.bam --pileup`) ─────────────────────────────
//
// Walks a sorted BAM/CRAM through htslib's bam_plp_auto engine, emitting one
// row per covered position in a schema identical to DelimKind::Mpileup
// (`chrom, pos, ref, depth, bases, quals`). The output is byte-for-byte
// compatible with `samtools mpileup` invoked without a reference FASTA: ref
// is set to 'N' on every row, and bases are rendered as literal letters
// (uppercase forward, lowercase reverse) — there's no `.` / `,` match
// notation because we have no reference to match against.
//
// Region queries (`-r chrom:start-end`) pipe through htslib's sam_itr_querys
// so we only walk the requested span — essential for whole-genome BAMs where
// `vv x.bam --pileup` over everything would emit ~3 billion rows. Composes
// with `--decode-pileup`, which materialises the typed allele-count view on
// top of this source.

class BamPileupSource : public TabularSource {
    std::string                              path_;
    std::shared_ptr<arrow::Schema>           schema_;
    std::string                              fmt_name_;     // BAM / CRAM / SAM

    // htslib handles. The plp iterator owns the read-callback closure.
    samFile*                                 fp_   = nullptr;
    sam_hdr_t*                               hdr_  = nullptr;
    hts_idx_t*                               idx_  = nullptr;   // optional
    hts_itr_multi_t*                         iter_ = nullptr;   // optional
    // htslib's multi-file pileup engine over this one file: only the mplp API
    // has read-pair overlap detection (bam_mplp_init_overlaps).
    bam_mplp_t                               plp_  = nullptr;
    bam1_t*                                  rec_  = nullptr;   // scratch for callback

    // Reference FASTA for -f/--pileup (ref column + ./, match notation). The
    // current contig's sequence is fetched once and cached (freed on tid change).
    faidx_t*                                 fai_  = nullptr;
    mutable char*                            ref_cache_ = nullptr;
    mutable int                              ref_cache_tid_ = -1;
    mutable hts_pos_t                        ref_cache_len_ = 0;

    // Reference bases for the current pileup column (nullptr = no -f); `ref_for`
    // fetches + caches the contig for `tid`, returning the whole-contig sequence.
    const char* ref_for(int tid) const {
        if (!fai_) return nullptr;
        if (tid == ref_cache_tid_) return ref_cache_;
        if (ref_cache_) { free(ref_cache_); ref_cache_ = nullptr; }
        ref_cache_tid_ = tid;
        ref_cache_len_ = 0;
        const char* name = sam_hdr_tid2name(hdr_, tid);
        if (!name) return nullptr;
        hts_pos_t clen = sam_hdr_tid2len(hdr_, tid);
        if (clen <= 0) return nullptr;
        ref_cache_ = faidx_fetch_seq64(fai_, name, 0, clen - 1, &ref_cache_len_);
        return ref_cache_;   // nullptr if the contig is absent from the FASTA
    }

    // Requested regions parsed at open time. bam_plp_auto emits every
    // position covered by the iterator's fetched reads, so a query like
    // `-r chr1:105-105` would otherwise spill the full span of any read
    // touching pos 105. Match `samtools mpileup`'s behaviour by filtering
    // emitted positions to the requested ranges.
    struct RegionRange { int tid; hts_pos_t beg; hts_pos_t end; };
    std::vector<RegionRange>                 regions_;

    // Streaming-source plumbing (same shape as BamSource / BcfSource).
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             all_read_    = false;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;
    mutable int64_t                          num_rows_total_ = -1;
    mutable arrow::Status                    read_status_;   // sticky stream error

    static constexpr int BATCH_ROWS = 16384;

    // Per-pileup-iterator state. We give one of these to bam_mplp_init so
    // the read-callback knows where to pull alignments from. last_rc records
    // the most recent underlying read return: bam_mplp_auto() returns 0 on
    // both EOF and error, so this is how advance() tells them apart.
    struct PlpData {
        samFile*           fp;
        sam_hdr_t*         hdr;
        hts_itr_multi_t*   iter;     // nullptr → full-file scan
        int                last_rc = 0;  // >=0 record, -1 EOF, <-1 read error
        // Read filters (samtools mpileup's: --ff / --rf / -q / -A).
        int                excl_flags = 0, incl_flags = 0, min_mapq = 0;
        bool               count_orphans = false;
    };
    PlpData plp_data_;
    int     min_bq_ = 0;   // bases below this quality are dropped (-Q)

    // Feed the pileup engine the next read that passes the read filters, as
    // samtools mpileup does: skip any excluded flag bit, a missing required
    // bit, a mapping quality below the minimum, and — unless orphans are
    // counted — a paired read that is not properly paired.
    static int plp_callback(void* data, bam1_t* b) {
        auto* d = static_cast<PlpData*>(data);
        for (;;) {
            int rc = d->iter ? sam_itr_multi_next(d->fp, d->iter, b)
                             : sam_read1(d->fp, d->hdr, b);
            d->last_rc = rc;
            if (rc < 0) return rc;
            const uint16_t f = b->core.flag;
            if (f & d->excl_flags) continue;
            if ((f & d->incl_flags) != d->incl_flags) continue;
            if (b->core.qual < d->min_mapq) continue;
            if (!d->count_orphans && (f & BAM_FPAIRED) && !(f & BAM_FPROPER_PAIR))
                continue;
            return rc;
        }
    }

    // Render one position's bases / quals strings from the bam_pileup1_t
    // array. `ref` is the current contig's sequence (nullptr without -f) and
    // `pos` the 0-based column: with a reference, a read base matching the ref
    // becomes `.` (forward) / `,` (reverse) and deleted bases are filled from
    // the reference — matching `samtools mpileup -f`. Without one, bases are the
    // literal letters (uppercase forward, lowercase reverse) and there is no
    // match notation.
    // Entries whose base quality (at qpos, as samtools reads it — also for a
    // deletion or skip) is below `min_bq` are left out entirely, markers
    // included; returns how many were written, which is the depth column.
    static int format_pileup_row(const bam_pileup1_t* plp, int n,
                                  std::string& bases, std::string& quals,
                                  const char* ref, hts_pos_t ref_len,
                                  hts_pos_t pos, int min_bq) {
        bases.clear();
        quals.clear();
        int shown = 0;
        for (int i = 0; i < n; ++i) {
            const bam_pileup1_t* p = &plp[i];
            if (min_bq > 0) {
                const int bq = (p->qpos < p->b->core.l_qseq)
                                   ? bam_get_qual(p->b)[p->qpos] : 0;
                if (bq < min_bq) continue;
            }
            ++shown;
            // Read-start marker: ^<mapq+33> precedes the base.
            if (p->is_head) {
                bases += '^';
                int mq = p->b->core.qual;
                if (mq > 93) mq = 93;
                bases += (char)(mq + 33);
            }
            // Base column: '*' for a deletion, '>' / '<' (forward/reverse) for
            // a reference skip — CIGAR N, e.g. an RNA-seq intron — else the
            // read base (uppercase forward, lowercase reverse). htslib sets
            // is_del for both deletions and refskips; is_refskip distinguishes
            // the two. (Previously refskips were rendered as '*', mislabelling
            // spliced reads as deletions.)
            if (p->is_del) {
                if (p->is_refskip) bases += bam_is_rev(p->b) ? '<' : '>';
                else               bases += '*';
            } else {
                // p->qpos comes from htslib's CIGAR walk, not from l_qseq. A
                // record with SEQ='*' (l_qseq == 0) but a full CIGAR is legal
                // SAM and reaches here, so qpos can point past the packed SEQ
                // array (0 bytes when SEQ is absent) — an out-of-bounds read of
                // heap bytes rendered as base calls. The quality read below is
                // already bounded the same way; bases with no sequence render
                // as 'N'. (Matching samtools' depth for such reads — it drops
                // them entirely — is a separate change: depth here is unchanged.)
                char nt = (p->qpos < p->b->core.l_qseq)
                              ? seq_nt16_str[bam_seqi(bam_get_seq(p->b), p->qpos)]
                              : 'N';                     // uppercase A/C/G/T/N/=
                bool rev = bam_is_rev(p->b);
                if (ref) {
                    int rb = (pos < ref_len) ? (unsigned char)ref[pos] : 'N';
                    if (nt == '=' ||
                        seq_nt16_table[(uint8_t)nt] == seq_nt16_table[(uint8_t)rb])
                        bases += rev ? ',' : '.';        // matches the reference
                    else
                        bases += rev ? (char)std::tolower(nt) : nt;
                } else {
                    bases += rev ? (char)std::tolower(nt) : nt;
                }
            }
            // Quality column: samtools emits the base quality at qpos for every
            // element — including deletions and reference skips — never '*'.
            // BAM stores Phred directly; pileup format is Phred+33.
            {
                int q = (p->qpos < p->b->core.l_qseq)
                            ? bam_get_qual(p->b)[p->qpos] : 0;
                if (q == 0xff) q = 0;
                quals += (char)(q + 33);
            }
            // Indel description on the current base. Insertion bases come
            // from the read at qpos+1 .. qpos+indel; deletion bases come
            // from the reference (we use 'N' since -f isn't supported).
            if (p->indel > 0) {
                bases += '+';
                bases += std::to_string(p->indel);
                bool rev = bam_is_rev(p->b);
                for (int k = 1; k <= p->indel; ++k) {
                    // Same bound as the base read above: an inserted base past
                    // the record's SEQ (inconsistent CIGAR, or SEQ='*') would
                    // otherwise over-read the packed array.
                    char nt = (p->qpos + k < p->b->core.l_qseq)
                                  ? seq_nt16_str[bam_seqi(bam_get_seq(p->b),
                                                          p->qpos + k)]
                                  : 'N';
                    if (rev) nt = (char)std::tolower(nt);
                    bases += nt;
                }
            } else if (p->indel < 0) {
                int n_del = -p->indel;
                bases += '-';
                bases += std::to_string(n_del);
                bool rev = bam_is_rev(p->b);
                for (int k = 1; k <= n_del; ++k) {
                    // Deleted bases come from the reference (or 'N' without -f).
                    char rc = (ref && pos + k < ref_len)
                                  ? ref[pos + k] : 'N';
                    bases += rev ? (char)std::tolower((unsigned char)rc)
                                 : (char)std::toupper((unsigned char)rc);
                }
            }
            if (p->is_tail) bases += '$';
        }
        // Every entry filtered out: samtools prints the row with depth 0 and
        // `*` for both columns.
        if (shown == 0 && n > 0) { bases = "*"; quals = "*"; }
        return shown;
    }

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder b_chrom, b_ref, b_bases, b_quals;
        arrow::Int64Builder  b_pos, b_depth;

        int count = 0;
        int tid, pos, n_plp = 0;
        const bam_pileup1_t* plp_arr = nullptr;
        std::string bases_str, quals_str;
        while (count < BATCH_ROWS &&
               bam_mplp_auto(plp_, &tid, &pos, &n_plp, &plp_arr) > 0) {
            if (tid < 0) continue;
            // Match `samtools mpileup`'s region-trimming: bam_plp_auto
            // returns every position covered by the iterator-fetched
            // reads, but only those whose pos falls inside a requested
            // range should be emitted.
            if (!regions_.empty()) {
                bool in_any = false;
                for (const auto& r : regions_) {
                    if (r.tid == tid && pos >= r.beg && pos < r.end) {
                        in_any = true; break;
                    }
                }
                if (!in_any) continue;
            }
            const char* ref = ref_for(tid);   // nullptr without -f
            const int depth = format_pileup_row(plp_arr, n_plp, bases_str, quals_str,
                                                ref, ref_cache_len_, pos, min_bq_);
            const char* chrom = sam_hdr_tid2name(hdr_, tid);
            ARROW_RETURN_NOT_OK(b_chrom.Append(chrom ? chrom : "*"));
            ARROW_RETURN_NOT_OK(b_pos.Append((int64_t)pos + 1));  // 1-based
            // Ref column: the FASTA base as-is (case preserved, like samtools),
            // 'N' where there's no reference.
            char rb = (ref && pos < ref_cache_len_) ? ref[pos] : 'N';
            ARROW_RETURN_NOT_OK(b_ref.Append(std::string(1, rb)));
            ARROW_RETURN_NOT_OK(b_depth.Append(depth));
            ARROW_RETURN_NOT_OK(b_bases.Append(bases_str));
            ARROW_RETURN_NOT_OK(b_quals.Append(quals_str));
            ++count;
        }
        // bam_mplp_auto() stopped for one of two reasons: clean EOF
        // (last_rc == -1) or a read error (< -1, e.g. a truncated/corrupt BAM).
        // Record the latter stickily so the CLI reports a truncated file rather
        // than silently emitting a partial pileup with exit 0.
        if (plp_data_.last_rc < -1) {
            read_status_ = arrow::Status::IOError(
                "error reading BAM record from ", path_);
            all_read_ = true;
        }
        if (count == 0) {
            all_read_ = true;
            num_rows_total_ = rows_so_far_;
            return read_status_;
        }
        std::shared_ptr<arrow::Array> a_chrom, a_pos, a_ref, a_depth, a_bases, a_quals;
        ARROW_RETURN_NOT_OK(b_chrom.Finish(&a_chrom));
        ARROW_RETURN_NOT_OK(b_pos.Finish(&a_pos));
        ARROW_RETURN_NOT_OK(b_ref.Finish(&a_ref));
        ARROW_RETURN_NOT_OK(b_depth.Finish(&a_depth));
        ARROW_RETURN_NOT_OK(b_bases.Finish(&a_bases));
        ARROW_RETURN_NOT_OK(b_quals.Finish(&a_quals));
        auto batch = arrow::RecordBatch::Make(schema_, count,
            {a_chrom, a_pos, a_ref, a_depth, a_bases, a_quals});
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BamPileupSource() {
        if (plp_)  { bam_mplp_destroy(plp_);  plp_  = nullptr; }
        if (iter_) { hts_itr_multi_destroy(iter_); iter_ = nullptr; }
        if (rec_)  { bam_destroy1(rec_);     rec_  = nullptr; }
        if (idx_)  { hts_idx_destroy(idx_);  idx_  = nullptr; }
        if (hdr_)  { sam_hdr_destroy(hdr_);  hdr_  = nullptr; }
        if (fp_)   { sam_close(fp_);         fp_   = nullptr; }
        if (ref_cache_) { free(ref_cache_);  ref_cache_ = nullptr; }
        if (fai_)  { fai_destroy(fai_);      fai_  = nullptr; }
    }

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BamPileupSource>* out) {
        auto self = std::make_unique<BamPileupSource>();
        self->path_ = path;

        self->fp_ = sam_open(path.c_str(), "r");
        if (!self->fp_) return "Cannot open '" + path + "'";
        int n = effective_threads(cfg);
        if (n > 1) hts_set_threads(self->fp_, n);

        self->hdr_ = sam_hdr_read(self->fp_);
        if (!self->hdr_)
            return "Cannot read BAM/SAM header from '" + path + "'";

        // Optional reference FASTA (-f): enables the ref column and the ./,
        // match notation, matching `samtools mpileup -f`.
        if (!cfg.pileup_ref.empty()) {
            self->fai_ = fai_load(cfg.pileup_ref.c_str());
            if (!self->fai_)
                return "Cannot load reference FASTA '" + cfg.pileup_ref +
                       "' (need a .fai index; run `samtools faidx`)";
        }

        // Detect format for the footer string.
        const htsFormat* fmt = hts_get_format(self->fp_);
        switch (fmt ? fmt->format : unknown_format) {
            case cram: self->fmt_name_ = "CRAM"; break;
            case sam:  self->fmt_name_ = "SAM";  break;
            default:   self->fmt_name_ = "BAM";  break;
        }

        // Optional region query. Needs a coordinate-sorted file with an
        // index (.bai / .csi / .crai). We accept comma-separated regions
        // via the same `chrom:start-end[,…]` syntax tabix uses.
        if (!cfg.region.empty()) {
            self->idx_ = sam_index_load(self->fp_, path.c_str());
            if (!self->idx_)
                return "'" + path + "': --pileup -r needs an index (.bai/.csi/.crai)";
            // cfg.region is canonical 0-based half-open; both sam_itr_regarray
            // and the hts_parse_region filter below read region strings as
            // 1-based inclusive, so convert each window at the boundary.
            std::vector<Region> windows = parse_region_list(cfg.region);
            resolve_region_chroms(windows,
                [&](const std::string& n){ return bam_name2id(self->hdr_, n.c_str()) >= 0; },
                path);
            std::vector<std::string> regs;
            for (const auto& w : windows) regs.push_back(region_to_htslib(w));
            std::vector<const char*> regp;
            regp.reserve(regs.size());
            for (auto& r : regs) regp.push_back(r.c_str());
            self->iter_ = sam_itr_regarray(self->idx_, self->hdr_,
                                            const_cast<char**>(regp.data()),
                                            (unsigned)regp.size());
            if (!self->iter_)
                return "'" + path + "': cannot build iterator for region '" +
                       cfg.region + "'";

            // Also resolve each region into a tid + half-open [beg, end)
            // span for the per-position filter applied during pileup
            // emission. hts_parse_region accepts `chrom`, `chrom:N`, and
            // `chrom:beg-end` (beg/end 1-based inclusive, returned as
            // 0-based half-open).
            for (auto& r : regs) {
                hts_pos_t beg = 0, end = 0;
                int tid = -1;
                const char* rest = hts_parse_region(
                    r.c_str(), &tid, &beg, &end,
                    (hts_name2id_f)bam_name2id, self->hdr_,
                    HTS_PARSE_THOUSANDS_SEP);
                if (rest && tid >= 0)
                    self->regions_.push_back({tid, beg, end});
            }
        }

        self->plp_data_.fp   = self->fp_;
        self->plp_data_.hdr  = self->hdr_;
        self->plp_data_.iter = self->iter_;
        self->plp_data_.excl_flags    = cfg.pileup_excl_flags;
        self->plp_data_.incl_flags    = cfg.pileup_incl_flags;
        self->plp_data_.min_mapq      = cfg.pileup_min_mapq;
        self->plp_data_.count_orphans = cfg.pileup_count_orphans;
        self->min_bq_ = cfg.pileup_min_bq;
        void* plp_data = &self->plp_data_;
        self->plp_ = bam_mplp_init(1, &BamPileupSource::plp_callback, &plp_data);
        if (!self->plp_) return "Out of memory initialising pileup iterator";
        // Overlapping mates: htslib keeps one base with the summed quality
        // (or both halved on a mismatch) and zeroes the other, which the base
        // quality filter then drops — samtools mpileup's default (-x disables).
        if (!cfg.pileup_ignore_overlaps) bam_mplp_init_overlaps(self->plp_);
        // No per-position depth cap: the depth column is the true depth.
        // (samtools mpileup caps at -d 8000 reads per file by default.)
        bam_mplp_set_maxcnt(self->plp_, INT_MAX);

        self->rec_ = bam_init1();

        self->schema_ = arrow::schema({
            arrow::field("chrom", arrow::utf8()),
            arrow::field("pos",   arrow::int64()),
            arrow::field("ref",   arrow::utf8()),
            arrow::field("depth", arrow::int64()),
            arrow::field("bases", arrow::utf8()),
            arrow::field("quals", arrow::utf8()),
        });

        // Eager first batch so schema-only views see a populated source.
        auto st = self->advance();
        if (!st.ok())
            return "Error pileup-walking '" + path + "': " + st.ToString();

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        return all_read_ ? num_rows_total_ : -1;
    }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    bool region_applied() const override { return iter_ != nullptr; }
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
    arrow::Status read_status() const override { return read_status_; }
    std::string footer() const override {
        return "Format: mpileup (from " + fmt_name_ + ")";
    }
};

std::string open_bam_pileup_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<BamPileupSource> s;
    std::string e = BamPileupSource::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── BCF source (binary VCF via htslib) ────────────────────────────────────────

// Reads a BCF file via htslib's bcf_read, reformats each record to the
// canonical VCF text line via vcf_format(), then splits into the eight
// canonical VCF columns plus a single trailing column for any FORMAT/sample
// data. Output schema mirrors VCF text so the existing TUI INFO-expansion
// path keeps working unchanged.
class BcfSource : public TabularSource {
    std::string                              path_;
    std::shared_ptr<arrow::Schema>           schema_;
    std::vector<std::string>                 preamble_lines_;
    int                                      n_samples_ = 0;

    mutable htsFile*    fp_  = nullptr;
    mutable bcf_hdr_t*  hdr_ = nullptr;
    mutable bcf1_t*     rec_ = nullptr;

    // Region-mode (--region): index + iterators over the requested windows.
    mutable hts_idx_t*               idx_       = nullptr;
    mutable std::vector<hts_itr_t*>  iters_;
    mutable size_t                   cur_iter_  = 0;
    bool                              region_mode_ = false;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             all_read_ = false;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;
    mutable arrow::Status                    read_status_;   // sticky stream error

    static constexpr int BATCH_SIZE = 32768;

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder chrom_b, id_b, ref_b, alt_b, filter_b, info_b, format_b;
        std::vector<arrow::StringBuilder> sample_b((size_t)n_samples_);  // one per sample
        arrow::Int64Builder  pos_b;
        arrow::FloatBuilder  qual_b;

        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        int count = 0, ret = 0;
        kstring_t s = {0, 0, nullptr};

        // next_record(): pull one record either from the linear stream or
        // from the current region iterator (advancing to the next iterator
        // on exhaustion). Returns 0 on a record, -1 at EOF, <-1 on error.
        auto next_record = [&]() -> int {
            if (!region_mode_) return bcf_read(fp_, hdr_, rec_);
            while (cur_iter_ < iters_.size()) {
                int r = bcf_itr_next(fp_, iters_[cur_iter_], rec_);
                if (r >= 0) return 0;
                if (r < -1) return r;   // genuine read error — propagate (a
                                        // truncated/corrupt file, not iter EOF)
                ++cur_iter_;            // r == -1: this iterator done; next one
            }
            return -1;
        };

        while (count < cap && (ret = next_record()) == 0) {
            bcf_unpack(rec_, BCF_UN_ALL);

            // vcf_format renders the canonical tab-separated line. We split
            // it on tabs to fill the eight fixed columns; anything past the
            // 9th tab (FORMAT + samples) is kept as a single string.
            s.l = 0;
            if (vcf_format(hdr_, rec_, &s) < 0)
                return arrow::Status::IOError("vcf_format failed for ", path_);
            std::string_view line(s.s, s.l);
            // Strip a trailing newline if present.
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.remove_suffix(1);

            std::array<std::string_view, 9> f;     // first 8 + tail
            int fi = 0;
            size_t start = 0;
            for (size_t i = 0; i < line.size() && fi < 8; ++i) {
                if (line[i] == '\t') { f[fi++] = line.substr(start, i - start); start = i + 1; }
            }
            // After the eight fixed fields (CHROM..INFO), `start` points at the
            // FORMAT field. Everything from there on — FORMAT plus the
            // per-sample columns — is split into FORMAT and one value per sample.
            // (Previously this skipped to after the next tab, dropping the
            // FORMAT spec such as GT:AD:DP entirely.)
            if (fi < 8) {
                // Pathological short line — fill remaining columns with ".".
                size_t end = line.find('\t', start);
                f[fi++] = (end == std::string_view::npos)
                          ? line.substr(start)
                          : line.substr(start, end - start);
                while (fi < 8) f[fi++] = ".";
                f[8] = std::string_view{};
            } else {
                f[8] = line.substr(start);   // FORMAT + samples, verbatim
            }

            ARROW_RETURN_NOT_OK(chrom_b.Append(f[0].data(), (int32_t)f[0].size()));
            int64_t pos = 0;
            std::from_chars(f[1].data(), f[1].data() + f[1].size(), pos);
            ARROW_RETURN_NOT_OK(pos_b.Append(pos));
            ARROW_RETURN_NOT_OK(id_b.Append(f[2].data(), (int32_t)f[2].size()));
            ARROW_RETURN_NOT_OK(ref_b.Append(f[3].data(), (int32_t)f[3].size()));
            ARROW_RETURN_NOT_OK(alt_b.Append(f[4].data(), (int32_t)f[4].size()));
            // QUAL: "." → null
            if (f[5] == "." || f[5].empty()) {
                ARROW_RETURN_NOT_OK(qual_b.AppendNull());
            } else {
                // std::from_chars<float> is missing in Apple's libc++ on
                // some macOS runners; strtof works everywhere. f[5] is a
                // string_view (not nul-terminated), so copy to a temp.
                std::string qbuf(f[5]);
                float q = std::strtof(qbuf.c_str(), nullptr);
                ARROW_RETURN_NOT_OK(qual_b.Append(q));
            }
            ARROW_RETURN_NOT_OK(filter_b.Append(f[6].data(), (int32_t)f[6].size()));
            ARROW_RETURN_NOT_OK(info_b.Append  (f[7].data(), (int32_t)f[7].size()));
            if (n_samples_ > 0) {
                // FORMAT, then one field per sample (as the text VCF reader has).
                std::string_view tail = f[8];
                for (int k = -1; k < n_samples_; ++k) {
                    const size_t tab = tail.find('\t');
                    std::string_view v = tail.substr(0, tab);
                    auto& b = (k < 0) ? format_b : sample_b[(size_t)k];
                    if (v.empty() && tab == std::string_view::npos && tail.empty())
                        ARROW_RETURN_NOT_OK(b.AppendNull());
                    else
                        ARROW_RETURN_NOT_OK(b.Append(v.data(), (int32_t)v.size()));
                    tail = (tab == std::string_view::npos) ? std::string_view{} : tail.substr(tab + 1);
                }
            }

            ++count;
        }
        if (s.s) free(s.s);

        if (ret < -1) {
            // Record the error stickily (callers ignore advance()'s return) so
            // the CLI reports a truncated/corrupt file instead of silently
            // emitting a partial result with exit 0.
            read_status_ = arrow::Status::IOError(
                "error reading BCF record from ", path_);
            all_read_ = true;
        }
        if (count == 0) { all_read_ = true; return read_status_; }
        if (ret < 0) all_read_ = true;

        std::vector<std::shared_ptr<arrow::Array>> a;
        std::shared_ptr<arrow::Array> tmp;
        ARROW_RETURN_NOT_OK(chrom_b.Finish(&tmp));  a.push_back(tmp);
        ARROW_RETURN_NOT_OK(pos_b.Finish(&tmp));    a.push_back(tmp);
        ARROW_RETURN_NOT_OK(id_b.Finish(&tmp));     a.push_back(tmp);
        ARROW_RETURN_NOT_OK(ref_b.Finish(&tmp));    a.push_back(tmp);
        ARROW_RETURN_NOT_OK(alt_b.Finish(&tmp));    a.push_back(tmp);
        ARROW_RETURN_NOT_OK(qual_b.Finish(&tmp));   a.push_back(tmp);
        ARROW_RETURN_NOT_OK(filter_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(info_b.Finish(&tmp));   a.push_back(tmp);
        if (n_samples_ > 0) {
            ARROW_RETURN_NOT_OK(format_b.Finish(&tmp));
            a.push_back(tmp);
            for (auto& b : sample_b) {
                ARROW_RETURN_NOT_OK(b.Finish(&tmp));
                a.push_back(tmp);
            }
        }

        auto batch = arrow::RecordBatch::Make(schema_, count, a);
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BcfSource() {
        for (auto* it : iters_) if (it) hts_itr_destroy(it);
        if (idx_) { hts_idx_destroy(idx_); idx_ = nullptr; }
        if (rec_) { bcf_destroy(rec_);     rec_ = nullptr; }
        if (hdr_) { bcf_hdr_destroy(hdr_); hdr_ = nullptr; }
        if (fp_)  { hts_close(fp_);        fp_  = nullptr; }
    }

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BcfSource>* out) {
        auto self = std::make_unique<BcfSource>();
        self->path_ = path;

        self->fp_ = hts_open(path.c_str(), "r");
        if (!self->fp_) return "Cannot open '" + path + "'";

        int n = effective_threads(cfg);
        if (n > 1) hts_set_threads(self->fp_, n);

        self->hdr_ = bcf_hdr_read(self->fp_);
        if (!self->hdr_) return "Cannot read BCF header from '" + path + "'";
        self->rec_ = bcf_init();
        if (!self->rec_) return "Out of memory allocating BCF record";

        self->n_samples_ = bcf_hdr_nsamples(self->hdr_);

        // Header lines (## meta-information) → preamble.
        {
            int n_lines = 0;
            char** hdr_text = nullptr;
            kstring_t hs = {0, 0, nullptr};
            if (bcf_hdr_format(self->hdr_, /*is_bcf=*/0, &hs) == 0 && hs.s) {
                std::istringstream iss(std::string(hs.s, hs.l));
                std::string line;
                int total = 0;
                while (std::getline(iss, line)) {
                    if (line.empty()) continue;
                    ++total;
                    if (total <= 20) self->preamble_lines_.push_back(line);
                }
                if (total > 20)
                    self->preamble_lines_.push_back(
                        "... (" + std::to_string(total - 20) + " more header lines)");
            }
            free(hs.s);
            (void)hdr_text; (void)n_lines;
        }

        // ── Range mode: load .csi/.tbi index and build per-window iterators ─
        if (!cfg.region.empty()) {
            self->idx_ = bcf_index_load(path.c_str());
            if (!self->idx_)
                return "No BCF index for '" + path + "' (try: "
                       "`bcftools index '" + path + "'`)";
            // cfg.region is canonical 0-based half-open; bcf_itr_querys reads
            // its string argument as 1-based inclusive, so convert per window.
            std::vector<Region> windows = parse_region_list(cfg.region);
            resolve_region_chroms(windows,
                [&](const std::string& n){ return bcf_hdr_name2id(self->hdr_, n.c_str()) >= 0; },
                path);
            for (const auto& r : windows) {
                std::string rstr = region_to_htslib(r);
                hts_itr_t* it =
                    bcf_itr_querys(self->idx_, self->hdr_, rstr.c_str());
                if (!it)
                    return "Cannot query region '" + rstr + "' in '" + path + "'";
                self->iters_.push_back(it);
            }
            self->region_mode_ = true;
        }

        arrow::FieldVector fields = {
            arrow::field("CHROM",  arrow::utf8()),
            arrow::field("POS",    arrow::int64()),
            arrow::field("ID",     arrow::utf8()),
            arrow::field("REF",    arrow::utf8()),
            arrow::field("ALT",    arrow::utf8()),
            arrow::field("QUAL",   arrow::float32()),
            arrow::field("FILTER", arrow::utf8()),
            arrow::field("INFO",   arrow::utf8()),
        };
        // FORMAT and one column per sample, named from the header — the
        // columns the text VCF reader has (they were one tab-joined
        // FORMAT_SAMPLES column).
        if (self->n_samples_ > 0) {
            fields.push_back(arrow::field("FORMAT", arrow::utf8()));
            for (int k = 0; k < self->n_samples_; ++k)
                fields.push_back(arrow::field(self->hdr_->samples[k], arrow::utf8()));
        }
        self->schema_ = arrow::schema(fields);

        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();
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
    bool region_applied() const override { return region_mode_; }
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
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        if (batches_.empty() && !all_read_ && rows > 0)
            ARROW_RETURN_NOT_OK(advance(rows));
        return TabularSource::read_first(rows, col_indices, out);
    }
    const std::string& path() const override { return path_; }
    arrow::Status read_status() const override { return read_status_; }
    std::string footer() const override {
        return "Format: BCF  |  Samples: " + std::to_string(n_samples_);
    }
    std::vector<std::string> preamble_below() const override { return preamble_lines_; }
    std::string format_cell(int col_idx, std::string val) const override {
        if (col_idx == 1) return digits_with_sep(val);  // POS
        return val;
    }
    int min_col_width(int col_idx) const override {
        switch (col_idx) {
            case 0:  return 6;   // CHROM
            case 1:  return 9;   // POS
            case 6:  return 6;   // FILTER
            default: return 4;
        }
    }
};

std::string open_bcf_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<BcfSource> s;
    std::string e = BcfSource::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── FASTA / FASTQ source (kseq.h via htslib BGZF) ─────────────────────────────

// kseq's input: BGZF for plain, gzip and bgzip files (multi-threaded
// decompression), or a decoded stream for zstd, bzip2 and xz.
struct FastxIn {
    BGZF* bg = nullptr;
    std::shared_ptr<arrow::io::InputStream> st;
    ~FastxIn() { if (bg) bgzf_close(bg); }
};
static int fastx_read(FastxIn* f, void* buf, int len) {
    if (f->bg) return bgzf_read(f->bg, buf, len);
    auto r = f->st->Read(len, buf);
    return r.ok() ? (int)*r : -1;
}
static std::string open_fastx_input(const std::string& path, const Config& cfg, FastxIn* in) {
    auto rf = arrow::io::ReadableFile::Open(path);
    if (!rf.ok()) return "Cannot open '" + path + "'";
    const StreamCodec c = sniff_file_codec(*rf);
    if (c != StreamCodec::None && c != StreamCodec::Gzip) {
        if (auto e = decode_stream(c, *rf, &in->st); !e.empty()) return "'" + path + "': " + e;
        return "";
    }
    (void)(*rf)->Close();
    in->bg = bgzf_open(path.c_str(), "r");
    if (!in->bg) return "Cannot open '" + path + "'";
    if (int n = effective_threads(cfg); n > 1) bgzf_mt(in->bg, n, 256);
    return "";
}

KSEQ_INIT(FastxIn*, fastx_read)

class FastxSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    bool                                   is_fastq_ = false;

    mutable FastxIn  in_;
    mutable kseq_t*  ks_ = nullptr;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>           batch_first_row_;
    mutable std::vector<int64_t>           batch_num_rows_;   // kept after eviction
    mutable int64_t                        rows_so_far_ = 0;
    mutable bool                           all_read_    = false;
    mutable bool                           retain_all_  = false;
    mutable bool                           evicted_any_ = false;
    mutable arrow::Status                  read_status_;   // sticky stream error

    static constexpr int BATCH_SIZE = 4096;

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder name_b, comm_b, seq_b, qual_b;
        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        int count = 0, ret = 0;
        int64_t bytes = 0;
        const int64_t byte_budget = fastx_byte_budget();
        while (count < cap && (ret = kseq_read(ks_)) >= 0) {
            // kseq doesn't reset .s on absent fields, only .l → use length.
            ARROW_RETURN_NOT_OK(name_b.Append(
                ks_->name.s ? ks_->name.s : "", (int32_t)ks_->name.l));
            ARROW_RETURN_NOT_OK(comm_b.Append(
                ks_->comment.s ? ks_->comment.s : "", (int32_t)ks_->comment.l));
            ARROW_RETURN_NOT_OK(seq_b.Append(
                ks_->seq.s ? ks_->seq.s : "", (int32_t)ks_->seq.l));
            if (is_fastq_)
                ARROW_RETURN_NOT_OK(qual_b.Append(
                    ks_->qual.s ? ks_->qual.s : "", (int32_t)ks_->qual.l));
            ++count;
            // Close the batch once its payload passes the budget, even below the
            // row cap, so a few very long records can't build one huge batch.
            bytes += (int64_t)ks_->name.l + (int64_t)ks_->comment.l +
                     (int64_t)ks_->seq.l +
                     (is_fastq_ ? (int64_t)ks_->qual.l : 0);
            if (bytes >= byte_budget) break;
        }
        if (ret < -1) {
            // Malformed record (kseq_read returns < -1). Record it stickily and
            // stop so ensure()'s (void)advance() loop can't spin forever.
            read_status_ = arrow::Status::IOError(
                "Error reading FASTA/FASTQ from ", path_);
            all_read_ = true;
            return read_status_;
        }
        if (count == 0) { all_read_ = true; return arrow::Status::OK(); }
        if (ret < 0) all_read_ = true;

        std::vector<std::shared_ptr<arrow::Array>> a;
        std::shared_ptr<arrow::Array> tmp;
        ARROW_RETURN_NOT_OK(name_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(comm_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(seq_b.Finish(&tmp));  a.push_back(tmp);
        if (is_fastq_) {
            ARROW_RETURN_NOT_OK(qual_b.Finish(&tmp));
            a.push_back(tmp);
        }
        auto batch = arrow::RecordBatch::Make(schema_, count, a);
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~FastxSource() {
        if (ks_) { kseq_destroy(ks_); ks_ = nullptr; }
    }

    static std::string open(const std::string& path, bool is_fastq,
                            const Config& cfg,
                            std::unique_ptr<FastxSource>* out)
    {
        auto self = std::make_unique<FastxSource>();
        self->path_     = path;
        self->is_fastq_ = is_fastq;

        if (auto e = open_fastx_input(path, cfg, &self->in_); !e.empty()) return e;
        self->ks_ = kseq_init(&self->in_);
        if (!self->ks_) return "Cannot init kseq for '" + path + "'";

        arrow::FieldVector fields = {
            arrow::field("name",    arrow::utf8()),
            arrow::field("comment", arrow::utf8()),
            arrow::field("seq",     arrow::utf8()),
        };
        if (is_fastq) fields.push_back(arrow::field("qual", arrow::utf8()));
        self->schema_ = arrow::schema(fields);

        // Records are read on demand, so an -n preview or a thumbnail
        // (read_first) reads only the rows it shows; a malformed file is
        // reported by the first read (read_status).
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
    // The first `rows` rows, reading only as many records as that needs
    // (open() read one): a batch not yet read is read capped at the
    // shortfall, not as a whole 4096-record / byte-budget batch. Each chunk
    // is collected as soon as it exists, as the base read_first does.
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        std::shared_ptr<arrow::Table> acc;
        for (int c = 0; !acc || acc->num_rows() < rows; ++c) {
            const int64_t got = acc ? acc->num_rows() : 0;
            while (!all_read_ && c >= (int)batches_.size())
                ARROW_RETURN_NOT_OK(advance(rows - got));
            if (c >= (int)batches_.size()) break;
            std::shared_ptr<arrow::Table> chunk;
            ARROW_RETURN_NOT_OK(read_chunk(c, col_indices, &chunk));
            if (!acc) { acc = chunk; continue; }
            ARROW_ASSIGN_OR_RAISE(acc, arrow::ConcatenateTables({acc, chunk}));
        }
        if (acc && acc->num_rows() > rows) acc = acc->Slice(0, rows);
        *out = acc;
        return arrow::Status::OK();
    }
    arrow::Status read_status() const override { return read_status_; }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return std::string("Format: ") + (is_fastq_ ? "FASTQ" : "FASTA");
    }
    int min_col_width(int col_idx) const override {
        switch (col_idx) {
            case 0: return 12;  // name
            case 1: return 8;   // comment
            case 2: return 16;  // seq
            case 3: return 16;  // qual
            default: return 4;
        }
    }
};

std::string open_fastx_source(const std::string& path, bool is_fastq, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<FastxSource> s;
    std::string e = FastxSource::open(path, is_fastq, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── mpileup --decode-pileup helpers ──────────────────────────────────────────
//
// Explodes the packed `bases` cell of an mpileup row into per-allele
// counts. Format: . = match on forward strand, , = match on reverse
// strand, [ACGTNacgtn] = mismatch on that strand, * = deletion
// placeholder (counts toward depth, consumes a quality char), ^X = start
// of read where X-33 is mapping quality (the char *after* X is the
// actual base), $ = end of read (postfix on previous base), +N<seq> =
// insertion of N bases after the previous base (consumes N bases from
// the string but no quality chars), -N<seq> = deletion of N bases
// likewise. We count A/C/G/T/N (case-insensitive; matches map to the
// reference allele), ins / del events, deletion placeholders, forward /
// reverse strand reads, and mean Phred quality across the real bases.

struct PileupCounts {
    int64_t A = 0, C = 0, G = 0, T = 0, N = 0;
    int64_t del_placeholder = 0;   // count of '*' characters
    int64_t ins = 0;               // count of +N<seq> events
    int64_t del = 0;               // count of -N<seq> events
    int64_t fwd = 0, rev = 0;
    double  mean_qual = -1.0;      // -1 when no quality chars consumed
};

static PileupCounts decode_pileup_cell(const std::string_view bases,
                                        const std::string_view quals,
                                        char ref) {
    PileupCounts c;
    char ref_upper = (char)std::toupper((unsigned char)ref);
    size_t qual_idx = 0;
    int64_t qual_sum = 0;
    int     qual_count = 0;
    // Every pileup element — a real base, a '*' deletion placeholder, or a
    // '>' / '<' reference skip — carries exactly one quality char, so the index
    // must advance for each to stay aligned with the base column. Only real
    // bases contribute to mean_qual (`count`); a placeholder or refskip consumes
    // its quality char without adding to the mean. Skipping the advance for a
    // non-base (as the old code did for refskips) both dropped their quality and
    // shifted every later base onto the wrong quality char.
    auto consume_qual = [&](bool count) {
        if (count && qual_idx < quals.size()) {
            qual_sum += (int)(unsigned char)quals[qual_idx] - 33;
            ++qual_count;
        }
        ++qual_idx;
    };
    auto bump_base = [&](char b, bool reverse) {
        char u = (char)std::toupper((unsigned char)b);
        if      (u == 'A') ++c.A;
        else if (u == 'C') ++c.C;
        else if (u == 'G') ++c.G;
        else if (u == 'T') ++c.T;
        else               ++c.N;
        if (reverse) ++c.rev; else ++c.fwd;
        consume_qual(/*count=*/true);
    };
    for (size_t i = 0; i < bases.size(); ) {
        char ch = bases[i];
        if (ch == '^') {                       // ^<mapq><base>: skip ^ and mapq
            i += 2; continue;
        }
        if (ch == '$') { ++i; continue; }      // postfix end-of-read marker
        if (ch == '*') {                        // deletion placeholder
            ++c.del_placeholder;
            consume_qual(/*count=*/false);       // has a quality char, not a base
            ++i;
            continue;
        }
        if (ch == '>' || ch == '<') {           // reference skip (CIGAR N)
            consume_qual(/*count=*/false);       // has a quality char, not a base
            ++i;
            continue;
        }
        if (ch == '+' || ch == '-') {           // indel description on prev base
            ++i;
            int n = 0;
            while (i < bases.size() && std::isdigit((unsigned char)bases[i])) {
                n = n * 10 + (bases[i] - '0');
                ++i;
            }
            if (ch == '+') ++c.ins; else ++c.del;
            // Skip the indel sequence; no quality chars belong to it.
            i = std::min(i + (size_t)n, bases.size());
            continue;
        }
        if (ch == '.' || ch == ',') {
            bump_base(ref_upper, /*reverse=*/ch == ',');
            ++i;
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            bool reverse = (ch >= 'a' && ch <= 'z');
            bump_base(ch, reverse);
            ++i;
            continue;
        }
        // Unknown char (rare) — skip silently. '>' / '<' refskips and '*'
        // deletion placeholders are handled above so their quality chars stay
        // aligned; anything else here carries no quality char.
        ++i;
    }
    c.mean_qual = (qual_count > 0)
                  ? (double)qual_sum / qual_count
                  : -1.0;
    return c;
}

// Read every row from the underlying mpileup DelimitedSource, decode each
// packed bases/quals cell, and return a TabularSource backed by an Arrow
// Table with the typed per-allele schema. The decoded view is materialised
// in memory; range queries on bgzipped mpileup files (-r chr:start-end)
// still go through the underlying source first, so only the queried rows
// hit the decoder.
std::string decode_mpileup_to_memory(
 TabularSource& src,
 const std::string& path,
 std::unique_ptr<TabularSource>* out) {
    auto schema = src.schema();
    int nf = schema->num_fields();
    if (nf < 6 || (nf - 3) % 3 != 0)
        return "decode-pileup: unexpected column count (" +
               std::to_string(nf) + ") for an mpileup file";
    int samples = (nf - 3) / 3;

    // Output schema: chrom/pos/ref + 11 typed columns per sample
    // (depth + A/C/G/T/N + del_placeholder + ins/del + fwd/rev + mean_qual).
    arrow::FieldVector fields;
    fields.push_back(arrow::field("chrom", arrow::utf8()));
    fields.push_back(arrow::field("pos",   arrow::int64()));
    fields.push_back(arrow::field("ref",   arrow::utf8()));
    static const char* kNames[] = {
        "A", "C", "G", "T", "N",
        "del_placeholder", "ins", "del",
        "fwd", "rev", "mean_qual"
    };
    for (int s = 0; s < samples; ++s) {
        std::string sfx = (samples == 1) ? std::string{}
                                            : "_" + std::to_string(s + 1);
        fields.push_back(arrow::field("depth" + sfx, arrow::int64()));
        for (int k = 0; k < 11; ++k) {
            auto t = (std::string(kNames[k]) == "mean_qual")
                     ? arrow::float64() : arrow::int64();
            fields.push_back(arrow::field(std::string(kNames[k]) + sfx, t));
        }
    }
    auto out_schema = arrow::schema(fields);

    // Builders in matching order.
    arrow::StringBuilder  b_chrom;
    arrow::Int64Builder   b_pos;
    arrow::StringBuilder  b_ref;
    struct SampleBuilders {
        arrow::Int64Builder   depth;
        arrow::Int64Builder   A, C, G, T, N;
        arrow::Int64Builder   del_p, ins, del, fwd, rev;
        arrow::DoubleBuilder  mean_qual;
    };
    std::vector<SampleBuilders> sb(samples);

    int nc = src.num_chunks();
    std::vector<int> all_cols(nf);
    std::iota(all_cols.begin(), all_cols.end(), 0);

    for (int ci = 0; ci < nc; ++ci) {
        std::shared_ptr<arrow::Table> chunk;
        auto st = src.read_chunk(ci, all_cols, &chunk);
        if (!st.ok()) return "decode-pileup: " + st.ToString();
        if (!chunk || chunk->num_rows() == 0) continue;
        auto cmb = chunk->CombineChunks();
        if (!cmb.ok()) return "decode-pileup: " + cmb.status().ToString();
        chunk = *cmb;

        auto col_chrom = std::dynamic_pointer_cast<arrow::StringArray>(
            chunk->column(0)->chunk(0));
        auto col_pos   = std::dynamic_pointer_cast<arrow::Int64Array>(
            chunk->column(1)->chunk(0));
        auto col_ref   = std::dynamic_pointer_cast<arrow::StringArray>(
            chunk->column(2)->chunk(0));
        if (!col_chrom || !col_pos || !col_ref)
            return "decode-pileup: unexpected types in chrom/pos/ref columns";

        std::vector<std::shared_ptr<arrow::Int64Array>>  col_depth(samples);
        std::vector<std::shared_ptr<arrow::StringArray>> col_bases(samples);
        std::vector<std::shared_ptr<arrow::StringArray>> col_quals(samples);
        for (int s = 0; s < samples; ++s) {
            col_depth[s] = std::dynamic_pointer_cast<arrow::Int64Array>(
                chunk->column(3 + s*3)->chunk(0));
            col_bases[s] = std::dynamic_pointer_cast<arrow::StringArray>(
                chunk->column(4 + s*3)->chunk(0));
            col_quals[s] = std::dynamic_pointer_cast<arrow::StringArray>(
                chunk->column(5 + s*3)->chunk(0));
            if (!col_depth[s] || !col_bases[s] || !col_quals[s])
                return "decode-pileup: sample " + std::to_string(s+1) +
                       " has unexpected types";
        }

        int64_t n = chunk->num_rows();
        for (int64_t r = 0; r < n; ++r) {
            (void)b_chrom.Append(col_chrom->GetView(r));
            (void)b_pos.Append(col_pos->Value(r));
            std::string_view rv = col_ref->IsNull(r)
                                  ? std::string_view{}
                                  : col_ref->GetView(r);
            (void)b_ref.Append(rv);
            char ref_char = rv.empty() ? 'N' : rv[0];
            for (int s = 0; s < samples; ++s) {
                int64_t depth_val = col_depth[s]->IsNull(r)
                                    ? 0 : col_depth[s]->Value(r);
                std::string_view bases = col_bases[s]->IsNull(r)
                                          ? std::string_view{}
                                          : col_bases[s]->GetView(r);
                std::string_view quals = col_quals[s]->IsNull(r)
                                          ? std::string_view{}
                                          : col_quals[s]->GetView(r);
                auto c = decode_pileup_cell(bases, quals, ref_char);
                (void)sb[s].depth.Append(depth_val);
                (void)sb[s].A.Append(c.A);
                (void)sb[s].C.Append(c.C);
                (void)sb[s].G.Append(c.G);
                (void)sb[s].T.Append(c.T);
                (void)sb[s].N.Append(c.N);
                (void)sb[s].del_p.Append(c.del_placeholder);
                (void)sb[s].ins.Append(c.ins);
                (void)sb[s].del.Append(c.del);
                (void)sb[s].fwd.Append(c.fwd);
                (void)sb[s].rev.Append(c.rev);
                if (c.mean_qual < 0)
                    (void)sb[s].mean_qual.AppendNull();
                else
                    (void)sb[s].mean_qual.Append(c.mean_qual);
            }
        }
    }

    std::vector<std::shared_ptr<arrow::Array>> arrs;
    auto finish = [&](auto& bldr) -> std::string {
        std::shared_ptr<arrow::Array> a;
        auto st = bldr.Finish(&a);
        if (!st.ok()) return st.ToString();
        arrs.push_back(std::move(a));
        return "";
    };
    std::string ferr;
    ferr = finish(b_chrom); if (!ferr.empty()) return "decode-pileup: " + ferr;
    ferr = finish(b_pos);   if (!ferr.empty()) return "decode-pileup: " + ferr;
    ferr = finish(b_ref);   if (!ferr.empty()) return "decode-pileup: " + ferr;
    for (int s = 0; s < samples; ++s) {
        ferr = finish(sb[s].depth);   if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].A);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].C);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].G);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].T);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].N);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].del_p);   if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].ins);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].del);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].fwd);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].rev);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].mean_qual); if (!ferr.empty()) return "decode-pileup: " + ferr;
    }

    auto table = arrow::Table::Make(out_schema, arrs);
    std::string footer = "Format: mpileup (decoded)";
    if (samples > 1)
        footer += "  |  Samples: " + std::to_string(samples);
    *out = std::make_unique<MemoryTableSource>(table, path, std::move(footer));
    return "";
}

// ── --contigs: reference-sequence dictionary + assembly detection ─────────────
//
// Lists the sequences a genomics file is aligned / called against — the @SQ
// lines of a BAM/CRAM/SAM header, or the ##contig records of a VCF/BCF — as a
// (name, length) table, and fingerprints the assembly by the length of chr1.
// Reads only the header, never the records. The result is a MemoryTableSource,
// so --tsv / --json / --sort / --filter / the TUI all work on it.

namespace contigs {

// chr1 length → assembly, for the common vertebrate references. A single
// primary-chromosome length is distinctive enough for a best-effort label; the
// values are the canonical chr1 lengths of each assembly.
struct AssemblyEntry { int64_t chr1_len; const char* name; const char* species; };
static const AssemblyEntry kAssemblies[] = {
    {248956422, "GRCh38 / hg38",     "Homo sapiens"},
    {249250621, "GRCh37 / hg19",     "Homo sapiens"},
    {247249719, "NCBI36 / hg18",     "Homo sapiens"},
    {248387328, "T2T-CHM13v2.0",     "Homo sapiens"},
    {195154279, "GRCm39 / mm39",     "Mus musculus"},
    {195471971, "GRCm38 / mm10",     "Mus musculus"},
    {197195432, "NCBI37 / mm9",      "Mus musculus"},
    {260522016, "mRatBN7.2 / rn7",   "Rattus norvegicus"},
    {282763074, "Rnor_6.0 / rn6",    "Rattus norvegicus"},
    { 59578282, "GRCz11 / danRer11", "Danio rerio"},
};

// Best-effort assembly label from the contig table, or "" if none matches.
static std::string detect(const std::vector<std::string>& names,
                          const std::vector<int64_t>& lengths) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (lengths[i] <= 0) continue;
        if (names[i] != "chr1" && names[i] != "1") continue;
        for (const auto& a : kAssemblies)
            if (a.chr1_len == lengths[i])
                return std::string(a.name) + " (" + a.species + ")";
    }
    return "";
}

}  // namespace contigs

// Distinct values in first-seen order.
static void push_distinct(std::vector<std::string>& v, std::string x) {
    if (!x.empty() && std::find(v.begin(), v.end(), x) == v.end())
        v.push_back(std::move(x));
}

std::string read_genomic_header(const std::string& path, const std::string& reference,
                                GenomicHeader* out) {
    *out = GenomicHeader{};
    bool is_aln = fends_ci(path, ".bam") || fends_ci(path, ".cram") ||
                  fends_ci(path, ".sam");
    bool is_var = fends_ci(path, ".vcf")   || fends_ci(path, ".vcf.gz") ||
                  fends_ci(path, ".bcf");
    if (!is_aln && !is_var)
        return "'" + path + "': --contigs applies to BAM/CRAM/SAM and VCF/BCF "
               "(the reference sequences named in the header)";

    htsFile* fp = hts_open(path.c_str(), "r");
    if (!fp) return "Cannot open '" + path + "'";
    if (!reference.empty())
        (void)hts_set_fai_filename(fp, reference.c_str());

    GenomicHeader& g = *out;
    const htsFormat* fmt = hts_get_format(fp);
    bool variant = fmt && fmt->category == variant_data;

    if (variant) {
        bcf_hdr_t* h = bcf_hdr_read(fp);
        if (!h) { hts_close(fp); return "Cannot read VCF/BCF header from '" + path + "'"; }
        int n = h->n[BCF_DT_CTG];
        for (int i = 0; i < n; ++i) {
            const char* key = h->id[BCF_DT_CTG][i].key;
            if (!key) continue;
            g.contig_names.emplace_back(key);
            // Contig length lives in info[0] of the CTG dictionary entry; 0 when
            // the ##contig line carried no length=.
            const bcf_idinfo_t* v = h->id[BCF_DT_CTG][i].val;
            g.contig_lengths.push_back(v ? (int64_t)v->info[0] : 0);
        }
        for (int i = 0; i < bcf_hdr_nsamples(h); ++i)
            g.samples.emplace_back(h->samples[i]);
        for (int i = 0; i < h->nhrec; ++i) {
            const bcf_hrec_t* r = h->hrec[i];
            if (r->type == BCF_HL_GEN && r->key && r->value &&
                std::strcmp(r->key, "source") == 0)
                push_distinct(g.programs, r->value);
        }
        bcf_hdr_destroy(h);
    } else {
        sam_hdr_t* h = sam_hdr_read(fp);
        if (!h) { hts_close(fp); return "Cannot read BAM/SAM header from '" + path + "'"; }
        int n = sam_hdr_nref(h);
        for (int i = 0; i < n; ++i) {
            const char* nm = sam_hdr_tid2name(h, i);
            g.contig_names.emplace_back(nm ? nm : "");
            g.contig_lengths.push_back((int64_t)sam_hdr_tid2len(h, i));
        }
        kstring_t ks = KS_INITIALIZE;
        if (sam_hdr_find_tag_hd(h, "SO", &ks) == 0) g.sort_order = ks.s;
        int nrg = sam_hdr_count_lines(h, "RG");
        g.read_groups = nrg > 0 ? nrg : 0;
        for (int i = 0; i < nrg; ++i)
            if (sam_hdr_find_tag_pos(h, "RG", i, "SM", &ks) == 0) push_distinct(g.samples, ks.s);
        int npg = sam_hdr_count_lines(h, "PG");
        for (int i = 0; i < npg; ++i) {
            std::string prog;
            if (sam_hdr_find_tag_pos(h, "PG", i, "PN", &ks) == 0)      prog = ks.s;
            else if (sam_hdr_find_tag_pos(h, "PG", i, "ID", &ks) == 0) prog = ks.s;
            if (!prog.empty() && sam_hdr_find_tag_pos(h, "PG", i, "VN", &ks) == 0)
                prog += std::string(" ") + ks.s;
            push_distinct(g.programs, std::move(prog));
        }
        ks_free(&ks);
        sam_hdr_destroy(h);
    }
    hts_close(fp);
    g.assembly = contigs::detect(g.contig_names, g.contig_lengths);
    return "";
}

std::vector<std::pair<std::string, std::string>>
genomic_header_fields(const GenomicHeader& h) {
    // "a, b, c, … (N)" — the first `show` items, then the total.
    auto abbreviate = [](const std::vector<std::string>& v, size_t show) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < show; ++i)
            s += (i ? ", " : "") + v[i];
        if (v.size() > show) s += ", … (" + std::to_string(v.size()) + ")";
        return s;
    };
    std::vector<std::pair<std::string, std::string>> f;
    f.emplace_back("Reference sequences", std::to_string(h.contig_names.size()));
    if (!h.assembly.empty())   f.emplace_back("Assembly", h.assembly);
    if (!h.sort_order.empty()) f.emplace_back("Sorted", h.sort_order);
    if (h.read_groups > 0)     f.emplace_back("Read groups", std::to_string(h.read_groups));
    if (!h.samples.empty())    f.emplace_back("Samples", abbreviate(h.samples, 3));
    if (!h.programs.empty())   f.emplace_back("Programs", abbreviate(h.programs, 4));
    return f;
}

bool read_index_stats(const std::string& path, IndexStats* out) {
    struct stat st;
    if (path == "-" || ::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    const int level = hts_get_log_level();
    hts_set_log_level(HTS_LOG_OFF);      // a missing index is expected here
    bool ok = false;
    htsFile* fp = hts_open(path.c_str(), "r");
    if (fp) {
        const htsFormat* f = hts_get_format(fp);
        IndexStats s;
        auto collect = [&](hts_idx_t* idx, int n, auto name_of) {
            if (!idx || hts_idx_fmt(idx) == HTS_FMT_CRAI) return false;
            for (int i = 0; i < n; ++i) {
                uint64_t m = 0, u = 0;
                if (hts_idx_get_stat(idx, i, &m, &u) < 0) m = u = 0;   // no records
                const char* nm = name_of(i);
                s.names.emplace_back(nm ? nm : "");
                s.mapped.push_back(m);
                s.unmapped.push_back(s.variant ? 0 : u);
            }
            s.no_coor = s.variant ? 0 : hts_idx_get_n_no_coor(idx);
            return true;
        };
        if (f->format == bam) {
            if (sam_hdr_t* h = sam_hdr_read(fp)) {
                if (hts_idx_t* idx = sam_index_load(fp, path.c_str())) {
                    ok = collect(idx, sam_hdr_nref(h), [&](int i) { return sam_hdr_tid2name(h, i); });
                    hts_idx_destroy(idx);
                }
                sam_hdr_destroy(h);
            }
        } else if (f->format == bcf) {
            s.variant = true;
            if (bcf_hdr_t* h = bcf_hdr_read(fp)) {
                if (hts_idx_t* idx = bcf_index_load(path.c_str())) {
                    ok = collect(idx, h->n[BCF_DT_CTG], [&](int i) { return bcf_hdr_id2name(h, i); });
                    hts_idx_destroy(idx);
                }
                bcf_hdr_destroy(h);
            }
        } else if (f->format == vcf && f->compression == bgzf) {
            s.variant = true;
            if (tbx_t* tbx = tbx_index_load(path.c_str())) {
                int n = 0;
                const char** seqs = tbx_seqnames(tbx, &n);
                ok = collect(tbx->idx, n, [&](int i) { return seqs[i]; });
                free(seqs);
                tbx_destroy(tbx);
            }
        }
        if (ok) *out = std::move(s);
        hts_close(fp);
    }
    hts_set_log_level((htsLogLevel)level);
    return ok;
}

// Build the reference-sequence table for --contigs from a genomics file's
// header alone. Returns an error string when the format has no such dictionary
// or when --contigs is combined with a flag that operates on the file's data.
std::string build_contigs(const Config& cfg,
                          std::unique_ptr<TabularSource>* out) {
    if (!cfg.region.empty())
        return "--contigs lists a file's reference sequences; it does not take a "
               "region (-r)";
    if (cfg.pileup || cfg.decode_pileup || !cfg.bam_tags.empty() ||
        !cfg.expand_col.empty())
        return "--contigs cannot be combined with a flag that reads the file's "
               "records (--pileup / --decode-pileup / --tags / --expand)";

    const std::string& path = cfg.path;
    GenomicHeader gh;
    if (auto err = read_genomic_header(path, cfg.pileup_ref, &gh); !err.empty())
        return err;
    const std::vector<std::string>& names   = gh.contig_names;
    const std::vector<int64_t>&     lengths = gh.contig_lengths;
    if (names.empty())
        return "'" + path + "': the header names no reference sequences";

    // With an index that counts records, add them per sequence (matched by
    // name: a tabix index has its own sequence list). A sequence the index
    // has but the header does not name is appended with no length.
    IndexStats ix;
    const bool have_ix = read_index_stats(path, &ix);
    std::vector<std::string> all_names = names;
    std::vector<int64_t>     all_lengths = lengths;
    std::map<std::string, size_t> ix_row;
    if (have_ix) {
        for (size_t i = 0; i < ix.names.size(); ++i) ix_row[ix.names[i]] = i;
        std::set<std::string> in_header(names.begin(), names.end());
        for (const auto& n : ix.names)
            if (!in_header.count(n)) { all_names.push_back(n); all_lengths.push_back(0); }
    }

    arrow::StringBuilder name_b;
    arrow::Int64Builder  len_b, mapped_b, unmapped_b;
    for (size_t i = 0; i < all_names.size(); ++i) {
        auto s1 = name_b.Append(all_names[i]);
        (void)s1;
        if (all_lengths[i] > 0) (void)len_b.Append(all_lengths[i]);
        else                    (void)len_b.AppendNull();
        if (!have_ix) continue;
        auto it = ix_row.find(all_names[i]);
        (void)mapped_b.Append(it == ix_row.end() ? 0 : (int64_t)ix.mapped[it->second]);
        (void)unmapped_b.Append(it == ix_row.end() ? 0 : (int64_t)ix.unmapped[it->second]);
    }
    std::shared_ptr<arrow::Array> name_a, len_a, mapped_a, unmapped_a;
    if (!name_b.Finish(&name_a).ok() || !len_b.Finish(&len_a).ok() ||
        !mapped_b.Finish(&mapped_a).ok() || !unmapped_b.Finish(&unmapped_a).ok())
        return "'" + path + "': failed to build the contig table";
    arrow::FieldVector fields = {arrow::field("name",   arrow::utf8()),
                                 arrow::field("length", arrow::int64())};
    std::vector<std::shared_ptr<arrow::Array>> cols = {name_a, len_a};
    if (have_ix && ix.variant) {
        fields.push_back(arrow::field("records", arrow::int64()));
        cols.push_back(mapped_a);
    } else if (have_ix) {
        fields.push_back(arrow::field("mapped", arrow::int64()));
        fields.push_back(arrow::field("unmapped", arrow::int64()));
        cols.push_back(mapped_a);
        cols.push_back(unmapped_a);
    }
    auto table = arrow::Table::Make(arrow::schema(fields), cols);

    std::string footer;
    for (const auto& [label, value] : genomic_header_fields(gh))
        footer += (footer.empty() ? "" : "  |  ") + label + ": " + value;
    if (have_ix) {
        footer += "  |  Counts: from the index";
        if (!ix.variant)
            footer += "  |  Unmapped without a position: " + std::to_string(ix.no_coor);
    }

    *out = std::make_unique<MemoryTableSource>(table, path, footer);
    return "";
}

// --seq-stats: one pass over a FASTA / FASTQ with htslib's kseq, summarised
// as one row (like `seqkit stats -a`): record count, total / min / mean / max
// length, N50, GC share, and for FASTQ the share of bases at Phred >= 20 and
// >= 30 (Phred+33). Lengths are kept as a histogram, so memory does not grow
// with the read count.
std::string build_seq_stats(const Config& cfg, std::unique_ptr<TabularSource>* out) {
    const std::string& path = cfg.path;
    const std::string det = strip_compression_suffix(path);
    const bool fastq = fends_ci(det, ".fq") || fends_ci(det, ".fastq");
    const bool fasta = fends_ci(det, ".fa") || fends_ci(det, ".fasta") || fends_ci(det, ".fna") ||
                       fends_ci(det, ".faa") || fends_ci(det, ".ffn") || fends_ci(det, ".frn");
    if (!fastq && !fasta)
        return "--seq-stats summarises a FASTA / FASTQ file (.fa .fasta .fna .faa .ffn .frn "
               ".fq .fastq, plus .gz / .zst / .bz2 / .xz)";
    if (!cfg.region.empty() || !cfg.filter_expr.empty())
        return "--seq-stats summarises every record; it does not take -r or --filter";

    FastxIn in;
    if (auto e = open_fastx_input(path, cfg, &in); !e.empty()) return e;
    kseq_t* ks = kseq_init(&in);
    std::map<int64_t, int64_t> len_hist;
    uint64_t res[256] = {0}, qual[256] = {0};
    int64_t n = 0, sum = 0;
    int ret;
    while ((ret = kseq_read(ks)) >= 0) {
        ++n;
        sum += (int64_t)ks->seq.l;
        ++len_hist[(int64_t)ks->seq.l];
        const unsigned char* s = (const unsigned char*)ks->seq.s;
        for (size_t i = 0; i < ks->seq.l; ++i) ++res[s[i]];
        if (fastq && ks->qual.l) {
            const unsigned char* q = (const unsigned char*)ks->qual.s;
            for (size_t i = 0; i < ks->qual.l; ++i) ++qual[q[i]];
        }
    }
    kseq_destroy(ks);
    if (ret < -1)
        return "malformed " + std::string(fastq ? "FASTQ" : "FASTA") +
               " record after " + std::to_string(n) + " records" +
               (ret == -2 ? " (quality string length differs from the sequence)" : "");

    auto both = [&](char c) { return res[(unsigned char)c] + res[(unsigned char)std::tolower(c)]; };
    const uint64_t a = both('A'), c = both('C'), g = both('G'), t = both('T'), u = both('U');
    const uint64_t nuc = a + c + g + t + u + both('N');
    const uint64_t total = (uint64_t)sum;
    // Nucleotides unless more than a tenth of the residues are other letters.
    const bool protein = total > 0 && (double)(total - nuc) > 0.1 * (double)total;
    const char* type = protein ? "Protein" : (u > 0 && t == 0 ? "RNA" : "DNA");

    int64_t n50 = 0, acc = 0;
    for (auto it = len_hist.rbegin(); it != len_hist.rend(); ++it) {
        acc += it->first * it->second;
        if (2 * acc >= sum) { n50 = it->first; break; }
    }
    uint64_t q_all = 0, q20 = 0, q30 = 0;
    for (int ch = 33; ch < 256; ++ch) {
        q_all += qual[ch];
        if (ch >= 33 + 20) q20 += qual[ch];
        if (ch >= 33 + 30) q30 += qual[ch];
    }

    arrow::StringBuilder file_b, fmt_b, type_b;
    arrow::Int64Builder  num_b, sum_b, min_b, max_b, n50_b;
    arrow::DoubleBuilder avg_b, gc_b, q20_b, q30_b;
    (void)file_b.Append(path);
    (void)fmt_b.Append(fastq ? "FASTQ" : "FASTA");
    (void)type_b.Append(type);
    (void)num_b.Append(n);
    (void)sum_b.Append(sum);
    if (n) {
        (void)min_b.Append(len_hist.begin()->first);
        (void)max_b.Append(len_hist.rbegin()->first);
        (void)avg_b.Append((double)sum / (double)n);
        (void)n50_b.Append(n50);
    } else {
        (void)min_b.AppendNull(); (void)max_b.AppendNull();
        (void)avg_b.AppendNull(); (void)n50_b.AppendNull();
    }
    const uint64_t acgtu = a + c + g + t + u;
    if (!protein && acgtu) (void)gc_b.Append(100.0 * (double)(g + c) / (double)acgtu);
    else                   (void)gc_b.AppendNull();
    if (fastq && q_all) {
        (void)q20_b.Append(100.0 * (double)q20 / (double)q_all);
        (void)q30_b.Append(100.0 * (double)q30 / (double)q_all);
    } else {
        (void)q20_b.AppendNull(); (void)q30_b.AppendNull();
    }
    std::vector<std::shared_ptr<arrow::Array>> cols(12);
    arrow::ArrayBuilder* bs[] = {&file_b, &fmt_b, &type_b, &num_b, &sum_b, &min_b, &avg_b,
                                 &max_b, &n50_b, &gc_b, &q20_b, &q30_b};
    for (size_t i = 0; i < 12; ++i)
        if (!bs[i]->Finish(&cols[i]).ok()) return "'" + path + "': failed to build the summary";
    auto schema = arrow::schema({
        arrow::field("file", arrow::utf8()),      arrow::field("format", arrow::utf8()),
        arrow::field("type", arrow::utf8()),      arrow::field("num_seqs", arrow::int64()),
        arrow::field("sum_len", arrow::int64()),  arrow::field("min_len", arrow::int64()),
        arrow::field("avg_len", arrow::float64()), arrow::field("max_len", arrow::int64()),
        arrow::field("n50", arrow::int64()),      arrow::field("gc_pct", arrow::float64()),
        arrow::field("q20_pct", arrow::float64()), arrow::field("q30_pct", arrow::float64())});
    *out = std::make_unique<MemoryTableSource>(arrow::Table::Make(schema, cols), path,
        std::string("Sequence statistics: GC over A/C/G/T/U (N excluded)") +
        (fastq ? "; Q20 / Q30 = share of bases at Phred >= 20 / 30 (Phred+33)" : ""));
    return "";
}
