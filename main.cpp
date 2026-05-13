// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// vv -- universal genomic file viewer
// https://github.com/balwierz/vv

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/statistics.h>
#include <arrow/type.h>
#include <arrow/scalar.h>
#include <arrow/csv/api.h>
#include <arrow/io/compressed.h>
#include <arrow/ipc/feather.h>
#include <arrow/ipc/reader.h>
#include <arrow/util/compression.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>

#include <htslib/sam.h>
#include <htslib/vcf.h>
#include <htslib/tbx.h>
#include <htslib/kseq.h>
#include <htslib/bgzf.h>

// libBigWig (vendored under vendored/libBigWig/, compiled with -DNOCURL).
// Wrapped in an extern "C" because it's a C library; ARROW headers above
// already bring in C++ machinery.
extern "C" {
#include <bigWig.h>
}

#include <algorithm>
#include <array>
#include <charconv>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <sys/ioctl.h>
#include <cerrno>
#include <sys/stat.h>

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
    std::string region;                  // -r: tabix region(s), e.g. "chr1:1000-2000"
    std::string regions_file;            // --regions-file BED: many windows from a BED
    std::string region_cols;             // --region-cols Chr,Start,End for generic Parquet
    int64_t     slop = 0;                // --slop N: pad each region by N bp
    std::string parquet_out;             // --parquet <file>: write Parquet to this path
    std::string compression = "zstd";    // --compression: parquet codec
    int         head_rows      = 10;
    bool        head_rows_set  = false;  // true when -n was given explicitly
    int         max_col_w      = 32;
    int         max_cols       = 0;
    int         threads        = 0;      // -@ / --threads (0 = auto-detect)
    bool        no_index       = false;
    ColorMode   color          = ColorMode::Auto;
    char        delimiter      = 0;      // 0 = table/interactive; '\t'/','= delimited
    bool        no_header      = false;
    bool        interactive    = false;  // -i / --interactive
    bool        no_interactive = false;  // --no-interactive
    bool        vertical       = false;  // --vertical (or invoked as `vh`)
    bool        schema_only    = false;  // --schema: print schema + footer, exit
    bool        describe       = false;  // --describe: per-column statistics
    bool        stats_only     = false;  // --stats: Parquet metadata dump (no data read)
    std::string unique_cols;             // --unique COL[,COL,...] : distinct value counts
    int         sample_n       = 0;      // --sample N (reservoir sample of N rows)
    std::string filter_expr;             // --filter "<col> <op> <literal> ..."
    std::string select_cols;             // --select Chr,Start,End (name-based projection)
    bool        json_array     = false;  // --json
    bool        json_lines     = false;  // --ndjson
    bool        md             = false;  // --md (GitHub-flavored markdown table)
    bool        validate       = false;  // --validate (LociSSD invariants check)
    bool        coords_one_based = false; // --coords 1-based (tabix-style)
    int         tail_rows      = 0;      // --tail N
    bool        tail_rows_set  = false;
};

// Effective worker-thread count: explicit override or
// min(8, max(2, hardware_concurrency()/2)) so we don't oversubscribe big boxes.
static int effective_threads(const Config& cfg) {
    if (cfg.threads > 0) return cfg.threads;
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 2;
    unsigned t = hc / 2;
    if (t < 2) t = 2;
    if (t > 8) t = 8;
    return (int)t;
}

static constexpr const char* kVersion = "1.5.0";

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "vv -- universal genomic file viewer\n"
        "\nUsage: %s [options] <file>\n"
        "\nSupported formats:\n"
        "  .parquet\n"
        "  .lociss                     LociSSD sorted-interval Parquet (manifest in KV)\n"
        "  .bam  .cram                  binary/compressed sequence alignments (htslib)\n"
        "  .sam                        text sequence alignments\n"
        "  .vcf  .vcf.gz               variant calls\n"
        "  .gff  .gff3  .gtf           and .gz variants  genome annotations\n"
        "  .bed  .tsv  .csv            and .gz variants\n"
        "  .bb  .bigBed                UCSC bigBed (libBigWig; autoSql columns)\n"
        "  .bw  .bigWig                UCSC bigWig (libBigWig)\n"
        "  .fa  .fasta  .fna  .faa     sequences (FASTA, plus .gz)\n"
        "  .fq  .fastq                 sequencing reads (FASTQ, plus .gz)\n"
        "  .bcf                        binary VCF (htslib)\n"
        "  .paf  .paf.gz               minimap2 pairwise alignments\n"
        "  -                           read text format from stdin (auto-gunzip)\n"
        "  (unknown extensions: sniffed by magic bytes / delimiter)\n"
        "\nInteractive viewer (default when stdout is a terminal):\n"
        "  -i / --interactive  open the ncurses row browser\n"
        "  --no-interactive    force plain table output even on a terminal\n"
        "  Keys: arrows/hjkl navigate, PgUp/PgDn, g/G top/bot, q quit\n"
        "\nTable options:\n"
        "  -n <rows>           rows to display  (default: 10, 0 = all)\n"
        "  --tail <N>          show the last N rows instead of the first N\n"
        "  -w <width>          max cell width   (default: 32)\n"
        "  -c <cols>           max columns to show (default: all)\n"
        "  --select <cols>     project columns by name (e.g. --select Chr,Start,End)\n"
        "  --filter <expr>     keep rows matching: <col> <op> <literal> joined by AND/OR\n"
        "                      ops: == != < <= > >=  e.g. --filter 'Score > 0.5'\n"
        "  --schema            print schema + file metadata and exit\n"
        "  --describe          per-column statistics and exit\n"
        "  --stats             print Parquet metadata footer (row groups, codecs,\n"
        "                      per-column sizes) without reading data; exit\n"
        "  --unique <cols>     comma-separated columns: print distinct-value counts\n"
        "  --sample <N>        reservoir-sample N rows uniformly instead of head-N\n"
        "  --validate          check LociSSD invariants (sort order, MaxEndSoFar,\n"
        "                      manifest vs. data); exit non-zero on failure\n"
        "  --no-index          suppress the row-index column\n"
        "  --color[=WHEN]      colorize output: auto (default), always, never\n"
        "  --vertical          \"vertical head\": transpose the preview so each\n"
        "                      field is a row; show as many records per line as\n"
        "                      fit. Implies --no-interactive. Default when the\n"
        "                      binary is invoked as `vh`.\n"
        "\nDelimited output (replaces table view):\n"
        "  --tsv               write tab-separated values to stdout\n"
        "  --csv               write comma-separated values to stdout\n"
        "  --json              write a JSON array of row objects to stdout\n"
        "  --ndjson            write one JSON object per line (JSON Lines)\n"
        "  --md / --markdown   write a GitHub-flavored markdown table\n"
        "  --delimiter <sep>   write with a custom single-character delimiter\n"
        "  --no-header         omit the header row\n"
        "  (-n defaults to all rows in this mode; -c still applies)\n"
        "\nParquet output (replaces table view):\n"
        "  --parquet <file>    write a Parquet file at <file> (or `-` for stdout)\n"
        "  --compression <c>   codec: zstd (default), snappy, gzip, lz4, none\n"
        "\nRange queries:\n"
        "  -r / --region <REGION>   e.g. chr1:1000-2000  (multiple comma-separated)\n"
        "  --window <REGION>        alias of -r for LociSSD readers' muscle memory\n"
        "  --regions-file <BED>     read additional windows from a BED file's\n"
        "                           first three columns\n"
        "  --region-cols <names>    chrom/start/end column names for plain\n"
        "                           Parquet (3 comma-separated names; default:\n"
        "                           auto-detect Chromosome/Chrom/Chr + Start/POS\n"
        "                           + End/Stop)\n"
        "  --slop <N>               pad each window by N bp on both sides\n"
        "  --coords <kind>          coordinate convention for -r: 0-based\n"
        "                           (default, BED) or 1-based (tabix / VCF)\n"
        "  Supported on tabix-indexed VCF/BED/GFF/TSV, indexed BCF\n"
        "  (.csi/.tbi), LociSSD Parquet (.lociss), plain sorted Parquet\n"
        "  with chrom/start/end columns, and bigBed/bigWig. Coordinates\n"
        "  are 0-based half-open (BED convention).\n"
        "\nPerformance:\n"
        "  -@ / --threads <N>  worker threads for I/O and decode (0 = auto)\n"
        "\n  -h / --help         show this help\n"
        "  -V / --version      print version and exit\n",
        prog);
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    // If invoked as `vh` (a symlink/copy of vv), default to vertical-head mode:
    // a "head"-style preview transposed so wide tables fit without horizontal
    // scrolling. Implies --no-interactive.
    if (argc > 0) {
        const char* base = std::strrchr(argv[0], '/');
        base = base ? base + 1 : argv[0];
        if (std::strcmp(base, "vh") == 0) {
            cfg.vertical       = true;
            cfg.no_interactive = true;
        }
    }
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            print_usage(argv[0]); std::exit(0);
        } else if (!std::strcmp(argv[i], "-V") || !std::strcmp(argv[i], "--version")) {
            std::printf("vv %s\n", kVersion); std::exit(0);
        } else if (!std::strcmp(argv[i], "--no-index")) {
            cfg.no_index = true;
        } else if (!std::strcmp(argv[i], "--no-header")) {
            cfg.no_header = true;
        } else if (!std::strcmp(argv[i], "-i") || !std::strcmp(argv[i], "--interactive")) {
            cfg.interactive = true;
        } else if (!std::strcmp(argv[i], "--no-interactive")) {
            cfg.no_interactive = true;
        } else if (!std::strcmp(argv[i], "--vertical")) {
            cfg.vertical       = true;
            cfg.no_interactive = true;
        } else if (!std::strcmp(argv[i], "-n") && i + 1 < argc) {
            cfg.head_rows     = std::atoi(argv[++i]);
            cfg.head_rows_set = true;
        } else if (!std::strcmp(argv[i], "-w") && i + 1 < argc) {
            cfg.max_col_w = std::max(4, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "-c") && i + 1 < argc) {
            cfg.max_cols = std::atoi(argv[++i]);
        } else if ((!std::strcmp(argv[i], "-r") ||
                    !std::strcmp(argv[i], "--region") ||
                    !std::strcmp(argv[i], "--window")) && i + 1 < argc) {
            cfg.region = argv[++i];
        } else if (!std::strcmp(argv[i], "--regions-file") && i + 1 < argc) {
            cfg.regions_file = argv[++i];
        } else if (!std::strcmp(argv[i], "--region-cols") && i + 1 < argc) {
            cfg.region_cols = argv[++i];
        } else if (!std::strcmp(argv[i], "--slop") && i + 1 < argc) {
            cfg.slop = (int64_t)std::atoll(argv[++i]);
        } else if (!std::strcmp(argv[i], "--coords") && i + 1 < argc) {
            const char* v = argv[++i];
            if (!std::strcmp(v, "1-based") || !std::strcmp(v, "1based") ||
                !std::strcmp(v, "tabix"))
                cfg.coords_one_based = true;
            else if (!std::strcmp(v, "0-based") || !std::strcmp(v, "0based") ||
                     !std::strcmp(v, "bed"))
                cfg.coords_one_based = false;
            else {
                std::fprintf(stderr, "--coords: expected '0-based' or '1-based', got %s\n", v);
                std::exit(2);
            }
        } else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) {
            cfg.tail_rows     = std::max(0, std::atoi(argv[++i]));
            cfg.tail_rows_set = true;
        } else if ((!std::strcmp(argv[i], "-@") ||
                    !std::strcmp(argv[i], "--threads")) && i + 1 < argc) {
            cfg.threads = std::max(0, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--parquet") && i + 1 < argc) {
            cfg.parquet_out = argv[++i];
        } else if (!std::strcmp(argv[i], "--compression") && i + 1 < argc) {
            cfg.compression = argv[++i];
        } else if (!std::strcmp(argv[i], "--schema")) {
            cfg.schema_only = true;
        } else if (!std::strcmp(argv[i], "--describe")) {
            cfg.describe = true;
        } else if (!std::strcmp(argv[i], "--stats")) {
            cfg.stats_only = true;
        } else if (!std::strcmp(argv[i], "--unique") && i + 1 < argc) {
            cfg.unique_cols = argv[++i];
        } else if (!std::strcmp(argv[i], "--sample") && i + 1 < argc) {
            cfg.sample_n = std::max(0, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) {
            cfg.filter_expr = argv[++i];
        } else if ((!std::strcmp(argv[i], "--select") ||
                    !std::strcmp(argv[i], "--cols")) && i + 1 < argc) {
            cfg.select_cols = argv[++i];
        } else if (!std::strcmp(argv[i], "--json")) {
            cfg.json_array = true;
        } else if (!std::strcmp(argv[i], "--ndjson")) {
            cfg.json_lines = true;
        } else if (!std::strcmp(argv[i], "--md") ||
                   !std::strcmp(argv[i], "--markdown")) {
            cfg.md = true;
        } else if (!std::strcmp(argv[i], "--validate")) {
            cfg.validate = true;
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
        } else if (argv[i][0] != '-' || (argv[i][0] == '-' && argv[i][1] == 0)) {
            // Bare "-" means stdin; treat as a positional argument.
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

// Box-drawing glyphs (each 3 UTF-8 bytes, 1 terminal column).
static constexpr const char BOX_HLINE[] = "\xe2\x94\x80";  // ─
static constexpr const char BOX_VLINE[] = "\xe2\x94\x82";  // │
static constexpr const char BOX_TL[]    = "\xe2\x95\xad";  // ╭ (rounded)
static constexpr const char BOX_TR[]    = "\xe2\x95\xae";  // ╮ (rounded)
static constexpr const char BOX_BR[]    = "\xe2\x95\xaf";  // ╯ (rounded)
static constexpr const char BOX_BL[]    = "\xe2\x95\xb0";  // ╰ (rounded)
static constexpr const char BOX_LT[]    = "\xe2\x94\x9c";  // ├
static constexpr const char BOX_RT[]    = "\xe2\x94\xa4";  // ┤
static constexpr const char BOX_TT[]    = "\xe2\x94\xac";  // ┬
static constexpr const char BOX_BT[]    = "\xe2\x94\xb4";  // ┴
static constexpr const char BOX_X[]     = "\xe2\x94\xbc";  // ┼

static std::string repeat_utf8(const char* glyph, int n) {
    std::string s;
    if (n <= 0) return s;
    size_t gl = std::strlen(glyph);
    s.reserve(gl * (size_t)n);
    for (int i = 0; i < n; ++i) s.append(glyph, gl);
    return s;
}

static bool is_integer_type(arrow::Type::type t) {
    switch (t) {
        case arrow::Type::INT8:   case arrow::Type::INT16:
        case arrow::Type::INT32:  case arrow::Type::INT64:
        case arrow::Type::UINT8:  case arrow::Type::UINT16:
        case arrow::Type::UINT32: case arrow::Type::UINT64:
            return true;
        default: return false;
    }
}

// Maximum number of characters needed to display any value of the given
// integer type after digits_with_sep formatting (including a leading minus
// sign for signed types). Used to size integer columns so digits never clip.
static int integer_type_max_width(arrow::Type::type t) {
    switch (t) {
        case arrow::Type::INT8:   return 4;   // -128
        case arrow::Type::UINT8:  return 3;   // 255
        case arrow::Type::INT16:  return 7;   // -32_768
        case arrow::Type::UINT16: return 6;   // 65_535
        case arrow::Type::INT32:  return 14;  // -2_147_483_648
        case arrow::Type::UINT32: return 13;  // 4_294_967_295
        case arrow::Type::INT64:  return 26;  // -9_223_372_036_854_775_808
        case arrow::Type::UINT64: return 26;  // 18_446_744_073_709_551_615
        default: return 0;
    }
}

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

    // Lists [..], tuples (..), maps {..}: prefer keeping the first element
    // visible — render "<open>first, …<close>" rather than slicing mid-value.
    if (s.size() >= 2) {
        char open  = s.front();
        char close = (open == '[') ? ']' : (open == '(') ? ')' : (open == '{') ? '}' : 0;
        if (close && s.back() == close) {
            int depth = 0;
            size_t comma = std::string::npos;
            for (size_t i = 0; i < s.size(); ++i) {
                char c = s[i];
                if (c == '[' || c == '(' || c == '{') ++depth;
                else if (c == ']' || c == ')' || c == '}') --depth;
                else if (depth == 1 && c == ',' && i + 1 < s.size() && s[i+1] == ' ') {
                    comma = i; break;
                }
            }
            if (comma != std::string::npos) {
                std::string cand;
                cand += open;
                cand.append(s, 1, comma - 1);
                cand += ", ";
                cand += ELLIPSIS;
                cand += close;
                if (display_width(cand) <= max_w) return cand;
            }
        }
    }

    // ELLIPSIS is 3 UTF-8 bytes but 1 display column, so we keep (max_w-1) content chars.
    return s.substr(0, max_w - 1) + ELLIPSIS;
}

// Format a decimal integer string with '_' grouping every three digits
// (Python PEP 515 style).  A leading '-' or '+' is preserved.
// e.g. "123456789" → "123_456_789", "-1000000" → "-1_000_000".
// Non-numeric strings pass through unchanged.
static std::string digits_with_sep(const std::string& s) {
    if (s.empty()) return s;
    size_t off = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (off == s.size()) return s;
    for (size_t i = off; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return s;
    std::string r = s.substr(0, off);
    r.reserve(s.size() + (s.size() - off - 1) / 3);
    for (size_t i = off; i < s.size(); ++i) {
        if (i > off && (s.size() - i) % 3 == 0) r += '_';
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
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g",
                (double)static_cast<const arrow::FloatArray&>(arr).Value(row));
            return buf;
        }
        case arrow::Type::DOUBLE: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g",
                static_cast<const arrow::DoubleArray&>(arr).Value(row));
            return buf;
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
        case arrow::Type::LIST: {
            auto& la = static_cast<const arrow::ListArray&>(arr);
            int32_t off = la.value_offset(row);
            int32_t len = la.value_length(row);
            auto values = la.values();
            std::string s = "[";
            for (int32_t i = 0; i < len; ++i) {
                if (i) s += ", ";
                s += cell_to_string(*values, off + i);
            }
            s += "]";
            return s;
        }
        case arrow::Type::LARGE_LIST: {
            auto& la = static_cast<const arrow::LargeListArray&>(arr);
            int64_t off = la.value_offset(row);
            int64_t len = la.value_length(row);
            auto values = la.values();
            std::string s = "[";
            for (int64_t i = 0; i < len; ++i) {
                if (i) s += ", ";
                s += cell_to_string(*values, off + i);
            }
            s += "]";
            return s;
        }
        case arrow::Type::FIXED_SIZE_LIST: {
            auto& la = static_cast<const arrow::FixedSizeListArray&>(arr);
            int32_t n = la.list_type()->list_size();
            int64_t off = (int64_t)row * n;
            auto values = la.values();
            std::string s = "(";
            for (int32_t i = 0; i < n; ++i) {
                if (i) s += ", ";
                s += cell_to_string(*values, off + i);
            }
            s += ")";
            return s;
        }
        case arrow::Type::MAP: {
            auto& ma = static_cast<const arrow::MapArray&>(arr);
            int32_t off = ma.value_offset(row);
            int32_t len = ma.value_length(row);
            auto keys  = ma.keys();
            auto items = ma.items();
            std::string s = "{";
            for (int32_t i = 0; i < len; ++i) {
                if (i) s += ", ";
                s += cell_to_string(*keys,  off + i);
                s += ": ";
                s += cell_to_string(*items, off + i);
            }
            s += "}";
            return s;
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

// Like cell_to_string(), but formats integer values with '_' grouping for
// human-readable display. Used for TUI + table view; CSV/TSV export and
// any value comparisons go through cell_to_string() to keep raw digits.
static std::string cell_to_display_string(const arrow::Array& arr, int64_t row) {
    if (arr.IsNull(row)) return NULL_SYMBOL;
    if (is_integer_type(arr.type_id()))
        return digits_with_sep(cell_to_string(arr, row));
    if (arr.type_id() == arrow::Type::DICTIONARY) {
        auto& dict_arr = static_cast<const arrow::DictionaryArray&>(arr);
        return cell_to_display_string(*dict_arr.dictionary(),
            dict_arr.GetValueIndex(row));
    }
    return cell_to_string(arr, row);
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

enum class SepKind { Top, Middle, Bottom };

static void draw_separator(const std::vector<Column>& cols,
                           SepKind kind = SepKind::Middle) {
    const char* left;
    const char* sep;
    const char* right;
    switch (kind) {
        case SepKind::Top:    left = BOX_TL; sep = BOX_TT; right = BOX_TR; break;
        case SepKind::Bottom: left = BOX_BL; sep = BOX_BT; right = BOX_BR; break;
        case SepKind::Middle: default:
                              left = BOX_LT; sep = BOX_X;  right = BOX_RT; break;
    }
    std::printf("%s%s", g_color.border, left);
    for (size_t i = 0; i < cols.size(); ++i) {
        for (int j = 0; j < cols[i].width + 2; ++j) std::printf("%s", BOX_HLINE);
        std::printf("%s", (i + 1 == cols.size()) ? right : sep);
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
        std::printf("%s%s%s", g_color.border, BOX_VLINE, g_color.reset);
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
    std::printf("%s%s%s\n", g_color.border, BOX_VLINE, g_color.reset);
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

// Sequential-only InputStream that wraps a raw file descriptor via read(2).
// We can't use Arrow's ReadableFile-by-fd here because that calls lseek()
// at open time (fails on a pipe), and arrow::io::StdinStream goes through
// std::cin which conflicts with our preceding raw read(2) sniff.
class FdInputStream : public arrow::io::InputStream {
    int  fd_     = -1;
    bool closed_ = false;
    int64_t pos_ = 0;
public:
    explicit FdInputStream(int fd) : fd_(fd) {}
    ~FdInputStream() override { if (!closed_) (void)Close(); }

    arrow::Status Close() override { closed_ = true; return arrow::Status::OK(); }
    bool closed() const override { return closed_; }
    arrow::Result<int64_t> Tell() const override { return pos_; }

    arrow::Result<int64_t> Read(int64_t n, void* out) override {
        uint8_t* p = (uint8_t*)out;
        int64_t total = 0;
        while (n > 0) {
            ssize_t got = ::read(fd_, p, (size_t)n);
            if (got == 0) break;
            if (got < 0) {
                if (errno == EINTR) continue;
                return arrow::Status::IOError("fd read failed: ", std::strerror(errno));
            }
            p += got; n -= got; total += got; pos_ += got;
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

// Streams the lines emitted by a tabix iterator (one or more comma-separated
// regions over a tabix-indexed bgzipped file) as if they were the data portion
// of the original file. Used to feed Arrow's CSV/TSV reader with only the
// records that overlap a requested region.
class TabixInputStream : public arrow::io::InputStream {
    htsFile*               fp_     = nullptr;
    tbx_t*                 tbx_    = nullptr;
    std::vector<hts_itr_t*> iters_;
    size_t                 cur_iter_ = 0;
    std::string            buf_;       // current line + '\n', drained byte-by-byte
    size_t                 pos_     = 0;
    bool                   eof_     = false;
    bool                   closed_  = false;
public:
    static std::string open(const std::string& path,
                            const std::string& region,
                            std::shared_ptr<TabixInputStream>* out)
    {
        auto self = std::shared_ptr<TabixInputStream>(new TabixInputStream());
        self->fp_  = hts_open(path.c_str(), "r");
        if (!self->fp_) return "Cannot open '" + path + "' for tabix";
        self->tbx_ = tbx_index_load(path.c_str());
        if (!self->tbx_) {
            hts_close(self->fp_); self->fp_ = nullptr;
            return "No tabix index (.tbi) found for '" + path + "'. "
                   "Index it with: tabix -p <type> '" + path + "'";
        }
        // Comma-separated list of regions
        std::vector<std::string> regs;
        size_t start = 0;
        while (start <= region.size()) {
            size_t comma = region.find(',', start);
            std::string r = region.substr(start,
                comma == std::string::npos ? std::string::npos : comma - start);
            if (!r.empty()) regs.push_back(r);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (regs.empty()) regs.push_back(region);
        for (const auto& r : regs) {
            hts_itr_t* it = tbx_itr_querys(self->tbx_, r.c_str());
            if (!it) return "Cannot query region '" + r + "' in '" + path + "'";
            self->iters_.push_back(it);
        }
        *out = std::move(self);
        return "";
    }

    ~TabixInputStream() override {
        for (auto* it : iters_) if (it) tbx_itr_destroy(it);
        if (tbx_) tbx_destroy(tbx_);
        if (fp_)  hts_close(fp_);
    }

    arrow::Status Close() override { closed_ = true; return arrow::Status::OK(); }
    bool closed() const override { return closed_; }
    arrow::Result<int64_t> Tell() const override {
        return arrow::Status::NotImplemented("TabixInputStream::Tell");
    }
    arrow::Result<int64_t> Read(int64_t n, void* out) override {
        uint8_t* p = static_cast<uint8_t*>(out);
        int64_t total = 0;
        while (n > 0 && !eof_) {
            if (pos_ >= buf_.size()) {
                if (!fetch_next_line()) break;
            }
            int64_t avail = (int64_t)(buf_.size() - pos_);
            int64_t take  = std::min(n, avail);
            std::memcpy(p, buf_.data() + pos_, (size_t)take);
            pos_ += (size_t)take;
            p += take; n -= take; total += take;
        }
        return total;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(n));
        ARROW_ASSIGN_OR_RAISE(int64_t actual, Read(n, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(actual, false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }
private:
    bool fetch_next_line() {
        kstring_t s = {0, 0, nullptr};
        while (cur_iter_ < iters_.size()) {
            int r = tbx_itr_next(fp_, tbx_, iters_[cur_iter_], &s);
            if (r >= 0) {
                buf_.assign(s.s, s.l);
                buf_ += '\n';
                pos_ = 0;
                if (s.s) free(s.s);
                return true;
            }
            ++cur_iter_;
        }
        if (s.s) free(s.s);
        eof_ = true;
        return false;
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

// CSV/TSV: strips any leading '#'-prefixed lines as preamble. If the last such
// line has a single '#' (not '##') and its field count matches the first data
// line, treat it as the header row (returned via *col_names_out, stripped of
// the leading '#'); otherwise leave it in the preamble.
static std::vector<std::string> strip_tsv_csv_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    char delim, bool seekable, std::string* put_back,
    std::vector<std::string>* col_names_out)
{
    std::vector<std::string> preamble;
    std::string first_data_line;
    int64_t pre_line_pos = 0;
    for (;;) {
        if (seekable) {
            auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
            if (auto t = rf->Tell(); t.ok()) pre_line_pos = *t;
        }
        std::string line;
        bool ok = read_stream_line(input, &line);
        if (!ok && line.empty()) break;
        if (!line.empty() && line[0] == '#') {
            preamble.push_back(line);
            if (!ok) break;
        } else {
            first_data_line = line;
            if (seekable) {
                auto rf = std::static_pointer_cast<arrow::io::RandomAccessFile>(input);
                (void)rf->Seek(pre_line_pos);
            } else if (put_back) {
                *put_back = line + "\n";
            }
            break;
        }
    }
    if (!preamble.empty() && !first_data_line.empty()) {
        const std::string& last = preamble.back();
        if (last.size() >= 2 && last[0] == '#' && last[1] != '#') {
            auto count_fields = [&](const std::string& s) {
                int n = 1;
                for (char c : s) if (c == delim) ++n;
                return n;
            };
            if (count_fields(last.substr(1)) == count_fields(first_data_line)) {
                std::istringstream ss(last.substr(1));
                std::string tok;
                while (std::getline(ss, tok, delim))
                    col_names_out->push_back(tok);
                preamble.pop_back();
            }
        }
    }
    return preamble;
}

// ── Region parsing ───────────────────────────────────────────────────────────
//
// Parse one "chrom:start-end" element (BED-style 0-based half-open). Open
// ends are accepted: "chr1:" → whole chrom, "chr1:78-" → 78 to end,
// "chr1:-99" → start to 99. Returns false on a syntactically bad spec.
struct Region {
    std::string chrom;
    int64_t     start;   // INT64_MIN means open lower bound
    int64_t     end;     // INT64_MAX means open upper bound
};

// Parse "chrom[:start[-end]]" into a Region. Output is always normalized
// to 0-based half-open. When one_based is true, the input is interpreted
// per the tabix / VCF / samtools convention (1-based inclusive at both
// ends) and converted internally.
static bool parse_region_one(const std::string& s, Region* out,
                              bool one_based = false) {
    auto colon = s.find(':');
    if (colon == std::string::npos) {
        out->chrom = s;
        out->start = INT64_MIN;
        out->end   = INT64_MAX;
        return !out->chrom.empty();
    }
    out->chrom = s.substr(0, colon);
    if (out->chrom.empty()) return false;
    std::string rest = s.substr(colon + 1);
    if (rest.empty()) {
        out->start = INT64_MIN;
        out->end   = INT64_MAX;
        return true;
    }
    auto dash = rest.find('-');
    std::string a, b;
    if (dash == std::string::npos) { a = rest; }
    else { a = rest.substr(0, dash); b = rest.substr(dash + 1); }
    auto parse_int = [](const std::string& t, int64_t* v) {
        if (t.empty()) return true;  // open end
        try { *v = std::stoll(t); return true; }
        catch (...) { return false; }
    };
    int64_t pa = INT64_MIN, pb = INT64_MAX;
    bool have_a = !a.empty();
    bool have_b = (dash != std::string::npos) && !b.empty();
    if (have_a && !parse_int(a, &pa)) return false;
    if (have_b && !parse_int(b, &pb)) return false;

    if (one_based) {
        // tabix-style coordinates → 0-based half-open
        //   "a-b"   1-based [a, b] inclusive  →  [a - 1, b)
        //   "a"     1-based single position a →  [a - 1, a)
        //   "a-"    open upper                →  [a - 1, INT64_MAX)
        //   "-b"    open lower                →  [0, b)
        if (have_a) out->start = std::max<int64_t>(0, pa - 1);
        else        out->start = INT64_MIN;
        if (dash == std::string::npos) {
            out->end = have_a ? pa : INT64_MAX;   // single position
        } else {
            out->end = have_b ? pb : INT64_MAX;
        }
    } else {
        // BED-style coordinates: already 0-based half-open
        out->start = have_a ? pa : INT64_MIN;
        if (dash == std::string::npos) {
            // "chrom:N" → single 0-based position [N, N+1)
            out->end = have_a ? out->start + 1 : INT64_MAX;
        } else {
            out->end = have_b ? pb : INT64_MAX;
        }
    }
    return true;
}

static std::vector<Region> parse_region_list(const std::string& spec,
                                              bool one_based = false) {
    std::vector<Region> out;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        std::string tok = spec.substr(start,
            comma == std::string::npos ? std::string::npos : comma - start);
        if (!tok.empty()) {
            Region r{};
            if (parse_region_one(tok, &r, one_based)) out.push_back(std::move(r));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// ── Simple value-predicate filter (--filter) ─────────────────────────────────
//
// Grammar:
//   filter   := and-clause ( OR  and-clause )*
//   clause   := atom       ( AND atom       )*
//   atom     := IDENT OP LITERAL
//   OP       := == | != | < | <= | > | >=
//   LITERAL  := int | float | "double-quoted" | 'single-quoted'
//
// Case-insensitive AND/OR. Whitespace-separated tokens. Strings may
// contain anything but the surrounding quote (no escapes — keep simple).
//
// The expression is evaluated per row over a Table whose column order
// matches the source's full schema, so atoms reference columns by their
// schema field index (resolved at parse time from the column name).
struct FilterAtom {
    int      col_idx = -1;
    enum Op { Eq, Ne, Lt, Le, Gt, Ge } op = Eq;
    enum Kind { K_Int, K_Double, K_String } kind = K_String;
    int64_t  i_lit = 0;
    double   f_lit = 0.0;
    std::string s_lit;
};
struct FilterExpr {
    // OR of AND clauses; row matches iff some clause's atoms all match.
    std::vector<std::vector<FilterAtom>> groups;
};

static std::vector<std::string> filter_tokenize(const std::string& s) {
    std::vector<std::string> toks;
    size_t i = 0;
    while (i < s.size()) {
        if (std::isspace((unsigned char)s[i])) { ++i; continue; }
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i++];
            std::string lit = std::string(1, q);
            while (i < s.size() && s[i] != q) lit += s[i++];
            if (i < s.size()) lit += s[i++];  // closing quote
            toks.push_back(lit);
            continue;
        }
        // Operator characters as a chunk: == != < <= > >=
        if (s[i]=='='||s[i]=='!'||s[i]=='<'||s[i]=='>') {
            std::string op(1, s[i++]);
            if (i < s.size() && s[i]=='=') op += s[i++];
            toks.push_back(op);
            continue;
        }
        // Bare word: identifier or numeric literal
        std::string w;
        while (i < s.size() && !std::isspace((unsigned char)s[i])
               && s[i]!='"' && s[i]!='\''
               && s[i]!='=' && s[i]!='!' && s[i]!='<' && s[i]!='>')
            w += s[i++];
        if (!w.empty()) toks.push_back(w);
    }
    return toks;
}

static bool filter_parse_op(const std::string& t, FilterAtom::Op* op) {
    if (t == "==") { *op = FilterAtom::Eq; return true; }
    if (t == "!=") { *op = FilterAtom::Ne; return true; }
    if (t == "<")  { *op = FilterAtom::Lt; return true; }
    if (t == "<=") { *op = FilterAtom::Le; return true; }
    if (t == ">")  { *op = FilterAtom::Gt; return true; }
    if (t == ">=") { *op = FilterAtom::Ge; return true; }
    return false;
}

// Parse the user's `--filter` expression. Returns true on success and
// populates `out`. On failure, writes a human-readable reason to `err`.
static bool parse_filter_expr(const std::string& expr,
                              const arrow::Schema& schema,
                              FilterExpr* out, std::string* err) {
    out->groups.clear();
    auto toks = filter_tokenize(expr);
    if (toks.empty()) { *err = "empty filter expression"; return false; }
    auto eq_ci = [](const std::string& a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t k = 0; k < a.size(); ++k)
            if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]))
                return false;
        return true;
    };
    out->groups.emplace_back();
    size_t i = 0;
    while (i < toks.size()) {
        if (i + 3 > toks.size()) {
            *err = "expected '<column> <op> <value>' near token '" + toks[i] + "'";
            return false;
        }
        FilterAtom a;
        a.col_idx = schema.GetFieldIndex(toks[i]);
        if (a.col_idx < 0) {
            *err = "unknown column '" + toks[i] + "' in filter";
            return false;
        }
        if (!filter_parse_op(toks[i+1], &a.op)) {
            *err = "expected an operator (== != < <= > >=), got '" + toks[i+1] + "'";
            return false;
        }
        const std::string& lit = toks[i+2];
        if (lit.size() >= 2 && (lit.front() == '"' || lit.front() == '\'')
            && lit.front() == lit.back()) {
            a.kind  = FilterAtom::K_String;
            a.s_lit = lit.substr(1, lit.size() - 2);
        } else if (lit.find_first_of(".eE") != std::string::npos) {
            try { a.f_lit = std::stod(lit); }
            catch (...) { *err = "bad number '" + lit + "'"; return false; }
            a.kind = FilterAtom::K_Double;
        } else {
            try { a.i_lit = std::stoll(lit); }
            catch (...) { *err = "bad integer '" + lit + "'"; return false; }
            a.kind = FilterAtom::K_Int;
        }
        out->groups.back().push_back(std::move(a));
        i += 3;
        if (i == toks.size()) break;
        if (eq_ci(toks[i], "and")) { ++i; continue; }
        if (eq_ci(toks[i], "or"))  { ++i; out->groups.emplace_back(); continue; }
        *err = "expected AND / OR, got '" + toks[i] + "'";
        return false;
    }
    return true;
}

// Get the int64 / double / string value of cell (col_idx, row) in `tbl`.
// Returns false for nulls or unsupported types.
static bool cell_as_int(const arrow::Table& tbl, int col, int64_t row,
                         int64_t* out) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) {
            if (ch->IsNull(r)) return false;
            if (auto a = std::dynamic_pointer_cast<arrow::Int64Array>(ch))  { *out = a->Value(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::Int32Array>(ch))  { *out = a->Value(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::Int16Array>(ch))  { *out = a->Value(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::Int8Array>(ch))   { *out = a->Value(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::UInt32Array>(ch)) { *out = (int64_t)a->Value(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::FloatArray>(ch))  { *out = (int64_t)a->Value(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::DoubleArray>(ch)) { *out = (int64_t)a->Value(r); return true; }
            return false;
        }
        r -= ch->length();
    }
    return false;
}
static bool cell_as_double(const arrow::Table& tbl, int col, int64_t row,
                            double* out) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) {
            if (ch->IsNull(r)) return false;
            if (auto a = std::dynamic_pointer_cast<arrow::DoubleArray>(ch)) { *out = a->Value(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::FloatArray>(ch))  { *out = a->Value(r); return true; }
            int64_t i;
            if (cell_as_int(tbl, col, row, &i)) { *out = (double)i; return true; }
            return false;
        }
        r -= ch->length();
    }
    return false;
}
static bool cell_as_string(const arrow::Table& tbl, int col, int64_t row,
                            std::string* out) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) {
            if (ch->IsNull(r)) return false;
            if (auto a = std::dynamic_pointer_cast<arrow::StringArray>(ch))      { *out = a->GetString(r); return true; }
            if (auto a = std::dynamic_pointer_cast<arrow::LargeStringArray>(ch)) { *out = a->GetString(r); return true; }
            return false;
        }
        r -= ch->length();
    }
    return false;
}

// Translate `a.col_idx` (a schema-level source-column index) into the
// position of that column inside the projected `tbl` passed to the
// filter. Returns -1 if absent (treated as "no match").
static int filter_col_in_table(const FilterAtom& a,
                                const std::vector<int>& read_indices) {
    for (size_t k = 0; k < read_indices.size(); ++k)
        if (read_indices[k] == a.col_idx) return (int)k;
    return -1;
}

static bool eval_atom(const arrow::Table& tbl, int64_t row, const FilterAtom& a,
                       const std::vector<int>& read_indices) {
    int tcol = filter_col_in_table(a, read_indices);
    if (tcol < 0) return false;
    if (a.kind == FilterAtom::K_String) {
        std::string s;
        if (!cell_as_string(tbl, tcol, row, &s)) return false;
        int c = s.compare(a.s_lit);
        switch (a.op) {
            case FilterAtom::Eq: return c == 0;
            case FilterAtom::Ne: return c != 0;
            case FilterAtom::Lt: return c <  0;
            case FilterAtom::Le: return c <= 0;
            case FilterAtom::Gt: return c >  0;
            case FilterAtom::Ge: return c >= 0;
        }
    } else if (a.kind == FilterAtom::K_Int) {
        int64_t v;
        if (cell_as_int(tbl, tcol, row, &v)) {
            switch (a.op) {
                case FilterAtom::Eq: return v == a.i_lit;
                case FilterAtom::Ne: return v != a.i_lit;
                case FilterAtom::Lt: return v <  a.i_lit;
                case FilterAtom::Le: return v <= a.i_lit;
                case FilterAtom::Gt: return v >  a.i_lit;
                case FilterAtom::Ge: return v >= a.i_lit;
            }
        }
        // Fall back to double if the column isn't integral.
        double d;
        if (!cell_as_double(tbl, tcol, row, &d)) return false;
        double L = (double)a.i_lit;
        switch (a.op) {
            case FilterAtom::Eq: return d == L;
            case FilterAtom::Ne: return d != L;
            case FilterAtom::Lt: return d <  L;
            case FilterAtom::Le: return d <= L;
            case FilterAtom::Gt: return d >  L;
            case FilterAtom::Ge: return d >= L;
        }
    } else {
        double d;
        if (!cell_as_double(tbl, tcol, row, &d)) return false;
        switch (a.op) {
            case FilterAtom::Eq: return d == a.f_lit;
            case FilterAtom::Ne: return d != a.f_lit;
            case FilterAtom::Lt: return d <  a.f_lit;
            case FilterAtom::Le: return d <= a.f_lit;
            case FilterAtom::Gt: return d >  a.f_lit;
            case FilterAtom::Ge: return d >= a.f_lit;
        }
    }
    return false;
}

// Apply `expr` to `tbl`, returning the subset of rows that match.
// Builds contiguous matching runs and concatenates them — avoids Arrow's
// compute kernels (which get GC'd from our static build).
static std::shared_ptr<arrow::Table> apply_filter(
        const std::shared_ptr<arrow::Table>& tbl, const FilterExpr& expr,
        const std::vector<int>& read_indices) {
    int64_t n = tbl->num_rows();
    std::vector<std::shared_ptr<arrow::Table>> runs;
    int64_t run_start = -1;
    auto flush = [&](int64_t end) {
        if (run_start >= 0) {
            runs.push_back(tbl->Slice(run_start, end - run_start));
            run_start = -1;
        }
    };
    for (int64_t r = 0; r < n; ++r) {
        bool any = false;
        for (const auto& clause : expr.groups) {
            bool all = true;
            for (const auto& a : clause) {
                if (!eval_atom(*tbl, r, a, read_indices)) { all = false; break; }
            }
            if (all) { any = true; break; }
        }
        if (any) { if (run_start < 0) run_start = r; }
        else     { flush(r); }
    }
    flush(n);
    if (runs.empty())
        return tbl->Slice(0, 0);
    if (runs.size() == 1) return runs[0];
    auto cr = arrow::ConcatenateTables(runs);
    return cr.ok() ? cr.ValueOrDie() : tbl;
}

// Field indices the filter expression references, union'd with `base`.
// Used to widen the read-projection so the filter has the cells it needs.
static std::vector<int> union_with_filter(
        const std::vector<int>& base, const FilterExpr& expr) {
    std::set<int> s(base.begin(), base.end());
    for (auto& g : expr.groups) for (auto& a : g) s.insert(a.col_idx);
    return std::vector<int>(s.begin(), s.end());
}

// Project `tbl` (which may contain extra columns loaded for the filter) down
// to the user's requested columns, in `requested_indices`-source-order.
static std::shared_ptr<arrow::Table> project_to_requested(
        const std::shared_ptr<arrow::Table>& tbl,
        const std::vector<int>& read_indices,
        const std::vector<int>& requested) {
    if (read_indices == requested) return tbl;
    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    arrow::FieldVector fields;
    for (int r : requested) {
        for (size_t k = 0; k < read_indices.size(); ++k) {
            if (read_indices[k] == r) {
                cols.push_back(tbl->column((int)k));
                fields.push_back(tbl->schema()->field((int)k));
                break;
            }
        }
    }
    return arrow::Table::Make(arrow::schema(fields), cols, tbl->num_rows());
}

// Apply --regions-file (read BED chrom/start/end, append to cfg.region) and
// --slop N (pad every window by N bp on each side). Mutates `cfg` to reflect
// the effective region list in cfg.region. Returns "" on success.
static std::string apply_region_modifiers(Config& cfg) {
    // 0) Canonicalise to 0-based half-open. --coords 1-based applies only to
    // -r / --region inputs; --regions-file entries are always BED (0-based)
    // per the spec. After this, cfg.region is guaranteed 0-based half-open
    // and downstream call sites can ignore cfg.coords_one_based.
    if (cfg.coords_one_based && !cfg.region.empty()) {
        auto regs = parse_region_list(cfg.region, /*one_based=*/true);
        std::string acc;
        for (auto& r : regs) {
            if (!acc.empty()) acc += ",";
            acc += r.chrom + ":";
            if (r.start != INT64_MIN) acc += std::to_string(r.start);
            acc += "-";
            if (r.end != INT64_MAX) acc += std::to_string(r.end);
        }
        cfg.region = acc;
    }
    cfg.coords_one_based = false;

    // 1) Read --regions-file and append its windows.
    if (!cfg.regions_file.empty()) {
        std::ifstream f(cfg.regions_file);
        if (!f.is_open())
            return "Cannot open --regions-file '" + cfg.regions_file + "'";
        std::string line;
        std::string acc = cfg.region;
        while (std::getline(f, line)) {
            // strip CR/LF and trailing whitespace
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            // skip blank lines and BED preamble
            if (line.empty()) continue;
            if (line[0] == '#') continue;
            if (line.rfind("track", 0) == 0 || line.rfind("browser", 0) == 0) continue;
            // First three TSV fields: chrom, start, end.
            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            size_t t3 = line.find('\t', t2 + 1);
            std::string chrom = line.substr(0, t1);
            std::string s     = line.substr(t1 + 1, t2 - t1 - 1);
            std::string e     = (t3 == std::string::npos)
                                ? line.substr(t2 + 1)
                                : line.substr(t2 + 1, t3 - t2 - 1);
            if (chrom.empty() || s.empty() || e.empty()) continue;
            if (!acc.empty()) acc += ",";
            acc += chrom + ":" + s + "-" + e;
        }
        cfg.region = acc;
    }

    // 2) Apply --slop by re-serialising the parsed region list.
    if (cfg.slop != 0 && !cfg.region.empty()) {
        auto regs = parse_region_list(cfg.region, cfg.coords_one_based);
        std::string acc;
        for (auto& r : regs) {
            // Don't expand open bounds — INT64_MIN/MAX stay sentinels.
            if (r.start != INT64_MIN) r.start = std::max<int64_t>(0, r.start - cfg.slop);
            if (r.end   != INT64_MAX) r.end   = r.end + cfg.slop;
            if (!acc.empty()) acc += ",";
            acc += r.chrom + ":";
            if (r.start != INT64_MIN) acc += std::to_string(r.start);
            acc += "-";
            if (r.end != INT64_MAX) acc += std::to_string(r.end);
        }
        cfg.region = acc;
    }
    return "";
}

// ── autoSql parser (minimal, hand-rolled) ────────────────────────────────────
//
// Parses the bigBed-embedded autoSql definition into an ordered list of
// (name, Arrow-type) pairs. The full UCSC autoSql grammar is sizeable
// (it can describe nested structs); for bigBed we only need the field
// list inside the top-level `table ... ( ... )` block. Unsupported types
// (object, simple, table references, lstring lists) fall back to `string`.
struct AutosqlField {
    std::string                     name;
    std::shared_ptr<arrow::DataType> arrow_type;
    bool                            is_list = false;
    std::shared_ptr<arrow::DataType> elem_type;  // only valid if is_list
};

static std::shared_ptr<arrow::DataType> autosql_base_type(const std::string& t) {
    if (t == "byte")    return arrow::int8();
    if (t == "ubyte")   return arrow::uint8();
    if (t == "short")   return arrow::int16();
    if (t == "ushort")  return arrow::uint16();
    if (t == "int")     return arrow::int32();
    if (t == "uint")    return arrow::uint32();
    if (t == "bigint")  return arrow::int64();
    if (t == "float")   return arrow::float32();
    if (t == "double")  return arrow::float64();
    // strings, char arrays, enum, set, lstring → utf8
    if (t == "string" || t == "lstring") return arrow::utf8();
    if (t.rfind("char[", 0) == 0)        return arrow::utf8();
    if (t.rfind("enum",  0) == 0)        return arrow::utf8();
    if (t.rfind("set",   0) == 0)        return arrow::utf8();
    return arrow::utf8();  // unknown — fall through
}

static std::vector<AutosqlField> parse_autosql(const std::string& sql) {
    std::vector<AutosqlField> out;
    // Find the opening '(' of the field block.
    size_t lp = sql.find('(');
    if (lp == std::string::npos) return out;
    size_t rp = sql.rfind(')');
    if (rp == std::string::npos || rp <= lp) return out;
    std::string body = sql.substr(lp + 1, rp - lp - 1);

    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1);
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    };

    // Walk char-by-char. Skip whitespace and "quoted comments"; collect
    // "<type> <name>" up to the next top-level ';'. Inside `{...}` (enum
    // / set values) commas and even ';' are part of the type.
    size_t p = 0;
    while (p < body.size()) {
        // Skip whitespace / inter-statement comments.
        while (p < body.size()) {
            char c = body[p];
            if (std::isspace((unsigned char)c)) { ++p; continue; }
            if (c == '"') {
                ++p;
                while (p < body.size() && body[p] != '"') ++p;
                if (p < body.size()) ++p;
                continue;
            }
            break;
        }
        if (p >= body.size()) break;

        // Collect everything up to the next top-level ';', tracking braces
        // and quoted strings so ';' inside `{a;b}` or `"text"` is ignored.
        std::string stmt;
        int braces = 0;
        while (p < body.size()) {
            char c = body[p];
            if (c == '"') {
                ++p;
                while (p < body.size() && body[p] != '"') ++p;
                if (p < body.size()) ++p;
                continue;
            }
            if (c == '{') { stmt += c; ++braces; ++p; continue; }
            if (c == '}') { stmt += c; --braces; ++p; continue; }
            if (c == ';' && braces == 0) { ++p; break; }
            stmt += c;
            ++p;
        }
        trim(stmt);
        if (stmt.empty()) continue;

        // Split into "<type> <name>". The type may contain `[...]`,
        // `{...}` (enum/set body), or be a single word. Boundary is the
        // last whitespace run.
        size_t name_start = stmt.find_last_of(" \t\n\r");
        if (name_start == std::string::npos) continue;
        std::string type_part = stmt.substr(0, name_start);
        std::string name_part = stmt.substr(name_start + 1);
        trim(type_part);
        trim(name_part);
        if (type_part.empty() || name_part.empty()) continue;

        AutosqlField f;
        f.name = name_part;
        // Array suffix `[N]` or `[fieldName]` — separate from `char[N]`
        // which is a fixed-size string, not a list. Look at the trailing
        // ']' and find the matching '['.
        if (!type_part.empty() && type_part.back() == ']') {
            size_t br = type_part.rfind('[');
            if (br != std::string::npos) {
                std::string base = type_part.substr(0, br);
                trim(base);
                if (base == "char") {
                    f.arrow_type = arrow::utf8();   // fixed-width string
                } else {
                    f.is_list   = true;
                    f.elem_type = autosql_base_type(base);
                    f.arrow_type = arrow::list(f.elem_type);
                }
                out.push_back(std::move(f));
                continue;
            }
        }
        f.arrow_type = autosql_base_type(type_part);
        out.push_back(std::move(f));
    }
    return out;
}

// ── LociSSD manifest parser (minimal, hand-rolled) ───────────────────────────
//
// The manifest is a UTF-8 JSON blob in the Parquet file footer under the
// `lociSSD_manifest` key. We only need three fields: each chromosome's
// `name`, `row_offset`, and `rows`. A focused string-scanning extractor is
// simpler than pulling in a JSON library and is robust to additive future
// fields. Returns true on a successful parse of at least one chromosome.
struct LocissChrom {
    std::string name;
    int64_t     row_offset = 0;
    int64_t     rows       = 0;
};

static bool parse_lociss_chromosomes(const std::string& json,
                                     std::vector<LocissChrom>* out) {
    auto skip_ws = [&](size_t& p) {
        while (p < json.size() && std::isspace((unsigned char)json[p])) ++p;
    };
    // Locate the chromosomes array start.
    auto key = json.find("\"chromosomes\"");
    if (key == std::string::npos) return false;
    size_t p = json.find('[', key);
    if (p == std::string::npos) return false;
    ++p;  // step past '['
    out->clear();
    while (p < json.size()) {
        skip_ws(p);
        if (p < json.size() && json[p] == ']') return !out->empty();
        if (json[p] != '{') return false;
        // Parse one object; track nested braces in case of future nesting.
        size_t obj_start = p, depth = 0;
        for (; p < json.size(); ++p) {
            if (json[p] == '{') ++depth;
            else if (json[p] == '}') { if (--depth == 0) { ++p; break; } }
            else if (json[p] == '"') {
                // skip a string literal (handles backslash-escapes)
                ++p;
                while (p < json.size() && json[p] != '"') {
                    if (json[p] == '\\' && p + 1 < json.size()) p += 2;
                    else ++p;
                }
            }
        }
        std::string obj = json.substr(obj_start, p - obj_start);
        // Extract scalar fields by literal key search inside this object.
        LocissChrom c{};
        auto find_str = [&](const char* k, std::string* dst) {
            std::string needle = std::string("\"") + k + "\"";
            auto q = obj.find(needle);
            if (q == std::string::npos) return false;
            q = obj.find(':', q);
            if (q == std::string::npos) return false;
            ++q;
            while (q < obj.size() && std::isspace((unsigned char)obj[q])) ++q;
            if (q >= obj.size() || obj[q] != '"') return false;
            ++q;
            size_t end = q;
            while (end < obj.size() && obj[end] != '"') {
                if (obj[end] == '\\' && end + 1 < obj.size()) end += 2;
                else ++end;
            }
            *dst = obj.substr(q, end - q);
            return true;
        };
        auto find_int = [&](const char* k, int64_t* dst) {
            std::string needle = std::string("\"") + k + "\"";
            auto q = obj.find(needle);
            if (q == std::string::npos) return false;
            q = obj.find(':', q);
            if (q == std::string::npos) return false;
            ++q;
            while (q < obj.size() && std::isspace((unsigned char)obj[q])) ++q;
            try { *dst = std::stoll(obj.substr(q)); return true; }
            catch (...) { return false; }
        };
        if (!find_str("name",       &c.name))        return false;
        if (!find_int("rows",       &c.rows))        return false;
        if (!find_int("row_offset", &c.row_offset))  return false;
        out->push_back(std::move(c));
        // Skip commas / whitespace before the next entry.
        skip_ws(p);
        if (p < json.size() && json[p] == ',') ++p;
    }
    return !out->empty();
}

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
    // Read only the first `rows` rows with col_indices columns. Default impl
    // reads forward via read_chunk() and concatenates; Parquet overrides with
    // a RecordBatchReader-based fast path so a 10-row preview doesn't decode
    // a whole 1M-row row group.
    virtual arrow::Status read_first(int64_t rows,
                                     const std::vector<int>& col_indices,
                                     std::shared_ptr<arrow::Table>* out) {
        std::shared_ptr<arrow::Table> acc;
        for (int c = 0; ; ++c) {
            ensure(c);
            if (c >= num_chunks()) break;
            std::shared_ptr<arrow::Table> chunk;
            ARROW_RETURN_NOT_OK(read_chunk(c, col_indices, &chunk));
            if (!acc) acc = chunk;
            else {
                auto r = arrow::ConcatenateTables({acc, chunk});
                ARROW_RETURN_NOT_OK(r.status());
                acc = r.ValueOrDie();
            }
            if (acc->num_rows() >= rows) break;
        }
        if (acc && acc->num_rows() > rows) acc = acc->Slice(0, rows);
        *out = acc;
        return arrow::Status::OK();
    }
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
    // Column names that should be hidden from human-facing views (table,
    // vertical-head, TUI). Delimited and Parquet output keep all columns.
    // Used e.g. to hide the derived `MaxEndSoFar` column in LociSSD files.
    virtual std::vector<std::string> hidden_for_display() const { return {}; }
};

// Field indices to display in human-facing views, with `hidden_for_display`
// names filtered out and `max_cols` honoured. Used by print_table, the
// vertical-head view, and the TUI. Delimited / Parquet output paths keep
// all columns and bypass this helper.
static std::vector<int> visible_field_indices(const TabularSource& src,
                                              int max_cols) {
    auto schema = src.schema();
    auto hidden = src.hidden_for_display();
    std::set<std::string> hide(hidden.begin(), hidden.end());
    std::vector<int> out;
    int n = schema->num_fields();
    int limit = (max_cols > 0) ? max_cols : n;
    for (int i = 0; i < n && (int)out.size() < (size_t)limit; ++i) {
        if (hide.count(schema->field(i)->name())) continue;
        out.push_back(i);
    }
    return out;
}

// Resolve `cfg.select_cols` (comma-separated names) into source field
// indices. If `cfg.select_cols` is empty, returns either the
// visible-only set (for human-facing views, default) or all fields
// (`include_hidden=true`, for export paths — `--tsv` / `--csv` /
// `--json` / `--parquet`). Unknown names appended to `*unknown_out`
// when given.
static std::vector<int> select_field_indices(
        const TabularSource& src, const Config& cfg,
        std::vector<std::string>* unknown_out = nullptr,
        bool include_hidden = false) {
    if (cfg.select_cols.empty()) {
        if (include_hidden) {
            int n = src.schema()->num_fields();
            int limit = (cfg.max_cols > 0) ? std::min(cfg.max_cols, n) : n;
            std::vector<int> out;
            for (int i = 0; i < limit; ++i) out.push_back(i);
            return out;
        }
        return visible_field_indices(src, cfg.max_cols);
    }
    auto schema = src.schema();
    std::vector<int> out;
    size_t pos = 0;
    while (pos <= cfg.select_cols.size()) {
        size_t comma = cfg.select_cols.find(',', pos);
        std::string name = cfg.select_cols.substr(pos,
            comma == std::string::npos ? std::string::npos : comma - pos);
        while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(0, 1);
        while (!name.empty() && std::isspace((unsigned char)name.back()))  name.pop_back();
        if (!name.empty()) {
            int idx = schema->GetFieldIndex(name);
            if (idx >= 0) out.push_back(idx);
            else if (unknown_out) unknown_out->push_back(name);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

// ── Parquet source ────────────────────────────────────────────────────────────

class ParquetSource : public TabularSource {
    std::unique_ptr<parquet::arrow::FileReader> reader_;
    std::shared_ptr<parquet::FileMetaData>      meta_;
    std::shared_ptr<arrow::Schema>              schema_;
    std::string                                  path_;
    std::vector<int64_t>                         chunk_start_;
    bool                                         is_lociss_ = false;

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
    int64_t                       region_total_rows_ = 0;
    // Column indices we need for filtering (looked up once at open()).
    int                           j_chrom_=-1, j_start_=-1, j_end_=-1, j_mes_=-1;
    // Per-row filter cache: the filtered Table for slice i (lazily computed).
    mutable std::map<int, std::shared_ptr<arrow::Table>> filtered_cache_;

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

        int64_t acc = 0;
        for (int i = 0; i < self->meta_->num_row_groups(); ++i) {
            self->chunk_start_.push_back(acc);
            acc += self->meta_->RowGroup(i)->num_rows();
        }

        // ── Region pruning (LociSSD-aware, otherwise generic) ────────────────
        if (!cfg.region.empty()) {
            // Read a row group's int-column min/max from Parquet statistics.
            // For flat numeric leaf columns (our use case) the Arrow field
            // index and the Parquet leaf column index coincide.
            auto col_stats_minmax_int = [&](int rg, int field_idx,
                                            int64_t* lo, int64_t* hi) -> bool {
                auto md = self->meta_->RowGroup(rg);
                if (field_idx < 0 || field_idx >= md->num_columns()) return false;
                auto cc = md->ColumnChunk(field_idx);
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
                auto md = self->meta_->RowGroup(rg);
                if (field_idx < 0 || field_idx >= md->num_columns()) return false;
                auto cc = md->ColumnChunk(field_idx);
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
        }

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        // Region mode: the pre-filter slice total. The per-row predicate
        // applied in read_chunk may reduce this slightly; close enough for
        // the TUI status bar and the [N rows × M columns] footer.
        return region_mode_ ? region_total_rows_ : meta_->num_rows();
    }
    int     num_chunks() const override {
        return region_mode_ ? (int)slices_.size() : meta_->num_row_groups();
    }
    ChunkMeta chunk_meta(int i) const override {
        if (region_mode_) return {slice_first_row_[i], slices_[i].len};
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
        if (!region_mode_)
            return reader_->ReadRowGroups({i}, arrow_to_leaf_indices(cols), out);

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
        ARROW_RETURN_NOT_OK(reader_->ReadRowGroups(
            {s.row_group}, arrow_to_leaf_indices(need), &raw));
        // Slice to the chromosome's portion of the row group.
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
        auto cell_int = [](const std::shared_ptr<arrow::Array>& a, int64_t r) -> int64_t {
            if (auto x = std::dynamic_pointer_cast<arrow::Int64Array>(a))  return x->Value(r);
            if (auto x = std::dynamic_pointer_cast<arrow::Int32Array>(a))  return x->Value(r);
            return 0;
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

// ── Delimited source (CSV / TSV / BED / VCF / GFF3+GTF / SAM, plain or gzip) ──

enum class DelimKind { CSV, TSV, BED, VCF, GFF, SAM, PAF };

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
    int                                   bed_level_ = 3; // detected BED standard cols (3..9)

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
        // 16 MiB blocks + per-block parsing on the CPU pool. The default
        // (~1 MiB) is too small for multi-GB files; raising it amortises
        // tokenizer overhead and lets parsing parallelise across threads.
        ropts.block_size  = 16 << 20;
        ropts.use_threads = true;
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
        return open(path, kind, /*region=*/"", out);
    }
    // Open a delimited source from an already-built InputStream.
    // `path_label` is used for error messages and as the displayed file name
    // (typically "-" for stdin). `is_gz` controls whether the gzip-decompression
    // wrapper has *already* been applied — set to true so the seekability
    // checks behave like for `.gz` files.
    static std::string open_from_stream(
        std::shared_ptr<arrow::io::InputStream> input,
        const std::string& path_label, DelimKind kind, bool is_gz,
        const std::string& region,
        std::unique_ptr<DelimitedSource>* out) {
        auto self = std::make_unique<DelimitedSource>();
        self->path_      = path_label;
        self->kind_      = kind;
        self->delimiter_ = (kind == DelimKind::CSV) ? ',' : '\t';
        return continue_open(std::move(self), std::move(input),
                             /*raw=*/nullptr, is_gz, kind, region, out);
    }
    static std::string open(const std::string& path, DelimKind kind,
                             const std::string& region,
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
        return continue_open(std::move(self), std::move(input),
                             std::move(raw), is_gz, kind, region, out);
    }
private:
    // Shared body of open() / open_from_stream(). Strips per-format preamble,
    // optionally swaps the data stream for a tabix iterator, builds the
    // streaming CSV reader, and finalises the schema.
    static std::string continue_open(
        std::unique_ptr<DelimitedSource> self,
        std::shared_ptr<arrow::io::InputStream> input,
        std::shared_ptr<arrow::io::ReadableFile> /*raw*/,
        bool is_gz, DelimKind kind, const std::string& region,
        std::unique_ptr<DelimitedSource>* out) {
        const std::string& path = self->path_;
        // Path-based opens with a regular file are seekable; gzipped or
        // stream-based (stdin) inputs are not.
        const bool seekable = !is_gz && path != "-";

        // Format-specific preamble stripping and column-name determination.
        // GFF and SAM wrap the stream in TruncateFieldsStream to handle variable columns.
        std::vector<std::string> col_names;
        std::string put_back;

        switch (kind) {
            case DelimKind::BED:
                self->preamble_lines_ = strip_bed_preamble(input, seekable, &put_back);
                break;
            case DelimKind::VCF:
                self->preamble_lines_ = strip_vcf_preamble(input, &col_names);
                break;
            case DelimKind::GFF: {
                self->preamble_lines_ = strip_prefix_preamble(input, '#', seekable, &put_back);
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
                self->preamble_lines_ = strip_prefix_preamble(input, '@', seekable, &put_back);
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
            case DelimKind::PAF: {
                // PAF (minimap2): 12 mandatory tab-separated columns followed
                // by zero or more optional `tag:type:value` tokens. No header.
                col_names = {"qname","qlen","qstart","qend","strand",
                             "tname","tlen","tstart","tend",
                             "nmatch","alen","mapq"};
                input = std::make_shared<TruncateFieldsStream>(input, 12);
                put_back.clear();
                break;
            }
            case DelimKind::CSV:
            case DelimKind::TSV:
                self->preamble_lines_ = strip_tsv_csv_preamble(
                    input, self->delimiter_, seekable, &put_back, &col_names);
                break;
            default:
                break;
        }

        // BED non-gz: put_back still handled here; GFF/SAM already cleared it above.
        if (!put_back.empty())
            input = std::make_shared<PrependInputStream>(std::move(put_back), input);

        // Tabix range query: drop the file-driven data stream and replace it
        // with a tabix iterator that yields only records overlapping `region`.
        // Preamble + column names already came from the original file above.
        if (!region.empty()) {
            std::shared_ptr<TabixInputStream> tabix;
            std::string err = TabixInputStream::open(path, region, &tabix);
            if (!err.empty()) return err;
            std::shared_ptr<arrow::io::InputStream> ti = tabix;
            if (kind == DelimKind::GFF) ti = std::make_shared<TruncateFieldsStream>(ti, 9);
            if (kind == DelimKind::SAM) ti = std::make_shared<TruncateFieldsStream>(ti, 11);
            input = ti;
        }

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

        // Eagerly read first batch so schema + first rows are immediately available.
        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();

        // BED: detect level (BED3..BED12) by checking column types sequentially,
        // then assign standard names. Cols beyond the detected level get "+1", "+2", ...
        // Allele disambiguation: if col 3 is string but values are very short (≤2 chars,
        // typical of allele columns like "A", "CG") AND col 4 isn't numeric, treat col 3
        // as an extra rather than BED4 Name.
        if (kind == DelimKind::BED) {
            int nf = self->schema_->num_fields();
            if (nf >= 3) {
                auto tt = [&](int c) { return self->schema_->field(c)->type()->id(); };
                auto is_str_t = [](arrow::Type::type t) {
                    return t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING;
                };
                auto is_num_t = [](arrow::Type::type t) {
                    return t == arrow::Type::INT8   || t == arrow::Type::INT16  ||
                           t == arrow::Type::INT32  || t == arrow::Type::INT64  ||
                           t == arrow::Type::UINT8  || t == arrow::Type::UINT16 ||
                           t == arrow::Type::UINT32 || t == arrow::Type::UINT64 ||
                           t == arrow::Type::FLOAT  || t == arrow::Type::DOUBLE ||
                           t == arrow::Type::HALF_FLOAT;
                };
                auto is_int_t = [](arrow::Type::type t) {
                    return t == arrow::Type::INT8   || t == arrow::Type::INT16  ||
                           t == arrow::Type::INT32  || t == arrow::Type::INT64  ||
                           t == arrow::Type::UINT8  || t == arrow::Type::UINT16 ||
                           t == arrow::Type::UINT32 || t == arrow::Type::UINT64;
                };
                int lvl = 3;
                do {
                    if (nf < 4  || !is_str_t(tt(3)))  break; lvl = 4;   // Name: string
                    if (nf < 5  || !is_num_t(tt(4)))  break; lvl = 5;   // Score: numeric
                    if (nf < 6  || !is_str_t(tt(5)))  break; lvl = 6;   // Strand: string
                    if (nf < 7  || !is_int_t(tt(6)))  break; lvl = 7;   // ThickStart: int
                    if (nf < 8  || !is_int_t(tt(7)))  break; lvl = 8;   // ThickEnd: int
                    if (nf < 9  || !is_str_t(tt(8)))  break; lvl = 9;   // itemRgb: string
                    if (nf < 10 || !is_int_t(tt(9)))  break; lvl = 10;  // blockCount: int
                    if (nf < 11 || !is_str_t(tt(10))) break; lvl = 11;  // blockSizes: string
                    if (nf < 12 || !is_str_t(tt(11))) break; lvl = 12;  // blockStarts: string
                } while (false);

                // Allele disambiguation: only trigger if detection stopped at BED4
                // (col 3 string, col 4 not numeric) — in BED5+ layouts the presence
                // of Score/Strand/etc. in the right slots is strong signal.
                if (lvl == 4 && !self->batches_.empty()) {
                    auto col3 = self->batches_[0]->column(3);
                    int64_t n_rows = std::min<int64_t>(self->batches_[0]->num_rows(), 100);
                    int n_nonnull = 0;
                    bool all_short = true;
                    auto check = [&](auto* arr) {
                        for (int64_t i = 0; i < n_rows; ++i) {
                            if (arr->IsNull(i)) continue;
                            ++n_nonnull;
                            if (arr->GetView(i).size() > 2) { all_short = false; return; }
                        }
                    };
                    if (auto sa = std::dynamic_pointer_cast<arrow::StringArray>(col3))
                        check(sa.get());
                    else if (auto la = std::dynamic_pointer_cast<arrow::LargeStringArray>(col3))
                        check(la.get());
                    if (all_short && n_nonnull > 0) lvl = 3;
                }
                self->bed_level_ = lvl;

                static const char* kBedNames[] = {
                    "Chr", "[Beg", "End)", "Name", "Score", "Str",
                    "ThBeg", "ThEnd", "RGB", "NBlk", "BlkSz", "BlkSt"
                };
                arrow::FieldVector fields;
                fields.reserve(nf);
                for (int i = 0; i < nf; ++i) {
                    auto f = self->schema_->field(i);
                    std::string nm = (i < lvl)
                        ? kBedNames[i]
                        : ("+" + std::to_string(i - lvl + 1));
                    fields.push_back(arrow::field(nm, f->type(), f->nullable()));
                }
                self->schema_ = arrow::schema(fields);
            }
        }

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
            case DelimKind::PAF: return "Format: PAF";
            case DelimKind::BED: {
                int nf = schema_->num_fields();
                std::string s = "Format: BED" + std::to_string(bed_level_);
                if (nf > bed_level_) {
                    int extra = nf - bed_level_;
                    s += " + " + std::to_string(extra) + " extra col" + (extra == 1 ? "" : "s");
                }
                return s;
            }
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
        return preamble_lines_;
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
                if (col_idx >= bed_level_) return 4;
                switch (col_idx) {
                    case 0:         return 6;   // Chr: "chrXII"
                    case 1: case 2: return 11;  // [Beg / End): "999_999_999"
                    case 3:         return 6;   // Name
                    case 5:         return 3;   // Str (strand: +/-/.)
                    case 8:         return 5;   // RGB bar
                    default:        return 4;
                }
            case DelimKind::VCF:
                switch (col_idx) {
                    case 0:  return 6;   // CHROM: "chrXII"
                    case 1:  return 9;   // POS
                    case 7:  return 12;  // INFO
                    default: return 4;
                }
            case DelimKind::GFF:
                switch (col_idx) {
                    case 0:         return 6;   // seqname: "chrXII"
                    case 3: case 4: return 9;   // start / end
                    case 6:         return 3;   // strand (+/-/.)
                    case 8:         return 15;  // attributes
                    default:        return 4;
                }
            case DelimKind::SAM:
                switch (col_idx) {
                    case 0:  return 10;  // QNAME
                    case 2:  return 6;   // RNAME: "chrXII"
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

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();

        arrow::StringBuilder qname_b, rname_b, cigar_b, rnext_b, seq_b, qual_b;
        arrow::Int32Builder  flag_b, mapq_b;
        arrow::Int64Builder  pos_b, pnext_b, tlen_b;

        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        int count = 0, ret = 0;
        while (count < cap && (ret = sam_read1(hts_, hdr_, rec_)) >= 0) {
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

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BamSource>* out) {
        auto self = std::make_unique<BamSource>();
        self->path_ = path;

        self->hts_ = hts_open(path.c_str(), "r");
        if (!self->hts_)
            return "Cannot open '" + path + "'";

        // Multi-threaded BGZF / CRAM slice decompression.
        int n = effective_threads(cfg);
        if (n > 1) hts_set_threads(self->hts_, n);

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
    // Cap the first-batch decode to `rows` so a small `-n` preview doesn't
    // decode 32 768 BAM records.
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        if (batches_.empty() && !all_read_ && rows > 0)
            ARROW_RETURN_NOT_OK(advance(rows));
        return TabularSource::read_first(rows, col_indices, out);
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
            case 2:  return 6;   // RNAME: "chrXII"
            case 3:  return 9;   // POS
            case 5:  return 8;   // CIGAR
            case 7:  return 9;   // PNEXT
            case 9:  return 10;  // SEQ
            case 10: return 10;  // QUAL
            default: return 4;
        }
    }
};

// ── BCF source (binary VCF via htslib) ────────────────────────────────────────

// Reads a BCF file via htslib's bcf_read, reformats each record to the
// canonical VCF text line via vcf_format(), then splits into the eight
// canonical VCF columns plus a single trailing column for any FORMAT/sample
// data. Output schema mirrors VCF text so the existing TUI INFO-expansion
// path keeps working unchanged.
class BcfSource : public TabularSource {
    std::string                              path_;
    std::shared_ptr<arrow::Schema>           schema_;
    std::vector<std::string>                 preamble_lines_;
    int                                      n_samples_ = 0;

    mutable htsFile*    fp_  = nullptr;
    mutable bcf_hdr_t*  hdr_ = nullptr;
    mutable bcf1_t*     rec_ = nullptr;

    // Region-mode (--region): index + iterators over the requested windows.
    mutable hts_idx_t*               idx_       = nullptr;
    mutable std::vector<hts_itr_t*>  iters_;
    mutable size_t                   cur_iter_  = 0;
    bool                              region_mode_ = false;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             all_read_ = false;

    static constexpr int BATCH_SIZE = 32768;

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder chrom_b, id_b, ref_b, alt_b, filter_b, info_b, samples_b;
        arrow::Int64Builder  pos_b;
        arrow::FloatBuilder  qual_b;

        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        int count = 0, ret = 0;
        kstring_t s = {0, 0, nullptr};

        // next_record(): pull one record either from the linear stream or
        // from the current region iterator (advancing to the next iterator
        // on exhaustion). Returns 0 on a record, -1 at EOF, <-1 on error.
        auto next_record = [&]() -> int {
            if (!region_mode_) return bcf_read(fp_, hdr_, rec_);
            while (cur_iter_ < iters_.size()) {
                int r = bcf_itr_next(fp_, iters_[cur_iter_], rec_);
                if (r >= 0) return 0;
                ++cur_iter_;
            }
            return -1;
        };

        while (count < cap && (ret = next_record()) == 0) {
            bcf_unpack(rec_, BCF_UN_ALL);

            // vcf_format renders the canonical tab-separated line. We split
            // it on tabs to fill the eight fixed columns; anything past the
            // 9th tab (FORMAT + samples) is kept as a single string.
            s.l = 0;
            if (vcf_format(hdr_, rec_, &s) < 0)
                return arrow::Status::IOError("vcf_format failed for ", path_);
            std::string_view line(s.s, s.l);
            // Strip a trailing newline if present.
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.remove_suffix(1);

            std::array<std::string_view, 9> f;     // first 8 + tail
            int fi = 0;
            size_t start = 0;
            for (size_t i = 0; i < line.size() && fi < 8; ++i) {
                if (line[i] == '\t') { f[fi++] = line.substr(start, i - start); start = i + 1; }
            }
            // Field 8 (INFO) ends at the next tab or end-of-line.
            // After it, anything else (FORMAT + sample columns) goes into f[8].
            size_t info_end = line.find('\t', start);
            if (fi < 8) {
                // Pathological short line — fill remaining with "."
                f[fi++] = (info_end == std::string_view::npos)
                          ? line.substr(start)
                          : line.substr(start, info_end - start);
                while (fi < 8) f[fi++] = ".";
                f[8] = std::string_view{};
            } else if (info_end == std::string_view::npos) {
                f[8] = std::string_view{};
            } else {
                f[8] = line.substr(info_end + 1);
            }

            ARROW_RETURN_NOT_OK(chrom_b.Append(f[0].data(), (int32_t)f[0].size()));
            int64_t pos = 0;
            std::from_chars(f[1].data(), f[1].data() + f[1].size(), pos);
            ARROW_RETURN_NOT_OK(pos_b.Append(pos));
            ARROW_RETURN_NOT_OK(id_b.Append(f[2].data(), (int32_t)f[2].size()));
            ARROW_RETURN_NOT_OK(ref_b.Append(f[3].data(), (int32_t)f[3].size()));
            ARROW_RETURN_NOT_OK(alt_b.Append(f[4].data(), (int32_t)f[4].size()));
            // QUAL: "." → null
            if (f[5] == "." || f[5].empty()) {
                ARROW_RETURN_NOT_OK(qual_b.AppendNull());
            } else {
                float q = 0.0f;
                std::from_chars(f[5].data(), f[5].data() + f[5].size(), q);
                ARROW_RETURN_NOT_OK(qual_b.Append(q));
            }
            ARROW_RETURN_NOT_OK(filter_b.Append(f[6].data(), (int32_t)f[6].size()));
            ARROW_RETURN_NOT_OK(info_b.Append  (f[7].data(), (int32_t)f[7].size()));
            if (n_samples_ > 0)
                ARROW_RETURN_NOT_OK(samples_b.Append(f[8].data(), (int32_t)f[8].size()));

            ++count;
        }
        if (s.s) free(s.s);

        if (ret < -1)
            return arrow::Status::IOError("error reading BCF record from ", path_);
        if (count == 0) { all_read_ = true; return arrow::Status::OK(); }
        if (ret < 0) all_read_ = true;

        std::vector<std::shared_ptr<arrow::Array>> a;
        std::shared_ptr<arrow::Array> tmp;
        ARROW_RETURN_NOT_OK(chrom_b.Finish(&tmp));  a.push_back(tmp);
        ARROW_RETURN_NOT_OK(pos_b.Finish(&tmp));    a.push_back(tmp);
        ARROW_RETURN_NOT_OK(id_b.Finish(&tmp));     a.push_back(tmp);
        ARROW_RETURN_NOT_OK(ref_b.Finish(&tmp));    a.push_back(tmp);
        ARROW_RETURN_NOT_OK(alt_b.Finish(&tmp));    a.push_back(tmp);
        ARROW_RETURN_NOT_OK(qual_b.Finish(&tmp));   a.push_back(tmp);
        ARROW_RETURN_NOT_OK(filter_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(info_b.Finish(&tmp));   a.push_back(tmp);
        if (n_samples_ > 0) {
            ARROW_RETURN_NOT_OK(samples_b.Finish(&tmp));
            a.push_back(tmp);
        }

        auto batch = arrow::RecordBatch::Make(schema_, count, a);
        batch_first_row_.push_back(rows_so_far_);
        rows_so_far_ += count;
        batches_.push_back(std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BcfSource() {
        for (auto* it : iters_) if (it) hts_itr_destroy(it);
        if (idx_) { hts_idx_destroy(idx_); idx_ = nullptr; }
        if (rec_) { bcf_destroy(rec_);     rec_ = nullptr; }
        if (hdr_) { bcf_hdr_destroy(hdr_); hdr_ = nullptr; }
        if (fp_)  { hts_close(fp_);        fp_  = nullptr; }
    }

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BcfSource>* out) {
        auto self = std::make_unique<BcfSource>();
        self->path_ = path;

        self->fp_ = hts_open(path.c_str(), "r");
        if (!self->fp_) return "Cannot open '" + path + "'";

        int n = effective_threads(cfg);
        if (n > 1) hts_set_threads(self->fp_, n);

        self->hdr_ = bcf_hdr_read(self->fp_);
        if (!self->hdr_) return "Cannot read BCF header from '" + path + "'";
        self->rec_ = bcf_init();
        if (!self->rec_) return "Out of memory allocating BCF record";

        self->n_samples_ = bcf_hdr_nsamples(self->hdr_);

        // Header lines (## meta-information) → preamble.
        {
            int n_lines = 0;
            char** hdr_text = nullptr;
            kstring_t hs = {0, 0, nullptr};
            if (bcf_hdr_format(self->hdr_, /*is_bcf=*/0, &hs) == 0 && hs.s) {
                std::istringstream iss(std::string(hs.s, hs.l));
                std::string line;
                int total = 0;
                while (std::getline(iss, line)) {
                    if (line.empty()) continue;
                    ++total;
                    if (total <= 20) self->preamble_lines_.push_back(line);
                }
                if (total > 20)
                    self->preamble_lines_.push_back(
                        "... (" + std::to_string(total - 20) + " more header lines)");
            }
            free(hs.s);
            (void)hdr_text; (void)n_lines;
        }

        // ── Range mode: load .csi/.tbi index and build per-window iterators ─
        if (!cfg.region.empty()) {
            self->idx_ = bcf_index_load(path.c_str());
            if (!self->idx_)
                return "No BCF index for '" + path + "' (try: "
                       "`bcftools index '" + path + "'`)";
            auto regs = parse_region_list(cfg.region, cfg.coords_one_based);
            for (auto& r : regs) {
                hts_itr_t* it = bcf_itr_querys(self->idx_, self->hdr_, r.chrom.c_str());
                // bcf_itr_querys takes a "chrom:start-end" string directly,
                // but we already parsed; rebuild the canonical form here.
                if (it) hts_itr_destroy(it);
                std::string rstr = r.chrom + ":";
                if (r.start != INT64_MIN) rstr += std::to_string(r.start);
                rstr += "-";
                if (r.end != INT64_MAX) rstr += std::to_string(r.end);
                it = bcf_itr_querys(self->idx_, self->hdr_, rstr.c_str());
                if (!it)
                    return "Cannot query region '" + rstr + "' in '" + path + "'";
                self->iters_.push_back(it);
            }
            self->region_mode_ = true;
        }

        arrow::FieldVector fields = {
            arrow::field("CHROM",  arrow::utf8()),
            arrow::field("POS",    arrow::int64()),
            arrow::field("ID",     arrow::utf8()),
            arrow::field("REF",    arrow::utf8()),
            arrow::field("ALT",    arrow::utf8()),
            arrow::field("QUAL",   arrow::float32()),
            arrow::field("FILTER", arrow::utf8()),
            arrow::field("INFO",   arrow::utf8()),
        };
        if (self->n_samples_ > 0)
            fields.push_back(arrow::field("FORMAT_SAMPLES", arrow::utf8()));
        self->schema_ = arrow::schema(fields);

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
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        if (batches_.empty() && !all_read_ && rows > 0)
            ARROW_RETURN_NOT_OK(advance(rows));
        return TabularSource::read_first(rows, col_indices, out);
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: BCF  |  Samples: " + std::to_string(n_samples_);
    }
    std::vector<std::string> preamble_below() const override { return preamble_lines_; }
    std::string format_cell(int col_idx, std::string val) const override {
        if (col_idx == 1) return digits_with_sep(val);  // POS
        return val;
    }
    int min_col_width(int col_idx) const override {
        switch (col_idx) {
            case 0:  return 6;   // CHROM
            case 1:  return 9;   // POS
            case 6:  return 6;   // FILTER
            default: return 4;
        }
    }
};

// ── bigBed / bigWig source (libBigWig, vendored) ──────────────────────────────
//
// One TabularSource handles both formats. For bigWig, the schema is
// fixed (chrom, start, end, value). For bigBed, the first three columns
// are the loci fields and the rest come from parsing the embedded
// autoSql with parse_autosql() above.
//
// Reads are batched per-chromosome (libBigWig's overlap API works one
// chromosome at a time anyway). With --region, only the requested
// windows are queried.
class BigSource : public TabularSource {
    std::string                              path_;
    std::shared_ptr<arrow::Schema>           schema_;
    bool                                     is_bb_ = false;
    std::vector<AutosqlField>                autosql_;

    mutable bigWigFile_t* fp_ = nullptr;

    // For region-mode, the precomputed plan of (chrom, start, end).
    bool                                     region_mode_ = false;
    std::vector<Region>                      windows_;
    mutable size_t                           cur_window_ = 0;
    mutable size_t                           cur_chrom_  = 0;
    mutable bool                             all_read_   = false;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable int64_t                          rows_so_far_ = 0;

    static constexpr int BATCH_SIZE = 32768;

    // For each pending (chrom, start, end) window, pull all overlapping
    // intervals/entries and convert into Arrow rows. Builds one batch
    // and returns; subsequent advance() calls drain the rest.
    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();

        // Choose source of windows to iterate.
        auto chrom_at = [&](size_t i) -> std::string {
            if (region_mode_) return windows_[i].chrom;
            return fp_->cl->chrom[i];
        };
        auto chrom_span = [&](size_t i, uint32_t* s, uint32_t* e) {
            if (region_mode_) {
                int64_t a = windows_[i].start;
                int64_t b = windows_[i].end;
                *s = (a == INT64_MIN) ? 0u : (uint32_t)std::max<int64_t>(0, a);
                *e = (b == INT64_MAX) ? UINT32_MAX : (uint32_t)b;
            } else {
                *s = 0u;
                *e = fp_->cl->len[i];
            }
        };
        size_t n_iter = region_mode_ ? windows_.size() : (size_t)fp_->cl->nKeys;
        size_t& idx = region_mode_ ? cur_window_ : cur_chrom_;

        // Build a list of (chrom, start, end, value, [extras...]) cells.
        arrow::StringBuilder chrom_b;
        arrow::UInt32Builder start_b, end_b;
        arrow::FloatBuilder  value_b;     // bigWig only
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> extra_b;  // bigBed extras
        if (is_bb_) {
            for (auto& f : autosql_) {
                if (f.is_list) {
                    // list<elem>: builders are awkward; fall back to a
                    // string column for v1 (TODO: real list builders).
                    extra_b.emplace_back(new arrow::StringBuilder());
                } else if (f.arrow_type->id() == arrow::Type::INT8)   extra_b.emplace_back(new arrow::Int8Builder());
                else if (f.arrow_type->id() == arrow::Type::UINT8)    extra_b.emplace_back(new arrow::UInt8Builder());
                else if (f.arrow_type->id() == arrow::Type::INT16)    extra_b.emplace_back(new arrow::Int16Builder());
                else if (f.arrow_type->id() == arrow::Type::UINT16)   extra_b.emplace_back(new arrow::UInt16Builder());
                else if (f.arrow_type->id() == arrow::Type::INT32)    extra_b.emplace_back(new arrow::Int32Builder());
                else if (f.arrow_type->id() == arrow::Type::UINT32)   extra_b.emplace_back(new arrow::UInt32Builder());
                else if (f.arrow_type->id() == arrow::Type::INT64)    extra_b.emplace_back(new arrow::Int64Builder());
                else if (f.arrow_type->id() == arrow::Type::FLOAT)    extra_b.emplace_back(new arrow::FloatBuilder());
                else if (f.arrow_type->id() == arrow::Type::DOUBLE)   extra_b.emplace_back(new arrow::DoubleBuilder());
                else                                                   extra_b.emplace_back(new arrow::StringBuilder());
            }
        }

        int count = 0;
        // Helper: append one int-like value from a string token.
        auto append_typed = [&](arrow::ArrayBuilder* b,
                                arrow::Type::type t,
                                const std::string& tok) -> arrow::Status {
            auto bad = [&]() -> arrow::Status { return static_cast<arrow::StringBuilder*>(b)->AppendNull(); };
            try {
                switch (t) {
                    case arrow::Type::INT8:   return static_cast<arrow::Int8Builder*>(b)->Append((int8_t)std::stoi(tok));
                    case arrow::Type::UINT8:  return static_cast<arrow::UInt8Builder*>(b)->Append((uint8_t)std::stoul(tok));
                    case arrow::Type::INT16:  return static_cast<arrow::Int16Builder*>(b)->Append((int16_t)std::stoi(tok));
                    case arrow::Type::UINT16: return static_cast<arrow::UInt16Builder*>(b)->Append((uint16_t)std::stoul(tok));
                    case arrow::Type::INT32:  return static_cast<arrow::Int32Builder*>(b)->Append((int32_t)std::stol(tok));
                    case arrow::Type::UINT32: return static_cast<arrow::UInt32Builder*>(b)->Append((uint32_t)std::stoul(tok));
                    case arrow::Type::INT64:  return static_cast<arrow::Int64Builder*>(b)->Append((int64_t)std::stoll(tok));
                    case arrow::Type::FLOAT:  return static_cast<arrow::FloatBuilder*>(b)->Append(std::stof(tok));
                    case arrow::Type::DOUBLE: return static_cast<arrow::DoubleBuilder*>(b)->Append(std::stod(tok));
                    case arrow::Type::STRING:
                    default:                  return static_cast<arrow::StringBuilder*>(b)->Append(tok);
                }
            } catch (...) { return bad(); }
        };

        while (count < BATCH_SIZE && idx < n_iter) {
            std::string chrom = chrom_at(idx);
            uint32_t qs, qe;
            chrom_span(idx, &qs, &qe);
            ++idx;
            // Unknown chrom — libBigWig will return NULL.
            if (is_bb_) {
                bbOverlappingEntries_t* o = bbGetOverlappingEntries(
                    fp_, chrom.c_str(), qs, qe, /*withString=*/1);
                if (!o) continue;
                for (uint32_t i = 0; i < o->l && count < BATCH_SIZE; ++i) {
                    ARROW_RETURN_NOT_OK(chrom_b.Append(chrom));
                    ARROW_RETURN_NOT_OK(start_b.Append(o->start[i]));
                    ARROW_RETURN_NOT_OK(end_b.Append(o->end[i]));
                    const char* s = o->str[i];
                    // Split s on tabs into N fields.
                    std::vector<std::string> toks;
                    if (s) {
                        const char* p = s;
                        const char* tb = p;
                        for (; *p; ++p) {
                            if (*p == '\t') { toks.emplace_back(tb, p - tb); tb = p + 1; }
                        }
                        toks.emplace_back(tb, p - tb);
                    }
                    for (size_t k = 0; k < autosql_.size(); ++k) {
                        std::string tok = (k < toks.size()) ? toks[k] : std::string();
                        auto& f = autosql_[k];
                        if (f.is_list) {
                            // Store raw "a,b,c" string for v1.
                            ARROW_RETURN_NOT_OK(static_cast<arrow::StringBuilder*>(
                                extra_b[k].get())->Append(tok));
                        } else {
                            ARROW_RETURN_NOT_OK(append_typed(extra_b[k].get(),
                                f.arrow_type->id(), tok));
                        }
                    }
                    ++count;
                }
                bbDestroyOverlappingEntries(o);
            } else {
                bwOverlappingIntervals_t* iv = bwGetOverlappingIntervals(
                    fp_, chrom.c_str(), qs, qe);
                if (!iv) continue;
                for (uint32_t i = 0; i < iv->l && count < BATCH_SIZE; ++i) {
                    ARROW_RETURN_NOT_OK(chrom_b.Append(chrom));
                    ARROW_RETURN_NOT_OK(start_b.Append(iv->start[i]));
                    ARROW_RETURN_NOT_OK(end_b.Append(iv->end[i]));
                    ARROW_RETURN_NOT_OK(value_b.Append(iv->value[i]));
                    ++count;
                }
                bwDestroyOverlappingIntervals(iv);
            }
        }
        if (idx >= n_iter) all_read_ = true;
        if (count == 0) return arrow::Status::OK();

        std::vector<std::shared_ptr<arrow::Array>> a;
        std::shared_ptr<arrow::Array> tmp;
        ARROW_RETURN_NOT_OK(chrom_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(start_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(end_b.Finish(&tmp));   a.push_back(tmp);
        if (is_bb_) {
            for (auto& b : extra_b) {
                ARROW_RETURN_NOT_OK(b->Finish(&tmp));
                a.push_back(tmp);
            }
        } else {
            ARROW_RETURN_NOT_OK(value_b.Finish(&tmp));
            a.push_back(tmp);
        }
        auto batch = arrow::RecordBatch::Make(schema_, count, a);
        batch_first_row_.push_back(rows_so_far_);
        rows_so_far_ += count;
        batches_.push_back(std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BigSource() {
        if (fp_) { bwClose(fp_); fp_ = nullptr; }
        bwCleanup();
    }

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BigSource>* out) {
        auto self = std::make_unique<BigSource>();
        self->path_ = path;
        if (bwInit(1 << 17) != 0)
            return "libBigWig init failed";
        // libBigWig auto-detects format from the magic; pick the right open.
        bool maybe_bw = bwIsBigWig(path.c_str(), nullptr) != 0;
        if (maybe_bw) {
            self->fp_ = bwOpen(path.c_str(), nullptr, "r");
            self->is_bb_ = false;
        } else {
            self->fp_ = bbOpen(path.c_str(), nullptr);
            self->is_bb_ = true;
        }
        if (!self->fp_)
            return "Cannot open '" + path + "' as bigWig/bigBed";

        // Schema.
        arrow::FieldVector fields = {
            arrow::field("chrom", arrow::utf8()),
            arrow::field("start", arrow::uint32()),
            arrow::field("end",   arrow::uint32()),
        };
        if (self->is_bb_) {
            // Read the embedded autoSql blob from the file (libBigWig
            // does not expose it; seek + read manually). The first 3
            // autoSql fields are always chrom/start/end — we skip them.
            std::string sql;
            if (self->fp_->hdr && self->fp_->hdr->sqlOffset) {
                urlSeek(self->fp_->URL, self->fp_->hdr->sqlOffset);
                char buf[4096];
                while (true) {
                    size_t n = urlRead(self->fp_->URL, buf, sizeof(buf));
                    if (n == 0) break;
                    bool done = false;
                    for (size_t i = 0; i < n; ++i) {
                        if (buf[i] == 0) {
                            sql.append(buf, i);
                            done = true;
                            break;
                        }
                    }
                    if (done) break;
                    sql.append(buf, n);
                    if (n < sizeof(buf)) break;
                }
            }
            auto all_fields = parse_autosql(sql);
            // Drop the first 3 autoSql fields (chrom/chromStart/chromEnd)
            // — we already added them above.
            int skip = std::min<int>((int)all_fields.size(), 3);
            for (int i = skip; i < (int)all_fields.size(); ++i) {
                fields.push_back(arrow::field(
                    all_fields[i].name, all_fields[i].arrow_type));
                self->autosql_.push_back(all_fields[i]);
            }
        } else {
            fields.push_back(arrow::field("value", arrow::float32()));
        }
        self->schema_ = arrow::schema(fields);

        // Region plan.
        if (!cfg.region.empty()) {
            self->windows_ = parse_region_list(cfg.region, cfg.coords_one_based);
            self->region_mode_ = true;
        }

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
        return std::string("Format: ") + (is_bb_ ? "bigBed" : "bigWig") +
               "  |  Chromosomes: " + std::to_string(fp_ ? fp_->cl->nKeys : 0);
    }
    int min_col_width(int col_idx) const override {
        if (col_idx == 0) return 6;   // chrom
        if (col_idx == 1 || col_idx == 2) return 9;  // start / end
        return 4;
    }
};

// ── FASTA / FASTQ source (kseq.h via htslib BGZF) ─────────────────────────────

KSEQ_INIT(BGZF*, bgzf_read)

class FastxSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    bool                                   is_fastq_ = false;

    mutable BGZF*    fp_ = nullptr;
    mutable kseq_t*  ks_ = nullptr;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>           batch_first_row_;
    mutable int64_t                        rows_so_far_ = 0;
    mutable bool                           all_read_    = false;

    static constexpr int BATCH_SIZE = 4096;

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder name_b, comm_b, seq_b, qual_b;
        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        int count = 0, ret = 0;
        while (count < cap && (ret = kseq_read(ks_)) >= 0) {
            // kseq doesn't reset .s on absent fields, only .l → use length.
            ARROW_RETURN_NOT_OK(name_b.Append(
                ks_->name.s ? ks_->name.s : "", (int32_t)ks_->name.l));
            ARROW_RETURN_NOT_OK(comm_b.Append(
                ks_->comment.s ? ks_->comment.s : "", (int32_t)ks_->comment.l));
            ARROW_RETURN_NOT_OK(seq_b.Append(
                ks_->seq.s ? ks_->seq.s : "", (int32_t)ks_->seq.l));
            if (is_fastq_)
                ARROW_RETURN_NOT_OK(qual_b.Append(
                    ks_->qual.s ? ks_->qual.s : "", (int32_t)ks_->qual.l));
            ++count;
        }
        if (ret < -1)
            return arrow::Status::IOError("Error reading FASTA/FASTQ from ", path_);
        if (count == 0) { all_read_ = true; return arrow::Status::OK(); }
        if (ret < 0) all_read_ = true;

        std::vector<std::shared_ptr<arrow::Array>> a;
        std::shared_ptr<arrow::Array> tmp;
        ARROW_RETURN_NOT_OK(name_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(comm_b.Finish(&tmp)); a.push_back(tmp);
        ARROW_RETURN_NOT_OK(seq_b.Finish(&tmp));  a.push_back(tmp);
        if (is_fastq_) {
            ARROW_RETURN_NOT_OK(qual_b.Finish(&tmp));
            a.push_back(tmp);
        }
        auto batch = arrow::RecordBatch::Make(schema_, count, a);
        batch_first_row_.push_back(rows_so_far_);
        rows_so_far_ += count;
        batches_.push_back(std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~FastxSource() {
        if (ks_) { kseq_destroy(ks_); ks_ = nullptr; }
        if (fp_) { bgzf_close(fp_);   fp_ = nullptr; }
    }

    static std::string open(const std::string& path, bool is_fastq,
                            const Config& cfg,
                            std::unique_ptr<FastxSource>* out)
    {
        auto self = std::make_unique<FastxSource>();
        self->path_     = path;
        self->is_fastq_ = is_fastq;

        self->fp_ = bgzf_open(path.c_str(), "r");
        if (!self->fp_) return "Cannot open '" + path + "'";
        // Multi-threaded BGZF decompression for large .gz files.
        int n = effective_threads(cfg);
        if (n > 1) bgzf_mt(self->fp_, n, 256);
        self->ks_ = kseq_init(self->fp_);
        if (!self->ks_) return "Cannot init kseq for '" + path + "'";

        arrow::FieldVector fields = {
            arrow::field("name",    arrow::utf8()),
            arrow::field("comment", arrow::utf8()),
            arrow::field("seq",     arrow::utf8()),
        };
        if (is_fastq) fields.push_back(arrow::field("qual", arrow::utf8()));
        self->schema_ = arrow::schema(fields);

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
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        if (batches_.empty() && !all_read_ && rows > 0)
            ARROW_RETURN_NOT_OK(advance(rows));
        return TabularSource::read_first(rows, col_indices, out);
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return std::string("Format: ") + (is_fastq_ ? "FASTQ" : "FASTA");
    }
    int min_col_width(int col_idx) const override {
        switch (col_idx) {
            case 0: return 12;  // name
            case 1: return 8;   // comment
            case 2: return 16;  // seq
            case 3: return 16;  // qual
            default: return 4;
        }
    }
};

// ── Arrow IPC / Feather source ────────────────────────────────────────────────

class IpcSource : public TabularSource {
    std::string                                       path_;
    bool                                              is_feather_ = false;
    std::shared_ptr<arrow::Schema>                    schema_;
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> rdr_;       // Arrow IPC only
    int                                               num_record_batches_ = 0; // Arrow IPC only
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>                      batch_first_row_;
    mutable int64_t                                   total_rows_ = 0;
    mutable bool                                      all_read_   = false;

    static constexpr int64_t BATCH_ROWS = 65536;

    // Slice a Table into BATCH_ROWS-sized RecordBatches and append to batches_
    // (Feather v1 path: the reader returns a single Table).
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
        all_read_ = true;
        return arrow::Status::OK();
    }

    // Decode the next not-yet-loaded record batch (Arrow IPC only).
    arrow::Status load_next_ipc() const {
        if (all_read_) return arrow::Status::OK();
        int i = (int)batches_.size();
        if (i >= num_record_batches_) { all_read_ = true; return arrow::Status::OK(); }
        ARROW_ASSIGN_OR_RAISE(auto b, rdr_->ReadRecordBatch(i));
        batch_first_row_.push_back(total_rows_);
        total_rows_ += b->num_rows();
        batches_.push_back(std::move(b));
        if ((int)batches_.size() >= num_record_batches_) all_read_ = true;
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
            // Arrow IPC: open the footer only — batches decoded lazily by ensure().
            auto maybe_rdr = arrow::ipc::RecordBatchFileReader::Open(file);
            if (!maybe_rdr.ok())
                return "Not a valid Arrow IPC file: " + maybe_rdr.status().ToString();
            self->rdr_                 = maybe_rdr.ValueOrDie();
            self->schema_              = self->rdr_->schema();
            self->num_record_batches_  = self->rdr_->num_record_batches();
            if (self->num_record_batches_ == 0) self->all_read_ = true;
        }

        if (self->batches_.empty() && self->all_read_) {
            // Empty file (Feather with 0 rows, or 0-batch IPC): seed a zero-row
            // batch so the schema can still render.
            self->batch_first_row_.push_back(0);
            self->batches_.push_back(arrow::RecordBatch::Make(
                self->schema_, 0, std::vector<std::shared_ptr<arrow::Array>>(
                    self->schema_->num_fields(),
                    arrow::MakeArrayOfNull(arrow::utf8(), 0).ValueOrDie())));
        }

        *out = std::move(self);
        return "";
    }

public:
    std::shared_ptr<arrow::Schema> schema()    const override { return schema_; }
    int64_t total_rows()                        const override {
        return all_read_ ? total_rows_ : -1;
    }
    int     num_chunks()                        const override {
        return is_feather_ ? (int)batches_.size() : num_record_batches_;
    }
    ChunkMeta chunk_meta(int i)                 const override {
        if (!is_feather_ && i >= (int)batches_.size())
            const_cast<IpcSource*>(this)->ensure(i);
        return {batch_first_row_[i], batches_[i]->num_rows()};
    }
    void ensure(int i) override {
        if (is_feather_) return;
        while (!all_read_ && (int)batches_.size() <= i)
            (void)load_next_ipc();
    }
    const std::string& path()                   const override { return path_; }
    std::string footer()                        const override {
        return std::string("Format: ") + (is_feather_ ? "Feather v2" : "Arrow IPC");
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
};

// ── Format detection + source factory ────────────────────────────────────────
// (fends is defined above, near the preamble helpers)

// Open any supported file.  Returns empty string on success; error message otherwise.
// Read up to `n` bytes from stdin into a buffer. Used to sniff format magic
// bytes and the first line for delimiter detection.
static std::string sniff_stdin(size_t n, std::string* out_buf) {
    out_buf->clear();
    out_buf->resize(n);
    size_t total = 0;
    while (total < n) {
        ssize_t got = ::read(STDIN_FILENO, out_buf->data() + total, n - total);
        if (got == 0) break;            // EOF
        if (got < 0) {
            if (errno == EINTR) continue;
            return std::string("stdin read error: ") + std::strerror(errno);
        }
        total += (size_t)got;
    }
    out_buf->resize(total);
    return "";
}

static std::string open_source(const std::string& path, const Config& cfg,
                                std::unique_ptr<TabularSource>* out) {
    // ── Determine file kind ──────────────────────────────────────────────────
    bool        is_parquet = false;
    DelimKind   dk         = DelimKind::TSV;

    // ── Stdin (`-`): text formats only ───────────────────────────────────────
    if (path == "-") {
        if (isatty(STDIN_FILENO))
            return "Refusing to read from a terminal on stdin. "
                   "Did you mean to pipe data in (`cat foo.tsv | vv -`)?";

        std::string sniff;
        std::string err = sniff_stdin(8, &sniff);
        if (!err.empty()) return err;

        // Reject binary formats that need a seekable file.
        auto starts_with = [&](const char* m, size_t l) {
            return sniff.size() >= l && std::memcmp(sniff.data(), m, l) == 0;
        };
        if (starts_with("PAR1", 4))
            return "Parquet requires a seekable file; pipe to a temp file or use "
                   "process substitution: `vv <(zcat foo.parquet.gz)`";
        if (starts_with("ARROW1\0\0", 8))
            return "Arrow IPC requires a seekable file; pipe to a temp file or use "
                   "process substitution: `vv <(zcat foo.arrow.gz)`";
        if (starts_with("FEA1", 4))
            return "Feather requires a seekable file; pipe to a temp file or use "
                   "process substitution: `vv <(zcat foo.feather.gz)`";
        // BAM and BCF both share the BGZF magic 1f 8b 08 04 — treat them as
        // binary too (we have no way to tell BAM/BCF apart from a gzip stream
        // without seeking).
        if (starts_with("\x1f\x8b\x08\x04", 4))
            return "BAM/BCF/CRAM require seekable input; pipe to a temp file or use "
                   "process substitution: `vv <(samtools view -b foo.sam)`";

        // Wrap stdin as a sequential-only InputStream that reads via read(2).
        // Avoid arrow::io::StdinStream — it uses std::cin which conflicts
        // with the preceding raw read(2) sniff.
        std::shared_ptr<arrow::io::InputStream> input =
            std::make_shared<FdInputStream>(STDIN_FILENO);

        bool is_gz = starts_with("\x1f\x8b", 2);   // plain gzip → wrap
        // Reattach the sniffed bytes BEFORE the gzip wrapper sees the stream.
        input = std::make_shared<PrependInputStream>(std::move(sniff), input);
        if (is_gz) {
            auto codec = arrow::util::Codec::Create(arrow::Compression::GZIP);
            if (!codec.ok()) return codec.status().ToString();
            auto ci = arrow::io::CompressedInputStream::Make(codec->get(), input);
            if (!ci.ok()) return ci.status().ToString();
            input = ci.ValueOrDie();
        }

        // Choose CSV/TSV from the user-provided flag (none → assume TSV; the
        // header-line auto-detect in DelimitedSource will catch obvious CSV).
        DelimKind kind = DelimKind::TSV;
        if (cfg.delimiter == ',') kind = DelimKind::CSV;
        std::unique_ptr<DelimitedSource> src;
        std::string e = DelimitedSource::open_from_stream(
            std::move(input), "-", kind, /*is_gz=*/is_gz, cfg.region, &src);
        if (!e.empty()) return e;
        *out = std::move(src);
        return "";
    }

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
        std::string err = BamSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends(path, ".bcf")) {
        std::unique_ptr<BcfSource> src;
        std::string err = BcfSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends(path, ".bb") || fends(path, ".bigBed") ||
               fends(path, ".bw") || fends(path, ".bigWig")) {
        std::unique_ptr<BigSource> src;
        std::string err = BigSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends(path, ".fa")    || fends(path, ".fa.gz")    ||
               fends(path, ".fasta") || fends(path, ".fasta.gz") ||
               fends(path, ".fna")   || fends(path, ".fna.gz")   ||
               fends(path, ".faa")   || fends(path, ".faa.gz")   ||
               fends(path, ".ffn")   || fends(path, ".ffn.gz")   ||
               fends(path, ".frn")   || fends(path, ".frn.gz")) {
        std::unique_ptr<FastxSource> src;
        std::string err = FastxSource::open(path, /*is_fastq=*/false, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends(path, ".fq")    || fends(path, ".fq.gz")    ||
               fends(path, ".fastq") || fends(path, ".fastq.gz")) {
        std::unique_ptr<FastxSource> src;
        std::string err = FastxSource::open(path, /*is_fastq=*/true, cfg, &src);
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
    } else if (fends(path, ".paf") || fends(path, ".paf.gz")) {
        dk = DelimKind::PAF;
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
    std::string err = DelimitedSource::open(path, dk, cfg.region, &src);
    if (!err.empty()) return err;
    *out = std::move(src);
    return "";
}

// ── Delimited output ──────────────────────────────────────────────────────────
// (write_csv_field is defined above)

static void write_delimited(TabularSource& src, const Config& cfg) {
    char sep = cfg.delimiter;
    // In delimiter mode default to all rows; honour -n if explicitly given.
    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;

    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) {
        std::string u;
        for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        std::fprintf(stderr, "unknown column(s) in --select: %s\n", u.c_str());
        return;
    }

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr)) {
            std::fprintf(stderr, "--filter: %s\n", ferr.c_str());
            return;
        }
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
            if (ci) std::putchar(sep);
            write_csv_field(src.schema()->field(requested[ci])->name(), sep);
        }
        std::putchar('\n');
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
                if (ci) std::putchar(sep);
                auto& cur = cursors[ci];
                std::string val = cell_to_string(cur.current(), cur.row_in_chunk);
                if (val != NULL_SYMBOL) write_csv_field(val, sep);
                cur.advance();
            }
            std::putchar('\n');
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
            return;
        }
        // Fall through to streaming path on error.
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

        int64_t rg_rows = std::min(table->num_rows(), rows_left);
        rows_left -= rg_rows;

        print_rows(*table, rg_rows);
    }
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

static void write_markdown(TabularSource& src, const Config& cfg) {
    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;

    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) {
        std::string u;
        for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        std::fprintf(stderr, "unknown column(s) in --select: %s\n", u.c_str());
        return;
    }

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *src.schema(), &fx, &ferr)) {
            std::fprintf(stderr, "--filter: %s\n", ferr.c_str());
            return;
        }
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
            return;
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
}

// ── In-memory adapter: wrap an Arrow Table as a TabularSource ────────────────
//
// Used by `--sample` (and potentially future row-selecting flags) to feed
// a pre-computed Table through the normal rendering / export pipeline as
// if it had been read from a file.
class MemoryTableSource : public TabularSource {
    std::shared_ptr<arrow::Table>    table_;
    std::string                       label_;
    std::string                       footer_str_;
    std::vector<std::string>          hidden_;
public:
    MemoryTableSource(std::shared_ptr<arrow::Table> t,
                       std::string label, std::string footer,
                       std::vector<std::string> hidden = {})
        : table_(std::move(t)),
          label_(std::move(label)),
          footer_str_(std::move(footer)),
          hidden_(std::move(hidden)) {}

    std::shared_ptr<arrow::Schema> schema() const override { return table_->schema(); }
    int64_t total_rows() const override { return table_->num_rows(); }
    int     num_chunks() const override { return 1; }
    ChunkMeta chunk_meta(int) const override {
        return {0, table_->num_rows()};
    }
    arrow::Status read_chunk(int /*i*/, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        arrow::FieldVector fields;
        for (int c : col_indices) {
            cols.push_back(table_->column(c));
            fields.push_back(table_->schema()->field(c));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols,
                                  table_->num_rows());
        return arrow::Status::OK();
    }
    const std::string& path() const override { return label_; }
    std::string footer() const override { return footer_str_; }
    std::vector<std::string> hidden_for_display() const override { return hidden_; }
};

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

// Stream the source's chunks into a Parquet file at cfg.parquet_out.
// Returns "" on success, an error message otherwise. Writes an
// "[N rows -> path]" summary to stderr on success.
static std::string write_parquet(TabularSource& src, const Config& cfg) {
    arrow::Compression::type codec;
    if (!resolve_compression(cfg.compression, &codec))
        return "Unknown --compression '" + cfg.compression +
               "' (try: zstd, snappy, gzip, lz4, none)";

    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) {
        std::string u;
        for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        return "unknown column(s) in --select: " + u;
    }
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
        char tmpl[] = "/tmp/vv-parquet-XXXXXX.parquet";
        tmp_fd = mkstemps(tmpl, 8);  // suffix length = ".parquet" = 8
        if (tmp_fd < 0)
            return std::string("Cannot create temp file for --parquet -: ") +
                   std::strerror(errno);
        out_path = tmpl;
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
    } else {
        std::fprintf(stderr, "%s[%lld rows → %s, %s]%s\n",
                     g_color.meta_key, (long long)total,
                     cfg.parquet_out.c_str(), cfg.compression.c_str(),
                     g_color.reset);
    }
    return "";
}

// ── JSON / JSON-Lines output ─────────────────────────────────────────────────

// Quote a string as a JSON string literal.
static void json_emit_string(const std::string& v) {
    std::putchar('"');
    for (unsigned char c : v) {
        switch (c) {
            case '"':  std::printf("\\\""); break;
            case '\\': std::printf("\\\\"); break;
            case '\b': std::printf("\\b");  break;
            case '\f': std::printf("\\f");  break;
            case '\n': std::printf("\\n");  break;
            case '\r': std::printf("\\r");  break;
            case '\t': std::printf("\\t");  break;
            default:
                if (c < 0x20) std::printf("\\u%04x", c);
                else          std::putchar((int)c);
        }
    }
    std::putchar('"');
}

// Emit one Arrow cell as a JSON value. Numbers go bare, strings are
// quoted, booleans as true/false, nulls as null. List/struct/map fall
// back to their Python-style cell_to_string rendering wrapped in a
// JSON string so the output stays parseable (good-enough v1; structured
// nesting is a follow-up).
static void json_emit_cell(const arrow::Array& arr, int64_t row) {
    if (arr.IsNull(row)) { std::printf("null"); return; }
    switch (arr.type_id()) {
        case arrow::Type::BOOL:
            std::printf(static_cast<const arrow::BooleanArray&>(arr).Value(row)
                        ? "true" : "false");
            return;
        case arrow::Type::INT8: case arrow::Type::INT16: case arrow::Type::INT32:
        case arrow::Type::INT64: case arrow::Type::UINT8: case arrow::Type::UINT16:
        case arrow::Type::UINT32: case arrow::Type::UINT64:
        case arrow::Type::FLOAT: case arrow::Type::DOUBLE:
            // cell_to_string already produces a decimal representation
            // suitable for JSON for these types.
            std::printf("%s", cell_to_string(arr, row).c_str());
            return;
        case arrow::Type::STRING: case arrow::Type::LARGE_STRING:
            json_emit_string(cell_to_string(arr, row));
            return;
        default:
            // Nested or unsupported types — emit their canonical string form.
            json_emit_string(cell_to_string(arr, row));
            return;
    }
}

static std::string write_json(TabularSource& src, const Config& cfg) {
    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown, true);
    if (!unknown.empty()) {
        std::string u; for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        return "unknown column(s) in --select: " + u;
    }
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

    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;
    bool first_row = true;
    if (cfg.json_array) std::putchar('[');

    auto emit_row = [&](const arrow::Table& tbl, int64_t r) {
        if (!first_row) std::printf(cfg.json_array ? ",\n" : "\n");
        first_row = false;
        std::putchar('{');
        for (size_t k = 0; k < requested.size(); ++k) {
            if (k) std::printf(", ");
            json_emit_string(src.schema()->field(requested[k])->name());
            std::printf(": ");
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
        std::putchar('}');
    };

    for (int c = 0; rows_left > 0; ++c) {
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> chunk;
        if (!src.read_chunk(c, read_set, &chunk).ok()) continue;
        if (have_filter) chunk = apply_filter(chunk, fx, read_set);
        if (!chunk || chunk->num_rows() == 0) continue;
        int64_t take = std::min(chunk->num_rows(), rows_left);
        for (int64_t r = 0; r < take; ++r) emit_row(*chunk, r);
        rows_left -= take;
    }
    if (cfg.json_array) std::printf("]\n");
    else if (!first_row) std::putchar('\n');
    return "";
}

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
static std::string validate_lociss(const std::string& path) {
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
    if (!is_string_like(*schema->field(j_chr)->type()))
        fail("Chromosome must be string or dict<string>");
    else                                                pass("Chromosome column is string-like");
    if (!is_int_like(*schema->field(j_st)->type()))    fail("Start must be integer");
    else                                                pass("Start column is integer");
    if (!is_int_like(*schema->field(j_en)->type()))    fail("End must be integer");
    else                                                pass("End column is integer");
    if (!is_int_like(*schema->field(j_mes)->type()))   fail("MaxEndSoFar must be integer");
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
    st = reader->GetRecordBatchReader(all_rgs, leaf_cols, &rb);
    if (!st.ok()) {
        fail("cannot read columns: " + st.ToString());
        return "validation failed: read error";
    }

    int64_t row = 0;
    int64_t chrom_first_row = 0;
    size_t  manifest_idx = 0;
    int64_t prev_start = INT64_MIN, prev_end = INT64_MIN;
    int64_t running_max_end = INT64_MIN;
    std::string prev_chrom;
    int   sort_failures = 0, mes_failures = 0, chrom_failures = 0;
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
        for (int64_t r = 0; r < batch->num_rows(); ++r, ++row) {
            std::string this_chrom = cell_to_string(*chr_col, r);
            int64_t this_start = std::stoll(cell_to_string(*st_col,  r));
            int64_t this_end   = std::stoll(cell_to_string(*en_col,  r));
            int64_t this_mes   = std::stoll(cell_to_string(*mes_col, r));

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

    std::printf("\n%d check(s) passed, %d failed\n", n_pass, n_fail);
    std::fflush(stdout);
    if (n_fail > 0) return "validation failed (" + std::to_string(n_fail) + " checks)";
    return "";
}

// ── --describe: per-column statistics ────────────────────────────────────────

// Print "Column | Type | Count | Nulls | Min | Max | Mean | Distinct".
// `Mean` only filled for numeric columns; `Distinct` only when small.
static std::string print_describe(TabularSource& src, const Config& cfg) {
    std::vector<std::string> unknown;
    std::vector<int> requested = select_field_indices(src, cfg, &unknown);
    if (!unknown.empty()) {
        std::string u; for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        return "unknown column(s) in --select: " + u;
    }

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
        bool         is_num = false;
        int64_t      count  = 0;
        int64_t      nulls  = 0;
        double       d_min  = std::numeric_limits<double>::infinity();
        double       d_max  = -std::numeric_limits<double>::infinity();
        long double  sum    = 0.0L;
        std::string  s_min, s_max;
        std::set<std::string> distinct;     // capped at 16
        bool         distinct_overflow = false;
    };
    std::vector<ColStat> stats(requested.size());
    for (size_t k = 0; k < requested.size(); ++k) {
        auto f = src.schema()->field(requested[k]);
        stats[k].name   = f->name();
        stats[k].type   = f->type()->ToString();
        stats[k].is_num = is_numeric_type(f->type()->id());
    }

    int64_t rows_left = (cfg.head_rows <= 0) ? INT64_MAX : (int64_t)cfg.head_rows;
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
                        if (auto a = std::dynamic_pointer_cast<arrow::DoubleArray>(ch)) d = a->Value(r);
                        else if (auto a = std::dynamic_pointer_cast<arrow::FloatArray>(ch))  d = a->Value(r);
                        else if (auto a = std::dynamic_pointer_cast<arrow::Int64Array>(ch))  d = (double)a->Value(r);
                        else if (auto a = std::dynamic_pointer_cast<arrow::Int32Array>(ch))  d = (double)a->Value(r);
                        else if (auto a = std::dynamic_pointer_cast<arrow::Int16Array>(ch))  d = (double)a->Value(r);
                        else if (auto a = std::dynamic_pointer_cast<arrow::Int8Array>(ch))   d = (double)a->Value(r);
                        else if (auto a = std::dynamic_pointer_cast<arrow::UInt32Array>(ch)) d = (double)a->Value(r);
                        else continue;
                        if (d < cs.d_min) cs.d_min = d;
                        if (d > cs.d_max) cs.d_max = d;
                        cs.sum += d;
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

    // Pretty-print
    auto fmt_num = [](double v) -> std::string {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        return std::string(buf);
    };
    auto width = [](const std::string& s) { return (int)display_width(s); };

    int wN = 6, wT = 4, wC = 5, wL = 5, wMin = 3, wMax = 3, wMean = 4, wD = 8;
    std::vector<std::array<std::string,7>> rows;  // name, type, count, nulls, min, max, mean
    std::vector<std::string> distincts;
    for (auto& cs : stats) {
        std::string mn, mx, me;
        if (cs.count == 0) {
            mn = "-"; mx = "-"; me = "-";
        } else if (cs.is_num) {
            mn = fmt_num(cs.d_min);
            mx = fmt_num(cs.d_max);
            me = fmt_num((double)(cs.sum / (long double)cs.count));
        } else {
            mn = cs.s_min; mx = cs.s_max; me = "";
        }
        std::string dn = cs.is_num ? ""
                       : (cs.distinct_overflow ? ">16"
                         : std::to_string(cs.distinct.size()));
        std::string sc = std::to_string(cs.count);
        std::string sn = std::to_string(cs.nulls);
        wN = std::max(wN, width(cs.name));
        wT = std::max(wT, width(cs.type));
        wC = std::max(wC, width(sc));
        wL = std::max(wL, width(sn));
        wMin = std::max(wMin, width(mn));
        wMax = std::max(wMax, width(mx));
        wMean = std::max(wMean, width(me));
        wD = std::max(wD, width(dn));
        rows.push_back({cs.name, cs.type, sc, sn, mn, mx, me});
        distincts.push_back(dn);
    }
    auto trim_col = [](int& w) { if (w > 40) w = 40; };
    trim_col(wN); trim_col(wT); trim_col(wMin); trim_col(wMax); trim_col(wMean);

    std::printf("%s%-*s  %-*s  %*s  %*s  %-*s  %-*s  %-*s  %-*s%s\n",
                g_color.header,
                wN,    "Column", wT, "Type", wC, "Count", wL, "Nulls",
                wMin,  "Min",    wMax, "Max", wMean, "Mean", wD, "Distinct",
                g_color.reset);
    std::printf("%s%s  %s  %s  %s  %s  %s  %s  %s%s\n",
                g_color.border,
                std::string(wN,'-').c_str(), std::string(wT,'-').c_str(),
                std::string(wC,'-').c_str(), std::string(wL,'-').c_str(),
                std::string(wMin,'-').c_str(), std::string(wMax,'-').c_str(),
                std::string(wMean,'-').c_str(), std::string(wD,'-').c_str(),
                g_color.reset);
    for (size_t k = 0; k < rows.size(); ++k) {
        auto& r = rows[k];
        std::printf("%-*s  %-*s  %*s  %*s  %-*s  %-*s  %-*s  %-*s\n",
                    wN, truncate(r[0], wN).c_str(),
                    wT, truncate(r[1], wT).c_str(),
                    wC, r[2].c_str(), wL, r[3].c_str(),
                    wMin,  truncate(r[4], wMin).c_str(),
                    wMax,  truncate(r[5], wMax).c_str(),
                    wMean, r[6].c_str(),
                    wD,    distincts[k].c_str());
    }
    return "";
}

// Forward decl (definition lives alongside print_table at the bottom of
// this file).
static void print_schema_block(TabularSource& src);

// ── --stats: Parquet metadata footer dump ────────────────────────────────────
//
// Prints what was written into the Parquet file (row groups, codecs, per-
// column sizes, statistics) without decoding any data. For non-Parquet
// sources, prints the schema block and a note that detailed stats are
// Parquet-only.
static std::string print_stats_only(TabularSource& src, const Config& /*cfg*/) {
    auto* pq = dynamic_cast<ParquetSource*>(&src);
    if (!pq) {
        print_schema_block(src);
        std::printf("\n%sNote:%s detailed per-column statistics are "
                    "Parquet-only; this file is a %s source.\n",
                    g_color.meta_key, g_color.reset,
                    src.footer().c_str());
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

    // Per-column table
    std::vector<std::array<std::string, 6>> rows;     // name, type, codec, comp, raw, ratio
    std::vector<std::string>                 nulls_col;
    int wN = 6, wT = 4, wK = 5, wC = 10, wR = 12, wRatio = 5, wNulls = 5;
    for (int i = 0; i < n_cols; ++i) {
        auto f = schema->field(i);
        std::string codec;
        for (auto c : agg[i].codecs) {
            if (!codec.empty()) codec += "+";
            codec += codec_name(c);
        }
        if (codec.empty()) codec = "?";
        std::string ratio = (agg[i].comp > 0)
            ? (std::to_string((double)agg[i].raw / (double)agg[i].comp).substr(0, 5) + "x")
            : "-";
        std::string nulls = agg[i].has_nulls
            ? digits_with_sep(std::to_string(agg[i].nulls))
            : "?";
        rows.push_back({
            f->name(), f->type()->ToString(), codec,
            fmt_size(agg[i].comp), fmt_size(agg[i].raw), ratio
        });
        nulls_col.push_back(nulls);
        wN = std::max(wN, (int)display_width(f->name()));
        wT = std::max(wT, (int)display_width(f->type()->ToString()));
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
    return "";
}

// ── --unique: distinct value counts per column ───────────────────────────────
static std::string print_unique(TabularSource& src, const Config& cfg) {
    if (cfg.unique_cols.empty()) return "--unique needs a comma-separated column list";
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
        return "unknown column(s) in --unique: " + u;
    }
    if (cols.empty()) return "--unique: no columns specified";

    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr))
            return "--filter: " + ferr;
        have_filter = true;
    }
    std::vector<int> read_set = have_filter ? union_with_filter(cols, fx) : cols;

    // Counts per column.
    std::vector<std::map<std::string, int64_t>> counts(cols.size());
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
                    std::string v = ch->IsNull(r) ? "(null)" : cell_to_string(*ch, r);
                    counts[k][v]++;
                }
            }
        }
    }

    // Output per column: top N entries sorted by count desc.
    constexpr int kTop = 50;
    bool first = true;
    for (size_t k = 0; k < cols.size(); ++k) {
        if (!first) std::printf("\n");
        first = false;
        auto& m = counts[k];
        std::vector<std::pair<std::string,int64_t>> entries(m.begin(), m.end());
        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("%s%s%s — %s%zu%s distinct value(s) (of %s%lld%s)\n",
                    g_color.header, schema->field(cols[k])->name().c_str(),
                    g_color.reset,
                    g_color.number, entries.size(), g_color.reset,
                    g_color.number, (long long)total, g_color.reset);
        int wV = 5, wC2 = 5;
        size_t n_show = std::min((size_t)kTop, entries.size());
        for (size_t i = 0; i < n_show; ++i) {
            wV  = std::max(wV,  (int)display_width(entries[i].first));
            wC2 = std::max(wC2, (int)display_width(
                digits_with_sep(std::to_string(entries[i].second))));
        }
        if (wV > 50) wV = 50;
        for (size_t i = 0; i < n_show; ++i) {
            std::printf("  %-*s  %*s\n",
                        wV,  truncate(entries[i].first, wV).c_str(),
                        wC2, digits_with_sep(std::to_string(entries[i].second)).c_str());
        }
        if (entries.size() > n_show)
            std::printf("  %s... %zu more distinct value(s)%s\n",
                        g_color.meta_key, entries.size() - n_show, g_color.reset);
    }
    return "";
}

// ── --sample N: reservoir sample N rows uniformly ────────────────────────────
//
// Reads (and optionally filters) the entire source into memory, then picks
// N rows uniformly without replacement via reservoir sampling. The result
// is wrapped as a MemoryTableSource so the normal output path renders it.
static std::string build_sample(std::unique_ptr<TabularSource>& src,
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
static std::string build_tail(std::unique_ptr<TabularSource>& src,
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

// ── Interactive TUI viewer (ncurses) ─────────────────────────────────────────

// ncurses color-pair IDs  (0 = terminal default)
enum : int {
    NCP_HEADER = 1,   // column header row
    NCP_INDEX,        // row-index column
    NCP_NULL,         // null value
    NCP_NUMBER,       // numeric / temporal value
    NCP_BOOL_T,       // true
    NCP_BOOL_F,       // false
    NCP_SEP,          // separator line
    NCP_SEARCH,       // search-match highlight row
    NCP_PLAIN,        // default-fg text (used as zebra-twin base)
};

// Each of the above pairs has an optional zebra twin at pair + ZEBRA_OFFSET,
// identical fg but with a dim grey background — applied to odd data rows.
static constexpr int ZEBRA_OFFSET = 100;

static void nc_str(int y, int x, const std::string& s,
                   attr_t attrs = A_NORMAL, int cp = 0) {
    attr_t full = attrs | (cp ? (attr_t)COLOR_PAIR(cp) : 0);
    if (full != A_NORMAL) attron(full);
    mvaddstr(y, x, s.c_str());
    if (full != A_NORMAL) attroff(full);
}

// Per-chunk cache.  Columns are loaded lazily (null until fetched) so a
// horizontal viewport only pays for the source columns currently on screen.
// Strings are rendered on demand in the draw loop; we never materialize an
// NxM grid of strings for a million-row row-group.
struct CachedRG {
    int64_t first_row = 0, num_rows = 0;
    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    bool ok = false;  // false if read_chunk failed
};

// Parse a VCF-INFO / GFF-attributes style key=value list. Handles "k=v;k=v"
// (VCF/GFF3) and 'k "v"; k "v";' (GTF). Bare tokens become flags with empty value.
static std::vector<std::pair<std::string,std::string>>
parse_kv_list(const std::string& s) {
    std::vector<std::pair<std::string,std::string>> out;
    auto is_sp = [](char c){ return c == ' ' || c == '\t'; };
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (is_sp(s[i]) || s[i] == ';')) ++i;
        if (i >= s.size()) break;
        size_t ks = i;
        while (i < s.size() && s[i] != '=' && s[i] != ';' && !is_sp(s[i])) ++i;
        std::string key = s.substr(ks, i - ks);
        if (key.empty()) { ++i; continue; }
        while (i < s.size() && (s[i] == '=' || is_sp(s[i]))) ++i;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            ++i;
            size_t vs = i;
            while (i < s.size() && s[i] != '"') ++i;
            value = s.substr(vs, i - vs);
            if (i < s.size()) ++i;
        } else if (i < s.size() && s[i] != ';') {
            size_t vs = i;
            while (i < s.size() && s[i] != ';') ++i;
            value = s.substr(vs, i - vs);
            while (!value.empty() && is_sp(value.back())) value.pop_back();
        }
        out.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

// Heuristic: does this cell look like a k=v;k=v list worth expanding?
static bool looks_like_kv_list(const std::string& s) {
    return s.find(';') != std::string::npos &&
           (s.find('=') != std::string::npos || s.find('"') != std::string::npos);
}

// Parse VCF ##INFO=<ID=X,Number=...,Type=T,Description="..."> header lines.
// Returns (ID, Arrow type) pairs in file order. Type maps VCF types to Arrow:
//   Integer → INT64, Float → DOUBLE, Flag → BOOL, everything else → STRING.
static std::vector<std::pair<std::string, arrow::Type::type>>
parse_vcf_info_headers(const std::vector<std::string>& preamble) {
    std::vector<std::pair<std::string, arrow::Type::type>> out;
    const std::string prefix = "##INFO=<";
    for (auto& line : preamble) {
        if (line.rfind(prefix, 0) != 0 || line.empty() || line.back() != '>') continue;
        std::string body = line.substr(prefix.size(), line.size() - prefix.size() - 1);
        std::string id, type;
        size_t i = 0, n = body.size();
        while (i < n) {
            size_t ke = body.find('=', i);
            if (ke == std::string::npos) break;
            std::string k = body.substr(i, ke - i);
            size_t vs = ke + 1, ve;
            std::string v;
            if (vs < n && body[vs] == '"') {
                ve = body.find('"', vs + 1);
                if (ve == std::string::npos) break;
                v = body.substr(vs + 1, ve - vs - 1);
                i = (ve + 1 < n) ? ve + 2 : n;  // skip closing quote + comma
            } else {
                ve = body.find(',', vs);
                if (ve == std::string::npos) ve = n;
                v = body.substr(vs, ve - vs);
                i = (ve < n) ? ve + 1 : n;
            }
            if (k == "ID")   id = v;
            if (k == "Type") type = v;
        }
        if (id.empty()) continue;
        arrow::Type::type t = arrow::Type::STRING;
        if      (type == "Integer") t = arrow::Type::INT64;
        else if (type == "Float")   t = arrow::Type::DOUBLE;
        else if (type == "Flag")    t = arrow::Type::BOOL;
        out.emplace_back(std::move(id), t);
    }
    return out;
}

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
    std::vector<bool>        is_integer_;   // integer-typed source column (not INFO expansion)
    int                      idx_w_ = 1;

    // Virtual→source column mapping. For a plain source, these are 1:1.
    // For VCF with expanded INFO, each declared INFO key becomes its own
    // virtual column that reads from the underlying INFO source column.
    int                      src_num_cols_ = 0;   // count of source columns
    std::vector<int>         virt_src_col_;       // virt col → source col
    std::vector<std::string> virt_info_key_;      // virt col → INFO key ("" if none)

    // Dynamic color-pair allocation for RGB cells (pair range chosen in setup_colors).
    std::unordered_map<int,int> rgb_pair_;   // packed 0xRRGGBB → ncurses pair number
    int                         next_rgb_pair_ = NCP_PLAIN + 1;
    bool                        zebra_enabled_ = false;

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
    bool    freeze_first_col_ = false;   // toggle with `z` — keep col 0 pinned left
    bool    help_open_        = false;   // overlay shown via `?` / F1 / H

    // ── Search state ─────────────────────────────────────────────────────────
    enum class SearchMode { None, Input, Active };
    SearchMode  search_mode_  = SearchMode::None;
    std::string search_input_;   // text being typed in the search bar
    std::string search_query_;   // committed query (empty = no active search)
    int64_t     search_row_   = -1;   // row of the focused match (-1 = none)
    bool        search_wrap_  = false;  // last search wrapped around
    bool        search_fail_  = false;  // last search found nothing
    bool        search_dir_forward_ = true;  // direction of the last `/` or `?`
    std::optional<std::regex> search_regex_;       // ECMAScript icase, valid only if…
    bool        search_regex_valid_ = false;       // …regex compiled successfully

    // Compile the active search query as a case-insensitive ECMAScript regex.
    // Falls back to literal substring matching if the pattern is invalid.
    void compile_search() {
        search_regex_.reset();
        search_regex_valid_ = false;
        if (search_query_.empty()) return;
        try {
            search_regex_.emplace(search_query_,
                std::regex_constants::ECMAScript |
                std::regex_constants::icase      |
                std::regex_constants::optimize);
            search_regex_valid_ = true;
        } catch (const std::regex_error&) {
            // Leave search_regex_ unset — find_next() falls back to literal.
        }
    }

    // True if `val` matches the active search (regex or literal substring).
    bool cell_matches(const std::string& val,
                      const std::string& lowered_query) const {
        if (search_regex_valid_) return std::regex_search(val, *search_regex_);
        std::string lo = val;
        for (auto& c : lo) c = (char)std::tolower((unsigned char)c);
        return lo.find(lowered_query) != std::string::npos;
    }

    // ── Detail pane state ────────────────────────────────────────────────────
    int64_t     detail_row_   = -1;  // -1 = pane closed
    int         detail_scroll_ = 0;  // vertical scroll offset within the pane

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
        for (int i = 0; i < src_num_cols_; ++i) all_cols.push_back(i);

        int nc = src_.num_chunks();

        // Check one row: returns true if any column matches the active search.
        auto row_matches = [&](const std::shared_ptr<arrow::Table>& tbl, int64_t local) -> bool {
            for (int col = 0; col < src_num_cols_ && col < tbl->num_columns(); ++col) {
                int64_t off = local;
                for (auto& arr : tbl->column(col)->chunks()) {
                    if (off < arr->length()) {
                        if (cell_matches(cell_to_string(*arr, off), q)) return true;
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

    // True if `row` matches the active search, *without* loading new chunks.
    // Used by draw_data_row to highlight every visible match — only consults
    // already-cached cells. Returns false if the row's chunk isn't loaded.
    bool row_matches_search(int64_t row) const {
        if (search_mode_ != SearchMode::Active || search_query_.empty()) return false;
        if (src_.num_chunks() == 0) return false;
        int c = chunk_for_row(row);
        auto it = cache_.find(c);
        if (it == cache_.end() || !it->second.ok) return false;
        const CachedRG& cr = it->second;
        int64_t local = row - cr.first_row;
        if (local < 0 || local >= cr.num_rows) return false;
        std::string q = search_query_;
        for (auto& c2 : q) c2 = (char)std::tolower((unsigned char)c2);
        for (int col = 0; col < (int)cr.cols.size(); ++col) {
            auto arr = cr.cols[col];
            if (!arr) continue;
            int64_t off = local;
            for (auto& chunk : arr->chunks()) {
                if (off < chunk->length()) {
                    if (cell_matches(cell_to_string(*chunk, off), q)) return true;
                    break;
                }
                off -= chunk->length();
            }
        }
        return false;
    }

    // ── Cache ────────────────────────────────────────────────────────────────

    // Ensure cache entry for chunk `c` exists and has all requested source
    // columns loaded.  A single read_chunk() call fetches whatever's missing.
    void ensure_cols(int c, const std::vector<int>& src_cols) {
        auto it = cache_.find(c);
        if (it == cache_.end()) {
            if ((int)cache_.size() >= MAX_CACHE) {
                cache_.erase(lru_.back()); lru_.pop_back();
            }
            CachedRG cr;
            cr.first_row = src_.chunk_meta(c).first_row;
            cr.num_rows  = src_.chunk_meta(c).num_rows;
            cr.cols.assign(src_num_cols_, nullptr);
            it = cache_.emplace(c, std::move(cr)).first;
            lru_.push_front(c);
        } else {
            lru_.remove(c); lru_.push_front(c);
        }
        CachedRG& cr = it->second;

        std::vector<int> missing;
        for (int sc : src_cols)
            if (sc >= 0 && sc < src_num_cols_ && !cr.cols[sc]) missing.push_back(sc);
        if (missing.empty()) return;

        src_.ensure(c);
        std::shared_ptr<arrow::Table> tbl;
        if (!src_.read_chunk(c, missing, &tbl).ok()) return;
        cr.ok       = true;
        cr.num_rows = tbl->num_rows();  // may be smaller than chunk_meta for streaming sources
        for (size_t i = 0; i < missing.size() && (int)i < tbl->num_columns(); ++i) {
            int sc = missing[i];
            auto ca = tbl->column((int)i);
            cr.cols[sc] = ca;
        }
    }

    // Extract a single cell as a formatted string (respects VCF INFO expansion,
    // per-format format_cell(), and max_col_w_ truncation).  `parsed_cache`
    // memoizes the parsed INFO map for the current row across virtual columns.
    std::string cell_at(const CachedRG& cr, int64_t local,
                        int vc,
                        std::unordered_map<std::string, std::string>* parsed_cache,
                        int* parsed_row_slot, int64_t this_row_slot) const {
        int sc = virt_src_col_[vc];
        auto arr = (sc >= 0 && sc < (int)cr.cols.size()) ? cr.cols[sc] : nullptr;
        if (!arr) return "";
        int64_t off = local;
        for (auto& chunk : arr->chunks()) {
            if (off < chunk->length()) {
                std::string val;
                const std::string& key = virt_info_key_[vc];
                if (!key.empty()) {
                    std::string raw = cell_to_string(*chunk, off);  // INFO: raw VCF text
                    if (parsed_cache) {
                        if (*parsed_row_slot != (int)this_row_slot) {
                            parsed_cache->clear();
                            for (auto& kv : parse_kv_list(raw))
                                parsed_cache->emplace(std::move(kv.first),
                                                      std::move(kv.second));
                            *parsed_row_slot = (int)this_row_slot;
                        }
                        auto fit = parsed_cache->find(key);
                        val = (fit != parsed_cache->end())
                                ? (fit->second.empty() ? "true" : fit->second)
                                : NULL_SYMBOL;
                    } else {
                        std::unordered_map<std::string, std::string> m;
                        for (auto& kv : parse_kv_list(raw))
                            m.emplace(std::move(kv.first), std::move(kv.second));
                        auto fit = m.find(key);
                        val = (fit != m.end())
                                ? (fit->second.empty() ? "true" : fit->second)
                                : NULL_SYMBOL;
                    }
                } else {
                    val = cell_to_display_string(*chunk, off);
                }
                std::string formatted = src_.format_cell(sc, std::move(val));
                // Never truncate integer values — digits must stay readable.
                if (is_integer_type(chunk->type_id())) return formatted;
                return truncate(std::move(formatted), max_col_w_);
            }
            off -= chunk->length();
        }
        return "";
    }

    // Collect the unique source columns referenced by a set of virtual cols.
    std::vector<int> src_cols_for_virt(const std::vector<int>& virt_cols) const {
        std::vector<bool> seen(src_num_cols_, false);
        std::vector<int> out;
        for (int vc : virt_cols) {
            int sc = virt_src_col_[vc];
            if (sc >= 0 && sc < src_num_cols_ && !seen[sc]) {
                seen[sc] = true; out.push_back(sc);
            }
        }
        return out;
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

    // Prefetch the source columns the visible virtual columns need, for the
    // chunks that currently intersect the viewport.  Cheap when already cached.
    // Fit each integer virtual column's width to the rows currently on screen.
    // Called after prefetch so cached chunks are available; columns whose data
    // is not yet loaded keep their previous width.
    void fit_integer_widths_to_visible(const std::vector<int>& visible_virt_cols) {
        if (src_.num_chunks() == 0) return;
        int64_t bot = top_row_ + (int64_t)data_lines() - 1;
        if (total_rows() > 0) bot = std::min(bot, total_rows() - 1);
        if (bot < top_row_) return;
        for (int vc : visible_virt_cols) {
            if (vc < 0 || vc >= num_cols_ || !is_integer_[vc]) continue;
            int sc = virt_src_col_[vc];
            int w = (int)display_width(col_names_[vc]);
            for (int64_t r = top_row_; r <= bot; ++r) {
                int c = chunk_for_row(r);
                auto it = cache_.find(c);
                if (it == cache_.end() || !it->second.ok) continue;
                const CachedRG& cr = it->second;
                int64_t local = r - cr.first_row;
                if (local < 0 || local >= cr.num_rows) continue;
                auto arr = (sc >= 0 && sc < (int)cr.cols.size()) ? cr.cols[sc] : nullptr;
                if (!arr) continue;
                int64_t off = local;
                for (auto& chunk : arr->chunks()) {
                    if (off < chunk->length()) {
                        if (!chunk->IsNull(off)) {
                            int ww = display_width(
                                digits_with_sep(cell_to_string(*chunk, off)));
                            if (ww > w) w = ww;
                        }
                        break;
                    }
                    off -= chunk->length();
                }
            }
            col_widths_[vc] = w;
        }
    }

    void prefetch_visible(const std::vector<int>& visible_virt_cols) {
        if (src_.num_chunks() == 0) { src_.ensure(0); return; }
        std::vector<int> src_cols = src_cols_for_virt(visible_virt_cols);
        int top_chunk = chunk_for_row(top_row_);
        ensure_cols(top_chunk, src_cols);
        int64_t bot = top_row_ + (int64_t)data_lines() - 1;
        if (total_rows() > 0) bot = std::min(bot, total_rows() - 1);
        if (total_rows() < 0)
            src_.ensure(src_.num_chunks());
        else
            src_.ensure(chunk_for_row(std::max(bot, top_row_)));
        if (bot > top_row_) {
            int bot_chunk = chunk_for_row(bot);
            if (bot_chunk != top_chunk) ensure_cols(bot_chunk, src_cols);
        }
    }

    // ── Layout ───────────────────────────────────────────────────────────────

    struct ColVis { int col, x, w; };

    std::vector<ColVis> visible_cols() const {
        std::vector<ColVis> v;
        int x = no_index_ ? 0 : (idx_w_ + 2);
        // Frozen first column: pin col 0 at the left edge whenever the user
        // has scrolled past it. Skipped when col 0 is already in the natural
        // window (left_col_ == 0) — that path renders column 0 normally.
        int start = left_col_;
        if (freeze_first_col_ && num_cols_ > 0 && left_col_ > 0) {
            int w0 = col_widths_[0];
            if (x + w0 + 2 <= scr_c_) {
                v.push_back({0, x, w0});
                x += w0 + 2;
            }
            // Avoid showing the frozen column twice if scrolling places it
            // back into view (defensive — left_col_ > 0 means it's not).
            if (start == 0) start = 1;
        }
        for (int c = start; c < num_cols_; ++c) {
            if (freeze_first_col_ && c == 0) continue;   // already shown
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
            nc_str(1, 0, " " + repeat_utf8(BOX_HLINE, idx_w_) + " ", A_NORMAL, NCP_SEP);
        }
        for (auto& col : vc) {
            std::string nm = truncate(col_names_[col.col], col.w);
            nc_str(0, col.x, " " + fit(nm, col.w, right_align_[col.col]) + " ",
                   A_BOLD, NCP_HEADER);
            nc_str(1, col.x, " " + repeat_utf8(BOX_HLINE, col.w) + " ",
                   A_NORMAL, NCP_SEP);
        }
    }

    void draw_data_row(int sy, int64_t row, const std::vector<ColVis>& vc) {
        int64_t tr = total_rows();
        if (tr >= 0 && row >= tr) return;

        bool is_focused = (row == search_row_);                        // n/N target
        bool is_match   = is_focused || row_matches_search(row);       // any visible hit
        int  zo = (zebra_enabled_ && !is_match && ((row - top_row_) % 2 == 1))
                  ? ZEBRA_OFFSET : 0;
        auto zpair = [&](int cp) {
            return cp ? cp + zo : (zo ? NCP_PLAIN + zo : 0);
        };

        // Paint the row background first so gaps between cells pick up the zebra bg.
        if (zo) {
            int p = NCP_PLAIN + zo;
            attron(COLOR_PAIR(p));
            mvhline(sy, 0, ' ', scr_c_);
            attroff(COLOR_PAIR(p));
        }

        if (!no_index_) {
            std::string idx_s = " " + fit(digits_with_sep(std::to_string(row)), idx_w_, true) + " ";
            if (is_match) nc_str(sy, 0, idx_s,
                                 is_focused ? (attr_t)(A_BOLD | A_REVERSE) : A_NORMAL,
                                 NCP_SEARCH);
            else          nc_str(sy, 0, idx_s, A_NORMAL, NCP_INDEX + zo);
        }

        if (src_.num_chunks() == 0) return;
        int  c  = chunk_for_row(row);
        auto it = cache_.find(c);
        if (it == cache_.end() || !it->second.ok) return;

        int64_t local = row - it->second.first_row;
        // Guard: row may be beyond the loaded portion of the last chunk
        // (happens while streaming and the user scrolled ahead of loaded data).
        if (local < 0 || local >= it->second.num_rows) return;

        std::unordered_map<std::string, std::string> parsed;
        int parsed_row = -1;
        for (auto& col : vc) {
            std::string val = cell_at(it->second, local, col.col,
                                      &parsed, &parsed_row, local);

            if (is_match) {
                // Whole row rendered with NCP_SEARCH highlight; the n/N
                // focused row gets reverse video so it stands out among the
                // other visible matches.
                nc_str(sy, col.x, " " + fit(val, col.w, right_align_[col.col]) + " ",
                       is_focused ? (attr_t)(A_BOLD | A_REVERSE) : A_BOLD,
                       NCP_SEARCH);
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
                           zpair(val == NULL_SYMBOL ? NCP_NULL : 0));
                continue;
            }

            attr_t extra = A_NORMAL; int cp = 0;
            if (val == NULL_SYMBOL)         { extra = A_DIM; cp = NCP_NULL; }
            else if (is_bool_[col.col])     { cp = (val=="true")?NCP_BOOL_T:NCP_BOOL_F; }
            else if (right_align_[col.col]) { cp = NCP_NUMBER; }
            nc_str(sy, col.x, " " + fit(val, col.w, right_align_[col.col]) + " ",
                   extra, zpair(cp));
        }
    }

    void draw_status(const std::vector<ColVis>& vc) {
        // ── Search-input mode: show a vim-style search bar ───────────────────
        if (search_mode_ == SearchMode::Input) {
            char prefix = search_dir_forward_ ? '/' : '?';
            std::string bar = std::string(1, prefix) + search_input_;
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
        s += digits_with_sep(std::to_string(top_row_ + 1)) + "-"
           + digits_with_sep(std::to_string(bot)) + "/";
        s += (tr >= 0) ? digits_with_sep(std::to_string(tr)) : "?";

        if (!vc.empty()) {
            s += "  Col ";
            s += std::to_string(vc.front().col+1) + "-";
            s += std::to_string(vc.back().col+1)  + "/";
            s += std::to_string(num_cols_);
        }
        // Show search state
        if (search_mode_ == SearchMode::Active && !search_query_.empty()) {
            s += search_dir_forward_ ? "  /" : "  ?";
            s += search_query_;
            if (!search_regex_valid_ && !search_query_.empty())
                s += " (literal)";
            if (search_fail_)         s += " (not found)";
            else if (search_wrap_)    s += " (wrapped)";
            s += "  [n/N]:next/prev  [Esc]:clear";
        } else {
            bool need_lr = left_col_ > 0 || (!vc.empty() && vc.back().col < num_cols_-1);
            if (need_lr) s += "  [h/l]:←→col  [,/.]:narrow/widen";
            s += "  [j/k]:rows  /:search  Enter:detail  z:freeze  H:help  q:quit";
        }
        if ((int)s.size() < scr_c_) s += std::string(scr_c_ - (int)s.size(), ' ');
        attron(A_REVERSE);
        mvaddnstr(scr_r_ - 1, 0, s.c_str(), scr_c_);
        attroff(A_REVERSE);
    }

    // ── Detail pane ──────────────────────────────────────────────────────────

    // Fetch all (virtual) columns of one row with FULL (untruncated) values.
    // Loads any columns not already in cache (only once: subsequent openings
    // of the detail pane on other rows in the same chunk are free).
    std::vector<std::string> load_full_row(int64_t row) {
        std::vector<std::string> out(num_cols_);
        if (src_.num_chunks() == 0) return out;
        int c = chunk_for_row(row);
        std::vector<int> all_src;
        for (int i = 0; i < src_num_cols_; ++i) all_src.push_back(i);
        ensure_cols(c, all_src);
        auto it = cache_.find(c);
        if (it == cache_.end() || !it->second.ok) return out;
        const CachedRG& cr = it->second;
        int64_t local = row - cr.first_row;
        if (local < 0 || local >= cr.num_rows) return out;

        // We want untruncated values here; cell_at() applies max_col_w_.
        // Temporarily bypass via a local unwrap.
        std::unordered_map<std::string, std::string> parsed;
        int parsed_row = -1;
        for (int vc = 0; vc < num_cols_; ++vc) {
            int sc = virt_src_col_[vc];
            auto arr = (sc >= 0 && sc < (int)cr.cols.size()) ? cr.cols[sc] : nullptr;
            if (!arr) continue;
            int64_t off = local;
            for (auto& chunk : arr->chunks()) {
                if (off < chunk->length()) {
                    std::string val;
                    const std::string& key = virt_info_key_[vc];
                    if (!key.empty()) {
                        std::string raw = cell_to_string(*chunk, off);
                        if (parsed_row != (int)local) {
                            parsed.clear();
                            for (auto& kv : parse_kv_list(raw))
                                parsed.emplace(std::move(kv.first),
                                               std::move(kv.second));
                            parsed_row = (int)local;
                        }
                        auto fit = parsed.find(key);
                        val = (fit != parsed.end())
                                ? (fit->second.empty() ? "true" : fit->second)
                                : NULL_SYMBOL;
                    } else {
                        val = cell_to_display_string(*chunk, off);
                    }
                    out[vc] = src_.format_cell(sc, std::move(val));
                    break;
                }
                off -= chunk->length();
            }
        }
        return out;
    }

    // Build the list of (label, value) lines shown in the detail pane.
    // Columns whose value looks like a k=v list are followed by indented sub-entries.
    std::vector<std::pair<std::string,std::string>>
    build_detail_lines(const std::vector<std::string>& vals) const {
        std::vector<std::pair<std::string,std::string>> L;
        for (int ci = 0; ci < num_cols_; ++ci) {
            L.emplace_back(col_names_[ci], vals[ci]);
            if (looks_like_kv_list(vals[ci])) {
                for (auto& [k, v] : parse_kv_list(vals[ci]))
                    L.emplace_back("  " + k, v);
            }
        }
        return L;
    }

    // Centred help overlay listing every TUI keybinding. Toggle with `?` /
    // F1 / `H`. Exits on any keystroke (including the toggle keys).
    void draw_help_overlay() {
        struct Row { const char* keys; const char* desc; };
        static const Row rows[] = {
            {"q  Esc",       "quit  (Esc clears search if active)"},
            {"↑↓  j k",      "scroll one row"},
            {"PgUp PgDn  ␣ b","scroll one page"},
            {"g  G  Home End","top / bottom of file"},
            {"←→  h l",      "scroll one column"},
            {",  .",          "narrow / widen the leftmost visible column"},
            {"z",            "toggle frozen first column"},
            {"/  ?",          "search forward / backward (regex, icase)"},
            {"n  N",          "next / previous match (direction-aware)"},
            {"Enter",         "open detail pane for the top-visible row"},
            {"mouse wheel",   "scroll rows"},
            {"?  F1  H",      "toggle this help"},
        };
        const int n = (int)(sizeof(rows) / sizeof(rows[0]));
        // Compute panel size.
        int w_keys = 0, w_desc = 0;
        for (auto& r : rows) {
            w_keys = std::max(w_keys, (int)display_width(r.keys));
            w_desc = std::max(w_desc, (int)display_width(r.desc));
        }
        const std::string title = " vv — keys ";
        int inner = std::max(w_keys + 2 + w_desc, (int)display_width(title));
        int panel_w = inner + 4;        // 2-space pad on each side
        int panel_h = n + 4;            // top border + title + sep + rows + bottom
        if (panel_w > scr_c_)  panel_w = scr_c_;
        if (panel_h > scr_r_)  panel_h = scr_r_;
        int y0 = std::max(0, (scr_r_ - panel_h) / 2);
        int x0 = std::max(0, (scr_c_ - panel_w) / 2);

        auto hbar = [&](const char* l, const char* m, const char* r,
                        int n_inner) {
            std::string s = l;
            for (int i = 0; i < n_inner; ++i) s += BOX_HLINE;
            s += r;
            return s;
        };

        // Top border with embedded title.
        {
            int title_w = (int)display_width(title);
            int rest = std::max(0, panel_w - 2 - title_w);
            int pad_l = rest / 2;
            int pad_r = rest - pad_l;
            std::string top = BOX_TL;
            for (int i = 0; i < pad_l; ++i) top += BOX_HLINE;
            top += title;
            for (int i = 0; i < pad_r; ++i) top += BOX_HLINE;
            top += BOX_TR;
            attron(A_BOLD);
            mvaddstr(y0, x0, top.c_str());
            attroff(A_BOLD);
        }
        // Body lines: paint inner area with spaces, then add side borders.
        for (int i = 1; i < panel_h - 1; ++i) {
            mvaddstr(y0 + i, x0, BOX_VLINE);
            mvhline(y0 + i, x0 + 1, ' ', panel_w - 2);
            mvaddstr(y0 + i, x0 + panel_w - 1, BOX_VLINE);
        }
        // Rows.
        for (int i = 0; i < n && i + 1 < panel_h - 1; ++i) {
            int yy = y0 + 1 + i;
            int xx = x0 + 2;
            attron(A_BOLD);
            mvaddstr(yy, xx, rows[i].keys);
            attroff(A_BOLD);
            mvaddstr(yy, xx + w_keys + 2, rows[i].desc);
        }
        // Footer hint inside the bottom border.
        std::string bottom = hbar(BOX_BL, "", BOX_BR, panel_w - 2);
        mvaddstr(y0 + panel_h - 1, x0, bottom.c_str());
    }

    void draw_detail_pane() {
        if (detail_row_ < 0) return;
        auto vals = load_full_row(detail_row_);
        auto lines = build_detail_lines(vals);

        // Compute widths
        int label_w = 0, val_w = 0;
        for (auto& [l, v] : lines) {
            label_w = std::max(label_w, (int)display_width(l));
            val_w   = std::max(val_w,   (int)display_width(v));
        }
        int max_inner_w = scr_c_ - 4;                  // leave 2-char margin each side
        int inner_w     = std::min(max_inner_w, label_w + 2 + val_w);
        if (inner_w < 20) inner_w = std::min(max_inner_w, 20);
        int pane_w = inner_w + 4;                      // +4: " | " + content + " | "
        int max_inner_h = scr_r_ - 4;
        int inner_h     = std::min((int)lines.size() + 1, max_inner_h);  // +1 for footer hint
        int pane_h      = inner_h + 2;                 // +2 for top/bottom border
        int y0 = (scr_r_ - pane_h) / 2;
        int x0 = (scr_c_ - pane_w) / 2;
        if (y0 < 0) y0 = 0;
        if (x0 < 0) x0 = 0;

        // Clamp scroll
        int visible_rows = inner_h - 1;  // -1 for footer hint line
        int max_scroll   = std::max(0, (int)lines.size() - visible_rows);
        if (detail_scroll_ > max_scroll) detail_scroll_ = max_scroll;
        if (detail_scroll_ < 0) detail_scroll_ = 0;

        // Borders (rounded, Unicode box-drawing)
        std::string title = " Row " + std::to_string(detail_row_) + " ";
        int title_cols = (int)display_width(title);
        int title_pad  = std::max(0, inner_w - title_cols);
        // Layout: ╭ ── title ─*pad ╮   (total = 1 + 2 + title + pad + 1 = pane_w)
        std::string top_border = std::string(BOX_TL)
                                 + repeat_utf8(BOX_HLINE, 2)
                                 + title
                                 + repeat_utf8(BOX_HLINE, title_pad)
                                 + BOX_TR;
        nc_str(y0, x0, top_border, A_BOLD);

        for (int r = 0; r < inner_h; ++r) {
            int sy = y0 + 1 + r;
            std::string line = std::string(BOX_VLINE) + " ";
            int idx = r + detail_scroll_;
            if (r == inner_h - 1) {
                // Footer hint line
                std::string hint = " [j/k]:scroll  [Esc/Enter]:close ";
                if ((int)hint.size() > inner_w) hint.resize(inner_w);
                line += hint + std::string(inner_w - (int)hint.size(), ' ');
            } else if (idx < (int)lines.size()) {
                const auto& [l, v] = lines[idx];
                // Fit label
                std::string L = l;
                if ((int)display_width(L) > label_w) L.resize(label_w);
                else L += std::string(label_w - display_width(L), ' ');
                std::string V = v;
                int avail_v = inner_w - label_w - 2;  // "label: value"
                if (avail_v < 1) avail_v = 1;
                if ((int)display_width(V) > avail_v) {
                    if (avail_v >= 3) {
                        V.resize(avail_v - 3);
                        V += ELLIPSIS;
                    } else V.resize(avail_v);
                }
                else V += std::string(avail_v - display_width(V), ' ');
                line += L + ": " + V;
            } else {
                line += std::string(inner_w, ' ');
            }
            line += std::string(" ") + BOX_VLINE;
            // Highlight sub-entries (labels starting with two spaces) dimly
            bool is_sub = idx < (int)lines.size() && !lines[idx].first.empty() &&
                          lines[idx].first[0] == ' ';
            nc_str(sy, x0, line, (r == inner_h - 1) ? A_DIM :
                                 (is_sub ? A_NORMAL : A_BOLD));
        }
        std::string bot_border = std::string(BOX_BL)
                                 + repeat_utf8(BOX_HLINE, pane_w - 2)
                                 + BOX_BR;
        nc_str(y0 + pane_h - 1, x0, bot_border, A_BOLD);
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
        // Prefetch just the source columns that are on screen right now,
        // then fit integer column widths to the rows currently visible.
        // Recompute visible_cols afterwards: width changes may add or drop
        // columns at the right edge.
        {
            std::vector<int> virt;
            virt.reserve(vc.size());
            for (auto& c : vc) virt.push_back(c.col);
            prefetch_visible(virt);
            fit_integer_widths_to_visible(virt);
        }
        vc = visible_cols();
        draw_header(vc);
        int dl = data_lines();
        for (int y = 0; y < dl; ++y)
            draw_data_row(HDR_H + y, top_row_ + y, vc);
        draw_status(vc);
        draw_detail_pane();  // overlay if detail_row_ >= 0
        if (help_open_) draw_help_overlay();
        refresh();
    }

    void setup_colors() {
        if (!has_colors()) return;
        start_color(); use_default_colors();

        const bool c256 = COLORS >= 256;

        // Soft 256-color palette, with a basic-16 fallback on lesser terminals.
        const int fg_header = c256 ? 111 : COLOR_WHITE;   // soft blue
        const int fg_index  = c256 ? 244 : COLOR_WHITE;   // mid grey
        const int fg_null   = c256 ? 243 : COLOR_WHITE;   // dim grey
        const int fg_number = c256 ?  81 : COLOR_CYAN;    // cyan
        const int fg_boolt  = c256 ? 114 : COLOR_GREEN;   // soft green
        const int fg_boolf  = c256 ? 210 : COLOR_YELLOW;  // soft red
        const int fg_sep    = c256 ? 238 : COLOR_WHITE;   // dim grey
        const int fg_search = c256 ? 232 : COLOR_BLACK;
        const int bg_search = c256 ? 220 : COLOR_YELLOW;  // gold

        init_pair(NCP_HEADER,  fg_header, -1);
        init_pair(NCP_INDEX,   fg_index,  -1);
        init_pair(NCP_NULL,    fg_null,   -1);
        init_pair(NCP_NUMBER,  fg_number, -1);
        init_pair(NCP_BOOL_T,  fg_boolt,  -1);
        init_pair(NCP_BOOL_F,  fg_boolf,  -1);
        init_pair(NCP_SEP,     fg_sep,    -1);
        init_pair(NCP_SEARCH,  fg_search, bg_search);
        init_pair(NCP_PLAIN,   -1,        -1);

        // Zebra twins: same fg, dim background (only with a 256-color term —
        // an approximate bg on an 8-color palette looks worse than none).
        if (c256) {
            const int bg_zebra = 235;  // barely off default background
            init_pair(NCP_HEADER + ZEBRA_OFFSET, fg_header, bg_zebra);
            init_pair(NCP_INDEX  + ZEBRA_OFFSET, fg_index,  bg_zebra);
            init_pair(NCP_NULL   + ZEBRA_OFFSET, fg_null,   bg_zebra);
            init_pair(NCP_NUMBER + ZEBRA_OFFSET, fg_number, bg_zebra);
            init_pair(NCP_BOOL_T + ZEBRA_OFFSET, fg_boolt,  bg_zebra);
            init_pair(NCP_BOOL_F + ZEBRA_OFFSET, fg_boolf,  bg_zebra);
            init_pair(NCP_SEP    + ZEBRA_OFFSET, fg_sep,    bg_zebra);
            init_pair(NCP_PLAIN  + ZEBRA_OFFSET, -1,        bg_zebra);
            zebra_enabled_ = true;
            // Start dynamic RGB pairs past the zebra range.
            next_rgb_pair_ = NCP_PLAIN + ZEBRA_OFFSET + 1;
        }
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
        // Account for digit-grouping underscores in the rendered row number.
        int64_t tr = src.total_rows();
        int64_t tr_for_width = (tr >= 0) ? tr : 999999;
        idx_w_ = (int)display_width(digits_with_sep(
            std::to_string(std::max<int64_t>(tr_for_width - 1, 0))));

        src_num_cols_ = num_cols_;

        // Detect VCF INFO expansion: need both an INFO source column and
        // ##INFO=<...> declarations in the preamble.
        int info_col_idx = -1;
        for (int ci = 0; ci < src_num_cols_; ++ci)
            if (src.schema()->field(ci)->name() == "INFO") { info_col_idx = ci; break; }
        std::vector<std::pair<std::string, arrow::Type::type>> info_fields;
        if (info_col_idx >= 0)
            info_fields = parse_vcf_info_headers(src.preamble_below());

        // Build the virtual column layout. `hidden_for_display` entries
        // (e.g. LociSSD's `MaxEndSoFar`) are skipped here so they don't
        // appear in the TUI; they remain accessible to the underlying
        // cache loader.
        auto hidden_v = src.hidden_for_display();
        std::set<std::string> hidden(hidden_v.begin(), hidden_v.end());
        std::vector<std::string>        v_names;
        std::vector<int>                v_src;
        std::vector<std::string>        v_info;
        std::vector<arrow::Type::type>  v_types;
        std::vector<bool>               v_is_bool;
        for (int sc = 0; sc < src_num_cols_; ++sc) {
            if (hidden.count(src.schema()->field(sc)->name())) continue;
            if (sc == info_col_idx && !info_fields.empty()) {
                for (auto& [k, t] : info_fields) {
                    v_names.push_back(k);
                    v_src.push_back(sc);
                    v_info.push_back(k);
                    v_types.push_back(t);
                    v_is_bool.push_back(t == arrow::Type::BOOL);
                }
            } else {
                auto f = src.schema()->field(sc);
                v_names.push_back(f->name());
                v_src.push_back(sc);
                v_info.push_back("");
                v_types.push_back(f->type()->id());
                v_is_bool.push_back(display_type(*f) == arrow::Type::BOOL);
            }
        }

        num_cols_      = (int)v_names.size();
        col_names_     = std::move(v_names);
        virt_src_col_  = std::move(v_src);
        virt_info_key_ = std::move(v_info);

        col_widths_.resize(num_cols_);
        right_align_.resize(num_cols_);
        is_bool_.resize(num_cols_);
        is_rgb_.resize(num_cols_);
        is_integer_.resize(num_cols_);
        for (int vc = 0; vc < num_cols_; ++vc) {
            auto t = v_types[vc];
            int min_w;
            if (virt_info_key_[vc].empty()) {
                min_w = src.min_col_width(virt_src_col_[vc]);
            } else {
                // Heuristic width for INFO key columns based on declared type.
                min_w = (t == arrow::Type::INT64 || t == arrow::Type::DOUBLE) ? 8
                      : (t == arrow::Type::BOOL) ? 5
                      : 16;
            }
            is_integer_[vc] = virt_info_key_[vc].empty() && is_integer_type(t);
            // Integer columns skip the max_col_w_ cap; their width is fitted
            // to the rows currently visible in fit_integer_widths_to_visible().
            int base = std::max((int)display_width(col_names_[vc]), min_w);
            // Floats/doubles render via "%.6g" — leave room for the typical
            // 8-char output ("0.172974") so values aren't immediately clipped.
            if (t == arrow::Type::FLOAT || t == arrow::Type::DOUBLE)
                base = std::max(base, 8);
            // Lists, fixed-size lists, and maps get truncated to "[first, …]"
            // form; size the column so that form has room for a typical first
            // element + brackets + ", …".
            if (t == arrow::Type::LIST || t == arrow::Type::LARGE_LIST
             || t == arrow::Type::FIXED_SIZE_LIST || t == arrow::Type::MAP)
                base = std::max(base, 14);
            // String columns: enough for 11 first characters + ellipsis when a
            // value overflows. Header may push it wider but never narrower.
            if (t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING)
                base = std::max(base, 12);
            col_widths_[vc]  = is_integer_[vc] ? base : std::min(base, max_col_w_);
            right_align_[vc] = is_numeric_type(t);
            is_bool_[vc]     = v_is_bool[vc];
            is_rgb_[vc]      = (col_names_[vc] == "RGB");
        }
    }

    // Returns false if the terminal type is not supported (missing terminfo).
    bool run() {
        setlocale(LC_ALL, "");
        SCREEN* scr = newterm(nullptr, stdout, stdin);
        if (!scr) return false;
        set_term(scr);
        noecho(); cbreak(); keypad(stdscr, TRUE); curs_set(0);
        set_escdelay(25); setup_colors();
        // Mouse: scroll wheel only (BUTTON4 = up, BUTTON5 = down).
        mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);

        bool quit = false;
        while (!quit) {
            draw();
            int ch = getch();
            int dl = data_lines();

            // ── Help overlay: any key dismisses it (and is consumed) ─────────
            if (help_open_) {
                help_open_ = false;
                continue;
            }

            // ── Mouse wheel ─────────────────────────────────────────────────
            if (ch == KEY_MOUSE) {
                MEVENT me;
                if (getmouse(&me) != ERR) {
                    constexpr int kWheelStep = 3;
                    if (me.bstate & BUTTON4_PRESSED) {
                        top_row_ = std::max<int64_t>(0, top_row_ - kWheelStep);
                    } else if (me.bstate & BUTTON5_PRESSED) {
                        int64_t tr = total_rows();
                        int64_t mt = (tr >= 0)
                                     ? std::max<int64_t>(0, tr - dl)
                                     : top_row_ + kWheelStep;
                        top_row_ = std::min<int64_t>(top_row_ + kWheelStep, mt);
                    }
                }
                continue;
            }

            // ── Search input mode ────────────────────────────────────────────
            if (search_mode_ == SearchMode::Input) {
                if (ch == '\n' || ch == KEY_ENTER) {
                    if (!search_input_.empty()) {
                        search_query_ = search_input_;
                        compile_search();
                        search_mode_  = SearchMode::Active;
                        search_row_   = -1;
                        do_search(search_dir_forward_);
                    } else {
                        search_mode_  = SearchMode::None;
                        search_query_.clear();
                        search_regex_.reset();
                        search_regex_valid_ = false;
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
                continue;
            }

            // ── Detail pane mode ─────────────────────────────────────────────
            if (detail_row_ >= 0) {
                switch (ch) {
                    case 27:                                  // Esc: close
                    case '\n': case '\r': case KEY_ENTER:
                        detail_row_ = -1; detail_scroll_ = 0; break;
                    case KEY_DOWN: case 'j': ++detail_scroll_; break;
                    case KEY_UP:   case 'k':
                        if (detail_scroll_ > 0) --detail_scroll_; break;
                    case KEY_NPAGE: case ' ':
                        detail_scroll_ += std::max(1, dl - 2); break;
                    case KEY_PPAGE: case 'b':
                        detail_scroll_ = std::max(0, detail_scroll_ - std::max(1, dl - 2)); break;
                    case 'g': case KEY_HOME: detail_scroll_ = 0; break;
                    case 'q': case 'Q': quit = true; detail_row_ = -1; break;
                    default: break;
                }
                continue;
            }

            // ── Navigation ───────────────────────────────────────────────────
            int64_t tr = total_rows();
            int64_t max_top = (tr >= 0) ? std::max<int64_t>(0, tr - dl) : top_row_ + dl;

            switch (ch) {
                case 'q': case 'Q': quit = true; break;
                case '\n': case '\r': case KEY_ENTER:  // Open detail pane for top-visible row
                    detail_row_    = top_row_;
                    detail_scroll_ = 0;
                    break;
                case 27:  // Esc: clear search if active, else quit
                    if (search_mode_ == SearchMode::Active) {
                        search_mode_  = SearchMode::None;
                        search_query_.clear();
                        search_regex_.reset();
                        search_regex_valid_ = false;
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
                case 'z':
                    freeze_first_col_ = !freeze_first_col_; break;
                case 'H': case KEY_F(1):
                    help_open_ = true; break;
                case '.':
                    col_widths_[left_col_] = std::min(256, col_widths_[left_col_] + 4);
                    break;
                case ',':
                    col_widths_[left_col_] = std::max(1, col_widths_[left_col_] - 4);
                    break;
                case '/':
                    search_mode_  = SearchMode::Input;
                    search_input_.clear();
                    search_dir_forward_ = true;
                    break;
                case '?':
                    search_mode_  = SearchMode::Input;
                    search_input_.clear();
                    search_dir_forward_ = false;
                    break;
                case 'n':
                    if (!search_query_.empty()) do_search(search_dir_forward_);
                    break;
                case 'N':
                    if (!search_query_.empty()) do_search(!search_dir_forward_);
                    break;
                case KEY_RESIZE: break;
                default: break;
            }
        }
        endwin();
        delscreen(scr);
        return true;
    }
};

// ── Table display (non-interactive) ──────────────────────────────────────────

// Detect terminal width. Falls back to $COLUMNS, then 80.
static int detect_terminal_width() {
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
static void print_schema_block(TabularSource& src);

static void print_vertical_table(TabularSource& src, const Config& cfg) {
    auto schema     = src.schema();
    int  n_fields   = schema->num_fields();
    std::vector<std::string> unknown;
    std::vector<int> col_indices = select_field_indices(src, cfg, &unknown);
    if (!unknown.empty()) {
        std::string u;
        for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        std::fprintf(stderr, "unknown column(s) in --select: %s\n", u.c_str());
        return;
    }
    int  show_fields = (int)col_indices.size();
    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr)) {
            std::fprintf(stderr, "--filter: %s\n", ferr.c_str());
            return;
        }
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
        return;
    }
    if (read_indices != col_indices)
        data = project_to_requested(data, read_indices, col_indices);

    int64_t n_records = data->num_rows();

    // Per-field flag: integer columns skip the max_col_w truncation.
    std::vector<bool> field_is_int(show_fields);
    for (int f = 0; f < show_fields; ++f)
        field_is_int[f] = is_integer_type(schema->field(col_indices[f])->type()->id());

    // Pre-render every cell into rendered[record][field].
    std::vector<std::vector<std::string>> rendered(n_records,
        std::vector<std::string>(show_fields));
    for (int f = 0; f < show_fields; ++f) {
        int f_src    = col_indices[f];
        auto arr_col = data->column(f);
        int64_t row  = 0;
        for (auto& chunk : arr_col->chunks()) {
            for (int64_t r = 0; r < chunk->length(); ++r, ++row) {
                std::string val = src.format_cell(f_src,
                    cell_to_display_string(*chunk, r));
                if (!field_is_int[f]) val = truncate(std::move(val), cfg.max_col_w);
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
        draw_row(columns, hdr, ra, /*is_header=*/true);
    }
    draw_separator(columns, SepKind::Middle);
    for (int f = 0; f < show_fields; ++f) {
        std::vector<std::string> row;
        std::vector<bool> ra;
        // Field-name column: left-aligned. Record columns: right-aligned
        // for every type, so values line up against the next field's column.
        for (size_t i = 0; i < columns.size(); ++i) {
            row.push_back(columns[i].cells[f]);
            ra.push_back(i != 0);
        }
        draw_row(columns, row, ra);
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
}

static void print_table(TabularSource& src, const Config& cfg) {
    auto schema = src.schema();
    std::vector<std::string> unknown;
    std::vector<int> col_indices = select_field_indices(src, cfg, &unknown);
    if (!unknown.empty()) {
        std::string u;
        for (auto& n : unknown) { if (!u.empty()) u += ","; u += n; }
        std::fprintf(stderr, "unknown column(s) in --select: %s\n", u.c_str());
        return;
    }
    int show_cols = (int)col_indices.size();
    FilterExpr fx;
    bool have_filter = false;
    if (!cfg.filter_expr.empty()) {
        std::string ferr;
        if (!parse_filter_expr(cfg.filter_expr, *schema, &fx, &ferr)) {
            std::fprintf(stderr, "--filter: %s\n", ferr.c_str());
            return;
        }
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
    if (!data) return;
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
                if (!is_int) val = truncate(std::move(val), cfg.max_col_w);
                if (display_width(val) > col.width) col.width = display_width(val);
                col.cells.push_back(std::move(val));
            }
        if (!is_int) col.width = std::min(col.width, cfg.max_col_w);
        col.header = truncate(col.header, cfg.max_col_w);
        col.width  = std::max(col.width, display_width(col.header));
        columns.push_back(std::move(col));
    }

    // BED track/browser lines shown above the table
    for (auto& line : src.preamble_above())
        std::printf("%s%s%s\n", g_color.meta_key, line.c_str(), g_color.reset);

    draw_separator(columns, SepKind::Top);
    { std::vector<std::string> hdr; std::vector<bool> ra;
      for (auto& c : columns) { hdr.push_back(c.header); ra.push_back(false); }
      draw_row(columns, hdr, ra, true); }
    draw_separator(columns, SepKind::Middle);
    for (int64_t r = 0; r < n_display; ++r) {
        std::vector<std::string> row; std::vector<bool> ra;
        for (auto& c : columns) { row.push_back(c.cells[r]); ra.push_back(c.right_align); }
        draw_row(columns, row, ra);
    }
    draw_separator(columns, SepKind::Bottom);

    if (show_cols < num_cols) {
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
    std::printf("\n%s[%lld rows x %d columns]%s\n",
                g_color.meta_key, (long long)total, num_cols, g_color.reset);

    print_schema_block(src);
}

// Schema + file-info block. Shared by print_table's footer and the
// stand-alone `--schema` mode.
static void print_schema_block(TabularSource& src) {
    auto schema = src.schema();
    int num_cols = schema->num_fields();
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

// ── Main ──────────────────────────────────────────────────────────────────────

// Friendly, short message for common filesystem problems; returns "" if the
// path is a readable regular file (further errors will come from the reader).
static std::string preflight_path(const std::string& path) {
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
    if (S_ISDIR(st.st_mode)) return "path is a directory";
    if (!S_ISREG(st.st_mode) && !S_ISFIFO(st.st_mode) && !S_ISLNK(st.st_mode))
        return "not a regular file";
    if (::access(path.c_str(), R_OK) != 0) return "permission denied";
    return "";
}

// Strip "IOError: Failed to open local file '<path>'. Detail: [errno N] "
// redundancy from Arrow's open-error strings — the path is already in our
// own "cannot open '...': " prefix.
static std::string shorten_reader_error(std::string msg) {
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

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    // Size Arrow's CPU thread pool so use_threads=true on the CSV / Parquet
    // readers actually has workers available.
    (void)arrow::SetCpuThreadPoolCapacity(effective_threads(cfg));

    // Apply --regions-file and --slop once, before any source-specific
    // region consumer parses cfg.region.
    if (auto e = apply_region_modifiers(cfg); !e.empty()) {
        std::fprintf(stderr, "vv: %s\n", e.c_str());
        return 1;
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

    {
        std::string why = preflight_path(cfg.path);
        if (!why.empty()) { report(cfg.path, why); return 1; }
    }

    // --validate: LociSSD invariants check. Doesn't go through open_source —
    // we re-open the Parquet file with our own reader so other flags (region,
    // select, filter) don't influence what we scan.
    if (cfg.validate) {
        std::string err = validate_lociss(cfg.path);
        if (!err.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        return 0;
    }

    std::unique_ptr<TabularSource> src;
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

    // --schema: just print schema + footer, then exit.
    if (cfg.schema_only) {
        print_schema_block(*src);
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

    // --unique: distinct value counts per column.
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
    if (cfg.tail_rows > 0) {
        std::string err = build_tail(src, cfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        cfg.filter_expr.clear();
        cfg.head_rows = 0; cfg.head_rows_set = false;
    }

    // --json / --ndjson: stream JSON rows to stdout.
    if (cfg.json_array || cfg.json_lines) {
        Config jcfg = cfg;
        if (!jcfg.head_rows_set) jcfg.head_rows = 0;
        std::string err = write_json(*src, jcfg);
        if (!err.empty()) { report(cfg.path, err); return 1; }
        return 0;
    }

    // --md: GitHub-flavored markdown table to stdout.
    if (cfg.md) {
        Config mcfg = cfg;
        if (!mcfg.head_rows_set) mcfg.head_rows = 0;
        write_markdown(*src, mcfg);
        return 0;
    }

    // Interactive viewer
    {
        bool auto_tui = !cfg.no_interactive && !cfg.delimiter && !cfg.vertical
                        && cfg.parquet_out.empty()
                        && !cfg.head_rows_set
                        && isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
        if (cfg.interactive || auto_tui) {
            TableTUI tui(*src, cfg);
            if (tui.run()) return 0;
            // Terminal type not supported — fall through to table output.
            if (cfg.interactive)
                std::fprintf(stderr, "error: cannot initialize terminal (missing terminfo?)\n");
        }
    }

    // Delimited output
    if (cfg.delimiter) {
        Config dcfg = cfg;
        if (!dcfg.head_rows_set) dcfg.head_rows = 0;
        write_delimited(*src, dcfg);
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
        return 0;
    }

    // Table display
    if (cfg.vertical) print_vertical_table(*src, cfg);
    else              print_table(*src, cfg);
    return 0;
}
