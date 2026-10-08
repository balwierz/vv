// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// internal.hpp: the reader core's shared declarations: the third-party and
// standard includes, types, constants and inline helpers used by more than
// one source file, and declarations of the functions they share. Generated
// once from the single-file main.cpp; edit it like any other source.
#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/statistics.h>
#include <arrow/type.h>
#include <arrow/scalar.h>
#include <arrow/csv/api.h>
#include <arrow/json/api.h>
#include <arrow/io/compressed.h>
#include <arrow/ipc/feather.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/util/compression.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <parquet/properties.h>
#if VV_HAVE_ORC
#include <arrow/adapters/orc/adapter.h>
#endif

#include <htslib/sam.h>
#include <htslib/vcf.h>
#include <htslib/tbx.h>
#include <htslib/kseq.h>
#include <htslib/bgzf.h>
#include <htslib/faidx.h>

// libBigWig (vendored under vendored/libBigWig/, compiled with -DNOCURL).
// Wrapped in an extern "C" because it's a C library; ARROW headers above
// already bring in C++ machinery.
extern "C" {
#include <bigWig.h>
}
extern "C" {
#include <sqlite3.h>
}
extern "C" {
#include <xlsxio_read.h>
}
// minizip + expat power the OpenDocument Spreadsheet (.ods) reader.
// minizip extracts content.xml from the .ods ZIP; expat parses the
// SpreadsheetML payload.
extern "C" {
// minizip's `unzip.h` lives under <minizip/> with the legacy zlib-contrib
// fork (Arch/Ubuntu/Brew) and under <minizip-ng/> with minizip-ng (our
// AlmaLinux static build). CMake's MINIZIP_INCLUDE_DIR points at the
// correct subdir, so a plain unprefixed include resolves on both.
#include <unzip.h>
#include <expat.h>
}
// md4c — CommonMark + GFM parser, vendored under vendored/md4c/.
// Drives the markdown viewer (`vv README.md`).
extern "C" {
#include <md4c.h>
}
// libhdf5 — drives the AnnData / generic HDF5 viewer
// (`vv x.h5ad`, `vv x.h5`). Headers are C-only; the C++ binding is
// disabled in our static build.
#include <hdf5.h>
// API-version shims. `VV_H5O_INFO_T` / `VV_H5Oget_info_by_name` etc. were
// introduced in HDF5 1.12. Ubuntu 22.04 / 24.04 still ship 1.10 where
// only the older v1 / v2 forms exist. Map our usage to whichever
// generation the installed library exposes.
#if H5_VERSION_GE(1,12,0)
  #define VV_H5O_INFO_T            H5O_info2_t
  #define VV_H5L_INFO_T            H5L_info2_t
  #define VV_H5Oget_info_by_name   H5Oget_info_by_name3
  #define VV_H5Oget_info           H5Oget_info3
  #define VV_H5L_ITERATE_T         H5L_iterate2_t
  #define VV_H5Lvisit              H5Lvisit2
#else
  #define VV_H5O_INFO_T            H5O_info_t
  #define VV_H5L_INFO_T            H5L_info_t
  #define VV_H5Oget_info_by_name   H5Oget_info_by_name2
  #define VV_H5Oget_info           H5Oget_info2
  #define VV_H5L_ITERATE_T         H5L_iterate_t
  #define VV_H5Lvisit              H5Lvisit
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <strings.h>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <zlib.h>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <numeric>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <mutex>
#include <thread>
#include <string>
#include <fnmatch.h>   // --select globs (POSIX; present on Linux + macOS)
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>
#include <unordered_map>
#include <vector>
#include <sys/ioctl.h>
#include <cerrno>
#include <langinfo.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

// The ncurses TUI is the CLI frontend only. libvvcore (VV_CORE_LIB) is the
// headless reader core shared with the Qt GUI / KDE plugins and must not pull
// in ncurses or define main().
#ifndef VV_CORE_LIB
#include <ncurses.h>
#undef OK   // ncurses defines OK as 0; conflicts with arrow::Status::OK()
#else
// Headless core: the Theme tables hold ncurses 16-color indices in their
// `nc16` fields but the core never paints with them. Provide the standard
// constants so those tables still compile without <ncurses.h>.
#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7
#endif

// Public reader-core surface (Config, TabularSource, FilterExpr, formatters,
// open_source), defined in the core sources; this header is what the Qt GUI
// and the KF6 plugins include to drive libvvcore.
#include "vv/vvcore.hpp"
#include "vv/vvjson.hpp"

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

inline Colors g_color;

// ── Themes ───────────────────────────────────────────────────────────────────
//
// Two parallel palettes: ANSI escape strings for the non-interactive
// ASCII table and ncurses 256-color indices for the TUI. The Theme
// struct bundles both. Built-in themes live in `kThemes`; selected via
// --theme NAME. Anything else (TOML / per-user themes) can be plumbed
// in later by extending the table.
//
// On terminals with fewer than 256 colors, the TUI falls back to a small
// set of named COLOR_* indices regardless of theme — every theme provides
// `nc16_*` fallbacks for that case.
struct Theme {
    const char* name;

    // ANSI escape strings for the non-interactive table output.
    const char* reset;
    const char* border;
    const char* header;
    const char* row_idx;
    const char* null_val;
    const char* number;
    const char* bool_true;
    const char* bool_false;
    const char* trunc;
    const char* type_int;
    const char* type_float;
    const char* type_str;
    const char* type_time;
    const char* type_bool;
    const char* type_other;
    const char* meta_key;

    // ncurses palette (foreground colors; -1 == terminal default).
    // 256-color indices on c256-capable terminals.
    int nc_fg_header, nc_fg_index, nc_fg_null, nc_fg_number;
    int nc_fg_boolt,  nc_fg_boolf, nc_fg_sep;
    int nc_fg_search, nc_bg_search;
    int nc_bg_zebra;   // -1 disables zebra striping for this theme

    // 16-color fallback for old terminals.
    int nc16_fg_header, nc16_fg_index, nc16_fg_null, nc16_fg_number;
    int nc16_fg_boolt,  nc16_fg_boolf, nc16_fg_sep;
    int nc16_fg_search, nc16_bg_search;
};

// ── Built-in themes ──────────────────────────────────────────────────────────
// "default" matches the colors vv has used since 1.0 — bright accents
// on whatever the terminal's default background is.
inline const Theme kThemeDefault = {
    "default",
    "\033[0m", "\033[90m", "\033[1;97m", "\033[90m",
    "\033[2;3m", "\033[96m", "\033[92m", "\033[33m",
    "\033[90m",
    "\033[96m", "\033[93m", "\033[92m", "\033[95m", "\033[94m",
    "\033[37m", "\033[1m",
    /*nc*/ 111, 244, 243, 81, 114, 210, 238, 232, 220, 235,
    /*nc16*/ COLOR_WHITE, COLOR_WHITE, COLOR_WHITE, COLOR_CYAN,
             COLOR_GREEN, COLOR_YELLOW, COLOR_WHITE, COLOR_BLACK, COLOR_YELLOW,
};

// "dark" — slightly punchier than the default. Darker text gets pushed
// to brighter shades; assumes a true dark (near-black) background.
inline const Theme kThemeDark = {
    "dark",
    "\033[0m", "\033[38;5;240m", "\033[1;38;5;75m", "\033[38;5;244m",
    "\033[2;3;38;5;243m", "\033[38;5;81m", "\033[38;5;114m", "\033[38;5;210m",
    "\033[38;5;240m",
    "\033[38;5;81m", "\033[38;5;221m", "\033[38;5;150m", "\033[38;5;213m",
    "\033[38;5;111m", "\033[38;5;250m", "\033[1m",
    /*nc*/  75, 244, 243, 81, 114, 210, 240, 232, 221, 234,
    /*nc16*/ COLOR_CYAN, COLOR_WHITE, COLOR_WHITE, COLOR_CYAN,
             COLOR_GREEN, COLOR_RED, COLOR_WHITE, COLOR_BLACK, COLOR_YELLOW,
};

// "light" — for terminals with a light background. Bright accents would
// vanish, so every color is shifted to a darker mid-saturation tone.
// Null / dim text stays distinguishable from the background.
inline const Theme kThemeLight = {
    "light",
    "\033[0m", "\033[38;5;245m", "\033[1;38;5;25m", "\033[38;5;240m",
    "\033[2;3;38;5;244m", "\033[38;5;31m", "\033[38;5;28m", "\033[38;5;124m",
    "\033[38;5;245m",
    "\033[38;5;31m", "\033[38;5;130m", "\033[38;5;28m", "\033[38;5;90m",
    "\033[38;5;25m",  "\033[38;5;240m", "\033[1m",
    /*nc*/  25, 240, 244, 31, 28, 124, 245, 231, 130, 254,
    /*nc16*/ COLOR_BLUE, COLOR_BLACK, COLOR_WHITE, COLOR_BLUE,
             COLOR_GREEN, COLOR_RED, COLOR_BLACK, COLOR_WHITE, COLOR_YELLOW,
};

// "solarized-dark" — Ethan Schoonover's Solarized palette
// (base03/02/01/00 dark anchors, base0..3 light anchors, plus the
// accent ring yellow/orange/red/magenta/violet/blue/cyan/green).
// 256-color values come from the canonical Solarized table.
inline const Theme kThemeSolarizedDark = {
    "solarized-dark",
    "\033[0m", "\033[38;5;240m", "\033[1;38;5;33m", "\033[38;5;240m",
    "\033[2;3;38;5;241m", "\033[38;5;37m", "\033[38;5;64m", "\033[38;5;160m",
    "\033[38;5;240m",
    "\033[38;5;37m", "\033[38;5;136m", "\033[38;5;64m", "\033[38;5;125m",
    "\033[38;5;33m", "\033[38;5;244m", "\033[1m",
    /*nc*/  33, 240, 241, 37, 64, 160, 240, 234, 136, 235,
    /*nc16*/ COLOR_BLUE, COLOR_WHITE, COLOR_WHITE, COLOR_CYAN,
             COLOR_GREEN, COLOR_RED, COLOR_WHITE, COLOR_BLACK, COLOR_YELLOW,
};

// "solarized-light" — same accent ring, flipped backgrounds. base3
// (#fdf6e3) substitutes for the terminal default background here too.
inline const Theme kThemeSolarizedLight = {
    "solarized-light",
    "\033[0m", "\033[38;5;245m", "\033[1;38;5;33m", "\033[38;5;245m",
    "\033[2;3;38;5;244m", "\033[38;5;37m", "\033[38;5;64m", "\033[38;5;160m",
    "\033[38;5;245m",
    "\033[38;5;37m", "\033[38;5;136m", "\033[38;5;64m", "\033[38;5;125m",
    "\033[38;5;33m", "\033[38;5;240m", "\033[1m",
    /*nc*/  33, 245, 244, 37, 64, 160, 245, 230, 136, 254,
    /*nc16*/ COLOR_BLUE, COLOR_BLACK, COLOR_WHITE, COLOR_CYAN,
             COLOR_GREEN, COLOR_RED, COLOR_BLACK, COLOR_WHITE, COLOR_YELLOW,
};

inline const Theme* g_theme = &kThemeDefault;

// ── Terminal background detection (for zebra-stripe contrast) ─────────────────
// The default/dark themes' zebra shade is a near-black grey that only reads as a
// subtle stripe on a dark terminal; on a light terminal (e.g. JupyterLab's web
// terminal) it becomes a hard black band that swallows the default-foreground
// text. Detect the actual background so the stripe can adapt.
enum class TermBg { Unknown, Dark, Light };

inline TermBg g_term_bg = TermBg::Unknown;

void detect_term_bg();

// Full list, in the order the TUI picker presents them.
inline const Theme* const kAllThemes[] = {
    &kThemeDefault, &kThemeDark, &kThemeLight,
    &kThemeSolarizedDark, &kThemeSolarizedLight,
};

inline constexpr int kNumThemes = (int)(sizeof(kAllThemes) / sizeof(kAllThemes[0]));

const Theme* find_theme(const std::string& name);

void init_colors();

void strip_ws_inplace(std::string& s);

void load_user_config(Config& cfg);

bool save_user_setting(const std::string& key, const std::string& value);

const char* type_color(arrow::Type::type t);

arrow::Type::type display_type(const arrow::Field& f);

int effective_threads(const Config& cfg);

int effective_decode_threads(const Config& cfg);

inline constexpr const char* kVersion = "1.28.0";

void json_emit_string(const std::string& v);

void print_formats(bool as_json);

bool fends_ci(const std::string& s, const std::string& sfx);

// True when --vertical came from being invoked as `vh`, not from the user
// typing it. Document modes (markdown, plain text) reject a TYPED --vertical
// but must not fail `vh notes.md` on a flag nobody asked for.
inline bool g_vertical_from_argv0 = false;

// SAM FLAG bit names, as samtools flags / view -f / -F spell them.
inline const std::pair<const char*, int> kSamFlagNames[] = {
    {"PAIRED", 0x1},      {"PROPER_PAIR", 0x2}, {"UNMAP", 0x4},
    {"MUNMAP", 0x8},      {"REVERSE", 0x10},    {"MREVERSE", 0x20},
    {"READ1", 0x40},      {"READ2", 0x80},      {"SECONDARY", 0x100},
    {"QCFAIL", 0x200},    {"DUP", 0x400},       {"SUPPLEMENTARY", 0x800},
};

Config parse_args(int argc, char** argv);

// U+2026 HORIZONTAL ELLIPSIS — 3 UTF-8 bytes, but exactly 1 terminal column.
// Used as the truncation marker: shows more content than "..." for the same width.
inline constexpr const char ELLIPSIS[]    = "\xe2\x80\xa6";

// U+2205 EMPTY SET — 3 UTF-8 bytes, 1 terminal column.
// Displayed in place of NULL values: compact and unambiguous.
inline constexpr const char NULL_SYMBOL[] = "\xe2\x88\x85";

// Box-drawing glyphs (each 3 UTF-8 bytes, 1 terminal column).
inline constexpr const char BOX_HLINE[] = "\xe2\x94\x80";

inline constexpr const char BOX_VLINE[] = "\xe2\x94\x82";

inline constexpr const char BOX_TL[]    = "\xe2\x95\xad";

inline constexpr const char BOX_TR[]    = "\xe2\x95\xae";

inline constexpr const char BOX_BR[]    = "\xe2\x95\xaf";

inline constexpr const char BOX_BL[]    = "\xe2\x95\xb0";

inline constexpr const char BOX_LT[]    = "\xe2\x94\x9c";

inline constexpr const char BOX_RT[]    = "\xe2\x94\xa4";

inline constexpr const char BOX_TT[]    = "\xe2\x94\xac";

inline constexpr const char BOX_BT[]    = "\xe2\x94\xb4";

inline constexpr const char BOX_X[]     = "\xe2\x94\xbc";

// The non-interactive table's frame glyphs, chosen at runtime so `--box ascii`
// (or a non-UTF-8 locale) draws with plain ASCII instead of the box-drawing
// characters — which render as mojibake on a C-locale terminal, in an ASCII-only
// pipe, or when pasted somewhere without the font. The Unicode set reuses the
// constants above; the TUI keeps drawing them directly (an interactive terminal
// that runs the ncurses UI is assumed to handle UTF-8).
struct BoxGlyphs {
    const char *hline, *vline, *tl, *tr, *br, *bl, *lt, *rt, *tt, *bt, *x;
    // The truncation marker travels with the frame style: the ASCII table uses
    // "..." (3 columns) where the Unicode one uses "…" (1 column). Both are 3
    // bytes, so the byte arithmetic that finds a trailing marker is identical;
    // only the display width reserved for it differs, handled where it is used.
    const char *ell;
};

inline constexpr BoxGlyphs kBoxUnicode = {
    BOX_HLINE, BOX_VLINE, BOX_TL, BOX_TR, BOX_BR, BOX_BL,
    BOX_LT, BOX_RT, BOX_TT, BOX_BT, BOX_X, ELLIPSIS,
};

inline constexpr BoxGlyphs kBoxAscii = {
    "-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+", "...",
};

inline const BoxGlyphs* g_box = &kBoxUnicode;

std::string select_box_glyphs(const std::string& style);

std::string repeat_utf8(const char* glyph, int n);

std::string base64_encode(const std::string& in);

void osc52_copy(const std::string& s);

bool is_integer_type(arrow::Type::type t);

uint32_t utf8_decode(const std::string& s, size_t i, int* len);

int codepoint_width(uint32_t cp);

std::string sub_display(const std::string& s, int start, int width);

size_t utf8_prefix_for_width(const std::string& s, int max_cols);

bool truncate_cuts(const std::string& s, int max_w);

bool parse_rgb(const std::string& s, int* r, int* g, int* b);

int nearest_256(int r, int g, int b);

// Float text precision. The table / TUI show 6 significant digits; an export
// or an identity key (--distinct, --unique) must not: `%.6g` wrote 1234567.891
// and 1234567.892 both as "1.23457e+06", and merged them. While an ExactFloats
// guard is alive on this thread, cell_to_string writes the shortest decimal
// that reads back as the same value (to_chars would, but its floating-point
// overloads are missing from older macOS runtimes, so search the precision).
inline thread_local bool t_exact_floats = false;

struct ExactFloats {
    bool prev;
    ExactFloats() : prev(t_exact_floats) { t_exact_floats = true; }
    ~ExactFloats() { t_exact_floats = prev; }
    ExactFloats(const ExactFloats&) = delete;
    ExactFloats& operator=(const ExactFloats&) = delete;
};

// RFC 4180 quoting: wrap in double-quotes if the value contains the delimiter,
// a double-quote, or a newline; escape embedded quotes by doubling them.
// ── Export sink ──────────────────────────────────────────────────────────────
// The CLI's writers print to stdout (TSV / CSV / JSON) or to a path (Parquet /
// Arrow). export_view() runs the same writers on a frontend's worker thread
// and installs an ExportSink for that thread: the text writers then print to
// its file instead of stdout, every writer adds the rows it writes to
// progress->rows and stops between chunks once progress->cancel is set, and
// the CLI's "[N rows → path]" stderr summary is left out. With no sink (the
// CLI), the writers behave exactly as before.
struct ExportSink {
    FILE*           out      = nullptr;   // text writers' output; null = stdout
    ExportProgress* progress = nullptr;
};

inline thread_local ExportSink* t_export = nullptr;

FILE* out_stream();

bool export_canceled();

void export_count(int64_t rows);

inline constexpr const char kExportCanceled[] = "canceled";

void write_csv_field(const std::string& val, char sep);

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

std::string chrom_alias(const std::string& n);

// Resolve a queried chromosome to the file's naming: `chrom` if the file has it,
// else its human/mouse alias if the file has that (noting the swap once), else
// `chrom` unchanged (so an actually-missing contig still errors/empties as before).
template <typename HavePred>
static std::string resolve_chrom(const std::string& chrom, HavePred have,
                                 const std::string& path, bool& noted) {
    if (have(chrom)) return chrom;
    std::string alt = chrom_alias(chrom);
    if (!alt.empty() && have(alt)) {
        if (!noted) {
            std::fprintf(stderr, "vv: %s: region chromosome '%s' not found; using "
                         "'%s' (UCSC/Ensembl naming)\n",
                         path.c_str(), chrom.c_str(), alt.c_str());
            noted = true;
        }
        return alt;
    }
    return chrom;
}

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

std::vector<std::string> strip_bed_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input, std::string* put_back);

std::vector<std::string> strip_prefix_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    char prefix_char, std::string* put_back);

std::vector<std::string> strip_gct_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    std::string* put_back, std::vector<std::string>* col_names);

std::vector<std::string> strip_pairs_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    std::string* put_back, std::vector<std::string>* col_names);

std::vector<std::string> strip_vcf_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    std::vector<std::string>* col_names_out, std::string* put_back);

std::vector<std::string> strip_tsv_csv_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    char delim, std::string* put_back,
    std::vector<std::string>* col_names_out);

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

bool parse_region_one(const std::string& s, Region* out,
                       bool one_based = false);

std::vector<Region> parse_region_list(const std::string& spec,
                                       bool one_based = false);

std::string region_to_htslib(const Region& r);

std::string regions_to_htslib(const std::string& canonical);

// Rewrite each region window's chromosome in place via resolve_chrom() (defined
// earlier, before TabixInputStream, so the string-based tabix path can reuse it).
template <typename HavePred>
static void resolve_region_chroms(std::vector<Region>& ws, HavePred have,
                                  const std::string& path) {
    bool noted = false;
    for (auto& w : ws) w.chrom = resolve_chrom(w.chrom, have, path, noted);
}

bool tabix_index_exists(const std::string& path);

std::string filter_quote_name(const std::string& name);

std::vector<std::string> filter_split_or(const std::string& s);

bool cell_as_double(const arrow::Table& tbl, int col, int64_t row,
                     double* out);

bool eval_atom(const arrow::Table& tbl, int64_t row, const FilterAtom& a,
                const std::vector<int>& read_indices);

std::shared_ptr<arrow::Table> apply_filter(
 const std::shared_ptr<arrow::Table>& tbl, const FilterExpr& expr,
 const std::vector<int>& read_indices);

std::vector<int> union_with_filter(
 const std::vector<int>& base, const FilterExpr& expr);

std::shared_ptr<arrow::Table> project_to_requested(
 const std::shared_ptr<arrow::Table>& tbl,
 const std::vector<int>& read_indices,
 const std::vector<int>& requested);

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

std::vector<AutosqlField> parse_autosql(const std::string& sql);

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

bool parse_lociss_chromosomes(const std::string& json,
                              std::vector<LocissChrom>* out);

bool lociss_manifest_value(const std::string& json, const char* key,
                           std::string* out);

std::string assembly_to_species(const std::string& assembly);

std::string open_lociss_v4_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

// Arrow's streaming CSV / JSON readers read ahead on its IO thread pool and parse
// on the CPU pool. A source that stops reading a large file part-way (a preview,
// a thumbnail) and is destroyed leaves that pipeline running; at process exit
// it then races the destruction of Arrow's thread pools and memory pool — a
// hang or crash in exit(). So each reader is fed through a ReadGate: closing a
// source flips the gate, which makes further reads return end-of-file, and then
// drains the reader so the blocks already read ahead finish. Afterwards nothing
// belonging to that source is left running.
struct ReadGate { std::atomic<bool> stop{false}; };

class GatedInputStream : public arrow::io::InputStream {
    std::shared_ptr<arrow::io::InputStream> inner_;
    std::shared_ptr<ReadGate>               gate_;
public:
    GatedInputStream(std::shared_ptr<arrow::io::InputStream> inner,
                     std::shared_ptr<ReadGate> gate)
        : inner_(std::move(inner)), gate_(std::move(gate)) {}
    arrow::Status Close() override { return inner_->Close(); }
    bool closed() const override { return inner_->closed(); }
    arrow::Result<int64_t> Tell() const override { return inner_->Tell(); }
    arrow::Result<int64_t> Read(int64_t n, void* buf) override {
        if (gate_->stop.load()) return 0;
        return inner_->Read(n, buf);
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t n) override {
        if (gate_->stop.load()) return std::make_shared<arrow::Buffer>(nullptr, 0);
        return inner_->Read(n);
    }
};

// Stop `gate` and drain `reader` (csv or json StreamingReader) until it reports
// the end of its (now truncated) input or an error, then release it. Bounded:
// only the blocks already read ahead remain to be parsed.
template <class Reader>
static void close_stream_reader(std::shared_ptr<Reader>& reader,
                                const std::shared_ptr<ReadGate>& gate) {
    if (!reader) return;
    if (gate) gate->stop.store(true);
    for (int i = 0; i < 1024; ++i) {
        std::shared_ptr<arrow::RecordBatch> b;
        if (!reader->ReadNext(&b).ok() || !b) break;
    }
    reader.reset();
}

std::vector<int> select_field_indices(
 const TabularSource& src, const Config& cfg,
 std::vector<std::string>* unknown_out = nullptr,
 bool include_hidden = false);

std::string unknown_columns_error(const TabularSource& src,
                                  const std::vector<std::string>& unknown);

std::string open_parquet_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

bool is_parquet_source(const TabularSource& src);

enum class DelimKind { CSV, TSV, BED, VCF, GFF, SAM, PAF, Mpileup, Mtx };

// TSV layouts read by DelimKind::TSV with their own header handling.
enum class TsvDialect { None, Bedpe, Pairs, Gct, Maf, Blast };

// ENCODE peak / signal flavours of BED. Carried alongside DelimKind::BED so
// the BED reader can apply variant-specific column names (signalValue,
// pValue, qValue, peak, value, sequence) instead of the generic +1/+2.
enum class BedVariant {
    None,         // vanilla BED3..BED12
    NarrowPeak,   // BED6+4: chr,start,end,name,score,strand + signal,p,q,peak
    BroadPeak,    // BED6+3: chr,start,end,name,score,strand + signal,p,q
    GappedPeak,   // BED12+3
    BedGraph,     // BED4: chr,start,end,value(float)
    TagAlign,     // BED6 with col3 = sequence (not Name)
};

std::shared_ptr<arrow::Table> batch_slice_to_table(
    const arrow::RecordBatch& batch,
    const std::vector<int>& col_indices,
    const std::shared_ptr<arrow::Schema>& full_schema);

// Trailing-window size (in decoded batches) for forward-only streaming
// sources. A forward-only stream can't re-read a freed batch, so we keep only
// the most-recent `cap` batches resident and free older ones; their
// (first_row, num_rows) metadata is retained forever so total_rows() /
// chunk_meta() / num_chunks() stay exact. This bounds RAM to ~cap×batch
// regardless of file size: pressing G / deep-scrolling a multi-GB stream no
// longer loads the whole file. Override with VV_STREAM_BATCH_CAP (used by the
// test suite to force eviction on a modest fixture). Operations that must see
// the whole file at once (search / sort / filter / stats) pin retention first
// (set_retain_all), so they keep their current behaviour.
inline int stream_batch_cap() {
    static const int cap = [] {
        if (const char* e = std::getenv("VV_STREAM_BATCH_CAP")) {
            int v = std::atoi(e);
            if (v > 0) return v;
        }
        return 64;
    }();
    return cap;
}

// Per-batch byte budget for record-oriented streaming sources (FASTA/FASTQ).
// Batching purely by record count assumes records are small; a genome FASTA of
// a few chromosome-sized records would otherwise pack gigabytes into one batch,
// defeating the trailing-window eviction above. Cap the accumulated payload per
// batch so memory stays bounded by record size, not file size. A single record
// larger than the budget still forms its own (one-row) batch — no record is
// split or dropped. Override with VV_FASTX_BATCH_BYTES (used by the test suite).
inline int64_t fastx_byte_budget() {
    static const int64_t budget = [] () -> int64_t {
        if (const char* e = std::getenv("VV_FASTX_BATCH_BYTES")) {
            char* end = nullptr;
            long long v = std::strtoll(e, &end, 10);
            if (end != e && v > 0) return (int64_t)v;
        }
        return (int64_t)64 << 20;   // 64 MiB
    }();
    return budget;
}

int64_t csv_block_bytes();

// Append a freshly-decoded batch to a forward-only streaming source's storage:
// record its (first_row, num_rows) metadata (kept forever, so chunk_meta() /
// total_rows() stay exact after eviction) and enforce the bounded trailing
// window — free the batch that just fell out, unless retention is pinned. The
// freed slot stays in `batches` as nullptr so indices remain stable; callers
// detect it (read_chunk returns CapacityError). Shared by every forward-only
// source.
inline void stream_retain(
        std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
        std::vector<int64_t>& first_row,
        std::vector<int64_t>& num_rows,
        int64_t& rows_so_far,
        bool retain_all, bool& evicted_any,
        std::shared_ptr<arrow::RecordBatch> batch) {
    first_row.push_back(rows_so_far);
    num_rows.push_back(batch->num_rows());
    rows_so_far += batch->num_rows();
    batches.push_back(std::move(batch));
    if (!retain_all) {
        int drop = (int)batches.size() - stream_batch_cap();
        if (drop > 0 && batches[drop - 1]) {
            batches[drop - 1].reset();
            evicted_any = true;
        }
    }
}

void split_delimited_line(const std::string& line, char delim,
                          std::vector<std::string>* out);

std::vector<std::string>
leading_zero_columns(const std::vector<std::string>& sample_lines, char delim,
              const std::vector<std::string>& col_names);

std::string open_delimited_source(const std::string& path, DelimKind kind, const std::string& region,
                                  std::unique_ptr<TabularSource>* out, char delim_override = 0,
                                  HeaderMode header_mode = HeaderMode::Auto,
                                  TsvDialect dialect = TsvDialect::None,
                                  const std::vector<std::string>& paf_tags = {});

std::string open_delimited_stream(std::shared_ptr<arrow::io::InputStream> input,
                                  const std::string& path_label, DelimKind kind, bool is_gz,
                                  const std::string& region, std::unique_ptr<TabularSource>* out,
                                  char delim_override = 0, HeaderMode header_mode = HeaderMode::Auto);

void delimited_apply_column_names(TabularSource& src, const std::vector<std::string>& names,
                                  std::string note);

bool delimited_mtx_shape(TabularSource* src, int64_t* rows, int64_t* cols);

void delimited_apply_tenx_sidecar(TabularSource& src, int kind);

void delimited_apply_bed_variant(TabularSource& src, BedVariant v);

// R data (src/formats/rdata.cpp): .rds / .RData data frames, vectors, matrices.
std::string open_rdata_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

// UCSC wiggle (src/formats/wig.cpp): fixedStep / variableStep expanded to
// bedGraph rows.
std::string open_wig_source(const std::string& path, const Config& cfg,
                            std::unique_ptr<TabularSource>* out);

std::string delimited_first_line_after_meta(const std::string& path);

std::string delimited_first_line_after_meta_raw(const std::string& path);

std::vector<std::string> parse_bam_tag_list(const std::string& spec,
                                            std::string* err);

std::string open_bam_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

std::string open_bam_pileup_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

std::string open_bcf_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

std::string open_big_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

std::string open_fastx_source(const std::string& path, bool is_fastq, const Config& cfg, std::unique_ptr<TabularSource>* out);

// Result of the binary heuristic.
enum class TextSniffResult { Text, Binary, Utf16, Utf32 };

TextSniffResult sniff_text(const char* p, size_t n);

std::string text_binary_error(const std::string& what,
                              TextSniffResult r);

// Compressed input (src/core/compress.cpp): the codec a stream starts with,
// by magic bytes, and a decoded stream over it.
enum class StreamCodec { None, Gzip, Zstd, Bz2, Xz };
StreamCodec sniff_codec(const uint8_t* m, size_t n);
// The codec a file starts with; rewinds `rf`.
StreamCodec sniff_file_codec(const std::shared_ptr<arrow::io::ReadableFile>& rf);
const char* codec_label(StreamCodec c);           // "gzip", "zstd", "bzip2", "xz", ""
bool has_compression_suffix(const std::string& path);    // .gz .bgz .zst .zstd .xz .bz2
std::string strip_compression_suffix(const std::string& path);
// `in` decoded with codec `c` (None: `in` itself). "" or an error.
std::string decode_stream(StreamCodec c, std::shared_ptr<arrow::io::InputStream> in,
                          std::shared_ptr<arrow::io::InputStream>* out);
// Open `path` and decode it by its magic bytes; *codec gets the codec found.
std::string open_decoded_file(const std::string& path, std::shared_ptr<arrow::io::InputStream>* out,
                              StreamCodec* codec = nullptr);

std::string open_text_stream(const std::string& label, std::shared_ptr<arrow::io::InputStream> in,
                             std::unique_ptr<TabularSource>* out);

bool text_final_newline(const TabularSource& src);

std::string open_json_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

std::string open_json_stream(const std::string& label, std::shared_ptr<arrow::io::InputStream> input,
                             StreamCodec comp, std::unique_ptr<TabularSource>* out);

bool is_json_source(const TabularSource* src);
namespace vvjson {

bool looks_like_json(const std::string& head);
}  // namespace vvjson

std::shared_ptr<arrow::io::InputStream> json_document_stream(TabularSource* src);

void make_json_stream_source(const std::string& label, std::shared_ptr<arrow::io::InputStream> in,
                             std::unique_ptr<TabularSource>* out);

std::string open_twobit_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out);

std::shared_ptr<arrow::DataType> arrow_type_for_id(arrow::Type::type t);

std::string open_sqlite_source(const std::string& path, std::unique_ptr<TabularSource>* out);

std::vector<std::unique_ptr<TabularSource>> sqlite_sibling_tables(TabularSource* src);

std::string open_ipc_source(const std::string& path, bool is_feather, std::unique_ptr<TabularSource>* out);

std::string open_ipc_stream_file(const std::string& path, std::unique_ptr<TabularSource>* out);

std::string open_ipc_stream(const std::string& label, std::shared_ptr<arrow::io::InputStream> in,
                            std::unique_ptr<TabularSource>* out);

bool looks_like_ipc_stream(const uint8_t* m, size_t n);
#if VV_HAVE_ORC

std::string open_orc_source(const std::string& path, std::unique_ptr<TabularSource>* out);
#endif

void remove_spooled_files();

std::string human_bytes(int64_t sz);

std::string spool_stream(const std::shared_ptr<arrow::io::InputStream>& in,
                         const std::string& ext, std::string* path_out,
                         int64_t* bytes_out, const std::string& progress = "");

bool path_is_pipe(const std::string& path);

// ── In-memory adapter: wrap an Arrow Table as a TabularSource ────────────────
//
// Used by `--sample` (and potentially future row-selecting flags) to feed
// a pre-computed Table through the normal rendering / export pipeline as
// if it had been read from a file.
class MemoryTableSource : public TabularSource {
protected:
    std::shared_ptr<arrow::Table>    table_;
    std::string                       label_;
    std::string                       footer_str_;
    std::vector<std::string>          hidden_;
    bool                              is_text_ = false;

public:
    // Set when this table was derived from a plain-text source (--tail on a
    // .log), so the frontends keep rendering it as text rather than as a
    // one-column table.
    void mark_text() { is_text_ = true; }
    bool is_text() const override { return is_text_; }
protected:
    // For slice navigation: swap the underlying table without
    // re-creating the source (preserves identity for the TUI).
    void replace_table(std::shared_ptr<arrow::Table> t, std::string footer) {
        table_      = std::move(t);
        footer_str_ = std::move(footer);
    }
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

class WorkbookSource : public MemoryTableSource {
public:
    using MemoryTableSource::MemoryTableSource;
    virtual std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const = 0;
    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        return open_sibling_sheets();
    }
};

arrow::Result<std::shared_ptr<arrow::Table>>
csv_buffer_to_table(const std::string& buf);

std::string open_xlsx_source(const std::string& path, std::unique_ptr<TabularSource>* out);

std::string open_ods_source(const std::string& path, std::unique_ptr<TabularSource>* out, bool flat = false);
namespace h5v {

inline constexpr int64_t kDataFrameRowCap = 1000;

std::string open_hdf5_source(const std::string& path, std::unique_ptr<TabularSource>* out,
                             int64_t df_row_cap = kDataFrameRowCap, bool matrix_long = false);

// Zarr stores (src/formats/zarr.cpp): a .zarr directory or a .zarr.zip.
bool is_zarr_dir(const std::string& path);
std::string open_zarr_source(const std::string& path, const Config& cfg,
                             std::unique_ptr<TabularSource>* out);

// Shared with the Cooler reader (src/formats/cooler.cpp).
// A scalar string attribute of an HDF5 object; "" when absent or not a string.
std::string read_string_attr(hid_t obj, const char* name);
// "'<dataset path>': cannot read HDF5 dataset: <why>" (names a missing filter).
std::string h5_read_failure(hid_t dset);
// Register vv's decoders for HDF5 filters libhdf5 lacks (LZF); once per process.
void register_hdf5_filters();
// libhdf5 is usually built without its thread-safe option: no two threads may
// be inside it at once, even on different files. Every HDF5-backed source is
// returned wrapped by lock_hdf5_source, which holds hdf5_mutex() around each
// call (and its destruction); the open functions hold it while they run.
std::recursive_mutex& hdf5_mutex();
std::unique_ptr<TabularSource> lock_hdf5_source(std::unique_ptr<TabularSource> src);
}  // namespace h5v

// The sibling tabs the interactive viewer opens beside a workbook-like source
// (xlsx, ods, HDF5 / AnnData): its sheets; empty for any other source.
std::vector<std::unique_ptr<TabularSource>> workbook_siblings(const TabularSource& src);

// GenBank / EMBL flat files (src/formats/genbank.cpp): features, records, sequences.
std::string open_genbank_source(const std::string& path, const Config& cfg,
                                std::unique_ptr<TabularSource>* out);

// Oxford Nanopore POD5 (src/formats/pod5.cpp): reads / signal / run_info tabs.
std::string open_pod5_source(const std::string& path, std::unique_ptr<TabularSource>* out);

// Text from a file as valid UTF-8 (invalid bytes → U+FFFD) (src/core/common.cpp).
std::string valid_utf8(std::string_view v);

// The obs / var row cap of an AnnData store for this run (src/core/open.cpp):
// the preview cap in the viewer, every row (or -n) for a complete answer.
int64_t anndata_df_row_cap(const Config& cfg);

// DuckDB databases (src/formats/duckdb.cpp; libduckdb optional, VV_HAVE_DUCKDB).
bool is_duckdb_file(const std::string& path);
std::string open_duckdb_source(const std::string& path, const Config& cfg,
                               std::unique_ptr<TabularSource>* out);

// Cooler Hi-C contact matrices (src/formats/cooler.cpp): .cool, .mcool.
bool is_cooler_file(const std::string& path);
std::string open_cooler_source(const std::string& path, const Config& cfg,
                               std::unique_ptr<TabularSource>* out);
namespace npz {

std::string open_npz_source(const std::string& path, std::unique_ptr<TabularSource>* out);

std::string open_npy_source(const std::string& path, std::unique_ptr<TabularSource>* out);
}  // namespace npz

std::string decode_mpileup_to_memory(
 TabularSource& src,
 const std::string& path,
 std::unique_ptr<TabularSource>* out);
namespace md {

// Logical "colour role" for a run. Maps to a g_color field at render
// time so light / dark / solarized themes work without re-rendering.
enum MdRole : uint8_t {
    ROLE_NONE = 0,
    ROLE_H1, ROLE_H2, ROLE_H3, ROLE_H4_PLUS,
    ROLE_CODE,
    ROLE_QUOTE,
    ROLE_LINK,
    ROLE_LINK_URL,
    ROLE_LIST_MARK,
    ROLE_HR,
    ROLE_IMAGE,
};

struct MdSegment {
    std::string text;
    uint16_t    style = 0;
    uint8_t     role  = ROLE_NONE;
    bool        verbatim = false;   // OSC-8 escape etc. — pass through
                                    // emit_line_ansi without SGR wrapping,
                                    // and don't count against the wrap
                                    // budget in wrap_runs.
};

struct MdLine {
    std::vector<MdSegment> runs;
};

enum class MdBlockKind {
    Heading, Paragraph, Code, Quote, List, ListItem,
    HRule, TablePlaceholder, Image, Spacer
};

struct MdBlock {
    MdBlockKind kind = MdBlockKind::Paragraph;
    int         level = 0;             // heading level OR list nesting
    std::vector<MdLine> lines;
    int         table_idx = -1;        // points into MarkdownDoc::tables
    int         image_idx = -1;
};

struct MarkdownDoc {
    std::vector<MdBlock> blocks;
    std::vector<std::shared_ptr<arrow::Table>> tables;
    std::vector<std::string>                   table_captions;
    std::string  source_dir;
    std::string  source_path;
};

enum class ImageProto { None, Kitty, ITerm2 };

ImageProto detect_image_proto();

std::string parse_markdown_file(const std::string& path,
                                 int width,
                                 MarkdownDoc* out);

void emit_via_pager(const std::function<void()>& emit_fn);

void emit_markdown_stdout(const MarkdownDoc& doc);
}  // namespace md

std::vector<std::pair<std::string,std::string>>
parse_kv_list(const std::string& s);

bool looks_like_kv_list(const std::string& s);

std::vector<std::pair<std::string, arrow::Type::type>>
parse_vcf_info_headers(const std::vector<std::string>& preamble);

bool is_expanded_source(const TabularSource& src);

bool text_ext(const std::string& p);

bool has_no_extension(const std::string& path);

std::string open_text(const std::string& path, const Config& cfg,
                      std::unique_ptr<TabularSource>* out,
                      bool note = false);

// A markdown file's embedded GFM tables, one per tab (like a workbook's
// sheets). The CLI renders markdown in main() before it ever reaches
// open_source(); the other libvvcore consumers — the Qt GUI and the KDE
// plugins — do go through open_source(), where a .md previously fell to the
// plain-text reader and showed as a single `line` column. This surfaces its
// tables instead. A markdown file with no table is left to the text reader.
class MarkdownTablesSource : public MemoryTableSource {
    std::vector<std::shared_ptr<arrow::Table>> tables_;
    std::vector<std::string>                   captions_;
    size_t                                     which_;

    static std::string tab_name(const std::vector<std::string>& caps, size_t i) {
        if (i < caps.size() && !caps[i].empty()) return caps[i];
        return "table " + std::to_string(i + 1);
    }
public:
    MarkdownTablesSource(std::vector<std::shared_ptr<arrow::Table>> tables,
                         std::vector<std::string> captions,
                         const std::string& path, size_t which)
        : MemoryTableSource(tables[which], path,
              "Format: markdown table (" + tab_name(captions, which) + ")"),
          tables_(std::move(tables)), captions_(std::move(captions)),
          which_(which) {}

    std::string tab_label() const override { return tab_name(captions_, which_); }

    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        std::vector<std::unique_ptr<TabularSource>> out;
        for (size_t i = 0; i < tables_.size(); ++i) {
            if (i == which_) continue;
            out.push_back(std::make_unique<MarkdownTablesSource>(
                tables_, captions_, path(), i));
        }
        return out;
    }
};

bool fastx_ext(const std::string& path, const std::string& det,
               std::initializer_list<const char*> exts);

std::string open_source_dispatch(const std::string& path, const Config& cfg,
                         std::unique_ptr<TabularSource>* out);

std::string build_contigs(const Config& cfg,
                          std::unique_ptr<TabularSource>* out);

std::string build_seq_stats(const Config& cfg, std::unique_ptr<TabularSource>* out);

std::string render_heatmap(TabularSource& src, const Config& cfg);

std::string write_delimited(TabularSource& src, const Config& cfg);

std::string write_markdown(TabularSource& src, const Config& cfg);

std::string write_parquet(TabularSource& src, const Config& cfg);

std::string write_arrow(TabularSource& src, const Config& cfg);

std::string write_json(TabularSource& src, const Config& cfg);

std::string validate_lociss(const std::string& path);

std::string shorten_reader_error(std::string msg);

std::string print_describe(TabularSource& src, const Config& cfg);

void print_schema_block(TabularSource& src);

void print_schema_metadata(const arrow::Schema& schema);

void emit_metadata_json(const arrow::Schema& schema);

std::string format_label_of(TabularSource& src);

std::string print_stats_only(TabularSource& src, const Config& cfg);

std::string print_unique(TabularSource& src, const Config& cfg);

std::string build_sample(std::unique_ptr<TabularSource>& src,
                          const Config& cfg);

std::string build_tail(std::unique_ptr<TabularSource>& src,
                        const Config& cfg);

std::string build_sort(std::unique_ptr<TabularSource>& src,
                        const Config& cfg);

std::string build_distinct(std::unique_ptr<TabularSource>& src,
                           Config& cfg);
#ifndef VV_CORE_LIB

// The view the TUI opens with, from --select / --filter / --sort, applied as
// its own live state (the column layout, the `&` filter, the `s` sort) so it
// can be changed or cleared in the viewer. Applies to the first file.
struct TuiStart {
    std::vector<int> select;     // source field indices, in --select order; empty = all
    std::string      filter;     // --filter text; empty = none
    std::string      sort_col;   // --sort column; empty = none
    bool             sort_desc = false;
};

std::string run_json_viewer(const std::string& file, const std::string& table_path,
                            const std::string& label, bool lines, bool keys_from_tty,
                            const Config& cfg, bool* term_failed);
#endif

int json_path_kind(const std::string& path);

std::string open_json_file(const std::string& path,
                           std::shared_ptr<arrow::io::InputStream>* out);
#ifndef VV_CORE_LIB

bool run_table_viewer(std::vector<std::unique_ptr<TabularSource>> srcs, const Config& cfg,
                      const TuiStart& start, std::unique_ptr<TabularSource>* first);
#endif

int detect_terminal_width();

std::string print_vertical_table(TabularSource& src, const Config& cfg);

std::string print_table(TabularSource& src, const Config& cfg,
                        bool with_footer = true);

void render_multitab_schema(
 std::vector<std::unique_ptr<TabularSource>>& tabs,
 const std::string& path);

std::string render_multitab_table(
 std::vector<std::unique_ptr<TabularSource>>& tabs,
 const Config& cfg);

void emit_schema_json(TabularSource& src, const std::string& fmt_name);

const char* json_table_flag(const Config& cfg);

std::string print_json_document(arrow::io::InputStream& in, const Config& cfg,
                                bool lines);

enum class DocKind { Markdown = 1, Text = 2 };

std::string document_flag_error(const Config& cfg, DocKind kind);

bool tui_wanted(const Config& cfg);

int emit_text_stream(TabularSource& src, const Config& cfg);

std::string preflight_path(const std::string& path);

std::string preview_refusal(const TabularSource& src, const std::string& mode,
                            int64_t rows_wanted = -1, bool cols_matter = true);

std::string select_tab(std::unique_ptr<TabularSource>& src,
                       const std::string& tab);

// Record counts held in a BAM (.bai / .csi), BCF (.csi) or bgzipped VCF
// (.tbi / .csi) index, per reference sequence — what `samtools idxstats` and
// `bcftools index --stats` print, read without touching the records. A CRAM
// index (.crai) holds none. `names` is the index's own sequence order (a
// tabix index lists only sequences that have records).
struct IndexStats {
    bool                     variant = false;
    std::vector<std::string> names;
    std::vector<uint64_t>    mapped, unmapped;   // unmapped: alignments only
    uint64_t                 no_coor = 0;        // unplaced unmapped reads
    uint64_t total() const {
        uint64_t n = no_coor;
        for (size_t i = 0; i < mapped.size(); ++i) n += mapped[i] + unmapped[i];
        return n;
    }
};

bool read_index_stats(const std::string& path, IndexStats* out);
