// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// --validate, --describe, --unique, --sample, --tail, --sort, --distinct.

#include "internal.hpp"

// ── --validate: LociSSD invariants check ─────────────────────────────────────
//
// Checks performed (per FORMAT_SPEC §3, §5, §7):
//   1. Footer contains `lociSSD_manifest` and parses cleanly.
//   2. Schema has Chromosome (string or dict<string>), Start, End,
//      MaxEndSoFar (all integer).
//   3. Manifest covers every row exactly once: per-chromosome row_offset
//      values are contiguous from 0 and sum to total_rows.
//   4. Within each chromosome, rows are sorted lexicographically by
//      (Start, End).
//   5. For every row, MaxEndSoFar[i] == max(End[chrom_first..i])
//      (resetting at each chromosome boundary).
//   6. The chromosome label at each row matches the manifest's window
//      for that row index.
//
// Prints PASS / FAIL lines and a one-line summary. Returns "" on success
// (all checks passed) or an error string on FAILURE / I/O error.
std::string validate_lociss(const std::string& path) {
    auto maybe_file = arrow::io::ReadableFile::Open(path);
    if (!maybe_file.ok())
        return "Cannot open '" + path + "': " + maybe_file.status().ToString();

    parquet::ReaderProperties rdr_props(arrow::default_memory_pool());
    rdr_props.enable_buffered_stream();
    rdr_props.set_buffer_size(4 << 20);

    parquet::arrow::FileReaderBuilder builder;
    auto st = builder.Open(maybe_file.ValueOrDie(), rdr_props);
    if (!st.ok()) return "Not a valid Parquet file: " + st.ToString();
    builder.memory_pool(arrow::default_memory_pool());
    parquet::ArrowReaderProperties ap = parquet::default_arrow_reader_properties();
    ap.set_pre_buffer(true);
    ap.set_use_threads(true);
    builder.properties(ap);

    std::unique_ptr<parquet::arrow::FileReader> reader;
    st = builder.Build(&reader);
    if (!st.ok()) return "Error opening Parquet: " + st.ToString();

    auto meta = reader->parquet_reader()->metadata();
    std::shared_ptr<arrow::Schema> schema;
    st = reader->GetSchema(&schema);
    if (!st.ok()) return "Error reading schema: " + st.ToString();

    int n_pass = 0, n_fail = 0;
    auto pass = [&](const std::string& msg) {
        std::printf("  PASS  %s\n", msg.c_str()); ++n_pass;
    };
    auto fail = [&](const std::string& msg) {
        std::printf("  FAIL  %s\n", msg.c_str()); ++n_fail;
    };

    std::printf("Validating LociSSD invariants for %s\n", path.c_str());

    // 1. Manifest present + parses.
    std::string manifest_json;
    if (auto kv = meta->key_value_metadata()) {
        if (kv->Contains("lociSSD_manifest")) {
            auto v = kv->Get("lociSSD_manifest");
            if (v.ok()) manifest_json = *v;
        }
    }
    if (manifest_json.empty()) {
        fail("lociSSD_manifest footer key is missing (not a LociSSD file)");
        std::fflush(stdout);
        return "validation failed: not a LociSSD file";
    }
    std::vector<LocissChrom> chroms;
    if (!parse_lociss_chromosomes(manifest_json, &chroms)) {
        fail("lociSSD_manifest is present but malformed");
        std::fflush(stdout);
        return "validation failed: malformed manifest";
    }
    pass("manifest present and parses (" + std::to_string(chroms.size()) +
         " chromosomes)");

    // 2. Schema has required columns with usable types.
    int j_chr = -1, j_st = -1, j_en = -1, j_mes = -1;
    for (int i = 0; i < schema->num_fields(); ++i) {
        const auto& n = schema->field(i)->name();
        if      (n == "Chromosome")  j_chr = i;
        else if (n == "Start")       j_st  = i;
        else if (n == "End")         j_en  = i;
        else if (n == "MaxEndSoFar") j_mes = i;
    }
    if (j_chr < 0 || j_st < 0 || j_en < 0 || j_mes < 0) {
        fail("schema missing one of Chromosome / Start / End / MaxEndSoFar");
        std::fflush(stdout);
        return "validation failed: schema incomplete";
    }
    auto is_int_like = [](const arrow::DataType& t) {
        return t.id() == arrow::Type::INT8  || t.id() == arrow::Type::INT16
            || t.id() == arrow::Type::INT32 || t.id() == arrow::Type::INT64
            || t.id() == arrow::Type::UINT8 || t.id() == arrow::Type::UINT16
            || t.id() == arrow::Type::UINT32|| t.id() == arrow::Type::UINT64;
    };
    auto is_string_like = [](const arrow::DataType& t) {
        if (t.id() == arrow::Type::STRING || t.id() == arrow::Type::LARGE_STRING) return true;
        if (t.id() == arrow::Type::DICTIONARY) {
            const auto& d = static_cast<const arrow::DictionaryType&>(t);
            return d.value_type()->id() == arrow::Type::STRING ||
                   d.value_type()->id() == arrow::Type::LARGE_STRING;
        }
        return false;
    };
    bool coords_int = true;
    if (!is_string_like(*schema->field(j_chr)->type()))
        fail("Chromosome must be string or dict<string>");
    else                                                pass("Chromosome column is string-like");
    if (!is_int_like(*schema->field(j_st)->type()))  { fail("Start must be integer");       coords_int = false; }
    else                                                pass("Start column is integer");
    if (!is_int_like(*schema->field(j_en)->type()))  { fail("End must be integer");         coords_int = false; }
    else                                                pass("End column is integer");
    if (!is_int_like(*schema->field(j_mes)->type())) { fail("MaxEndSoFar must be integer"); coords_int = false; }
    else                                                pass("MaxEndSoFar column is integer");

    // 3. Manifest coverage: row_offsets contiguous from 0, total = file rows.
    int64_t expected = 0;
    bool coverage_ok = true;
    for (size_t i = 0; i < chroms.size(); ++i) {
        if (chroms[i].row_offset != expected) {
            fail("chromosome '" + chroms[i].name + "' row_offset=" +
                 std::to_string(chroms[i].row_offset) + " expected " +
                 std::to_string(expected));
            coverage_ok = false;
        }
        expected += chroms[i].rows;
    }
    int64_t total_rows = meta->num_rows();
    if (expected != total_rows) {
        fail("manifest sum-of-rows " + std::to_string(expected) +
             " != Parquet num_rows " + std::to_string(total_rows));
        coverage_ok = false;
    }
    if (coverage_ok) pass("manifest covers all " + std::to_string(total_rows) +
                          " rows contiguously");

    // The per-row invariant scan below reads Start/End/MaxEndSoFar as integers.
    // If any of those columns is not integer-typed the schema check above already
    // failed; skip the scan rather than let a string cell reach std::stoll (which
    // would throw and abort). The overall result is still "validation failed".
    if (!coords_int) {
        std::printf("\n%d check(s) passed, %d failed\n", n_pass, n_fail);
        std::fflush(stdout);
        return "validation failed (" + std::to_string(n_fail) + " checks)";
    }

    // 4-6. Stream every row, checking sort order, MaxEndSoFar, chrom label.
    // GetRecordBatchReader takes Parquet *leaf* column indices — expand each
    // Arrow field to its leaves via the schema manifest (Chromosome/Start/End/
    // MaxEndSoFar are flat for any well-formed LociSSD file, so each maps to
    // exactly one leaf, but we go through the manifest for safety).
    std::vector<int> arrow_cols = {j_chr, j_st, j_en, j_mes};
    std::vector<int> leaf_cols;
    {
        const auto& m = reader->manifest();
        std::function<void(const parquet::arrow::SchemaField&)> walk =
            [&](const parquet::arrow::SchemaField& sf) {
                if (sf.is_leaf()) { leaf_cols.push_back(sf.column_index); return; }
                for (const auto& ch : sf.children) walk(ch);
            };
        for (int idx : arrow_cols) {
            if (idx >= 0 && idx < (int)m.schema_fields.size())
                walk(m.schema_fields[idx]);
        }
    }
    std::vector<int> all_rgs;
    for (int g = 0; g < meta->num_row_groups(); ++g) all_rgs.push_back(g);
    std::shared_ptr<arrow::RecordBatchReader> rb;
    // The Result-returning GetRecordBatchReader has shipped since the Status one
    // was deprecated (Arrow 21.0.0), so this needs no version guard (the static
    // build's Arrow 23.0.1 already uses it elsewhere).
    if (auto rb_res = reader->GetRecordBatchReader(all_rgs, leaf_cols); rb_res.ok()) {
        rb = std::move(*rb_res);
    } else {
        fail("cannot read columns: " + rb_res.status().ToString());
        return "validation failed: read error";
    }

    int64_t row = 0;
    int64_t chrom_first_row = 0;
    size_t  manifest_idx = 0;
    int64_t prev_start = INT64_MIN, prev_end = INT64_MIN;
    int64_t running_max_end = INT64_MIN;
    std::string prev_chrom;
    int   sort_failures = 0, mes_failures = 0, chrom_failures = 0, coord_failures = 0;
    int64_t SHOW_MAX = 5;  // cap how many violations we print per check

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        st = rb->ReadNext(&batch);
        if (!st.ok()) { fail("read error: " + st.ToString()); return "validation failed"; }
        if (!batch) break;
        auto chr_col = batch->column(0);
        auto st_col  = batch->column(1);
        auto en_col  = batch->column(2);
        auto mes_col = batch->column(3);
        // A null or otherwise unparseable coordinate must be reported, not
        // crash: cell_to_string renders a null as the null symbol, and
        // std::stoll throws on it (and on any non-digit text).
        auto read_coord = [](const arrow::Array& col, int64_t r, bool* ok) -> int64_t {
            if (col.IsNull(r)) { *ok = false; return 0; }
            try { return std::stoll(cell_to_string(col, r)); }
            catch (...) { *ok = false; return 0; }
        };
        for (int64_t r = 0; r < batch->num_rows(); ++r, ++row) {
            std::string this_chrom = cell_to_string(*chr_col, r);
            bool coord_ok = true;
            int64_t this_start = read_coord(*st_col,  r, &coord_ok);
            int64_t this_end   = read_coord(*en_col,  r, &coord_ok);
            int64_t this_mes   = read_coord(*mes_col, r, &coord_ok);
            if (!coord_ok) {
                if (coord_failures++ < SHOW_MAX)
                    fail("row " + std::to_string(row) +
                         " has a null or non-integer coordinate");
                continue;
            }

            // Find this row's manifest entry.
            while (manifest_idx < chroms.size() &&
                   row >= chroms[manifest_idx].row_offset + chroms[manifest_idx].rows)
                ++manifest_idx;
            if (manifest_idx >= chroms.size()) {
                if (chrom_failures++ < SHOW_MAX)
                    fail("row " + std::to_string(row) + " past manifest end");
                continue;
            }
            const auto& cur_chrom = chroms[manifest_idx];

            // chrom label vs manifest
            if (this_chrom != cur_chrom.name) {
                if (chrom_failures++ < SHOW_MAX)
                    fail("row " + std::to_string(row) + " chromosome '" +
                         this_chrom + "' but manifest says '" + cur_chrom.name + "'");
            }

            // Chromosome boundary detection (by manifest, not by label).
            if (row == cur_chrom.row_offset) {
                chrom_first_row = row;
                prev_start = INT64_MIN; prev_end = INT64_MIN;
                running_max_end = INT64_MIN;
            }

            // sort order within chromosome
            if (row > chrom_first_row) {
                if (this_start < prev_start ||
                    (this_start == prev_start && this_end < prev_end)) {
                    if (sort_failures++ < SHOW_MAX)
                        fail("sort-order violation at row " + std::to_string(row) +
                             " on " + cur_chrom.name + ": (" +
                             std::to_string(this_start) + "," + std::to_string(this_end) +
                             ") < (" + std::to_string(prev_start) + "," +
                             std::to_string(prev_end) + ")");
                }
            }
            prev_start = this_start; prev_end = this_end;

            // MaxEndSoFar
            if (this_end > running_max_end) running_max_end = this_end;
            if (this_mes != running_max_end) {
                if (mes_failures++ < SHOW_MAX)
                    fail("MaxEndSoFar wrong at row " + std::to_string(row) +
                         " on " + cur_chrom.name + ": stored=" +
                         std::to_string(this_mes) + " expected=" +
                         std::to_string(running_max_end));
            }
        }
    }
    if (row != total_rows)
        fail("scanned " + std::to_string(row) + " rows, expected " +
             std::to_string(total_rows));

    if (sort_failures == 0)
        pass("rows are sorted by (Start, End) within each chromosome");
    else if (sort_failures > SHOW_MAX)
        std::printf("        (%d more sort-order violations not shown)\n",
                    (int)(sort_failures - SHOW_MAX));
    if (mes_failures == 0)
        pass("MaxEndSoFar matches running max(End) within each chromosome");
    else if (mes_failures > SHOW_MAX)
        std::printf("        (%d more MaxEndSoFar violations not shown)\n",
                    (int)(mes_failures - SHOW_MAX));
    if (chrom_failures == 0)
        pass("Chromosome labels match the manifest at every row");
    else if (chrom_failures > SHOW_MAX)
        std::printf("        (%d more label/manifest mismatches not shown)\n",
                    (int)(chrom_failures - SHOW_MAX));
    if (coord_failures == 0)
        pass("every Start / End / MaxEndSoFar is a non-null integer");
    else if (coord_failures > SHOW_MAX)
        std::printf("        (%d more null/non-integer coordinates not shown)\n",
                    (int)(coord_failures - SHOW_MAX));

    std::printf("\n%d check(s) passed, %d failed\n", n_pass, n_fail);
    std::fflush(stdout);
    if (n_fail > 0) return "validation failed (" + std::to_string(n_fail) + " checks)";
    return "";
}

// ── --describe: per-column statistics ────────────────────────────────────────

// Print "Column | Type | Count | Nulls | Min | Max | Mean | Distinct".
// Structured per-column statistics for one source column. Drains streaming
// sources and scans every value. Shares the per-value logic with the
// (multi-column, text-formatting) print_describe below; kept separate so the
// GUI gets a clean struct without the ASCII table.
ColStats compute_col_stats(TabularSource& src, int src_col) {
    ColStats cs;
    if (src_col < 0 || src_col >= src.schema()->num_fields()) return cs;
    auto f = src.schema()->field(src_col);
    cs.name = f->name();
    cs.type = type_label(*f->type());
    cs.is_numeric = is_numeric_type(f->type()->id());
    cs.valid = true;

    double dmin = std::numeric_limits<double>::infinity();
    double dmax = -std::numeric_limits<double>::infinity();
    long double sum = 0.0L;
    std::set<std::string> distinct;

    src.set_retain_all(true);  // re-reads every chunk after draining
    while (true) { int n = src.num_chunks(); src.ensure(n); if (src.num_chunks() == n) break; }
    for (int c = 0; c < src.num_chunks(); ++c) {
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, {src_col}, &tbl).ok() || !tbl) continue;
        auto col = tbl->column(0);
        for (auto& ch : col->chunks()) {
            int64_t n = ch->length();
            for (int64_t r = 0; r < n; ++r) {
                if (ch->IsNull(r)) { cs.nulls++; continue; }
                cs.count++;
                if (cs.is_numeric) {
                    double d;
                    if (!array_value_as_double(*ch, r, &d)) continue;
                    if (d < dmin) dmin = d;
                    if (d > dmax) dmax = d;
                    sum += d;
                } else {
                    std::string s = cell_to_string(*ch, r);
                    if (cs.count == 1 || s < cs.s_min) cs.s_min = s;
                    if (cs.count == 1 || s > cs.s_max) cs.s_max = s;
                    if (!cs.distinct_overflow) {
                        distinct.insert(s);
                        if (distinct.size() > 16) { cs.distinct_overflow = true; distinct.clear(); }
                    }
                }
            }
        }
    }
    if (cs.is_numeric && cs.count > 0) {
        cs.min = dmin; cs.max = dmax;
        cs.mean = (double)(sum / (long double)cs.count);
    }
    cs.distinct.assign(distinct.begin(), distinct.end());
    return cs;
}

// `Mean` only filled for numeric columns; `Distinct` only when small.
std::string print_describe(TabularSource& src, const Config& cfg) {
    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> read_set = have_filter
        ? union_with_filter(requested, fx) : requested;

    struct ColStat {
        std::string  name;
        std::string  type;
        std::shared_ptr<arrow::DataType> dtype;
        bool         is_num = false;
        int64_t      count  = 0;
        int64_t      nulls  = 0;
        double       d_min  = std::numeric_limits<double>::infinity();
        double       d_max  = -std::numeric_limits<double>::infinity();
        long double  sum    = 0.0L;
        std::string  s_min, s_max;
        std::set<std::string> distinct;     // capped at 16
        bool         distinct_overflow = false;
        // Standard deviation, by Welford's update (stable where a sum of
        // squares cancels).
        int64_t      w_n = 0;
        long double  w_mean = 0.0L, w_m2 = 0.0L;
        // Percentiles: every non-NaN value up to `sample_cap`, then a uniform
        // reservoir sample of that size (Algorithm R, fixed seed so the output
        // is reproducible).
        std::vector<double> sample;
        int64_t      sample_seen = 0;
        uint64_t     rng = 0x9e3779b97f4a7c15ULL;
    };
    std::vector<ColStat> stats(requested.size());
    for (size_t k = 0; k < requested.size(); ++k) {
        auto f = src.schema()->field(requested[k]);
        stats[k].name   = f->name();
        stats[k].type   = type_label(*f->type());
        stats[k].dtype  = f->type();
        stats[k].is_num = is_numeric_type(f->type()->id());
    }

    // --describe summarises the WHOLE table by default (Config::head_rows
    // defaults to 10, the table-view preview size — but a describe over only
    // the first 10 rows would be misleading). Honour -n only when the user set
    // it explicitly, matching the --json/--md/--parquet output paths.
    int64_t rows_left = (!cfg.head_rows_set || cfg.head_rows <= 0)
                        ? INT64_MAX : (int64_t)cfg.head_rows;

    // Values kept for the percentiles: 16 Mi doubles (128 MiB) shared by the
    // numeric columns, at least 1024 each so a wide matrix still gets an
    // estimate.
    const bool want_pct = !cfg.percentiles.empty();
    int64_t n_numeric = 0;
    for (auto& cs : stats) n_numeric += cs.is_num;
    int64_t sample_cap = std::max<int64_t>(1024, (int64_t{1} << 24) /
                                                 std::max<int64_t>(1, n_numeric));
    // Test hook: a small cap exercises the sampled path on a small file.
    if (const char* e = std::getenv("VV_DESCRIBE_SAMPLE_CAP"); e && std::atoll(e) > 0)
        sample_cap = std::atoll(e);
    auto splitmix = [](uint64_t& x) {
        uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    };

    for (int c = 0; rows_left > 0; ++c) {
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, read_set, &tbl).ok()) continue;
        if (have_filter) tbl = apply_filter(tbl, fx, read_set);
        if (!tbl || tbl->num_rows() == 0) continue;
        int64_t take = std::min(tbl->num_rows(), rows_left);
        if (take < tbl->num_rows()) tbl = tbl->Slice(0, take);

        for (size_t k = 0; k < requested.size(); ++k) {
            int p = -1;
            for (size_t j = 0; j < read_set.size(); ++j)
                if (read_set[j] == requested[k]) { p = (int)j; break; }
            auto col = tbl->column(p);
            ColStat& cs = stats[k];
            for (auto& ch : col->chunks()) {
                int64_t n = ch->length();
                for (int64_t r = 0; r < n; ++r) {
                    if (ch->IsNull(r)) { cs.nulls++; continue; }
                    cs.count++;
                    if (cs.is_num) {
                        double d;
                        if (!array_value_as_double(*ch, r, &d)) continue;
                        if (d < cs.d_min) cs.d_min = d;
                        if (d > cs.d_max) cs.d_max = d;
                        cs.sum += d;
                        ++cs.w_n;
                        const long double delta = d - cs.w_mean;
                        cs.w_mean += delta / cs.w_n;
                        cs.w_m2   += delta * (d - cs.w_mean);
                        if (want_pct && !std::isnan(d)) {
                            if ((int64_t)cs.sample.size() < sample_cap) {
                                cs.sample.push_back(d);
                            } else {
                                uint64_t j = splitmix(cs.rng) % (uint64_t)(cs.sample_seen + 1);
                                if (j < (uint64_t)sample_cap) cs.sample[j] = d;
                            }
                            ++cs.sample_seen;
                        }
                    } else {
                        std::string s = cell_to_string(*ch, r);
                        if (cs.count == 1 || s < cs.s_min) cs.s_min = s;
                        if (cs.count == 1 || s > cs.s_max) cs.s_max = s;
                        if (!cs.distinct_overflow) {
                            cs.distinct.insert(s);
                            if (cs.distinct.size() > 16) {
                                cs.distinct_overflow = true;
                                cs.distinct.clear();
                            }
                        }
                    }
                }
            }
        }
        rows_left -= take;
    }
    // A stream that failed part-way (a malformed record, a truncated file)
    // must not be summarised as if the rows read so far were the whole table.
    if (!src.read_status().ok())
        return shorten_reader_error(src.read_status().ToString());

    // Percentiles by linear interpolation between the closest ranks (R's
    // type 7, NumPy's and pandas' default); std with n - 1 in the denominator
    // (R's sd(), pandas' std()).
    std::vector<std::vector<double>> pct(stats.size());
    std::vector<bool> pct_sampled(stats.size(), false);
    for (size_t k = 0; k < stats.size(); ++k) {
        auto& v = stats[k].sample;
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        for (double p : cfg.percentiles) {
            const double h = (double)(v.size() - 1) * p / 100.0;
            const size_t lo = (size_t)std::floor(h);
            const size_t hi = std::min(lo + 1, v.size() - 1);
            pct[k].push_back(v[lo] + (h - (double)lo) * (v[hi] - v[lo]));
        }
        pct_sampled[k] = stats[k].sample_seen > (int64_t)v.size();
        std::vector<double>().swap(v);
    }
    auto std_of = [](const ColStat& cs, double* out) {
        if (cs.w_n < 2) return false;
        *out = (double)std::sqrt(cs.w_m2 / (long double)(cs.w_n - 1));
        return true;
    };
    // "25%", "99.5%"
    auto pct_label = [](double p) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g%%", p);
        return std::string(buf);
    };

    // Machine-readable stats: `--describe --json` emits a JSON array of per-column
    // objects, `--describe --ndjson` one object per line. Numbers are exact
    // (integers as integers, floats round-trippable); strings JSON-escaped.
    if (cfg.json_array || cfg.json_lines) {
        auto jnum = [](double v) -> std::string {
            if (!std::isfinite(v)) return "null";
            char buf[40];
            if (v == std::floor(v) && std::fabs(v) < 9.2e18)
                std::snprintf(buf, sizeof buf, "%lld", (long long)v);
            else
                std::snprintf(buf, sizeof buf, "%.17g", v);
            return buf;
        };
        if (cfg.json_array) std::putchar('[');
        for (size_t k = 0; k < stats.size(); ++k) {
            auto& cs = stats[k];
            if (cfg.json_array && k) std::putchar(',');
            std::printf("{\"column\":");        json_emit_string(cs.name);
            std::printf(",\"type\":");          json_emit_string(cs.type);
            std::printf(",\"numeric\":%s,\"count\":%lld,\"nulls\":%lld",
                        cs.is_num ? "true" : "false",
                        (long long)cs.count, (long long)cs.nulls);
            if (cs.count == 0) {
                std::printf(",\"min\":null,\"max\":null");
                if (cs.is_num) std::printf(",\"mean\":null");
            } else if (cs.is_num) {
                std::printf(",\"min\":%s,\"max\":%s,\"mean\":%s",
                            jnum(cs.d_min).c_str(), jnum(cs.d_max).c_str(),
                            jnum((double)(cs.sum / (long double)cs.count)).c_str());
            } else {
                std::printf(",\"min\":"); json_emit_string(cs.s_min);
                std::printf(",\"max\":"); json_emit_string(cs.s_max);
            }
            if (cs.is_num) {
                double sd;
                std::printf(",\"std\":%s", std_of(cs, &sd) ? jnum(sd).c_str() : "null");
                if (!cfg.percentiles.empty()) {
                    std::printf(",\"percentiles\":{");
                    for (size_t i = 0; i < cfg.percentiles.size(); ++i) {
                        if (i) std::putchar(',');
                        json_emit_string(pct_label(cfg.percentiles[i]));
                        std::printf(":%s", pct[k].empty() ? "null" : jnum(pct[k][i]).c_str());
                    }
                    std::printf("},\"percentiles_sampled\":%s",
                                pct_sampled[k] ? "true" : "false");
                }
            }
            if (!cs.is_num) {
                if (cs.distinct_overflow)
                    std::printf(",\"distinct\":null,\"distinct_overflow\":true");
                else
                    std::printf(",\"distinct\":%zu", cs.distinct.size());
            }
            std::putchar('}');
            if (cfg.json_lines) std::putchar('\n');
        }
        if (cfg.json_array) std::printf("]\n");
        return "";
    }

    // Pretty-print
    auto fmt_num = [](double v) -> std::string {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        return std::string(buf);
    };
    // A date / timestamp statistic (a count of days / ms / … since the epoch)
    // as the column shows its values: 2024-06-15, 2024-06-15 12:30:00.000.
    auto fmt_temporal = [](double v, const std::shared_ptr<arrow::DataType>& t) -> std::string {
        std::shared_ptr<arrow::Scalar> sc;
        const int64_t n = (int64_t)std::llround(v);
        switch (t->id()) {
            case arrow::Type::DATE32:    sc = std::make_shared<arrow::Date32Scalar>((int32_t)n); break;
            case arrow::Type::DATE64:    sc = std::make_shared<arrow::Date64Scalar>(n); break;
            case arrow::Type::TIMESTAMP: sc = std::make_shared<arrow::TimestampScalar>(n, t); break;
            default: return "";
        }
        auto arr = arrow::MakeArrayFromScalar(*sc, 1);
        return arr.ok() ? cell_to_string(**arr, 0) : "";
    };
    auto width = [](const std::string& s) { return (int)display_width(s); };

    // Std and the percentiles appear when a numeric column is shown.
    const bool num_cols = n_numeric > 0;
    std::vector<std::string> head = {"Column", "Type", "Count", "Nulls", "Min", "Max", "Mean"};
    if (num_cols) {
        head.push_back("Std");
        for (double p : cfg.percentiles) head.push_back(pct_label(p));
    }
    head.push_back("Distinct");
    const size_t n_out = head.size();
    // Count and Nulls are right-aligned; Column, Type, Min and Max are cut at
    // 40 columns.
    auto right = [](size_t i) { return i == 2 || i == 3; };
    auto capped = [](size_t i) { return i <= 1 || i == 4 || i == 5; };

    std::vector<std::vector<std::string>> rows;
    for (size_t k = 0; k < stats.size(); ++k) {
        auto& cs = stats[k];
        const bool temporal = cs.is_num && is_date_or_timestamp(*cs.dtype);
        auto fmt = [&](double v) { return temporal ? fmt_temporal(v, cs.dtype) : fmt_num(v); };
        std::string mn, mx, me;
        if (cs.count == 0) {
            mn = "-"; mx = "-"; me = "-";
        } else if (cs.is_num) {
            mn = fmt(cs.d_min);
            mx = fmt(cs.d_max);
            me = fmt((double)(cs.sum / (long double)cs.count));
        } else {
            mn = cs.s_min; mx = cs.s_max; me = "";
        }
        std::vector<std::string> r = {cs.name, cs.type, std::to_string(cs.count),
                                      std::to_string(cs.nulls), mn, mx, me};
        if (num_cols) {
            double sd;
            if (!cs.is_num || temporal) r.push_back("");
            else if (!std_of(cs, &sd)) r.push_back("-");
            else r.push_back(std::isnan(sd) ? "nan" : fmt_num(sd));   // not "-nan"
            for (size_t i = 0; i < cfg.percentiles.size(); ++i) {
                if (!cs.is_num)          r.push_back("");
                else if (pct[k].empty()) r.push_back("-");
                else r.push_back((pct_sampled[k] ? "~" : "") + fmt(pct[k][i]));
            }
        }
        r.push_back(cs.is_num ? ""
                    : (cs.distinct_overflow ? ">16" : std::to_string(cs.distinct.size())));
        rows.push_back(std::move(r));
    }

    std::vector<int> w(n_out);
    for (size_t i = 0; i < n_out; ++i) {
        w[i] = width(head[i]);
        for (auto& r : rows) w[i] = std::max(w[i], width(r[i]));
        if (w[i] > 40 && i != n_out - 1) w[i] = 40;
    }
    auto pad = [&](const std::string& v, size_t i) {
        std::string t = capped(i) ? truncate(v, w[i]) : v;
        std::string fill(std::max(0, w[i] - width(t)), ' ');
        return right(i) ? fill + t : t + fill;
    };
    std::string line = g_color.header;
    for (size_t i = 0; i < n_out; ++i) line += (i ? "  " : "") + pad(head[i], i);
    std::printf("%s%s\n", line.c_str(), g_color.reset);
    line = g_color.border;
    for (size_t i = 0; i < n_out; ++i) line += (i ? "  " : "") + std::string(w[i], '-');
    std::printf("%s%s\n", line.c_str(), g_color.reset);
    for (auto& r : rows) {
        line.clear();
        for (size_t i = 0; i < n_out; ++i) line += (i ? "  " : "") + pad(r[i], i);
        std::printf("%s\n", line.c_str());
    }
    return "";
}

// Forward decl (definition lives alongside print_table at the bottom of
// this file).

// ── --stats: Parquet metadata footer dump ────────────────────────────────────
//
// Prints what was written into the Parquet file (row groups, codecs, per-
// column sizes, statistics) without decoding any data. For non-Parquet
// sources, prints the schema block and a note that detailed stats are
// Parquet-only.

// ── --value-counts (--unique): distinct value counts per column ─────────────
std::string print_unique(TabularSource& src, const Config& cfg) {
    ExactFloats exact;   // values are identity keys: never merge by rounding
    if (cfg.unique_cols.empty()) return "--value-counts needs a comma-separated column list";
    auto schema = src.schema();
    std::vector<int> cols;
    std::vector<std::string> unknown;
    size_t p = 0;
    while (p <= cfg.unique_cols.size()) {
        size_t comma = cfg.unique_cols.find(',', p);
        std::string name = cfg.unique_cols.substr(p,
            comma == std::string::npos ? std::string::npos : comma - p);
        while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(0, 1);
        while (!name.empty() && std::isspace((unsigned char)name.back()))  name.pop_back();
        if (!name.empty()) {
            int idx = schema->GetFieldIndex(name);
            if (idx >= 0) cols.push_back(idx);
            else          unknown.push_back(name);
        }
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
    if (!unknown.empty()) {
        std::string u; for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        return "unknown column(s) in --value-counts: " + u;
    }
    if (cols.empty()) return "--value-counts: no columns specified";

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> read_set = have_filter ? union_with_filter(cols, fx) : cols;

    // Counts per column. Hash map: O(1) average insert per cell, versus a
    // red-black tree's O(log distinct) string comparisons — the distinct set is
    // unbounded here, so on a high-cardinality column the tree dominates. The
    // final sort imposes a deterministic order, so iteration order is moot.
    std::vector<std::unordered_map<std::string, int64_t>> counts(cols.size());
    std::vector<int64_t> nulls(cols.size(), 0);   // kept apart: no string can stand for null
    int64_t total = 0;
    for (int c = 0; ; ++c) {
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, read_set, &tbl).ok()) continue;
        if (have_filter) tbl = apply_filter(tbl, fx, read_set);
        if (!tbl || tbl->num_rows() == 0) continue;
        total += tbl->num_rows();
        for (size_t k = 0; k < cols.size(); ++k) {
            int p_in_tbl = -1;
            for (size_t j = 0; j < read_set.size(); ++j)
                if (read_set[j] == cols[k]) { p_in_tbl = (int)j; break; }
            auto col = tbl->column(p_in_tbl);
            for (auto& ch : col->chunks()) {
                int64_t n = ch->length();
                for (int64_t r = 0; r < n; ++r) {
                    if (ch->IsNull(r)) ++nulls[k];
                    else               counts[k][cell_to_string(*ch, r)]++;
                }
            }
        }
    }

    // A stream that failed part-way must not be counted as if the rows read
    // so far were the whole table.
    if (!src.read_status().ok())
        return shorten_reader_error(src.read_status().ToString());

    // Per column: entries by count descending; equal counts by value ascending
    // (numerically for a numeric column) so the output is deterministic
    // despite the hash map's unspecified order. Null is an entry like any
    // other value.
    struct Entry { std::string value; int64_t count; bool null; };
    std::vector<std::vector<Entry>> sorted(cols.size());
    for (size_t k = 0; k < cols.size(); ++k) {
        const bool numeric = is_numeric_type(schema->field(cols[k])->type()->id());
        auto& e = sorted[k];
        e.reserve(counts[k].size() + 1);
        for (auto& [v, n] : counts[k]) e.push_back({v, n, false});
        std::unordered_map<std::string, int64_t>().swap(counts[k]);
        if (nulls[k]) e.push_back({"", nulls[k], true});
        auto as_num = [](const std::string& v) {
            char* end = nullptr;
            double d = std::strtod(v.c_str(), &end);
            return (end && !*end && !v.empty()) ? d : std::numeric_limits<double>::quiet_NaN();
        };
        std::sort(e.begin(), e.end(), [&](const Entry& a, const Entry& b) {
            if (a.count != b.count) return a.count > b.count;
            if (a.null != b.null) return b.null;            // null last among equals
            if (numeric) {
                double x = as_num(a.value), y = as_num(b.value);
                if (x < y) return true;
                if (y < x) return false;
            }
            return a.value < b.value;
        });
    }
    auto pct_of = [&](int64_t n) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f%%", total ? 100.0 * (double)n / (double)total : 0.0);
        return std::string(buf);
    };
    auto frac_of = [&](int64_t n) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.6g", total ? (double)n / (double)total : 0.0);
        return std::string(buf);
    };

    // --tsv / --csv / --delimiter: one row per (column, value), every value.
    if (cfg.delimiter) {
        const char sep = cfg.delimiter;
        if (!cfg.no_header) {
            const char* h[] = {"column", "value", "count", "fraction"};
            for (int i = 0; i < 4; ++i) {
                if (i) std::putchar(sep);
                write_csv_field(h[i], sep);
            }
            std::putchar('\n');
        }
        for (size_t k = 0; k < cols.size(); ++k)
            for (auto& e : sorted[k]) {
                write_csv_field(schema->field(cols[k])->name(), sep);
                std::putchar(sep);
                if (!e.null) write_csv_field(e.value, sep);
                std::printf("%c%lld%c%s\n", sep, (long long)e.count, sep, frac_of(e.count).c_str());
            }
        return "";
    }

    // --json / --ndjson: one object per column with every value.
    if (cfg.json_array || cfg.json_lines) {
        if (cfg.json_array) std::putchar('[');
        for (size_t k = 0; k < cols.size(); ++k) {
            if (cfg.json_array && k) std::putchar(',');
            std::printf("{\"column\":");
            json_emit_string(schema->field(cols[k])->name());
            std::printf(",\"rows\":%lld,\"distinct\":%zu,\"values\":[",
                        (long long)total, sorted[k].size());
            const bool numeric = is_numeric_type(schema->field(cols[k])->type()->id());
            for (size_t i = 0; i < sorted[k].size(); ++i) {
                auto& e = sorted[k][i];
                std::printf("%s{\"value\":", i ? "," : "");
                // A number stays a number; NaN / Inf (not JSON) and anything
                // else a numeric column renders (a date) is a string.
                char* end = nullptr;
                double d = e.null ? 0 : std::strtod(e.value.c_str(), &end);
                if (e.null) std::printf("null");
                else if (numeric && !e.value.empty() && !*end && std::isfinite(d) &&
                         e.value.find_first_of("xXnN") == std::string::npos)
                    std::fputs(e.value.c_str(), stdout);
                else        json_emit_string(e.value);
                std::printf(",\"count\":%lld,\"fraction\":%s}",
                            (long long)e.count, frac_of(e.count).c_str());
            }
            std::printf("]}");
            if (cfg.json_lines) std::putchar('\n');
        }
        if (cfg.json_array) std::printf("]\n");
        return "";
    }

    // Text: the 50 most frequent values per column.
    constexpr size_t kTop = 50;
    for (size_t k = 0; k < cols.size(); ++k) {
        if (k) std::printf("\n");
        auto& entries = sorted[k];
        std::printf("%s%s%s — %s%zu%s distinct value(s) (of %s%lld%s)\n",
                    g_color.header, schema->field(cols[k])->name().c_str(),
                    g_color.reset,
                    g_color.number, entries.size(), g_color.reset,
                    g_color.number, (long long)total, g_color.reset);
        auto label = [](const Entry& e) { return e.null ? std::string("(null)") : e.value; };
        int wV = 5, wC2 = 5, wP = 4;
        size_t n_show = std::min(kTop, entries.size());
        for (size_t i = 0; i < n_show; ++i) {
            wV  = std::max(wV,  (int)display_width(label(entries[i])));
            wC2 = std::max(wC2, (int)display_width(
                digits_with_sep(std::to_string(entries[i].count))));
            wP  = std::max(wP, (int)pct_of(entries[i].count).size());
        }
        if (wV > 50) wV = 50;
        for (size_t i = 0; i < n_show; ++i) {
            std::string v = truncate(label(entries[i]), wV);
            std::printf("  %s%s  %*s  %*s\n", v.c_str(),
                        std::string(std::max(0, wV - (int)display_width(v)), ' ').c_str(),
                        wC2, digits_with_sep(std::to_string(entries[i].count)).c_str(),
                        wP, pct_of(entries[i].count).c_str());
        }
        if (entries.size() > n_show)
            std::printf("  %s... %zu more distinct value(s); --tsv lists all%s\n",
                        g_color.meta_key, entries.size() - n_show, g_color.reset);
    }
    return "";
}

// ── --sample N: reservoir sample N rows uniformly ────────────────────────────
//
// Reads (and optionally filters) the entire source into memory, then picks
// N rows uniformly without replacement via reservoir sampling. The result
// is wrapped as a MemoryTableSource so the normal output path renders it.
std::string build_sample(std::unique_ptr<TabularSource>& src,
                          const Config& cfg) {
    int N = cfg.sample_n;
    if (N <= 0) return "";

    // Read all data (loading every column so the user can still --select after).
    int n_fields = src->schema()->num_fields();
    std::vector<int> all_cols;
    for (int i = 0; i < n_fields; ++i) all_cols.push_back(i);

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src->schema(), &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }

    std::vector<std::shared_ptr<arrow::Table>> chunks;
    for (int c = 0; ; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src->read_chunk(c, all_cols, &tbl).ok()) continue;
        if (have_filter) tbl = apply_filter(tbl, fx, all_cols);
        if (tbl && tbl->num_rows() > 0) chunks.push_back(std::move(tbl));
    }
    // A stream that failed part-way is an error, not a shorter table.
    if (!src->read_status().ok())
        return shorten_reader_error(src->read_status().ToString());
    auto hidden_from_src = src->hidden_for_display();
    if (chunks.empty()) {
        // Empty result — still build an empty table.
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int i = 0; i < n_fields; ++i)
            cols.push_back(std::make_shared<arrow::ChunkedArray>(
                arrow::ArrayVector{}, src->schema()->field(i)->type()));
        auto empty = arrow::Table::Make(src->schema(), cols, 0);
        std::string old_path = src->path();
        src = std::make_unique<MemoryTableSource>(empty,
            "<sample of " + old_path + ">",
            "Sampled rows: 0 (no data after filter)",
            hidden_from_src);
        return "";
    }
    auto cat = arrow::ConcatenateTables(chunks);
    if (!cat.ok()) return "concat failed: " + cat.status().ToString();
    auto master = cat.ValueOrDie();
    int64_t M = master->num_rows();
    if (M <= N) {
        // Smaller than sample size; just keep everything.
        std::string old_path = src->path();
        src = std::make_unique<MemoryTableSource>(master,
            "<sample of " + old_path + ">",
            "Sampled rows: " + std::to_string(M) + " / " + std::to_string(M) +
            " (smaller than --sample N)",
            hidden_from_src);
        return "";
    }

    // Reservoir sample: collect N indices into [0, M).
    std::vector<int64_t> chosen(N);
    for (int i = 0; i < N; ++i) chosen[i] = i;
    std::mt19937_64 rng(std::random_device{}());
    for (int64_t i = N; i < M; ++i) {
        std::uniform_int_distribution<int64_t> dist(0, i);
        int64_t j = dist(rng);
        if (j < N) chosen[j] = i;
    }
    std::sort(chosen.begin(), chosen.end());

    // Build the sampled Table by slicing contiguous runs.
    std::vector<std::shared_ptr<arrow::Table>> runs;
    int64_t run_start = chosen[0], run_len = 1;
    for (size_t k = 1; k < chosen.size(); ++k) {
        if (chosen[k] == run_start + run_len) { ++run_len; continue; }
        runs.push_back(master->Slice(run_start, run_len));
        run_start = chosen[k];
        run_len   = 1;
    }
    runs.push_back(master->Slice(run_start, run_len));
    auto sampled_or = arrow::ConcatenateTables(runs);
    if (!sampled_or.ok()) return "concat failed: " + sampled_or.status().ToString();

    std::string old_path = src->path();
    src = std::make_unique<MemoryTableSource>(sampled_or.ValueOrDie(),
        "<sample of " + old_path + ">",
        "Sampled rows: " + std::to_string(N) + " / " +
            std::to_string(M) + " (uniform random)",
        hidden_from_src);
    return "";
}

// ── --tail N: keep the last N rows of the source ─────────────────────────────
//
// Mirrors build_sample's structure: read every chunk through any active
// --filter, slice off all but the last N rows, then wrap the result as a
// MemoryTableSource. Streaming sources (BAM, BCF, FASTX, …) are forced
// through a full scan; bounded sources (Parquet, Arrow IPC) do the same,
// but Arrow's chunk-cache means we don't re-decode.
std::string build_tail(std::unique_ptr<TabularSource>& src,
                        const Config& cfg) {
    int N = cfg.tail_rows;
    if (N <= 0) return "";

    int n_fields = src->schema()->num_fields();
    std::vector<int> all_cols;
    for (int i = 0; i < n_fields; ++i) all_cols.push_back(i);

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src->schema(), &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }

    std::vector<std::shared_ptr<arrow::Table>> chunks;
    for (int c = 0; ; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src->read_chunk(c, all_cols, &tbl).ok()) continue;
        if (have_filter) tbl = apply_filter(tbl, fx, all_cols);
        if (tbl && tbl->num_rows() > 0) chunks.push_back(std::move(tbl));
    }
    // A stream that failed part-way is an error, not a shorter table.
    if (!src->read_status().ok())
        return shorten_reader_error(src->read_status().ToString());
    auto hidden_from_src = src->hidden_for_display();
    if (chunks.empty()) {
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int i = 0; i < n_fields; ++i)
            cols.push_back(std::make_shared<arrow::ChunkedArray>(
                arrow::ArrayVector{}, src->schema()->field(i)->type()));
        auto empty = arrow::Table::Make(src->schema(), cols, 0);
        std::string old_path = src->path();
        src = std::make_unique<MemoryTableSource>(empty,
            "<tail of " + old_path + ">",
            "Tail rows: 0 (no data)",
            hidden_from_src);
        return "";
    }
    auto cat = arrow::ConcatenateTables(chunks);
    if (!cat.ok()) return "concat failed: " + cat.status().ToString();
    auto master = cat.ValueOrDie();
    int64_t M = master->num_rows();
    int64_t take = std::min<int64_t>(N, M);
    auto tail = master->Slice(M - take, take);

    std::string old_path = src->path();
    src = std::make_unique<MemoryTableSource>(tail,
        "<tail of " + old_path + ">",
        "Tail rows: " + std::to_string(take) + " / " + std::to_string(M),
        hidden_from_src);
    return "";
}

// Row permutation that stable-sorts one column's values (see vvcore.hpp).
// Numeric columns compare numerically, others by rendered text; nulls last;
// ties keep input order. Hand-rolled because arrow::compute's sort kernels are
// GC'd from the static build / not reliably registered — shared by --sort and
// the GUI's click-to-sort so their semantics stay identical.
std::vector<int64_t> stable_sort_order(const arrow::Array& key, bool descending) {
    const int64_t N = key.length();
    std::vector<int64_t> order((size_t)N);
    std::iota(order.begin(), order.end(), (int64_t)0);
    if (is_numeric_type(key.type_id())) {
        std::vector<double> kv((size_t)N);
        std::vector<char>   kn((size_t)N);
        for (int64_t i = 0; i < N; ++i) {
            double d = 0;
            if (key.IsNull(i)) kn[(size_t)i] = 1;
            else if (array_value_as_double(key, i, &d)) { kn[(size_t)i]=0; kv[(size_t)i]=d; }
            else kn[(size_t)i] = 1;
        }
        std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) -> bool {
            if (kn[(size_t)a] != kn[(size_t)b]) return !kn[(size_t)a];  // non-null first
            if (kn[(size_t)a]) return false;
            return descending ? kv[(size_t)a] > kv[(size_t)b] : kv[(size_t)a] < kv[(size_t)b];
        });
    } else {
        std::vector<std::string> kv((size_t)N);
        std::vector<char>        kn((size_t)N);
        for (int64_t i = 0; i < N; ++i) {
            if (key.IsNull(i)) kn[(size_t)i] = 1;
            else kv[(size_t)i] = cell_to_string(key, i);
        }
        std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) -> bool {
            if (kn[(size_t)a] != kn[(size_t)b]) return !kn[(size_t)a];  // non-null first
            if (kn[(size_t)a]) return false;
            return descending ? kv[(size_t)a] > kv[(size_t)b] : kv[(size_t)a] < kv[(size_t)b];
        });
    }
    return order;
}

// Rows `idx` of `a`, in that order, via the builder's AppendArraySlice (a plain
// builder method, not a compute kernel, so it survives --gc-sections). Arrow
// has no builder for an extension type: gather its storage and re-wrap it.
static arrow::Result<std::shared_ptr<arrow::Array>>
gather_rows(const std::shared_ptr<arrow::Array>& a, const std::vector<int64_t>& idx) {
    if (a->type_id() == arrow::Type::EXTENSION) {
        auto& ea = static_cast<const arrow::ExtensionArray&>(*a);
        ARROW_ASSIGN_OR_RAISE(auto st, gather_rows(ea.storage(), idx));
        return arrow::ExtensionType::WrapArray(a->type(), st);
    }
    std::unique_ptr<arrow::ArrayBuilder> b;
    ARROW_RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(), a->type(), &b));
    if (!idx.empty()) ARROW_RETURN_NOT_OK(b->Reserve((int64_t)idx.size()));
    arrow::ArraySpan span(*a->data());
    for (int64_t i : idx) ARROW_RETURN_NOT_OK(b->AppendArraySlice(span, i, 1));
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(b->Finish(&out));
    return out;
}

// --sort COL[:asc|:desc]: fully materialise the (filtered) source, stable-sort
// its rows by one column, and replace `src` with a MemoryTableSource so every
// downstream view / export renders the sorted result identically. Sorting needs
// the whole table in memory (a viewer convenience; for ordering huge files, a
// query engine is the right tool). Numeric columns sort numerically, others by
// their rendered text, matching the interactive `s` sort; nulls sort last and
// ties keep input order (stable). Arrow's compute kernels are GC'd from the
// static build, so the gather is done by hand with AppendArraySlice.
std::string build_sort(std::unique_ptr<TabularSource>& src,
                        const Config& cfg) {
    auto schema = src->schema();
    int n_fields = schema->num_fields();
    int sort_idx = -1;
    for (int i = 0; i < n_fields; ++i)
        if (schema->field(i)->name() == cfg.sort_col) { sort_idx = i; break; }
    if (sort_idx < 0) return "--sort: unknown column '" + cfg.sort_col + "'";

    std::vector<int> all_cols;
    for (int i = 0; i < n_fields; ++i) all_cols.push_back(i);

    FilterExpr fx; bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<std::shared_ptr<arrow::Table>> chunks;
    for (int c = 0; ; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src->read_chunk(c, all_cols, &tbl).ok()) continue;
        if (have_filter) tbl = apply_filter(tbl, fx, all_cols);
        if (tbl && tbl->num_rows() > 0) chunks.push_back(std::move(tbl));
    }
    // A stream that failed part-way is an error, not a shorter table.
    if (!src->read_status().ok())
        return shorten_reader_error(src->read_status().ToString());
    auto hidden_from_src = src->hidden_for_display();
    std::string old_path = src->path();

    std::shared_ptr<arrow::Table> master;
    if (chunks.empty()) {
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int i = 0; i < n_fields; ++i)
            cols.push_back(std::make_shared<arrow::ChunkedArray>(
                arrow::ArrayVector{}, schema->field(i)->type()));
        master = arrow::Table::Make(schema, cols, 0);
    } else {
        auto cat = arrow::ConcatenateTables(chunks);
        if (!cat.ok()) return "--sort: concat failed: " + cat.status().ToString();
        master = cat.ValueOrDie();
    }
    int64_t N = master->num_rows();

    // Flatten each column to one contiguous array (for the key and the gather).
    std::vector<std::shared_ptr<arrow::Array>> flat(n_fields);
    for (int c = 0; c < n_fields; ++c) {
        auto ch = master->column(c);
        if (ch->num_chunks() == 1) { flat[c] = ch->chunk(0); continue; }
        if (ch->num_chunks() == 0) {
            flat[c] = arrow::MakeArrayOfNull(schema->field(c)->type(), 0).ValueOrDie();
            continue;
        }
        auto cc = arrow::Concatenate(ch->chunks());
        if (!cc.ok()) return "--sort: concat column failed: " + cc.status().ToString();
        flat[c] = cc.ValueOrDie();
    }

    std::vector<int64_t> order = stable_sort_order(*flat[sort_idx], cfg.sort_desc);

    // Gather every column into the sorted order.
    arrow::ArrayVector sorted_cols((size_t)n_fields);
    for (int c = 0; c < n_fields; ++c) {
        auto g = gather_rows(flat[c], order);
        if (!g.ok()) return "--sort: gather failed: " + g.status().ToString();
        sorted_cols[(size_t)c] = *g;
    }
    auto sorted = arrow::Table::Make(schema, sorted_cols, N);

    src = std::make_unique<MemoryTableSource>(sorted,
        "<sorted " + old_path + ">",
        "Sorted by " + cfg.sort_col + (cfg.sort_desc ? " (desc)" : "") +
        "  |  Rows: " + std::to_string(N),
        hidden_from_src);
    return "";
}

// --distinct: SQL SELECT DISTINCT — drop duplicate rows, keeping the first
// occurrence, over the columns that would be shown (honouring --select and
// --filter). So `--select chrom --distinct` lists distinct chromosomes, and a
// bare `--distinct` deduplicates whole displayed rows. Materialises like --sort
// (a viewer convenience; a query engine is the tool for deduping huge files);
// the gather uses AppendArraySlice, not a compute kernel. Produces a
// MemoryTableSource of the projected, deduplicated rows and clears --select /
// --filter, which it has already applied.
std::string build_distinct(std::unique_ptr<TabularSource>& src,
                           Config& cfg) {
    ExactFloats exact;   // the row key must not merge floats by rounding
    // The columns to compare on (and to keep) are the ones the output would
    // show: --select if given, else the visible set.
    std::vector<std::string> unknown;
    std::vector<int> proj = select_field_indices(*src, cfg, &unknown, false);
    if (!unknown.empty())
        return "--select: unknown column '" + unknown[0] + "'";
    if (proj.empty())
        return "--distinct: no columns to compare";

    auto src_schema = src->schema();
    int n_fields = src_schema->num_fields();
    arrow::FieldVector pf;
    for (int i : proj) pf.push_back(src_schema->field(i));
    auto proj_schema = arrow::schema(pf);

    FilterExpr fx; bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src_schema, &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }

    std::vector<int> all_cols;
    for (int i = 0; i < n_fields; ++i) all_cols.push_back(i);

    std::vector<std::shared_ptr<arrow::Table>> chunks;
    for (int c = 0; ; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src->read_chunk(c, all_cols, &tbl).ok()) continue;
        if (have_filter) tbl = apply_filter(tbl, fx, all_cols);
        if (tbl && tbl->num_rows() > 0) chunks.push_back(std::move(tbl));
    }
    std::string old_path = src->path();

    std::shared_ptr<arrow::Table> master;
    if (chunks.empty()) {
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int i = 0; i < n_fields; ++i)
            cols.push_back(std::make_shared<arrow::ChunkedArray>(
                arrow::ArrayVector{}, src_schema->field(i)->type()));
        master = arrow::Table::Make(src_schema, cols, 0);
    } else {
        auto cat = arrow::ConcatenateTables(chunks);
        if (!cat.ok()) return "--distinct: concat failed: " + cat.status().ToString();
        master = cat.ValueOrDie();
    }
    int64_t N = master->num_rows();

    // Flatten just the projected columns (for the key and the gather).
    const int np = (int)proj.size();
    std::vector<std::shared_ptr<arrow::Array>> flat((size_t)np);
    for (int k = 0; k < np; ++k) {
        auto ch = master->column(proj[k]);
        if (ch->num_chunks() == 1) flat[(size_t)k] = ch->chunk(0);
        else if (ch->num_chunks() == 0)
            flat[(size_t)k] = arrow::MakeArrayOfNull(
                src_schema->field(proj[k])->type(), 0).ValueOrDie();
        else {
            auto cc = arrow::Concatenate(ch->chunks());
            if (!cc.ok()) return "--distinct: concat column failed: " + cc.status().ToString();
            flat[(size_t)k] = cc.ValueOrDie();
        }
    }

    // Keep the first occurrence of each distinct projected row. The key joins
    // each cell's rendered text with a null marker and a unit separator that a
    // value can't contain, so distinct rows never collide.
    std::vector<int64_t> keep;
    keep.reserve((size_t)N);
    std::unordered_set<std::string> seen;
    seen.reserve((size_t)N * 2 + 1);
    std::string key;
    for (int64_t i = 0; i < N; ++i) {
        key.clear();
        for (int k = 0; k < np; ++k) {
            const arrow::Array& a = *flat[(size_t)k];
            if (a.IsNull(i)) key.push_back('\x00');
            else { key.push_back('\x01'); key += cell_to_string(a, i); }
            key.push_back('\x1f');   // unit separator between columns
        }
        if (seen.insert(key).second) keep.push_back(i);
    }
    int64_t M = (int64_t)keep.size();

    // Gather the kept rows for each projected column.
    arrow::ArrayVector out_cols((size_t)np);
    for (int k = 0; k < np; ++k) {
        auto g = gather_rows(flat[(size_t)k], keep);
        if (!g.ok()) return "--distinct: gather failed: " + g.status().ToString();
        out_cols[(size_t)k] = *g;
    }
    auto distinct = arrow::Table::Make(proj_schema, out_cols, M);

    src = std::make_unique<MemoryTableSource>(distinct,
        "<distinct " + old_path + ">",
        "Distinct rows: " + std::to_string(M) + " / " + std::to_string(N));
    // --select and --filter are now baked into the deduplicated table.
    cfg.select_cols.clear();
    cfg.filter_expr.clear();
    return "";
}
