// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// The non-interactive table, vertical view and schema footer.

#include "internal.hpp"

// ── ASCII table drawing ───────────────────────────────────────────────────────

struct Column {
    std::string              header;
    bool                     right_align;
    bool                     is_index = false;
    bool                     is_bool  = false;
    bool                     is_rgb   = false;
    std::vector<std::string> cells;
    std::vector<bool>        cut;      // cells[i] was shortened by truncate()
    int                      width;
};

enum class SepKind { Top, Middle, Bottom };

static void draw_separator(const std::vector<Column>& cols,
                           SepKind kind = SepKind::Middle) {
    const char* left;
    const char* sep;
    const char* right;
    switch (kind) {
        case SepKind::Top:    left = g_box->tl; sep = g_box->tt; right = g_box->tr; break;
        case SepKind::Bottom: left = g_box->bl; sep = g_box->bt; right = g_box->br; break;
        case SepKind::Middle: default:
                              left = g_box->lt; sep = g_box->x;  right = g_box->rt; break;
    }
    std::printf("%s%s", g_color.border, left);
    for (size_t i = 0; i < cols.size(); ++i) {
        for (int j = 0; j < cols[i].width + 2; ++j) std::printf("%s", g_box->hline);
        std::printf("%s", (i + 1 == cols.size()) ? right : sep);
    }
    std::printf("%s\n", g_color.reset);
}

// Emit one cell with color, proper padding, but no border characters.
// Returns nothing; writes directly to stdout.
// `cut`: truncate() shortened the value, so its trailing ellipsis is the
// truncation marker (dimmed); a datum that itself ends in "…" is not.
static void emit_cell(const Column& col, const std::string& val,
                      bool right_align, bool is_header, bool cut) {
    int pad = col.width - display_width(val);

    // Choose foreground color for the content
    const char* fg = "";
    if (*g_color.reset) {
        if (is_header) {
            fg = g_color.header;
        } else if (col.is_index) {
            fg = g_color.row_idx;
        } else if (val == NULL_SYMBOL) {
            fg = g_color.null_val;
        } else if (col.is_bool) {
            fg = (val == "true") ? g_color.bool_true : g_color.bool_false;
        } else if (right_align) {
            fg = g_color.number;
        }
    }

    // For truncated values, render the body normally and the marker dimmed.
    // Both "…" and the ASCII "..." are 3 bytes, so the body is val.size()-3 bytes
    // regardless of style; only the marker glyph differs (g_box->ell).
    bool truncated = cut && !is_header && val.size() >= 3 &&
                     val.compare(val.size() - 3, 3, g_box->ell) == 0;

    if (right_align) {
        std::printf(" %*s", pad, "");   // leading spaces (no color)
        if (truncated) {
            std::printf("%s%.*s%s%s%s%s",
                fg, (int)val.size() - 3, val.c_str(),   // body
                g_color.reset, g_color.trunc, g_box->ell, g_color.reset);
        } else {
            std::printf("%s%s%s", fg, val.c_str(), *fg ? g_color.reset : "");
        }
    } else {
        if (truncated) {
            std::printf(" %s%.*s%s%s%s%s%*s",
                fg, (int)val.size() - 3, val.c_str(),   // body
                g_color.reset, g_color.trunc, g_box->ell, g_color.reset,
                pad, "");
        } else {
            std::printf(" %s%s%s%*s",
                fg, val.c_str(), *fg ? g_color.reset : "", pad, "");
        }
    }
}

// `cut[i]`: vals[i] was shortened by truncate() (empty for a header row).
static void draw_row(const std::vector<Column>& cols,
                     const std::vector<std::string>& vals,
                     const std::vector<bool>& right_align,
                     const std::vector<bool>& cut,
                     bool is_header = false) {
    for (std::size_t i = 0; i < cols.size(); ++i) {
        std::printf("%s%s%s", g_color.border, g_box->vline, g_color.reset);
        int r, gv, b;
        if (!is_header && cols[i].is_rgb && *g_color.reset
                       && parse_rgb(vals[i], &r, &gv, &b)) {
            // Truecolor background bar using ANSI 24-bit escape; width = col.width + 2
            std::printf(" \033[48;2;%d;%d;%dm%*s\033[0m ", r, gv, b, cols[i].width, "");
        } else {
            emit_cell(cols[i], vals[i], right_align[i], is_header,
                      i < cut.size() && cut[i]);
            std::printf(" ");
        }
    }
    std::printf("%s%s%s\n", g_color.border, g_box->vline, g_color.reset);
}

// ── Table display (non-interactive) ──────────────────────────────────────────

// Detect terminal width. Falls back to $COLUMNS, then 80.
int detect_terminal_width() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    if (const char* c = std::getenv("COLUMNS")) {
        int w = std::atoi(c);
        if (w > 0) return w;
    }
    return 80;
}

// "Vertical head": transpose the preview so each field is a row and each
// record is a column. Show as many record-columns as fit in the terminal so
// wide tables can be scanned by scrolling vertically. Default when the
// binary is invoked as `vh`; opt in elsewhere with `--vertical`.
// Forward decl: shared by print_table and the stand-alone --schema mode.

// Returns "" on success, an error message otherwise, so a bad --select /
// --filter exits non-zero instead of printing and exiting 0.
std::string print_vertical_table(TabularSource& src, const Config& cfg) {
    auto schema     = src.schema();
    int  n_fields   = schema->num_fields();
    std::vector<std::string> unknown;
    std::vector<int> col_indices = select_field_indices(src, cfg, &unknown);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);
    int  show_fields = (int)col_indices.size();
    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> read_indices = have_filter
        ? union_with_filter(col_indices, fx) : col_indices;

    int64_t tr          = src.total_rows();
    int64_t rows_wanted = (cfg.head_rows <= 0) ? (tr >= 0 ? tr : INT64_MAX)
                                               : (int64_t)cfg.head_rows;

    std::shared_ptr<arrow::Table> data;
    if (cfg.head_rows > 0 && !have_filter) {
        (void)src.read_first(rows_wanted, read_indices, &data);
    } else {
        int64_t need = rows_wanted;
        for (int c = 0; need > 0; ++c) {
            src.ensure(c);
            if (c >= src.num_chunks()) break;
            std::shared_ptr<arrow::Table> chunk;
            if (!src.read_chunk(c, read_indices, &chunk).ok()) continue;
            if (have_filter) chunk = apply_filter(chunk, fx, read_indices);
            if (!chunk || chunk->num_rows() == 0) continue;
            if (chunk->num_rows() > need) chunk = chunk->Slice(0, need);
            need -= chunk->num_rows();
            if (!data) data = chunk;
            else {
                auto r = arrow::ConcatenateTables({data, chunk});
                if (r.ok()) data = r.ValueOrDie();
            }
        }
    }
    if (!data || data->num_rows() == 0) {
        std::printf("[0 rows x %d columns]\n", n_fields);
        return "";
    }
    if (read_indices != col_indices)
        data = project_to_requested(data, read_indices, col_indices);

    int64_t n_records = data->num_rows();

    // Per-field flag: integer columns skip the max_col_w truncation.
    const int max_w = (src.show_cells_in_full() && !cfg.max_col_w_set) ? (1 << 20) : cfg.max_col_w;
    std::vector<bool> field_is_int(show_fields);
    for (int f = 0; f < show_fields; ++f)
        field_is_int[f] = is_integer_type(schema->field(col_indices[f])->type()->id());

    // Pre-render every cell into rendered[record][field].
    std::vector<std::vector<std::string>> rendered(n_records,
        std::vector<std::string>(show_fields));
    std::vector<std::vector<bool>> rendered_cut(n_records,
        std::vector<bool>((size_t)show_fields, false));
    for (int f = 0; f < show_fields; ++f) {
        int f_src    = col_indices[f];
        auto arr_col = data->column(f);
        int64_t row  = 0;
        for (auto& chunk : arr_col->chunks()) {
            for (int64_t r = 0; r < chunk->length(); ++r, ++row) {
                std::string val = src.format_cell(f_src,
                    cell_to_display_string(*chunk, r));
                if (!field_is_int[f] && truncate_cuts(val, max_w)) {
                    val = truncate(std::move(val), max_w);
                    rendered_cut[row][f] = true;
                }
                rendered[row][f] = std::move(val);
            }
        }
    }

    // Field-name column (left-most): cells are the field names.
    Column field_col;
    field_col.header      = "field";
    field_col.right_align = false;
    field_col.is_index    = true;            // dim-grey foreground
    field_col.width       = (int)display_width(field_col.header);
    for (int f = 0; f < show_fields; ++f) {
        std::string name = schema->field(col_indices[f])->name();
        if ((int)display_width(name) > field_col.width)
            field_col.width = (int)display_width(name);
        field_col.cells.push_back(std::move(name));
    }

    // Record columns: one per visible record.
    std::vector<Column> rec_cols(n_records);
    for (int64_t r = 0; r < n_records; ++r) {
        Column& c    = rec_cols[r];
        c.header     = "#" + std::to_string(r);
        c.right_align = false;               // mixed types per cell — see ra below
        c.width      = (int)display_width(c.header);
        c.cells.resize(show_fields);
        c.cut = rendered_cut[r];
        for (int f = 0; f < show_fields; ++f) {
            c.cells[f] = rendered[r][f];
            int w = (int)display_width(c.cells[f]);
            if (w > c.width) c.width = w;
        }
    }

    // Fit as many record columns as the terminal width allows.
    // Each rendered column contributes "| <pad>value<pad> " = 1 + 2 + width chars.
    // Plus a final '|' at the end of the line.
    int term_w = detect_terminal_width();
    auto col_chars = [](int w) { return 1 + 2 + w; };
    int used = 1 /* trailing | */ + col_chars(field_col.width);
    int max_records = 0;
    for (int64_t r = 0; r < n_records; ++r) {
        int need = col_chars(rec_cols[r].width);
        if (used + need > term_w && max_records > 0) break;
        used += need;
        ++max_records;
    }
    if (max_records == 0) max_records = 1;   // always show at least one record

    std::vector<Column> columns;
    columns.reserve(1 + max_records);
    columns.push_back(std::move(field_col));
    for (int r = 0; r < max_records; ++r)
        columns.push_back(std::move(rec_cols[r]));

    // Format-specific lines (BED track/browser etc.)
    for (auto& line : src.preamble_above())
        std::printf("%s%s%s\n", g_color.meta_key, line.c_str(), g_color.reset);

    draw_separator(columns, SepKind::Top);
    {
        std::vector<std::string> hdr; std::vector<bool> ra;
        for (auto& c : columns) { hdr.push_back(c.header); ra.push_back(false); }
        draw_row(columns, hdr, ra, {}, /*is_header=*/true);
    }
    draw_separator(columns, SepKind::Middle);
    for (int f = 0; f < show_fields; ++f) {
        std::vector<std::string> row;
        std::vector<bool> ra, cut;
        // Field-name column: left-aligned. Record columns: right-aligned
        // for every type, so values line up against the next field's column.
        for (size_t i = 0; i < columns.size(); ++i) {
            row.push_back(columns[i].cells[f]);
            ra.push_back(i != 0);
            cut.push_back((size_t)f < columns[i].cut.size() && columns[i].cut[(size_t)f]);
        }
        draw_row(columns, row, ra, cut);
    }
    draw_separator(columns, SepKind::Bottom);

    if (max_records < (int64_t)n_records)
        std::printf("  ... %lld more record(s) not shown (widen terminal, "
                    "lower -w, or pipe to less -S)\n",
                    (long long)(n_records - max_records));
    if (show_fields < n_fields) {
        int n_hidden = (int)src.hidden_for_display().size();
        int n_truncated = n_fields - show_fields - n_hidden;
        if (n_truncated > 0)
            std::printf("  ... %d more field(s) not shown (-c 0 to see all)\n",
                        n_truncated);
        if (n_hidden > 0)
            std::printf("  ... %d derived field(s) hidden by file format\n",
                        n_hidden);
    }

    int64_t total = (tr >= 0) ? tr : n_records;
    std::printf("\n%s[%lld rows x %d columns]%s  vertical: %lld record(s) shown\n",
                g_color.meta_key, (long long)total, n_fields,
                g_color.reset, (long long)max_records);
    return "";
}

// Returns "" on success, an error message otherwise, so a bad --select /
// --filter exits non-zero instead of printing and exiting 0.
std::string print_table(TabularSource& src, const Config& cfg,
                        bool with_footer) {
    auto schema = src.schema();
    std::vector<std::string> unknown;
    std::vector<int> col_indices = select_field_indices(src, cfg, &unknown);
    if (!unknown.empty()) return unknown_columns_error(src, unknown);
    int show_cols = (int)col_indices.size();
    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> read_indices = have_filter
        ? union_with_filter(col_indices, fx) : col_indices;

    int64_t tr          = src.total_rows();
    int64_t rows_wanted = (cfg.head_rows <= 0) ? (tr >= 0 ? tr : INT64_MAX)
                                               : (int64_t)cfg.head_rows;

    // Collect rows up to rows_wanted. For head_rows > 0 we can ask the source
    // for a partial read (Parquet uses a RecordBatchReader to avoid decoding
    // a whole row group just to display the first handful of rows). Skip
    // the fast path when --filter is active.
    std::shared_ptr<arrow::Table> data;
    if (cfg.head_rows > 0 && !have_filter) {
        (void)src.read_first(rows_wanted, read_indices, &data);
    } else {
        int64_t need = rows_wanted;
        for (int c = 0; need > 0; ++c) {
            src.ensure(c);
            if (c >= src.num_chunks()) break;
            std::shared_ptr<arrow::Table> chunk;
            if (!src.read_chunk(c, read_indices, &chunk).ok()) continue;
            if (have_filter) chunk = apply_filter(chunk, fx, read_indices);
            if (!chunk || chunk->num_rows() == 0) continue;
            if (chunk->num_rows() > need) chunk = chunk->Slice(0, need);
            need -= chunk->num_rows();
            if (!data) data = chunk;
            else {
                auto r = arrow::ConcatenateTables({data, chunk});
                if (r.ok()) data = r.ValueOrDie();
            }
        }
    }
    // Nothing matched (or nothing to read): draw the header and the
    // "0 rows" footer, as for an empty file, rather than print nothing.
    if (!data) {
        arrow::FieldVector fv;
        for (int i : read_indices) fv.push_back(schema->field(i));
        auto empty = arrow::Table::MakeEmpty(arrow::schema(fv));
        if (!empty.ok()) return "";
        data = empty.ValueOrDie();
    }
    // Drop filter-only columns from `data` so the display loop's column
    // indices line up with col_indices (the user-requested set).
    if (read_indices != col_indices)
        data = project_to_requested(data, read_indices, col_indices);

    int64_t n_display = data->num_rows();
    int     num_cols  = schema->num_fields();

    // Build Column display structs
    std::vector<Column> columns;
    columns.reserve(show_cols + 1);

    if (!cfg.no_index) {
        Column idx;
        idx.header = ""; idx.right_align = true; idx.is_index = true;
        int digits = 1;
        for (int64_t v = std::max<int64_t>(n_display-1, 0); v >= 10; v /= 10) ++digits;
        idx.width = digits;
        for (int64_t r = 0; r < n_display; ++r) idx.cells.push_back(std::to_string(r));
        columns.push_back(std::move(idx));
    }

    const int max_w = (src.show_cells_in_full() && !cfg.max_col_w_set) ? (1 << 20) : cfg.max_col_w;
    for (int ci = 0; ci < show_cols; ++ci) {
        int ci_src   = col_indices[ci];          // original source field index
        auto field   = schema->field(ci_src);
        auto arr_col = data->column(ci);
        bool is_int  = is_integer_type(field->type()->id());
        Column col;
        col.header      = field->name();
        col.right_align = is_numeric_type(field->type()->id());
        col.is_bool     = (display_type(*field) == arrow::Type::BOOL);
        col.is_rgb      = (field->name() == "RGB");
        col.width       = std::max(display_width(col.header), src.min_col_width(ci_src));
        for (auto& chunk : arr_col->chunks())
            for (int64_t r = 0; r < chunk->length(); ++r) {
                std::string val = src.format_cell(ci_src, cell_to_display_string(*chunk, r));
                // Integer columns must show every digit — skip max_col_w clipping.
                const bool cut = !is_int && truncate_cuts(val, max_w);
                if (cut) val = truncate(std::move(val), max_w);
                if (display_width(val) > col.width) col.width = display_width(val);
                col.cells.push_back(std::move(val));
                col.cut.push_back(cut);
            }
        if (!is_int) col.width = std::min(col.width, max_w);
        col.header = truncate(col.header, max_w);
        col.width  = std::max(col.width, display_width(col.header));
        columns.push_back(std::move(col));
    }

    // BED track/browser lines shown above the table
    for (auto& line : src.preamble_above())
        std::printf("%s%s%s\n", g_color.meta_key, line.c_str(), g_color.reset);

    draw_separator(columns, SepKind::Top);
    { std::vector<std::string> hdr; std::vector<bool> ra;
      for (auto& c : columns) { hdr.push_back(c.header); ra.push_back(false); }
      draw_row(columns, hdr, ra, {}, true); }
    draw_separator(columns, SepKind::Middle);
    for (int64_t r = 0; r < n_display; ++r) {
        std::vector<std::string> row; std::vector<bool> ra, cut;
        for (auto& c : columns) {
            row.push_back(c.cells[r]); ra.push_back(c.right_align);
            cut.push_back((size_t)r < c.cut.size() && c.cut[(size_t)r]);
        }
        draw_row(columns, row, ra, cut);
    }
    draw_separator(columns, SepKind::Bottom);

    // --select prints exactly the named columns (no -c clamp, hidden ones
    // included on request): the rest are not "not shown", they were left out.
    if (show_cols < num_cols && cfg.select_cols.empty()) {
        int n_hidden = (int)src.hidden_for_display().size();
        int n_truncated = num_cols - show_cols - n_hidden;
        if (n_truncated > 0)
            std::printf("  ... %d more column(s) not shown (-c 0 to see all)\n",
                        n_truncated);
        if (n_hidden > 0)
            std::printf("  ... %d derived column(s) hidden by file format "
                        "(--tsv / --parquet preserve them)\n", n_hidden);
    }

    // Summary
    int64_t total = (tr >= 0) ? tr : n_display;
    std::printf("\n%s[%lld rows x %d columns]%s%s\n",
                g_color.meta_key, (long long)total, num_cols, g_color.reset,
                (have_filter && n_display == 0) ? "  no rows match --filter" : "");

    if (with_footer) print_schema_block(src);
    return "";
}

// Schema + file-info block. Shared by print_table's footer and the
// stand-alone `--schema` mode.
static void print_schema_columns(const arrow::Schema& schema, int max_rows = 0) {
    int num_cols = schema.num_fields();
    int shown = (max_rows > 0 && num_cols > max_rows) ? max_rows : num_cols;
    int name_w = 6, type_w = 4;
    for (int ci = 0; ci < num_cols; ++ci) {
        auto f = schema.field(ci);
        name_w = std::max(name_w, (int)f->name().size());
        type_w = std::max(type_w, (int)type_label(*f->type()).size());
    }
    name_w = std::min(name_w, 40); type_w = std::min(type_w, 40);

    std::printf("\n%s%-*s  %-*s  Nullable%s\n",
                g_color.header, name_w, "Column", type_w, "Type", g_color.reset);
    std::printf("%s%s  %s  --------%s\n", g_color.border,
                std::string(name_w,'-').c_str(), std::string(type_w,'-').c_str(), g_color.reset);
    for (int ci = 0; ci < shown; ++ci) {
        auto f = schema.field(ci);
        std::string fname = truncate(f->name(), name_w);
        std::string ftype = truncate(type_label(*f->type()), type_w);
        const char* tc = *g_color.reset ? type_color(display_type(*f)) : "";
        std::printf("%-*s  %s%-*s%s  %s\n",
                    name_w, fname.c_str(),
                    tc, type_w, ftype.c_str(), g_color.reset,
                    f->nullable() ? "yes" : "no");
    }
    if (shown < num_cols)
        std::printf("%s... %d more column(s)%s\n",
                    g_color.meta_key, num_cols - shown, g_color.reset);
}

// The schema's key-value metadata: what a Parquet or Arrow IPC writer stored
// beside the data (pandas' index description, an assembly name, a
// provenance note). Parquet's `ARROW:schema` is not among them — the Arrow
// reader consumes it and it only repeats the columns. One line per key; a
// value is cut to one line of about 80 characters, with its size when cut.
void print_schema_metadata(const arrow::Schema& schema) {
    auto kv = schema.metadata();
    if (!kv || kv->size() == 0) return;
    std::printf("%sMetadata:%s\n", g_color.meta_key, g_color.reset);
    for (int64_t i = 0; i < kv->size(); ++i) {
        const std::string& v = kv->value(i);
        std::string shown;
        for (char c : v) {
            if (c == '\n')      shown += "\\n";
            else if (c == '\t') shown += "\\t";
            else if ((unsigned char)c < 0x20) shown += '?';
            else                shown += c;
        }
        constexpr size_t kMax = 80;
        if (display_width(shown) > kMax) {
            shown = truncate(shown, kMax);
            char sz[32];
            if (v.size() < 1024) std::snprintf(sz, sizeof(sz), " (%zu B)", v.size());
            else                 std::snprintf(sz, sizeof(sz), " (%.1f KiB)", v.size() / 1024.0);
            shown += sz;
        }
        std::printf("  %s%s%s = %s\n", g_color.meta_key, kv->key(i).c_str(), g_color.reset,
                    shown.c_str());
    }
}

// `, "metadata": {key: value, …}` — the full values; {} when there are none.
void emit_metadata_json(const arrow::Schema& schema) {
    std::printf(", \"metadata\": {");
    if (auto kv = schema.metadata())
        for (int64_t i = 0; i < kv->size(); ++i) {
            if (i) std::printf(", ");
            json_emit_string(kv->key(i));
            std::printf(": ");
            json_emit_string(kv->value(i));
        }
    std::printf("}");
}

void print_schema_block(TabularSource& src) {
    print_schema_columns(*src.schema());

    // File info footer
    std::printf("\n%sFile:%s %s\n", g_color.meta_key, g_color.reset, src.path().c_str());
    std::printf("%s%s%s\n", g_color.meta_key, src.footer().c_str(), g_color.reset);
    if (!src.created_by().empty())
        std::printf("%sCreated by:%s %s\n", g_color.meta_key, g_color.reset,
                    src.created_by().c_str());
    print_schema_metadata(*src.schema());
    // VCF/BAM/SAM/GFF meta header lines shown below the schema (display-truncated)
    {
        auto pb = src.preamble_below();
        size_t limit = 20;
        size_t n = std::min(pb.size(), limit);
        for (size_t i = 0; i < n; ++i)
            std::printf("%s%s%s\n", g_color.meta_key, pb[i].c_str(), g_color.reset);
        if (pb.size() > limit)
            std::printf("%s... (%zu more header lines)%s\n",
                        g_color.meta_key, pb.size() - limit, g_color.reset);
    }
}

// Multi-tab overview: a file that expands into component tabs (AnnData obs/var/X,
// Excel sheets, SQLite tables, HDF5 datasets, NPZ arrays) shows only its first
// tab under --schema / the table view. These render every tab instead — the
// direct fix for "the schema shows very little" and "no rows of any tab". Wide
// tabs (an X matrix) are column-capped by print_table's own fit-to-terminal.

static std::string tab_list_line(
        const std::vector<std::unique_ptr<TabularSource>>& tabs) {
    std::string names;
    for (auto& t : tabs) { if (!names.empty()) names += ", "; names += t->tab_label(); }
    return "Tabs (" + std::to_string(tabs.size()) + "): " + names;
}

void render_multitab_schema(
 std::vector<std::unique_ptr<TabularSource>>& tabs,
 const std::string& path) {
    constexpr int kWideTab = 40;   // above this a tab is matrix-like, not a frame
    for (auto& t : tabs) {
        std::printf("\n%s\xe2\x94\x80\xe2\x94\x80 %s \xe2\x94\x80\xe2\x94\x80%s\n",
                    g_color.header, t->tab_label().c_str(), g_color.reset);
        int nc = t->schema()->num_fields();
        print_schema_columns(*t->schema(), nc > kWideTab ? 24 : 0);
        std::printf("%s%s%s\n", g_color.meta_key, t->footer().c_str(), g_color.reset);
    }
    std::printf("\n%sFile:%s %s\n", g_color.meta_key, g_color.reset, path.c_str());
    std::printf("%s%s%s\n", g_color.meta_key, tab_list_line(tabs).c_str(), g_color.reset);
}

std::string render_multitab_table(
 std::vector<std::unique_ptr<TabularSource>>& tabs,
 const Config& cfg) {
    constexpr int kWideTab = 40;   // above this a tab is matrix-like, not a frame
    Config base = cfg;
    if (base.head_rows <= 0 || base.head_rows > 10) base.head_rows = 10;  // bounded preview
    base.head_rows_set = true;
    for (auto& t : tabs) {
        std::printf("\n%s\xe2\x94\x80\xe2\x94\x80 %s \xe2\x94\x80\xe2\x94\x80%s\n",
                    g_color.header, t->tab_label().c_str(), g_color.reset);
        Config pcfg = base;
        // A matrix-like tab (X, layers) would print a very wide row; show only
        // the first columns and let print_table note the rest. A data frame
        // (obs / var) keeps all its columns.
        if (t->schema()->num_fields() > kWideTab && pcfg.max_cols == 0)
            pcfg.max_cols = 12;
        std::string err = print_table(*t, pcfg, /*with_footer=*/false);
        if (!err.empty()) return err;
        std::printf("%s%s%s\n", g_color.meta_key, t->footer().c_str(), g_color.reset);
    }
    std::printf("\n%sFile:%s %s\n", g_color.meta_key, g_color.reset,
                tabs.empty() ? "" : tabs.front()->path().c_str());
    std::printf("%s%s%s\n", g_color.meta_key, tab_list_line(tabs).c_str(), g_color.reset);
    return "";
}

// Machine-readable counterpart of print_schema_block. The only structured
// shape vv had was `--describe --json`, which runs compute_col_stats over
// every row — on a BAM that is the whole file, so automation reached for the
// most expensive mode just to learn the column names.
//
// `rows` is null when the source hasn't been fully scanned. Deliberately: the
// point of this mode is to be cheap, and draining a streaming source to
// produce a number would defeat it. `--count` is there when the number is
// what you want.
// Every source's footer() starts "Format: <name>  |  <details>". Pull the
// name back out rather than adding a virtual that fifteen classes would have
// to implement identically.
std::string format_label_of(TabularSource& src) {
    const std::string f = src.footer();
    const std::string key = "Format: ";
    auto p = f.find(key);
    if (p == std::string::npos)   // plain Parquet's footer starts at "Row groups:"
        return is_parquet_source(src) ? "Parquet" : "";
    std::string rest = f.substr(p + key.size());
    auto bar = rest.find("  |");
    if (bar != std::string::npos) rest.erase(bar);
    while (!rest.empty() && std::isspace((unsigned char)rest.back())) rest.pop_back();
    return rest;
}

void emit_schema_json(TabularSource& src, const std::string& fmt_name) {
    auto schema = src.schema();
    std::printf("{");
    std::printf("\"path\": ");   json_emit_string(src.path());
    std::printf(", \"format\": "); json_emit_string(fmt_name);
    int64_t tr = src.total_rows();
    if (tr >= 0) std::printf(", \"rows\": %lld", (long long)tr);
    else         std::printf(", \"rows\": null");
    std::printf(", \"region_applied\": %s", src.region_applied() ? "true" : "false");
    if (!src.created_by().empty()) {
        std::printf(", \"created_by\": ");
        json_emit_string(src.created_by());
    }
    // Columns hidden from human-facing views (e.g. LociSSD's derived
    // MaxEndSoFar) are reported, flagged — an exporter needs to know they
    // exist, a UI needs to know not to show them by default.
    std::set<std::string> hidden;
    for (const auto& h : src.hidden_for_display()) hidden.insert(h);
    std::printf(", \"columns\": [");
    for (int i = 0; i < schema->num_fields(); ++i) {
        if (i) std::printf(", ");
        auto f = schema->field(i);
        std::printf("{\"name\": ");
        json_emit_string(f->name());
        std::printf(", \"type\": ");
        json_emit_string(type_label(*f->type()));
        std::printf(", \"nullable\": %s", f->nullable() ? "true" : "false");
        std::printf(", \"hidden\": %s", hidden.count(f->name()) ? "true" : "false");
        std::printf("}");
    }
    std::printf("]");
    emit_metadata_json(*schema);
    std::printf("}\n");
}
