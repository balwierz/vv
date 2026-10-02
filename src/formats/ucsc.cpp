// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// UCSC bigWig / bigBed and 2bit.

#include "internal.hpp"

// ── bigBed / bigWig source (libBigWig, vendored) ──────────────────────────────
//
// One TabularSource handles both formats. For bigWig, the schema is
// fixed (chrom, start, end, value). For bigBed, the first three columns
// are the loci fields and the rest come from parsing the embedded
// autoSql with parse_autosql() above.
//
// Reads are batched per-chromosome (libBigWig's overlap API works one
// chromosome at a time anyway). With --region, only the requested
// windows are queried.
class BigSource : public TabularSource {
    std::string                              path_;
    std::shared_ptr<arrow::Schema>           schema_;
    bool                                     is_bb_ = false;
    std::vector<AutosqlField>                autosql_;

    mutable bigWigFile_t* fp_ = nullptr;

    // For region-mode, the precomputed plan of (chrom, start, end).
    bool                                     region_mode_ = false;
    std::vector<Region>                      windows_;
    mutable size_t                           cur_window_ = 0;
    mutable size_t                           cur_chrom_  = 0;
    mutable bool                             all_read_   = false;

    // A single window (chrom span) can hold far more than BATCH_SIZE
    // intervals. Retain its libBigWig container across advance() calls and
    // resume at cur_off_ so nothing past the batch boundary is dropped.
    mutable bwOverlappingIntervals_t*        cur_iv_    = nullptr;  // bigWig
    mutable bbOverlappingEntries_t*          cur_bb_    = nullptr;  // bigBed
    mutable uint32_t                         cur_off_   = 0;        // next unread index
    mutable std::string                      cur_label_;           // chrom of the window

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;

    static constexpr int BATCH_SIZE = 32768;

    // For each pending (chrom, start, end) window, pull all overlapping
    // intervals/entries and convert into Arrow rows. Builds one batch
    // and returns; subsequent advance() calls drain the rest.
    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();

        // Choose source of windows to iterate.
        auto chrom_at = [&](size_t i) -> std::string {
            if (region_mode_) return windows_[i].chrom;
            return fp_->cl->chrom[i];
        };
        auto chrom_span = [&](size_t i, uint32_t* s, uint32_t* e) {
            if (region_mode_) {
                int64_t a = windows_[i].start;
                int64_t b = windows_[i].end;
                *s = (a == INT64_MIN) ? 0u : (uint32_t)std::max<int64_t>(0, a);
                *e = (b == INT64_MAX) ? UINT32_MAX : (uint32_t)b;
            } else {
                *s = 0u;
                *e = fp_->cl->len[i];
            }
        };
        size_t n_iter = region_mode_ ? windows_.size() : (size_t)fp_->cl->nKeys;
        size_t& idx = region_mode_ ? cur_window_ : cur_chrom_;

        // Build a list of (chrom, start, end, value, [extras...]) cells.
        arrow::StringBuilder chrom_b;
        arrow::UInt32Builder start_b, end_b;
        arrow::FloatBuilder  value_b;     // bigWig only
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> extra_b;  // bigBed extras
        if (is_bb_) {
            for (auto& f : autosql_) {
                if (f.is_list) {
                    // list<elem>: builders are awkward; fall back to a
                    // string column for v1 (TODO: real list builders).
                    extra_b.emplace_back(new arrow::StringBuilder());
                } else if (f.arrow_type->id() == arrow::Type::INT8)   extra_b.emplace_back(new arrow::Int8Builder());
                else if (f.arrow_type->id() == arrow::Type::UINT8)    extra_b.emplace_back(new arrow::UInt8Builder());
                else if (f.arrow_type->id() == arrow::Type::INT16)    extra_b.emplace_back(new arrow::Int16Builder());
                else if (f.arrow_type->id() == arrow::Type::UINT16)   extra_b.emplace_back(new arrow::UInt16Builder());
                else if (f.arrow_type->id() == arrow::Type::INT32)    extra_b.emplace_back(new arrow::Int32Builder());
                else if (f.arrow_type->id() == arrow::Type::UINT32)   extra_b.emplace_back(new arrow::UInt32Builder());
                else if (f.arrow_type->id() == arrow::Type::INT64)    extra_b.emplace_back(new arrow::Int64Builder());
                else if (f.arrow_type->id() == arrow::Type::FLOAT)    extra_b.emplace_back(new arrow::FloatBuilder());
                else if (f.arrow_type->id() == arrow::Type::DOUBLE)   extra_b.emplace_back(new arrow::DoubleBuilder());
                else                                                   extra_b.emplace_back(new arrow::StringBuilder());
            }
        }

        int count = 0;
        // Helper: append one int-like value from a string token.
        auto append_typed = [&](arrow::ArrayBuilder* b,
                                arrow::Type::type t,
                                const std::string& tok) -> arrow::Status {
            auto bad = [&]() -> arrow::Status { return static_cast<arrow::StringBuilder*>(b)->AppendNull(); };
            try {
                switch (t) {
                    case arrow::Type::INT8:   return static_cast<arrow::Int8Builder*>(b)->Append((int8_t)std::stoi(tok));
                    case arrow::Type::UINT8:  return static_cast<arrow::UInt8Builder*>(b)->Append((uint8_t)std::stoul(tok));
                    case arrow::Type::INT16:  return static_cast<arrow::Int16Builder*>(b)->Append((int16_t)std::stoi(tok));
                    case arrow::Type::UINT16: return static_cast<arrow::UInt16Builder*>(b)->Append((uint16_t)std::stoul(tok));
                    case arrow::Type::INT32:  return static_cast<arrow::Int32Builder*>(b)->Append((int32_t)std::stol(tok));
                    case arrow::Type::UINT32: return static_cast<arrow::UInt32Builder*>(b)->Append((uint32_t)std::stoul(tok));
                    case arrow::Type::INT64:  return static_cast<arrow::Int64Builder*>(b)->Append((int64_t)std::stoll(tok));
                    case arrow::Type::FLOAT:  return static_cast<arrow::FloatBuilder*>(b)->Append(std::stof(tok));
                    case arrow::Type::DOUBLE: return static_cast<arrow::DoubleBuilder*>(b)->Append(std::stod(tok));
                    case arrow::Type::STRING:
                    default:                  return static_cast<arrow::StringBuilder*>(b)->Append(tok);
                }
            } catch (...) { return bad(); }
        };

        while (count < BATCH_SIZE) {
            // Fetch the next window's intervals only once the previous one
            // is fully drained; cur_off_ resumes a window split across
            // batches so nothing past BATCH_SIZE is discarded.
            if (!cur_iv_ && !cur_bb_) {
                if (idx >= n_iter) break;
                cur_label_ = chrom_at(idx);
                uint32_t qs, qe;
                chrom_span(idx, &qs, &qe);
                ++idx;
                cur_off_ = 0;
                // Unknown chrom — libBigWig will return NULL.
                if (is_bb_) {
                    cur_bb_ = bbGetOverlappingEntries(
                        fp_, cur_label_.c_str(), qs, qe, /*withString=*/1);
                } else {
                    cur_iv_ = bwGetOverlappingIntervals(
                        fp_, cur_label_.c_str(), qs, qe);
                }
                if (!cur_iv_ && !cur_bb_) continue;
            }
            if (is_bb_) {
                for (; cur_off_ < cur_bb_->l && count < BATCH_SIZE; ++cur_off_) {
                    ARROW_RETURN_NOT_OK(chrom_b.Append(cur_label_));
                    ARROW_RETURN_NOT_OK(start_b.Append(cur_bb_->start[cur_off_]));
                    ARROW_RETURN_NOT_OK(end_b.Append(cur_bb_->end[cur_off_]));
                    const char* s = cur_bb_->str[cur_off_];
                    // Split s on tabs into N fields.
                    std::vector<std::string> toks;
                    if (s) {
                        const char* p = s;
                        const char* tb = p;
                        for (; *p; ++p) {
                            if (*p == '\t') { toks.emplace_back(tb, p - tb); tb = p + 1; }
                        }
                        toks.emplace_back(tb, p - tb);
                    }
                    for (size_t k = 0; k < autosql_.size(); ++k) {
                        std::string tok = (k < toks.size()) ? toks[k] : std::string();
                        auto& f = autosql_[k];
                        if (f.is_list) {
                            // Store raw "a,b,c" string for v1.
                            ARROW_RETURN_NOT_OK(static_cast<arrow::StringBuilder*>(
                                extra_b[k].get())->Append(tok));
                        } else {
                            ARROW_RETURN_NOT_OK(append_typed(extra_b[k].get(),
                                f.arrow_type->id(), tok));
                        }
                    }
                    ++count;
                }
                if (cur_off_ >= cur_bb_->l) {
                    bbDestroyOverlappingEntries(cur_bb_);
                    cur_bb_ = nullptr;
                }
            } else {
                for (; cur_off_ < cur_iv_->l && count < BATCH_SIZE; ++cur_off_) {
                    ARROW_RETURN_NOT_OK(chrom_b.Append(cur_label_));
                    ARROW_RETURN_NOT_OK(start_b.Append(cur_iv_->start[cur_off_]));
                    ARROW_RETURN_NOT_OK(end_b.Append(cur_iv_->end[cur_off_]));
                    ARROW_RETURN_NOT_OK(value_b.Append(cur_iv_->value[cur_off_]));
                    ++count;
                }
                if (cur_off_ >= cur_iv_->l) {
                    bwDestroyOverlappingIntervals(cur_iv_);
                    cur_iv_ = nullptr;
                }
            }
        }
        if (idx >= n_iter && !cur_iv_ && !cur_bb_) all_read_ = true;
        if (count == 0) return arrow::Status::OK();

        std::vector<std::shared_ptr<arrow::Array>> a;
        std::shared_ptr<arrow::Array> tmp;
        ARROW_RETURN_NOT_OK(chrom_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(start_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(end_b.Finish(&tmp));   a.push_back(tmp);
        if (is_bb_) {
            for (auto& b : extra_b) {
                ARROW_RETURN_NOT_OK(b->Finish(&tmp));
                a.push_back(tmp);
            }
        } else {
            ARROW_RETURN_NOT_OK(value_b.Finish(&tmp));
            a.push_back(tmp);
        }
        auto batch = arrow::RecordBatch::Make(schema_, count, a);
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BigSource() {
        if (cur_iv_) { bwDestroyOverlappingIntervals(cur_iv_); cur_iv_ = nullptr; }
        if (cur_bb_) { bbDestroyOverlappingEntries(cur_bb_);   cur_bb_ = nullptr; }
        if (fp_) { bwClose(fp_); fp_ = nullptr; }
        bwCleanup();
    }

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BigSource>* out) {
        auto self = std::make_unique<BigSource>();
        self->path_ = path;
        if (bwInit(1 << 17) != 0)
            return "libBigWig init failed";
        // libBigWig auto-detects format from the magic; pick the right open.
        bool maybe_bw = bwIsBigWig(path.c_str(), nullptr) != 0;
        if (maybe_bw) {
            self->fp_ = bwOpen(path.c_str(), nullptr, "r");
            self->is_bb_ = false;
        } else {
            self->fp_ = bbOpen(path.c_str(), nullptr);
            self->is_bb_ = true;
        }
        if (!self->fp_)
            return "Cannot open '" + path + "' as bigWig/bigBed";

        // Schema.
        arrow::FieldVector fields = {
            arrow::field("chrom", arrow::utf8()),
            arrow::field("start", arrow::uint32()),
            arrow::field("end",   arrow::uint32()),
        };
        if (self->is_bb_) {
            // Read the embedded autoSql blob from the file (libBigWig
            // does not expose it; seek + read manually). The first 3
            // autoSql fields are always chrom/start/end — we skip them.
            std::string sql;
            if (self->fp_->hdr && self->fp_->hdr->sqlOffset) {
                urlSeek(self->fp_->URL, self->fp_->hdr->sqlOffset);
                char buf[4096];
                while (true) {
                    size_t n = urlRead(self->fp_->URL, buf, sizeof(buf));
                    if (n == 0) break;
                    bool done = false;
                    for (size_t i = 0; i < n; ++i) {
                        if (buf[i] == 0) {
                            sql.append(buf, i);
                            done = true;
                            break;
                        }
                    }
                    if (done) break;
                    sql.append(buf, n);
                    if (n < sizeof(buf)) break;
                }
            }
            auto all_fields = parse_autosql(sql);
            // Drop the first 3 autoSql fields (chrom/chromStart/chromEnd)
            // — we already added them above.
            int skip = std::min<int>((int)all_fields.size(), 3);
            for (int i = skip; i < (int)all_fields.size(); ++i) {
                fields.push_back(arrow::field(
                    all_fields[i].name, all_fields[i].arrow_type));
                self->autosql_.push_back(all_fields[i]);
            }
        } else {
            fields.push_back(arrow::field("value", arrow::float32()));
        }
        self->schema_ = arrow::schema(fields);

        // Region plan.
        if (!cfg.region.empty()) {
            self->windows_ = parse_region_list(cfg.region, cfg.coords_one_based);
            self->region_mode_ = true;
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
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return std::string("Format: ") + (is_bb_ ? "bigBed" : "bigWig") +
               "  |  Chromosomes: " + std::to_string(fp_ ? fp_->cl->nKeys : 0);
    }
    int min_col_width(int col_idx) const override {
        if (col_idx == 0) return 6;   // chrom
        if (col_idx == 1 || col_idx == 2) return 9;  // start / end
        return 4;
    }
};

std::string open_big_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<BigSource> s;
    std::string e = BigSource::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── 2bit (UCSC) source ────────────────────────────────────────────────────────
//
// 2bit is the UCSC binary container for genome-scale DNA sequences
// (typically a hg38.2bit or mm10.2bit reference). Each base is packed
// into 2 bits, with side tables for N-runs and soft-masked regions.
// Spec: https://genome.ucsc.edu/FAQ/FAQformat.html#format7
//
// vv exposes the file's sequence *index*, not the bases themselves —
// chromosomes are massive (the human reference decodes to ~3 GB of
// strings) and the typical use of `vv hg38.2bit` is "what's in this
// file?" The columns are name / length_bp / n_blocks / mask_blocks
// (the last two are counts of unknown-base and lowercase-soft-mask
// runs, useful for spotting unmasked or contig-poor assemblies).
class TwoBitSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    std::shared_ptr<arrow::RecordBatch>    batch_;
    int64_t                                seq_count_ = 0;

public:
    static std::string open(const std::string& path, const Config& /*cfg*/,
                             std::unique_ptr<TwoBitSource>* out) {
        auto self = std::make_unique<TwoBitSource>();
        self->path_ = path;

        FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp)
            return "Cannot open '" + path + "': " + std::strerror(errno);

        // Header: signature, version, seqCount, reserved (4 × 4 bytes).
        uint8_t hdr[16];
        if (std::fread(hdr, 1, 16, fp) != 16) {
            std::fclose(fp);
            return "Cannot read 2bit header from '" + path + "'";
        }
        auto le32 = [](const uint8_t* p) -> uint32_t {
            return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                 | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        };
        auto be32 = [](const uint8_t* p) -> uint32_t {
            return (uint32_t)p[3] | ((uint32_t)p[2] << 8)
                 | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24);
        };
        uint32_t sig_le = le32(hdr);
        bool be = false;
        if (sig_le != 0x1A412743u) {
            uint32_t sig_be = be32(hdr);
            if (sig_be != 0x1A412743u) {
                std::fclose(fp);
                return "Not a 2bit file (bad signature) at '" + path + "'";
            }
            be = true;
        }
        auto r32 = [&](const uint8_t* p) { return be ? be32(p) : le32(p); };
        uint32_t version    = r32(hdr + 4);
        uint32_t seq_count  = r32(hdr + 8);
        if (version != 0) {
            std::fclose(fp);
            return "2bit version " + std::to_string(version) +
                   " (long-offset variant) is not supported by vv";
        }

        // Pass 1: read the sequence index (name + 32-bit offset per seq).
        struct Idx { std::string name; uint32_t offset; };
        std::vector<Idx> idx;
        // seq_count is an attacker-controllable uint32 (up to ~4.3e9). Each
        // index entry needs at least 5 bytes on disk (1-byte name length + ≥0
        // name + 4-byte offset), so reject a count the file cannot possibly
        // hold rather than let reserve() request ~170 GB and abort the process.
        long fsize = (std::fseek(fp, 0, SEEK_END) == 0) ? std::ftell(fp) : -1;
        std::fseek(fp, 16, SEEK_SET);   // rewind to just past the header
        uint64_t max_seqs = (fsize >= 16) ? ((uint64_t)fsize - 16) / 5 : 0;
        if (fsize >= 16 && seq_count > max_seqs) {
            std::fclose(fp);
            return "2bit sequence count " + std::to_string(seq_count) +
                   " exceeds the size of '" + path + "'";
        }
        idx.reserve(std::min<uint32_t>(seq_count, 1u << 20));
        for (uint32_t i = 0; i < seq_count; ++i) {
            uint8_t name_size;
            if (std::fread(&name_size, 1, 1, fp) != 1) {
                std::fclose(fp);
                return "Truncated 2bit sequence index at '" + path + "'";
            }
            std::string name(name_size, '\0');
            if (std::fread(name.data(), 1, name_size, fp) != name_size) {
                std::fclose(fp);
                return "Truncated 2bit sequence name at '" + path + "'";
            }
            uint8_t off_buf[4];
            if (std::fread(off_buf, 1, 4, fp) != 4) {
                std::fclose(fp);
                return "Truncated 2bit offset at '" + path + "'";
            }
            idx.push_back({std::move(name), r32(off_buf)});
        }

        // Pass 2: jump to each seqRecord header and read dna_size,
        // n_block_count, mask_block_count. The packed DNA payload after
        // the mask block tables is skipped entirely.
        arrow::StringBuilder name_b;
        arrow::UInt32Builder length_b, nb_b, mb_b;
        ARROW_UNUSED(name_b);  // suppress unused-warning until we Finish()
        for (auto& s : idx) {
            if (std::fseek(fp, (long)s.offset, SEEK_SET) != 0) {
                std::fclose(fp);
                return "Cannot seek to 2bit seqRecord at offset " +
                       std::to_string(s.offset);
            }
            uint8_t buf[4];
            auto read32 = [&](uint32_t* v) -> bool {
                if (std::fread(buf, 1, 4, fp) != 4) return false;
                *v = r32(buf); return true;
            };
            uint32_t dna_size, n_block_count, mask_block_count;
            if (!read32(&dna_size) || !read32(&n_block_count)) {
                std::fclose(fp);
                return "Truncated 2bit seqRecord (header)";
            }
            // Skip the N-block start/size arrays. Compute the byte count in
            // 64-bit: n_block_count*8 in uint32 would overflow for a crafted
            // count > 0x1FFFFFFF and seek to the wrong offset.
            if (std::fseek(fp, (long)((uint64_t)n_block_count * 8), SEEK_CUR) != 0) {
                std::fclose(fp);
                return "Cannot skip 2bit N-block table";
            }
            if (!read32(&mask_block_count)) {
                std::fclose(fp);
                return "Truncated 2bit seqRecord (mask count)";
            }
            // We have everything we want; no need to scan the rest.

            auto st = name_b.Append(s.name);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
            st = length_b.Append(dna_size);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
            st = nb_b.Append(n_block_count);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
            st = mb_b.Append(mask_block_count);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
        }
        std::fclose(fp);

        std::shared_ptr<arrow::Array> a_name, a_len, a_nb, a_mb;
        auto st = name_b.Finish(&a_name);
        if (!st.ok()) return st.ToString();
        st = length_b.Finish(&a_len);
        if (!st.ok()) return st.ToString();
        st = nb_b.Finish(&a_nb);
        if (!st.ok()) return st.ToString();
        st = mb_b.Finish(&a_mb);
        if (!st.ok()) return st.ToString();

        self->schema_ = arrow::schema({
            arrow::field("name",        arrow::utf8()),
            arrow::field("length",      arrow::uint32()),
            arrow::field("n_blocks",    arrow::uint32()),
            arrow::field("mask_blocks", arrow::uint32()),
        });
        self->batch_ = arrow::RecordBatch::Make(self->schema_, idx.size(),
            {a_name, a_len, a_nb, a_mb});
        self->seq_count_ = (int64_t)idx.size();
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return seq_count_; }
    int     num_chunks() const override { return 1; }
    ChunkMeta chunk_meta(int /*i*/) const override { return {0, seq_count_}; }
    void ensure(int /*i*/) override {}
    arrow::Status read_chunk(int /*i*/, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        *out = batch_slice_to_table(*batch_, col_indices, schema_);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: 2bit  |  Sequences: " + std::to_string(seq_count_);
    }
    int min_col_width(int col_idx) const override {
        switch (col_idx) {
            case 0: return 8;   // name
            case 1: return 12;  // length
            default: return 6;
        }
    }
};

std::string open_twobit_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<TwoBitSource> s;
    std::string e = TwoBitSource::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
