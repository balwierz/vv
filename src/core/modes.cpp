// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Document modes (markdown, text, JSON documents) and shared CLI helpers.

#include "internal.hpp"

// ── Document modes (markdown today; plain text next) ─────────────────────────
//
// A "document" is a file vv renders rather than tabulates. It returns early in
// main(), before the TabularSource pipeline, so most of the tabular flag
// surface simply does not apply.
//
// Markdown used to IGNORE those flags: `vv --count foo.md` rendered the
// document and exited 0, and so did --schema / --describe / --stats /
// --list-tabs. PR #86 removed exactly that class of silent no-op everywhere
// else; this is the same fix for the one path that was missed.
//
// Markdown deliberately KEEPS --tsv / --csv / --select / --filter: a markdown
// file can embed GFM tables, and those flags genuinely drive them.
// Plain text differs from markdown in both directions. It IS a real
// TabularSource (one utf8 column, `line`), so --count / --tail / --filter /
// --list-columns / -n work naturally and are allowed. But --tsv / --csv /
// --select on a file with no fields produce output that fails downstream
// parsers for no stated reason, so those are errors here and not for markdown.
// ── JSON documents (routing) ─────────────────────────────────────────────────
//
// A JSON file is a document first: on a pipe `vv x.json` prints it re-indented
// (or, with --json-paths, as path = value lines). A flag that asks for rows or
// columns — an export, a report, a filter, -n, --table, --no-tree — keeps it
// on the table path (JsonSource), exactly as before.

// 1 for .json, 2 for NDJSON / JSON Lines (after .gz / .bgz / .zst), else 0.
int json_path_kind(const std::string& path) {
    std::string p = path;
    for (const char* z : {".gz", ".bgz", ".zstd", ".zst"})
        if (fends_ci(p, z)) { p.resize(p.size() - std::strlen(z)); break; }
    // GeoJSON, Jupyter notebooks and HTTP archives are JSON documents.
    if (fends_ci(p, ".json") || fends_ci(p, ".geojson") || fends_ci(p, ".ipynb") ||
        fends_ci(p, ".har")) return 1;
    if (fends_ci(p, ".ndjson") || fends_ci(p, ".jsonl")) return 2;
    return 0;
}

// The first flag that asks for JSON as a table, or nullptr.
const char* json_table_flag(const Config& cfg) {
    struct F { bool set; const char* name; };
    const F flags[] = {
        {cfg.delimiter == '\t', "--tsv"}, {cfg.delimiter == ',', "--csv"},
        {cfg.delimiter != 0, "--delimiter"}, {cfg.json_array, "--json"},
        {cfg.json_lines, "--ndjson"}, {cfg.md, "--md"},
        {!cfg.parquet_out.empty(), "--parquet"}, {!cfg.arrow_out.empty(), "--arrow"},
        {cfg.schema_only, "--schema"}, {cfg.describe, "--describe"},
        {cfg.stats_only, "--stats"}, {cfg.count, "--count"},
        {!cfg.unique_cols.empty(), "--unique"}, {cfg.sample_n > 0, "--sample"},
        {cfg.tail_rows_set, "--tail"}, {cfg.list_columns, "--list-columns"},
        {cfg.list_tabs, "--list-tabs"}, {!cfg.tab.empty(), "--tab"},
        {cfg.heatmap, "--heatmap"}, {cfg.distinct, "--distinct"},
        {!cfg.filter_expr.empty(), "--filter"}, {!cfg.select_cols.empty(), "--select"},
        {!cfg.sort_col.empty(), "--sort"}, {cfg.flatten, "--flatten"},
        {!cfg.expand_col.empty(), "--expand"}, {cfg.head_rows_set, "-n"},
        {cfg.vertical, "--vertical"}, {cfg.max_cols > 0, "-c"},
        {cfg.max_col_w_set, "-w"}, {cfg.no_index, "--no-index"},
        {cfg.no_header, "--no-header"},
        {!cfg.region.empty() || !cfg.regions_file.empty(), "-r"},
        {cfg.in_delimiter != 0, "-d"}, {cfg.no_interactive, "--table"},
        {cfg.json_no_tree, "--no-tree"},
    };
    for (const auto& f : flags) if (f.set) return f.name;
    return nullptr;
}

// Print a JSON document from `in` (pretty, or --json-paths); "" or the error.
std::string print_json_document(arrow::io::InputStream& in, const Config& cfg,
                                bool lines) {
    const vvjson::JsonError e = vvjson::write_json_document(
        in, stdout, cfg.json_paths ? vvjson::JsonOut::Paths : vvjson::JsonOut::Pretty,
        lines, /*color=*/*g_color.reset != '\0');
    return e.ok() ? std::string() : e.describe();
}

// The decoded bytes of a JSON file (gzip / zstd by magic).
std::string open_json_file(const std::string& path,
                           std::shared_ptr<arrow::io::InputStream>* out) {
    auto raw = arrow::io::ReadableFile::Open(path);
    if (!raw.ok()) return "Cannot open '" + path + "': " + raw.status().ToString();
    std::shared_ptr<arrow::io::InputStream> in = *raw;
    const arrow::Compression::type comp = sniff_stream_codec(*raw);
    if (comp != arrow::Compression::UNCOMPRESSED) {
        auto codec = arrow::util::Codec::Create(comp);
        if (!codec.ok()) return codec.status().ToString();
        auto ci = arrow::io::CompressedInputStream::Make(codec->get(), in);
        if (!ci.ok()) return ci.status().ToString();
        in = *ci;
    }
    *out = std::move(in);
    return "";
}


std::string document_flag_error(const Config& cfg, DocKind kind) {
    const int k = (int)kind;
    const bool md = (kind == DocKind::Markdown);
    const char* what = md ? "a markdown file" : "a text file";
    // `both` = rejected by markdown and by text; MD / TX = one kind only.
    const int BOTH = 3, MD = 1, TX = 2;
    struct Rule { bool set; int kinds; const char* flag; const char* why; };
    const Rule rules[] = {
        {cfg.schema_only,              BOTH, "--schema",    md ? "it has no columns" : "its only column is `line`"},
        {cfg.describe,                 BOTH, "--describe",  md ? "it has no columns" : "its only column is `line`"},
        {cfg.stats_only,               BOTH, "--stats",     "it has no Parquet metadata"},
        {cfg.seq_stats,                BOTH, "--seq-stats", "it is not a FASTA / FASTQ file"},
        {cfg.count,                    MD,   "--count",     "it has no rows"},
        {!cfg.unique_cols.empty(),     BOTH, "--unique",    md ? "it has no columns" : "its only column is `line`"},
        {cfg.sample_n > 0,             BOTH, "--sample",    "it has no rows"},
        {cfg.heatmap,                  BOTH, "--heatmap",   "it has no numeric columns"},
        {cfg.list_columns,             MD,   "--list-columns", "it has no columns"},
        {cfg.list_tabs,                BOTH, "--list-tabs", "it has no component tabs"},
        {!cfg.tab.empty(),             BOTH, "--tab",       "it has no component tabs"},
        {!cfg.expand_col.empty(),      BOTH, "--expand",    md ? "it has no columns" : "its only column is `line`"},
        {!cfg.select_cols.empty(),     TX,   "--select",    "its only column is `line`"},
        {!cfg.parquet_out.empty(),     BOTH, "--parquet",   "it is not tabular"},
        {!cfg.arrow_out.empty(),       BOTH, "--arrow",     "it is not tabular"},
        {cfg.json_array || cfg.json_lines, BOTH, "--json",  "it is not tabular"},
        {cfg.pileup,                   BOTH, "--pileup",    "it is not an alignment file"},
        {cfg.decode_pileup,            BOTH, "--decode-pileup", "it is not an mpileup file"},
        {cfg.tail_rows_set,            MD,   "--tail",      "it has no rows"},
        {cfg.delimiter != 0,           TX,   "--tsv/--csv/--delimiter",
                                             "it has no fields to separate; it is already plain text"},
        {cfg.md,                       TX,   "--md/--markdown",
                                             "it has no fields; a one-column `line` table is not a table"},
        {cfg.vertical && !g_vertical_from_argv0,
                                       BOTH, "--vertical",  md ? "it has no columns to transpose"
                                                               : "its only column is `line`"},
    };
    for (const auto& r : rules) {
        if (!r.set || !(r.kinds & k)) continue;
        return std::string(r.flag) + " does not apply to " + what + " — " + r.why;
    }
    return "";
}


// Will the interactive viewer take over? The single source of truth for that
// question — main() asks it twice (once to decide whether --tail may stream,
// once to actually launch), and the two must not drift.
bool tui_wanted(const Config& cfg) {
    bool auto_tui = !cfg.no_interactive && !cfg.delimiter && !cfg.vertical
                    && cfg.parquet_out.empty() && cfg.arrow_out.empty()
                    && !cfg.head_rows_set
                    && isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
    return cfg.interactive || auto_tui;
}

// Stream a text source to stdout verbatim: the bytes that came in, with the
// line terminators put back. `vv f.log > copy` must be byte-identical to
// f.log, so this deliberately does NOT go through the table renderer, the
// index gutter, truncation, or the colouriser.
//
// Honours -n N (0 = all), --tail N and --filter 'line contains "..."'.
int emit_text_stream(TabularSource& src, const Config& cfg) {
    const std::vector<int> col = {0};
    FilterExpr fx;
    const bool have_filter = !cfg.filter_expr.empty();
    if (have_filter) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr)) {
            std::fprintf(stderr, "vv: %s: --filter: %s\n",
                         cfg.path.c_str(), ferr.c_str());
            return 1;
        }
    }
    // A trailing line with no terminator must stay that way. Only TextSource
    // knows; anything else (stdin) is treated as newline-terminated.
    const bool final_newline = text_final_newline(src);

    // --tail keeps a bounded ring of the last N lines; everything else
    // streams straight out.
    const bool tail = cfg.tail_rows_set && cfg.tail_rows > 0;
    std::vector<std::string> ring;
    int64_t limit = (cfg.head_rows_set && cfg.head_rows > 0)
                    ? (int64_t)cfg.head_rows : INT64_MAX;
    if (cfg.head_rows_set && cfg.head_rows == 0) limit = INT64_MAX;  // -n 0 = all
    int64_t emitted = 0;
    bool truncated = false;   // stopped early, so the last line printed is not
                              // the file's last line
    if (tail) src.set_retain_all(false);

    for (int c = 0; !truncated; ++c) {
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, col, &tbl).ok() || !tbl) continue;
        if (have_filter) {
            tbl = apply_filter(tbl, fx, col);
            if (!tbl) continue;
        }
        for (int ch = 0; ch < tbl->column(0)->num_chunks(); ++ch) {
            auto arr = std::static_pointer_cast<arrow::StringArray>(
                           tbl->column(0)->chunk(ch));
            for (int64_t i = 0; i < arr->length(); ++i) {
                std::string_view v =
                    arr->IsNull(i) ? std::string_view() : std::string_view(arr->GetView(i));
                if (tail) {
                    ring.emplace_back(v.data(), v.size());
                    if ((int64_t)ring.size() > (int64_t)cfg.tail_rows)
                        ring.erase(ring.begin());
                    continue;
                }
                if (emitted > 0) std::fputc('\n', stdout);
                std::fwrite(v.data(), 1, v.size(), stdout);
                if (++emitted >= limit) { truncated = true; break; }
            }
            if (truncated) break;
        }
    }
    if (tail) {
        for (size_t i = 0; i < ring.size(); ++i) {
            std::fwrite(ring[i].data(), 1, ring[i].size(), stdout);
            if (i + 1 < ring.size()) std::fputc('\n', stdout);
        }
        if (!ring.empty() && final_newline) std::fputc('\n', stdout);
    } else if (emitted > 0) {
        // The newline after the final line: present unless the file itself
        // ended without one AND we printed all the way to the end.
        if (truncated || final_newline) std::fputc('\n', stdout);
    }
    if (!src.read_status().ok()) {
        std::fprintf(stderr, "vv: %s: %s\n", cfg.path.c_str(),
                     shorten_reader_error(src.read_status().ToString()).c_str());
        return 1;
    }
    return 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────

// Friendly, short message for common filesystem problems; returns "" if the
// path is a readable regular file (further errors will come from the reader).
std::string preflight_path(const std::string& path) {
    if (path == "-") return "";
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        switch (errno) {
            case ENOENT: return "file not found";
            case EACCES: return "permission denied";
            case ENOTDIR: return "not a directory";
            case ELOOP:  return "too many symlinks";
            default:     return std::string("cannot stat: ") + std::strerror(errno);
        }
    }
    // A directory is a dataset (concatenated data files); let the dataset
    // opener validate its contents and report any problem.
    if (!S_ISDIR(st.st_mode) &&
        !S_ISREG(st.st_mode) && !S_ISFIFO(st.st_mode) && !S_ISLNK(st.st_mode))
        return "not a regular file";
    if (::access(path.c_str(), R_OK) != 0) return "permission denied";
    return "";
}

// Strip "IOError: Failed to open local file '<path>'. Detail: [errno N] "
// redundancy from Arrow's open-error strings — the path is already in our
// own "cannot open '...': " prefix.
std::string shorten_reader_error(std::string msg) {
    if (msg.find("straddling object straddles two block boundaries") != std::string::npos)
        return "a record is longer than the " + std::to_string(csv_block_bytes() >> 20) +
               " MiB read block; set VV_CSV_BLOCK_MB to a larger size (e.g. " +
               std::to_string((csv_block_bytes() >> 20) * 8) + ") and run again";
    auto erase = [&](const std::string& needle) {
        auto p = msg.find(needle);
        if (p != std::string::npos) msg.erase(p, needle.size());
    };
    erase("IOError: ");
    auto dp = msg.find(". Detail: ");
    if (dp != std::string::npos) {
        std::string detail = msg.substr(dp + 10);
        auto bracket = detail.find("] ");
        if (detail.rfind("[errno ", 0) == 0 && bracket != std::string::npos)
            detail.erase(0, bracket + 2);
        msg = msg.substr(0, dp);
        if (!detail.empty()) msg = std::move(detail);
    }
    return msg;
}

std::vector<int> select_columns(const TabularSource& src, const std::string& spec,
                                std::vector<std::string>* unknown) {
    Config cfg;
    cfg.select_cols = spec;
    return select_field_indices(src, cfg, unknown);
}

std::string packed_column_for(const std::string& path) {
    std::string det = path;
    if      (fends_ci(det, ".zstd")) det.resize(det.size() - 5);
    else if (fends_ci(det, ".zst"))  det.resize(det.size() - 4);
    if (fends_ci(det, ".gz")) det.resize(det.size() - 3);
    if (fends_ci(det, ".gff") || fends_ci(det, ".gff3") || fends_ci(det, ".gtf"))
        return "attributes";
    if (fends_ci(det, ".vcf") || fends_ci(det, ".bcf"))
        return "INFO";
    return "";
}

// Why `mode` (e.g. "--tsv") must not run on `src`, or "" when it may. A mode
// that writes or aggregates every row would otherwise present a capped preview
// as the whole dataset and exit 0. HDF5 matrices never get here (the callers
// swap in full_matrix()); what remains is a NumPy array past 4096 columns. `rows_wanted`: how many rows the mode reads (-n), or -1 for all — a row
// cap matters only when the mode wants more rows than the preview holds.
// `cols_matter`: false for a mode that only counts rows (--count).
std::string preview_refusal(const TabularSource& src, const std::string& mode,
                            int64_t rows_wanted, bool cols_matter) {
    const PreviewLimit l = src.preview_limit();
    const bool rows_short = l.rows_capped() &&
                            (rows_wanted < 0 || rows_wanted > l.shown_rows);
    if (!rows_short && !(cols_matter && l.cols_capped())) return "";
    auto shape = [](int64_t r, int64_t c) {
        return std::to_string(r) + " \xc3\x97 " + std::to_string(c);
    };
    return "tab '" + src.tab_label() + "' is a " + shape(l.shown_rows, l.shown_cols) +
           " preview of a " + shape(l.full_rows, l.full_cols) +
           " (rows \xc3\x97 columns) matrix; " + mode +
           " would give the preview as if it were the whole matrix. Exports stream "
           "HDF5 / AnnData / Loom / Cell Ranger matrices in full, but not NumPy "
           "arrays past 4096 columns; use the TUI or the table view to look at "
           "this one";
}

// --tab NAME: replace `src` with its component tab NAME (AnnData obs/var/X, a
// workbook sheet, …). Matching is case-insensitive: exact, or a prefix at a
// word boundary so `--tab X` selects "X (preview)". Tabs are enumerated by
// label without building them (lazy), so only the selected component is read.
// Returns "" or an error listing the available tabs.
std::string select_tab(std::unique_ptr<TabularSource>& src,
                       const std::string& tab) {
    auto lc = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string want = lc(tab);
    auto matches = [&](const std::string& label) {
        std::string a = lc(label);
        if (a == want) return true;
        return a.size() > want.size() &&
               a.compare(0, want.size(), want) == 0 &&
               (a[want.size()] == ' ' || a[want.size()] == '(' ||
                a[want.size()] == '[');
    };
    std::vector<std::string> avail{ src->tab_label() };
    if (matches(src->tab_label())) return "";
    std::unique_ptr<TabularSource> chosen;
    for (auto& sib : src->expand_tabs()) {
        avail.push_back(sib->tab_label());
        if (!chosen && matches(sib->tab_label())) chosen = std::move(sib);
    }
    if (!chosen) {
        std::string list;
        for (auto& a : avail) { if (!list.empty()) list += ", "; list += a; }
        return "no tab named '" + tab + "'; available: " + list;
    }
    src = std::move(chosen);
    return "";
}

std::string export_view(const Config& cfg_in, const std::string& out_path,
                        ExportFormat format, ExportProgress* progress) {
    namespace fs = std::filesystem;
    Config cfg = cfg_in;
    if (out_path.empty()) return "no output file";
    std::error_code ec;
    const fs::path in_p(cfg.path), out_p(out_path);
    if (fs::exists(out_p, ec) && fs::equivalent(in_p, out_p, ec))
        return "'" + out_path + "' is the input file; choose another name";
    if (fs::is_directory(in_p, ec)) {
        // A dataset directory is re-read on every open; a file written into it
        // would join the dataset.
        const fs::path dir  = fs::weakly_canonical(in_p, ec);
        const fs::path dest = fs::weakly_canonical(out_p, ec).parent_path();
        auto d = dest.begin(), r = dir.begin();
        for (; r != dir.end() && d != dest.end() && *r == *d; ++r, ++d) {}
        if (r == dir.end())
            return "'" + out_path + "' is inside the input dataset directory; "
                   "choose another location";
    }

    // The output mode is set before the source opens: readers size their
    // previews by it (an AnnData obs / var is read in full only when every row
    // will be written).
    const std::string part = out_path + ".part";
    const char* mode = "";
    switch (format) {
        case ExportFormat::Parquet: cfg.parquet_out = part; mode = "a Parquet export"; break;
        case ExportFormat::Arrow:   cfg.arrow_out   = part; mode = "an Arrow export";  break;
        case ExportFormat::Tsv:     cfg.delimiter = '\t';   mode = "a TSV export";     break;
        case ExportFormat::Csv:     cfg.delimiter = ',';    mode = "a CSV export";     break;
        case ExportFormat::Json:    cfg.json_array = true;  mode = "a JSON export";    break;
        case ExportFormat::Ndjson:  cfg.json_lines = true;  mode = "an NDJSON export"; break;
    }
    if (auto err = apply_region_modifiers(cfg); !err.empty()) return err;
    std::unique_ptr<TabularSource> src;
    if (auto err = open_source(cfg.path, cfg, &src); !err.empty() || !src)
        return err.empty() ? "cannot open '" + cfg.path + "'" : err;
    if (!cfg.tab.empty())
        if (auto err = select_tab(src, cfg.tab); !err.empty()) return err;
    if (!src->read_status().ok())
        return shorten_reader_error(src->read_status().ToString());
    if (src->preview_limit().capped())                 // stream a whole matrix
        if (auto full = src->full_matrix()) src = std::move(full);
    if (auto err = preview_refusal(*src, mode); !err.empty()) return err;
    if (!cfg.sort_col.empty()) {
        if (auto err = build_sort(src, cfg); !err.empty()) return err;
        cfg.filter_expr.clear();   // applied during the sort's materialisation
    }
    cfg.head_rows = 0; cfg.head_rows_set = false;
    cfg.no_header = false;

    ExportSink sink;
    sink.progress = progress;
    FILE* f = nullptr;
    if (format != ExportFormat::Parquet && format != ExportFormat::Arrow) {
        f = std::fopen(part.c_str(), "wb");
        if (!f) return "cannot write '" + part + "': " + std::strerror(errno);
        sink.out = f;
    }
    std::string err;
    t_export = &sink;
    switch (format) {
        case ExportFormat::Parquet: err = write_parquet(*src, cfg);   break;
        case ExportFormat::Arrow:   err = write_arrow(*src, cfg);     break;
        case ExportFormat::Tsv:
        case ExportFormat::Csv:     err = write_delimited(*src, cfg); break;
        case ExportFormat::Json:
        case ExportFormat::Ndjson:  err = write_json(*src, cfg);      break;
    }
    t_export = nullptr;
    if (f) {
        const bool bad = std::ferror(f) != 0;
        if (std::fclose(f) != 0 || bad)
            if (err.empty()) err = "write to '" + part + "' failed: " + std::strerror(errno);
    }
    if (err.empty() && !src->read_status().ok())
        err = shorten_reader_error(src->read_status().ToString());
    if (err.empty() && progress && progress->cancel.load()) err = kExportCanceled;
    if (!err.empty()) {
        std::remove(part.c_str());
        return err;
    }
    fs::rename(part, out_p, ec);
    if (ec) {
        std::remove(part.c_str());
        return "cannot rename '" + part + "' to '" + out_path + "': " + ec.message();
    }
    return "";
}
