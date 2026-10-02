// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// In-memory tables and spreadsheets (xlsx, ods).

#include "internal.hpp"

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
            std::unique_ptr<XlsxSource> src;
            std::string err = build_one(path(), workbook_, s, {}, &src);
            if (!err.empty()) {
                std::fprintf(stderr, "vv: Excel sheet '%s': %s\n",
                             s.c_str(), err.c_str());
                continue;
            }
            out.push_back(std::move(src));
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

static std::string ods_unzip_content_xml(const std::string& path,
                                          std::string* out) {
    unzFile zf = unzOpen(path.c_str());
    if (!zf) return "Cannot open '" + path + "' as ODS (zip)";
    if (unzLocateFile(zf, "content.xml", /*iCaseSensitivity=*/1) != UNZ_OK) {
        unzClose(zf);
        return "'" + path + "': not an ODS (missing content.xml)";
    }
    if (unzOpenCurrentFile(zf) != UNZ_OK) {
        unzClose(zf);
        return "'" + path + "': cannot open content.xml inside the zip";
    }
    char chunk[64 * 1024];
    out->clear();
    while (true) {
        int n = unzReadCurrentFile(zf, chunk, sizeof(chunk));
        if (n < 0) {
            unzCloseCurrentFile(zf); unzClose(zf);
            return "'" + path + "': error inflating content.xml";
        }
        if (n == 0) break;
        out->append(chunk, n);
    }
    unzCloseCurrentFile(zf);
    unzClose(zf);
    return "";
}

namespace {
struct OdsParserState {
    // Output: name → CSV buffer, plus name order.
    std::vector<std::pair<std::string, std::string>> sheets;

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
    if (std::strcmp(name, "table:table") == 0) {
        s->sheets.push_back({"Sheet" + std::to_string(s->sheets.size() + 1),
                              std::string{}});
        if (auto n = OdsParserState::attr(atts, "table:name"))
            s->sheets.back().first = n;
        s->in_sheet = true;
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
    } else if (s->cell_depth > 0) {
        s->cell_depth++;
        if (std::strcmp(name, "text:p") == 0) s->in_text_p = true;
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
        if (std::strcmp(name, "table:table-cell") == 0 ||
            std::strcmp(name, "table:covered-table-cell") == 0) {
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

            s->row_cells.push_back(std::move(v));
            s->row_repeat.push_back(s->cell_repeat);
            s->cell_depth = 0;
            s->cell_covered = false;
        } else {
            s->cell_depth--;
        }
        return;
    }
    if (s->in_sheet && std::strcmp(name, "table:table-row") == 0) {
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
        s->sheets.back().second =
            assemble_ragged_csv(s->sheet_rows, s->sheet_widths);
        s->sheet_rows.clear();
        s->sheet_widths.clear();
        s->in_sheet = false;
    }
}

static std::string ods_parse_sheets(const std::string& xml,
                                     std::vector<std::pair<std::string,
                                                            std::string>>* out) {
    OdsParserState state;
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p) return "expat: cannot create parser";
    XML_SetUserData(p, &state);
    XML_SetElementHandler(p, ods_start, ods_end);
    XML_SetCharacterDataHandler(p, ods_chardata);
    if (XML_Parse(p, xml.data(), (int)xml.size(), 1) != XML_STATUS_OK) {
        std::string err = "expat: ";
        err += XML_ErrorString(XML_GetErrorCode(p));
        XML_ParserFree(p);
        return err;
    }
    XML_ParserFree(p);
    *out = std::move(state.sheets);
    return "";
}

class OdsSource : public WorkbookSource {
    // The full set of (sheet_name, csv_buffer) pairs, shared across sibling
    // sources via shared_ptr so a multi-sheet .ods is parsed once.
    std::shared_ptr<std::vector<std::pair<std::string, std::string>>> all_sheets_;
    std::string               sheet_;
    std::vector<std::string>  sibling_sheets_;

    OdsSource(std::shared_ptr<arrow::Table>  table,
               std::string                     path,
               std::string                     footer,
               std::shared_ptr<std::vector<std::pair<std::string,
                                                       std::string>>> all,
               std::string                     sheet,
               std::vector<std::string>        siblings)
        : WorkbookSource(std::move(table), std::move(path),
                          std::move(footer)),
          all_sheets_(std::move(all)),
          sheet_(std::move(sheet)),
          sibling_sheets_(std::move(siblings)) {}

    static std::string build_one(const std::string& path,
                                  std::shared_ptr<std::vector<std::pair<
                                      std::string, std::string>>> all,
                                  const std::string& sheet,
                                  std::vector<std::string> siblings,
                                  std::unique_ptr<OdsSource>* out) {
        const std::string* csv = nullptr;
        for (auto& p : *all) if (p.first == sheet) { csv = &p.second; break; }
        if (!csv || csv->empty())
            return "'" + path + "': sheet '" + sheet + "' is empty";

        auto tbl_or = csv_buffer_to_table(*csv);
        if (!tbl_or.ok())
            return tbl_or.status().ToString();

        std::string footer = "Format: ODS  |  Sheet: " + sheet;
        if (!siblings.empty())
            footer += "  |  +" + std::to_string(siblings.size()) +
                      " more sheet(s)";
        out->reset(new OdsSource(*tbl_or, path, std::move(footer),
                                  std::move(all), sheet,
                                  std::move(siblings)));
        return "";
    }

public:
    // `flat`: a flat OpenDocument file (.fods) — the same office:spreadsheet
    // body as an .ods's content.xml, as one uncompressed XML document.
    static std::string open_first(const std::string& path,
                                   std::unique_ptr<OdsSource>* out,
                                   bool flat = false) {
        std::string xml;
        std::string err;
        if (flat) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return "Cannot open '" + path + "'";
            xml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (in.bad()) return "'" + path + "': read error";
        } else {
            err = ods_unzip_content_xml(path, &xml);
        }
        if (!err.empty()) return err;

        auto all = std::make_shared<std::vector<std::pair<std::string,
                                                            std::string>>>();
        err = ods_parse_sheets(xml, all.get());
        if (!err.empty())
            return "Error parsing '" + path + "': " + err;
        // Filter out sheets that turned out fully empty after trim.
        all->erase(std::remove_if(all->begin(), all->end(),
                                    [](const std::pair<std::string,std::string>& p){
                                        return p.second.empty();
                                    }),
                     all->end());
        if (all->empty())
            return "'" + path + "': ODS file has no data sheets";

        std::vector<std::string> siblings;
        for (size_t i = 1; i < all->size(); ++i)
            siblings.push_back((*all)[i].first);
        return build_one(path, all, (*all)[0].first, std::move(siblings), out);
    }

    std::string tab_label() const override { return sheet_; }

    std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> out;
        for (const auto& s : sibling_sheets_) {
            std::unique_ptr<OdsSource> src;
            std::string err = build_one(path(), all_sheets_, s, {}, &src);
            if (!err.empty()) {
                std::fprintf(stderr, "vv: ODS sheet '%s': %s\n",
                             s.c_str(), err.c_str());
                continue;
            }
            out.push_back(std::move(src));
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
