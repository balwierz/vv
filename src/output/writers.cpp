// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// TSV / CSV, Markdown, Parquet / Arrow and JSON writers.

#include "internal.hpp"

// ── Delimited output ──────────────────────────────────────────────────────────
// (write_csv_field is defined above)

// Returns "" on success, an error message otherwise — same contract as
// write_json / write_parquet, so a bad --select or --filter reaches main()'s
// report() and exits non-zero instead of printing to stderr and exiting 0.
std::string write_delimited(TabularSource& src, const Config& cfg) {
    ExactFloats exact;   // an export writes every digit a float has
    char sep = cfg.delimiter;
    FILE* out = out_stream();
    // In delimiter mode default to all rows; honour -n if explicitly given.
    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;

    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> col_indices = have_filter
        ? union_with_filter(requested, fx) : requested;
    int  show_cols = (int)requested.size();

    // Position of each *requested* column within the read projection.
    std::vector<int> req_in_read(requested.size());
    for (size_t k = 0; k < requested.size(); ++k) {
        for (size_t j = 0; j < col_indices.size(); ++j)
            if (col_indices[j] == requested[k]) { req_in_read[k] = (int)j; break; }
    }

    if (!cfg.no_header) {
        for (int ci = 0; ci < show_cols; ++ci) {
            if (ci) std::fputc(sep, out);
            write_csv_field(src.schema()->field(requested[ci])->name(), sep);
        }
        std::fputc('\n', out);
    }

    struct ChunkCursor {
        const arrow::ChunkedArray* col;
        int     chunk_idx    = 0;
        int64_t row_in_chunk = 0;
        const arrow::Array& current() const { return *col->chunk(chunk_idx); }
        void advance() {
            if (++row_in_chunk >= col->chunk(chunk_idx)->length()) {
                ++chunk_idx; row_in_chunk = 0;
            }
        }
    };
    auto print_rows = [&](const arrow::Table& table, int64_t n_rows) {
        // Each cursor points at one column at the requested-output position
        // (so we read from the read projection but emit in user order).
        std::vector<ChunkCursor> cursors;
        cursors.reserve(show_cols);
        for (int ci = 0; ci < show_cols; ++ci)
            cursors.push_back({table.column(req_in_read[ci]).get()});
        for (int64_t r = 0; r < n_rows; ++r) {
            for (int ci = 0; ci < show_cols; ++ci) {
                if (ci) std::fputc(sep, out);
                auto& cur = cursors[ci];
                std::string val = cell_to_string(cur.current(), cur.row_in_chunk);
                if (val != NULL_SYMBOL) write_csv_field(val, sep);
                cur.advance();
            }
            std::fputc('\n', out);
        }
    };

    // -n on Parquet: ask the source for just `head_rows` rows. ParquetSource's
    // override fetches only the row groups that contain them, skipping a scan
    // of the rest of the file. Skip this fast path when --filter is active
    // (we'd over-count rows once the filter prunes some).
    if (cfg.head_rows > 0 && !have_filter) {
        std::shared_ptr<arrow::Table> table;
        if (src.read_first(rows_left, col_indices, &table).ok() && table) {
            print_rows(*table, std::min(table->num_rows(), rows_left));
            export_count(std::min(table->num_rows(), rows_left));
            return "";
        }
        // Fall through to streaming path on error.
    }

    for (int c = 0; rows_left > 0; ++c) {
        if (export_canceled()) return kExportCanceled;
        src.ensure(c);
        if (c >= src.num_chunks()) break;

        std::shared_ptr<arrow::Table> table;
        auto st = src.read_chunk(c, col_indices, &table);
        if (!st.ok()) {
            std::fprintf(stderr, "Warning: error reading chunk %d: %s\n",
                         c, st.ToString().c_str());
            continue;
        }
        if (have_filter) table = apply_filter(table, fx, col_indices);
        if (!table || table->num_rows() == 0) continue;

        int64_t rg_rows = std::min(table->num_rows(), rows_left);
        rows_left -= rg_rows;

        print_rows(*table, rg_rows);
        export_count(rg_rows);
    }
    return "";
}

// ── GitHub-flavored Markdown table output ────────────────────────────────────
//
// Stream the source as a pipe-delimited markdown table. Designed for pasting
// into GitHub / GitLab issues, READMEs, and other markdown-rendering surfaces.
// Cells are HTML-escaped only minimally — pipes are backslash-escaped (the
// one character that breaks the table structure) and embedded newlines
// become <br>. Honours --select, --filter, -n, --no-header.
static std::string md_escape_cell(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '|':  out += "\\|"; break;
            case '\n': out += "<br>"; break;
            case '\r': /* drop */ break;
            default:   out += c;
        }
    }
    return out;
}

// Returns "" on success, an error message otherwise (see write_delimited).
std::string write_markdown(TabularSource& src, const Config& cfg) {
    ExactFloats exact;   // an export writes every digit a float has
    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;

    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> col_indices = have_filter
        ? union_with_filter(requested, fx) : requested;
    int show_cols = (int)requested.size();

    std::vector<int> req_in_read(requested.size());
    for (size_t k = 0; k < requested.size(); ++k) {
        for (size_t j = 0; j < col_indices.size(); ++j)
            if (col_indices[j] == requested[k]) { req_in_read[k] = (int)j; break; }
    }

    if (!cfg.no_header) {
        for (int ci = 0; ci < show_cols; ++ci)
            std::printf("| %s ", md_escape_cell(src.schema()->field(requested[ci])->name()).c_str());
        std::printf("|\n");
        for (int ci = 0; ci < show_cols; ++ci) std::printf("| --- ");
        std::printf("|\n");
    }

    struct ChunkCursor {
        const arrow::ChunkedArray* col;
        int     chunk_idx    = 0;
        int64_t row_in_chunk = 0;
        const arrow::Array& current() const { return *col->chunk(chunk_idx); }
        void advance() {
            if (++row_in_chunk >= col->chunk(chunk_idx)->length()) {
                ++chunk_idx; row_in_chunk = 0;
            }
        }
    };
    auto print_rows = [&](const arrow::Table& table, int64_t n_rows) {
        std::vector<ChunkCursor> cursors;
        cursors.reserve(show_cols);
        for (int ci = 0; ci < show_cols; ++ci)
            cursors.push_back({table.column(req_in_read[ci]).get()});
        for (int64_t r = 0; r < n_rows; ++r) {
            for (int ci = 0; ci < show_cols; ++ci) {
                auto& cur = cursors[ci];
                std::string val = cell_to_display_string(cur.current(), cur.row_in_chunk);
                if (val == NULL_SYMBOL) val.clear();
                std::printf("| %s ", md_escape_cell(val).c_str());
                cur.advance();
            }
            std::printf("|\n");
        }
    };

    if (cfg.head_rows > 0 && !have_filter) {
        std::shared_ptr<arrow::Table> table;
        if (src.read_first(rows_left, col_indices, &table).ok() && table) {
            print_rows(*table, std::min(table->num_rows(), rows_left));
            return "";
        }
    }

    for (int c = 0; rows_left > 0; ++c) {
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> table;
        auto st = src.read_chunk(c, col_indices, &table);
        if (!st.ok()) {
            std::fprintf(stderr, "Warning: error reading chunk %d: %s\n",
                         c, st.ToString().c_str());
            continue;
        }
        if (have_filter) table = apply_filter(table, fx, col_indices);
        if (!table || table->num_rows() == 0) continue;
        int64_t take = std::min(table->num_rows(), rows_left);
        rows_left -= take;
        print_rows(*table, take);
    }
    return "";
}

// ── Parquet output ───────────────────────────────────────────────────────────

// Map a user-facing codec name to Arrow's enum. Returns false on error.
static bool resolve_compression(const std::string& name,
                                arrow::Compression::type* out) {
    if      (name == "zstd")    *out = arrow::Compression::ZSTD;
    else if (name == "snappy")  *out = arrow::Compression::SNAPPY;
    else if (name == "gzip")    *out = arrow::Compression::GZIP;
    else if (name == "lz4")     *out = arrow::Compression::LZ4;
    else if (name == "none" ||
             name == "uncompressed") *out = arrow::Compression::UNCOMPRESSED;
    else return false;
    return true;
}

// Build an mkstemps() template under $TMPDIR (falling back to the system's
// temporary directory). The `--parquet -` / `--arrow -` spools need a
// seekable file; hardcoding /tmp breaks containers whose /tmp is tiny or
// read-only.
static std::string spool_template(const char* name) {
    return temp_dir() + "/" + name;
}

// Stream the source's chunks into a Parquet file at cfg.parquet_out.
// Returns "" on success, an error message otherwise. Writes an
// "[N rows -> path]" summary to stderr on success.
std::string write_parquet(TabularSource& src, const Config& cfg) {
    arrow::Compression::type codec;
    if (!resolve_compression(cfg.compression, &codec))
        return "Unknown --compression '" + cfg.compression +
               "' (try: zstd, snappy, gzip, lz4, none)";

    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);
    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> col_indices = have_filter
        ? union_with_filter(requested, fx) : requested;

    // Build the projected schema (so the output Parquet has only the
    // user-visible columns, in user-requested order — filter cols loaded
    // for evaluation but dropped before writing).
    arrow::FieldVector fields;
    for (int i : requested) fields.push_back(src.schema()->field(i));
    auto out_schema = arrow::schema(fields);

    // `--parquet -`: Parquet's footer-at-end requires seekable writes, so we
    // can't stream directly to stdout. Spool to a temp file, then copy it to
    // stdout after the writer closes. unlink() up front so the file
    // disappears on crash and isn't left behind.
    bool to_stdout = (cfg.parquet_out == "-");
    std::string out_path = cfg.parquet_out;
    int tmp_fd = -1;
    if (to_stdout) {
        out_path = spool_template("vv-parquet-XXXXXX.parquet");
        tmp_fd = make_temp_file(&out_path, 8);  // suffix length = ".parquet" = 8
        if (tmp_fd < 0)
            return std::string("Cannot create temp file for --parquet -: ") +
                   std::strerror(errno);
        // Keep fd open (Arrow opens the path by name); we'll clean up below.
        ::close(tmp_fd);
        tmp_fd = -1;
    }

    auto sink_or = arrow::io::FileOutputStream::Open(out_path);
    if (!sink_or.ok()) {
        if (to_stdout) ::unlink(out_path.c_str());
        return "Cannot open '" + out_path + "' for write: " +
               sink_or.status().ToString();
    }
    auto sink = sink_or.ValueOrDie();

    auto wprops = parquet::WriterProperties::Builder()
                      .compression(codec)
                      ->created_by("vv " + std::string(kVersion))
                      ->build();
    auto aprops = parquet::ArrowWriterProperties::Builder()
                      .store_schema()        // round-trip Arrow types
                      ->build();

    auto writer_or = parquet::arrow::FileWriter::Open(
        *out_schema, arrow::default_memory_pool(), sink, wprops, aprops);
    if (!writer_or.ok())
        return "Parquet writer init failed: " + writer_or.status().ToString();
    auto writer = std::move(writer_or).ValueOrDie();

    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;
    int64_t total = 0;
    for (int c = 0; rows_left > 0; ++c) {
        if (export_canceled()) {
            (void)writer->Close();
            (void)sink->Close();
            if (to_stdout) ::unlink(out_path.c_str());
            return kExportCanceled;
        }
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> table;
        auto st = src.read_chunk(c, col_indices, &table);
        if (!st.ok()) {
            std::fprintf(stderr, "Warning: error reading chunk %d: %s\n",
                         c, st.ToString().c_str());
            continue;
        }
        if (have_filter) table = apply_filter(table, fx, col_indices);
        if (!table || table->num_rows() == 0) continue;
        int64_t take = std::min(table->num_rows(), rows_left);
        if (take < table->num_rows()) table = table->Slice(0, take);
        // Project down to user-requested columns (drop filter-only loads).
        table = project_to_requested(table, col_indices, requested);
        st = writer->WriteTable(*table, table->num_rows());
        if (!st.ok()) return "WriteTable failed: " + st.ToString();
        total += take;
        rows_left -= take;
        export_count(take);
    }

    auto cs = writer->Close();
    if (!cs.ok()) {
        if (to_stdout) ::unlink(out_path.c_str());
        return "Parquet Close failed: " + cs.ToString();
    }
    auto fc = sink->Close();
    if (!fc.ok()) {
        if (to_stdout) ::unlink(out_path.c_str());
        return "File close failed: " + fc.ToString();
    }

    if (to_stdout) {
        // Stream the temp file to stdout.
        FILE* in = std::fopen(out_path.c_str(), "rb");
        if (!in) {
            std::string err = std::string("cannot read back temp file: ") +
                              std::strerror(errno);
            ::unlink(out_path.c_str());
            return err;
        }
        constexpr size_t BUF = 64 * 1024;
        std::vector<char> buf(BUF);
        std::fflush(stdout);
        while (true) {
            size_t n = std::fread(buf.data(), 1, BUF, in);
            if (n == 0) break;
            if (std::fwrite(buf.data(), 1, n, stdout) != n) {
                std::fclose(in);
                ::unlink(out_path.c_str());
                return std::string("write to stdout failed: ") + std::strerror(errno);
            }
        }
        std::fclose(in);
        ::unlink(out_path.c_str());
        std::fprintf(stderr, "%s[%lld rows → stdout, %s]%s\n",
                     g_color.meta_key, (long long)total,
                     cfg.compression.c_str(), g_color.reset);
    } else if (!t_export) {
        std::fprintf(stderr, "%s[%lld rows → %s, %s]%s\n",
                     g_color.meta_key, (long long)total,
                     cfg.parquet_out.c_str(), cfg.compression.c_str(),
                     g_color.reset);
    }
    return "";
}

// Stream the source's chunks into an Arrow IPC file (a.k.a. Feather v2) at
// cfg.arrow_out. Same shape as write_parquet — column projection + --filter, an
// "[N rows → path]" stderr summary, and a temp-file spool for `-` (stdout).
std::string write_arrow(TabularSource& src, const Config& cfg) {
    // IPC body compression is limited to zstd / lz4 / none (no snappy/gzip).
    std::shared_ptr<arrow::util::Codec> ipc_codec;
    const std::string& comp = cfg.compression;
    if (comp.empty() || comp == "none" || comp == "uncompressed") {
        // uncompressed
    } else if (comp == "zstd") {
        ipc_codec = *arrow::util::Codec::Create(arrow::Compression::ZSTD);
    } else if (comp == "lz4") {
        ipc_codec = *arrow::util::Codec::Create(arrow::Compression::LZ4_FRAME);
    } else {
        return "Arrow/Feather output supports --compression zstd, lz4, or none "
               "(not '" + comp + "')";
    }

    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);
    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> col_indices = have_filter
        ? union_with_filter(requested, fx) : requested;

    arrow::FieldVector fields;
    for (int i : requested) fields.push_back(src.schema()->field(i));
    auto out_schema = arrow::schema(fields);

    // `--arrow -`: spool to a temp file, then copy to stdout after close (matches
    // --parquet -). unlink() up front so it disappears on crash.
    bool to_stdout = (cfg.arrow_out == "-");
    std::string out_path = cfg.arrow_out;
    if (to_stdout) {
        out_path = spool_template("vv-arrow-XXXXXX.arrow");
        int fd = make_temp_file(&out_path, 6);   // suffix ".arrow" = 6
        if (fd < 0)
            return std::string("Cannot create temp file for --arrow -: ") +
                   std::strerror(errno);
        ::close(fd);
    }

    auto sink_or = arrow::io::FileOutputStream::Open(out_path);
    if (!sink_or.ok()) {
        if (to_stdout) ::unlink(out_path.c_str());
        return "Cannot open '" + out_path + "' for write: " +
               sink_or.status().ToString();
    }
    auto sink = sink_or.ValueOrDie();

    auto wopts = arrow::ipc::IpcWriteOptions::Defaults();
    if (ipc_codec) wopts.codec = ipc_codec;
    auto writer_or = arrow::ipc::MakeFileWriter(sink, out_schema, wopts);
    if (!writer_or.ok()) {
        if (to_stdout) ::unlink(out_path.c_str());
        return "Arrow IPC writer init failed: " + writer_or.status().ToString();
    }
    auto writer = std::move(writer_or).ValueOrDie();

    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;
    int64_t total = 0;
    for (int c = 0; rows_left > 0; ++c) {
        if (export_canceled()) {
            (void)writer->Close();
            (void)sink->Close();
            if (to_stdout) ::unlink(out_path.c_str());
            return kExportCanceled;
        }
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> table;
        auto st = src.read_chunk(c, col_indices, &table);
        if (!st.ok()) {
            std::fprintf(stderr, "Warning: error reading chunk %d: %s\n",
                         c, st.ToString().c_str());
            continue;
        }
        if (have_filter) table = apply_filter(table, fx, col_indices);
        if (!table || table->num_rows() == 0) continue;
        int64_t take = std::min(table->num_rows(), rows_left);
        if (take < table->num_rows()) table = table->Slice(0, take);
        table = project_to_requested(table, col_indices, requested);
        st = writer->WriteTable(*table);
        if (!st.ok()) {
            if (to_stdout) ::unlink(out_path.c_str());
            return "WriteTable failed: " + st.ToString();
        }
        total += take;
        rows_left -= take;
        export_count(take);
    }

    auto cs = writer->Close();
    if (!cs.ok()) {
        if (to_stdout) ::unlink(out_path.c_str());
        return "Arrow Close failed: " + cs.ToString();
    }
    auto fc = sink->Close();
    if (!fc.ok()) {
        if (to_stdout) ::unlink(out_path.c_str());
        return "File close failed: " + fc.ToString();
    }

    const char* clabel = ipc_codec ? comp.c_str() : "none";
    if (to_stdout) {
        FILE* in = std::fopen(out_path.c_str(), "rb");
        if (!in) {
            std::string err = std::string("cannot read back temp file: ") +
                              std::strerror(errno);
            ::unlink(out_path.c_str());
            return err;
        }
        constexpr size_t BUF = 64 * 1024;
        std::vector<char> buf(BUF);
        std::fflush(stdout);
        while (true) {
            size_t n = std::fread(buf.data(), 1, BUF, in);
            if (n == 0) break;
            if (std::fwrite(buf.data(), 1, n, stdout) != n) {
                std::fclose(in);
                ::unlink(out_path.c_str());
                return std::string("write to stdout failed: ") + std::strerror(errno);
            }
        }
        std::fclose(in);
        ::unlink(out_path.c_str());
        std::fprintf(stderr, "%s[%lld rows → stdout, %s]%s\n",
                     g_color.meta_key, (long long)total, clabel, g_color.reset);
    } else if (!t_export) {
        std::fprintf(stderr, "%s[%lld rows → %s, %s]%s\n",
                     g_color.meta_key, (long long)total,
                     cfg.arrow_out.c_str(), clabel, g_color.reset);
    }
    return "";
}

// ── JSON / JSON-Lines output ─────────────────────────────────────────────────

// Quote a string as a JSON string literal.
void json_emit_string(const std::string& v) {
    FILE* out = out_stream();
    std::fputc('"', out);
    for (unsigned char c : v) {
        switch (c) {
            case '"':  std::fputs("\\\"", out); break;
            case '\\': std::fputs("\\\\", out); break;
            case '\b': std::fputs("\\b", out);  break;
            case '\f': std::fputs("\\f", out);  break;
            case '\n': std::fputs("\\n", out);  break;
            case '\r': std::fputs("\\r", out);  break;
            case '\t': std::fputs("\\t", out);  break;
            default:
                if (c < 0x20) std::fprintf(out, "\\u%04x", c);
                else          std::fputc((int)c, out);
        }
    }
    std::fputc('"', out);
}

// Emit one Arrow cell as a JSON value. Numbers go bare, strings are
// quoted, booleans as true/false, nulls as null; lists, structs and maps
// as JSON arrays / objects (see below).
static void json_emit_cell(const arrow::Array& arr, int64_t row) {
    FILE* out = out_stream();
    if (arr.IsNull(row)) { std::fputs("null", out); return; }
    if (arr.type_id() == arrow::Type::EXTENSION &&
        static_cast<const arrow::ExtensionType&>(*arr.type()).extension_name() == "arrow.bool8") {
        std::fputs(cell_to_string(arr, row) == "true" ? "true" : "false", out);
        return;
    }
    switch (arr.type_id()) {
        case arrow::Type::BOOL:
            std::fputs(static_cast<const arrow::BooleanArray&>(arr).Value(row)
                       ? "true" : "false", out);
            return;
        case arrow::Type::FLOAT: case arrow::Type::DOUBLE: {
            // JSON has no NaN / Infinity literal: write null, as JSON.stringify
            // and pandas' to_json do, rather than an unparseable `nan`.
            const double v = arr.type_id() == arrow::Type::FLOAT
                ? (double)static_cast<const arrow::FloatArray&>(arr).Value(row)
                : static_cast<const arrow::DoubleArray&>(arr).Value(row);
            if (!std::isfinite(v)) { std::fputs("null", out); return; }
            std::fputs(cell_to_string(arr, row).c_str(), out);
            return;
        }
        case arrow::Type::INT8: case arrow::Type::INT16: case arrow::Type::INT32:
        case arrow::Type::INT64: case arrow::Type::UINT8: case arrow::Type::UINT16:
        case arrow::Type::UINT32: case arrow::Type::UINT64:
            // cell_to_string already produces a decimal representation
            // suitable for JSON for these types.
            std::fputs(cell_to_string(arr, row).c_str(), out);
            return;
        case arrow::Type::STRING: case arrow::Type::LARGE_STRING:
            json_emit_string(cell_to_string(arr, row));
            return;
        // Nested values as JSON, recursively (each element typed like a
        // top-level cell): a list as an array, a struct as an object, a map as
        // an object whose keys are the keys' text.
        case arrow::Type::LIST: case arrow::Type::LARGE_LIST: {
            int64_t off, len;
            std::shared_ptr<arrow::Array> values;
            if (arr.type_id() == arrow::Type::LIST) {
                auto& la = static_cast<const arrow::ListArray&>(arr);
                off = la.value_offset(row); len = la.value_length(row); values = la.values();
            } else {
                auto& la = static_cast<const arrow::LargeListArray&>(arr);
                off = la.value_offset(row); len = la.value_length(row); values = la.values();
            }
            std::fputc('[', out);
            for (int64_t i = 0; i < len; ++i) {
                if (i) std::fputs(", ", out);
                json_emit_cell(*values, off + i);
            }
            std::fputc(']', out);
            return;
        }
        case arrow::Type::FIXED_SIZE_LIST: {
            auto& la = static_cast<const arrow::FixedSizeListArray&>(arr);
            const int32_t n = la.list_type()->list_size();
            const int64_t off = la.value_offset(row);
            std::fputc('[', out);
            for (int32_t i = 0; i < n; ++i) {
                if (i) std::fputs(", ", out);
                json_emit_cell(*la.values(), off + i);
            }
            std::fputc(']', out);
            return;
        }
        case arrow::Type::STRUCT: {
            auto& sa = static_cast<const arrow::StructArray&>(arr);
            const auto& st = static_cast<const arrow::StructType&>(*arr.type());
            std::fputc('{', out);
            for (int f = 0; f < st.num_fields(); ++f) {
                if (f) std::fputs(", ", out);
                json_emit_string(st.field(f)->name());
                std::fputs(": ", out);
                json_emit_cell(*sa.field(f), row);   // field() is offset-adjusted
            }
            std::fputc('}', out);
            return;
        }
        case arrow::Type::MAP: {
            auto& ma = static_cast<const arrow::MapArray&>(arr);
            const int64_t off = ma.value_offset(row), len = ma.value_length(row);
            std::fputc('{', out);
            for (int64_t i = 0; i < len; ++i) {
                if (i) std::fputs(", ", out);
                json_emit_string(cell_to_string(*ma.keys(), off + i));
                std::fputs(": ", out);
                json_emit_cell(*ma.items(), off + i);
            }
            std::fputc('}', out);
            return;
        }
        case arrow::Type::DICTIONARY: {
            // The decoded value, typed (a dictionary of ints stays a number).
            auto& da = static_cast<const arrow::DictionaryArray&>(arr);
            const int64_t k = da.GetValueIndex(row);
            if (k < 0 || k >= da.dictionary()->length()) { std::fputs("null", out); return; }
            json_emit_cell(*da.dictionary(), k);
            return;
        }
        case arrow::Type::NA:
            std::fputs("null", out);
            return;
        default:
            // Other types (dates, decimals, binary, …): their text form.
            json_emit_string(cell_to_string(arr, row));
            return;
    }
}

std::string write_json(TabularSource& src, const Config& cfg) {
    ExactFloats exact;   // an export writes every digit a float has
    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);
    if (requested.empty()) return "";

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

    FILE* out = out_stream();
    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;
    bool first_row = true;
    if (cfg.json_array) std::fputc('[', out);

    auto emit_row = [&](const arrow::Table& tbl, int64_t r) {
        if (!first_row) std::fputs(cfg.json_array ? ",\n" : "\n", out);
        first_row = false;
        std::fputc('{', out);
        for (size_t k = 0; k < requested.size(); ++k) {
            if (k) std::fputs(", ", out);
            json_emit_string(src.schema()->field(requested[k])->name());
            std::fputs(": ", out);
            // Find the column position in `tbl` (it was loaded as read_set).
            int p = -1;
            for (size_t j = 0; j < read_set.size(); ++j)
                if (read_set[j] == requested[k]) { p = (int)j; break; }
            auto col = tbl.column(p);
            int64_t off = r;
            for (auto& ch : col->chunks()) {
                if (off < ch->length()) { json_emit_cell(*ch, off); break; }
                off -= ch->length();
            }
        }
        std::fputc('}', out);
    };

    for (int c = 0; rows_left > 0; ++c) {
        if (export_canceled()) return kExportCanceled;
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> chunk;
        if (!src.read_chunk(c, read_set, &chunk).ok()) continue;
        if (have_filter) chunk = apply_filter(chunk, fx, read_set);
        if (!chunk || chunk->num_rows() == 0) continue;
        int64_t take = std::min(chunk->num_rows(), rows_left);
        for (int64_t r = 0; r < take; ++r) emit_row(*chunk, r);
        rows_left -= take;
        export_count(take);
    }
    if (cfg.json_array) std::fputs("]\n", out);
    else if (!first_row) std::fputc('\n', out);
    return "";
}
