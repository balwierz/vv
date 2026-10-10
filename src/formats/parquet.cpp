// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Apache Parquet, coordinate-column detection and --stats.

#include "internal.hpp"

// ── Generic Parquet coordinate-column detection ──────────────────────────────
//
// For region queries on plain Parquet (no LociSSD manifest), discover which
// columns hold chromosome / start / end. Either picked by the user via
// --region-cols Chr,Start,End, or auto-detected from a small priority list
// of common names (Chromosome / Chrom / Chr / POS / chromStart / …). Returns
// indices = -1 when a column can't be resolved.
struct GenomicCoordCols {
    int chrom = -1, start = -1, end = -1;
    std::string chrom_name, start_name, end_name;
};

static GenomicCoordCols detect_coord_columns(const arrow::Schema& schema,
                                             const std::string& override_cols) {
    GenomicCoordCols r;
    auto find_idx = [&](const std::string& name) -> int {
        for (int i = 0; i < schema.num_fields(); ++i)
            if (schema.field(i)->name() == name) return i;
        return -1;
    };

    if (!override_cols.empty()) {
        std::vector<std::string> parts;
        std::string buf;
        for (char c : override_cols) {
            if (c == ',') { parts.push_back(std::move(buf)); buf.clear(); }
            else buf += c;
        }
        if (!buf.empty()) parts.push_back(std::move(buf));
        if (parts.size() != 3) return r;
        r.chrom_name = parts[0]; r.start_name = parts[1]; r.end_name = parts[2];
        r.chrom = find_idx(r.chrom_name);
        r.start = find_idx(r.start_name);
        r.end   = find_idx(r.end_name);
        return r;
    }

    static const char* CHROM_NAMES[] = {
        "Chromosome", "chromosome", "Chrom", "chrom", "Chr", "chr",
        "CHROM", "#CHROM", "seqname", "seqid", "contig"
    };
    static const char* START_NAMES[] = {
        "Start", "start", "chromStart", "POS", "pos", "Position",
        "position", "txStart", "begin", "Begin"
    };
    static const char* END_NAMES[] = {
        "End", "end", "chromEnd", "Stop", "stop", "chromStop", "txEnd"
    };
    auto find_by_priority = [&](const char* const* names, size_t n) -> int {
        for (size_t k = 0; k < n; ++k) { int i = find_idx(names[k]); if (i >= 0) return i; }
        return -1;
    };
    r.chrom = find_by_priority(CHROM_NAMES, sizeof(CHROM_NAMES)/sizeof(*CHROM_NAMES));
    r.start = find_by_priority(START_NAMES, sizeof(START_NAMES)/sizeof(*START_NAMES));
    r.end   = find_by_priority(END_NAMES,   sizeof(END_NAMES)/sizeof(*END_NAMES));
    if (r.chrom >= 0) r.chrom_name = schema.field(r.chrom)->name();
    if (r.start >= 0) r.start_name = schema.field(r.start)->name();
    if (r.end   >= 0) r.end_name   = schema.field(r.end)->name();
    return r;
}




// Wraps an InputStream, truncating each line to at most max_fields tab-separated fields.
// Used for SAM (variable optional alignment tags) and GFF3 (occasional extra columns).

// PAF with --tags: each line keeps its 12 mandatory fields and gains one
// field per requested tag — the value of the optional `TG:type:value` field
// named TG, or empty when the line has none (read as null).

// Wraps an InputStream so every line's fields are separated by exactly one
// space: runs of spaces / tabs collapse to one, leading and trailing whitespace
// is dropped, and blank lines are skipped. MatrixMarket bodies are "separated by
// whitespace", which Arrow's single-character CSV delimiter cannot express.

// Turn a top-level JSON array `[ {…}, {…} ]` into the newline-delimited records
// that Arrow's JSON reader wants: it drops the enclosing `[` / `]` and replaces
// the commas between elements with newlines, leaving everything else byte for
// byte. NDJSON, a bare object, and pretty-printed or concatenated objects have
// no enclosing array, so they stream straight through untouched — the reader
// already tolerates newlines inside a record. Structural characters inside
// strings, and any nesting depth, are tracked so a `,` / `[` / `]` inside a
// value is never mistaken for the array framing.

// ── Parquet source ────────────────────────────────────────────────────────────

class ParquetSource : public TabularSource {
    std::unique_ptr<parquet::arrow::FileReader> reader_;
    std::shared_ptr<parquet::FileMetaData>      meta_;
    std::shared_ptr<arrow::Schema>              schema_;
    std::string                                  path_;
    std::vector<int64_t>                         chunk_start_;
    bool                                         is_lociss_ = false;
    std::string                                  lociss_assembly_;   // e.g. "hg38"
    std::string                                  lociss_species_;    // e.g. "Homo sapiens"

    // ── LociSSD region-query state ───────────────────────────────────────────
    // Populated only when cfg.region is set on a LociSSD file. A "slice" is a
    // span of rows inside one row group that is known (after row-group
    // statistics pruning) to potentially contain matches; per-row predicate
    // filtering happens inside read_chunk and produces the actual visible rows.
    struct RegionSlice {
        int      row_group;
        int64_t  off_in_rg;    // first row of the slice inside the row group
        int64_t  len;          // number of rows to consider before filtering
        Region   window;       // the window the slice belongs to
    };
    bool                          region_mode_ = false;
    std::vector<RegionSlice>      slices_;
    std::vector<int64_t>          slice_first_row_;  // cumulative virtual row offsets
    std::vector<int64_t>          slice_count_;      // exact post-filter rows per slice
    int64_t                       region_total_rows_ = 0;
    // Column indices we need for filtering (looked up once at open()).
    int                           j_chrom_=-1, j_start_=-1, j_end_=-1, j_mes_=-1;
    // Decoded-row-group cache for region mode. Two windows overlapping the same
    // row group, and the count pass in open() ahead of the output pass, would
    // otherwise each re-decode it. Keyed by (row_group, projected columns), it
    // holds the full row-group decode before the per-slice Slice + BED overlap
    // filter. A small LRU: a multi-window query over one row group decodes it
    // once, without pinning every touched row group in memory.
    struct RgCacheEntry {
        int                           row_group;
        std::vector<int>              need;    // projected field indices, sorted
        std::shared_ptr<arrow::Table> raw;
    };
    mutable std::vector<RgCacheEntry> rg_cache_;
    static constexpr size_t           kRgCacheMax = 4;

    static std::string fmt_size(int64_t sz) {
        char buf[32];
        if      (sz < 1024)             std::snprintf(buf,sizeof(buf),"%lld B",(long long)sz);
        else if (sz < 1024*1024)        std::snprintf(buf,sizeof(buf),"%.1f KiB",sz/1024.0);
        else if (sz < 1024LL*1024*1024) std::snprintf(buf,sizeof(buf),"%.2f MiB",sz/(1024.0*1024));
        else                            std::snprintf(buf,sizeof(buf),"%.2f GiB",sz/(1024.0*1024*1024));
        return buf;
    }

public:
    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<ParquetSource>* out) {
        auto self = std::make_unique<ParquetSource>();
        self->path_ = path;

        auto maybe_file = arrow::io::ReadableFile::Open(path);
        if (!maybe_file.ok())
            return "Cannot open '" + path + "': " + maybe_file.status().ToString();

        parquet::ArrowReaderProperties props = parquet::default_arrow_reader_properties();
        props.set_pre_buffer(true);
        props.set_use_threads(true);   // parallel column decode within a row group
        if (cfg.head_rows > 0) props.set_batch_size(cfg.head_rows);

        // Larger buffered-stream window helps cold reads from spinning disks /
        // network FS by amortising small column-chunk fetches.
        parquet::ReaderProperties rdr_props(arrow::default_memory_pool());
        rdr_props.enable_buffered_stream();
        rdr_props.set_buffer_size(4 << 20);  // 4 MiB

        parquet::arrow::FileReaderBuilder builder;
        auto st = builder.Open(maybe_file.ValueOrDie(), rdr_props);
        if (!st.ok()) return "Not a valid Parquet file: " + st.ToString();
        builder.memory_pool(arrow::default_memory_pool());
        builder.properties(props);
        st = builder.Build(&self->reader_);
        if (!st.ok()) return "Error opening Parquet: " + st.ToString();

        self->meta_ = self->reader_->parquet_reader()->metadata();
        st = self->reader_->GetSchema(&self->schema_);
        if (!st.ok()) return "Error reading schema: " + st.ToString();

        // LociSSD detection: file-level KV metadata key `lociSSD_manifest`.
        // The derived `MaxEndSoFar` column is then hidden from display.
        std::string lociss_manifest_json;
        if (auto kv = self->meta_->key_value_metadata()) {
            if (kv->Contains("lociSSD_manifest")) {
                self->is_lociss_ = true;
                auto v = kv->Get("lociSSD_manifest");
                if (v.ok()) lociss_manifest_json = *v;
            }
        }
        if (self->is_lociss_ && !lociss_manifest_json.empty()) {
            // Genome assembly + species for the header banner (see top_banner()).
            lociss_manifest_value(lociss_manifest_json, "assembly",
                                  &self->lociss_assembly_);
            if (!lociss_manifest_value(lociss_manifest_json, "species",
                                       &self->lociss_species_) &&
                !self->lociss_assembly_.empty())
                self->lociss_species_ = assembly_to_species(self->lociss_assembly_);
        }

        int64_t acc = 0;
        for (int i = 0; i < self->meta_->num_row_groups(); ++i) {
            self->chunk_start_.push_back(acc);
            acc += self->meta_->RowGroup(i)->num_rows();
        }

        // ── Region pruning (LociSSD-aware, otherwise generic) ────────────────
        if (!cfg.region.empty()) {
            // Parquet column statistics are indexed by *leaf* column, not Arrow
            // field: a nested column (e.g. a list) before chrom/start/end
            // shifts the two apart, so a raw field index would read the wrong
            // column's min/max and mis-prune row groups. Map field → leaf.
            auto field_to_leaf = [&](int field_idx) -> int {
                auto lv = self->arrow_to_leaf_indices({field_idx});
                return lv.empty() ? -1 : lv[0];
            };
            // Read a row group's int-column min/max from Parquet statistics.
            auto col_stats_minmax_int = [&](int rg, int field_idx,
                                            int64_t* lo, int64_t* hi) -> bool {
                int leaf = field_to_leaf(field_idx);
                auto md = self->meta_->RowGroup(rg);
                if (leaf < 0 || leaf >= md->num_columns()) return false;
                auto cc = md->ColumnChunk(leaf);
                if (!cc->is_stats_set()) return false;
                auto stats = cc->statistics();
                if (!stats || !stats->HasMinMax()) return false;
                if (auto s64 = std::dynamic_pointer_cast<parquet::Int64Statistics>(stats)) {
                    *lo = s64->min(); *hi = s64->max(); return true;
                }
                if (auto s32 = std::dynamic_pointer_cast<parquet::Int32Statistics>(stats)) {
                    *lo = s32->min(); *hi = s32->max(); return true;
                }
                return false;
            };
            // Read a row group's string-column min/max from Parquet statistics.
            // Used to skip row groups whose chrom range doesn't contain a
            // queried chromosome. Dictionary-encoded strings are still stored
            // physically as ByteArray statistics.
            auto col_stats_minmax_str = [&](int rg, int field_idx,
                                            std::string* lo, std::string* hi) -> bool {
                int leaf = field_to_leaf(field_idx);
                auto md = self->meta_->RowGroup(rg);
                if (leaf < 0 || leaf >= md->num_columns()) return false;
                auto cc = md->ColumnChunk(leaf);
                if (!cc->is_stats_set()) return false;
                auto stats = cc->statistics();
                if (!stats || !stats->HasMinMax()) return false;
                if (auto sb = std::dynamic_pointer_cast<parquet::ByteArrayStatistics>(stats)) {
                    auto mn = sb->min(); auto mx = sb->max();
                    lo->assign(reinterpret_cast<const char*>(mn.ptr), mn.len);
                    hi->assign(reinterpret_cast<const char*>(mx.ptr), mx.len);
                    return true;
                }
                return false;
            };
            auto windows = parse_region_list(cfg.region, cfg.coords_one_based);

            // Detect chrom/start/end columns. The user may override via
            // --region-cols (used to disambiguate non-standard names).
            GenomicCoordCols cc = detect_coord_columns(*self->schema_, cfg.region_cols);
            if (cc.chrom < 0 || cc.start < 0 || cc.end < 0) {
                if (!cfg.region_cols.empty())
                    return "region: --region-cols column(s) not found in schema";
                if (self->is_lociss_)
                    return "LociSSD file missing required columns (Chromosome / Start / End)";
                return "region: could not auto-detect chrom/start/end columns; "
                       "use --region-cols Chr,Start,End to specify them";
            }
            self->j_chrom_ = cc.chrom;
            self->j_start_ = cc.start;
            self->j_end_   = cc.end;
            // Optional: MaxEndSoFar (LociSSD only) for max-end row-group pruning.
            for (int i = 0; i < self->schema_->num_fields(); ++i)
                if (self->schema_->field(i)->name() == "MaxEndSoFar") {
                    self->j_mes_ = i; break;
                }

            int64_t virt = 0;
            if (self->is_lociss_) {
                // LociSSD path: use the manifest to locate each chromosome's
                // row range, then prune row groups by Start.min / MaxEndSoFar.max.
                std::vector<LocissChrom> chromosomes;
                if (!parse_lociss_chromosomes(lociss_manifest_json, &chromosomes))
                    return "Cannot parse LociSSD manifest";
                auto find_chrom = [&](const std::string& name) -> const LocissChrom* {
                    for (auto& c : chromosomes) if (c.name == name) return &c;
                    return nullptr;
                };
                auto rg_range_for_rows = [&](int64_t row_a, int64_t row_b,
                                             int* rg_first, int* rg_last) {
                    int n_rg = self->meta_->num_row_groups();
                    *rg_first = n_rg; *rg_last = -1;
                    for (int g = 0; g < n_rg; ++g) {
                        int64_t a = self->chunk_start_[g];
                        int64_t b = a + self->meta_->RowGroup(g)->num_rows();
                        if (b <= row_a || a >= row_b) continue;
                        if (g < *rg_first) *rg_first = g;
                        if (g > *rg_last)  *rg_last  = g;
                    }
                };
                for (const auto& w : windows) {
                    const LocissChrom* c = find_chrom(w.chrom);
                    if (!c) continue;
                    int64_t row_a = c->row_offset;
                    int64_t row_b = c->row_offset + c->rows;
                    int rg_first, rg_last;
                    rg_range_for_rows(row_a, row_b, &rg_first, &rg_last);
                    if (rg_last < rg_first) continue;
                    int64_t qs = w.start, qe = w.end;
                    for (int g = rg_first; g <= rg_last; ++g) {
                        int64_t lo = INT64_MIN, hi = INT64_MAX;
                        if (qe != INT64_MAX &&
                            col_stats_minmax_int(g, self->j_start_, &lo, &hi) &&
                            lo >= qe) continue;
                        if (qs != INT64_MIN && self->j_mes_ >= 0 &&
                            col_stats_minmax_int(g, self->j_mes_, &lo, &hi) &&
                            hi <= qs) continue;
                        int64_t rg_a = self->chunk_start_[g];
                        int64_t rg_b = rg_a + self->meta_->RowGroup(g)->num_rows();
                        int64_t slice_a = std::max(rg_a, row_a);
                        int64_t slice_b = std::min(rg_b, row_b);
                        if (slice_b <= slice_a) continue;
                        RegionSlice s;
                        s.row_group = g;
                        s.off_in_rg = slice_a - rg_a;
                        s.len       = slice_b - slice_a;
                        s.window    = w;
                        self->slice_first_row_.push_back(virt);
                        virt += s.len;
                        self->slices_.push_back(std::move(s));
                    }
                }
            } else {
                // Generic Parquet: no manifest. Enumerate every row group and
                // prune by per-group statistics. Reading the whole row group
                // and applying the per-row predicate inside read_chunk is
                // always correct — pruning is best-effort.
                int n_rg = self->meta_->num_row_groups();
                for (const auto& w : windows) {
                    for (int g = 0; g < n_rg; ++g) {
                        // chrom range overlap test
                        std::string clo, chi;
                        if (col_stats_minmax_str(g, self->j_chrom_, &clo, &chi)) {
                            if (clo > w.chrom || chi < w.chrom) continue;
                        }
                        // Start.min vs window end
                        int64_t lo = INT64_MIN, hi = INT64_MAX;
                        if (w.end != INT64_MAX &&
                            col_stats_minmax_int(g, self->j_start_, &lo, &hi) &&
                            lo >= w.end) continue;
                        // End.max vs window start (best-effort: only useful if
                        // intervals are short relative to the row group; a
                        // sorted-by-Start file is most likely to benefit).
                        if (w.start != INT64_MIN &&
                            col_stats_minmax_int(g, self->j_end_, &lo, &hi) &&
                            hi <= w.start) continue;

                        RegionSlice s;
                        s.row_group = g;
                        s.off_in_rg = 0;
                        s.len       = self->meta_->RowGroup(g)->num_rows();
                        s.window    = w;
                        self->slice_first_row_.push_back(virt);
                        virt += s.len;
                        self->slices_.push_back(std::move(s));
                    }
                }
            }
            self->region_total_rows_ = virt;
            self->region_mode_       = true;

            // The slice lengths above are only row-group/manifest-pruned bounds;
            // read_chunk further applies the per-row overlap predicate, so they
            // over-report the visible rows — badly for plain Parquet, where a
            // slice spans a whole row group. Run that same filter once per slice
            // (single-column projection) to record the exact counts, so
            // total_rows() / chunk_meta() agree with read_chunk and the
            // TUI/table view shows no phantom trailing rows.
            self->slice_count_.assign(self->slices_.size(), 0);
            int64_t exact = 0;
            for (size_t i = 0; i < self->slices_.size(); ++i) {
                self->slice_first_row_[i] = exact;
                std::shared_ptr<arrow::Table> t;
                if (self->read_chunk((int)i, {self->j_chrom_}, &t).ok() && t)
                    self->slice_count_[i] = t->num_rows();
                exact += self->slice_count_[i];
            }
            self->region_total_rows_ = exact;
        }

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    bool region_applied() const override { return region_mode_; }
    int64_t total_rows() const override {
        // Region mode: the exact post-filter total (computed at open by running
        // the overlap predicate once per slice), so it matches the rows
        // read_chunk actually yields.
        return region_mode_ ? region_total_rows_ : meta_->num_rows();
    }
    int     num_chunks() const override {
        return region_mode_ ? (int)slices_.size() : meta_->num_row_groups();
    }
    ChunkMeta chunk_meta(int i) const override {
        if (region_mode_) return {slice_first_row_[i], slice_count_[i]};
        return {chunk_start_[i], meta_->RowGroup(i)->num_rows()};
    }
    // ReadRowGroups / GetRecordBatchReader take Parquet *leaf* column indices,
    // not Arrow top-level field indices. For nested types (struct/list/map)
    // a single Arrow field may span multiple leaves — passing the wrong index
    // count yields a partial table and crashes later. Walk the manifest to
    // expand Arrow field indices to the full set of leaf column indices.
    std::vector<int> arrow_to_leaf_indices(const std::vector<int>& arrow_cols) const {
        const auto& manifest = reader_->manifest();
        std::vector<int> leaves;
        std::function<void(const parquet::arrow::SchemaField&)> walk =
            [&](const parquet::arrow::SchemaField& sf) {
                if (sf.is_leaf()) { leaves.push_back(sf.column_index); return; }
                for (const auto& ch : sf.children) walk(ch);
            };
        for (int idx : arrow_cols) {
            if (idx >= 0 && idx < (int)manifest.schema_fields.size())
                walk(manifest.schema_fields[idx]);
        }
        return leaves;
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols,
                              std::shared_ptr<arrow::Table>* out) override {
        if (!region_mode_) {
            // The Result-returning ReadRowGroups overload landed in Arrow 24.0.0
            // (when the Status one was deprecated); the static build still pins
            // Arrow 23.0.1, so keep the old call there.
#if ARROW_VERSION_MAJOR >= 24
            ARROW_ASSIGN_OR_RAISE(
                *out, reader_->ReadRowGroups({i}, arrow_to_leaf_indices(cols)));
            return arrow::Status::OK();
#else
            return reader_->ReadRowGroups({i}, arrow_to_leaf_indices(cols), out);
#endif
        }

        // Region mode: read the full row group of the slice, slice it to the
        // pruned row range, then apply the BED-style overlap predicate
        //   chrom == w.chrom AND start < w.end AND end > w.start
        // per row. Works for both LociSSD (row range bounded by the
        // manifest) and plain Parquet (the slice spans the whole row group
        // and chrom-mismatched rows are rejected by the predicate).
        // The caller may have asked for only a projection of columns; we
        // always read chrom/start/end for the predicate, then return only
        // the projected columns.
        const RegionSlice& s = slices_[i];

        // Build the union of (requested cols ∪ {Chromosome, Start, End}).
        std::set<int> need_set(cols.begin(), cols.end());
        need_set.insert(j_chrom_);
        need_set.insert(j_start_);
        need_set.insert(j_end_);
        std::vector<int> need(need_set.begin(), need_set.end());

        std::shared_ptr<arrow::Table> raw;
        // Reuse a cached decode when a prior slice needed the same
        // (row_group, columns); move the hit to the front (LRU).
        for (size_t k = 0; k < rg_cache_.size(); ++k) {
            if (rg_cache_[k].row_group == s.row_group && rg_cache_[k].need == need) {
                raw = rg_cache_[k].raw;
                if (k != 0)
                    std::rotate(rg_cache_.begin(), rg_cache_.begin() + k,
                                rg_cache_.begin() + k + 1);
                break;
            }
        }
        if (!raw) {
#if ARROW_VERSION_MAJOR >= 24
            ARROW_ASSIGN_OR_RAISE(raw, reader_->ReadRowGroups(
                {s.row_group}, arrow_to_leaf_indices(need)));
#else
            ARROW_RETURN_NOT_OK(reader_->ReadRowGroups(
                {s.row_group}, arrow_to_leaf_indices(need), &raw));
#endif
            rg_cache_.insert(rg_cache_.begin(),
                             RgCacheEntry{s.row_group, need, raw});
            if (rg_cache_.size() > kRgCacheMax) rg_cache_.pop_back();
        }
        // Slice to the chromosome's portion of the row group. (The cached table
        // is the full row group; Slice returns a view and never mutates it.)
        raw = raw->Slice(s.off_in_rg, s.len);

        // Locate the columns *within the projected table*.
        auto col_in_raw = [&](int field_idx) -> int {
            for (size_t k = 0; k < need.size(); ++k)
                if (need[k] == field_idx) return (int)k;
            return -1;
        };
        int p_chrom = col_in_raw(j_chrom_);
        int p_start = col_in_raw(j_start_);
        int p_end   = col_in_raw(j_end_);
        if (p_chrom < 0 || p_start < 0 || p_end < 0)
            return arrow::Status::Invalid("region: required columns missing");

        // BED-style half-open overlap predicate
        //   chrom == w.chrom AND start < w.end AND end > w.start
        // applied by walking rows and collecting contiguous runs of matches.
        // Avoids depending on Arrow compute kernels (pruned by --gc-sections
        // in the static build). Built by slicing + concatenating the
        // original table — works for every Arrow type the data may carry.
        const int64_t n_rows = raw->num_rows();
        auto chrom_arr = raw->column(p_chrom)->chunk(0);
        auto start_arr = raw->column(p_start)->chunk(0);
        auto end_arr   = raw->column(p_end)->chunk(0);
        // chunk(0) is safe here: ReadRowGroups returns a single chunk per
        // column, and Slice() shares the chunking. (`raw->Slice` may break
        // multi-chunk inputs but ReadRowGroups produces single-chunk
        // tables.) Defend against the unexpected:
        auto cell_chrom = [&](int64_t r) -> std::string {
            if (auto a = std::dynamic_pointer_cast<arrow::StringArray>(chrom_arr))
                return a->GetString(r);
            if (auto a = std::dynamic_pointer_cast<arrow::LargeStringArray>(chrom_arr))
                return a->GetString(r);
            // Dictionary-encoded strings: common when a Parquet writer
            // emits the chrom column with dict-encoding turned on.
            if (auto d = std::dynamic_pointer_cast<arrow::DictionaryArray>(chrom_arr)) {
                if (d->IsNull(r)) return std::string();
                int64_t idx = -1;
                auto ind = d->indices();
                if (auto a = std::dynamic_pointer_cast<arrow::Int8Array>(ind))  idx = a->Value(r);
                else if (auto a = std::dynamic_pointer_cast<arrow::Int16Array>(ind)) idx = a->Value(r);
                else if (auto a = std::dynamic_pointer_cast<arrow::Int32Array>(ind)) idx = a->Value(r);
                else if (auto a = std::dynamic_pointer_cast<arrow::Int64Array>(ind)) idx = a->Value(r);
                else if (auto a = std::dynamic_pointer_cast<arrow::UInt8Array>(ind))  idx = a->Value(r);
                else if (auto a = std::dynamic_pointer_cast<arrow::UInt16Array>(ind)) idx = a->Value(r);
                else if (auto a = std::dynamic_pointer_cast<arrow::UInt32Array>(ind)) idx = a->Value(r);
                else if (auto a = std::dynamic_pointer_cast<arrow::UInt64Array>(ind)) idx = (int64_t)a->Value(r);
                if (idx < 0) return std::string();
                auto dict = d->dictionary();
                if (auto a = std::dynamic_pointer_cast<arrow::StringArray>(dict))
                    return a->GetString(idx);
                if (auto a = std::dynamic_pointer_cast<arrow::LargeStringArray>(dict))
                    return a->GetString(idx);
            }
            return std::string();
        };
        // Genomic coordinate columns use whatever integer width is most
        // compact (UInt32 is common for positions) and may be dictionary-
        // encoded. Reading only Int32/Int64 returned 0 for every other type,
        // which made the window test (en <= w.start) reject every row and
        // silently empty the region result.
        auto plain_int = [](const arrow::Array& a, int64_t r) -> int64_t {
            switch (a.type_id()) {
                case arrow::Type::INT8:   return static_cast<const arrow::Int8Array&>(a).Value(r);
                case arrow::Type::INT16:  return static_cast<const arrow::Int16Array&>(a).Value(r);
                case arrow::Type::INT32:  return static_cast<const arrow::Int32Array&>(a).Value(r);
                case arrow::Type::INT64:  return static_cast<const arrow::Int64Array&>(a).Value(r);
                case arrow::Type::UINT8:  return static_cast<const arrow::UInt8Array&>(a).Value(r);
                case arrow::Type::UINT16: return static_cast<const arrow::UInt16Array&>(a).Value(r);
                case arrow::Type::UINT32: return static_cast<const arrow::UInt32Array&>(a).Value(r);
                case arrow::Type::UINT64: return (int64_t)static_cast<const arrow::UInt64Array&>(a).Value(r);
                default: return 0;
            }
        };
        auto cell_int = [&](const std::shared_ptr<arrow::Array>& a, int64_t r) -> int64_t {
            if (a->type_id() == arrow::Type::DICTIONARY) {
                const auto& d = static_cast<const arrow::DictionaryArray&>(*a);
                if (d.IsNull(r)) return 0;
                return plain_int(*d.dictionary(), d.GetValueIndex(r));
            }
            return plain_int(*a, r);
        };

        std::vector<std::shared_ptr<arrow::Table>> runs;
        int64_t run_start = -1;
        auto flush = [&](int64_t r_end) {
            if (run_start >= 0) {
                runs.push_back(raw->Slice(run_start, r_end - run_start));
                run_start = -1;
            }
        };
        for (int64_t r = 0; r < n_rows; ++r) {
            bool match = (cell_chrom(r) == s.window.chrom);
            if (match) {
                int64_t st = cell_int(start_arr, r);
                int64_t en = cell_int(end_arr,   r);
                if (s.window.end   != INT64_MAX && st >= s.window.end)   match = false;
                if (s.window.start != INT64_MIN && en <= s.window.start) match = false;
            }
            if (match) { if (run_start < 0) run_start = r; }
            else       { flush(r); }
        }
        flush(n_rows);

        std::shared_ptr<arrow::Table> ft;
        if (runs.empty()) {
            // Build an empty table with the projected schema.
            std::vector<std::shared_ptr<arrow::ChunkedArray>> empty_cols;
            arrow::FieldVector empty_fields;
            for (int c : cols) {
                empty_cols.push_back(std::make_shared<arrow::ChunkedArray>(
                    arrow::ArrayVector{}, schema_->field(c)->type()));
                empty_fields.push_back(schema_->field(c));
            }
            *out = arrow::Table::Make(arrow::schema(empty_fields), empty_cols, 0);
            return arrow::Status::OK();
        }
        if (runs.size() == 1) ft = runs[0];
        else {
            auto cr = arrow::ConcatenateTables(runs);
            if (!cr.ok()) return cr.status();
            ft = cr.ValueOrDie();
        }

        // Project down to just the originally-requested columns.
        std::vector<std::shared_ptr<arrow::ChunkedArray>> out_cols;
        arrow::FieldVector out_fields;
        for (int c : cols) {
            int p = col_in_raw(c);
            out_cols.push_back(ft->column(p));
            out_fields.push_back(schema_->field(c));
        }
        *out = arrow::Table::Make(arrow::schema(out_fields), out_cols,
                                  ft->num_rows());
        return arrow::Status::OK();
    }
    arrow::Status read_first(int64_t rows, const std::vector<int>& cols,
                              std::shared_ptr<arrow::Table>* out) override {
        // In region mode, the fast RecordBatchReader path bypasses our
        // predicate filter. Fall back to the default read_chunk-based
        // implementation, which goes through our overridden read_chunk
        // and applies the filter correctly.
        if (region_mode_)
            return TabularSource::read_first(rows, cols, out);

        std::vector<int> rgs;
        int64_t acc = 0;
        for (int i = 0; i < meta_->num_row_groups() && acc < rows; ++i) {
            rgs.push_back(i);
            acc += meta_->RowGroup(i)->num_rows();
        }
        if (rgs.empty()) return arrow::Status::OK();
        ARROW_ASSIGN_OR_RAISE(auto rb_uniq,
            reader_->GetRecordBatchReader(rgs, arrow_to_leaf_indices(cols)));
        std::shared_ptr<arrow::RecordBatchReader> rb(std::move(rb_uniq));
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        int64_t have = 0;
        while (have < rows) {
            std::shared_ptr<arrow::RecordBatch> b;
            ARROW_RETURN_NOT_OK(rb->ReadNext(&b));
            if (!b) break;
            if (have + b->num_rows() > rows) b = b->Slice(0, rows - have);
            have += b->num_rows();
            batches.push_back(std::move(b));
        }
        auto r = arrow::Table::FromRecordBatches(rb->schema(), batches);
        ARROW_RETURN_NOT_OK(r.status());
        *out = r.ValueOrDie();
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    bool random_access() const override { return true; }

    std::string footer() const override {
        int64_t sz = 0;
        for (int i = 0; i < meta_->num_row_groups(); ++i)
            sz += meta_->RowGroup(i)->total_compressed_size();
        std::string s;
        if (is_lociss_) s += "Format: LociSSD  |  ";
        s += "Row groups: " + std::to_string(meta_->num_row_groups()) +
             "  |  Compressed: " + fmt_size(sz);
        return s;
    }
    // Prominent top banner for LociSSD: genome assembly (+ species) and the
    // total element count. Empty for plain Parquet.
    std::string top_banner() const override {
        if (!is_lociss_) return "";
        std::string s = "LociSSD";
        if (!lociss_assembly_.empty()) {
            s += "  \xe2\x80\xa2  " + lociss_assembly_;
            if (!lociss_species_.empty()) s += " (" + lociss_species_ + ")";
        }
        int64_t n = meta_ ? meta_->num_rows() : -1;
        if (n >= 0)
            s += "  \xe2\x80\xa2  " + digits_with_sep(std::to_string(n)) + " elements";
        return s;
    }
    // Render the banner above the table in non-interactive views (the TUI draws
    // it as a reserved top row).
    std::vector<std::string> preamble_above() const override {
        std::string b = top_banner();
        return b.empty() ? std::vector<std::string>{} : std::vector<std::string>{b};
    }
    std::string created_by() const override { return meta_->created_by(); }
    // Accessors used by --stats to walk per-row-group and per-column metadata
    // without re-opening the file.
    std::shared_ptr<parquet::FileMetaData> parquet_meta() const { return meta_; }
    std::vector<int> parquet_arrow_leaves_for(int field_idx) const {
        return arrow_to_leaf_indices({field_idx});
    }
    std::vector<std::string> hidden_for_display() const override {
        // LociSSD's MaxEndSoFar is a technical derived column; hide it
        // from human-facing views. Delimited / Parquet output keep it.
        if (is_lociss_) return {"MaxEndSoFar"};
        return {};
    }
};

std::string open_parquet_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<ParquetSource> s;
    std::string e = ParquetSource::open(path, cfg, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
bool is_parquet_source(const TabularSource& src) {
    return dynamic_cast<const ParquetSource*>(&src) != nullptr;
}

std::string print_stats_only(TabularSource& src, const Config& cfg) {
    const bool json = cfg.json_array || cfg.json_lines;
    auto* pq = dynamic_cast<ParquetSource*>(&src);
    if (!pq) {
        const std::string fmt = format_label_of(src);
        if (json)
            return "--stats reads the Parquet footer; this file is " +
                   (fmt.empty() ? std::string("not Parquet") : fmt) +
                   " (--schema --json gives its columns)";
        print_schema_block(src);
        std::printf("\n%sNote:%s detailed per-column statistics are "
                    "Parquet-only; this file is a %s source.\n",
                    g_color.meta_key, g_color.reset, fmt.c_str());
        return "";
    }
    auto meta = pq->parquet_meta();
    if (!meta) return "Parquet metadata unavailable";

    int64_t total_rows = meta->num_rows();
    int     n_rg       = meta->num_row_groups();
    int64_t comp_sz = 0, raw_sz = 0;
    for (int g = 0; g < n_rg; ++g) {
        comp_sz += meta->RowGroup(g)->total_compressed_size();
        raw_sz  += meta->RowGroup(g)->total_byte_size();
    }
    auto fmt_size = [](int64_t sz) {
        char buf[32];
        if      (sz < 1024)             std::snprintf(buf,sizeof(buf),"%lld B",(long long)sz);
        else if (sz < 1024*1024)        std::snprintf(buf,sizeof(buf),"%.1f KiB",sz/1024.0);
        else if (sz < 1024LL*1024*1024) std::snprintf(buf,sizeof(buf),"%.2f MiB",sz/(1024.0*1024));
        else                            std::snprintf(buf,sizeof(buf),"%.2f GiB",sz/(1024.0*1024*1024));
        return std::string(buf);
    };
    auto codec_name = [](parquet::Compression::type c) -> const char* {
        switch (c) {
            case parquet::Compression::UNCOMPRESSED: return "none";
            case parquet::Compression::SNAPPY:       return "snappy";
            case parquet::Compression::GZIP:         return "gzip";
            case parquet::Compression::LZO:          return "lzo";
            case parquet::Compression::BROTLI:       return "brotli";
            case parquet::Compression::LZ4:          return "lz4";
            case parquet::Compression::ZSTD:         return "zstd";
            case parquet::Compression::LZ4_FRAME:    return "lz4_frame";
            case parquet::Compression::LZ4_HADOOP:   return "lz4_hadoop";
            default:                                  return "?";
        }
    };


    // Per-column rollup: sum (compressed, uncompressed) across all row groups.
    auto schema = src.schema();
    int n_cols = schema->num_fields();
    struct ColAgg {
        int64_t comp = 0;
        int64_t raw  = 0;
        int64_t nulls = 0;
        std::set<parquet::Compression::type> codecs;
        bool has_nulls = false;
    };
    // Map Arrow field index → first matching leaf column in Parquet
    // (skip lookup for nested types: stats reflect the leaf, not the parent).
    std::vector<int> leaf_for_field(n_cols, -1);
    for (int i = 0; i < n_cols; ++i) {
        // Use manifest from ParquetSource.
        auto leaves = pq->parquet_arrow_leaves_for(i);
        if (!leaves.empty()) leaf_for_field[i] = leaves[0];
    }
    std::vector<ColAgg> agg(n_cols);
    for (int g = 0; g < n_rg; ++g) {
        auto rg = meta->RowGroup(g);
        for (int i = 0; i < n_cols; ++i) {
            int leaf = leaf_for_field[i];
            if (leaf < 0 || leaf >= rg->num_columns()) continue;
            auto cc = rg->ColumnChunk(leaf);
            agg[i].comp += cc->total_compressed_size();
            agg[i].raw  += cc->total_uncompressed_size();
            agg[i].codecs.insert(cc->compression());
            if (cc->is_stats_set()) {
                auto st = cc->statistics();
                if (st && st->HasNullCount()) {
                    agg[i].nulls += st->null_count();
                    agg[i].has_nulls = true;
                }
            }
        }
    }

    auto codecs_of = [&](const ColAgg& c) {
        std::vector<std::string> names;
        for (auto k : c.codecs) names.push_back(codec_name(k));
        return names;
    };
    // --select narrows the columns reported (all of them otherwise).
    std::vector<int> sel;
    if (cfg.select_cols.empty()) {
        for (int i = 0; i < n_cols; ++i) sel.push_back(i);
    } else {
        std::vector<std::string> unknown;
        sel = select_field_indices(src, cfg, &unknown, /*include_hidden=*/true);
        if (!unknown.empty()) return "--select: no column matches '" + unknown[0] + "'";
    }
    // Min / max of field `i` in row group `g`, from the column chunk's
    // statistics; false when absent or the field is nested.
    auto rg_minmax = [&](int g, int i, std::string* mn, std::string* mx, bool* numeric) {
        if (schema->field(i)->type()->num_fields() > 0) return false;
        auto rg = meta->RowGroup(g);
        const int leaf = leaf_for_field[i];
        if (leaf < 0 || leaf >= rg->num_columns()) return false;
        auto cc = rg->ColumnChunk(leaf);
        if (!cc->is_stats_set()) return false;
        auto st = cc->statistics();
        if (!st || !st->HasMinMax()) return false;
        std::shared_ptr<arrow::Scalar> a, b;
        if (!parquet::arrow::StatisticsAsScalars(*st, &a, &b).ok() || !a || !b) return false;
        *mn = a->ToString();
        *mx = b->ToString();
        *numeric = arrow::is_integer(a->type->id()) || arrow::is_floating(a->type->id());
        // Writers store a zero minimum as -0.0 (the Parquet spec); show 0.
        if (*mn == "-0") *mn = "0";
        if (*mx == "-0") *mx = "0";
        return true;
    };
    auto rg_nulls = [&](int g, int i, int64_t* n) {
        auto rg = meta->RowGroup(g);
        const int leaf = leaf_for_field[i];
        if (leaf < 0 || leaf >= rg->num_columns()) return false;
        auto cc = rg->ColumnChunk(leaf);
        if (!cc->is_stats_set() || !cc->statistics() || !cc->statistics()->HasNullCount()) return false;
        *n = cc->statistics()->null_count();
        return true;
    };
    if (json) {
        std::printf("{\"path\": ");  json_emit_string(src.path());
        std::printf(", \"format\": "); json_emit_string(format_label_of(src));
        std::printf(", \"rows\": %lld, \"row_groups\": %d", (long long)total_rows, n_rg);
        std::printf(", \"compressed_bytes\": %lld, \"uncompressed_bytes\": %lld",
                    (long long)comp_sz, (long long)raw_sz);
        std::printf(", \"created_by\": ");
        if (pq->created_by().empty()) std::printf("null");
        else                          json_emit_string(pq->created_by());
        emit_metadata_json(*schema);
        std::printf(", \"columns\": [");
        for (size_t si = 0; si < sel.size(); ++si) {
            const int i = sel[si];
            auto f = schema->field(i);
            std::printf("%s{\"name\": ", si ? ", " : "");
            json_emit_string(f->name());
            std::printf(", \"type\": "); json_emit_string(type_label(*f->type()));
            std::printf(", \"codecs\": [");
            auto names = codecs_of(agg[i]);
            for (size_t k = 0; k < names.size(); ++k) {
                if (k) std::printf(", ");
                json_emit_string(names[k]);
            }
            std::printf("], \"compressed_bytes\": %lld, \"uncompressed_bytes\": %lld",
                        (long long)agg[i].comp, (long long)agg[i].raw);
            if (agg[i].has_nulls) std::printf(", \"nulls\": %lld}", (long long)agg[i].nulls);
            else                  std::printf(", \"nulls\": null}");
        }
        // Per row group: rows, sizes, and each reported column's nulls /
        // min / max from its statistics (null where the writer stored none).
        std::printf("], \"row_group_stats\": [");
        int64_t first = 0;
        for (int g = 0; g < n_rg; ++g) {
            auto rg = meta->RowGroup(g);
            std::printf("%s{\"index\": %d, \"rows\": %lld, \"first_row\": %lld, "
                        "\"compressed_bytes\": %lld, \"uncompressed_bytes\": %lld, \"columns\": [",
                        g ? ", " : "", g, (long long)rg->num_rows(), (long long)first,
                        (long long)rg->total_compressed_size(), (long long)rg->total_byte_size());
            first += rg->num_rows();
            for (size_t si = 0; si < sel.size(); ++si) {
                const int i = sel[si];
                std::printf("%s{\"name\": ", si ? ", " : "");
                json_emit_string(schema->field(i)->name());
                int64_t nn = 0;
                if (rg_nulls(g, i, &nn)) std::printf(", \"nulls\": %lld", (long long)nn);
                else                     std::printf(", \"nulls\": null");
                std::string mn, mx;
                bool numeric = false;
                if (rg_minmax(g, i, &mn, &mx, &numeric)) {
                    std::printf(", \"min\": ");
                    if (numeric && mn != "nan" && mx != "nan") std::printf("%s", mn.c_str()); else json_emit_string(mn);
                    std::printf(", \"max\": ");
                    if (numeric && mn != "nan" && mx != "nan") std::printf("%s", mx.c_str()); else json_emit_string(mx);
                } else {
                    std::printf(", \"min\": null, \"max\": null");
                }
                std::printf("}");
            }
            std::printf("]}");
        }
        std::printf("]}\n");
        return "";
    }

    // File-level summary
    std::printf("%sFile:%s          %s\n", g_color.meta_key, g_color.reset, src.path().c_str());
    std::printf("%sFormat:%s        Parquet\n", g_color.meta_key, g_color.reset);
    std::printf("%sRows:%s          %s\n", g_color.meta_key, g_color.reset,
                digits_with_sep(std::to_string(total_rows)).c_str());
    std::printf("%sRow groups:%s    %d\n", g_color.meta_key, g_color.reset, n_rg);
    std::printf("%sCompressed:%s    %s\n", g_color.meta_key, g_color.reset, fmt_size(comp_sz).c_str());
    std::printf("%sUncompressed:%s  %s", g_color.meta_key, g_color.reset, fmt_size(raw_sz).c_str());
    if (comp_sz > 0)
        std::printf("  (ratio: %.2fx)", (double)raw_sz / (double)comp_sz);
    std::putchar('\n');
    if (!pq->created_by().empty())
        std::printf("%sCreated by:%s    %s\n", g_color.meta_key, g_color.reset,
                    pq->created_by().c_str());
    print_schema_metadata(*schema);

    // Per-column table
    std::vector<std::array<std::string, 6>> rows;     // name, type, codec, comp, raw, ratio
    std::vector<std::string>                 nulls_col;
    int wN = 6, wT = 4, wK = 5, wC = 10, wR = 12, wRatio = 5, wNulls = 5;
    for (int i : sel) {
        auto f = schema->field(i);
        std::string codec;
        for (const auto& c : codecs_of(agg[i])) {
            if (!codec.empty()) codec += "+";
            codec += c;
        }
        if (codec.empty()) codec = "?";
        std::string ratio = (agg[i].comp > 0)
            ? (std::to_string((double)agg[i].raw / (double)agg[i].comp).substr(0, 5) + "x")
            : "-";
        std::string nulls = agg[i].has_nulls
            ? digits_with_sep(std::to_string(agg[i].nulls))
            : "?";
        rows.push_back({
            f->name(), type_label(*f->type()), codec,
            fmt_size(agg[i].comp), fmt_size(agg[i].raw), ratio
        });
        nulls_col.push_back(nulls);
        wN = std::max(wN, (int)display_width(f->name()));
        wT = std::max(wT, (int)display_width(type_label(*f->type())));
        wK = std::max(wK, (int)display_width(codec));
        wC = std::max(wC, (int)display_width(rows.back()[3]));
        wR = std::max(wR, (int)display_width(rows.back()[4]));
        wRatio = std::max(wRatio, (int)display_width(ratio));
        wNulls = std::max(wNulls, (int)display_width(nulls));
    }
    std::printf("\n%s%-*s  %-*s  %-*s  %*s  %*s  %*s  %*s%s\n",
                g_color.header,
                wN, "Column", wT, "Type", wK, "Codec",
                wC, "Compressed", wR, "Uncompressed",
                wRatio, "Ratio", wNulls, "Nulls",
                g_color.reset);
    std::printf("%s%s  %s  %s  %s  %s  %s  %s%s\n",
                g_color.border,
                std::string(wN,'-').c_str(), std::string(wT,'-').c_str(),
                std::string(wK,'-').c_str(), std::string(wC,'-').c_str(),
                std::string(wR,'-').c_str(), std::string(wRatio,'-').c_str(),
                std::string(wNulls,'-').c_str(),
                g_color.reset);
    for (size_t k = 0; k < rows.size(); ++k) {
        auto& r = rows[k];
        std::printf("%-*s  %-*s  %-*s  %*s  %*s  %*s  %*s\n",
                    wN, truncate(r[0], wN).c_str(),
                    wT, truncate(r[1], wT).c_str(),
                    wK, r[2].c_str(),
                    wC, r[3].c_str(),
                    wR, r[4].c_str(),
                    wRatio, r[5].c_str(),
                    wNulls, nulls_col[k].c_str());
    }

    // Per row group: rows, first row, sizes; with --select, the selected
    // columns' min / max from the row group's statistics. At most 50 row
    // groups are listed (--stats --json lists every one).
    {
        constexpr int kMaxShown = 50;
        std::vector<std::string> head = {"Group", "Rows", "First row", "Compressed", "Uncompressed"};
        const bool with_minmax = !cfg.select_cols.empty();
        if (with_minmax)
            for (int i : sel) {
                head.push_back(schema->field(i)->name() + " min");
                head.push_back(schema->field(i)->name() + " max");
            }
        std::vector<std::vector<std::string>> body;
        int64_t first = 0;
        for (int g = 0; g < n_rg; ++g) {
            auto rg = meta->RowGroup(g);
            if (g < kMaxShown) {
                std::vector<std::string> r = {
                    std::to_string(g), digits_with_sep(std::to_string(rg->num_rows())),
                    digits_with_sep(std::to_string(first)),
                    fmt_size(rg->total_compressed_size()), fmt_size(rg->total_byte_size())};
                if (with_minmax)
                    for (int i : sel) {
                        std::string mn = "-", mx = "-";
                        bool numeric = false;
                        rg_minmax(g, i, &mn, &mx, &numeric);
                        r.push_back(mn);
                        r.push_back(mx);
                    }
                body.push_back(std::move(r));
            }
            first += rg->num_rows();
        }
        std::vector<int> w(head.size());
        for (size_t c = 0; c < head.size(); ++c) {
            w[c] = (int)display_width(head[c]);
            for (const auto& r : body) w[c] = std::max(w[c], (int)display_width(r[c]));
            w[c] = std::min(w[c], 40);
        }
        std::printf("\n%s", g_color.header);
        for (size_t c = 0; c < head.size(); ++c)
            std::printf("%s%*s", c ? "  " : "", w[c], truncate(head[c], w[c]).c_str());
        std::printf("%s\n%s", g_color.reset, g_color.border);
        for (size_t c = 0; c < head.size(); ++c)
            std::printf("%s%s", c ? "  " : "", std::string((size_t)w[c], '-').c_str());
        std::printf("%s\n", g_color.reset);
        for (const auto& r : body) {
            for (size_t c = 0; c < r.size(); ++c)
                std::printf("%s%*s", c ? "  " : "", w[c], truncate(r[c], w[c]).c_str());
            std::printf("\n");
        }
        if (n_rg > kMaxShown)
            std::printf("%s… %d more row groups (--stats --json lists every one)%s\n",
                        g_color.meta_key, n_rg - kMaxShown, g_color.reset);
    }
    return "";
}
