#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/type.h>
#include <arrow/scalar.h>
#include <arrow/csv/api.h>
#include <arrow/io/compressed.h>
#include <arrow/ipc/feather.h>
#include <arrow/ipc/reader.h>
#include <arrow/util/compression.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>

#include <htslib/sam.h>

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <ncurses.h>
#undef OK   // ncurses defines OK as 0; conflicts with arrow::Status::OK()

// ── Colors ────────────────────────────────────────────────────────────────────

struct Colors {
    const char* reset      = "";
    const char* border     = "";   // table lines
    const char* header     = "";   // column name row
    const char* row_idx    = "";   // row index column
    const char* null_val   = "";   // null cells
    const char* number     = "";   // numeric / temporal values
    const char* bool_true  = "";   // true
    const char* bool_false = "";   // false
    const char* trunc      = "";   // the "..." suffix
    const char* type_int   = "";   // schema: integer type names
    const char* type_float = "";   // schema: float type names
    const char* type_str   = "";   // schema: string/binary type names
    const char* type_time  = "";   // schema: temporal type names
    const char* type_bool  = "";   // schema: bool type names
    const char* type_other = "";   // schema: everything else
    const char* meta_key   = "";   // summary labels ("File:", "Row groups:", …)
};

static Colors g_color;  // populated by init_colors()

static void init_colors() {
    g_color.reset      = "\033[0m";
    g_color.border     = "\033[90m";       // dark gray
    g_color.header     = "\033[1;97m";     // bold bright-white
    g_color.row_idx    = "\033[90m";       // dark gray
    g_color.null_val   = "\033[2;3m";      // dim + italic
    g_color.number     = "\033[96m";       // bright cyan
    g_color.bool_true  = "\033[92m";       // bright green
    g_color.bool_false = "\033[33m";       // yellow
    g_color.trunc      = "\033[90m";       // dark gray for "..."
    g_color.type_int   = "\033[96m";       // bright cyan
    g_color.type_float = "\033[93m";       // bright yellow
    g_color.type_str   = "\033[92m";       // bright green
    g_color.type_time  = "\033[95m";       // bright magenta
    g_color.type_bool  = "\033[94m";       // bright blue
    g_color.type_other = "\033[37m";       // white
    g_color.meta_key   = "\033[1m";        // bold
}

// Pick the right color for an Arrow type in the schema summary
static const char* type_color(arrow::Type::type t) {
    switch (t) {
        case arrow::Type::INT8:  case arrow::Type::INT16:
        case arrow::Type::INT32: case arrow::Type::INT64:
        case arrow::Type::UINT8: case arrow::Type::UINT16:
        case arrow::Type::UINT32: case arrow::Type::UINT64:
        case arrow::Type::DECIMAL128: case arrow::Type::DECIMAL256:
            return g_color.type_int;
        case arrow::Type::FLOAT: case arrow::Type::DOUBLE:
        case arrow::Type::HALF_FLOAT:
            return g_color.type_float;
        case arrow::Type::STRING: case arrow::Type::LARGE_STRING:
        case arrow::Type::BINARY: case arrow::Type::LARGE_BINARY:
        case arrow::Type::FIXED_SIZE_BINARY:
            return g_color.type_str;
        case arrow::Type::DATE32: case arrow::Type::DATE64:
        case arrow::Type::TIME32: case arrow::Type::TIME64:
        case arrow::Type::TIMESTAMP: case arrow::Type::DURATION:
            return g_color.type_time;
        case arrow::Type::BOOL:
            return g_color.type_bool;
        default:
            return g_color.type_other;
    }
}

// Unwrap dictionary to its value type for coloring purposes
static arrow::Type::type display_type(const arrow::Field& f) {
    auto t = f.type();
    if (t->id() == arrow::Type::DICTIONARY)
        return std::static_pointer_cast<arrow::DictionaryType>(t)->value_type()->id();
    return t->id();
}

// ── CLI args ─────────────────────────────────────────────────────────────────

enum class ColorMode { Auto, Always, Never };

struct Config {
    std::string path;
    int         head_rows      = 10;
    bool        head_rows_set  = false;  // true when -n was given explicitly
    int         max_col_w      = 32;
    int         max_cols       = 0;
    bool        no_index       = false;
    ColorMode   color          = ColorMode::Auto;
    char        delimiter      = 0;      // 0 = table/interactive; '\t'/','= delimited
    bool        no_header      = false;
    bool        interactive    = false;  // -i / --interactive
    bool        no_interactive = false;  // --no-interactive
};

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] <file>\n"
        "\nSupported formats:\n"
        "  .parquet\n"
        "  .bam  .cram                  binary/compressed sequence alignments (htslib)\n"
        "  .sam                        text sequence alignments\n"
        "  .vcf  .vcf.gz               variant calls\n"
        "  .gff  .gff3  .gtf           and .gz variants  genome annotations\n"
        "  .bed  .tsv  .csv            and .gz variants\n"
        "  (unknown extensions: sniffed by magic bytes / delimiter)\n"
        "\nInteractive viewer (default when stdout is a terminal):\n"
        "  -i / --interactive  open the ncurses row browser\n"
        "  --no-interactive    force plain table output even on a terminal\n"
        "  Keys: arrows/hjkl navigate, PgUp/PgDn, g/G top/bot, q quit\n"
        "\nTable options:\n"
        "  -n <rows>           rows to display  (default: 10, 0 = all)\n"
        "  -w <width>          max cell width   (default: 32)\n"
        "  -c <cols>           max columns to show (default: all)\n"
        "  --no-index          suppress the row-index column\n"
        "  --color[=WHEN]      colorize output: auto (default), always, never\n"
        "\nDelimited output (replaces table view):\n"
        "  --tsv               write tab-separated values to stdout\n"
        "  --csv               write comma-separated values to stdout\n"
        "  --delimiter <sep>   write with a custom single-character delimiter\n"
        "  --no-header         omit the header row\n"
        "  (-n defaults to all rows in this mode; -c still applies)\n"
        "\n  -h                  show this help\n",
        prog);
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            print_usage(argv[0]); std::exit(0);
        } else if (!std::strcmp(argv[i], "--no-index")) {
            cfg.no_index = true;
        } else if (!std::strcmp(argv[i], "--no-header")) {
            cfg.no_header = true;
        } else if (!std::strcmp(argv[i], "-i") || !std::strcmp(argv[i], "--interactive")) {
            cfg.interactive = true;
        } else if (!std::strcmp(argv[i], "--no-interactive")) {
            cfg.no_interactive = true;
        } else if (!std::strcmp(argv[i], "-n") && i + 1 < argc) {
            cfg.head_rows     = std::atoi(argv[++i]);
            cfg.head_rows_set = true;
        } else if (!std::strcmp(argv[i], "-w") && i + 1 < argc) {
            cfg.max_col_w = std::max(4, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "-c") && i + 1 < argc) {
            cfg.max_cols = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--color") ||
                   !std::strcmp(argv[i], "--color=auto")) {
            cfg.color = ColorMode::Auto;
        } else if (!std::strcmp(argv[i], "--color=always")) {
            cfg.color = ColorMode::Always;
        } else if (!std::strcmp(argv[i], "--color=never")) {
            cfg.color = ColorMode::Never;
        } else if (!std::strcmp(argv[i], "--tsv")) {
            cfg.delimiter = '\t';
        } else if (!std::strcmp(argv[i], "--csv")) {
            cfg.delimiter = ',';
        } else if (!std::strcmp(argv[i], "--delimiter") && i + 1 < argc) {
            const char* sep = argv[++i];
            if (!std::strcmp(sep, "tab"))   cfg.delimiter = '\t';
            else if (!std::strcmp(sep, "comma")) cfg.delimiter = ',';
            else if (sep[0] && !sep[1])     cfg.delimiter = sep[0];
            else { std::fprintf(stderr, "delimiter must be a single character\n"); std::exit(1); }
        } else if (argv[i][0] != '-') {
            if (positional++ == 0) cfg.path = argv[i];
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]); std::exit(1);
        }
    }
    if (cfg.path.empty()) { print_usage(argv[0]); std::exit(1); }
    return cfg;
}

// ── Value formatting ──────────────────────────────────────────────────────────

// U+2026 HORIZONTAL ELLIPSIS — 3 UTF-8 bytes, but exactly 1 terminal column.
// Used as the truncation marker: shows more content than "..." for the same width.
static constexpr const char ELLIPSIS[]    = "\xe2\x80\xa6";

// U+2205 EMPTY SET — 3 UTF-8 bytes, 1 terminal column.
// Displayed in place of NULL values: compact and unambiguous.
static constexpr const char NULL_SYMBOL[] = "\xe2\x88\x85";

static bool is_numeric_type(arrow::Type::type t) {
    switch (t) {
        case arrow::Type::INT8:    case arrow::Type::INT16:
        case arrow::Type::INT32:   case arrow::Type::INT64:
        case arrow::Type::UINT8:   case arrow::Type::UINT16:
        case arrow::Type::UINT32:  case arrow::Type::UINT64:
        case arrow::Type::FLOAT:   case arrow::Type::DOUBLE:
        case arrow::Type::DECIMAL128: case arrow::Type::DECIMAL256:
        case arrow::Type::DATE32:  case arrow::Type::DATE64:
        case arrow::Type::DURATION:
        case arrow::Type::TIME32:  case arrow::Type::TIME64:
        case arrow::Type::TIMESTAMP:
            return true;
        default: return false;
    }
}

static int display_width(const std::string& s) {
    // Count Unicode codepoints: UTF-8 continuation bytes (10xxxxxx) don't add columns.
    int w = 0;
    for (unsigned char c : s)
        if ((c & 0xC0u) != 0x80u) ++w;
    return w;
}

static std::string truncate(const std::string& s, int max_w) {
    if (max_w < 2) max_w = 2;
    if (display_width(s) <= max_w) return s;
    // ELLIPSIS is 3 UTF-8 bytes but 1 display column, so we keep (max_w-1) content chars.
    return s.substr(0, max_w - 1) + ELLIPSIS;
}

// Format a non-negative decimal integer string with '_' grouping every three digits.
// e.g. "123456789" → "123_456_789".  Non-numeric strings pass through unchanged.
static std::string digits_with_sep(const std::string& s) {
    if (s.empty()) return s;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return s;
    std::string r;
    r.reserve(s.size() + (s.size() - 1) / 3);
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (s.size() - i) % 3 == 0) r += '_';
        r += s[i];
    }
    return r;
}

// Parse a BED itemRgb field "r,g,b" into three 0-255 components.
static bool parse_rgb(const std::string& s, int* r, int* g, int* b) {
    return std::sscanf(s.c_str(), "%d,%d,%d", r, g, b) == 3 &&
           (unsigned)*r <= 255 && (unsigned)*g <= 255 && (unsigned)*b <= 255;
}

// Map an RGB triplet to the nearest xterm 256-color index (6×6×6 cube, indices 16-231).
static int nearest_256(int r, int g, int b) {
    auto q = [](int v) -> int {
        if (v < 48)  return 0;
        if (v < 115) return 1;
        return (v - 35) / 40;   // 115→2, 155→3, 195→4, 235→5
    };
    return 16 + 36 * q(r) + 6 * q(g) + q(b);
}

static std::string cell_to_string(const arrow::Array& arr, int64_t row) {
    if (arr.IsNull(row)) return NULL_SYMBOL;

    switch (arr.type_id()) {
        case arrow::Type::BOOL:
            return static_cast<const arrow::BooleanArray&>(arr).Value(row) ? "true" : "false";
        case arrow::Type::INT8:
            return std::to_string(static_cast<const arrow::Int8Array&>(arr).Value(row));
        case arrow::Type::INT16:
            return std::to_string(static_cast<const arrow::Int16Array&>(arr).Value(row));
        case arrow::Type::INT32:
            return std::to_string(static_cast<const arrow::Int32Array&>(arr).Value(row));
        case arrow::Type::INT64:
            return std::to_string(static_cast<const arrow::Int64Array&>(arr).Value(row));
        case arrow::Type::UINT8:
            return std::to_string(static_cast<const arrow::UInt8Array&>(arr).Value(row));
        case arrow::Type::UINT16:
            return std::to_string(static_cast<const arrow::UInt16Array&>(arr).Value(row));
        case arrow::Type::UINT32:
            return std::to_string(static_cast<const arrow::UInt32Array&>(arr).Value(row));
        case arrow::Type::UINT64:
            return std::to_string(static_cast<const arrow::UInt64Array&>(arr).Value(row));
        case arrow::Type::FLOAT: {
            std::ostringstream ss;
            ss << std::setprecision(6) << static_cast<const arrow::FloatArray&>(arr).Value(row);
            return ss.str();
        }
        case arrow::Type::DOUBLE: {
            std::ostringstream ss;
            ss << std::setprecision(8) << static_cast<const arrow::DoubleArray&>(arr).Value(row);
            return ss.str();
        }
        case arrow::Type::STRING:
            return static_cast<const arrow::StringArray&>(arr).GetString(row);
        case arrow::Type::LARGE_STRING:
            return static_cast<const arrow::LargeStringArray&>(arr).GetString(row);
        case arrow::Type::BINARY: {
            auto& a = static_cast<const arrow::BinaryArray&>(arr);
            return "<binary " + std::to_string(a.value_length(row)) + "B>";
        }
        case arrow::Type::LARGE_BINARY: {
            auto& a = static_cast<const arrow::LargeBinaryArray&>(arr);
            return "<binary " + std::to_string(a.value_length(row)) + "B>";
        }
        case arrow::Type::DICTIONARY: {
            auto& dict_arr = static_cast<const arrow::DictionaryArray&>(arr);
            auto  dict     = dict_arr.dictionary();
            auto  indices  = dict_arr.indices();
            int64_t idx = -1;
            switch (indices->type_id()) {
                case arrow::Type::INT8:   idx = static_cast<const arrow::Int8Array&>(*indices).Value(row);   break;
                case arrow::Type::INT16:  idx = static_cast<const arrow::Int16Array&>(*indices).Value(row);  break;
                case arrow::Type::INT32:  idx = static_cast<const arrow::Int32Array&>(*indices).Value(row);  break;
                case arrow::Type::INT64:  idx = static_cast<const arrow::Int64Array&>(*indices).Value(row);  break;
                case arrow::Type::UINT8:  idx = static_cast<const arrow::UInt8Array&>(*indices).Value(row);  break;
                case arrow::Type::UINT16: idx = static_cast<const arrow::UInt16Array&>(*indices).Value(row); break;
                case arrow::Type::UINT32: idx = static_cast<const arrow::UInt32Array&>(*indices).Value(row); break;
                case arrow::Type::UINT64: idx = static_cast<int64_t>(static_cast<const arrow::UInt64Array&>(*indices).Value(row)); break;
                default: break;
            }
            if (idx >= 0 && idx < dict->length())
                return cell_to_string(*dict, idx);
            return NULL_SYMBOL;
        }
        default: {
            auto res = arr.GetScalar(row);
            return res.ok() ? res.ValueOrDie()->ToString() : "?";
        }
    }
}

// ── ASCII table drawing ───────────────────────────────────────────────────────

struct Column {
    std::string              header;
    bool                     right_align;
    bool                     is_index = false;
    bool                     is_bool  = false;
    bool                     is_rgb   = false;
    std::vector<std::string> cells;
    int                      width;
};

static void draw_separator(const std::vector<Column>& cols) {
    std::printf("%s+", g_color.border);
    for (auto& c : cols) {
        for (int i = 0; i < c.width + 2; ++i) std::putchar('-');
        std::putchar('+');
    }
    std::printf("%s\n", g_color.reset);
}

// Emit one cell with color, proper padding, but no border characters.
// Returns nothing; writes directly to stdout.
static void emit_cell(const Column& col, const std::string& val,
                      bool right_align, bool is_header) {
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

    // For truncated values, render the body normally and the "…" dimmed.
    // ELLIPSIS is 3 UTF-8 bytes so the body is val.size()-3 bytes (same arithmetic as "...").
    bool truncated = !is_header && val.size() >= 3 &&
                     val.compare(val.size() - 3, 3, ELLIPSIS) == 0;

    if (right_align) {
        std::printf(" %*s", pad, "");   // leading spaces (no color)
        if (truncated) {
            std::printf("%s%.*s%s%s%s%s",
                fg, (int)val.size() - 3, val.c_str(),   // body
                g_color.reset, g_color.trunc, ELLIPSIS, g_color.reset);
        } else {
            std::printf("%s%s%s", fg, val.c_str(), *fg ? g_color.reset : "");
        }
    } else {
        if (truncated) {
            std::printf(" %s%.*s%s%s%s%s%*s",
                fg, (int)val.size() - 3, val.c_str(),   // body
                g_color.reset, g_color.trunc, ELLIPSIS, g_color.reset,
                pad, "");
        } else {
            std::printf(" %s%s%s%*s",
                fg, val.c_str(), *fg ? g_color.reset : "", pad, "");
        }
    }
}

static void draw_row(const std::vector<Column>& cols,
                     const std::vector<std::string>& vals,
                     const std::vector<bool>& right_align,
                     bool is_header = false) {
    for (std::size_t i = 0; i < cols.size(); ++i) {
        std::printf("%s|%s", g_color.border, g_color.reset);
        int r, gv, b;
        if (!is_header && cols[i].is_rgb && *g_color.reset
                       && parse_rgb(vals[i], &r, &gv, &b)) {
            // Truecolor background bar using ANSI 24-bit escape; width = col.width + 2
            std::printf(" \033[48;2;%d;%d;%dm%*s\033[0m ", r, gv, b, cols[i].width, "");
        } else {
            emit_cell(cols[i], vals[i], right_align[i], is_header);
            std::printf(" ");
        }
    }
    std::printf("%s|%s\n", g_color.border, g_color.reset);
}

// ── Delimited output ─────────────────────────────────────────────────────────

// RFC 4180 quoting: wrap in double-quotes if the value contains the delimiter,
// a double-quote, or a newline; escape embedded quotes by doubling them.
static void write_csv_field(const std::string& val, char sep) {
    bool needs_quote = val.find(sep)  != std::string::npos ||
                       val.find('"')  != std::string::npos ||
                       val.find('\n') != std::string::npos ||
                       val.find('\r') != std::string::npos;
    if (!needs_quote) {
        std::fputs(val.c_str(), stdout);
        return;
    }
    std::putchar('"');
    for (char c : val) {
        if (c == '"') std::putchar('"');   // double the quote
        std::putchar(c);
    }
    std::putchar('"');
}

// Suffix-match helper shared by DelimitedSource::open and open_source.
static bool fends(const std::string& s, const std::string& sfx) {
    return s.size() >= sfx.size() &&
           s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}

// ── Preamble helpers and stream wrappers ──────────────────────────────────────

// Minimal InputStream that serves 'prefix' bytes first, then delegates to 'rest'.
// Used to "put back" the first non-preamble line when reading gzipped files.
class PrependInputStream : public arrow::io::InputStream {
    std::string  prefix_;
    size_t       pos_ = 0;
    std::shared_ptr<arrow::io::InputStream> rest_;
public:
    PrependInputStream(std::string prefix, std::shared_ptr<arrow::io::InputStream> rest)
        : prefix_(std::move(prefix)), rest_(std::move(rest)) {}

    arrow::Status Close() override { return rest_->Close(); }
    bool closed() const override { return rest_->closed(); }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("PrependInputStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* out) override {
        uint8_t* p = static_cast<uint8_t*>(out);
        int64_t total = 0;
        if (pos_ < prefix_.size()) {
            int64_t from_pre = std::min<int64_t>(n, (int64_t)(prefix_.size() - pos_));
            std::memcpy(p, prefix_.data() + pos_, (size_t)from_pre);
            pos_ += (size_t)from_pre; p += from_pre; n -= from_pre; total += from_pre;
        }
        if (n > 0) {
            ARROW_ASSIGN_OR_RAISE(int64_t from_rest, rest_->Read(n, p));
            total += from_rest;
        }
        return total;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(n));
        ARROW_ASSIGN_OR_RAISE(int64_t actual, Read(n, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(actual, false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }
};

// Buffered line reader wrapping an Arrow InputStream.
// Reads in 8 KiB chunks to amortise per-call overhead — critical for
// TruncateFieldsStream which processes every data line in GFF/SAM files.
class LineReader {
    std::shared_ptr<arrow::io::InputStream> src_;
    static constexpr int BUF = 8192;
    char   buf_[BUF];
    int    pos_ = 0, fill_ = 0;
    bool   eof_ = false;

    void refill_buf() {
        auto r = src_->Read(BUF);
        if (!r.ok() || (*r)->size() == 0) { eof_ = true; return; }
        fill_ = (int)(*r)->size();
        std::memcpy(buf_, (*r)->data(), fill_);
        pos_ = 0;
    }
public:
    explicit LineReader(std::shared_ptr<arrow::io::InputStream> s) : src_(std::move(s)) {}

    // Returns true if a '\n' terminated the line; false on EOF (line may have content).
    bool read_line(std::string* out) {
        out->clear();
        for (;;) {
            if (pos_ >= fill_) {
                if (eof_) return false;
                refill_buf();
                if (eof_ && pos_ >= fill_) return !out->empty();
            }
            while (pos_ < fill_) {
                char c = buf_[pos_++];
                if (c == '\n') return true;
                if (c != '\r') *out += c;
            }
        }
    }

    // Any bytes already fetched from the stream but not yet consumed by read_line.
    // Use this to create a PrependInputStream after preamble stripping so no
    // look-ahead bytes are lost.
    std::string leftover() const {
        return (pos_ < fill_) ? std::string(buf_ + pos_, fill_ - pos_) : std::string{};
    }
};

// Single-byte fallback for seekable streams (preamble strippers that need Seek).
// Returns true if a newline was found; false on EOF.
static bool read_stream_line(
    const std::shared_ptr<arrow::io::InputStream>& input, std::string* out)
{
    out->clear();
    for (;;) {
        auto r = input->Read(1);
        if (!r.ok() || (*r)->size() == 0) return !out->empty();
        char c = static_cast<char>((*r)->data()[0]);
        if (c == '\n') return true;
        if (c != '\r') *out += c;
    }
}

// Reads and strips "track"/"browser" preamble lines from the current stream position.
// For seekable streams (non-gz): leaves the stream positioned at the first data byte.
// For non-seekable streams (gz): writes the first non-preamble line to *put_back.
static std::vector<std::string> strip_bed_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    bool seekable, std::string* put_back)
{
    std::vector<std::string> headers;
    int64_t after_last = 0;
    if (seekable) {
        auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
        if (auto t = rf->Tell(); t.ok()) after_last = *t;
    }
    for (;;) {
        std::string line;
        bool ok = read_stream_line(input, &line);
        if (!ok && line.empty()) break;
        auto has_prefix = [&](const char* p, size_t n) {
            return line.size() >= n && line.compare(0, n, p, n) == 0 &&
                   (line.size() == n || line[n] == ' ' || line[n] == '\t');
        };
        if (has_prefix("track", 5) || has_prefix("browser", 7)) {
            headers.push_back(line);
            if (seekable) {
                auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
                if (auto t = rf->Tell(); t.ok()) after_last = *t;
            }
            if (!ok) break;
        } else {
            if (seekable) {
                auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
                (void)rf->Seek(after_last);
            } else if (put_back) {
                *put_back = line + "\n";
            }
            break;
        }
    }
    return headers;
}

// Strips lines whose first character equals prefix_char (e.g. '#' for GFF3, '@' for SAM).
// Seekable: stream left positioned at the first non-preamble line.
// Non-seekable (gz): first non-preamble line → *put_back.
static std::vector<std::string> strip_prefix_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    char prefix_char, bool seekable, std::string* put_back)
{
    std::vector<std::string> preamble;
    int64_t after_last = 0;
    if (seekable) {
        auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
        if (auto t = rf->Tell(); t.ok()) after_last = *t;
    }
    for (;;) {
        std::string line;
        bool ok = read_stream_line(input, &line);
        if (!ok && line.empty()) break;
        if (!line.empty() && line[0] == prefix_char) {
            preamble.push_back(line);
            if (seekable) {
                auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
                if (auto t = rf->Tell(); t.ok()) after_last = *t;
            }
            if (!ok) break;
        } else {
            if (seekable) {
                auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
                (void)rf->Seek(after_last);
            } else if (put_back) {
                *put_back = line + "\n";
            }
            break;
        }
    }
    return preamble;
}

// VCF: strips ## meta-information lines; reads the #CHROM header line for column names.
// The #CHROM line is consumed; stream is left at the first data line.
// *col_names_out is populated from #CHROM; returned vector contains ## lines only.
static std::vector<std::string> strip_vcf_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    std::vector<std::string>* col_names_out)
{
    std::vector<std::string> preamble;
    for (;;) {
        std::string line;
        bool ok = read_stream_line(input, &line);
        if (!ok && line.empty()) break;
        if (line.size() >= 2 && line[0] == '#' && line[1] == '#') {
            preamble.push_back(line);
            if (!ok) break;
        } else if (!line.empty() && line[0] == '#') {
            // #CHROM line: strip leading '#', split on tab → column names
            std::istringstream ss(line.substr(1));
            std::string tok;
            while (std::getline(ss, tok, '\t'))
                col_names_out->push_back(tok);
            break;
        } else {
            break;  // data before #CHROM (malformed); don't consume
        }
    }
    return preamble;
}

// Wraps an InputStream, truncating each line to at most max_fields tab-separated fields.
// Used for SAM (variable optional alignment tags) and GFF3 (occasional extra columns).
class TruncateFieldsStream : public arrow::io::InputStream {
    std::shared_ptr<arrow::io::InputStream> inner_;
    LineReader   lr_;          // buffered reader — avoids one-byte-at-a-time reads
    int          max_fields_;
    std::string  out_buf_;
    size_t       out_pos_    = 0;
    bool         inner_done_ = false;

    bool refill() {
        out_buf_.clear(); out_pos_ = 0;
        std::string line;
        while (line.empty()) {
            bool ok = lr_.read_line(&line);
            if (!ok && line.empty()) { inner_done_ = true; return false; }
            if (!ok) inner_done_ = true;
        }
        // Truncate to at most max_fields tab-separated fields
        int fields = 0;
        size_t end = line.size();
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\t' && ++fields == max_fields_) { end = i; break; }
        }
        out_buf_ = line.substr(0, end);
        out_buf_ += '\n';
        return true;
    }

public:
    TruncateFieldsStream(std::shared_ptr<arrow::io::InputStream> inner, int max_fields)
        : inner_(inner), lr_(inner), max_fields_(max_fields) {}

    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("TruncateFieldsStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* buf) override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        int64_t total = 0;
        while (n > 0) {
            if (out_pos_ >= out_buf_.size()) {
                if (inner_done_ || !refill()) break;
            }
            int64_t avail = (int64_t)(out_buf_.size() - out_pos_);
            int64_t take  = std::min(n, avail);
            std::memcpy(p, out_buf_.data() + out_pos_, (size_t)take);
            p += take; out_pos_ += (size_t)take; n -= take; total += take;
        }
        return total;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(n));
        ARROW_ASSIGN_OR_RAISE(int64_t actual, Read(n, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(actual, false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }
};

// ── Tabular source abstraction ────────────────────────────────────────────────
//
// Common interface for Parquet, CSV, TSV, BED, etc.
// "Chunks" map to row groups (Parquet) or read batches (delimited).

struct ChunkMeta { int64_t first_row; int64_t num_rows; };

class TabularSource {
public:
    virtual ~TabularSource() = default;
    virtual std::shared_ptr<arrow::Schema> schema() const = 0;
    virtual int64_t total_rows() const = 0;     // -1 = not yet fully scanned
    virtual int     num_chunks() const = 0;     // known so far
    virtual ChunkMeta chunk_meta(int i) const = 0;
    // Read chunk i, keeping only col_indices columns.
    virtual arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                                      std::shared_ptr<arrow::Table>* out) = 0;
    // Ensure chunk i is loaded (triggers forward reads for streaming sources).
    virtual void ensure(int i) {}
    virtual const std::string& path() const = 0;
    // One-line footer shown after the table / in the TUI status bar.
    virtual std::string footer() const = 0;
    virtual std::string created_by() const { return ""; }
    // Header lines shown before the table (BED track/browser lines).
    virtual std::vector<std::string> preamble_above() const { return {}; }
    // Meta header lines shown after the schema (VCF/BAM/SAM/GFF).
    virtual std::vector<std::string> preamble_below() const { return {}; }
    // Post-process a cell value for human-readable display (table view and TUI).
    // NOT called for delimited (--csv/--tsv) output.
    virtual std::string format_cell(int /*col_idx*/, std::string val) const { return val; }
    // Minimum display-column width for a given column index (TUI pre-sizes columns from this).
    virtual int min_col_width(int /*col_idx*/) const { return 4; }
};

// ── Parquet source ────────────────────────────────────────────────────────────

class ParquetSource : public TabularSource {
    std::unique_ptr<parquet::arrow::FileReader> reader_;
    std::shared_ptr<parquet::FileMetaData>      meta_;
    std::shared_ptr<arrow::Schema>              schema_;
    std::string                                  path_;
    std::vector<int64_t>                         chunk_start_;

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
        if (cfg.head_rows > 0) props.set_batch_size(cfg.head_rows);

        parquet::arrow::FileReaderBuilder builder;
        auto st = builder.Open(maybe_file.ValueOrDie());
        if (!st.ok()) return "Not a valid Parquet file: " + st.ToString();
        builder.memory_pool(arrow::default_memory_pool());
        builder.properties(props);
        st = builder.Build(&self->reader_);
        if (!st.ok()) return "Error opening Parquet: " + st.ToString();

        self->meta_ = self->reader_->parquet_reader()->metadata();
        st = self->reader_->GetSchema(&self->schema_);
        if (!st.ok()) return "Error reading schema: " + st.ToString();

        int64_t acc = 0;
        for (int i = 0; i < self->meta_->num_row_groups(); ++i) {
            self->chunk_start_.push_back(acc);
            acc += self->meta_->RowGroup(i)->num_rows();
        }
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return meta_->num_rows(); }
    int     num_chunks() const override { return meta_->num_row_groups(); }
    ChunkMeta chunk_meta(int i) const override {
        return {chunk_start_[i], meta_->RowGroup(i)->num_rows()};
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols,
                              std::shared_ptr<arrow::Table>* out) override {
        return reader_->ReadRowGroups({i}, cols, out);
    }
    const std::string& path() const override { return path_; }

    std::string footer() const override {
        int64_t sz = 0;
        for (int i = 0; i < meta_->num_row_groups(); ++i)
            sz += meta_->RowGroup(i)->total_compressed_size();
        return "Row groups: " + std::to_string(meta_->num_row_groups()) +
               "  |  Compressed: " + fmt_size(sz);
    }
    std::string created_by() const override { return meta_->created_by(); }
};

// ── Delimited source (CSV / TSV / BED / VCF / GFF3+GTF / SAM, plain or gzip) ──

enum class DelimKind { CSV, TSV, BED, VCF, GFF, SAM };

// Wrap a single RecordBatch column slice as a single-chunk Table.
static std::shared_ptr<arrow::Table> batch_slice_to_table(
    const arrow::RecordBatch& batch,
    const std::vector<int>& col_indices,
    const std::shared_ptr<arrow::Schema>& full_schema)
{
    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    arrow::FieldVector fields;
    for (int ci : col_indices) {
        cols.push_back(std::make_shared<arrow::ChunkedArray>(
            arrow::ArrayVector{batch.column(ci)}));
        fields.push_back(full_schema->field(ci));
    }
    return arrow::Table::Make(arrow::schema(fields), cols, batch.num_rows());
}

class DelimitedSource : public TabularSource {
    std::string                           path_;
    char                                  delimiter_;
    DelimKind                             kind_;
    std::shared_ptr<arrow::Schema>        schema_;
    std::vector<std::string>              preamble_lines_;

    // Growing cache of batches read so far (never evicted).
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>          batch_first_row_;
    mutable int64_t                       rows_so_far_ = 0;
    mutable bool                          all_read_    = false;

    mutable std::shared_ptr<arrow::csv::StreamingReader> reader_;

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        std::shared_ptr<arrow::RecordBatch> batch;
        ARROW_RETURN_NOT_OK(reader_->ReadNext(&batch));
        if (!batch) { all_read_ = true; return arrow::Status::OK(); }
        batch_first_row_.push_back(rows_so_far_);
        rows_so_far_ += batch->num_rows();
        batches_.push_back(std::move(batch));
        return arrow::Status::OK();
    }

    // Open the file as a (possibly gzip-decompressed) InputStream.
    static std::string open_stream(const std::string& path, bool is_gz,
                                    std::shared_ptr<arrow::io::ReadableFile>*  raw_out,
                                    std::shared_ptr<arrow::io::InputStream>*   input_out) {
        auto maybe_raw = arrow::io::ReadableFile::Open(path);
        if (!maybe_raw.ok())
            return "Cannot open '" + path + "': " + maybe_raw.status().ToString();
        auto raw = maybe_raw.ValueOrDie();
        std::shared_ptr<arrow::io::InputStream> input = raw;
        if (is_gz) {
            auto mc = arrow::util::Codec::Create(arrow::Compression::GZIP);
            if (!mc.ok()) return mc.status().ToString();
            auto ci = arrow::io::CompressedInputStream::Make(mc->get(), raw);
            if (!ci.ok()) return ci.status().ToString();
            input = ci.ValueOrDie();
        }
        *raw_out   = raw;
        *input_out = input;
        return "";
    }

    // Create a StreamingReader from an already-open stream.
    static arrow::Result<std::shared_ptr<arrow::csv::StreamingReader>>
    make_reader(std::shared_ptr<arrow::io::InputStream> input, char delim,
                bool autogen_names, const std::vector<std::string>& col_names) {
        auto ropts = arrow::csv::ReadOptions::Defaults();
        if (!col_names.empty())
            ropts.column_names = col_names;
        else
            ropts.autogenerate_column_names = autogen_names;
        auto popts = arrow::csv::ParseOptions::Defaults();
        popts.delimiter = delim;
        return arrow::csv::StreamingReader::Make(
            arrow::io::default_io_context(), input,
            ropts, popts, arrow::csv::ConvertOptions::Defaults());
    }

public:
    static std::string open(const std::string& path, DelimKind kind,
                             std::unique_ptr<DelimitedSource>* out) {
        auto self = std::make_unique<DelimitedSource>();
        self->path_      = path;
        self->kind_      = kind;
        self->delimiter_ = (kind == DelimKind::CSV) ? ',' : '\t';

        bool is_gz = fends(path, ".gz");

        std::shared_ptr<arrow::io::ReadableFile>  raw;
        std::shared_ptr<arrow::io::InputStream>   input;
        {
            std::string err = open_stream(path, is_gz, &raw, &input);
            if (!err.empty()) return err;
        }

        // Format-specific preamble stripping and column-name determination.
        // GFF and SAM wrap the stream in TruncateFieldsStream to handle variable columns.
        std::vector<std::string> col_names;
        std::string put_back;

        switch (kind) {
            case DelimKind::BED:
                self->preamble_lines_ = strip_bed_preamble(input, !is_gz, &put_back);
                break;
            case DelimKind::VCF:
                self->preamble_lines_ = strip_vcf_preamble(input, &col_names);
                break;
            case DelimKind::GFF: {
                self->preamble_lines_ = strip_prefix_preamble(input, '#', !is_gz, &put_back);
                col_names = {"seqname","source","feature","start","end",
                             "score","strand","frame","attributes"};
                // Rebuild input with put_back then truncate to 9 fields
                std::shared_ptr<arrow::io::InputStream> base =
                    put_back.empty() ? input
                    : std::shared_ptr<arrow::io::InputStream>(
                        std::make_shared<PrependInputStream>(std::move(put_back), input));
                input = std::make_shared<TruncateFieldsStream>(base, 9);
                put_back.clear();
                break;
            }
            case DelimKind::SAM: {
                self->preamble_lines_ = strip_prefix_preamble(input, '@', !is_gz, &put_back);
                col_names = {"QNAME","FLAG","RNAME","POS","MAPQ",
                             "CIGAR","RNEXT","PNEXT","TLEN","SEQ","QUAL"};
                std::shared_ptr<arrow::io::InputStream> base =
                    put_back.empty() ? input
                    : std::shared_ptr<arrow::io::InputStream>(
                        std::make_shared<PrependInputStream>(std::move(put_back), input));
                input = std::make_shared<TruncateFieldsStream>(base, 11);
                put_back.clear();
                break;
            }
            default:
                break;  // CSV/TSV: no preamble
        }

        // BED non-gz: put_back still handled here; GFF/SAM already cleared it above.
        if (!put_back.empty())
            input = std::make_shared<PrependInputStream>(std::move(put_back), input);

        bool autogen = (kind == DelimKind::BED);
        auto r = make_reader(input, self->delimiter_, autogen, col_names);
        if (!r.ok()) return "Cannot open '" + path + "': " + r.status().ToString();
        self->reader_ = r.ValueOrDie();
        self->schema_ = self->reader_->schema();

        // For CSV/TSV: if all column names are numeric, the file has no header → retry.
        if (kind == DelimKind::CSV || kind == DelimKind::TSV) {
            bool all_numeric = self->schema_->num_fields() > 0;
            for (int i = 0; i < self->schema_->num_fields() && all_numeric; ++i) {
                const std::string& nm = self->schema_->field(i)->name();
                char* ep; std::strtod(nm.c_str(), &ep);
                if (*ep != '\0') all_numeric = false;
            }
            if (all_numeric) {
                std::shared_ptr<arrow::io::ReadableFile>  raw2;
                std::shared_ptr<arrow::io::InputStream>   input2;
                if (open_stream(path, is_gz, &raw2, &input2).empty()) {
                    auto r2 = make_reader(input2, self->delimiter_, /*autogen=*/true, {});
                    if (r2.ok()) {
                        self->reader_ = r2.ValueOrDie();
                        self->schema_ = self->reader_->schema();
                    }
                }
            }
        }

        // BED: assign standard column names based on count and inferred types.
        if (kind == DelimKind::BED) {
            int nf = self->schema_->num_fields();
            if (nf >= 3) {
                arrow::FieldVector fields;
                fields.reserve(nf);
                for (int i = 0; i < nf; ++i) {
                    auto f   = self->schema_->field(i);
                    auto tid = f->type()->id();
                    bool is_int = (tid == arrow::Type::INT8  || tid == arrow::Type::INT16  ||
                                   tid == arrow::Type::INT32 || tid == arrow::Type::INT64  ||
                                   tid == arrow::Type::UINT8 || tid == arrow::Type::UINT16 ||
                                   tid == arrow::Type::UINT32|| tid == arrow::Type::UINT64);
                    bool is_str = (tid == arrow::Type::STRING || tid == arrow::Type::LARGE_STRING);
                    std::string nm;
                    switch (i) {
                        case 0: nm = "Chr";  break;
                        case 1: nm = "[Beg"; break;
                        case 2: nm = "End)"; break;
                        case 3: nm = is_str ? "Name"  : f->name(); break;
                        case 4: nm = is_int ? "Score" : f->name(); break;
                        case 5: nm = is_str ? "Str"   : f->name(); break;
                        case 6: nm = is_int ? "ThBeg" : f->name(); break;
                        case 7: nm = is_int ? "ThEnd" : f->name(); break;
                        case 8: nm = is_str ? "RGB"   : f->name(); break;
                        default: nm = f->name(); break;
                    }
                    fields.push_back(arrow::field(nm, f->type(), f->nullable()));
                }
                self->schema_ = arrow::schema(fields);
            }
        }

        // Eagerly read first batch so schema + first rows are immediately available.
        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batches_[i]->num_rows()};
    }

    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }

    const std::string& path() const override { return path_; }

    std::string footer() const override {
        switch (kind_) {
            case DelimKind::VCF: return "Format: VCF";
            case DelimKind::GFF: return "Format: GFF3/GTF";
            case DelimKind::SAM: return "Format: SAM";
            case DelimKind::BED: return "Format: BED";
            default:
                std::string d(1, delimiter_);
                if (delimiter_ == '\t') d = "tab";
                return "Format: delimited (separator: " + d + ")";
        }
    }

    // BED track/browser lines are shown above the table; all other format
    // preambles (VCF ##INFO/##FILTER, SAM @SQ/@PG, GFF ##) go below the schema.
    std::vector<std::string> preamble_above() const override {
        return (kind_ == DelimKind::BED) ? preamble_lines_ : std::vector<std::string>{};
    }
    std::vector<std::string> preamble_below() const override {
        if (kind_ == DelimKind::BED) return {};
        if (preamble_lines_.size() <= 20) return preamble_lines_;
        std::vector<std::string> out(preamble_lines_.begin(), preamble_lines_.begin() + 20);
        out.push_back("... (" + std::to_string(preamble_lines_.size() - 20) + " more header lines)");
        return out;
    }

    // Per-format: display coordinate columns with '_' digit grouping.
    std::string format_cell(int col_idx, std::string val) const override {
        switch (kind_) {
            case DelimKind::BED:
                if (col_idx == 1 || col_idx == 2) return digits_with_sep(val);
                break;
            case DelimKind::VCF:
                if (col_idx == 1) return digits_with_sep(val);          // POS
                break;
            case DelimKind::GFF:
                if (col_idx == 3 || col_idx == 4) return digits_with_sep(val); // start, end
                break;
            case DelimKind::SAM:
                if (col_idx == 3 || col_idx == 7) return digits_with_sep(val); // POS, PNEXT
                break;
            default: break;
        }
        return val;
    }

    int min_col_width(int col_idx) const override {
        switch (kind_) {
            case DelimKind::BED:
                switch (col_idx) {
                    case 1: case 2: return 11;  // [Beg / End): "999_999_999"
                    case 3:         return 6;   // Name
                    case 5:         return 3;   // Str (strand: +/-/.)
                    case 8:         return 5;   // RGB bar
                    default:        return 4;
                }
            case DelimKind::VCF:
                switch (col_idx) {
                    case 1:  return 9;   // POS
                    case 7:  return 12;  // INFO
                    default: return 4;
                }
            case DelimKind::GFF:
                switch (col_idx) {
                    case 3: case 4: return 9;   // start / end
                    case 6:         return 3;   // strand (+/-/.)
                    case 8:         return 15;  // attributes
                    default:        return 4;
                }
            case DelimKind::SAM:
                switch (col_idx) {
                    case 0:  return 10;  // QNAME
                    case 3:  return 9;   // POS
                    case 5:  return 8;   // CIGAR
                    case 7:  return 9;   // PNEXT
                    case 9:  return 10;  // SEQ
                    case 10: return 10;  // QUAL
                    default: return 4;
                }
            default: return 4;
        }
    }
};

// ── BAM source ────────────────────────────────────────────────────────────────

class BamSource : public TabularSource {
    std::string                           path_;
    std::shared_ptr<arrow::Schema>        schema_;
    std::vector<std::string>              preamble_lines_;
    int                                   num_refs_   = 0;
    std::string                           fmt_name_;   // "BAM", "CRAM", or "SAM"

    mutable htsFile*   hts_ = nullptr;
    mutable sam_hdr_t* hdr_ = nullptr;
    mutable bam1_t*    rec_ = nullptr;   // reused across advance() calls

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>          batch_first_row_;
    mutable int64_t                       rows_so_far_ = 0;
    mutable bool                          all_read_    = false;

    static constexpr int BATCH_SIZE = 32768;

    static constexpr const char NT16[] = "=ACMGRSVTWYHKDBN";

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();

        arrow::StringBuilder qname_b, rname_b, cigar_b, rnext_b, seq_b, qual_b;
        arrow::Int32Builder  flag_b, mapq_b;
        arrow::Int64Builder  pos_b, pnext_b, tlen_b;

        int count = 0, ret = 0;
        while (count < BATCH_SIZE && (ret = sam_read1(hts_, hdr_, rec_)) >= 0) {
            ARROW_RETURN_NOT_OK(qname_b.Append(bam_get_qname(rec_)));
            ARROW_RETURN_NOT_OK(flag_b.Append((int32_t)rec_->core.flag));

            // RNAME / POS
            if (rec_->core.tid < 0) {
                ARROW_RETURN_NOT_OK(rname_b.Append("*"));
                ARROW_RETURN_NOT_OK(pos_b.Append((int64_t)0));
            } else {
                ARROW_RETURN_NOT_OK(rname_b.Append(
                    sam_hdr_tid2name(hdr_, rec_->core.tid)));
                ARROW_RETURN_NOT_OK(pos_b.Append((int64_t)rec_->core.pos + 1));
            }

            ARROW_RETURN_NOT_OK(mapq_b.Append((int32_t)rec_->core.qual));

            // CIGAR
            {
                uint32_t nc = rec_->core.n_cigar;
                if (nc == 0) {
                    ARROW_RETURN_NOT_OK(cigar_b.Append("*"));
                } else {
                    std::string cig;
                    cig.reserve(nc * 5);
                    const uint32_t* cr = bam_get_cigar(rec_);
                    for (uint32_t i = 0; i < nc; ++i) {
                        char buf[16];
                        int n = std::snprintf(buf, sizeof(buf), "%u%c",
                                              bam_cigar_oplen(cr[i]),
                                              bam_cigar_opchr(cr[i]));
                        cig.append(buf, n);
                    }
                    ARROW_RETURN_NOT_OK(cigar_b.Append(cig));
                }
            }

            // RNEXT / PNEXT
            if (rec_->core.mtid < 0) {
                ARROW_RETURN_NOT_OK(rnext_b.Append("*"));
                ARROW_RETURN_NOT_OK(pnext_b.Append((int64_t)0));
            } else if (rec_->core.mtid == rec_->core.tid) {
                ARROW_RETURN_NOT_OK(rnext_b.Append("="));
                ARROW_RETURN_NOT_OK(pnext_b.Append((int64_t)rec_->core.mpos + 1));
            } else {
                ARROW_RETURN_NOT_OK(rnext_b.Append(
                    sam_hdr_tid2name(hdr_, rec_->core.mtid)));
                ARROW_RETURN_NOT_OK(pnext_b.Append((int64_t)rec_->core.mpos + 1));
            }

            ARROW_RETURN_NOT_OK(tlen_b.Append((int64_t)rec_->core.isize));

            // SEQ
            {
                int lq = rec_->core.l_qseq;
                if (lq == 0) {
                    ARROW_RETURN_NOT_OK(seq_b.Append("*"));
                } else {
                    std::string seq((size_t)lq, ' ');
                    const uint8_t* s = bam_get_seq(rec_);
                    for (int i = 0; i < lq; ++i)
                        seq[i] = NT16[bam_seqi(s, i)];
                    ARROW_RETURN_NOT_OK(seq_b.Append(seq));
                }
            }

            // QUAL (Phred+33; '*' if not stored)
            {
                int lq = rec_->core.l_qseq;
                const uint8_t* q = bam_get_qual(rec_);
                if (lq == 0 || q[0] == 0xff) {
                    ARROW_RETURN_NOT_OK(qual_b.Append("*"));
                } else {
                    std::string qual((size_t)lq, ' ');
                    for (int i = 0; i < lq; ++i)
                        qual[i] = (char)(q[i] + 33);
                    ARROW_RETURN_NOT_OK(qual_b.Append(qual));
                }
            }

            ++count;
        }

        if (ret < -1)
            return arrow::Status::IOError("Error reading BAM record from ", path_);
        if (count == 0) { all_read_ = true; return arrow::Status::OK(); }
        if (ret < 0) all_read_ = true;   // EOF hit during this batch

        std::shared_ptr<arrow::Array> a[11];
        ARROW_RETURN_NOT_OK(qname_b.Finish(&a[0]));
        ARROW_RETURN_NOT_OK(flag_b.Finish(&a[1]));
        ARROW_RETURN_NOT_OK(rname_b.Finish(&a[2]));
        ARROW_RETURN_NOT_OK(pos_b.Finish(&a[3]));
        ARROW_RETURN_NOT_OK(mapq_b.Finish(&a[4]));
        ARROW_RETURN_NOT_OK(cigar_b.Finish(&a[5]));
        ARROW_RETURN_NOT_OK(rnext_b.Finish(&a[6]));
        ARROW_RETURN_NOT_OK(pnext_b.Finish(&a[7]));
        ARROW_RETURN_NOT_OK(tlen_b.Finish(&a[8]));
        ARROW_RETURN_NOT_OK(seq_b.Finish(&a[9]));
        ARROW_RETURN_NOT_OK(qual_b.Finish(&a[10]));

        auto batch = arrow::RecordBatch::Make(schema_, count,
            {a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10]});
        batch_first_row_.push_back(rows_so_far_);
        rows_so_far_ += count;
        batches_.push_back(std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BamSource() {
        if (rec_) { bam_destroy1(rec_); rec_ = nullptr; }
        if (hdr_) { sam_hdr_destroy(hdr_); hdr_ = nullptr; }
        if (hts_) { hts_close(hts_); hts_ = nullptr; }
    }

    static std::string open(const std::string& path, std::unique_ptr<BamSource>* out) {
        auto self = std::make_unique<BamSource>();
        self->path_ = path;

        self->hts_ = hts_open(path.c_str(), "r");
        if (!self->hts_)
            return "Cannot open '" + path + "'";

        self->hdr_ = sam_hdr_read(self->hts_);
        if (!self->hdr_)
            return "Cannot read BAM/SAM header from '" + path + "'";

        self->rec_ = bam_init1();
        if (!self->rec_)
            return "Out of memory allocating BAM record";

        self->num_refs_ = sam_hdr_nref(self->hdr_);

        // Detect exact format (BAM / CRAM / SAM) for the footer
        {
            const htsFormat* fmt = hts_get_format(self->hts_);
            switch (fmt ? fmt->format : unknown_format) {
                case cram: self->fmt_name_ = "CRAM"; break;
                case sam:  self->fmt_name_ = "SAM";  break;
                default:   self->fmt_name_ = "BAM";  break;
            }
        }

        // Collect preamble lines from the embedded SAM header text (cap at 20)
        {
            int total = 0;
            std::istringstream ss(std::string(self->hdr_->text,
                                              (size_t)self->hdr_->l_text));
            std::string line;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                ++total;
                if (total <= 20) self->preamble_lines_.push_back(line);
            }
            if (total > 20)
                self->preamble_lines_.push_back(
                    "... (" + std::to_string(total - 20) + " more header lines)");
        }

        self->schema_ = arrow::schema({
            arrow::field("QNAME", arrow::utf8()),
            arrow::field("FLAG",  arrow::int32()),
            arrow::field("RNAME", arrow::utf8()),
            arrow::field("POS",   arrow::int64()),
            arrow::field("MAPQ",  arrow::int32()),
            arrow::field("CIGAR", arrow::utf8()),
            arrow::field("RNEXT", arrow::utf8()),
            arrow::field("PNEXT", arrow::int64()),
            arrow::field("TLEN",  arrow::int64()),
            arrow::field("SEQ",   arrow::utf8()),
            arrow::field("QUAL",  arrow::utf8()),
        });

        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batches_[i]->num_rows()};
    }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: " + fmt_name_ + "  |  References: " + std::to_string(num_refs_);
    }
    std::vector<std::string> preamble_below() const override { return preamble_lines_; }
    std::string format_cell(int col_idx, std::string val) const override {
        if (col_idx == 3 || col_idx == 7) return digits_with_sep(val);  // POS, PNEXT
        return val;
    }
    int min_col_width(int col_idx) const override {
        switch (col_idx) {
            case 0:  return 10;  // QNAME
            case 3:  return 9;   // POS
            case 5:  return 8;   // CIGAR
            case 7:  return 9;   // PNEXT
            case 9:  return 10;  // SEQ
            case 10: return 10;  // QUAL
            default: return 4;
        }
    }
};

// ── Arrow IPC / Feather source ────────────────────────────────────────────────

class IpcSource : public TabularSource {
    std::string                                    path_;
    bool                                           is_feather_ = false;
    std::shared_ptr<arrow::Schema>                 schema_;
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    std::vector<int64_t>                           batch_first_row_;
    int64_t                                        total_rows_ = 0;

    static constexpr int64_t BATCH_ROWS = 65536;

    // Slice a Table into BATCH_ROWS-sized RecordBatches and append to batches_.
    arrow::Status ingest_table(const std::shared_ptr<arrow::Table>& table) {
        arrow::TableBatchReader rdr(*table);
        rdr.set_chunksize(BATCH_ROWS);
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            ARROW_RETURN_NOT_OK(rdr.ReadNext(&batch));
            if (!batch) break;
            batch_first_row_.push_back(total_rows_);
            total_rows_ += batch->num_rows();
            batches_.push_back(std::move(batch));
        }
        return arrow::Status::OK();
    }

public:
    static std::string open(const std::string& path, bool is_feather,
                             std::unique_ptr<IpcSource>* out) {
        auto self = std::make_unique<IpcSource>();
        self->path_       = path;
        self->is_feather_ = is_feather;

        auto maybe_file = arrow::io::ReadableFile::Open(path);
        if (!maybe_file.ok())
            return "Cannot open '" + path + "': " + maybe_file.status().ToString();
        auto file = maybe_file.ValueOrDie();

        if (is_feather) {
            auto maybe_rdr = arrow::ipc::feather::Reader::Open(file);
            if (!maybe_rdr.ok())
                return "Not a valid Feather file: " + maybe_rdr.status().ToString();
            auto rdr = maybe_rdr.ValueOrDie();
            self->schema_ = rdr->schema();
            std::shared_ptr<arrow::Table> table;
            auto st = rdr->Read(&table);
            if (!st.ok()) return "Error reading Feather: " + st.ToString();
            st = self->ingest_table(table);
            if (!st.ok()) return "Error batching Feather: " + st.ToString();
        } else {
            auto maybe_rdr = arrow::ipc::RecordBatchFileReader::Open(file);
            if (!maybe_rdr.ok())
                return "Not a valid Arrow IPC file: " + maybe_rdr.status().ToString();
            auto rdr = maybe_rdr.ValueOrDie();
            self->schema_ = rdr->schema();
            for (int i = 0; i < rdr->num_record_batches(); ++i) {
                auto maybe_batch = rdr->ReadRecordBatch(i);
                if (!maybe_batch.ok()) continue;
                self->batch_first_row_.push_back(self->total_rows_);
                self->total_rows_ += maybe_batch.ValueOrDie()->num_rows();
                self->batches_.push_back(maybe_batch.ValueOrDie());
            }
        }

        if (self->batches_.empty()) {
            // Empty file: create one zero-row batch so the schema is visible.
            self->batch_first_row_.push_back(0);
            self->batches_.push_back(arrow::RecordBatch::Make(
                self->schema_, 0, std::vector<std::shared_ptr<arrow::Array>>(
                    self->schema_->num_fields(),
                    arrow::MakeArrayOfNull(arrow::utf8(), 0).ValueOrDie())));
        }

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema()    const override { return schema_; }
    int64_t total_rows()                        const override { return total_rows_; }
    int     num_chunks()                        const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i)                 const override {
        return {batch_first_row_[i], batches_[i]->num_rows()};
    }
    const std::string& path()                   const override { return path_; }
    std::string footer()                        const override {
        return std::string("Format: ") + (is_feather_ ? "Feather v2" : "Arrow IPC");
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
};

// ── Format detection + source factory ────────────────────────────────────────
// (fends is defined above, near the preamble helpers)

// Open any supported file.  Returns empty string on success; error message otherwise.
static std::string open_source(const std::string& path, const Config& cfg,
                                std::unique_ptr<TabularSource>* out) {
    // ── Determine file kind ──────────────────────────────────────────────────
    bool        is_parquet = false;
    DelimKind   dk         = DelimKind::TSV;

    if (fends(path, ".parquet")) {
        is_parquet = true;
    } else if (fends(path, ".arrow")) {
        std::unique_ptr<IpcSource> src;
        std::string err = IpcSource::open(path, false, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends(path, ".feather")) {
        std::unique_ptr<IpcSource> src;
        std::string err = IpcSource::open(path, true, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends(path, ".bam") || fends(path, ".cram")) {
        std::unique_ptr<BamSource> src;
        std::string err = BamSource::open(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends(path, ".vcf")   || fends(path, ".vcf.gz")) {
        dk = DelimKind::VCF;
    } else if (fends(path, ".gff")   || fends(path, ".gff.gz")  ||
               fends(path, ".gff3")  || fends(path, ".gff3.gz") ||
               fends(path, ".gtf")   || fends(path, ".gtf.gz")) {
        dk = DelimKind::GFF;
    } else if (fends(path, ".sam")) {
        dk = DelimKind::SAM;
    } else if (fends(path, ".bed")   || fends(path, ".bed.gz")) {
        dk = DelimKind::BED;
    } else if (fends(path, ".tsv")   || fends(path, ".tsv.gz")) {
        dk = DelimKind::TSV;
    } else if (fends(path, ".csv")   || fends(path, ".csv.gz")) {
        dk = DelimKind::CSV;
    } else {
        // Unknown extension: sniff magic bytes.
        // Read 8 bytes: enough for Parquet (PAR1), Arrow IPC (ARROW1\0\0),
        // and Feather v1 (FEA1).
        auto rf = arrow::io::ReadableFile::Open(path);
        if (!rf.ok()) return "Cannot open '" + path + "': " + rf.status().ToString();
        auto buf = rf.ValueOrDie()->Read(8);
        (void)rf.ValueOrDie()->Close();
        bool is_ipc = false, is_feather = false;
        if (buf.ok() && (*buf)->size() >= 4) {
            const uint8_t* m = (*buf)->data();
            is_parquet = ((*buf)->size() >= 4 &&
                          m[0]=='P' && m[1]=='A' && m[2]=='R' && m[3]=='1');
            is_ipc     = ((*buf)->size() >= 8 &&
                          m[0]=='A' && m[1]=='R' && m[2]=='R' && m[3]=='O' &&
                          m[4]=='W' && m[5]=='1' && m[6]==0   && m[7]==0);
            is_feather = ((*buf)->size() >= 4 &&
                          m[0]=='F' && m[1]=='E' && m[2]=='A' && m[3]=='1');
        }
        if (is_ipc || is_feather) {
            std::unique_ptr<IpcSource> src;
            std::string err = IpcSource::open(path, is_feather, &src);
            if (!err.empty()) return err;
            *out = std::move(src);
            return "";
        }
        if (!is_parquet) {
            // Count tabs vs commas in first line to choose delimiter.
            auto rf2 = arrow::io::ReadableFile::Open(path);
            if (!rf2.ok()) return "Cannot open '" + path + "': " + rf2.status().ToString();
            auto fh = rf2.ValueOrDie();
            std::string line;
            std::string first_line;
            auto lb = fh->Read(4096);
            (void)fh->Close();
            if (lb.ok()) {
                const char* d = (const char*)(*lb)->data();
                int len = (int)(*lb)->size();
                for (int i = 0; i < len; ++i) { if (d[i]=='\n') break; first_line += d[i]; }
            }
            int tabs   = (int)std::count(first_line.begin(), first_line.end(), '\t');
            int commas = (int)std::count(first_line.begin(), first_line.end(), ',');
            dk = (tabs >= commas) ? DelimKind::TSV : DelimKind::CSV;
        }
    }

    // ── Open appropriate source ───────────────────────────────────────────────
    if (is_parquet) {
        std::unique_ptr<ParquetSource> src;
        std::string err = ParquetSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    }

    std::unique_ptr<DelimitedSource> src;
    std::string err = DelimitedSource::open(path, dk, &src);
    if (!err.empty()) return err;
    *out = std::move(src);
    return "";
}

// ── Delimited output ──────────────────────────────────────────────────────────
// (write_csv_field is defined above)

static void write_delimited(TabularSource& src, const Config& cfg) {
    char sep = cfg.delimiter;
    int  show_cols = (cfg.max_cols > 0)
                     ? std::min(cfg.max_cols, src.schema()->num_fields())
                     : src.schema()->num_fields();
    // In delimiter mode default to all rows; honour -n if explicitly given.
    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;

    std::vector<int> col_indices;
    for (int i = 0; i < show_cols; ++i) col_indices.push_back(i);

    if (!cfg.no_header) {
        for (int ci = 0; ci < show_cols; ++ci) {
            if (ci) std::putchar(sep);
            write_csv_field(src.schema()->field(ci)->name(), sep);
        }
        std::putchar('\n');
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

        int64_t rg_rows = std::min(table->num_rows(), rows_left);
        rows_left -= rg_rows;

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

        std::vector<ChunkCursor> cursors;
        cursors.reserve(show_cols);
        for (int ci = 0; ci < show_cols; ++ci)
            cursors.push_back({table->column(ci).get()});

        for (int64_t r = 0; r < rg_rows; ++r) {
            for (int ci = 0; ci < show_cols; ++ci) {
                if (ci) std::putchar(sep);
                auto& cur = cursors[ci];
                std::string val = cell_to_string(cur.current(), cur.row_in_chunk);
                if (val != NULL_SYMBOL) write_csv_field(val, sep);
                cur.advance();
            }
            std::putchar('\n');
        }
    }
}

// ── Interactive TUI viewer (ncurses) ─────────────────────────────────────────

// ncurses color-pair IDs  (0 = terminal default)
enum : int {
    NCP_HEADER = 1,   // column header row
    NCP_INDEX,        // row-index column  (also A_DIM)
    NCP_NULL,         // null value        (also A_DIM)
    NCP_NUMBER,       // numeric / temporal value
    NCP_BOOL_T,       // true
    NCP_BOOL_F,       // false
    NCP_SEP,          // separator line    (also A_DIM)
    NCP_SEARCH,       // search-match highlight row
};

static void nc_str(int y, int x, const std::string& s,
                   attr_t attrs = A_NORMAL, int cp = 0) {
    attr_t full = attrs | (cp ? (attr_t)COLOR_PAIR(cp) : 0);
    if (full != A_NORMAL) attron(full);
    mvaddstr(y, x, s.c_str());
    if (full != A_NORMAL) attroff(full);
}

struct CachedRG {
    int64_t                               first_row = 0, num_rows = 0;
    std::vector<std::vector<std::string>> cells;  // [local_row][col]
};

class TableTUI {
    TabularSource& src_;
    int   num_cols_;
    int   max_col_w_;
    bool  no_index_;

    std::vector<std::string> col_names_;
    std::vector<int>         col_widths_;
    std::vector<bool>        right_align_;
    std::vector<bool>        is_bool_;
    std::vector<bool>        is_rgb_;
    int                      idx_w_ = 1;

    // Dynamic color-pair allocation for RGB cells (pairs NCP_SEARCH+1 … COLOR_PAIRS-1).
    std::unordered_map<int,int> rgb_pair_;   // packed 0xRRGGBB → ncurses pair number
    int                         next_rgb_pair_ = NCP_SEARCH + 1;

    int get_rgb_pair(int r, int g, int b) {
        if (COLORS < 256 || next_rgb_pair_ >= COLOR_PAIRS) return 0;
        int key = (r << 16) | (g << 8) | b;
        auto it = rgb_pair_.find(key);
        if (it != rgb_pair_.end()) return it->second;
        int pair = next_rgb_pair_++;
        init_pair(pair, -1, nearest_256(r, g, b));
        rgb_pair_[key] = pair;
        return pair;
    }

    std::map<int, CachedRG> cache_;
    std::list<int>          lru_;
    static constexpr int    MAX_CACHE = 4;

    int64_t top_row_  = 0;
    int     left_col_ = 0;
    int     scr_r_ = 24, scr_c_ = 80;

    // ── Search state ─────────────────────────────────────────────────────────
    enum class SearchMode { None, Input, Active };
    SearchMode  search_mode_  = SearchMode::None;
    std::string search_input_;   // text being typed in the search bar
    std::string search_query_;   // committed query (empty = no active search)
    int64_t     search_row_   = -1;   // row highlighted by last match (-1 = none)
    bool        search_wrap_  = false;  // last search wrapped around
    bool        search_fail_  = false;  // last search found nothing

    static constexpr int HDR_H = 2;
    static constexpr int FTR_H = 1;
    int data_lines() const { return std::max(0, scr_r_ - HDR_H - FTR_H); }

    int64_t total_rows() const { return src_.total_rows(); }
    int     num_chunks()  const { return src_.num_chunks(); }

    // ── Search ───────────────────────────────────────────────────────────────

    // Search forward (forward=true) or backward through all loaded chunks.
    // Returns absolute row index of first match >= from_row (forward) or
    // <= from_row (backward), or -1 if not found.
    // Shows "Searching…" in the status line while scanning large files.
    int64_t find_next(int64_t from_row, bool forward) {
        if (search_query_.empty()) return -1;
        std::string q = search_query_;
        for (auto& c : q) c = (char)std::tolower((unsigned char)c);

        std::vector<int> all_cols;
        for (int i = 0; i < num_cols_; ++i) all_cols.push_back(i);

        int nc = src_.num_chunks();

        // Check one row: returns true if any column contains q (case-insensitive).
        auto row_matches = [&](const std::shared_ptr<arrow::Table>& tbl, int64_t local) -> bool {
            for (int col = 0; col < num_cols_ && col < tbl->num_columns(); ++col) {
                int64_t off = local;
                for (auto& arr : tbl->column(col)->chunks()) {
                    if (off < arr->length()) {
                        std::string val = cell_to_string(*arr, off);
                        for (auto& c : val) c = (char)std::tolower((unsigned char)c);
                        if (val.find(q) != std::string::npos) return true;
                        break;
                    }
                    off -= arr->length();
                }
            }
            return false;
        };

        if (forward) {
            for (int c = 0; c < nc; ++c) {
                auto meta = src_.chunk_meta(c);
                if (meta.first_row + meta.num_rows <= from_row) continue;
                // Show progress for slow sources
                mvprintw(scr_r_-1, 0, " Searching… chunk %d/%d ", c+1, nc);
                clrtoeol(); refresh();
                std::shared_ptr<arrow::Table> tbl;
                if (!src_.read_chunk(c, all_cols, &tbl).ok()) continue;
                int64_t start = std::max<int64_t>(0, from_row - meta.first_row);
                for (int64_t r = start; r < tbl->num_rows(); ++r)
                    if (row_matches(tbl, r)) return meta.first_row + r;
            }
        } else {
            for (int c = nc - 1; c >= 0; --c) {
                auto meta = src_.chunk_meta(c);
                if (meta.first_row > from_row) continue;
                mvprintw(scr_r_-1, 0, " Searching… chunk %d/%d ", c+1, nc);
                clrtoeol(); refresh();
                std::shared_ptr<arrow::Table> tbl;
                if (!src_.read_chunk(c, all_cols, &tbl).ok()) continue;
                int64_t end = std::min(tbl->num_rows() - 1, from_row - meta.first_row);
                for (int64_t r = end; r >= 0; --r)
                    if (row_matches(tbl, r)) return meta.first_row + r;
            }
        }
        return -1;
    }

    // Commit a search: find the first match, update state, scroll to it.
    void do_search(bool forward) {
        if (search_query_.empty()) return;
        int64_t start;
        if (search_row_ >= 0) {
            start = forward ? search_row_ + 1 : search_row_ - 1;
        } else {
            start = forward ? top_row_ : top_row_ + data_lines() - 1;
        }
        int64_t found = find_next(start, forward);
        // Wrap around if not found
        if (found < 0) {
            int64_t wrap_start = forward ? 0 : (total_rows() >= 0 ? total_rows()-1 : src_.chunk_meta(num_chunks()-1).first_row + src_.chunk_meta(num_chunks()-1).num_rows - 1);
            found = find_next(wrap_start, forward);
            search_wrap_ = (found >= 0);
        } else {
            search_wrap_ = false;
        }
        if (found >= 0) {
            search_row_  = found;
            search_fail_ = false;
            // Scroll so the match is visible
            int dl = data_lines();
            if (found < top_row_ || found >= top_row_ + dl) {
                int64_t tr = total_rows();
                int64_t mt = (tr >= 0) ? std::max<int64_t>(0, tr - dl) : found;
                top_row_ = std::min(found, mt);
            }
        } else {
            search_fail_ = true;
        }
    }

    // ── Cache ────────────────────────────────────────────────────────────────

    void load_chunk(int c) {
        if (cache_.count(c)) { lru_.remove(c); lru_.push_front(c); return; }
        if ((int)cache_.size() >= MAX_CACHE) {
            cache_.erase(lru_.back()); lru_.pop_back();
        }
        std::vector<int> cols;
        for (int i = 0; i < num_cols_; ++i) cols.push_back(i);

        std::shared_ptr<arrow::Table> tbl;
        src_.ensure(c);
        if (!src_.read_chunk(c, cols, &tbl).ok()) {
            auto m = src_.chunk_meta(c);
            cache_[c] = CachedRG{m.first_row, m.num_rows, {}};
            lru_.push_front(c);
            return;
        }
        CachedRG& cr  = cache_[c];
        cr.first_row  = src_.chunk_meta(c).first_row;
        cr.num_rows   = tbl->num_rows();
        cr.cells.assign((size_t)cr.num_rows, std::vector<std::string>(num_cols_));
        for (int ci = 0; ci < num_cols_; ++ci) {
            int64_t r = 0;
            for (auto& chunk : tbl->column(ci)->chunks())
                for (int64_t i = 0; i < chunk->length(); ++i, ++r)
                    cr.cells[r][ci] = truncate(
                        src_.format_cell(ci, cell_to_string(*chunk, i)), max_col_w_);
        }
        lru_.push_front(c);
    }

    int chunk_for_row(int64_t r) const {
        // Binary search in known chunks
        int lo = 0, hi = src_.num_chunks() - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (src_.chunk_meta(mid).first_row <= r) lo = mid; else hi = mid - 1;
        }
        return lo;
    }

    void prefetch() {
        if (src_.num_chunks() == 0) { src_.ensure(0); return; }
        int c = chunk_for_row(top_row_);
        load_chunk(c);
        int64_t bot = top_row_ + (int64_t)data_lines() - 1;
        if (total_rows() > 0) bot = std::min(bot, total_rows() - 1);
        // While streaming (total unknown), trigger loading of the next unread
        // chunk so the view advances as the user scrolls down.  When total is
        // known, seek by row as before.
        if (total_rows() < 0)
            src_.ensure(src_.num_chunks());
        else
            src_.ensure(chunk_for_row(std::max(bot, top_row_)));
        if (bot > top_row_) load_chunk(chunk_for_row(bot));
    }

    // ── Layout ───────────────────────────────────────────────────────────────

    struct ColVis { int col, x, w; };

    std::vector<ColVis> visible_cols() const {
        std::vector<ColVis> v;
        int x = no_index_ ? 0 : (idx_w_ + 2);
        for (int c = left_col_; c < num_cols_; ++c) {
            int w = col_widths_[c];
            if (x + w + 2 > scr_c_) break;
            v.push_back({c, x, w});
            x += w + 2;
        }
        return v;
    }

    static std::string fit(const std::string& val, int w, bool ra) {
        int dw = display_width(val);
        if (ra) {
            if (dw > w) return val.substr(dw - w);
            return std::string(w - dw, ' ') + val;
        }
        if (dw > w) {
            bool has_ell = val.size() >= 3 && val.compare(val.size()-3, 3, ELLIPSIS) == 0;
            std::string base = has_ell ? val.substr(0, val.size()-3) : val;
            if ((int)base.size() > w - 1) base.resize(w - 1);
            return base + ELLIPSIS;
        }
        return val + std::string(w - dw, ' ');
    }

    // ── Drawing ──────────────────────────────────────────────────────────────

    void draw_header(const std::vector<ColVis>& vc) {
        if (!no_index_) {
            nc_str(0, 0, " " + std::string(idx_w_, ' ') + " ", A_BOLD, NCP_INDEX);
            nc_str(1, 0, " " + std::string(idx_w_, '-') + " ", A_DIM,  NCP_SEP);
        }
        for (auto& col : vc) {
            std::string nm = truncate(col_names_[col.col], col.w);
            nc_str(0, col.x, " " + fit(nm, col.w, right_align_[col.col]) + " ",
                   A_BOLD, NCP_HEADER);
            nc_str(1, col.x, " " + std::string(col.w, '-') + " ", A_DIM, NCP_SEP);
        }
    }

    void draw_data_row(int sy, int64_t row, const std::vector<ColVis>& vc) {
        int64_t tr = total_rows();
        if (tr >= 0 && row >= tr) return;

        bool is_match = (row == search_row_);
        attr_t row_attr = is_match ? (attr_t)COLOR_PAIR(NCP_SEARCH) : A_NORMAL;

        if (!no_index_) {
            std::string idx_s = " " + fit(std::to_string(row), idx_w_, true) + " ";
            if (is_match) nc_str(sy, 0, idx_s, A_BOLD, NCP_SEARCH);
            else          nc_str(sy, 0, idx_s, A_DIM, NCP_INDEX);
        }

        if (src_.num_chunks() == 0) return;
        int  c  = chunk_for_row(row);
        auto it = cache_.find(c);
        if (it == cache_.end() || it->second.cells.empty()) return;

        int64_t local = row - it->second.first_row;
        // Guard: row may be beyond the loaded portion of the last chunk
        // (happens while streaming and the user scrolled ahead of loaded data).
        if (local < 0 || local >= (int64_t)it->second.cells.size()) return;
        for (auto& col : vc) {
            const std::string& val = it->second.cells[local][col.col];

            if (is_match) {
                // Whole row rendered with NCP_SEARCH highlight
                nc_str(sy, col.x, " " + fit(val, col.w, right_align_[col.col]) + " ",
                       A_BOLD, NCP_SEARCH);
                continue;
            }

            if (is_rgb_[col.col]) {
                int r = 0, gv = 0, bv = 0;
                int pair = (val != NULL_SYMBOL && parse_rgb(val, &r, &gv, &bv))
                           ? get_rgb_pair(r, gv, bv) : 0;
                if (pair > 0)
                    nc_str(sy, col.x, " " + std::string(col.w, ' ') + " ", A_NORMAL, pair);
                else
                    nc_str(sy, col.x, " " + fit(val, col.w, false) + " ",
                           val == NULL_SYMBOL ? A_DIM : A_NORMAL,
                           val == NULL_SYMBOL ? NCP_NULL : 0);
                continue;
            }

            attr_t extra = A_NORMAL; int cp = 0;
            if (val == NULL_SYMBOL)         { extra = A_DIM; cp = NCP_NULL; }
            else if (is_bool_[col.col])     { cp = (val=="true")?NCP_BOOL_T:NCP_BOOL_F; }
            else if (right_align_[col.col]) { cp = NCP_NUMBER; }
            nc_str(sy, col.x, " " + fit(val, col.w, right_align_[col.col]) + " ",
                   extra, cp);
        }
    }

    void draw_status(const std::vector<ColVis>& vc) {
        // ── Search-input mode: show a vim-style search bar ───────────────────
        if (search_mode_ == SearchMode::Input) {
            std::string bar = "/" + search_input_;
            if ((int)bar.size() < scr_c_) bar += std::string(scr_c_ - (int)bar.size(), ' ');
            mvaddnstr(scr_r_ - 1, 0, bar.c_str(), scr_c_);
            // Position the cursor after the typed text
            curs_set(1);
            move(scr_r_ - 1, (int)search_input_.size() + 1);
            return;
        }
        curs_set(0);

        // ── Normal status bar ────────────────────────────────────────────────
        int64_t tr  = total_rows();
        int64_t bot = top_row_ + (int64_t)data_lines();
        if (tr >= 0) bot = std::min(bot, tr);

        std::string s = " Row ";
        s += std::to_string(top_row_ + 1) + "-" + std::to_string(bot) + "/";
        s += (tr >= 0) ? std::to_string(tr) : "?";

        if (!vc.empty()) {
            s += "  Col ";
            s += std::to_string(vc.front().col+1) + "-";
            s += std::to_string(vc.back().col+1)  + "/";
            s += std::to_string(num_cols_);
        }
        // Show search state
        if (search_mode_ == SearchMode::Active && !search_query_.empty()) {
            s += "  /" + search_query_;
            if (search_fail_)         s += " (not found)";
            else if (search_wrap_)    s += " (wrapped)";
            s += "  [n/N]:next/prev  [Esc]:clear";
        } else {
            bool need_lr = left_col_ > 0 || (!vc.empty() && vc.back().col < num_cols_-1);
            if (need_lr) s += "  [</>/h/l]:cols";
            s += "  [^/v/j/k]:rows  PgUp/Dn  g/G:top/bot  /:search  q:quit";
        }
        if ((int)s.size() < scr_c_) s += std::string(scr_c_ - (int)s.size(), ' ');
        attron(A_REVERSE);
        mvaddnstr(scr_r_ - 1, 0, s.c_str(), scr_c_);
        attroff(A_REVERSE);
    }

    void draw() {
        getmaxyx(stdscr, scr_r_, scr_c_);
        erase();
        // Once total is known, clamp top_row_ so the last page stays filled.
        // This handles the case where the user scrolled past EOF while streaming.
        {
            int64_t tr = total_rows();
            int dl2 = data_lines();
            if (tr >= 0) {
                int64_t mt = std::max<int64_t>(0, tr - dl2);
                if (top_row_ > mt) top_row_ = mt;
            }
        }
        auto vc = visible_cols();
        draw_header(vc);
        int dl = data_lines();
        for (int y = 0; y < dl; ++y)
            draw_data_row(HDR_H + y, top_row_ + y, vc);
        draw_status(vc);
        refresh();
    }

    void setup_colors() {
        if (!has_colors()) return;
        start_color(); use_default_colors();
        init_pair(NCP_HEADER,  COLOR_WHITE,  -1);
        init_pair(NCP_INDEX,   COLOR_WHITE,  -1);
        init_pair(NCP_NULL,    COLOR_WHITE,  -1);
        init_pair(NCP_NUMBER,  COLOR_CYAN,   -1);
        init_pair(NCP_BOOL_T,  COLOR_GREEN,  -1);
        init_pair(NCP_BOOL_F,  COLOR_YELLOW, -1);
        init_pair(NCP_SEP,     COLOR_WHITE,  -1);
        init_pair(NCP_SEARCH,  COLOR_BLACK,  COLOR_YELLOW);
    }

public:
    TableTUI(TabularSource& src, const Config& cfg)
        : src_(src),
          num_cols_((cfg.max_cols > 0)
                    ? std::min(cfg.max_cols, src.schema()->num_fields())
                    : src.schema()->num_fields()),
          max_col_w_(cfg.max_col_w),
          no_index_(cfg.no_index)
    {
        // Compute index column width from total rows (or a guess if unknown).
        int64_t tr = src.total_rows();
        int64_t tr_for_width = (tr >= 0) ? tr : 999999;
        for (int64_t v = std::max<int64_t>(tr_for_width-1, 0); v >= 10; v /= 10) ++idx_w_;

        col_names_.resize(num_cols_);
        col_widths_.resize(num_cols_);
        right_align_.resize(num_cols_);
        is_bool_.resize(num_cols_);
        is_rgb_.resize(num_cols_);
        for (int ci = 0; ci < num_cols_; ++ci) {
            auto f = src.schema()->field(ci);
            col_names_[ci]   = f->name();
            col_widths_[ci]  = std::min(
                std::max((int)display_width(f->name()), src.min_col_width(ci)), max_col_w_);
            right_align_[ci] = is_numeric_type(f->type()->id());
            is_bool_[ci]     = (display_type(*f) == arrow::Type::BOOL);
            is_rgb_[ci]      = (f->name() == "RGB");
        }
    }

    void run() {
        setlocale(LC_ALL, "");  // enable UTF-8 rendering in ncurses
        initscr(); noecho(); cbreak(); keypad(stdscr, TRUE); curs_set(0);
        set_escdelay(25); setup_colors();

        prefetch();

        bool quit = false;
        while (!quit) {
            draw();
            int ch = getch();
            int dl = data_lines();

            // ── Search input mode ────────────────────────────────────────────
            if (search_mode_ == SearchMode::Input) {
                if (ch == '\n' || ch == KEY_ENTER) {
                    if (!search_input_.empty()) {
                        search_query_ = search_input_;
                        search_mode_  = SearchMode::Active;
                        search_row_   = -1;
                        do_search(true);
                    } else {
                        search_mode_  = SearchMode::None;
                        search_query_.clear();
                        search_row_   = -1;
                    }
                } else if (ch == 27) {    // Esc — cancel
                    search_mode_  = SearchMode::None;
                    search_input_.clear();
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                    if (!search_input_.empty()) search_input_.pop_back();
                } else if (ch >= 32 && ch < 127) {
                    search_input_ += (char)ch;
                }
                prefetch();
                continue;
            }

            // ── Navigation ───────────────────────────────────────────────────
            int64_t tr = total_rows();
            int64_t max_top = (tr >= 0) ? std::max<int64_t>(0, tr - dl) : top_row_ + dl;

            switch (ch) {
                case 'q': case 'Q': quit = true; break;
                case 27:  // Esc: clear search if active, else quit
                    if (search_mode_ == SearchMode::Active) {
                        search_mode_  = SearchMode::None;
                        search_query_.clear();
                        search_row_   = -1;
                        search_fail_  = false;
                    } else {
                        quit = true;
                    }
                    break;
                case KEY_DOWN: case 'j':
                    if (tr < 0 || top_row_ + dl < tr) ++top_row_; break;
                case KEY_UP: case 'k':
                    if (top_row_ > 0) --top_row_; break;
                case KEY_NPAGE: case ' ':
                    top_row_ = std::min(top_row_ + dl, max_top); break;
                case KEY_PPAGE: case 'b':
                    top_row_ = std::max<int64_t>(0, top_row_ - dl); break;
                case 'g': case KEY_HOME: top_row_ = 0; break;
                case 'G': case KEY_END:
                    // For streaming sources we must read to EOF before we know
                    // the last row.  Show a status message and drain the stream.
                    if (tr < 0) {
                        mvaddstr(scr_r_ - 1, 0, " Loading to end of file… ");
                        refresh();
                        while (src_.total_rows() < 0)
                            src_.ensure(src_.num_chunks());
                        tr = total_rows();
                    }
                    top_row_ = std::max<int64_t>(0, tr - dl);
                    break;
                case KEY_RIGHT: case 'l':
                    if (left_col_ + 1 < num_cols_) ++left_col_; break;
                case KEY_LEFT: case 'h':
                    if (left_col_ > 0) --left_col_; break;
                case '/':
                    search_mode_  = SearchMode::Input;
                    search_input_.clear();
                    break;
                case 'n':
                    if (!search_query_.empty()) do_search(true);
                    break;
                case 'N':
                    if (!search_query_.empty()) do_search(false);
                    break;
                case KEY_RESIZE: break;
                default: break;
            }
            prefetch();
        }
        endwin();
    }
};

// ── Table display (non-interactive) ──────────────────────────────────────────

static void print_table(TabularSource& src, const Config& cfg) {
    auto schema = src.schema();
    int show_cols = (cfg.max_cols > 0)
                    ? std::min(cfg.max_cols, schema->num_fields())
                    : schema->num_fields();
    int64_t tr          = src.total_rows();
    int64_t rows_wanted = (cfg.head_rows <= 0) ? (tr >= 0 ? tr : INT64_MAX)
                                               : (int64_t)cfg.head_rows;

    std::vector<int> col_indices;
    for (int i = 0; i < show_cols; ++i) col_indices.push_back(i);

    // Collect rows up to rows_wanted
    std::shared_ptr<arrow::Table> data;
    for (int c = 0; ; ++c) {
        src.ensure(c);
        if (c >= src.num_chunks()) break;

        std::shared_ptr<arrow::Table> chunk;
        if (!src.read_chunk(c, col_indices, &chunk).ok()) continue;

        if (!data) { data = chunk; }
        else {
            auto r = arrow::ConcatenateTables({data, chunk});
            if (r.ok()) data = r.ValueOrDie();
        }
        if (cfg.head_rows > 0 && data->num_rows() >= rows_wanted) break;
    }
    if (!data) return;
    if (cfg.head_rows > 0 && data->num_rows() > rows_wanted)
        data = data->Slice(0, rows_wanted);

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

    for (int ci = 0; ci < show_cols; ++ci) {
        auto field   = schema->field(ci);
        auto arr_col = data->column(ci);
        Column col;
        col.header      = field->name();
        col.right_align = is_numeric_type(field->type()->id());
        col.is_bool     = (display_type(*field) == arrow::Type::BOOL);
        col.is_rgb      = (field->name() == "RGB");
        col.width       = std::max(display_width(col.header), src.min_col_width(ci));
        for (auto& chunk : arr_col->chunks())
            for (int64_t r = 0; r < chunk->length(); ++r) {
                std::string val = truncate(
                    src.format_cell(ci, cell_to_string(*chunk, r)), cfg.max_col_w);
                if (display_width(val) > col.width) col.width = display_width(val);
                col.cells.push_back(std::move(val));
            }
        col.width  = std::min(col.width, cfg.max_col_w);
        col.header = truncate(col.header, cfg.max_col_w);
        col.width  = std::max(col.width, display_width(col.header));
        columns.push_back(std::move(col));
    }

    // BED track/browser lines shown above the table
    for (auto& line : src.preamble_above())
        std::printf("%s%s%s\n", g_color.meta_key, line.c_str(), g_color.reset);

    draw_separator(columns);
    { std::vector<std::string> hdr; std::vector<bool> ra;
      for (auto& c : columns) { hdr.push_back(c.header); ra.push_back(false); }
      draw_row(columns, hdr, ra, true); }
    draw_separator(columns);
    for (int64_t r = 0; r < n_display; ++r) {
        std::vector<std::string> row; std::vector<bool> ra;
        for (auto& c : columns) { row.push_back(c.cells[r]); ra.push_back(c.right_align); }
        draw_row(columns, row, ra);
    }
    draw_separator(columns);

    if (show_cols < num_cols)
        std::printf("  ... %d more column(s) not shown (-c 0 to see all)\n",
                    num_cols - show_cols);

    // Summary
    int64_t total = (tr >= 0) ? tr : n_display;
    std::printf("\n%s[%lld rows x %d columns]%s\n",
                g_color.meta_key, (long long)total, num_cols, g_color.reset);

    // Schema table
    int name_w = 6, type_w = 4;
    for (int ci = 0; ci < num_cols; ++ci) {
        auto f = schema->field(ci);
        name_w = std::max(name_w, (int)f->name().size());
        type_w = std::max(type_w, (int)f->type()->ToString().size());
    }
    name_w = std::min(name_w, 40); type_w = std::min(type_w, 40);

    std::printf("\n%s%-*s  %-*s  Nullable%s\n",
                g_color.header, name_w, "Column", type_w, "Type", g_color.reset);
    std::printf("%s%s  %s  --------%s\n", g_color.border,
                std::string(name_w,'-').c_str(), std::string(type_w,'-').c_str(), g_color.reset);
    for (int ci = 0; ci < num_cols; ++ci) {
        auto f = schema->field(ci);
        std::string fname = truncate(f->name(), name_w);
        std::string ftype = truncate(f->type()->ToString(), type_w);
        const char* tc = *g_color.reset ? type_color(display_type(*f)) : "";
        std::printf("%-*s  %s%-*s%s  %s\n",
                    name_w, fname.c_str(),
                    tc, type_w, ftype.c_str(), g_color.reset,
                    f->nullable() ? "yes" : "no");
    }

    // File info footer
    std::printf("\n%sFile:%s %s\n", g_color.meta_key, g_color.reset, src.path().c_str());
    std::printf("%s%s%s\n", g_color.meta_key, src.footer().c_str(), g_color.reset);
    if (!src.created_by().empty())
        std::printf("%sCreated by:%s %s\n", g_color.meta_key, g_color.reset,
                    src.created_by().c_str());
    // VCF/BAM/SAM/GFF meta header lines shown below the schema
    for (auto& line : src.preamble_below())
        std::printf("%s%s%s\n", g_color.meta_key, line.c_str(), g_color.reset);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    bool use_color = (cfg.color == ColorMode::Always) ||
                     (cfg.color == ColorMode::Auto && isatty(STDOUT_FILENO));
    if (use_color) init_colors();

    std::unique_ptr<TabularSource> src;
    std::string err = open_source(cfg.path, cfg, &src);
    if (!err.empty()) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }

    // Interactive viewer
    {
        bool auto_tui = !cfg.no_interactive && !cfg.delimiter && !cfg.head_rows_set
                        && isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
        if (cfg.interactive || auto_tui) {
            TableTUI tui(*src, cfg);
            tui.run();
            return 0;
        }
    }

    // Delimited output
    if (cfg.delimiter) {
        Config dcfg = cfg;
        if (!dcfg.head_rows_set) dcfg.head_rows = 0;
        write_delimited(*src, dcfg);
        return 0;
    }

    // Table display
    print_table(*src, cfg);
    return 0;
}
