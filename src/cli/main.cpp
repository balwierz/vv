// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// vv -- universal genomic file viewer
// https://github.com/balwierz/vv
//
// main(): open the input and dispatch to the requested mode.

#include "internal.hpp"

#ifndef VV_CORE_LIB   // CLI entry point — excluded from libvvcore
int main(int argc, char** argv) {
    init_console();
    Config cfg = parse_args(argc, argv);
#ifdef VV_NO_TUI
    if (cfg.interactive)
        std::fprintf(stderr, "vv: built without the interactive viewer; -i ignored "
                             "(vvg is the desktop viewer)\n");
#endif

    // --formats: the registry, no input file needed.
    if (cfg.list_formats) {
        print_formats(cfg.json_array || cfg.json_lines);
        return 0;
    }

    // Size Arrow's CPU thread pool so use_threads=true on the CSV / Parquet
    // readers actually has workers available.
    (void)arrow::SetCpuThreadPoolCapacity(effective_decode_threads(cfg));

    // Apply --regions-file and --slop once, before any source-specific
    // region consumer parses cfg.region.
    if (auto e = apply_region_modifiers(cfg); !e.empty()) {
        std::fprintf(stderr, "vv: %s\n", e.c_str());
        return 1;
    }

    // Config file first — every key applies only where its CLI flag wasn't
    // given, so this is unconditional. (It used to run only when --theme was
    // absent, which silently discarded every OTHER config key — scrolloff —
    // whenever a theme was named on the command line.)
    load_user_config(cfg);
    // Resolve theme: CLI flag wins; otherwise the config value; otherwise
    // the built-in default.
    if (cfg.theme.empty()) cfg.theme = "default";
    if (auto* t = find_theme(cfg.theme)) {
        g_theme = t;
    } else {
        std::fprintf(stderr,
            "vv: unknown --theme '%s'. Available: default, dark, light, "
            "solarized-dark, solarized-light.\n", cfg.theme.c_str());
        return 2;
    }

    bool use_color = (cfg.color == ColorMode::Always) ||
                     (cfg.color == ColorMode::Auto && isatty(STDOUT_FILENO));
    if (use_color) init_colors();

    bool err_color = (cfg.color == ColorMode::Always) ||
                     (cfg.color == ColorMode::Auto && isatty(STDERR_FILENO));
    const char* C_BOLD = err_color ? "\033[1m"    : "";
    const char* C_RED  = err_color ? "\033[1;31m" : "";
    const char* C_DIM  = err_color ? "\033[2m"    : "";
    const char* C_RST  = err_color ? "\033[0m"    : "";
    auto report = [&](const std::string& path, const std::string& why) {
        std::fprintf(stderr, "%svv:%s %s%s%s: %s%s%s\n",
                     C_BOLD, C_RST, C_DIM, path.c_str(), C_RST,
                     C_RED, why.c_str(), C_RST);
    };

    // Resolve the ASCII-table frame style (--box, else the locale).
    if (auto e = select_box_glyphs(cfg.box_style); !e.empty()) {
        std::fprintf(stderr, "vv: %s\n", e.c_str());
        return 2;
    }

    {
        std::string why = preflight_path(cfg.path);
        if (!why.empty()) { report(cfg.path, why); return 1; }
    }

    // Extra positionals are only meaningful in the interactive viewer, which
    // gives each file its own tab. Every other mode read cfg.path and dropped
    // the rest without a word — say so instead of answering about one file
    // when the user asked about several.
    if (cfg.paths.size() > 1) {
        // A document (markdown today) never reaches the multi-file TUI loop —
        // it returns early below, rendering only cfg.path. Without this it
        // silently showed the first file and exited 0.
        const bool doc = !cfg.force_text &&
                        (fends_ci(cfg.path, ".md")    || fends_ci(cfg.path, ".markdown") ||
                         fends_ci(cfg.path, ".mdown") || fends_ci(cfg.path, ".mkd"));
        bool scripted = doc || cfg.schema_only || cfg.stats_only || cfg.describe ||
                        cfg.count || cfg.heatmap || !cfg.unique_cols.empty() ||
                        cfg.json_array || cfg.json_lines || cfg.md ||
                        cfg.delimiter || !cfg.parquet_out.empty() ||
                        !cfg.arrow_out.empty() || cfg.vertical || cfg.validate;
        bool tui = !scripted &&
                   (cfg.interactive ||
                    (!cfg.no_interactive && !cfg.head_rows_set &&
                     isatty(STDOUT_FILENO) && isatty(STDIN_FILENO)));
        if (!tui) {
            report(cfg.path,
                   "multiple input files (" + std::to_string(cfg.paths.size()) +
                   " given) are only supported in the interactive viewer; "
                   "this mode reads just the first");
            return 1;
        }
    }

    // --validate: LociSSD invariants check. Doesn't go through open_source —
    // we re-open the Parquet file with our own reader so other flags (region,
    // select, filter) don't influence what we scan.
    if (cfg.validate) {
        // The checker is LociSSD-v3-specific (it walks Parquet row groups), so
        // anything else got "Not a valid Parquet file" — misleading for a BED,
        // and outright wrong for a v4 "colblock" .lociss, which is not Parquet
        // at all. Say which case the user is in.
        if (!fends_ci(cfg.path, ".lociss")) {
            report(cfg.path, "--validate checks LociSSD invariants; this is "
                             "not a LociSSD file (expected a .lociss path)");
            return 1;
        }
        {
            unsigned char magic[4] = {0, 0, 0, 0};
            if (std::FILE* f = std::fopen(cfg.path.c_str(), "rb")) {
                (void)std::fread(magic, 1, 4, f);
                std::fclose(f);
            }
            if (magic[0] == 'L' && magic[1] == 'S' &&
                magic[2] == 'B' && magic[3] == '1') {
                report(cfg.path, "--validate supports LociSSD v3 (Parquet) "
                                 "only; this is a v4 \"colblock\" file");
                return 1;
            }
        }
        std::string err = validate_lociss(cfg.path);
        if (!err.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        return 0;
    }

    // ── Markdown (`.md` / `.markdown` / `.mdown` / `.mkd`) ───────────────────
    // Routed before the TabularSource pipeline because markdown isn't
    // tabular. Parse via md4c, render prose as ANSI, dump every
    // embedded GFM table via the existing print_table path (so column-
    // type inference / colours match a real `.csv`). On a TTY we pipe
    // the whole stream through `less -R` for scroll / search — that's
    // the "interactive" experience for markdown until a proper
    // ncurses tab kind lands in TableTUI (TODO).
    // --text asks for the SOURCE, so it has to be tested before the renderer
    // claims the file. `vv --text notes.md` is the documented example in
    // man/vv.1, docs/USAGE.md and the README; without this guard the branch
    // below swallowed the file first and the flag was a silent no-op.
    if (!cfg.force_text &&
        (fends_ci(cfg.path, ".md")        || fends_ci(cfg.path, ".markdown") ||
         fends_ci(cfg.path, ".mdown")     || fends_ci(cfg.path, ".mkd"))) {
        if (std::string fe = document_flag_error(cfg, DocKind::Markdown); !fe.empty()) {
            report(cfg.path, fe);
            return 1;
        }
        int term_w = detect_terminal_width();
        if (term_w <= 0) term_w = 80;
        md::MarkdownDoc doc;
        std::string merr = md::parse_markdown_file(cfg.path, term_w, &doc);
        if (!merr.empty()) {
            std::fprintf(stderr, "vv: %s\n", merr.c_str());
            return 1;
        }
        // First error from any embedded GFM table (a bad --select / --filter);
        // surfaced after the pager closes so it isn't swallowed by `less`.
        std::string table_err;
        // In a delimited mode the caller wants parseable data, not a
        // rendered document: emit only the embedded tables, without the prose
        // or the captions. `vv x.md --tsv > out.tsv` used to write the whole
        // rendered README ahead of the TSV.
        // --md is a scripted output mode too: `vv README.md --md > tables.md`
        // wants the embedded tables, not the rendered prose. It used to be
        // ignored here, so it returned the ANSI document.
        const bool scripted_out = cfg.delimiter != 0 || cfg.md;
        auto emit = [&]() {
            if (!scripted_out) md::emit_markdown_stdout(doc);
            for (size_t i = 0; i < doc.tables.size(); ++i) {
                if (!scripted_out)
                    std::fprintf(stdout, "\n%s%s%s\n",
                                  g_color.header,
                                  doc.table_captions[i].c_str(),
                                  g_color.reset);
                MemoryTableSource ts(doc.tables[i],
                                      doc.source_path,
                                      "Format: markdown table");
                std::string werr;
                if (cfg.schema_only) {
                    /* schema-only doesn't make sense per-table; skip. */
                } else if (cfg.md) {
                    werr = write_markdown(ts, cfg);
                } else if (cfg.delimiter) {
                    werr = write_delimited(ts, cfg);
                } else {
                    werr = print_table(ts, cfg);
                }
                if (!werr.empty() && table_err.empty()) table_err = werr;
            }
        };
        // Pager when we're plausibly interactive: TTY output, and the
        // user hasn't asked for a scripted form (--tsv/--csv, --schema,
        // -n, --no-interactive).
        bool want_pager = isatty(STDOUT_FILENO)
                          && !cfg.no_interactive
                          && !cfg.md
                          && !cfg.delimiter
                          && !cfg.head_rows_set
                          && !cfg.schema_only
                          && !cfg.describe
                          && !cfg.stats_only
                          && cfg.parquet_out.empty()
                          && cfg.arrow_out.empty();
        if (want_pager) md::emit_via_pager(emit);
        else            emit();
        if (!table_err.empty()) { report(cfg.path, table_err); return 1; }
        return 0;
    }

    // ── JSON documents ───────────────────────────────────────────────────────
    // On a pipe (or with --pretty / --json-paths) a JSON file is printed as a
    // document; a table flag keeps it on the table path. JSON on stdin is
    // recognised by its content inside open_source (json_document asks for
    // the stream rather than a table).
    const bool json_stdin = cfg.path == "-" || path_is_pipe(cfg.path);
    const char* json_tflag = json_table_flag(cfg);
    {
        // Several files open as tabs of the table viewer, as before.
        const bool one_file = cfg.paths.size() <= 1;
        const int jkind = (cfg.force_text || !one_file) ? 0 : json_path_kind(cfg.path);
        if (cfg.json_pretty || cfg.json_paths) {
            const char* which = cfg.json_pretty ? "--pretty" : "--json-paths";
            if (cfg.json_pretty && cfg.json_paths) {
                report(cfg.path, "--pretty and --json-paths both print the document; give one");
                return 1;
            }
            if (json_tflag) {
                report(cfg.path, std::string(which) + " prints the JSON document; it does not "
                                 "combine with " + json_tflag);
                return 1;
            }
            if (jkind == 0 && !json_stdin) {
                report(cfg.path, std::string(which) + " applies to JSON input (.json, .ndjson, "
                                 ".jsonl, .geojson, .ipynb, .har, or JSON on stdin)");
                return 1;
            }
        }
        int jk = jkind;
        if (cfg.json_tree) {
            const char* bad = json_tflag ? json_tflag : cfg.json_pretty ? "--pretty"
                            : cfg.json_paths ? "--json-paths" : nullptr;
            if (bad) {
                report(cfg.path, std::string("--tree opens the JSON viewer; it does not combine with ") + bad);
                return 1;
            }
            // Any extension: --tree takes a file whose content is JSON.
            if (jk == 0 && !json_stdin) {
                std::shared_ptr<arrow::io::InputStream> in;
                std::string head(8192, '\0');
                if (open_json_file(cfg.path, &in).empty()) {
                    auto got = in->Read((int64_t)head.size(), head.data());
                    head.resize(got.ok() ? (size_t)*got : 0);
                    if (vvjson::looks_like_json(head)) jk = 1;
                }
                if (jk == 0) { report(cfg.path, "--tree: the file is not JSON"); return 1; }
            }
        }
        const bool want_tree = jk != 0 && !json_tflag && !cfg.json_pretty && !cfg.json_paths &&
                               (cfg.json_tree || (tui_wanted(cfg) && cfg.json_view != "table"));
        bool tree_failed = false;
        if (want_tree) {
            // The viewer maps the file; a compressed one is decompressed to a
            // temporary copy first (removed at exit).
            std::string file = cfg.path;
            {
                auto raw = arrow::io::ReadableFile::Open(cfg.path);
                if (raw.ok() && sniff_file_codec(*raw) != StreamCodec::None) {
                    std::shared_ptr<arrow::io::InputStream> in;
                    if (auto e = open_json_file(cfg.path, &in); !e.empty()) { report(cfg.path, e); return 1; }
                    int64_t bytes = 0;
                    if (auto e = spool_stream(in, ".json", &file, &bytes, cfg.path); !e.empty()) {
                        report(cfg.path, e);
                        return 1;
                    }
                }
            }
            if (auto e = run_json_viewer(file, cfg.path, cfg.path, jk == 2, false, cfg, &tree_failed);
                !e.empty()) {
                report(cfg.path, e);
                return 1;
            }
            if (!tree_failed) return 0;
            std::fprintf(stderr, "vv: interactive viewer unavailable (terminal init failed); "
                         "showing non-interactive output\n");
        }
        if (jk != 0 && !json_tflag &&
            (tree_failed || cfg.json_pretty || cfg.json_paths || !tui_wanted(cfg))) {
            std::shared_ptr<arrow::io::InputStream> in;
            if (auto e = open_json_file(cfg.path, &in); !e.empty()) { report(cfg.path, e); return 1; }
            if (auto e = print_json_document(*in, cfg, jk == 2); !e.empty()) {
                report(cfg.path, e);
                return 1;
            }
            return 0;
        }
        cfg.json_document = json_stdin && one_file && !json_tflag && !cfg.force_text;
    }

    std::unique_ptr<TabularSource> src;
    // --contigs is applied inside open_source() now (so the GUI / KDE plugins
    // reach it too); it reads only the header and returns the reference-sequence
    // table, which the modes below (--tsv / --json / --sort / --filter / the TUI)
    // then render.
    std::string err = open_source(cfg.path, cfg, &src);
    if (!err.empty()) {
        // open_source returns "Cannot open '<path>': <detail>"; split it back
        // out so we can reformat with color and strip Arrow's noisy prefix.
        std::string detail = err;
        const std::string pfx = "Cannot open '" + cfg.path + "': ";
        if (detail.rfind(pfx, 0) == 0) detail.erase(0, pfx.size());
        else if (detail.rfind("Cannot open '", 0) == 0) {
            auto q = detail.find("': ");
            if (q != std::string::npos) detail.erase(0, q + 3);
        }
        report(cfg.path, shorten_reader_error(std::move(detail)));
        return 1;
    }

    // JSON on stdin, wanted as a document: print it from the decoded stream.
    if (auto json_doc = json_document_stream(src.get())) {
        const std::string label = cfg.path == "-" ? "stdin" : cfg.path;
        // A terminal on stdout: the tree viewer, reading keys from the
        // controlling terminal (stdin carries the data), over a copy of the
        // stream it can map.
        if (!cfg.json_pretty && !cfg.json_paths && cfg.json_view != "table" &&
            (cfg.json_tree || cfg.interactive || isatty(STDOUT_FILENO))) {
            std::string tmp;
            int64_t bytes = 0;
            if (auto e = spool_stream(json_doc, ".json", &tmp, &bytes, label); !e.empty()) {
                report(label, e);
                return 1;
            }
            bool failed = false;
            if (auto e = run_json_viewer(tmp, tmp, label, false, true, cfg, &failed); !e.empty()) {
                report(label, e);
                return 1;
            }
            if (!failed) return 0;
            std::shared_ptr<arrow::io::InputStream> in;
            if (auto e = open_json_file(tmp, &in); !e.empty()) { report(label, e); return 1; }
            if (auto e = print_json_document(*in, cfg, false); !e.empty()) { report(label, e); return 1; }
            return 0;
        }
        if (auto e = print_json_document(*json_doc, cfg, false); !e.empty()) {
            report(cfg.path == "-" ? "stdin" : cfg.path, e);
            return 1;
        }
        return 0;
    }
    if (cfg.json_pretty || cfg.json_paths) {
        report(cfg.path, std::string(cfg.json_pretty ? "--pretty" : "--json-paths") +
                         ": the input is not JSON");
        return 1;
    }

    // -r asked for a window this format cannot provide. vv used to ignore the
    // flag and hand back the whole file with exit 0 — the answer looks like a
    // region query and isn't one. Warn rather than fail: the output is still
    // valid, it is just not what was asked for.
    if (!cfg.region.empty() && !src->region_applied())
        std::fprintf(stderr,
                     "%svv:%s %s%s%s: warning: this format has no region index; "
                     "-r was not applied and the whole file is shown\n",
                     C_BOLD, C_RST, C_DIM, cfg.path.c_str(), C_RST);

    // A plain-text source is a document, not a table. Most of the tabular
    // flag surface has no meaning over one utf8 column of lines; reject those
    // rather than answering with a number nobody asked for. --count / --tail /
    // --filter / --list-columns / -n do mean something and are allowed.
    if (src->is_text()) {
        std::string fe = document_flag_error(cfg, DocKind::Text);
        if (!fe.empty()) { report(cfg.path, fe); return 1; }
    }

    // --tab NAME: view a named component tab (AnnData obs/var/X, a workbook
    // sheet, …) instead of the first one. Lets the CLI reach the data tabs that
    // are otherwise only navigable in the interactive TUI. Matching is
    // case-insensitive: exact, or a prefix at a word boundary so `--tab X`
    // selects "X (preview)". Tabs are enumerated by label without building
    // them (lazy), so only the selected component is read.
    if (!cfg.tab.empty()) {
        if (auto terr = select_tab(src, cfg.tab); !terr.empty()) {
            report(cfg.path, terr);
            return 1;
        }
        // A component that failed to read (e.g. an HDF5 dataset compressed with
        // a codec this build lacks) is reported here, before any output mode
        // would print its error placeholder as if it were data.
        if (!src->read_status().ok()) {
            report(cfg.path, shorten_reader_error(src->read_status().ToString()));
            return 1;
        }
    }

    // A capped preview (an HDF5 / AnnData matrix, a wide NumPy array) is fine
    // to look at, but an export or an aggregate over it would present the
    // preview as the whole dataset. Refuse those modes up front.
    {
        std::string mode;
        bool cols_matter = true, honours_n = true;
        if      (!cfg.parquet_out.empty())        mode = "--parquet";
        else if (!cfg.arrow_out.empty())          mode = "--arrow";
        else if (cfg.json_array)                  mode = "--json";
        else if (cfg.json_lines)                  mode = "--ndjson";
        else if (cfg.md)                          mode = "--md";
        else if (cfg.delimiter == '\t')           mode = "--tsv";
        else if (cfg.delimiter == ',')            mode = "--csv";
        else if (cfg.delimiter)                   mode = "--delimiter";
        else if (cfg.describe)                  { mode = "--describe"; honours_n = false; }
        else if (!cfg.unique_cols.empty())      { mode = "--value-counts"; honours_n = false; }
        else if (cfg.sample_n > 0)              { mode = "--sample";   honours_n = false; }
        else if (cfg.tail_rows_set)             { mode = "--tail";     honours_n = false; }
        else if (cfg.count) { mode = "--count"; honours_n = false; cols_matter = false; }
        // An export honours -n: `-n 10 --tsv` of a 1000-row preview is complete.
        const int64_t rows_wanted = (honours_n && cfg.head_rows_set && cfg.head_rows > 0)
                                    ? (int64_t)cfg.head_rows : -1;
        // A matrix preview that can be read in full is exported in full.
        if (!mode.empty() && src->preview_limit().capped() &&
            preview_refusal(*src, mode, rows_wanted, cols_matter) != "")
            if (auto full = src->full_matrix()) src = std::move(full);
        if (!mode.empty())
            if (auto perr = preview_refusal(*src, mode, rows_wanted, cols_matter);
                !perr.empty()) {
                report(cfg.path, perr);
                return 1;
            }
    }

    // --list-columns: one name per line — the shape a shell completion or an
    // xargs pipeline wants, without parsing the schema table.
    if (cfg.list_columns) {
        auto sch = src->schema();
        std::set<std::string> hidden;
        for (const auto& h : src->hidden_for_display()) hidden.insert(h);
        for (int i = 0; i < sch->num_fields(); ++i) {
            const std::string& n = sch->field(i)->name();
            // Honour the same hidden semantics the views use; --schema still
            // shows everything.
            if (hidden.count(n)) continue;
            std::printf("%s\n", n.c_str());
        }
        return 0;
    }

    // --list-tabs: the component tabs of a multi-tab container. This is the
    // enumerator from the --tab error path, promoted to a success path.
    // NB it calls expand_tabs(), which CONSTRUCTS the sibling sources — on an
    // .h5ad that is real work, not a metadata peek.
    if (cfg.list_tabs) {
        std::printf("%s\n", src->tab_label().c_str());
        for (auto& sib : src->expand_tabs())
            std::printf("%s\n", sib->tab_label().c_str());
        return 0;
    }

    // --schema: print schema + footer, then exit. With --json/--ndjson emit
    // the machine-readable form instead — both were silently ignored here.
    if (cfg.schema_only) {
        if (cfg.json_array || cfg.json_lines) {
            emit_schema_json(*src, format_label_of(*src));
        } else if (cfg.tab.empty()) {
            auto sibs = src->expand_tabs();
            if (!sibs.empty()) {
                std::vector<std::unique_ptr<TabularSource>> tabs;
                tabs.push_back(std::move(src));
                for (auto& sib : sibs) tabs.push_back(std::move(sib));
                render_multitab_schema(tabs, cfg.path);
            } else {
                print_schema_block(*src);
            }
        } else {
            print_schema_block(*src);
        }
        return 0;
    }

    // --stats: Parquet metadata footer dump (no data read).
    if (cfg.stats_only) {
        std::string err = print_stats_only(*src, cfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        return 0;
    }

    // --describe: per-column statistics.
    if (cfg.describe) {
        std::string err = print_describe(*src, cfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        return 0;
    }

    // --distinct: drop duplicate rows over the shown columns, then fall through
    // to the normal path. Runs before --count / --heatmap / output so they see
    // the deduplicated set (`--distinct --count` counts distinct rows, and
    // `--select chrom --distinct` lists distinct chromosomes). Materialises like
    // --sort; it bakes in --select / --filter, so those are cleared inside.
    if (cfg.distinct) {
        std::string derr = build_distinct(src, cfg);
        if (!derr.empty()) { report(cfg.path, derr); return 1; }
    }

    // --count: row count and exit. Reflects any -r region (baked into
    // total_rows()); with --filter, counts the matching rows via a scan.
    if (cfg.count) {
        int64_t total = 0;
        IndexStats ix;
        // Nothing narrows the rows: an index that counts records answers
        // without reading them (samtools idxstats / bcftools index --stats).
        // --samples long makes one row per record × sample, so it scans.
        if (cfg.filter_expr.empty() && cfg.region.empty() && !cfg.distinct &&
            !cfg.pileup && !cfg.contigs && cfg.tab.empty() && cfg.samples != "long" &&
            read_index_stats(cfg.path, &ix)) {
            total = (int64_t)ix.total();
        } else if (cfg.filter_expr.empty()) {
            while (src->total_rows() < 0) src->ensure(src->num_chunks());
            total = src->total_rows();
        } else {
            FilterExpr fx; std::string ferr;
            if (!parse_filter_expr(cfg.filter_expr, *src->schema(), &fx, &ferr)) {
                report(cfg.path, std::string("--filter: ") + ferr); return 1;
            }
            std::vector<int> read_set = union_with_filter({}, fx);
            for (int c = 0; ; ++c) {
                src->ensure(c);
                if (c >= src->num_chunks()) break;
                std::shared_ptr<arrow::Table> tbl;
                if (!src->read_chunk(c, read_set, &tbl).ok() || !tbl) continue;
                total += count_filter_matches(*tbl, fx, read_set);
            }
        }
        if (!src->read_status().ok()) {
            report(cfg.path, shorten_reader_error(src->read_status().ToString()));
            return 1;
        }
        if (cfg.json_array || cfg.json_lines)
            std::printf("{\"rows\": %lld}\n", (long long)total);
        else
            std::printf("%lld\n", (long long)total);
        return 0;
    }

    // --heatmap: render the numeric matrix as a terminal colour heatmap.
    if (cfg.heatmap) {
        std::string err = render_heatmap(*src, cfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        return 0;
    }

    // --value-counts (--unique): distinct value counts per column.
    if (!cfg.unique_cols.empty()) {
        std::string err = print_unique(*src, cfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        return 0;
    }

    // --sample N: reservoir-sample N rows, then fall through to normal output.
    // build_sample applies --filter while reading, so clear it afterwards so
    // the rendering layer doesn't redo the work.
    if (cfg.sample_n > 0) {
        std::string err = build_sample(src, cfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        cfg.filter_expr.clear();
        cfg.head_rows = 0; cfg.head_rows_set = false;  // sample IS the row set
    }

    // --tail N: keep only the last N rows. Like --sample, this fully
    // materialises the result as a MemoryTableSource so every downstream
    // view / export mode renders it identically.
    // Plain text is the exception: build_tail() concatenates every chunk to
    // slice off the last N rows, and `vv --tail 100 /var/log/syslog` is
    // exactly the command where slurping the file is worst. emit_text_stream()
    // keeps a bounded ring instead, so the pipe path skips this. The TUI still
    // materialises (it needs random access), and mark_text() keeps the result
    // rendering as text.
    const bool text_tail_streams = src->is_text() && !tui_wanted(cfg);
    if (cfg.tail_rows > 0 && !text_tail_streams) {
        bool was_text = src->is_text();
        std::string err = build_tail(src, cfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        if (was_text)
            if (auto* m = dynamic_cast<MemoryTableSource*>(src.get())) m->mark_text();
        cfg.filter_expr.clear();
        cfg.head_rows = 0; cfg.head_rows_set = false;
    }

    // The TUI opens with --select / --filter / --sort as its own live state
    // (TuiStart) rather than a baked-in view; validate them here so a typo is
    // reported instead of silently ignored.
    TuiStart tui_start;
    const bool tui_view = tui_wanted(cfg) && !src->is_text();
    if (tui_view) {
        if (!cfg.select_cols.empty()) {
            std::vector<std::string> unknown;
            tui_start.select = select_field_indices(*src, cfg, &unknown, true);
            if (!unknown.empty()) { report(cfg.path, unknown_columns_error(*src, unknown)); return 1; }
        }
        if (!cfg.filter_expr.empty()) {
            FilterExpr fx; std::string ferr;
            if (!parse_filter_expr(cfg.filter_expr, *src->schema(), &fx, &ferr)) {
                report(cfg.path, std::string("--filter: ") + ferr); return 1;
            }
        }
    }
    // The TUI sorts by an on-screen column; a --sort column left out by
    // --select is materialised as before.
    bool tui_sorts = false;
    if (tui_view && !cfg.sort_col.empty()) {
        const int si = src->schema()->GetFieldIndex(cfg.sort_col);
        tui_sorts = si >= 0 && (tui_start.select.empty() ||
                                std::find(tui_start.select.begin(), tui_start.select.end(), si)
                                    != tui_start.select.end());
        if (si < 0) { report(cfg.path, "--sort: unknown column '" + cfg.sort_col + "'"); return 1; }
    }

    // --sort COL: materialise + stable-sort the rows, then fall through to the
    // normal view / export path (which now sees a pre-sorted MemoryTableSource).
    if (!cfg.sort_col.empty() && !tui_sorts) {
        std::string err = build_sort(src, cfg, /*project=*/!tui_view);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        cfg.filter_expr.clear();   // applied during the sort's materialisation
    }
    if (tui_view) {
        tui_start.filter = cfg.filter_expr;
        if (tui_sorts) { tui_start.sort_col = cfg.sort_col; tui_start.sort_desc = cfg.sort_desc; }
    }

    // --json / --ndjson: stream JSON rows to stdout.
    if (cfg.json_array || cfg.json_lines) {
        Config jcfg = cfg;
        if (!jcfg.head_rows_set) jcfg.head_rows = 0;
        std::string err = write_json(*src, jcfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        // A streaming source that hit a parse/I/O error mid-file has emitted a
        // truncated result; report it and exit non-zero so pipelines can tell.
        if (!src->read_status().ok()) {
            report(cfg.path, shorten_reader_error(src->read_status().ToString()));
            return 1;
        }
        return 0;
    }

    // --md: GitHub-flavored markdown table to stdout.
    if (cfg.md) {
        Config mcfg = cfg;
        if (!mcfg.head_rows_set) mcfg.head_rows = 0;
        std::string err = write_markdown(*src, mcfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        if (!src->read_status().ok()) {
            report(cfg.path, shorten_reader_error(src->read_status().ToString()));
            return 1;
        }
        return 0;
    }

    // Interactive viewer
    {
        if (tui_wanted(cfg)) {
            // Build tabs: file #0 is the source we already opened; any
            // additional positionals get opened here. Open errors abort
            // start-up so the user sees them before the TUI takes over.
            std::vector<std::unique_ptr<TabularSource>> tab_srcs;
            tab_srcs.push_back(std::move(src));
            // SQLite: a single positional that points at a multi-table
            // database expands into one tab per user table. The handle
            // is shared (refcounted) across the sibling sources.
            for (auto& s : sqlite_sibling_tables(tab_srcs.back().get()))
                tab_srcs.push_back(std::move(s));
            // Spreadsheet (.xlsx today, future .ods): one tab per sheet,
            // sharing the underlying workbook handle.
            for (auto& s : workbook_siblings(*tab_srcs.back()))
                tab_srcs.push_back(std::move(s));
            for (size_t i = 1; i < cfg.paths.size(); ++i) {
                std::string why = preflight_path(cfg.paths[i]);
                if (!why.empty()) { report(cfg.paths[i], why); return 1; }
                std::unique_ptr<TabularSource> s;
                std::string err = open_source(cfg.paths[i], cfg, &s);
                if (!err.empty()) {
                    std::string detail = err;
                    const std::string pfx = "Cannot open '" + cfg.paths[i] + "': ";
                    if (detail.rfind(pfx, 0) == 0) detail.erase(0, pfx.size());
                    report(cfg.paths[i], shorten_reader_error(std::move(detail)));
                    return 1;
                }
                tab_srcs.push_back(std::move(s));
                for (auto& es : sqlite_sibling_tables(tab_srcs.back().get()))
                    tab_srcs.push_back(std::move(es));
                for (auto& es : workbook_siblings(*tab_srcs.back()))
                    tab_srcs.push_back(std::move(es));
            }
            // The TUI takes ownership of the sources while it runs. If
            // tui.run() fails (e.g. unsupported terminal), reclaim the
            // first source so the non-interactive fall-through paths
            // below can still use *src.
            if (run_table_viewer(std::move(tab_srcs), cfg, tui_start, &src)) return 0;
            // Falling back to a non-interactive view: apply a sort the TUI
            // was going to do.
            if (tui_sorts) {
                std::string err = build_sort(src, cfg);
                if (!err.empty()) { report(cfg.path, err); return 1; }
                cfg.filter_expr.clear();
            }
            // The TUI could not start. With an explicit -i that is an error;
            // for auto-launch (a TTY, no -i) it used to fall through silently,
            // so the user saw a static table with no hint the interactive
            // viewer was even attempted. Note the fallback either way.
            if (cfg.interactive)
                std::fprintf(stderr, "error: cannot start the interactive viewer "
                                     "(no terminal, or missing terminfo)\n");
            else
                std::fprintf(stderr, "vv: interactive viewer unavailable "
                             "(terminal init failed); showing non-interactive "
                             "output\n");
        }
    }

    // Plain text with no TUI (a pipe, -n N, --no-interactive, or a terminal
    // vv could not initialise): dump the file verbatim. Reached only after
    // the interactive block above declined, so "is the TUI running?" is
    // decided in exactly one place. --count / --list-columns / --filter have
    // already had their say further up.
    if (src->is_text()) return emit_text_stream(*src, cfg);

    // Delimited output
    if (cfg.delimiter) {
        Config dcfg = cfg;
        if (!dcfg.head_rows_set) dcfg.head_rows = 0;
        std::string werr = write_delimited(*src, dcfg);
        if (!werr.empty()) { report(cfg.path, werr); return 1; }
        // A streaming source that hit a parse/I/O error mid-file has emitted a
        // truncated result; report it and exit non-zero so pipelines can tell.
        if (!src->read_status().ok()) {
            report(cfg.path, shorten_reader_error(src->read_status().ToString()));
            return 1;
        }
        return 0;
    }

    // Parquet output
    if (!cfg.parquet_out.empty()) {
        Config pcfg = cfg;
        if (!pcfg.head_rows_set) pcfg.head_rows = 0;  // default to "all rows"
        std::string err = write_parquet(*src, pcfg);
        if (!err.empty()) {
            report(cfg.path, err);
            return 1;
        }
        // Never hand back a silently truncated conversion.
        if (!src->read_status().ok()) {
            report(cfg.path, shorten_reader_error(src->read_status().ToString()));
            return 1;
        }
        return 0;
    }

    // Arrow IPC / Feather output
    if (!cfg.arrow_out.empty()) {
        Config acfg = cfg;
        if (!acfg.head_rows_set) acfg.head_rows = 0;  // default to "all rows"
        std::string err = write_arrow(*src, acfg);
        if (!err.empty()) {
            report(cfg.path, err);
            return 1;
        }
        if (!src->read_status().ok()) {
            report(cfg.path, shorten_reader_error(src->read_status().ToString()));
            return 1;
        }
        return 0;
    }

    // Table display
    {
        if (!cfg.vertical && cfg.tab.empty()) {
            auto sibs = src->expand_tabs();
            if (!sibs.empty()) {
                std::vector<std::unique_ptr<TabularSource>> tabs;
                tabs.push_back(std::move(src));
                for (auto& sib : sibs) tabs.push_back(std::move(sib));
                std::string terr = render_multitab_table(tabs, cfg);
                if (!terr.empty()) { report(cfg.path, terr); return 1; }
                return 0;   // src moved into tabs; skip the read_status check
            }
        }
        std::string terr = cfg.vertical ? print_vertical_table(*src, cfg)
                                        : print_table(*src, cfg);
        if (!terr.empty()) { report(cfg.path, terr); return 1; }
    }
    if (!src->read_status().ok()) {
        report(cfg.path, shorten_reader_error(src->read_status().ToString()));
        return 1;
    }
    return 0;
}
#endif  // VV_CORE_LIB
