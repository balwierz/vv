// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// In-memory tables and spreadsheets (xlsx, ods).

#include "internal.hpp"
#include <functional>

// ── Workbook source (xlsx, future ods) ───────────────────────────────────────
//
// Multi-sheet spreadsheet files (.xlsx today, .ods later) share the same
// "one source per sheet, plus a list of sibling sheet names" shape that
// SQLite uses for tables. WorkbookSource captures that shape on top of
// MemoryTableSource: each sheet is buffered into memory and parsed
// through Arrow's CSV reader for type inference (numbers, booleans, dates
// in ISO 8601). A library-specific subclass (XlsxSource here, an
// OdsSource later) only has to:
//   1. List sheet names and pick the first.
//   2. Stream one sheet's rows into a CSV byte buffer.
//   3. Build a sibling-sheet source for every other sheet, sharing the
//      underlying library handle.
// open_sibling_sheets() returns those siblings as plain TabularSource
// pointers so main()'s tab-expansion loop stays uniform.


// Quote one CSV cell for inclusion in an in-memory buffer that we then
// feed to Arrow's CSV reader. RFC 4180 rules: wrap in "..." iff the cell
// contains a comma, double-quote, CR, or LF, and double any internal ".
static void csv_append_quoted(std::string& out, const char* s) {
    if (!s) return;
    bool needs_quote = false;
    for (const char* p = s; *p; ++p) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quote = true; break;
        }
    }
    if (!needs_quote) { out += s; return; }
    out += '"';
    for (const char* p = s; *p; ++p) {
        if (*p == '"') out += '"';
        out += *p;
    }
    out += '"';
}

// Parse a complete CSV byte buffer into an arrow::Table via Arrow's CSV
// TableReader. Type inference (int / float / bool / ISO-8601 date /
// string) is whatever Arrow's CSV converter does. Shared between the
// WorkbookSource subclasses (xlsx, ods) — both stream their sheet bodies
// into an in-memory CSV first, then run them through this helper.
arrow::Result<std::shared_ptr<arrow::Table>>
csv_buffer_to_table(const std::string& buf) {
    auto in_buf = std::make_shared<arrow::Buffer>(
        reinterpret_cast<const uint8_t*>(buf.data()), (int64_t)buf.size());
    auto in_stream = std::make_shared<arrow::io::BufferReader>(in_buf);

    auto ropts = arrow::csv::ReadOptions::Defaults();
    ropts.use_threads = true;
    auto popts = arrow::csv::ParseOptions::Defaults();
    popts.delimiter = ',';
    auto copts = arrow::csv::ConvertOptions::Defaults();
    copts.strings_can_be_null = true;          // empty cell → null

    // Leading-zero IDs: keep a column like "007" as utf8 instead of letting
    // inference drop the zeros. The buffer is in memory — tokenise the header
    // (line 0) and a sample of data rows directly, then force those columns.
    {
        std::vector<std::string> header, sample;
        std::string line;
        size_t i = 0;
        while (i < buf.size() && sample.size() < 200) {
            size_t nl = buf.find('\n', i);
            line.assign(buf, i, (nl == std::string::npos ? buf.size() : nl) - i);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            i = (nl == std::string::npos) ? buf.size() : nl + 1;
            if (line.empty()) continue;
            if (header.empty()) split_delimited_line(line, ',', &header);
            else                sample.push_back(line);
        }
        for (const auto& name : leading_zero_columns(sample, ',', header))
            copts.column_types[name] = arrow::utf8();
    }

    ARROW_ASSIGN_OR_RAISE(auto reader, arrow::csv::TableReader::Make(
        arrow::io::default_io_context(), in_stream, ropts, popts, copts));
    return reader->Read();
}

// Assemble per-row CSV fragments (each a comma-joined run of quoted cells, with
// no trailing padding) into one buffer where *every* row is padded out to the
// widest row. Workbook readers used to lock the column count to the first
// (header) row and only pad shorter rows; a later row with *more* columns than
// the header then made Arrow's CSV reader reject the whole sheet as ragged.
// Padding to the maximum keeps that data instead of dropping the sheet. Header
// cells past the original header width are given synthetic "colN" names (N =
// 1-based column position) so the widened header has no duplicate empty names.
static std::string assemble_ragged_csv(const std::vector<std::string>& rows,
                                       const std::vector<size_t>& widths) {
    size_t max_cols = 0;
    for (size_t w : widths) max_cols = std::max(max_cols, w);
    std::string buf;
    for (size_t i = 0; i < rows.size(); ++i) {
        buf += rows[i];
        for (size_t c = widths[i]; c < max_cols; ++c) {
            buf += ',';
            if (i == 0)                       // name the header's overflow cols
                buf += "col" + std::to_string(c + 1);
        }
        buf += '\n';
    }
    return buf;
}

// Convert one xlsx sheet to an arrow::Table by buffering rows as CSV and
// running them through csv_buffer_to_table. `sheet_name` may be empty to
// open the first sheet by position.
static arrow::Result<std::shared_ptr<arrow::Table>>
xlsx_sheet_to_table(xlsxioreader rdr, const std::string& sheet_name) {
    xlsxioreadersheet sh = xlsxioread_sheet_open(
        rdr, sheet_name.empty() ? nullptr : sheet_name.c_str(),
        XLSXIOREAD_SKIP_EMPTY_ROWS);
    if (!sh)
        return arrow::Status::IOError("xlsxio: cannot open sheet '",
                                       sheet_name, "'");

    // Buffer each row's cells, then pad every row to the widest one (a row
    // wider than the header must not make Arrow reject the sheet — see
    // assemble_ragged_csv).
    std::vector<std::string> rows;
    std::vector<size_t>      widths;
    while (xlsxioread_sheet_next_row(sh)) {
        std::string line;
        bool   first_cell = true;
        size_t col = 0;
        char* cell;
        while ((cell = xlsxioread_sheet_next_cell(sh)) != nullptr) {
            if (!first_cell) line += ',';
            csv_append_quoted(line, cell);
            xlsxioread_free(cell);
            first_cell = false;
            ++col;
        }
        rows.push_back(std::move(line));
        widths.push_back(col);
    }
    xlsxioread_sheet_close(sh);

    if (rows.empty() || widths.front() == 0)
        return arrow::Status::IOError("xlsxio: sheet '", sheet_name,
                                       "' has no header row");
    return csv_buffer_to_table(assemble_ragged_csv(rows, widths));
}

class DeferredSheetSource final : public WorkbookSource {
    using Load = std::function<arrow::Result<std::shared_ptr<arrow::Table>>()>;
    std::string           sheet_;
    mutable Load          load_;
    mutable arrow::Status status_;
    void                  build() const {
        if (!load_) return;
        auto result = load_();
        load_       = {};
        auto* self  = const_cast<DeferredSheetSource*>(this);
        if (result.ok())
            self->replace_table(*result, MemoryTableSource::footer());
        else
            status_ = result.status();
    }

public:
    DeferredSheetSource(std::string path, std::string sheet, std::string format, Load load)
        : WorkbookSource(arrow::Table::Make(arrow::schema({}),
                                            std::vector<std::shared_ptr<arrow::ChunkedArray>>{}, 0),
                         std::move(path), "Format: " + format + "  |  Sheet: " + sheet),
          sheet_(std::move(sheet)),
          load_(std::move(load)) {}
    std::string                    tab_label() const override { return sheet_; }
    std::shared_ptr<arrow::Schema> schema() const override {
        build();
        return MemoryTableSource::schema();
    }
    int64_t total_rows() const override {
        build();
        return MemoryTableSource::total_rows();
    }
    int num_chunks() const override {
        build();
        return MemoryTableSource::num_chunks();
    }
    ChunkMeta chunk_meta(int i) const override {
        build();
        return MemoryTableSource::chunk_meta(i);
    }
    arrow::Status read_status() const override {
        build();
        return status_;
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols,
                             std::shared_ptr<arrow::Table>* out) override {
        build();
        ARROW_RETURN_NOT_OK(status_);
        return MemoryTableSource::read_chunk(i, cols, out);
    }
    std::vector<std::unique_ptr<TabularSource>> open_sibling_sheets() const override { return {}; }
};

class XlsxSource : public WorkbookSource {
    std::shared_ptr<void>     workbook_;        // xlsxioreader (handle)
    std::string               sheet_;
    std::vector<std::string>  sibling_sheets_;

    XlsxSource(std::shared_ptr<arrow::Table>   table,
                std::string                     path,
                std::string                     footer,
                std::shared_ptr<void>           wb,
                std::string                     sheet,
                std::vector<std::string>        siblings)
        : WorkbookSource(std::move(table), std::move(path),
                          std::move(footer)),
          workbook_(std::move(wb)),
          sheet_(std::move(sheet)),
          sibling_sheets_(std::move(siblings)) {}

    static std::string build_one(const std::string& path,
                                  std::shared_ptr<void> wb,
                                  const std::string& sheet,
                                  std::vector<std::string> siblings,
                                  std::unique_ptr<XlsxSource>* out) {
        auto rdr = static_cast<xlsxioreader>(wb.get());
        auto tbl_or = xlsx_sheet_to_table(rdr, sheet);
        if (!tbl_or.ok())
            return tbl_or.status().ToString();
        std::string footer = "Format: Excel  |  Sheet: " + sheet;
        if (!siblings.empty())
            footer += "  |  +" + std::to_string(siblings.size()) +
                      " more sheet(s)";
        out->reset(new XlsxSource(*tbl_or, path, std::move(footer),
                                   std::move(wb), sheet,
                                   std::move(siblings)));
        return "";
    }

public:
    // Open an .xlsx / .xlsm workbook, build an XlsxSource for its first
    // sheet, and remember the other sheet names for sibling expansion.
    static std::string open_first(const std::string& path,
                                   std::unique_ptr<XlsxSource>* out) {
        xlsxioreader raw = xlsxioread_open(path.c_str());
        if (!raw)
            return "Cannot open '" + path + "' as Excel (.xlsx/.xlsm)";
        std::shared_ptr<void> wb(raw, [](void* p){
            xlsxioread_close(static_cast<xlsxioreader>(p));
        });

        // List sheets.
        std::vector<std::string> sheets;
        xlsxioreadersheetlist sl = xlsxioread_sheetlist_open(raw);
        if (sl) {
            const char* n;
            while ((n = xlsxioread_sheetlist_next(sl)) != nullptr)
                sheets.emplace_back(n);
            xlsxioread_sheetlist_close(sl);
        }
        if (sheets.empty())
            return "'" + path + "': Excel file has no sheets";

        std::vector<std::string> siblings(sheets.begin() + 1, sheets.end());
        return build_one(path, std::move(wb), sheets.front(),
                          std::move(siblings), out);
    }

    std::string tab_label() const override { return sheet_; }

    std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> out;
        for (const auto& s : sibling_sheets_) {
            out.push_back(std::make_unique<DeferredSheetSource>(
                path(), s, "Excel", [wb = workbook_, sheet = s] {
                    return xlsx_sheet_to_table(static_cast<xlsxioreader>(wb.get()), sheet);
                }));
        }
        return out;
    }
};

std::string open_xlsx_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<XlsxSource> s;
    std::string e = XlsxSource::open_first(path, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── OpenDocument Spreadsheet (.ods) source ───────────────────────────────────
//
// .ods is a ZIP archive whose `content.xml` carries the spreadsheet body
// in OpenDocument SpreadsheetML. We unzip content.xml with minizip and
// SAX-parse it with expat, emitting per-sheet CSV buffers that hit the
// same WorkbookSource pipeline as the xlsx reader.
//
// Element grammar (only the parts we care about):
//   <table:table table:name="…">                  ← sheet
//     <table:table-row table:number-rows-repeated="N">
//       <table:table-cell office:value-type="…"
//                          office:value="…"
//                          office:date-value="…"
//                          office:boolean-value="…"
//                          table:number-columns-repeated="K">
//         <text:p>display-text</text:p>           ← cell text
//       </table:table-cell>
//     </table:table-row>
//   </table:table>
//
// Type policy:
//   value-type="float" / "percentage" / "currency" → use office:value
//   value-type="date"                               → use office:date-value
//   value-type="boolean"                            → use office:boolean-value
//   value-type="time"                               → use office:time-value
//   anything else (string, missing, …)              → use accumulated text
// The typed attribute (when present) is canonical and locale-free, so it
// feeds Arrow's CSV type inference reliably even if the spreadsheet
// formats numbers with thousands separators.
//
// Compaction: ODS aggressively uses table:number-{columns,rows}-repeated
// to collapse long runs of identical / empty cells. We honour the cell
// repeat by emitting the cell N times — *unless* it's trailing-empty,
// in which case it would balloon the CSV. We detect trailing empties by
// buffering the row's cells and dropping the empty suffix at row close.

// Position of the current event in the XML stream, and its length (-1 when
// unknown). Expat before 2.9 has only the `long` forms, 32 bits on Windows:
// a sheet past 2 GiB there then gets no span and is found by a full parse.
static int64_t ods_byte_index(XML_Parser p) {
#if XML_MAJOR_VERSION > 2 || (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 9)
    return XML_GetCurrentByteIndex64(p);
#else
    return (int64_t)XML_GetCurrentByteIndex(p);
#endif
}
static int64_t ods_byte_count(XML_Parser p) {
#if XML_MAJOR_VERSION > 2 || (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 9)
    return XML_GetCurrentByteCount64(p);
#else
    return (int64_t)XML_GetCurrentByteCount(p);
#endif
}

namespace {
struct OdsParserState {
    // Every sheet in document order: its name, whether any cell holds a
    // value (an empty sheet gets no tab), and the bytes of its table:table
    // element in the XML, [begin, end) (-1: unknown).
    struct Sheet { std::string name; bool nonempty = false; int64_t begin = -1, end = -1; };
    std::vector<Sheet> sheets;

    // Which sheet's text to keep: `target` >= 0 is a sheet's position in
    // `sheets`; -1 keeps the first non-empty sheet. Only that sheet's rows
    // are buffered; the others are scanned for a value and dropped.
    int          target = -1;
    bool         stop_after_target = false;  // stop parsing once it closes
    int          found = -1;                 // position of the sheet kept
    std::string  csv;                        // its CSV text
    bool         collect = false;            // keeping the current sheet's rows
    XML_Parser   parser = nullptr;
    bool         stopped = false;
    std::vector<int> open_tables;            // positions of the table:table elements open

    // Current sheet state.
    std::string row_csv;                 // bytes for the current row (un-terminated)
    std::vector<std::string> row_cells;  // cells buffered for trailing-empty trim
    std::vector<int>         row_repeat; // repeat count per buffered cell
    int          row_repeat_count = 1;   // table:number-rows-repeated for this row
    bool         in_sheet = false;

    // Current cell state.
    int          cell_depth = 0;          // > 0 while inside a table:table-cell
    int          cell_repeat = 1;
    bool         cell_covered = false;    // table:covered-table-cell (merge filler)
    std::string  cell_value_type;         // office:value-type
    std::string  cell_typed_value;        // office:value / date-value / boolean-value
    std::string  cell_text;               // accumulated <text:p>
    bool         in_text_p = false;

    // Buffered rows for the current sheet (one comma-joined CSV fragment per
    // row, plus its field count); assembled with max-width padding at sheet
    // close so a row wider than the header doesn't make Arrow reject the sheet.
    std::vector<std::string> sheet_rows;
    std::vector<size_t>      sheet_widths;

    static const char* attr(const char** atts, const char* key) {
        for (int i = 0; atts && atts[i]; i += 2)
            if (std::strcmp(atts[i], key) == 0) return atts[i + 1];
        return nullptr;
    }
};
}  // namespace

static void XMLCALL ods_start(void* ud, const char* name, const char** atts) {
    auto* s = static_cast<OdsParserState*>(ud);
    // Everything inside a cell is that cell's content, a table:table too:
    // only its text:p text is kept, joined to the cell's. A sheet is a
    // table:table outside any cell.
    if (s->cell_depth > 0) {
        s->cell_depth++;
        if (std::strcmp(name, "text:p") == 0) s->in_text_p = true;
        return;
    }
    if (std::strcmp(name, "table:table") == 0) {
        s->sheets.push_back({"Sheet" + std::to_string(s->sheets.size() + 1)});
        if (auto n = OdsParserState::attr(atts, "table:name"))
            s->sheets.back().name = n;
        if (s->parser) s->sheets.back().begin = ods_byte_index(s->parser);
        s->open_tables.push_back((int)s->sheets.size() - 1);
        s->in_sheet = true;
        s->collect = s->found < 0 &&
                     (s->target < 0 || s->target == (int)s->sheets.size() - 1);
        s->sheet_rows.clear();
        s->sheet_widths.clear();
    } else if (s->in_sheet && std::strcmp(name, "table:table-row") == 0) {
        s->row_cells.clear();
        s->row_repeat.clear();
        // table:number-rows-repeated repeats the whole row N times. Empty
        // repeated rows (trailing/filler) are still dropped by the
        // trailing-empty trim at row close; a *non-empty* repeated row is
        // emitted N times (capped) instead of losing the duplicates.
        s->row_repeat_count = 1;
        if (auto v = OdsParserState::attr(atts,
                                          "table:number-rows-repeated")) {
            int n = std::atoi(v);
            if (n > 1) s->row_repeat_count = n;
        }
    } else if (s->in_sheet &&
               (std::strcmp(name, "table:table-cell") == 0 ||
                std::strcmp(name, "table:covered-table-cell") == 0)) {
        // A table:covered-table-cell holds the grid positions a spanned
        // (merged) cell overlaps. It carries no value of its own but still
        // occupies columns; dropping it shifted every later column left. Treat
        // it as an empty cell so the columns after a merge stay aligned (what
        // pandas/odf do — the merged value stays in its top-left column).
        s->cell_depth = 1;
        s->cell_repeat = 1;
        s->cell_covered =
            std::strcmp(name, "table:covered-table-cell") == 0;
        s->cell_value_type.clear();
        s->cell_typed_value.clear();
        s->cell_text.clear();
        if (!s->cell_covered) {
            if (auto v = OdsParserState::attr(atts, "office:value-type"))
                s->cell_value_type = v;
            if (auto v = OdsParserState::attr(atts, "office:value"))
                s->cell_typed_value = v;
            else if (auto v = OdsParserState::attr(atts, "office:date-value"))
                s->cell_typed_value = v;
            else if (auto v = OdsParserState::attr(atts, "office:time-value"))
                s->cell_typed_value = v;
            else if (auto v = OdsParserState::attr(atts, "office:boolean-value"))
                s->cell_typed_value = v;
        }
        if (auto v = OdsParserState::attr(atts,
                                           "table:number-columns-repeated")) {
            int n = std::atoi(v);
            if (n > 0 && n < 1000000) s->cell_repeat = n;
        }
    }
}

static void XMLCALL ods_chardata(void* ud, const char* data, int len) {
    auto* s = static_cast<OdsParserState*>(ud);
    if (s->cell_depth > 0 && s->in_text_p)
        s->cell_text.append(data, len);
}

static void XMLCALL ods_end(void* ud, const char* name) {
    auto* s = static_cast<OdsParserState*>(ud);
    if (s->cell_depth > 0) {
        if (std::strcmp(name, "text:p") == 0) s->in_text_p = false;
        // The cell's own end tag (depth 1), not one of a table inside it.
        if (s->cell_depth == 1 &&
            (std::strcmp(name, "table:table-cell") == 0 ||
             std::strcmp(name, "table:covered-table-cell") == 0)) {
            // Pick the canonical value: typed attribute wins for numeric /
            // date / boolean cells; text is the fallback (string cells +
            // anything without office:value). A covered cell holds no value —
            // it only reserves the columns a merge spans.
            std::string v;
            if (s->cell_covered) {
                // empty placeholder — keeps later columns aligned
            } else if (!s->cell_typed_value.empty() &&
                       s->cell_value_type != "string" &&
                       !s->cell_value_type.empty()) {
                v = std::move(s->cell_typed_value);
            } else {
                v = std::move(s->cell_text);
            }

            // Multiple inline <text:p> children → newlines. The simplest
            // sanitisation for CSV is to swap them for spaces; preserving
            // them would require CR/LF quoting which Arrow CSV handles
            // but few biology-data consumers will look for.
            for (char& c : v) if (c == '\n' || c == '\r') c = ' ';

            if (!v.empty() && !s->sheets.empty()) s->sheets.back().nonempty = true;
            if (s->collect) {
                s->row_cells.push_back(std::move(v));
                s->row_repeat.push_back(s->cell_repeat);
            }
            s->cell_depth = 0;
            s->cell_covered = false;
        } else {
            s->cell_depth--;
        }
        return;
    }
    if (s->in_sheet && std::strcmp(name, "table:table-row") == 0) {
        if (!s->collect) return;
        // Drop trailing empty cells so a million-column-wide repeated
        // empty doesn't poison the CSV. Keep the row only if it has at
        // least one non-empty cell.
        int last_nonempty = -1;
        for (int i = 0; i < (int)s->row_cells.size(); ++i)
            if (!s->row_cells[i].empty()) last_nonempty = i;
        if (last_nonempty < 0) return;  // entirely empty row → skip

        // Render the row as one comma-joined CSV fragment (no trailing pad);
        // assemble_ragged_csv pads every row to the sheet's widest at close.
        std::string line;
        size_t emitted = 0;
        for (int i = 0; i <= last_nonempty; ++i) {
            int reps = s->row_repeat[i];
            // Cap repeats sanely; ODS sometimes uses huge values for
            // "rest of the row" even when there's no real data.
            if (reps > 16384) reps = 16384;
            // Quote once, then repeat — a cell carrying a large
            // number-columns-repeated would otherwise re-scan and re-quote the
            // same value up to `reps` times.
            std::string quoted;
            csv_append_quoted(quoted, s->row_cells[i].c_str());
            for (int r = 0; r < reps; ++r) {
                if (emitted) line += ',';
                line += quoted;
                ++emitted;
            }
        }
        // Emit the row table:number-rows-repeated times (non-empty rows only;
        // empty ones already returned above). Cap to keep a hostile "repeat a
        // data row a million times" from exploding the CSV buffer.
        int row_reps = s->row_repeat_count;
        if (row_reps > 16384) row_reps = 16384;
        for (int r = 0; r < row_reps; ++r) {
            s->sheet_rows.push_back(line);
            s->sheet_widths.push_back(emitted);
        }
    } else if (std::strcmp(name, "table:table") == 0) {
        if (!s->open_tables.empty()) {
            const int k = s->open_tables.back();
            s->open_tables.pop_back();
            const int64_t at = s->parser ? ods_byte_index(s->parser) : -1;
            const int64_t n = s->parser ? ods_byte_count(s->parser) : -1;
            if (at >= 0 && n > 0) s->sheets[(size_t)k].end = at + n;
        }
        if (s->collect && !s->sheets.empty()) {
            std::string csv = assemble_ragged_csv(s->sheet_rows, s->sheet_widths);
            if (!csv.empty() || s->target >= 0) {
                s->csv = std::move(csv);
                s->found = (int)s->sheets.size() - 1;
            }
        }
        s->collect = false;
        s->sheet_rows.clear();
        s->sheet_rows.shrink_to_fit();
        s->sheet_widths.clear();
        s->sheet_widths.shrink_to_fit();
        s->in_sheet = false;
        if (s->found >= 0 && s->stop_after_target && s->parser) {
            s->stopped = true;
            XML_StopParser(s->parser, XML_FALSE);
        }
    }
}

// Parse an ODS's content.xml (inflated from the zip in 64 KiB pieces) or a
// flat .fods file through `state`, without holding the document: only the
// target sheet's rows are kept (OdsParserState::target). With a span
// [begin, end), only those bytes are parsed — one table:table element, a
// well-formed document on its own (the parser does no namespace processing,
// and ODS declares no entities); the bytes before it are inflated or seeked
// past, not parsed.
static std::string ods_parse(const std::string& path, bool flat, OdsParserState* state,
                             int64_t begin = -1, int64_t end = -1) {
    const bool span = begin >= 0 && end > begin;
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p) return "Error parsing '" + path + "': expat: cannot create parser";
    std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)> guard(p, XML_ParserFree);
    XML_SetUserData(p, state);
    XML_SetElementHandler(p, ods_start, ods_end);
    XML_SetCharacterDataHandler(p, ods_chardata);
    state->parser = p;
    // false: stop reading (an expat error, or the target sheet is done).
    auto feed = [&](const char* data, int n, bool last) -> bool {
        return XML_Parse(p, data, n, last ? 1 : 0) == XML_STATUS_OK;
    };
    auto parse_error = [&]() -> std::string {
        if (state->stopped) return "";
        return "Error parsing '" + path + "': expat: " + XML_ErrorString(XML_GetErrorCode(p));
    };
    char chunk[64 * 1024];
    int64_t left = span ? end - begin : INT64_MAX;   // bytes still to parse
    if (flat) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "Cannot open '" + path + "'";
        if (span && !in.seekg(begin)) return "'" + path + "': read error";
        while (true) {
            in.read(chunk, (std::streamsize)std::min<int64_t>(sizeof(chunk), left));
            const std::streamsize n = in.gcount();
            if (in.bad()) return "'" + path + "': read error";
            left -= n;
            const bool last = left == 0 || n == 0 || (!span && n < (std::streamsize)sizeof(chunk));
            if (!feed(chunk, (int)n, last)) return parse_error();
            if (last) return "";
        }
    }
    unzFile zf = unzOpen(path.c_str());
    if (!zf) return "Cannot open '" + path + "' as ODS (zip)";
    std::unique_ptr<void, int (*)(unzFile)> zguard(zf, unzClose);
    if (unzLocateFile(zf, "content.xml", /*iCaseSensitivity=*/1) != UNZ_OK)
        return "'" + path + "': not an ODS (missing content.xml)";
    if (unzOpenCurrentFile(zf) != UNZ_OK)
        return "'" + path + "': cannot open content.xml inside the zip";
    std::string err;
    for (int64_t skip = span ? begin : 0; skip > 0;) {   // inflate past the bytes before the span
        const int n = unzReadCurrentFile(zf, chunk, (unsigned)std::min<int64_t>(sizeof(chunk), skip));
        if (n <= 0) { err = "'" + path + "': error inflating content.xml"; break; }
        skip -= n;
    }
    while (err.empty()) {
        const int n = unzReadCurrentFile(zf, chunk, (unsigned)std::min<int64_t>(sizeof(chunk), left));
        if (n < 0) { err = "'" + path + "': error inflating content.xml"; break; }
        left -= n;
        const bool last = n == 0 || left == 0;
        if (!feed(chunk, n, last)) { err = parse_error(); break; }
        if (last) break;
    }
    unzCloseCurrentFile(zf);
    return err;
}

class OdsSource : public WorkbookSource {
    // The other non-empty sheets: name and position in the document. A
    // sibling tab parses the document again when first read, keeping only
    // its sheet, so no sheet's text outlives its conversion to Arrow.
    struct Sibling { std::string name; int index; int64_t begin, end; };
    bool                      flat_ = false;
    std::string               sheet_;
    std::vector<Sibling>      sibling_sheets_;

    OdsSource(std::shared_ptr<arrow::Table> table, std::string path, std::string footer, bool flat,
              std::string sheet, std::vector<Sibling> siblings)
        : WorkbookSource(std::move(table), std::move(path), std::move(footer)),
          flat_(flat),
          sheet_(std::move(sheet)),
          sibling_sheets_(std::move(siblings)) {}

public:
    // `flat`: a flat OpenDocument file (.fods) — the same office:spreadsheet
    // body as an .ods's content.xml, as one uncompressed XML document.
    static std::string open_first(const std::string& path,
                                   std::unique_ptr<OdsSource>* out,
                                   bool flat = false) {
        OdsParserState state;   // target -1: keep the first non-empty sheet
        if (auto err = ods_parse(path, flat, &state); !err.empty()) return err;
        if (state.found < 0)
            return "'" + path + "': ODS file has no data sheets";
        const std::string sheet = state.sheets[(size_t)state.found].name;
        std::vector<Sibling> siblings;
        for (size_t i = (size_t)state.found + 1; i < state.sheets.size(); ++i)
            if (state.sheets[i].nonempty)
                siblings.push_back({state.sheets[i].name, (int)i, state.sheets[i].begin, state.sheets[i].end});

        auto tbl_or = csv_buffer_to_table(state.csv);
        if (!tbl_or.ok())
            return tbl_or.status().ToString();
        std::string footer = "Format: ODS  |  Sheet: " + sheet;
        if (!siblings.empty())
            footer += "  |  +" + std::to_string(siblings.size()) +
                      " more sheet(s)";
        out->reset(new OdsSource(*tbl_or, path, std::move(footer), flat, sheet,
                                 std::move(siblings)));
        return "";
    }

    std::string tab_label() const override { return sheet_; }

    std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> out;
        for (const auto& s : sibling_sheets_) {
            out.push_back(std::make_unique<DeferredSheetSource>(
                path(), s.name, "ODS",
                [path = path(), flat = flat_, s]() -> arrow::Result<std::shared_ptr<arrow::Table>> {
                    // Its own bytes when the open pass recorded them, else a
                    // parse of the document up to the sheet's end.
                    const bool span = s.begin >= 0 && s.end > s.begin;
                    OdsParserState state;
                    state.target = span ? 0 : s.index;
                    state.stop_after_target = true;
                    if (auto err = span ? ods_parse(path, flat, &state, s.begin, s.end)
                                        : ods_parse(path, flat, &state);
                        !err.empty())
                        return arrow::Status::IOError(err);
                    if (state.found != state.target || state.csv.empty())
                        return arrow::Status::Invalid("ODS: missing sheet '", s.name, "'");
                    return csv_buffer_to_table(state.csv);
                }));
        }
        return out;
    }
};

std::string open_ods_source(const std::string& path, std::unique_ptr<TabularSource>* out, bool flat) {
    std::unique_ptr<OdsSource> s;
    std::string e = OdsSource::open_first(path, &s, flat);
    if (e.empty()) *out = std::move(s);
    return e;
}
