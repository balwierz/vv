// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// --validate, --describe, --unique, --sample, --tail, --sort, --distinct.

#include "internal.hpp"

#include <deque>

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

// Print "Column | Type | Count | Nulls | Min | Max | Mean | Std | percentiles |
// Distinct" from summarize_columns. `Mean` / `Std` / percentiles only for
// numeric columns; `Distinct` only for the others, capped at 16.
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
    // --describe summarises the WHOLE table by default (Config::head_rows
    // defaults to 10, the table-view preview size — but a describe over only
    // the first 10 rows would be misleading). Honour -n only when the user set
    // it explicitly, matching the --json/--md/--parquet output paths.
    SummaryOptions opt;
    opt.cols        = requested;
    opt.filter      = have_filter ? &fx : nullptr;
    opt.max_rows    = (!cfg.head_rows_set || cfg.head_rows <= 0) ? -1 : (int64_t)cfg.head_rows;
    opt.percentiles = cfg.percentiles;
    std::vector<ColumnSummary> stats;
    if (auto err = summarize_columns(src, opt, &stats); !err.empty()) return err;
    int64_t n_numeric = 0;
    for (auto& cs : stats) n_numeric += cs.numeric;

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
                        cs.numeric ? "true" : "false",
                        (long long)cs.count, (long long)cs.nulls);
            if (cs.count == 0) {
                std::printf(",\"min\":null,\"max\":null");
                if (cs.numeric) std::printf(",\"mean\":null,\"sum\":0");
            } else if (cs.numeric) {
                std::printf(",\"min\":%s,\"max\":%s,\"mean\":%s,\"sum\":%s",
                            jnum(cs.min).c_str(), jnum(cs.max).c_str(),
                            jnum(cs.mean).c_str(), jnum(cs.sum).c_str());
            } else {
                std::printf(",\"min\":"); json_emit_string(cs.s_min);
                std::printf(",\"max\":"); json_emit_string(cs.s_max);
            }
            if (cs.numeric) {
                std::printf(",\"std\":%s", cs.has_std ? jnum(cs.std).c_str() : "null");
                if (!cfg.percentiles.empty()) {
                    std::printf(",\"percentiles\":{");
                    for (size_t i = 0; i < cfg.percentiles.size(); ++i) {
                        if (i) std::putchar(',');
                        json_emit_string(pct_label(cfg.percentiles[i]));
                        std::printf(":%s", cs.percentiles.empty() ? "null"
                                                : jnum(cs.percentiles[i]).c_str());
                    }
                    std::printf("},\"percentiles_sampled\":%s",
                                cs.percentiles_sampled ? "true" : "false");
                }
            }
            if (!cs.numeric) {
                if (cs.distinct < 0)
                    std::printf(",\"distinct\":null,\"distinct_overflow\":true");
                else
                    std::printf(",\"distinct\":%lld", (long long)cs.distinct);
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
        const auto dtype = src.schema()->field(requested[k])->type();
        const bool temporal = cs.temporal;
        auto fmt = [&](double v) { return temporal ? format_temporal_value(v, dtype) : fmt_num(v); };
        std::string mn, mx, me;
        if (cs.count == 0) {
            mn = "-"; mx = "-"; me = "-";
        } else if (cs.numeric) {
            mn = fmt(cs.min);
            mx = fmt(cs.max);
            me = fmt(cs.mean);
        } else {
            mn = cs.s_min; mx = cs.s_max; me = "";
        }
        std::vector<std::string> r = {cs.name, cs.type, std::to_string(cs.count),
                                      std::to_string(cs.nulls), mn, mx, me};
        if (num_cols) {
            if (!cs.numeric || temporal) r.push_back("");
            else if (!cs.has_std) r.push_back("-");
            else r.push_back(std::isnan(cs.std) ? "nan" : fmt_num(cs.std));   // not "-nan"
            for (size_t i = 0; i < cfg.percentiles.size(); ++i) {
                if (!cs.numeric)                r.push_back("");
                else if (cs.percentiles.empty()) r.push_back("-");
                else r.push_back((cs.percentiles_sampled ? "~" : "") + fmt(cs.percentiles[i]));
            }
        }
        r.push_back(cs.numeric ? ""
                    : (cs.distinct < 0 ? ">16" : std::to_string(cs.distinct)));
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
    SummaryOptions opt;
    opt.cols         = cols;
    opt.filter       = have_filter ? &fx : nullptr;
    opt.percentiles  = {};
    opt.distinct_cap = SIZE_MAX;
    opt.value_counts = true;
    std::vector<ColumnSummary> sums;
    if (auto err = summarize_columns(src, opt, &sums); !err.empty()) return err;
    // Every column saw the same rows.
    const int64_t total = sums.empty() ? 0 : sums[0].count + sums[0].nulls;
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
            for (auto& e : sums[k].values) {
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
                        (long long)total, sums[k].values.size());
            const bool numeric = is_numeric_type(schema->field(cols[k])->type()->id());
            for (size_t i = 0; i < sums[k].values.size(); ++i) {
                auto& e = sums[k].values[i];
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
        auto& entries = sums[k].values;
        std::printf("%s%s%s — %s%zu%s distinct value(s) (of %s%lld%s)\n",
                    g_color.header, schema->field(cols[k])->name().c_str(),
                    g_color.reset,
                    g_color.number, entries.size(), g_color.reset,
                    g_color.number, (long long)total, g_color.reset);
        auto label = [](const ValueCount& e) { return e.null ? std::string("(null)") : e.value; };
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

// ── --sample / --tail / --sort / --distinct ──────────────────────────────────
// Each reads the (filtered) source and replaces `src` with a MemoryTableSource
// of the result, so every view and export renders it alike. They keep what the
// result needs, not the whole source: the rows sampled so far, the last
// chunks, the distinct rows; --sort, which needs every row, keeps only the
// columns it outputs. Gathers use the builders' AppendArraySlice, a plain
// builder method rather than a compute kernel (those are GC'd from the static
// build).

static std::shared_ptr<arrow::Table> empty_table(const std::shared_ptr<arrow::Schema>& schema) {
    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    for (const auto& f : schema->fields())
        cols.push_back(std::make_shared<arrow::ChunkedArray>(arrow::ArrayVector{}, f->type()));
    return arrow::Table::Make(schema, cols, 0);
}

// Rows `idx` of the concatenation of `ca`'s chunks, in that order. Arrow has no
// builder for an extension type: gather its storage and re-wrap it.
static arrow::Result<std::shared_ptr<arrow::Array>>
gather_chunked(const arrow::ChunkedArray& ca, const std::vector<int64_t>& idx) {
    const auto& type = ca.type();
    if (type->id() == arrow::Type::EXTENSION) {
        arrow::ArrayVector storage;
        for (const auto& ch : ca.chunks())
            storage.push_back(static_cast<const arrow::ExtensionArray&>(*ch).storage());
        const arrow::ChunkedArray st(std::move(storage),
            static_cast<const arrow::ExtensionType&>(*type).storage_type());
        ARROW_ASSIGN_OR_RAISE(auto g, gather_chunked(st, idx));
        return arrow::ExtensionType::WrapArray(type, g);
    }
    std::unique_ptr<arrow::ArrayBuilder> b;
    ARROW_RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(), type, &b));
    if (!idx.empty()) ARROW_RETURN_NOT_OK(b->Reserve((int64_t)idx.size()));
    std::vector<int64_t> starts;
    std::vector<arrow::ArraySpan> spans;
    int64_t at = 0;
    for (const auto& ch : ca.chunks()) {
        starts.push_back(at);
        spans.emplace_back(*ch->data());
        at += ch->length();
    }
    for (int64_t i : idx) {
        const size_t c = spans.size() == 1 ? 0
            : (size_t)(std::upper_bound(starts.begin(), starts.end(), i) - starts.begin()) - 1;
        ARROW_RETURN_NOT_OK(b->AppendArraySlice(spans[c], i - starts[c], 1));
    }
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(b->Finish(&out));
    return out;
}

// Rows `idx` of every column of `t`.
static arrow::Result<std::shared_ptr<arrow::Table>>
gather_table(const arrow::Table& t, const std::vector<int64_t>& idx) {
    arrow::ArrayVector cols;
    for (int c = 0; c < t.num_columns(); ++c) {
        ARROW_ASSIGN_OR_RAISE(auto g, gather_chunked(*t.column(c), idx));
        cols.push_back(std::move(g));
    }
    return arrow::Table::Make(t.schema(), cols, (int64_t)idx.size());
}

// Chunk `c` of `src`, columns `cols`, through `fx` when `have_filter`; null
// when it cannot be read or nothing is left.
static std::shared_ptr<arrow::Table> read_filtered(TabularSource& src, int c,
                                                   const std::vector<int>& cols,
                                                   const FilterExpr& fx, bool have_filter) {
    std::shared_ptr<arrow::Table> tbl;
    if (!src.read_chunk(c, cols, &tbl).ok() || !tbl) return nullptr;
    if (have_filter) tbl = apply_filter(tbl, fx, cols);
    if (!tbl || tbl->num_rows() == 0) return nullptr;
    return tbl;
}

// --sample N: N rows chosen uniformly without replacement (reservoir
// sampling, Algorithm R, over the rows the filter keeps), in source order.
// The rows in the sample are copied out of each chunk as it is read, so
// memory holds the sample, not the source.
std::string build_sample(std::unique_ptr<TabularSource>& src,
                          const Config& cfg) {
    int N = cfg.sample_n;
    if (N <= 0) return "";

    // Every column, so the user can still --select after.
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

    // Each sampled row: its index among the kept rows, the piece holding a
    // copy of it, and its row there (piece -1: still in the current chunk).
    struct Pick { int64_t idx; int piece; int64_t row; };
    std::vector<Pick> res;
    res.reserve((size_t)N);
    std::vector<std::shared_ptr<arrow::Table>> pieces;
    int64_t piece_rows = 0;
    std::mt19937_64 rng(std::random_device{}());
    int64_t M = 0;
    for (int c = 0; ; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        auto tbl = read_filtered(*src, c, all_cols, fx, have_filter);
        if (!tbl) continue;
        for (int64_t r = 0; r < tbl->num_rows(); ++r, ++M) {
            if (M < N) { res.push_back({M, -1, r}); continue; }
            std::uniform_int_distribution<int64_t> dist(0, M);
            const int64_t j = dist(rng);
            if (j < N) res[(size_t)j] = {M, -1, r};
        }
        // Copy this chunk's rows that are in the sample now.
        std::vector<size_t> slots;
        for (size_t k = 0; k < res.size(); ++k)
            if (res[k].piece < 0) slots.push_back(k);
        if (slots.empty()) continue;
        std::sort(slots.begin(), slots.end(),
                  [&](size_t x, size_t y) { return res[x].row < res[y].row; });
        std::vector<int64_t> rows;
        for (size_t k : slots) rows.push_back(res[k].row);
        auto piece = gather_table(*tbl, rows);
        if (!piece.ok()) return "--sample: gather failed: " + piece.status().ToString();
        for (size_t i = 0; i < slots.size(); ++i)
            res[slots[i]] = {res[slots[i]].idx, (int)pieces.size(), (int64_t)i};
        pieces.push_back(*piece);
        piece_rows += (int64_t)rows.size();
        // Rows replaced since keep their piece alive: compact when the
        // pieces hold N + 4096 more rows than the sample (about ln(M / N)
        // times in all).
        if (piece_rows > 2 * (int64_t)N + 4096) {
            auto all = arrow::ConcatenateTables(pieces);
            if (!all.ok()) return "concat failed: " + all.status().ToString();
            std::vector<int64_t> offs(pieces.size(), 0);
            for (size_t i = 1; i < pieces.size(); ++i)
                offs[i] = offs[i - 1] + pieces[i - 1]->num_rows();
            std::vector<int64_t> live;
            for (auto& p : res) live.push_back(offs[(size_t)p.piece] + p.row);
            auto one = gather_table(**all, live);
            if (!one.ok()) return "--sample: gather failed: " + one.status().ToString();
            for (size_t k = 0; k < res.size(); ++k) res[k] = {res[k].idx, 0, (int64_t)k};
            pieces.assign(1, *one);
            piece_rows = (int64_t)res.size();
        }
    }
    // A stream that failed part-way is an error, not a shorter table.
    if (!src->read_status().ok())
        return shorten_reader_error(src->read_status().ToString());
    auto hidden_from_src = src->hidden_for_display();
    std::string old_path = src->path();
    if (M == 0) {
        src = std::make_unique<MemoryTableSource>(empty_table(src->schema()),
            "<sample of " + old_path + ">",
            "Sampled rows: 0 (no data after filter)",
            hidden_from_src);
        return "";
    }
    // The sample in source order.
    std::sort(res.begin(), res.end(), [](const Pick& x, const Pick& y) { return x.idx < y.idx; });
    auto all = arrow::ConcatenateTables(pieces);
    if (!all.ok()) return "concat failed: " + all.status().ToString();
    std::vector<int64_t> offs(pieces.size(), 0);
    for (size_t i = 1; i < pieces.size(); ++i) offs[i] = offs[i - 1] + pieces[i - 1]->num_rows();
    std::vector<int64_t> rows;
    for (auto& p : res) rows.push_back(offs[(size_t)p.piece] + p.row);
    auto sampled = gather_table(**all, rows);
    if (!sampled.ok()) return "--sample: gather failed: " + sampled.status().ToString();
    src = std::make_unique<MemoryTableSource>(*sampled,
        "<sample of " + old_path + ">",
        M <= N ? "Sampled rows: " + std::to_string(M) + " / " + std::to_string(M) +
                 " (smaller than --sample N)"
               : "Sampled rows: " + std::to_string(N) + " / " + std::to_string(M) +
                 " (uniform random)",
        hidden_from_src);
    return "";
}

// --tail N: the last N rows the filter keeps. A random-access source (Parquet,
// ORC) reads only its trailing chunks in full and the rest just to count
// them (with --filter, only the filter's columns); a stream keeps the last
// chunks that cover N rows as it goes.
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

    std::deque<std::shared_ptr<arrow::Table>> kept;
    int64_t kept_rows = 0, M = 0;   // M: every row the filter keeps
    if (src->random_access()) {
        int c = src->num_chunks() - 1;
        for (; c >= 0 && kept_rows < N; --c) {
            auto tbl = read_filtered(*src, c, all_cols, fx, have_filter);
            if (!tbl) continue;
            kept_rows += tbl->num_rows();
            kept.push_front(std::move(tbl));
        }
        M = kept_rows;
        const std::vector<int> fcols = union_with_filter({}, fx);
        for (int i = 0; i <= c; ++i) {
            if (!have_filter) { M += src->chunk_meta(i).num_rows; continue; }
            std::shared_ptr<arrow::Table> t;
            if (src->read_chunk(i, fcols, &t).ok() && t) M += count_filter_matches(*t, fx, fcols);
        }
    } else {
        for (int c = 0; ; ++c) {
            src->ensure(c);
            if (c >= src->num_chunks()) break;
            auto tbl = read_filtered(*src, c, all_cols, fx, have_filter);
            if (!tbl) continue;
            kept_rows += tbl->num_rows();
            M += tbl->num_rows();
            kept.push_back(std::move(tbl));
            while (kept_rows - kept.front()->num_rows() >= N) {
                kept_rows -= kept.front()->num_rows();
                kept.pop_front();
            }
        }
    }
    // A stream that failed part-way is an error, not a shorter table.
    if (!src->read_status().ok())
        return shorten_reader_error(src->read_status().ToString());
    auto hidden_from_src = src->hidden_for_display();
    std::string old_path = src->path();
    if (kept.empty()) {
        src = std::make_unique<MemoryTableSource>(empty_table(src->schema()),
            "<tail of " + old_path + ">",
            "Tail rows: 0 (no data)",
            hidden_from_src);
        return "";
    }
    auto cat = arrow::ConcatenateTables(std::vector<std::shared_ptr<arrow::Table>>(kept.begin(), kept.end()));
    if (!cat.ok()) return "concat failed: " + cat.status().ToString();
    auto master = cat.ValueOrDie();
    int64_t take = std::min<int64_t>(N, kept_rows);
    auto tail = master->Slice(kept_rows - take, take);

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
    std::vector<char> kn((size_t)N);
    auto sort_by = [&](const auto& kv) {
        std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) -> bool {
            if (kn[(size_t)a] != kn[(size_t)b]) return !kn[(size_t)a];  // non-null first
            if (kn[(size_t)a]) return false;
            return descending ? kv[(size_t)a] > kv[(size_t)b] : kv[(size_t)a] < kv[(size_t)b];
        });
    };
    if (is_numeric_type(key.type_id())) {
        std::vector<double> kv((size_t)N);
        for (int64_t i = 0; i < N; ++i) {
            double d = 0;
            if (key.IsNull(i)) kn[(size_t)i] = 1;
            else if (array_value_as_double(key, i, &d)) { kn[(size_t)i]=0; kv[(size_t)i]=d; }
            else kn[(size_t)i] = 1;
        }
        sort_by(kv);
    } else if (key.type_id() == arrow::Type::STRING || key.type_id() == arrow::Type::LARGE_STRING) {
        // The rendered text of a string is the string: compare views into the
        // array instead of a copy per row.
        std::vector<std::string_view> kv((size_t)N);
        const bool large = key.type_id() == arrow::Type::LARGE_STRING;
        for (int64_t i = 0; i < N; ++i) {
            if (key.IsNull(i)) { kn[(size_t)i] = 1; continue; }
            kv[(size_t)i] = large ? static_cast<const arrow::LargeStringArray&>(key).GetView(i)
                                  : static_cast<const arrow::StringArray&>(key).GetView(i);
        }
        sort_by(kv);
    } else {
        std::vector<std::string> kv((size_t)N);
        for (int64_t i = 0; i < N; ++i) {
            if (key.IsNull(i)) kn[(size_t)i] = 1;
            else kv[(size_t)i] = cell_to_string(key, i);
        }
        sort_by(kv);
    }
    return order;
}

bool sort_rows_by_column(TabularSource& src, int col, bool descending,
                         const std::vector<int64_t>* subset,
                         const std::atomic<bool>* cancel, std::vector<int64_t>* out) {
    out->clear();
    src.set_retain_all(true);
    arrow::ArrayVector keys;
    std::vector<int64_t> src_row;   // the source row of each key position
    for (int c = 0; ; ++c) {
        if (cancel && cancel->load()) return false;
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> t;
        if (!src.read_chunk(c, {col}, &t).ok() || !t || t->num_columns() == 0) continue;
        const int64_t first = src.chunk_meta(c).first_row;
        for (int64_t i = 0; i < t->num_rows(); ++i) src_row.push_back(first + i);
        for (const auto& ch : t->column(0)->chunks()) keys.push_back(ch);
    }
    if (src_row.empty()) return true;
    std::shared_ptr<arrow::Array> flat;
    if (keys.size() == 1) flat = keys[0];
    else {
        auto cc = arrow::Concatenate(keys);
        if (!cc.ok()) return true;
        flat = *cc;
    }
    keys.clear();
    if (cancel && cancel->load()) return false;
    std::vector<int64_t> order = stable_sort_order(*flat, descending);
    flat.reset();
    if (cancel && cancel->load()) return false;
    std::vector<char> keep;
    if (subset) {
        keep.assign((size_t)(src_row.back() + 1), 0);
        for (int64_t r : *subset)
            if (r >= 0 && r < (int64_t)keep.size()) keep[(size_t)r] = 1;
    }
    out->reserve(subset ? subset->size() : order.size());
    for (int64_t p : order) {
        const int64_t r = src_row[(size_t)p];
        if (!subset || keep[(size_t)r]) out->push_back(r);
    }
    return true;
}

// --sort COL[:asc|:desc]: stable-sort the (filtered) rows by one column and
// replace `src` with a MemoryTableSource so every downstream view / export
// renders the sorted result identically. Every row is held (a viewer
// convenience; for ordering huge files, a query engine is the right tool),
// but with `project` (an export, where --select names what is written) only
// the selected columns and the sort column, and each column is gathered
// straight from the chunks read and released once gathered. Numeric columns
// sort numerically, others by their rendered text, matching the interactive
// `s` sort; nulls sort last and ties keep input order (stable).
std::string build_sort(std::unique_ptr<TabularSource>& src,
                        const Config& cfg, bool project) {
    auto schema = src->schema();
    int n_fields = schema->num_fields();
    int sort_idx = -1;
    for (int i = 0; i < n_fields; ++i)
        if (schema->field(i)->name() == cfg.sort_col) { sort_idx = i; break; }
    if (sort_idx < 0) return "--sort: unknown column '" + cfg.sort_col + "'";

    FilterExpr fx; bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }

    // The columns of the result, in schema order.
    std::vector<int> out_cols;
    if (project && !cfg.select_cols.empty()) {
        std::vector<std::string> unknown;
        std::vector<int> sel = select_field_indices(*src, cfg, &unknown, true);
        if (unknown.empty()) {   // else keep every column; the writer reports it
            std::vector<char> want((size_t)n_fields, 0);
            for (int i : sel) want[(size_t)i] = 1;
            want[(size_t)sort_idx] = 1;
            for (int i = 0; i < n_fields; ++i) if (want[(size_t)i]) out_cols.push_back(i);
        }
    }
    const bool all = out_cols.empty();
    if (all) for (int i = 0; i < n_fields; ++i) out_cols.push_back(i);
    const std::vector<int> read_set = have_filter ? union_with_filter(out_cols, fx) : out_cols;
    std::vector<int> pos(out_cols.size());
    for (size_t k = 0; k < out_cols.size(); ++k)
        pos[k] = (int)(std::find(read_set.begin(), read_set.end(), out_cols[k]) - read_set.begin());
    size_t key_k = 0;
    while (out_cols[key_k] != sort_idx) ++key_k;

    // Each result column's chunks, as read.
    std::vector<arrow::ArrayVector> cols(out_cols.size());
    int64_t N = 0;
    for (int c = 0; ; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        auto tbl = read_filtered(*src, c, read_set, fx, have_filter);
        if (!tbl) continue;
        N += tbl->num_rows();
        for (size_t k = 0; k < out_cols.size(); ++k)
            for (const auto& ch : tbl->column(pos[k])->chunks()) cols[k].push_back(ch);
    }
    // A stream that failed part-way is an error, not a shorter table.
    if (!src->read_status().ok())
        return shorten_reader_error(src->read_status().ToString());
    auto hidden_from_src = src->hidden_for_display();
    std::string old_path = src->path();

    std::shared_ptr<arrow::Schema> out_schema = schema;
    if (!all) {
        arrow::FieldVector fields;
        for (int i : out_cols) fields.push_back(schema->field(i));
        out_schema = arrow::schema(fields, schema->metadata());
    }

    std::vector<int64_t> order;
    {
        const arrow::ChunkedArray key(cols[key_k], schema->field(sort_idx)->type());
        std::shared_ptr<arrow::Array> flat;
        if (key.num_chunks() == 1) flat = key.chunk(0);
        else if (key.num_chunks() == 0)
            flat = arrow::MakeArrayOfNull(key.type(), 0).ValueOrDie();
        else {
            auto cc = arrow::Concatenate(key.chunks());
            if (!cc.ok()) return "--sort: concat column failed: " + cc.status().ToString();
            flat = cc.ValueOrDie();
        }
        order = stable_sort_order(*flat, cfg.sort_desc);
    }

    // Gather each column into the sorted order, releasing what was read.
    arrow::ArrayVector sorted_cols(out_cols.size());
    for (size_t k = 0; k < out_cols.size(); ++k) {
        const arrow::ChunkedArray ca(std::move(cols[k]), out_schema->field((int)k)->type());
        cols[k].clear();
        auto g = gather_chunked(ca, order);
        if (!g.ok()) return "--sort: gather failed: " + g.status().ToString();
        sorted_cols[k] = *g;
    }
    auto sorted = arrow::Table::Make(out_schema, sorted_cols, N);

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
// bare `--distinct` deduplicates whole displayed rows. Reads the shown and
// filter columns chunk by chunk and keeps the distinct rows and their keys.
// Produces a MemoryTableSource of the projected, deduplicated rows and clears
// --select / --filter, which it has already applied.
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
    const std::vector<int> read_set = have_filter ? union_with_filter(proj, fx) : proj;
    const int np = (int)proj.size();
    std::vector<int> pos((size_t)np);
    for (int k = 0; k < np; ++k)
        pos[(size_t)k] = (int)(std::find(read_set.begin(), read_set.end(), proj[(size_t)k]) - read_set.begin());

    // Keep the first occurrence of each distinct projected row. The key joins
    // each cell's rendered text with a null marker and a unit separator that a
    // value can't contain, so distinct rows never collide.
    std::unordered_set<std::string> seen;
    std::string key;
    std::vector<std::shared_ptr<arrow::Table>> pieces;
    int64_t N = 0, M = 0;
    for (int c = 0; ; ++c) {
        src->ensure(c);
        if (c >= src->num_chunks()) break;
        auto tbl = read_filtered(*src, c, read_set, fx, have_filter);
        if (!tbl) continue;
        const int64_t n = tbl->num_rows();
        N += n;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> pcols((size_t)np);
        std::vector<std::shared_ptr<arrow::Array>> flat((size_t)np);
        for (int k = 0; k < np; ++k) {
            pcols[(size_t)k] = tbl->column(pos[(size_t)k]);
            if (pcols[(size_t)k]->num_chunks() == 1) { flat[(size_t)k] = pcols[(size_t)k]->chunk(0); continue; }
            auto cc = arrow::Concatenate(pcols[(size_t)k]->chunks());
            if (!cc.ok()) return "--distinct: concat column failed: " + cc.status().ToString();
            flat[(size_t)k] = cc.ValueOrDie();
        }
        std::vector<int64_t> keep;
        for (int64_t i = 0; i < n; ++i) {
            key.clear();
            for (int k = 0; k < np; ++k) {
                const arrow::Array& a = *flat[(size_t)k];
                if (a.IsNull(i)) key.push_back('\x00');
                else { key.push_back('\x01'); append_cell(a, i, key); }
                key.push_back('\x1f');   // unit separator between columns
            }
            if (seen.insert(key).second) keep.push_back(i);
        }
        if (keep.empty()) continue;
        arrow::ArrayVector out((size_t)np);
        for (int k = 0; k < np; ++k) {
            auto g = gather_chunked(arrow::ChunkedArray(flat[(size_t)k]), keep);
            if (!g.ok()) return "--distinct: gather failed: " + g.status().ToString();
            out[(size_t)k] = *g;
        }
        M += (int64_t)keep.size();
        pieces.push_back(arrow::Table::Make(proj_schema, out, (int64_t)keep.size()));
    }
    std::string old_path = src->path();

    std::shared_ptr<arrow::Table> distinct = empty_table(proj_schema);
    if (!pieces.empty()) {
        auto cat = arrow::ConcatenateTables(pieces);
        if (!cat.ok()) return "--distinct: concat failed: " + cat.status().ToString();
        auto one = (*cat)->CombineChunks();
        if (!one.ok()) return "--distinct: concat failed: " + one.status().ToString();
        distinct = *one;
    }

    src = std::make_unique<MemoryTableSource>(distinct,
        "<distinct " + old_path + ">",
        "Distinct rows: " + std::to_string(M) + " / " + std::to_string(N));
    // --select and --filter are now baked into the deduplicated table.
    cfg.select_cols.clear();
    cfg.filter_expr.clear();
    return "";
}
