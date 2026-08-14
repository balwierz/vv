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
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <numeric>
#include <random>
#include <regex>
#include <set>
#include <sstream>
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
#include <sys/stat.h>

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
// open_source). Defined here in main.cpp; this header is what the Qt GUI and
// the KF6 plugins include to drive libvvcore.
#include "vv/vvcore.hpp"

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
static const Theme kThemeDefault = {
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
static const Theme kThemeDark = {
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
static const Theme kThemeLight = {
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
static const Theme kThemeSolarizedDark = {
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
static const Theme kThemeSolarizedLight = {
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

static const Theme* g_theme = &kThemeDefault;

// ── Terminal background detection (for zebra-stripe contrast) ─────────────────
// The default/dark themes' zebra shade is a near-black grey that only reads as a
// subtle stripe on a dark terminal; on a light terminal (e.g. JupyterLab's web
// terminal) it becomes a hard black band that swallows the default-foreground
// text. Detect the actual background so the stripe can adapt.
enum class TermBg { Unknown, Dark, Light };
static TermBg g_term_bg = TermBg::Unknown;
// Set by the config file's `background` key (load_user_config); consulted by
// detect_term_bg() after the VV_BACKGROUND env override and before the OSC 11
// query — so a configured background also skips the query entirely, which
// slow transports (web consoles) answer too late to be useful anyway.
static TermBg g_config_term_bg = TermBg::Unknown;

// Classify an OSC 11 "]11;rgb:RRRR/GGGG/BBBB" reply (2- or 4-hex-digit
// components) by relative luminance. Unknown if it doesn't parse.
static TermBg classify_osc11_reply(const std::string& s) {
    size_t p = s.find("rgb:");
    if (p == std::string::npos) return TermBg::Unknown;
    p += 4;
    double comp[3];
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = (char)std::tolower((unsigned char)c);
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (int i = 0; i < 3; ++i) {
        unsigned long v = 0; int ndig = 0;
        while (p < s.size()) { int h = hexv(s[p]); if (h < 0) break; v = v * 16 + (unsigned)h; ++p; ++ndig; }
        if (ndig == 0) return TermBg::Unknown;
        comp[i] = (double)v / (double)((1ul << (4 * ndig)) - 1);
        if (i < 2) { if (p < s.size() && s[p] == '/') ++p; else return TermBg::Unknown; }
    }
    double lum = 0.2126 * comp[0] + 0.7152 * comp[1] + 0.0722 * comp[2];
    return lum >= 0.5 ? TermBg::Light : TermBg::Dark;
}

// Best-effort OSC 11 background-colour query on the controlling tty. Short,
// bounded timeout; restores termios; Unknown on no/garbled reply (so callers
// fall back to the dark-terminal default with no regression).
static TermBg query_osc11_bg() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return TermBg::Unknown;
    struct termios saved;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return TermBg::Unknown;
    struct termios raw = saved;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return TermBg::Unknown;

    const char* q = "\033]11;?\033\\";
    ssize_t wr = ::write(STDOUT_FILENO, q, std::strlen(q)); (void)wr;

    std::string resp;
    for (int i = 0; i < 4; ++i) {                 // ≤ ~80 ms total
        struct pollfd pfd; pfd.fd = STDIN_FILENO; pfd.events = POLLIN; pfd.revents = 0;
        int pr = ::poll(&pfd, 1, 20);
        if (pr <= 0) { if (!resp.empty()) break; else continue; }
        char buf[128];
        ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) continue;
        resp.append(buf, (size_t)n);
        if (resp.find('\\') != std::string::npos ||   // ST
            resp.find('\a') != std::string::npos) break;  // BEL
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return classify_osc11_reply(resp);
}

// Resolve the terminal background once (before ncurses takes over the tty):
// explicit VV_BACKGROUND override → OSC 11 query → COLORFGBG hint → Unknown.
static void detect_term_bg() {
    if (const char* e = std::getenv("VV_BACKGROUND")) {
        if (!std::strcmp(e, "light")) { g_term_bg = TermBg::Light; return; }
        if (!std::strcmp(e, "dark"))  { g_term_bg = TermBg::Dark;  return; }
    }
    if (g_config_term_bg != TermBg::Unknown) { g_term_bg = g_config_term_bg; return; }
    g_term_bg = query_osc11_bg();
    if (g_term_bg != TermBg::Unknown) return;
    if (const char* c = std::getenv("COLORFGBG")) {   // "fg;bg" or "fg;;bg"
        std::string s(c);
        size_t pos = s.rfind(';');
        if (pos != std::string::npos && pos + 1 < s.size()) {
            std::string bg = s.substr(pos + 1);
            if (bg == "7" || bg == "15") g_term_bg = TermBg::Light;
            else if (std::isdigit((unsigned char)bg[0])) g_term_bg = TermBg::Dark;
        }
    }
}

// Full list, in the order the TUI picker presents them.
static const Theme* const kAllThemes[] = {
    &kThemeDefault, &kThemeDark, &kThemeLight,
    &kThemeSolarizedDark, &kThemeSolarizedLight,
};
static constexpr int kNumThemes = (int)(sizeof(kAllThemes) / sizeof(kAllThemes[0]));

static const Theme* find_theme(const std::string& name) {
    for (auto* t : kAllThemes) if (name == t->name) return t;
    // Accept a few synonyms for muscle memory.
    if (name == "solarized") return &kThemeSolarizedDark;
    return nullptr;
}

static void init_colors() {
    const Theme& t = *g_theme;
    g_color.reset      = t.reset;
    g_color.border     = t.border;
    g_color.header     = t.header;
    g_color.row_idx    = t.row_idx;
    g_color.null_val   = t.null_val;
    g_color.number     = t.number;
    g_color.bool_true  = t.bool_true;
    g_color.bool_false = t.bool_false;
    g_color.trunc      = t.trunc;
    g_color.type_int   = t.type_int;
    g_color.type_float = t.type_float;
    g_color.type_str   = t.type_str;
    g_color.type_time  = t.type_time;
    g_color.type_bool  = t.type_bool;
    g_color.type_other = t.type_other;
    g_color.meta_key   = t.meta_key;
}

// (Config is defined in vv/vvcore.hpp, included above.)

// ── User-settings persistence (XDG Base Directory Spec) ──────────────────────
//
// The "modern Linux app" idiom: configuration lives at
// $XDG_CONFIG_HOME/vv/config (default $HOME/.config/vv/config), in a
// simple INI-style `key = value` format. Lines starting with `#` are
// comments. Today we read/write a single key (`theme`); the format is
// extensible — future keys (default --threads, --decode-threads, etc.)
// slot in without breaking forward / backward compatibility.

// Return $XDG_CONFIG_HOME/vv or $HOME/.config/vv, "" if neither is set.
static std::string xdg_config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/vv";
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/vv";
    return "";
}

static void strip_ws_inplace(std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    s = s.substr(a, b - a + 1);
}

// Read $XDG/vv/config and populate Config fields whose CLI flag wasn't given.
// Today: only `theme` (empty cfg.theme means "no explicit --theme").
// Missing file / unreadable file is not an error — config is optional.
// Defined out-of-line after the Config struct is fully visible.
static void load_user_config(Config& cfg);

// Write key=value into the config file, preserving any existing lines
// (comments, other keys). Atomic via .tmp + rename. Returns true on
// success — best-effort: silent on permission errors / quota exhaustion
// so a failed write doesn't crash the TUI.
static bool save_user_setting(const std::string& key, const std::string& value) {
    std::string dir = xdg_config_dir();
    if (dir.empty()) return false;
    // mkdir -p $XDG/vv (the parent $XDG_CONFIG_HOME usually exists but
    // create it too just in case — first run on a fresh home).
    auto slash = dir.rfind('/');
    if (slash != std::string::npos) {
        ::mkdir(dir.substr(0, slash).c_str(), 0755);  // ok if exists
    }
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    std::string path = dir + "/config";

    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) lines.push_back(std::move(line));
    }

    bool replaced = false;
    for (auto& line : lines) {
        std::string probe = line;
        auto h = probe.find('#');
        if (h != std::string::npos) probe.erase(h);
        auto eq = probe.find('=');
        if (eq == std::string::npos) continue;
        std::string k = probe.substr(0, eq);
        strip_ws_inplace(k);
        if (k == key) { line = key + " = " + value; replaced = true; break; }
    }
    if (!replaced) lines.push_back(key + " = " + value);

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        // Friendly header for a freshly-created file.
        if (lines.empty() || lines.front().empty() || lines.front()[0] != '#')
            f << "# vv configuration. See `man vv` for the supported keys.\n";
        for (auto& l : lines) f << l << "\n";
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
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
// (ColorMode + Config are defined in vv/vvcore.hpp, included near the top.)

// Out-of-line definition of load_user_config — declared further up
// (near the other XDG-config helpers) but needs the full Config struct.
static void load_user_config(Config& cfg) {
    std::string dir = xdg_config_dir();
    if (dir.empty()) return;
    std::ifstream f(dir + "/config");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto h = line.find('#');
        if (h != std::string::npos) line.erase(h);
        strip_ws_inplace(line);
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        strip_ws_inplace(key);
        strip_ws_inplace(val);
        if (key == "theme" && cfg.theme.empty()) cfg.theme = val;
        else if (key == "scrolloff") {
            // Rows kept between the cell cursor and the viewport edge.
            // Clamped again at use against the window height.
            try {
                int v = std::stoi(val);
                if (v >= 0 && v <= 1000) cfg.scrolloff = v;
            } catch (...) { /* ignore a malformed value */ }
        }
        else if (key == "background") {
            // Same values as VV_BACKGROUND, which wins over this. Also
            // skips the OSC 11 background query — the terminals that need
            // this key (web consoles) answer that query too late anyway.
            if      (val == "dark")  g_config_term_bg = TermBg::Dark;
            else if (val == "light") g_config_term_bg = TermBg::Light;
            // any other value: ignored like every malformed entry
        }
        else if (key == "max_col_width" && !cfg.max_col_w_set) {
            // Same floor as -w; -w on the command line wins.
            try {
                int v = std::stoi(val);
                if (v >= 4 && v <= 100000) cfg.max_col_w = v;
            } catch (...) { /* ignore */ }
        }
        else if (key == "threads" && cfg.threads == 0) {
            // Same as -@; -@ on the command line wins (0 stays "auto").
            try {
                int v = std::stoi(val);
                if (v >= 1 && v <= 1024) cfg.threads = v;
            } catch (...) { /* ignore */ }
        }
        // Unknown keys are ignored: a config written for a newer vv must
        // not break an older one.
    }
}

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

// Dedicated knob for Arrow's CPU thread pool (used for Parquet column
// decode and CSV / TSV parallel parsing). Falls back to --threads when
// not set explicitly. The point of a separate flag is cold Parquet
// reads where decode contends with I/O — bumping decode parallelism
// above --threads can give 1.5-2× on machines with idle cores. Bound
// to twice the hardware concurrency to avoid pathological choices.
static int effective_decode_threads(const Config& cfg) {
    if (cfg.decode_threads > 0) {
        unsigned hc = std::thread::hardware_concurrency();
        int cap = hc > 0 ? (int)(hc * 2) : 32;
        return std::min(cfg.decode_threads, cap);
    }
    return effective_threads(cfg);
}

static constexpr const char* kVersion = "1.18.4";

// ── Format registry ──────────────────────────────────────────────────────────
//
// One authoritative list of what vv reads. Before this table the same
// information was restated in seven places — print_usage(), README.md,
// docs/USAGE.md, man/vv.1, three shell completions and four KDE manifests —
// and they had already drifted apart: `.npz` was missing from all three
// completions, `.arrow`/`.feather` from --help entirely, `.fods` and
// `.ffn`/`.frn` from most of them, while README documented `.npy` and the
// KDE .desktop claimed `.xls` — neither of which vv can open.
//
// `--formats [--json]` prints this table, and tests/run_tests.sh diffs the
// completions against it so the next drift fails CI instead of shipping.
//
// exts: canonical spelling, space-separated. Matching is case-insensitive
// (fends_ci), so `.bigBed` also matches `.bigbed` — but shell globs are
// case-sensitive, which is why the completion generator emits both.
struct FormatInfo {
    const char* name;      // human label
    const char* exts;      // space-separated, leading dot, canonical case
    const char* reader;    // the source class it dispatches to
    bool gz;               // .gz variants dispatched too
    bool region;           // -r honoured (see `region_note` for the condition)
    bool tabs;             // expands into component tabs (--tab / TUI tabs)
    bool streaming;        // forward-only reader (vs random access)
    bool magic;            // also detected by magic bytes on an unknown ext
    const char* region_note;
};

static const FormatInfo kFormats[] = {
  {"Apache Parquet", ".parquet", "ParquetSource",
   false, true,  false, false, true,  "needs chrom/start/end columns (--region-cols)"},
  {"LociSSD", ".lociss", "LocissV4Source / ParquetSource",
   false, true,  false, false, true,  "v3 via row-group stats, v4 via the zone map"},
  {"Arrow IPC", ".arrow", "IpcSource",
   false, false, false, false, true,  ""},
  {"Feather", ".feather", "IpcSource",
   false, false, false, false, true,  ""},
  {"Apache ORC", ".orc", "OrcSource",
   false, false, false, false, false, ""},
  {"BAM / CRAM alignments", ".bam .cram", "BamSource / BamPileupSource",
   false, true,  false, true,  false, "needs a .bai/.csi/.crai index"},
  {"SAM alignments (text)", ".sam", "DelimitedSource",
   false, false, false, true,  false, "no index; convert with `samtools view -b`"},
  {"BCF", ".bcf", "BcfSource",
   false, true,  false, true,  false, "needs a .csi/.tbi index"},
  {"VCF", ".vcf", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"GFF / GFF3 / GTF", ".gff .gff3 .gtf", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"BED", ".bed", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"ENCODE peak / signal", ".narrowPeak .broadPeak .gappedPeak .bedGraph .bg .tagAlign",
   "DelimitedSource", true, true, false, true, false, "bgzip + tabix"},
  {"Delimited text", ".tsv .csv", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"samtools mpileup", ".pileup .mpileup .pile", "DelimitedSource",
   true,  true,  false, true,  false, "bgzip + tabix"},
  {"PAF (minimap2)", ".paf", "DelimitedSource",
   true,  false, false, true,  false, ""},
  {"FASTA", ".fa .fasta .fna .faa .ffn .frn", "FastxSource",
   true,  false, false, true,  false, ""},
  {"FASTQ", ".fq .fastq", "FastxSource",
   true,  false, false, true,  false, ""},
  {"UCSC bigBed / bigWig", ".bb .bigBed .bw .bigWig", "BigSource",
   false, true,  false, true,  false, "native block-level overlap; no sidecar index"},
  {"UCSC 2bit", ".2bit", "TwoBitSource",
   false, false, false, false, false, ""},
  {"SQLite", ".sqlite .sqlite3 .db", "SqliteSource",
   false, false, true,  true,  false, ""},
  {"Excel workbook", ".xlsx .xlsm", "XlsxSource",
   false, false, true,  false, false, ""},
  {"OpenDocument spreadsheet", ".ods", "OdsSource",
   false, false, true,  false, false, ""},
  {"HDF5 / AnnData / Loom", ".h5ad .h5 .hdf5 .loom", "Hdf5Source",
   false, false, true,  false, false, ""},
  {"NumPy archive", ".npz", "NpzSource",
   false, false, true,  false, false, ""},
  {"NumPy array", ".npy", "NpzSource",
   false, false, false, false, false, ""},
  {"Markdown", ".md .markdown .mdown .mkd", "md4c renderer",
   false, false, false, false, false, ""},
  // Plain text is last on purpose: it is the fallback, and any file no other
  // row claims is content-sniffed into it. The extension list is short by
  // design — .py / .c / .conf / .toml reach the same reader through the
  // sniff, and claiming them here would advertise vv as a code viewer.
  {"Plain text", ".txt .text .log", "TextSource",
   true,  false, false, true,  true,  ""},
};
static constexpr size_t kNumFormats = sizeof(kFormats) / sizeof(kFormats[0]);

// Split a FormatInfo::exts blob into individual extensions.
static std::vector<std::string> format_ext_list(const FormatInfo& f) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = f.exts; ; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur += *p;
        }
    }
    return out;
}

// Defined with the other JSON helpers further down.
static void json_emit_string(const std::string& v);

// `--formats` / `--formats --json`: print the registry. The JSON form is what
// the CI drift check and the completion generator consume.
static void print_formats(bool as_json) {
    if (as_json) {
        std::printf("[");
        for (size_t i = 0; i < kNumFormats; ++i) {
            const FormatInfo& f = kFormats[i];
            if (i) std::printf(", ");
            std::printf("{\"name\": ");
            json_emit_string(f.name);
            std::printf(", \"reader\": ");
            json_emit_string(f.reader);
            std::printf(", \"extensions\": [");
            auto exts = format_ext_list(f);
            for (size_t k = 0; k < exts.size(); ++k) {
                if (k) std::printf(", ");
                json_emit_string(exts[k]);
            }
            std::printf("], \"gz\": %s", f.gz ? "true" : "false");
            std::printf(", \"region\": %s", f.region ? "true" : "false");
            std::printf(", \"tabs\": %s", f.tabs ? "true" : "false");
            std::printf(", \"streaming\": %s", f.streaming ? "true" : "false");
            std::printf(", \"magic\": %s", f.magic ? "true" : "false");
            if (*f.region_note) {
                std::printf(", \"region_note\": ");
                json_emit_string(f.region_note);
            }
            std::printf("}");
        }
        std::printf("]\n");
        return;
    }
    size_t w = 4;
    for (const auto& f : kFormats) w = std::max(w, std::strlen(f.name));
    std::printf("%-*s  %-8s %-6s %-4s %-9s %s\n",
                (int)w, "Format", "gz", "region", "tabs", "streaming", "extensions");
    for (const auto& f : kFormats) {
        std::printf("%-*s  %-8s %-6s %-4s %-9s %s\n",
                    (int)w, f.name,
                    f.gz     ? "yes" : "-",
                    f.region ? "yes" : "-",
                    f.tabs   ? "yes" : "-",
                    f.streaming ? "stream" : "random",
                    f.exts);
    }
}

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "vv -- universal genomic file viewer\n"
        "\nUsage: %s [options] <file>\n"
        "\nSupported formats:\n"
        "  .parquet\n"
        "  .arrow  .feather          Arrow IPC / Feather (v1 and v2)\n"
        "  .lociss                     LociSSD sorted-interval — v3 Parquet (manifest\n"
        "                              in KV) or v4 \"colblock\" binary; dispatched by magic\n"
        "  .bam  .cram                  binary/compressed sequence alignments (htslib)\n"
        "  .sam                        text sequence alignments\n"
        "  .vcf  .vcf.gz               variant calls\n"
        "  .gff  .gff3  .gtf           and .gz variants  genome annotations\n"
        "  .bed  .tsv  .csv            and .gz variants\n"
        "  .pileup  .mpileup  .pile        samtools mpileup output (single- or multi-sample)\n"
        "  .narrowPeak  .broadPeak  .gappedPeak  .bedGraph  .bg  .tagAlign\n"
        "                              ENCODE peak / signal formats (BED+typed cols)\n"
        "  .bb  .bigBed                UCSC bigBed (libBigWig; autoSql columns)\n"
        "  .bw  .bigWig                UCSC bigWig (libBigWig)\n"
        "  .2bit                       UCSC 2bit (sequence index: name/length/blocks)\n"
        "  .sqlite  .sqlite3  .db      SQLite database (each table → one TUI tab)\n"
        "  .xlsx  .xlsm                Excel spreadsheet (each sheet → one TUI tab)\n"
        "  .ods                OpenDocument spreadsheet (each sheet → one\n"
        "                              TUI tab)\n"
        "  .h5ad                       AnnData (single-cell) — obs / var / X / obsm tabs\n"
        "  .h5  .hdf5  .loom           generic HDF5 — hierarchy tab + per-dataset tabs\n"
        "  .npz                        NumPy archive — summary tab + per-array tabs (3-D+ scrubs via [/])\n"
        "  .npy                        NumPy single array\n"
        "  .orc                        Apache ORC (columnar; one stripe → one chunk)\n"
        "  .md  .markdown  .mdown  .mkd\n"
        "                              CommonMark + GFM markdown (renders as ANSI;\n"
        "                              GFM tables routed through the table renderer)\n"
        "  .fa  .fasta  .fna  .faa  .ffn  .frn\n"
        "                              sequences (FASTA, plus .gz)\n"
        "  .fq  .fastq                 sequencing reads (FASTQ, plus .gz)\n"
        "  .bcf                        binary VCF (htslib)\n"
        "  .paf  .paf.gz               minimap2 pairwise alignments\n"
        "  .txt  .text  .log           plain text (also .gz; viewed like less -SN,\n"
        "                              not tabulated). The fallback for any file\n"
        "                              no other format claims.\n"
        "  -                           read text format from stdin (auto-gunzip)\n"
        "  (`vv --formats` prints this table with capability columns;\n"
        "   add --json for the machine-readable form)\n"
        "  (unknown extensions: identified by magic bytes, else sniffed as text;\n"
        "   binary files are refused — vv has no hex view)\n"
        "\nInteractive viewer (default when stdout is a terminal):\n"
        "  -i / --interactive  open the ncurses row browser\n"
        "  --no-interactive    force plain table output even on a terminal\n"        "  --text              read the file as plain text whatever its extension\n"
        "  Keys: arrows/hjkl move the cell cursor, PgUp/PgDn, g/G, /:search,\n"
        "        S:column-stats, s:sort by current column (u clears),\n"
        "        &:live filter, c:show/hide columns, y:copy cell (OSC52),\n"
        "        T:pick a theme (saved to ~/.config/vv/config),\n"
        "        ::command line (:<N> jump, :q quit, :theme NAME),\n"
        "        Tab / Shift-Tab: switch files (with multiple positionals),\n"
        "        H/F1: in-app help, q quit\n"
        "\nTable options:\n"
        "  -n <rows>           rows to display  (default: 10, 0 = all)\n"
        "  --tail <N>          show the last N rows instead of the first N\n"
        "  -w <width>          max cell width   (default: 32)\n"
        "  -c <cols>           max columns to show (default: all)\n"
        "  --select <terms>    project columns. Comma-separated; output follows\n"
        "                      the given order, so it also reorders. Terms:\n"
        "                        Chr        an exact column name (always wins)\n"
        "                        chr*       a glob (* and ?)\n"
        "                        2-4, 5-    a 1-based inclusive index range\n"
        "                        @numeric   a type class: @numeric @string\n"
        "                                   @list @bool @temporal\n"
        "                        !TERM      exclude everything TERM matches\n"
        "                      e.g. --select 'chr*,!*_pct'  (quote it — the\n"
        "                      shell would otherwise expand * itself)\n"
        "  --filter <expr>     keep rows matching: <col> <op> <value>, joined by\n"
        "                      AND / OR. Operators:\n"
        "                        == != < <= > >=   compare\n"
        "                        ~  !~             regex (ECMAScript), unanchored\n"
        "                        contains startswith endswith\n"
        "                        in (a, b, c)      set membership; `not in` too\n"
        "                        is null / is not null\n"
        "                      e.g. --filter 'Score > 0.5'\n"
        "                           --filter 'FILTER is null OR Gene ~ \"^BRCA\"'\n"
        "  --schema            print schema + file metadata and exit\n"
        "                      (with --json: machine-readable; `rows` is null\n"
        "                      when the file has not been fully scanned)\n"
        "  --expand <col>      unpack a packed key=value column into real\n"
        "                      columns, appended to the schema — VCF INFO,\n"
        "                      GFF/GTF attributes. The keys then work with\n"
        "                      --select / --filter / --parquet like any other\n"
        "                      column, e.g.\n"
        "                        vv v.vcf --expand INFO --filter \'AF > 0.05\'\n"
        "                      VCF types come from the ##INFO declarations.\n"
        "                      GFF/GTF declares nothing, so keys are taken\n"
        "                      from the first chunk: a -n preview and a full\n"
        "                      scan CAN disagree on the column set.\n"
        "  --list-columns      column names, one per line\n"
        "  --list-tabs         component tab labels, one per line\n"
        "  --formats           the supported-format table (add --json)\n"
        "  --tab <name>        view a named component tab (AnnData obs/var/X,\n"
        "                      a workbook sheet, …) instead of the first; e.g.\n"
        "                      `vv cells.h5ad --tab obs -n 20`\n"
        "  --describe          per-column statistics and exit (add --json /\n"
        "                      --ndjson for machine-readable stats)\n"
        "  --count             print the row count and exit (honours -r and\n"
        "                      --filter)\n"
        "  --stats             print Parquet metadata footer (row groups, codecs,\n"
        "                      per-column sizes) without reading data; exit\n"
        "  --unique <cols>     comma-separated columns: print distinct-value counts\n"
        "  --sample <N>        reservoir-sample N rows uniformly instead of head-N\n"
        "  --validate          check LociSSD invariants (sort order, MaxEndSoFar,\n"
        "                      manifest vs. data); exit non-zero on failure\n"
        "  --pileup            BAM/CRAM only: emit mpileup-style per-base rows\n"
        "                      from the alignments via htslib's bam_plp engine\n"
        "                      (equivalent to `samtools mpileup`, no BAQ)\n"
        "  -f, --fasta <ref>   reference FASTA (needs .fai). With --pileup it\n"
        "                      fills the ref column and renders matches as\n"
        "                      . / , like `samtools mpileup -f`; on a CRAM\n"
        "                      input it supplies the reference needed to\n"
        "                      decode the reads (otherwise htslib falls back\n"
        "                      to $REF_PATH / $REF_CACHE)\n"
        "  --decode-pileup     mpileup only: replace the packed bases/quals\n"
        "                      columns with typed per-allele counts\n"
        "                      (A, C, G, T, N, del, ins, fwd, rev, mean_qual)\n"
        "  --no-index          suppress the row-index column\n"
        "  --color[=WHEN]      colorize output: auto (default), always, never\n"
        "  --theme <name>      color palette: default, dark, light,\n"
        "                      solarized-dark, solarized-light (default = default)\n"
        "  --vertical          \"vertical head\": transpose the preview so each\n"
        "                      field is a row; show as many records per line as\n"
        "                      fit. Implies --no-interactive. Default when the\n"
        "                      binary is invoked as `vh`.\n"
        "\nVisualization (replaces table view):\n"
        "  --heatmap           render the numeric columns as a colour heatmap in\n"
        "                      the terminal (rows x numeric-columns, globally\n"
        "                      normalised). Writes a plain ASCII grid when stdout\n"
        "                      is not a terminal.\n"
        "  --image-mode <how>  heatmap backend: auto (default), kitty, sixel,\n"
        "                      halfblock, ascii\n"
        "\nDelimited output (replaces table view):\n"
        "  --tsv               write tab-separated values to stdout\n"
        "  --csv               write comma-separated values to stdout\n"
        "  --json              write a JSON array of row objects to stdout\n"
        "  --ndjson            write one JSON object per line (JSON Lines)\n"
        "  --md / --markdown   write a GitHub-flavored markdown table\n"
        "  --delimiter <sep>   write with a custom single-character delimiter\n"
        "  --no-header         omit the header row\n"
        "  (-n defaults to all rows in this mode; -c still applies)\n"
        "\nParquet / Arrow output (replaces table view):\n"
        "  --parquet <file>    write a Parquet file at <file> (or `-` for stdout)\n"
        "  --arrow, --feather <file>\n"
        "                      write an Arrow IPC file (Feather v2) at <file>\n"
        "                      (or `-` for stdout)\n"
        "  --compression <c>   Parquet: zstd (default), snappy, gzip, lz4, none;\n"
        "                      Arrow/Feather: zstd (default), lz4, none\n"
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
        "  --coords <kind>          coordinate convention for -r: UCSC\n"
        "                           (default; 0-based half-open, as in BED)\n"
        "                           or NCBI (1-based inclusive, as in GenBank,\n"
        "                           VCF, GFF, and the samtools/tabix CLI)\n"
        "  Supported on indexed BAM/CRAM (.bai/.csi/.crai), tabix-indexed\n"
        "  VCF/BED/GFF/TSV, indexed BCF (.csi/.tbi), LociSSD (.lociss),\n"
        "  plain sorted Parquet with chrom/start/end columns, and\n"
        "  bigBed/bigWig. Formats with no region index warn on stderr and\n"
        "  show the whole file. Coordinates follow the UCSC convention\n"
        "  (0-based half-open) by default; pass --coords NCBI for 1-based\n"
        "  inclusive (samtools/tabix style).\n"
        "\nPerformance:\n"
        "  -@ / --threads <N>  worker threads for I/O and decode (0 = auto)\n"
        "  --decode-threads <N>  Arrow CPU thread pool size for Parquet /\n"
        "                       CSV decode (0 = follow --threads; useful when\n"
        "                       cold reads bottleneck on column decompression)\n"
        "\n  -h / --help         show this help\n"
        "  -V / --version      print version and exit\n",
        prog);
}

// Case-insensitive suffix test; defined with the other path helpers below.
static bool fends_ci(const std::string& s, const std::string& sfx);

// True when --vertical came from being invoked as `vh`, not from the user
// typing it. Document modes (markdown, plain text) reject a TYPED --vertical
// but must not fail `vh notes.md` on a flag nobody asked for.
static bool g_vertical_from_argv0 = false;

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
            g_vertical_from_argv0 = true;
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
            cfg.max_col_w_set = true;
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
            // NCBI / GenBank / tabix / VCF / samtools: 1-based inclusive
            if (!std::strcmp(v, "ncbi") || !std::strcmp(v, "NCBI") ||
                !std::strcmp(v, "genbank") || !std::strcmp(v, "GenBank") ||
                !std::strcmp(v, "1-based") || !std::strcmp(v, "1based") ||
                !std::strcmp(v, "tabix"))
                cfg.coords_one_based = true;
            // UCSC / Kent / BED tools: 0-based half-open
            else if (!std::strcmp(v, "ucsc") || !std::strcmp(v, "UCSC") ||
                     !std::strcmp(v, "kent") || !std::strcmp(v, "Kent") ||
                     !std::strcmp(v, "0-based") || !std::strcmp(v, "0based") ||
                     !std::strcmp(v, "bed"))
                cfg.coords_one_based = false;
            else {
                std::fprintf(stderr,
                    "--coords: expected 'UCSC' (0-based half-open, default) "
                    "or 'NCBI' (1-based inclusive), got %s\n", v);
                std::exit(2);
            }
        } else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) {
            cfg.tail_rows     = std::max(0, std::atoi(argv[++i]));
            cfg.tail_rows_set = true;
        } else if ((!std::strcmp(argv[i], "-@") ||
                    !std::strcmp(argv[i], "--threads")) && i + 1 < argc) {
            cfg.threads = std::max(0, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--decode-threads") && i + 1 < argc) {
            cfg.decode_threads = std::max(0, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--parquet") && i + 1 < argc) {
            cfg.parquet_out = argv[++i];
        } else if ((!std::strcmp(argv[i], "--arrow") ||
                    !std::strcmp(argv[i], "--feather")) && i + 1 < argc) {
            cfg.arrow_out = argv[++i];
        } else if (!std::strcmp(argv[i], "--compression") && i + 1 < argc) {
            cfg.compression = argv[++i];
        } else if (!std::strcmp(argv[i], "--expand") && i + 1 < argc) {
            cfg.expand_col = argv[++i];
        } else if (!std::strcmp(argv[i], "--list-columns")) {
            cfg.list_columns = true;
        } else if (!std::strcmp(argv[i], "--list-tabs")) {
            cfg.list_tabs = true;
        } else if (!std::strcmp(argv[i], "--formats")) {
            cfg.list_formats = true;
        } else if (!std::strcmp(argv[i], "--schema")) {
            cfg.schema_only = true;
        } else if (!std::strcmp(argv[i], "--describe")) {
            cfg.describe = true;
        } else if (!std::strcmp(argv[i], "--count")) {
            cfg.count = true;
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
        } else if (!std::strcmp(argv[i], "--text")) {
            cfg.force_text = true;
        } else if (!std::strcmp(argv[i], "--validate")) {
            cfg.validate = true;
        } else if (!std::strcmp(argv[i], "--decode-pileup")) {
            cfg.decode_pileup = true;
        } else if (!std::strcmp(argv[i], "--pileup")) {
            cfg.pileup = true;
        } else if ((!std::strcmp(argv[i], "-f") ||
                    !std::strcmp(argv[i], "--fasta")) && i + 1 < argc) {
            cfg.pileup_ref = argv[++i];
        } else if (!std::strcmp(argv[i], "--heatmap")) {
            cfg.heatmap = true;
        } else if (!std::strcmp(argv[i], "--image-mode") && i + 1 < argc) {
            cfg.image_mode = argv[++i];
        } else if (!std::strcmp(argv[i], "--tab") && i + 1 < argc) {
            cfg.tab = argv[++i];
        } else if (!std::strcmp(argv[i], "--theme") && i + 1 < argc) {
            cfg.theme = argv[++i];
        } else if (!std::strcmp(argv[i], "--color=auto")) {
            cfg.color = ColorMode::Auto;
        } else if (!std::strcmp(argv[i], "--color=always")) {
            cfg.color = ColorMode::Always;
        } else if (!std::strcmp(argv[i], "--color=never")) {
            cfg.color = ColorMode::Never;
        } else if (!std::strcmp(argv[i], "--color")) {
            // Also accept the space-separated form "--color MODE" (GNU-style);
            // a bare "--color" with no mode (or a non-mode next token, e.g. a
            // filename) means auto.
            if (i + 1 < argc && (!std::strcmp(argv[i + 1], "auto") ||
                                 !std::strcmp(argv[i + 1], "always") ||
                                 !std::strcmp(argv[i + 1], "never"))) {
                const char* m = argv[++i];
                cfg.color = !std::strcmp(m, "always") ? ColorMode::Always
                          : !std::strcmp(m, "never")  ? ColorMode::Never
                                                      : ColorMode::Auto;
            } else {
                cfg.color = ColorMode::Auto;
            }
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
            // Bare "-" means stdin; treat as a positional argument. Every
            // positional becomes a TUI tab; the first one keeps cfg.path
            // as the "primary" file for non-interactive output modes.
            if (positional++ == 0) cfg.path = argv[i];
            cfg.paths.push_back(argv[i]);
        } else {
            // A known flag that takes an argument falls through to here only
            // when it's the last token (its `i + 1 < argc` guard failed), so
            // report the missing argument specifically rather than the
            // misleading "Unknown option".
            static const std::set<std::string> needs_arg = {
                "-n", "-w", "-c", "-r", "--region", "--window",
                "--regions-file", "--region-cols", "--slop", "--coords",
                "--tail", "-@", "--threads", "--decode-threads", "--parquet",
                "--arrow", "--feather",
                "--compression", "--unique", "--sample", "--filter",
                "--select", "--cols", "--image-mode", "--tab", "--theme",
                "--expand",
                "--delimiter", "-f", "--fasta",
            };
            if (needs_arg.count(argv[i])) {
                std::fprintf(stderr, "Option %s requires an argument.\n", argv[i]);
                std::exit(2);
            }
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]); std::exit(1);
        }
    }
    // --formats describes vv itself, so it takes no input file.
    if (cfg.list_formats) return cfg;
    if (cfg.path.empty()) { print_usage(argv[0]); std::exit(1); }
    // NO_COLOR (https://no-color.org): any non-empty value disables colour,
    // unless the user explicitly chose --color=always/never (those win, per the
    // spec). Resolving it into cfg.color here means every downstream consumer
    // (table, delimited, markdown, heatmap, TUI) honours it from one place.
    if (cfg.color == ColorMode::Auto) {
        if (const char* e = std::getenv("NO_COLOR"); e && e[0])
            cfg.color = ColorMode::Never;
    }
    // Validate --image-mode up front so a typo is reported rather than silently
    // ignored (the heatmap renderer also checks, but only when --heatmap runs).
    if (!cfg.image_mode.empty() && cfg.image_mode != "auto" &&
        cfg.image_mode != "kitty" && cfg.image_mode != "sixel" &&
        cfg.image_mode != "halfblock" && cfg.image_mode != "ascii") {
        std::fprintf(stderr, "--image-mode: unknown mode '%s' "
                     "(use auto|kitty|sixel|halfblock|ascii)\n",
                     cfg.image_mode.c_str());
        std::exit(2);
    }
    // -f/--fasta feeds two things: the reference-aware pileup, and CRAM
    // reference resolution (CRAM stores bases as differences from a
    // reference, so decoding one without $REF_PATH / $REF_CACHE needs it).
    // It adds nothing to a plain BAM read without --pileup, so that stays a
    // usage error rather than a silently ignored flag.
    if (!cfg.pileup_ref.empty() && !cfg.pileup && !fends_ci(cfg.path, ".cram")) {
        std::fprintf(stderr, "-f/--fasta applies to --pileup "
                     "(reference-aware pileup) or to a CRAM input "
                     "(reference resolution)\n");
        std::exit(2);
    }
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

// Base64 (RFC 4648). Used by the OSC52 clipboard escape — small enough
// to avoid pulling in a dependency.
static std::string base64_encode(const std::string& in) {
    static const char* B =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8)
                     | (uint8_t)in[i+2];
        out += B[(v >> 18) & 63];
        out += B[(v >> 12) & 63];
        out += B[(v >>  6) & 63];
        out += B[(v >>  0) & 63];
    }
    if (i < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (uint8_t)in[i+1] << 8;
        out += B[(v >> 18) & 63];
        out += B[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? B[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Copy a string to the terminal's clipboard via OSC52. Supported by
// iTerm2, kitty, alacritty, foot, wezterm, tmux 3.3+, and most modern
// emulators — including over SSH (no xclip / pbcopy needed). The
// escape paints nothing on screen; the terminal intercepts it and
// updates the system clipboard.
static void osc52_copy(const std::string& s) {
    std::string esc = "\033]52;c;" + base64_encode(s) + "\a";
    std::fwrite(esc.data(), 1, esc.size(), stdout);
    std::fflush(stdout);
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

// Decode one UTF-8 codepoint at s[i]; *len receives its byte length. An invalid
// lead byte or a truncated sequence decodes as the single byte (length 1) so we
// always make forward progress and never read past the end.
static uint32_t utf8_decode(const std::string& s, size_t i, int* len) {
    unsigned char c = (unsigned char)s[i];
    auto cont = [&](size_t k) { return k < s.size() &&
                                       ((unsigned char)s[k] & 0xC0u) == 0x80u; };
    if (c < 0x80) { *len = 1; return c; }
    if ((c & 0xE0u) == 0xC0u && cont(i + 1)) {
        *len = 2; return ((c & 0x1Fu) << 6) | ((unsigned char)s[i+1] & 0x3Fu);
    }
    if ((c & 0xF0u) == 0xE0u && cont(i + 1) && cont(i + 2)) {
        *len = 3; return ((c & 0x0Fu) << 12) | (((unsigned char)s[i+1] & 0x3Fu) << 6)
                       | ((unsigned char)s[i+2] & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        *len = 4; return ((c & 0x07u) << 18) | (((unsigned char)s[i+1] & 0x3Fu) << 12)
                       | (((unsigned char)s[i+2] & 0x3Fu) << 6)
                       | ((unsigned char)s[i+3] & 0x3Fu);
    }
    *len = 1; return c;
}

// Terminal column width of one codepoint (wcwidth-style): 0 for combining /
// zero-width / control, 2 for East Asian Wide & Fullwidth and emoji, else 1.
// Sorted ranges (binary search); a wide/combining char counted as 1 (or a
// combining mark counted as 1) is what previously misaligned the table.
static bool cp_in_ranges(uint32_t cp, const uint32_t (*r)[2], size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) >> 1;
        if (cp < r[mid][0]) hi = mid;
        else if (cp > r[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}
static int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20u || (cp >= 0x7Fu && cp < 0xA0u)) return 0;   // C0 / C1 control
    static const uint32_t kZeroWidth[][2] = {
        {0x0300,0x036F},{0x0483,0x0489},{0x0591,0x05BD},{0x05BF,0x05BF},
        {0x05C1,0x05C2},{0x05C4,0x05C5},{0x05C7,0x05C7},{0x0610,0x061A},
        {0x064B,0x065F},{0x0670,0x0670},{0x06D6,0x06DC},{0x06DF,0x06E4},
        {0x06E7,0x06E8},{0x06EA,0x06ED},{0x0711,0x0711},{0x0730,0x074A},
        {0x07A6,0x07B0},{0x07EB,0x07F3},{0x0816,0x0823},{0x0825,0x082D},
        {0x0859,0x085B},{0x08E3,0x0902},{0x093C,0x093C},{0x0941,0x0948},
        {0x094D,0x094D},{0x0951,0x0957},{0x0962,0x0963},{0x0981,0x0981},
        {0x09BC,0x09BC},{0x09C1,0x09C4},{0x09CD,0x09CD},{0x0A3C,0x0A3C},
        {0x0A41,0x0A42},{0x0A47,0x0A48},{0x0A4B,0x0A4D},{0x0ABC,0x0ABC},
        {0x0AC1,0x0AC5},{0x0AC7,0x0AC8},{0x0ACD,0x0ACD},{0x0B3C,0x0B3C},
        {0x0B41,0x0B44},{0x0B4D,0x0B4D},{0x0BC0,0x0BC0},{0x0BCD,0x0BCD},
        {0x0C3E,0x0C40},{0x0C46,0x0C48},{0x0C4A,0x0C4D},{0x0CBC,0x0CBC},
        {0x0CCC,0x0CCD},{0x0D41,0x0D44},{0x0D4D,0x0D4D},{0x0DCA,0x0DCA},
        {0x0E31,0x0E31},{0x0E34,0x0E3A},{0x0E47,0x0E4E},{0x0EB1,0x0EB1},
        {0x0EB4,0x0EBC},{0x0EC8,0x0ECD},{0x0F71,0x0F7E},{0x0F80,0x0F84},
        {0x0F90,0x0F97},{0x0F99,0x0FBC},{0x102D,0x1030},{0x1032,0x1037},
        {0x1039,0x103A},{0x103D,0x103E},{0x1058,0x1059},{0x135D,0x135F},
        {0x1712,0x1714},{0x1732,0x1734},{0x1A17,0x1A18},{0x1AB0,0x1AFF},
        {0x1B6B,0x1B73},{0x1DC0,0x1DFF},{0x200B,0x200F},{0x202A,0x202E},
        {0x2060,0x2064},{0x206A,0x206F},{0x20D0,0x20FF},{0x302A,0x302D},
        {0x3099,0x309A},{0xFB1E,0xFB1E},{0xFE00,0xFE0F},{0xFE20,0xFE2F},
        {0xFEFF,0xFEFF},{0xFFF9,0xFFFB},{0xE0100,0xE01EF},
    };
    static const uint32_t kWide[][2] = {
        {0x1100,0x115F},{0x2329,0x232A},{0x2E80,0x303E},{0x3041,0x33FF},
        {0x3400,0x4DBF},{0x4E00,0x9FFF},{0xA000,0xA4CF},{0xAC00,0xD7A3},
        {0xF900,0xFAFF},{0xFE10,0xFE19},{0xFE30,0xFE6F},{0xFF00,0xFF60},
        {0xFFE0,0xFFE6},{0x1F300,0x1F64F},{0x1F900,0x1F9FF},{0x1FA70,0x1FAFF},
        {0x20000,0x3FFFD},
    };
    if (cp_in_ranges(cp, kZeroWidth, sizeof(kZeroWidth)/sizeof(*kZeroWidth)))
        return 0;
    if (cp_in_ranges(cp, kWide, sizeof(kWide)/sizeof(*kWide)))
        return 2;
    return 1;
}

int display_width(const std::string& s) {
    // Sum terminal column widths so wide (CJK / fullwidth / emoji) and
    // zero-width / combining characters are accounted for, not just codepoints.
    int w = 0;
    for (size_t i = 0; i < s.size(); ) {
        int len = 1;
        w += codepoint_width(utf8_decode(s, i, &len));
        i += len;
    }
    return w;
}

// ── Text-line rendering helpers ──────────────────────────────────────────────

// The slice of `s` occupying display columns [start, start+width). Used to
// scroll a text line sideways. Wide characters are kept whole: one straddling
// either edge is dropped rather than half-painted, so the result never
// desynchronises the terminal's column count.
static std::string sub_display(const std::string& s, int start, int width) {
    if (width <= 0) return "";
    std::string out;
    int col = 0;
    for (size_t i = 0; i < s.size(); ) {
        int len = 1;
        int cw = codepoint_width(utf8_decode(s, i, &len));
        if (col >= start + width) break;
        if (col >= start && col + cw <= start + width)
            out.append(s, i, (size_t)len);
        col += cw;
        i += (size_t)len;
    }
    return out;
}

// Byte offset of the longest prefix of s whose display width is <= max_cols.
// Never splits a codepoint and never includes a wide char that would overflow,
// so truncation cuts on a terminal-column boundary (consistent with
// display_width) rather than a raw byte or codepoint count.
static size_t utf8_prefix_for_width(const std::string& s, int max_cols) {
    if (max_cols <= 0) return 0;
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        int len = 1;
        int cw = codepoint_width(utf8_decode(s, i, &len));
        if (w + cw > max_cols) break;
        w += cw;
        i += len;
    }
    return i;
}

std::string truncate(const std::string& s, int max_w) {
    if (max_w < 2) max_w = 2;
    if (display_width(s) <= max_w) return s;

    // Lists [..], tuples (..), maps {..}: prefer keeping as many leading
    // elements visible as possible — render
    //   "<open>e1, e2, … en, …<close>"
    // with the largest n that fits in max_w. Falls back to "[e1, …]"
    // when even one element + ellipsis doesn't fit.
    if (s.size() >= 2) {
        char open  = s.front();
        char close = (open == '[') ? ']' : (open == '(') ? ')' : (open == '{') ? '}' : 0;
        if (close && s.back() == close) {
            // Collect top-level comma boundaries.
            std::vector<size_t> commas;
            int depth = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                char c = s[i];
                if (c == '[' || c == '(' || c == '{') ++depth;
                else if (c == ']' || c == ')' || c == '}') --depth;
                else if (depth == 1 && c == ',' && i + 1 < s.size() && s[i+1] == ' ')
                    commas.push_back(i);
            }
            if (!commas.empty()) {
                // Try the largest prefix that fits, down to 1 element.
                for (size_t n = commas.size(); n >= 1; --n) {
                    std::string cand;
                    cand += open;
                    cand.append(s, 1, commas[n - 1] - 1);  // up to "eN"
                    cand += ", ";
                    cand += ELLIPSIS;
                    cand += close;
                    if (display_width(cand) <= max_w) return cand;
                }
            }
        }
    }

    // ELLIPSIS is 3 UTF-8 bytes but 1 display column, so we keep content up to
    // (max_w-1) display columns. Cut on a terminal-column boundary — a
    // byte-based substr would split a multibyte codepoint (invalid UTF-8) and a
    // codepoint-based one would overshoot the column budget for wide chars.
    return s.substr(0, utf8_prefix_for_width(s, max_w - 1)) + ELLIPSIS;
}

// Format a decimal integer string with '_' grouping every three digits
// (Python PEP 515 style).  A leading '-' or '+' is preserved.
// e.g. "123456789" → "123_456_789", "-1000000" → "-1_000_000".
// Non-numeric strings pass through unchanged.
std::string digits_with_sep(const std::string& s) {
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

std::string cell_to_string(const arrow::Array& arr, int64_t row) {
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
std::string cell_to_display_string(const arrow::Array& arr, int64_t row) {
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
// Case-insensitive variant. Used for filename extensions so that
// `.bigwig` (ENCODE convention), `.bigWig` (UCSC docs), `.BIGWIG`
// (Windows habit) all resolve to the same matcher.
static bool fends_ci(const std::string& s, const std::string& sfx) {
    if (s.size() < sfx.size()) return false;
    for (size_t i = 0; i < sfx.size(); ++i) {
        char a = s[s.size() - sfx.size() + i];
        char b = sfx[i];
        if (std::tolower((unsigned char)a) !=
            std::tolower((unsigned char)b)) return false;
    }
    return true;
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

// ── UCSC <-> Ensembl chromosome-name aliasing for region queries ─────────────
// A `-r chr1:…` query against a file that names the contig `1` (or vice versa)
// otherwise silently returns zero rows. The alias applies ONLY to human/mouse
// standard chromosomes — autosomes 1..22 (covers human 1-22 and mouse 1-19), X,
// Y, and the mitochondrion — so scaffolds / patches / alt-contigs are never
// remapped. The mitochondrion is `chrM` <-> `MT` (never `M`).
static std::string chrom_alias(const std::string& n) {
    auto std_core = [](const std::string& c) -> bool {   // 1..22, X, Y
        if (c == "X" || c == "Y") return true;
        if (c.empty() || c.size() > 2) return false;
        for (char ch : c) if (ch < '0' || ch > '9') return false;
        int v = std::atoi(c.c_str());
        return v >= 1 && v <= 22;
    };
    if (n.size() > 3 && n.compare(0, 3, "chr") == 0) {    // UCSC -> Ensembl
        std::string core = n.substr(3);
        if (core == "M" || core == "MT") return "MT";      // chrM -> MT
        if (std_core(core)) return core;                   // chr1 -> 1, chrX -> X
        return "";
    }
    if (n == "MT" || n == "M") return "chrM";              // MT -> chrM
    if (std_core(n)) return "chr" + n;                     // 1 -> chr1, X -> chrX
    return "";
}

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
    // Reused across records: htslib's line reader (bgzf_getline) resets `l` and
    // reallocs `s` only when a line outgrows it, so keeping one buffer for the
    // whole scan avoids a malloc+free per record. Freed once in the destructor.
    kstring_t              ks_      = {0, 0, nullptr};
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
        // UCSC<->Ensembl human/mouse chrom aliasing: if a region's chromosome
        // isn't among the tabix index's sequence names but its alias is, swap it.
        {
            int nseq = 0;
            const char** seqs = tbx_seqnames(self->tbx_, &nseq);
            std::set<std::string> have;
            for (int i = 0; seqs && i < nseq; ++i) have.insert(seqs[i]);
            free(seqs);
            bool noted = false;
            auto havef = [&](const std::string& n){ return have.count(n) > 0; };
            for (auto& r : regs) {
                size_t colon = r.find(':');
                std::string chrom = (colon == std::string::npos) ? r : r.substr(0, colon);
                std::string res = resolve_chrom(chrom, havef, path, noted);
                if (res != chrom)
                    r = res + (colon == std::string::npos ? std::string() : r.substr(colon));
            }
        }
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
        if (ks_.s) free(ks_.s);
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
        while (cur_iter_ < iters_.size()) {
            int r = tbx_itr_next(fp_, tbx_, iters_[cur_iter_], &ks_);
            if (r >= 0) {
                buf_.assign(ks_.s, ks_.l);
                buf_ += '\n';
                pos_ = 0;
                return true;
            }
            ++cur_iter_;
        }
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

// All four preamble strippers read through the buffered LineReader (8 KiB
// reads) rather than one byte at a time — a VCF with a 50–100 KiB ## header
// otherwise meant 100 K single-byte Read()s, each allocating an Arrow Buffer.
// When the first data line is reached it (plus any bytes the LineReader read
// ahead, via leftover()) is handed back through *put_back, which the caller
// replays with a PrependInputStream — so there is no need to Seek the
// underlying stream back, and the gz / stdin (non-seekable) and regular-file
// paths are now identical.

// Reads and strips "track"/"browser" preamble lines from the current position.
static std::vector<std::string> strip_bed_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input, std::string* put_back)
{
    std::vector<std::string> headers;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        auto has_prefix = [&](const char* p, size_t n) {
            return line.size() >= n && line.compare(0, n, p, n) == 0 &&
                   (line.size() == n || line[n] == ' ' || line[n] == '\t');
        };
        if (has_prefix("track", 5) || has_prefix("browser", 7)) {
            headers.push_back(line);
            if (!ok) break;
        } else {
            if (put_back) *put_back = line + "\n" + lr.leftover();
            break;
        }
    }
    return headers;
}

// Strips lines whose first character equals prefix_char (e.g. '#' for GFF3,
// '@' for SAM); the first data line (+ look-ahead) goes to *put_back.
static std::vector<std::string> strip_prefix_preamble(
    const std::shared_ptr<arrow::io::InputStream>& input,
    char prefix_char, std::string* put_back)
{
    std::vector<std::string> preamble;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        if (!line.empty() && line[0] == prefix_char) {
            preamble.push_back(line);
            if (!ok) break;
        } else {
            if (put_back) *put_back = line + "\n" + lr.leftover();
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
    std::vector<std::string>* col_names_out, std::string* put_back)
{
    std::vector<std::string> preamble;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        if (line.size() >= 2 && line[0] == '#' && line[1] == '#') {
            preamble.push_back(line);
            if (!ok) break;
        } else if (!line.empty() && line[0] == '#') {
            // #CHROM line: strip leading '#', split on tab → column names. The
            // data follows it; hand the LineReader's look-ahead back so the CSV
            // reader resumes exactly at the first record.
            std::istringstream ss(line.substr(1));
            std::string tok;
            while (std::getline(ss, tok, '\t'))
                col_names_out->push_back(tok);
            if (put_back) *put_back = lr.leftover();
            break;
        } else {
            // data before #CHROM (malformed): replay it rather than drop it.
            if (put_back) *put_back = line + "\n" + lr.leftover();
            break;
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
    char delim, std::string* put_back,
    std::vector<std::string>* col_names_out)
{
    std::vector<std::string> preamble;
    std::string first_data_line;
    LineReader lr(input);
    for (;;) {
        std::string line;
        bool ok = lr.read_line(&line);
        if (!ok && line.empty()) break;
        if (!line.empty() && line[0] == '#') {
            preamble.push_back(line);
            if (!ok) break;
        } else {
            first_data_line = line;
            if (put_back) *put_back = line + "\n" + lr.leftover();
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
// to the UCSC convention (0-based half-open). When one_based is true,
// the input is interpreted per the NCBI / GenBank / VCF / GFF / tabix /
// samtools convention (1-based inclusive at both ends) and converted
// internally before storage.
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
        try {
            size_t pos = 0;
            *v = std::stoll(t, &pos);
            return pos == t.size();   // reject trailing garbage ("5x", "5-10")
        } catch (...) { return false; }
    };
    int64_t pa = INT64_MIN, pb = INT64_MAX;
    bool have_a = !a.empty();
    bool have_b = (dash != std::string::npos) && !b.empty();
    if (have_a && !parse_int(a, &pa)) return false;
    if (have_b && !parse_int(b, &pb)) return false;

    if (one_based) {
        // NCBI-style 1-based inclusive → UCSC 0-based half-open
        //   "a-b"   NCBI [a, b] inclusive  →  UCSC [a - 1, b)
        //   "a"     NCBI single position a →  UCSC [a - 1, a)
        //   "a-"    open upper             →  UCSC [a - 1, INT64_MAX)
        //   "-b"    open lower             →  UCSC [0, b)
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

// ── htslib region-string conventions ─────────────────────────────────────────
//
// vv canonicalises every region to UCSC 0-based half-open (see Region /
// parse_region_one / apply_region_modifiers). htslib's region parsers
// (tbx_itr_querys, sam_itr_regarray, hts_parse_region, bcf_itr_querys) instead
// interpret a "chrom:beg-end" STRING as 1-based inclusive at both ends. Feeding
// them a 0-based string therefore shifts every query one base to the left and
// turns the half-open end into an inclusive one — and makes htslib-backed
// formats (tabix BED/VCF/GFF, BAM pileup, BCF) silently disagree with the
// Parquet interval path for the same -r query. Convert at the boundary:
//   UCSC [s, e)  ==  htslib  chrom:(s+1)-e
static std::string region_to_htslib(const Region& r) {
    // Whole-chromosome (both bounds open) → bare chrom name.
    if (r.start == INT64_MIN && r.end == INT64_MAX) return r.chrom;
    int64_t beg1 = (r.start == INT64_MIN) ? 1 : r.start + 1;
    if (beg1 < 1) beg1 = 1;
    std::string s = r.chrom + ":" + std::to_string(beg1);
    // Open upper bound → "chrom:beg" (htslib reads it as beg..end-of-chrom).
    if (r.end != INT64_MAX) s += "-" + std::to_string(r.end);
    return s;
}

// Convert a canonical (0-based half-open) comma-separated region list into the
// comma-separated 1-based-inclusive form htslib's parsers expect.
static std::string regions_to_htslib(const std::string& canonical) {
    std::string acc;
    for (const auto& r : parse_region_list(canonical)) {
        if (!acc.empty()) acc += ",";
        acc += region_to_htslib(r);
    }
    return acc;
}

// Rewrite each region window's chromosome in place via resolve_chrom() (defined
// earlier, before TabixInputStream, so the string-based tabix path can reuse it).
template <typename HavePred>
static void resolve_region_chroms(std::vector<Region>& ws, HavePred have,
                                  const std::string& path) {
    bool noted = false;
    for (auto& w : ws) w.chrom = resolve_chrom(w.chrom, have, path, noted);
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
// (FilterAtom + FilterExpr are defined in vv/vvcore.hpp, included near the top.)

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
        // `in (...)` punctuation, each its own token.
        if (s[i]=='(' || s[i]==')' || s[i]==',') {
            toks.push_back(std::string(1, s[i++]));
            continue;
        }
        // Operator characters as a chunk: == != < <= > >= ~ !~
        if (s[i]=='='||s[i]=='!'||s[i]=='<'||s[i]=='>'||s[i]=='~') {
            std::string op(1, s[i++]);
            // '!' pairs with '=' (!=) and with '~' (!~); the others only '='.
            if (i < s.size() && (s[i]=='=' || (op=="!" && s[i]=='~')))
                op += s[i++];
            toks.push_back(op);
            continue;
        }
        // Bare word: identifier or numeric literal. The terminator set must
        // include the new punctuation, or `Gene~"BRCA"` lexes as one word
        // `Gene~` and `in("A","B")` as a single token.
        std::string w;
        while (i < s.size() && !std::isspace((unsigned char)s[i])
               && s[i]!='"' && s[i]!='\''
               && s[i]!='=' && s[i]!='!' && s[i]!='<' && s[i]!='>'
               && s[i]!='~' && s[i]!='(' && s[i]!=')' && s[i]!=',')
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
    if (t == "~")  { *op = FilterAtom::Match;    return true; }
    if (t == "!~") { *op = FilterAtom::NotMatch; return true; }
    return false;
}

// Case-insensitive token compare, used for the word operators and AND / OR.
static bool filter_tok_is(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t k = 0; k < a.size(); ++k)
        if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]))
            return false;
    return true;
}

// Strip surrounding quotes from a literal token, if present.
static std::string filter_unquote(const std::string& lit) {
    if (lit.size() >= 2 && (lit.front() == '"' || lit.front() == '\'')
        && lit.front() == lit.back())
        return lit.substr(1, lit.size() - 2);
    return lit;
}

// Parse the user's `--filter` expression. Returns true on success and
// populates `out`. On failure, writes a human-readable reason to `err`.
bool parse_filter_expr(const std::string& expr,
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
        if (i + 2 > toks.size()) {
            *err = "expected '<column> <op> <value>' near token '" + toks[i] + "'";
            return false;
        }
        FilterAtom a;
        a.col_idx = schema.GetFieldIndex(toks[i]);
        if (a.col_idx < 0) {
            *err = "unknown column '" + toks[i] + "' in filter";
            return false;
        }
        // Word operators are operators only in operator position, so a column
        // genuinely named `in`, `is` or `contains` stays filterable.
        const std::string& opt = toks[i + 1];
        size_t next = 0;                       // index just past this atom

        auto need_literal = [&](size_t at, std::string* dst) {
            if (at >= toks.size()) {
                *err = "expected a value after '" + opt + "'";
                return false;
            }
            *dst = filter_unquote(toks[at]);
            return true;
        };

        if (filter_tok_is(opt, "is")) {
            // is null | is not null
            if (i + 2 < toks.size() && filter_tok_is(toks[i+2], "null")) {
                a.op = FilterAtom::IsNull; a.kind = FilterAtom::K_None;
                next = i + 3;
            } else if (i + 3 < toks.size() && filter_tok_is(toks[i+2], "not")
                       && filter_tok_is(toks[i+3], "null")) {
                a.op = FilterAtom::NotNull; a.kind = FilterAtom::K_None;
                next = i + 4;
            } else {
                *err = "expected 'is null' or 'is not null'";
                return false;
            }
        } else if (filter_tok_is(opt, "in") ||
                   (filter_tok_is(opt, "not") && i + 2 < toks.size() &&
                    filter_tok_is(toks[i+2], "in"))) {
            // in (a, b, c) | not in (a, b, c)
            bool negate = filter_tok_is(opt, "not");
            size_t p = i + (negate ? 3 : 2);
            a.op   = negate ? FilterAtom::NotIn : FilterAtom::In;
            a.kind = FilterAtom::K_String;
            if (p >= toks.size() || toks[p] != "(") {
                *err = "expected '(' after 'in'";
                return false;
            }
            ++p;
            while (p < toks.size() && toks[p] != ")") {
                if (toks[p] == ",") { ++p; continue; }
                a.set_lits.push_back(filter_unquote(toks[p]));
                ++p;
            }
            if (p >= toks.size()) { *err = "unterminated 'in (' list"; return false; }
            if (a.set_lits.empty()) { *err = "'in ()' needs at least one value"; return false; }
            next = p + 1;                      // past ')'
        } else if (filter_tok_is(opt, "contains") ||
                   filter_tok_is(opt, "startswith") ||
                   filter_tok_is(opt, "endswith")) {
            a.op = filter_tok_is(opt, "contains")   ? FilterAtom::Contains
                 : filter_tok_is(opt, "startswith") ? FilterAtom::StartsWith
                                                    : FilterAtom::EndsWith;
            a.kind = FilterAtom::K_String;
            if (!need_literal(i + 2, &a.s_lit)) return false;
            next = i + 3;
        } else if (filter_parse_op(opt, &a.op)) {
            if (i + 2 >= toks.size()) {
                *err = "expected a value after '" + opt + "'";
                return false;
            }
            const std::string& lit = toks[i+2];
            if (a.op == FilterAtom::Match || a.op == FilterAtom::NotMatch) {
                // The pattern is always text, never a number.
                a.kind  = FilterAtom::K_String;
                a.s_lit = filter_unquote(lit);
                try { std::regex probe(a.s_lit, std::regex::ECMAScript); (void)probe; }
                catch (const std::regex_error& e) {
                    *err = "bad regex '" + a.s_lit + "': " + e.what();
                    return false;
                }
            } else if (lit.size() >= 2 &&
                       (lit.front() == '"' || lit.front() == '\'') &&
                       lit.front() == lit.back()) {
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
            next = i + 3;
        } else {
            *err = "expected an operator (== != < <= > >= ~ !~ contains "
                   "startswith endswith in 'is null'), got '" + opt + "'";
            return false;
        }

        out->groups.back().push_back(std::move(a));
        i = next;
        if (i >= toks.size()) break;
        if (eq_ci(toks[i], "and")) { ++i; continue; }
        if (eq_ci(toks[i], "or"))  { ++i; out->groups.emplace_back(); continue; }
        *err = "expected AND / OR, got '" + toks[i] + "'";
        return false;
    }
    return true;
}

// Walk a ChunkedArray to the array holding `row`, returning it plus the
// offset inside it. One copy of the loop that cell_as_int / cell_as_double /
// cell_as_string / cell_is_null all used to carry separately.
static const arrow::Array* locate_cell(const arrow::Table& tbl, int col,
                                       int64_t row, int64_t* off) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) { *off = r; return ch.get(); }
        r -= ch->length();
    }
    return nullptr;
}

// True when the cell exists and holds a null. Distinct from "could not read
// it": `is null` must match an actual null, not an unsupported type.
static bool cell_is_null(const arrow::Table& tbl, int col, int64_t row) {
    int64_t off = 0;
    const arrow::Array* a = locate_cell(tbl, col, row, &off);
    return a && a->IsNull(off);
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
// Extract any value of a type is_numeric_type() accepts from array `a` at
// index `r` as a double. Returns false on null or a genuinely non-numeric
// type. Branches on type_id() (no per-cell dynamic_pointer_cast / RTTI).
// Temporal types yield their underlying epoch / elapsed count, so they sort
// chronologically and scale in heatmaps; decimals honour their scale.
static bool array_value_as_double(const arrow::Array& a, int64_t r, double* out) {
    if (a.IsNull(r)) return false;
    switch (a.type_id()) {
        case arrow::Type::DOUBLE: *out = static_cast<const arrow::DoubleArray&>(a).Value(r); return true;
        case arrow::Type::FLOAT:  *out = static_cast<const arrow::FloatArray&>(a).Value(r);  return true;
        case arrow::Type::INT64:  *out = (double)static_cast<const arrow::Int64Array&>(a).Value(r);  return true;
        case arrow::Type::INT32:  *out = (double)static_cast<const arrow::Int32Array&>(a).Value(r);  return true;
        case arrow::Type::INT16:  *out = (double)static_cast<const arrow::Int16Array&>(a).Value(r);  return true;
        case arrow::Type::INT8:   *out = (double)static_cast<const arrow::Int8Array&>(a).Value(r);   return true;
        case arrow::Type::UINT64: *out = (double)static_cast<const arrow::UInt64Array&>(a).Value(r); return true;
        case arrow::Type::UINT32: *out = (double)static_cast<const arrow::UInt32Array&>(a).Value(r); return true;
        case arrow::Type::UINT16: *out = (double)static_cast<const arrow::UInt16Array&>(a).Value(r); return true;
        case arrow::Type::UINT8:  *out = (double)static_cast<const arrow::UInt8Array&>(a).Value(r);  return true;
        case arrow::Type::DATE32: *out = (double)static_cast<const arrow::Date32Array&>(a).Value(r); return true;
        case arrow::Type::DATE64: *out = (double)static_cast<const arrow::Date64Array&>(a).Value(r); return true;
        case arrow::Type::TIME32: *out = (double)static_cast<const arrow::Time32Array&>(a).Value(r); return true;
        case arrow::Type::TIME64: *out = (double)static_cast<const arrow::Time64Array&>(a).Value(r); return true;
        case arrow::Type::TIMESTAMP: *out = (double)static_cast<const arrow::TimestampArray&>(a).Value(r); return true;
        case arrow::Type::DURATION:  *out = (double)static_cast<const arrow::DurationArray&>(a).Value(r);  return true;
        case arrow::Type::DECIMAL128: {
            const auto& arr = static_cast<const arrow::Decimal128Array&>(a);
            *out = arrow::Decimal128(arr.GetValue(r)).ToDouble(
                static_cast<const arrow::Decimal128Type&>(*a.type()).scale());
            return true;
        }
        case arrow::Type::DECIMAL256: {
            const auto& arr = static_cast<const arrow::Decimal256Array&>(a);
            *out = arrow::Decimal256(arr.GetValue(r)).ToDouble(
                static_cast<const arrow::Decimal256Type&>(*a.type()).scale());
            return true;
        }
        default: return false;
    }
}

static bool cell_as_double(const arrow::Table& tbl, int col, int64_t row,
                            double* out) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) return array_value_as_double(*ch, r, out);
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

// Compile-once cache for `~` / `!~`. eval_atom runs per row — recompiling the
// pattern for every cell would dominate the scan. thread_local because the Qt
// frontend evaluates filters on a worker thread.
static const std::regex* filter_regex_for(const std::string& pat) {
    thread_local std::map<std::string, std::regex> cache;
    auto it = cache.find(pat);
    if (it == cache.end()) {
        try {
            it = cache.emplace(pat, std::regex(pat, std::regex::ECMAScript)).first;
        } catch (const std::regex_error&) {
            return nullptr;   // rejected at parse time; belt and braces
        }
    }
    return &it->second;
}

static bool eval_atom(const arrow::Table& tbl, int64_t row, const FilterAtom& a,
                       const std::vector<int>& read_indices) {
    int tcol = filter_col_in_table(a, read_indices);
    if (tcol < 0) return false;

    // Null predicates come first: every other branch treats a null as "no
    // match", which is right for them and wrong here.
    if (a.op == FilterAtom::IsNull)  return  cell_is_null(tbl, tcol, row);
    if (a.op == FilterAtom::NotNull) {
        int64_t off = 0;
        const arrow::Array* arr = locate_cell(tbl, tcol, row, &off);
        return arr && !arr->IsNull(off);
    }

    // String / set predicates read the cell as text whatever the literal
    // looked like, so `Chr in (1,2)` works on a string chrom column.
    switch (a.op) {
        case FilterAtom::Match:
        case FilterAtom::NotMatch: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            const std::regex* re = filter_regex_for(a.s_lit);
            if (!re) return false;
            bool hit = std::regex_search(s, *re);
            return a.op == FilterAtom::Match ? hit : !hit;
        }
        case FilterAtom::Contains:
        case FilterAtom::NotContains: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            bool hit = s.find(a.s_lit) != std::string::npos;
            return a.op == FilterAtom::Contains ? hit : !hit;
        }
        case FilterAtom::StartsWith: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            return s.rfind(a.s_lit, 0) == 0;
        }
        case FilterAtom::EndsWith: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            return s.size() >= a.s_lit.size() &&
                   s.compare(s.size() - a.s_lit.size(), a.s_lit.size(),
                             a.s_lit) == 0;
        }
        case FilterAtom::In:
        case FilterAtom::NotIn: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) {
                // Numeric column: compare the rendered value instead, so
                // `Start in (100, 200)` behaves as written.
                double d;
                if (!cell_as_double(tbl, tcol, row, &d)) return false;
                for (const auto& m : a.set_lits) {
                    try { if (std::stod(m) == d)
                              return a.op == FilterAtom::In; }
                    catch (...) {}
                }
                return a.op == FilterAtom::NotIn;
            }
            for (const auto& m : a.set_lits)
                if (m == s) return a.op == FilterAtom::In;
            return a.op == FilterAtom::NotIn;
        }
        default: break;   // fall through to the ordering comparisons
    }

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
            default: break;   // string/set/null ops handled above
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
                default: break;   // handled above
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
            default: break;   // handled above
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
            default: break;   // handled above
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

// Evaluate a parsed filter over the entire source, returning the source row
// indices that match, in source order. Drains streaming sources and reads
// every chunk with all columns so any referenced column is present. The GUI
// uses this to build a filtered view; CLI export paths use apply_filter on
// the already-projected table instead.
std::vector<int64_t> filter_rows(TabularSource& src, const FilterExpr& expr) {
    std::vector<int64_t> keep;
    int nf = src.schema()->num_fields();
    std::vector<int> all((size_t)nf);
    for (int i = 0; i < nf; ++i) all[(size_t)i] = i;
    src.set_retain_all(true);            // re-reads every chunk after draining
    while (true) {                       // drain streaming sources
        int n = src.num_chunks();
        src.ensure(n);
        if (src.num_chunks() == n) break;
    }
    for (int c = 0; c < src.num_chunks(); ++c) {
        ChunkMeta m = src.chunk_meta(c);
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, all, &tbl).ok() || !tbl) continue;
        int64_t n = tbl->num_rows();
        for (int64_t r = 0; r < n; ++r) {
            bool any = false;
            for (const auto& clause : expr.groups) {
                bool good = true;
                for (const auto& a : clause)
                    if (!eval_atom(*tbl, r, a, all)) { good = false; break; }
                if (good) { any = true; break; }
            }
            if (any) keep.push_back(m.first_row + r);
        }
    }
    return keep;
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
// Declared in vvcore.hpp (external linkage) so GUI frontends can offer region
// queries; parse_region_list / Region stay internal to this TU.
std::string apply_region_modifiers(Config& cfg) {
    // Reject a malformed -r / --region up front. parse_region_list silently
    // drops a token it can't parse, which would turn an invalid region (e.g.
    // "chr1:-5-10" or "chr1:5x") into a whole-file query — surface it instead.
    if (!cfg.region.empty()) {
        size_t pos = 0;
        while (pos <= cfg.region.size()) {
            size_t comma = cfg.region.find(',', pos);
            std::string tok = cfg.region.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) {
                Region r{};
                if (!parse_region_one(tok, &r, cfg.coords_one_based))
                    return "Invalid region '" + tok +
                           "' (expected chrom[:start[-end]])";
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }

    // 0) Canonicalise to UCSC (0-based half-open). --coords NCBI applies only
    // to -r / --region inputs; --regions-file entries are always BED (UCSC)
    // per the spec. After this, cfg.region is guaranteed UCSC convention
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

// Extract a top-level scalar (string or number) for `key` from the LociSSD
// manifest JSON, as a string. Returns false if the key is absent or null.
// The manifest's top-level keys (assembly / species / row_count) don't collide
// with the per-chromosome object keys, so a literal search is safe.
static bool lociss_manifest_value(const std::string& json, const char* key,
                                  std::string* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t q = json.find(needle);
    if (q == std::string::npos) return false;
    q = json.find(':', q + needle.size());
    if (q == std::string::npos) return false;
    ++q;
    while (q < json.size() && std::isspace((unsigned char)json[q])) ++q;
    if (q >= json.size()) return false;
    if (json[q] == '"') {                         // string value
        ++q; std::string v;
        while (q < json.size() && json[q] != '"') {
            if (json[q] == '\\' && q + 1 < json.size()) { v += json[q + 1]; q += 2; }
            else v += json[q++];
        }
        *out = std::move(v);
        return !out->empty();
    }
    if (json.compare(q, 4, "null") == 0) return false;
    size_t e = q;                                 // number / bare token
    while (e < json.size() && json[e] != ',' && json[e] != '}' &&
           json[e] != ']' && !std::isspace((unsigned char)json[e])) ++e;
    *out = json.substr(q, e - q);
    return e > q;
}

// Best-effort species name for a genome assembly, used when the LociSSD
// manifest leaves `species` null (common). Covers the usual model organisms;
// matched case-insensitively against the UCSC / Ensembl assembly aliases.
static std::string assembly_to_species(const std::string& assembly) {
    std::string a;
    for (char c : assembly) a += (char)std::tolower((unsigned char)c);
    auto has = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys) if (a == k) return true;
        return false;
    };
    if (has({"hg38", "hg19", "hg18", "grch38", "grch37", "grch36", "t2t-chm13"}))
        return "Homo sapiens";
    if (has({"mm39", "mm10", "mm9", "grcm39", "grcm38"})) return "Mus musculus";
    if (has({"rn7", "rn6", "rn5", "mratbn7.2"}))          return "Rattus norvegicus";
    if (has({"danrer11", "danrer10", "grcz11", "grcz10"})) return "Danio rerio";
    if (has({"dm6", "dm3", "bdgp6"}))                     return "Drosophila melanogaster";
    if (has({"ce11", "ce10", "wbcel235"}))                return "Caenorhabditis elegans";
    if (has({"saccer3", "saccer2", "r64-1-1"}))           return "Saccharomyces cerevisiae";
    if (has({"galgal6", "galgal5", "grcg6a"}))            return "Gallus gallus";
    if (has({"susscr11", "susscr3"}))                     return "Sus scrofa";
    if (has({"bostau9", "bostau8", "ars-ucd1.2"}))        return "Bos taurus";
    if (has({"xentro10", "xentro9"}))                     return "Xenopus tropicalis";
    if (has({"tair10"}))                                  return "Arabidopsis thaliana";
    return "";
}

// ── LociSSD v4 "colblock" reader ─────────────────────────────────────────────
//
// A custom binary columnar container (NOT Parquet): data file magic "LSB1"
// (version 4) + a sidecar PATH.idx ("LSI1") with a block zone-map index and
// per-(block,column) chunk pointers. Each column chunk is
// zstd(has_nulls byte ‖ [validity bitmap] ‖ codec_payload). Site-level read only
// — the optional genotype matrix (spec §7) is never addressed (we read the index
// up to col_clen[] and ignore any trailing mat_* arrays / matrix section).
// Spec: /home/piotr/Sources/Loci1/docs/lociss_columnar_format_spec.md.
namespace lociss_v4 {

// Map a v4 schema type string to an Arrow type.
static std::shared_ptr<arrow::DataType> arrow_type_of(const std::string& t) {
    if (t == "int8")    return arrow::int8();
    if (t == "int16")   return arrow::int16();
    if (t == "int32")   return arrow::int32();
    if (t == "int64")   return arrow::int64();
    if (t == "uint8")   return arrow::uint8();
    if (t == "uint16")  return arrow::uint16();
    if (t == "uint32")  return arrow::uint32();
    if (t == "uint64")  return arrow::uint64();
    if (t == "float32") return arrow::float32();
    if (t == "float64") return arrow::float64();
    if (t == "large_utf8") return arrow::large_utf8();
    if (t == "bool")    return arrow::boolean();
    return arrow::utf8();   // "utf8" and unknown
}

// --- minimal JSON helpers for the index meta (a well-formed JSON object) ---

// Parse a JSON string array `"key": ["a","b",...]` into `out`.
static bool json_string_array(const std::string& json, const char* key,
                              std::vector<std::string>* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t q = json.find(needle);
    if (q == std::string::npos) return false;
    q = json.find('[', q + needle.size());
    if (q == std::string::npos) return false;
    ++q; out->clear();
    while (q < json.size()) {
        while (q < json.size() &&
               (std::isspace((unsigned char)json[q]) || json[q] == ',')) ++q;
        if (q >= json.size() || json[q] == ']') break;
        if (json[q] != '"') return false;
        ++q; std::string v;
        while (q < json.size() && json[q] != '"') {
            if (json[q] == '\\' && q + 1 < json.size()) { v += json[q + 1]; q += 2; }
            else v += json[q++];
        }
        if (q < json.size()) ++q;
        out->push_back(std::move(v));
    }
    return true;
}

// Byte span [*beg,*end) of the object value for `"obj_key": { ... }`.
static bool json_object_span(const std::string& json, const char* obj_key,
                             size_t* beg, size_t* end) {
    std::string needle = std::string("\"") + obj_key + "\"";
    size_t q = json.find(needle);
    if (q == std::string::npos) return false;
    q = json.find('{', q + needle.size());
    if (q == std::string::npos) return false;
    size_t depth = 0;
    for (size_t p = q; p < json.size(); ++p) {
        char c = json[p];
        if (c == '"') { ++p; while (p < json.size() && json[p] != '"') {
                              if (json[p] == '\\' && p + 1 < json.size()) ++p; ++p; } }
        else if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) { *beg = q; *end = p + 1; return true; } }
    }
    return false;
}

// Look up a scalar field inside the JSON object named `obj_key`.
static bool json_object_value(const std::string& json, const char* obj_key,
                              const std::string& field, std::string* out) {
    size_t beg, end;
    if (!json_object_span(json, obj_key, &beg, &end)) return false;
    std::string obj = json.substr(beg, end - beg);
    return lociss_manifest_value(obj, field.c_str(), out);
}

// Read a little-endian fixed-width int (cw = 4 or 8) and sign-extend to int64.
static inline int64_t rd_int(const uint8_t* p, int cw) {
    if (cw == 8) { int64_t v; std::memcpy(&v, p, 8); return v; }
    int32_t v; std::memcpy(&v, p, 4); return (int64_t)v;
}

// zstd-decompress a compressed buffer (decompressed size unknown up front) via
// Arrow's streaming decompressor, reading to end. `max_out` bounds the output so
// a tiny compressed chunk can't inflate to gigabytes (decompression bomb).
static arrow::Result<std::shared_ptr<arrow::Buffer>>
zstd_inflate(std::shared_ptr<arrow::Buffer> comp, int64_t max_out) {
    ARROW_ASSIGN_OR_RAISE(auto codec,
        arrow::util::Codec::Create(arrow::Compression::ZSTD));
    auto reader = std::make_shared<arrow::io::BufferReader>(comp);
    ARROW_ASSIGN_OR_RAISE(auto cis,
        arrow::io::CompressedInputStream::Make(codec.get(), reader));
    arrow::BufferBuilder bb;
    uint8_t tmp[64 * 1024];
    int64_t total = 0;
    for (;;) {
        ARROW_ASSIGN_OR_RAISE(int64_t got, cis->Read(sizeof tmp, tmp));
        if (got == 0) break;
        total += got;
        if (total > max_out)
            return arrow::Status::Invalid(
                "colblock: decompressed chunk exceeds size cap (corrupt or bomb)");
        ARROW_RETURN_NOT_OK(bb.Append(tmp, got));
    }
    std::shared_ptr<arrow::Buffer> out;
    ARROW_RETURN_NOT_OK(bb.Finish(&out));
    return out;
}

// Decode one (already zstd-decompressed) column chunk into an Arrow array.
// `n` = block row count, `cw` = coord width, `start` = decoded Start values for
// the LENGTH codec (else null). Splits has_nulls + validity bitmap, then decodes
// by codec_id. External linkage (not static) so the libFuzzer harness in
// tests/fuzz/ can call it directly; see include/vv/vvfuzz.hpp.
arrow::Result<std::shared_ptr<arrow::Array>>
decode_colblock(const uint8_t* buf, size_t blen, int codec_id,
                const arrow::DataType& type, int64_t n, int cw,
                const int64_t* start) {
    if (blen < 1) return arrow::Status::Invalid("colblock: empty chunk");
    bool has_nulls = buf[0] != 0;
    size_t p = 1;
    std::vector<uint8_t> valid;            // 1 = valid; empty = all valid
    if (has_nulls) {
        size_t nb = (size_t)((n + 7) / 8);
        if (p + nb > blen) return arrow::Status::Invalid("colblock: short bitmap");
        valid.assign((size_t)n, 0);
        for (int64_t i = 0; i < n; ++i)
            valid[(size_t)i] = (buf[p + (size_t)(i >> 3)] >> (i & 7)) & 1u;
        p += nb;
    }
    const uint8_t* pay = buf + p;
    size_t paylen = blen - p;
    const uint8_t* vb = valid.empty() ? nullptr : valid.data();
    std::shared_ptr<arrow::Array> arr;

    auto need = [&](size_t bytes) -> arrow::Status {
        return paylen >= bytes ? arrow::Status::OK()
             : arrow::Status::Invalid("colblock: payload too short");
    };

    // String result (DICT / FRONTCODE / ARENA): build per declared utf8 width.
    auto build_strings = [&](std::vector<std::string>& vals)
        -> arrow::Result<std::shared_ptr<arrow::Array>> {
        std::shared_ptr<arrow::Array> a;
        if (type.id() == arrow::Type::LARGE_STRING) {
            arrow::LargeStringBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(vals, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        } else {
            arrow::StringBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(vals, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        }
        return a;
    };
    // Integer-coordinate result (DELTA / LENGTH): build per the column type.
    auto build_coords = [&](const std::vector<int64_t>& v)
        -> arrow::Result<std::shared_ptr<arrow::Array>> {
        std::shared_ptr<arrow::Array> a;
        if (type.id() == arrow::Type::INT64) {
            arrow::Int64Builder b;
            if (n > 0) ARROW_RETURN_NOT_OK(b.AppendValues(v.data(), n, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        } else {
            std::vector<int32_t> w(v.size());
            for (size_t i = 0; i < v.size(); ++i) w[i] = (int32_t)v[i];
            arrow::Int32Builder b;
            if (n > 0) ARROW_RETURN_NOT_OK(b.AppendValues(w.data(), n, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&a));
        }
        return a;
    };

    switch (codec_id) {
        case 0: {  // RAW — fixed-width numeric, n × itemsize LE
            #define VV_RAW(BUILDER, CT)                                         \
                do { ARROW_RETURN_NOT_OK(need((size_t)n * sizeof(CT)));         \
                     std::vector<CT> v((size_t)n);                              \
                     BUILDER b;                                                 \
                     if (n > 0) {                                              \
                         std::memcpy(v.data(), pay, (size_t)n * sizeof(CT));    \
                         ARROW_RETURN_NOT_OK(b.AppendValues(v.data(), n, vb));  \
                     }                                                          \
                     ARROW_RETURN_NOT_OK(b.Finish(&arr)); } while (0)
            switch (type.id()) {
                case arrow::Type::INT8:   VV_RAW(arrow::Int8Builder,   int8_t);   break;
                case arrow::Type::INT16:  VV_RAW(arrow::Int16Builder,  int16_t);  break;
                case arrow::Type::INT32:  VV_RAW(arrow::Int32Builder,  int32_t);  break;
                case arrow::Type::INT64:  VV_RAW(arrow::Int64Builder,  int64_t);  break;
                case arrow::Type::UINT8:  VV_RAW(arrow::UInt8Builder,  uint8_t);  break;
                case arrow::Type::UINT16: VV_RAW(arrow::UInt16Builder, uint16_t); break;
                case arrow::Type::UINT32: VV_RAW(arrow::UInt32Builder, uint32_t); break;
                case arrow::Type::UINT64: VV_RAW(arrow::UInt64Builder, uint64_t); break;
                case arrow::Type::FLOAT:  VV_RAW(arrow::FloatBuilder,  float);    break;
                case arrow::Type::DOUBLE: VV_RAW(arrow::DoubleBuilder, double);   break;
                default: return arrow::Status::Invalid("colblock RAW: unsupported type");
            }
            #undef VV_RAW
            return arr;
        }
        case 1: {  // DELTA — cumsum (used for Start)
            ARROW_RETURN_NOT_OK(need((size_t)n * (size_t)cw));
            std::vector<int64_t> v((size_t)n);
            // Accumulate in uint64 — a crafted file's deltas can overflow int64
            // (signed overflow is UB); unsigned wraps deterministically and the
            // result is unchanged for legitimate (bounded) coordinates.
            uint64_t acc = 0;
            for (int64_t i = 0; i < n; ++i) {
                acc += (uint64_t)rd_int(pay + (size_t)i * cw, cw);
                v[(size_t)i] = (int64_t)acc;
            }
            return build_coords(v);
        }
        case 2: {  // LENGTH — Start + len (used for End)
            ARROW_RETURN_NOT_OK(need((size_t)n * (size_t)cw));
            if (!start) return arrow::Status::Invalid("colblock LENGTH: Start not decoded");
            std::vector<int64_t> v((size_t)n);
            for (int64_t i = 0; i < n; ++i)
                v[(size_t)i] = (int64_t)((uint64_t)start[i] +
                                         (uint64_t)rd_int(pay + (size_t)i * cw, cw));
            return build_coords(v);
        }
        case 3: {  // DICT
            ARROW_RETURN_NOT_OK(need(4));
            uint32_t n_dict; std::memcpy(&n_dict, pay, 4);
            size_t q = 4;
            // Widen before +1 so n_dict==UINT32_MAX can't wrap the length to 0.
            ARROW_RETURN_NOT_OK(need(q + ((size_t)n_dict + 1) * 4));
            const uint8_t* offp = pay + q; q += ((size_t)n_dict + 1) * 4;
            uint32_t blob_len; std::memcpy(&blob_len, offp + (size_t)n_dict * 4, 4);
            ARROW_RETURN_NOT_OK(need(q + blob_len));
            const char* blob = (const char*)(pay + q); q += blob_len;
            int code_w = (n_dict <= 256) ? 1 : 2;
            ARROW_RETURN_NOT_OK(need(q + (size_t)n * code_w));
            std::vector<std::string> dict((size_t)n_dict);
            for (uint32_t k = 0; k < n_dict; ++k) {
                uint32_t o0, o1; std::memcpy(&o0, offp + (size_t)k * 4, 4);
                std::memcpy(&o1, offp + ((size_t)k + 1) * 4, 4);
                // Offsets are attacker-controlled: reject non-monotone / OOB
                // (o1<o0 would underflow the length to ~4 GiB).
                if (o0 > o1 || o1 > blob_len)
                    return arrow::Status::Invalid("colblock DICT: bad dictionary offset");
                dict[k].assign(blob + o0, o1 - o0);
            }
            std::vector<std::string> vals((size_t)n);
            for (int64_t i = 0; i < n; ++i) {
                uint32_t code = (code_w == 1) ? pay[q + (size_t)i]
                              : (uint32_t)(pay[q + (size_t)i * 2] | (pay[q + (size_t)i * 2 + 1] << 8));
                if (code < n_dict) vals[(size_t)i] = dict[code];
            }
            return build_strings(vals);
        }
        case 4: {  // FRONTCODE — sequential lcp + suffix
            ARROW_RETURN_NOT_OK(need((size_t)n * 8));
            const uint8_t* lcpp = pay;
            const uint8_t* slenp = pay + (size_t)n * 4;
            const uint8_t* sufp = pay + (size_t)n * 8;
            size_t spos = 0; std::string prev;
            std::vector<std::string> vals((size_t)n);
            for (int64_t i = 0; i < n; ++i) {
                uint32_t lcp, sl;
                std::memcpy(&lcp, lcpp + (size_t)i * 4, 4);
                std::memcpy(&sl, slenp + (size_t)i * 4, 4);
                if (lcp > prev.size()) lcp = (uint32_t)prev.size();
                ARROW_RETURN_NOT_OK(need((size_t)n * 8 + spos + sl));
                std::string cur = prev.substr(0, lcp);
                cur.append((const char*)(sufp + spos), sl);
                spos += sl;
                vals[(size_t)i] = cur;
                prev = std::move(cur);
            }
            return build_strings(vals);
        }
        case 5: {  // ARENA — off[n+1] + utf8
            size_t hdr = ((size_t)n + 1) * 4;
            ARROW_RETURN_NOT_OK(need(hdr));
            const uint8_t* offp = pay;
            const char* blob = (const char*)(pay + hdr);
            size_t blob_avail = paylen - hdr;
            std::vector<std::string> vals((size_t)n);
            for (int64_t i = 0; i < n; ++i) {
                uint32_t o0, o1; std::memcpy(&o0, offp + (size_t)i * 4, 4);
                std::memcpy(&o1, offp + ((size_t)i + 1) * 4, 4);
                // Offsets are attacker-controlled: reject non-monotone / OOB.
                if (o0 > o1 || o1 > blob_avail)
                    return arrow::Status::Invalid("colblock ARENA: bad offset");
                vals[(size_t)i].assign(blob + o0, o1 - o0);
            }
            return build_strings(vals);
        }
        case 6: {  // BOOL — bit-packed LSB-first
            size_t nb = (size_t)((n + 7) / 8);
            ARROW_RETURN_NOT_OK(need(nb));
            std::vector<uint8_t> bits((size_t)n);
            for (int64_t i = 0; i < n; ++i)
                bits[(size_t)i] = (pay[(size_t)(i >> 3)] >> (i & 7)) & 1u;
            arrow::BooleanBuilder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(bits.data(), n, vb));
            ARROW_RETURN_NOT_OK(b.Finish(&arr));
            return arr;
        }
        default:
            return arrow::Status::Invalid("colblock: unknown codec id " +
                                          std::to_string(codec_id));
    }
}

// Select the rows of `a` where keep[i] is true (region overlap mask). Uses core
// Arrow scalars (no arrow_compute dependency); region results are small.
static arrow::Result<std::shared_ptr<arrow::Array>>
filter_array(const std::shared_ptr<arrow::Array>& a, const std::vector<bool>& keep) {
    std::unique_ptr<arrow::ArrayBuilder> bld;
    ARROW_RETURN_NOT_OK(arrow::MakeBuilder(arrow::default_memory_pool(),
                                           a->type(), &bld));
    for (int64_t i = 0; i < a->length(); ++i) {
        if ((size_t)i < keep.size() && !keep[(size_t)i]) continue;
        ARROW_ASSIGN_OR_RAISE(auto sc, a->GetScalar(i));
        ARROW_RETURN_NOT_OK(bld->AppendScalar(*sc));
    }
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(bld->Finish(&out));
    return out;
}

}  // namespace lociss_v4

// A LociSSD v4 dataset, presented chunk = block. The sidecar .idx (zone-map +
// chunk pointers) is parsed at open; column chunks are zstd-decompressed and
// codec-decoded on demand in read_chunk. Region (-r) queries prune blocks via
// the index and mask rows per the §5 overlap test.
class LocissV4Source : public TabularSource {
    std::string                               path_;
    std::shared_ptr<arrow::io::ReadableFile>  data_;
    std::shared_ptr<arrow::Schema>            schema_;
    int       n_blocks_ = 0, n_cols_ = 0, cw_ = 4;
    int64_t   row_count_ = 0;
    std::vector<std::string>  stored_;
    std::vector<int>          codecs_;
    std::vector<std::shared_ptr<arrow::DataType>> col_types_;
    std::vector<int64_t>      cids_, min_start_, max_end_, prefix_max_end_;
    std::vector<int64_t>      n_rows_, block_first_row_;
    std::vector<uint64_t>     col_offset_;
    std::vector<uint32_t>     col_clen_;
    std::map<int64_t, std::string> rank_to_name_;
    std::map<std::string, int64_t> name_to_rank_;
    std::string assembly_, species_;
    int start_si_ = -1, end_si_ = -1;     // stored indices of Start / End

    bool region_mode_ = false;
    struct Slice { int block; Region win; };
    std::vector<Slice>   slices_;
    std::vector<int64_t> slice_first_row_, slice_count_;
    int64_t region_total_ = 0;
    mutable arrow::Status read_status_;

    // Region mode only: memoise decoded column arrays per (block, col) so the
    // open-time count pass and the subsequent display read don't decode the same
    // candidate blocks twice. Bounded; off for sequential scans (where each block
    // is read exactly once and a cache would be pure overhead).
    mutable std::map<int64_t, std::shared_ptr<arrow::Array>> region_col_cache_;
    static constexpr size_t kRegionCacheCap = 512;

    int stored_index(const std::string& nm) const {
        for (int i = 0; i < (int)stored_.size(); ++i) if (stored_[i] == nm) return i;
        return -1;
    }
    // Read + decompress + decode stored column `c` of block `b`.
    arrow::Result<std::shared_ptr<arrow::Array>>
    decode_col(int b, int c, const int64_t* start) const {
        if (c < 0 || c >= n_cols_)
            return arrow::Status::Invalid("colblock: column index out of range");
        int64_t key = (int64_t)b * n_cols_ + c;
        if (region_mode_) {
            auto it = region_col_cache_.find(key);
            if (it != region_col_cache_.end()) return it->second;
        }
        size_t rec = (size_t)b * n_cols_ + (size_t)c;
        ARROW_ASSIGN_OR_RAISE(auto comp, data_->ReadAt((int64_t)col_offset_[rec],
                                                       (int64_t)col_clen_[rec]));
        // Decompression-bomb guard: a block's column chunk can't legitimately
        // exceed a generous per-row bound (64 MiB base covers the dictionary
        // blob / bitmap; ~4 KiB/row is ample for genomic strings), capped at 2 GiB.
        int64_t nr = n_rows_[(size_t)b];
        int64_t max_out = (64LL << 20) + (nr > 0 ? nr * 4096 : 0);
        if (max_out > (2LL << 30)) max_out = 2LL << 30;
        ARROW_ASSIGN_OR_RAISE(auto raw, lociss_v4::zstd_inflate(comp, max_out));
        ARROW_ASSIGN_OR_RAISE(auto arr,
            lociss_v4::decode_colblock(raw->data(), (size_t)raw->size(),
                                       codecs_[(size_t)c], *col_types_[(size_t)c],
                                       nr, cw_, start));
        if (region_mode_) {
            if (region_col_cache_.size() >= kRegionCacheCap) region_col_cache_.clear();
            region_col_cache_[key] = arr;
        }
        return arr;
    }
    // Extract a decoded coordinate array (int32 or int64) as int64 values —
    // used for the region mask and as the LENGTH codec's Start input.
    static std::vector<int64_t> array_to_int64(const std::shared_ptr<arrow::Array>& a) {
        std::vector<int64_t> v((size_t)a->length());
        if (a->type_id() == arrow::Type::INT64) {
            auto ia = std::static_pointer_cast<arrow::Int64Array>(a);
            for (int64_t i = 0; i < a->length(); ++i) v[(size_t)i] = ia->Value(i);
        } else {
            auto ia = std::static_pointer_cast<arrow::Int32Array>(a);
            for (int64_t i = 0; i < a->length(); ++i) v[(size_t)i] = ia->Value(i);
        }
        return v;
    }
    std::shared_ptr<arrow::Array> chrom_array(int b, int64_t count) const {
        auto it = rank_to_name_.find(cids_[(size_t)b]);
        std::string nm = (it != rank_to_name_.end()) ? it->second : "?";
        arrow::StringBuilder bld;
        for (int64_t i = 0; i < count; ++i) (void)bld.Append(nm);
        std::shared_ptr<arrow::Array> a; (void)bld.Finish(&a); return a;
    }
    std::string build_region(const Config& cfg);

public:
    static std::string open(const std::string& path, const Config& cfg,
                            std::unique_ptr<LocissV4Source>* out);

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return region_mode_ ? region_total_ : row_count_; }
    int     num_chunks() const override { return region_mode_ ? (int)slices_.size() : n_blocks_; }
    arrow::Status read_status() const override { return read_status_; }
    bool region_applied() const override { return region_mode_; }
    ChunkMeta chunk_meta(int i) const override {
        if (region_mode_) return {slice_first_row_[(size_t)i], slice_count_[(size_t)i]};
        return {block_first_row_[(size_t)i], n_rows_[(size_t)i]};
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: LociSSD v4  |  Blocks: " + std::to_string(n_blocks_);
    }
    std::string top_banner() const override {
        std::string s = "LociSSD";
        if (!assembly_.empty()) {
            s += "  \xe2\x80\xa2  " + assembly_;
            if (!species_.empty()) s += " (" + species_ + ")";
        }
        s += "  \xe2\x80\xa2  " + digits_with_sep(std::to_string(row_count_)) + " elements";
        return s;
    }
    std::vector<std::string> preamble_above() const override {
        return {top_banner()};   // banner above the table in non-interactive views
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        // A decode failure (corrupt/hostile chunk) is sticky so the CLI exits
        // non-zero instead of silently skipping the block.
        arrow::Status st = read_chunk_impl(i, col_indices, out);
        if (!st.ok() && read_status_.ok()) read_status_ = st;
        return st;
    }
    arrow::Status read_chunk_impl(int i, const std::vector<int>& col_indices,
                                  std::shared_ptr<arrow::Table>* out) {
        int     b = region_mode_ ? slices_[(size_t)i].block : i;
        int64_t n = n_rows_[(size_t)b];

        // Decode Start once (as an array) when End is requested (LENGTH needs its
        // values) or for the region mask; the same array is reused as the Start
        // output column below instead of decoding it a second time.
        bool want_end = false;
        for (int f : col_indices) if (f == end_si_ + 1) want_end = true;
        std::shared_ptr<arrow::Array> start_arr;
        std::vector<int64_t> start;
        const int64_t* startp = nullptr;
        if ((region_mode_ || want_end) && start_si_ >= 0) {
            ARROW_ASSIGN_OR_RAISE(start_arr, decode_col(b, start_si_, nullptr));
            start = array_to_int64(start_arr);
            startp = start.empty() ? nullptr : start.data();
        }

        // Region overlap mask (Start < hi & End > lo); chrom is block-uniform.
        std::shared_ptr<arrow::Array> end_arr;
        std::vector<bool> keep;
        int64_t kept = n;
        if (region_mode_) {
            ARROW_ASSIGN_OR_RAISE(end_arr, decode_col(b, end_si_, startp));
            const Region& w = slices_[(size_t)i].win;
            bool e64 = (end_arr->type_id() == arrow::Type::INT64);
            auto e32 = e64 ? nullptr : std::static_pointer_cast<arrow::Int32Array>(end_arr);
            auto ei64 = e64 ? std::static_pointer_cast<arrow::Int64Array>(end_arr) : nullptr;
            keep.assign((size_t)n, false); kept = 0;
            for (int64_t r = 0; r < n; ++r) {
                int64_t st = start[(size_t)r];
                int64_t en = e64 ? ei64->Value(r) : e32->Value(r);
                bool ok = (w.end == INT64_MAX || st < w.end) &&
                          (w.start == INT64_MIN || en > w.start);
                keep[(size_t)r] = ok; if (ok) ++kept;
            }
        }

        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::Array>> cols;
        for (int f : col_indices) {
            std::shared_ptr<arrow::Array> a;
            if (f == 0) a = chrom_array(b, n);
            else if (f - 1 == end_si_ && end_arr) a = end_arr;
            else if (f - 1 == start_si_ && start_arr) a = start_arr;   // reuse decode
            else { ARROW_ASSIGN_OR_RAISE(a, decode_col(b, f - 1, startp)); }
            if (region_mode_ && kept != n) {
                ARROW_ASSIGN_OR_RAISE(a, lociss_v4::filter_array(a, keep));
            }
            cols.push_back(a);
            fields.push_back(schema_->field(f));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols,
                                  region_mode_ ? kept : n);
        return arrow::Status::OK();
    }
};

std::string LocissV4Source::open(const std::string& path, const Config& cfg,
                                 std::unique_ptr<LocissV4Source>* out) {
    auto self = std::make_unique<LocissV4Source>();
    self->path_ = path;

    // Open the data file first — the LSI1 index may live inline (V4.1) or in a
    // sidecar (legacy V4); the column-chunk pointers are absolute into this file
    // either way.
    auto df = arrow::io::ReadableFile::Open(path);
    if (!df.ok()) return "LociSSD v4: cannot open '" + path + "': " + df.status().ToString();
    self->data_ = df.ValueOrDie();
    int64_t fsize = 0;
    if (auto sz = self->data_->GetSize(); sz.ok()) fsize = *sz;

    // ── Acquire the LSI1 index (V4.1 inline footer or legacy V4 .idx sidecar) ──
    // A V4.1 file ends in a 24-byte trailer: index_offset u64, index_len u64,
    // magic "LSIX", minor u8, reserved[3]. The trailer magic is authoritative
    // (the data header's offset-5 flags bit0 is a redundant fast-path hint).
    std::string idx;
    bool inline_idx = false;
    if (fsize >= 24) {
        if (auto tb = self->data_->ReadAt(fsize - 24, 24); tb.ok() && (*tb)->size() == 24) {
            const uint8_t* t = (*tb)->data();
            uint8_t hdr[8] = {0};
            if (auto hb = self->data_->ReadAt(0, 8); hb.ok() && (*hb)->size() >= 6)
                std::memcpy(hdr, (*hb)->data(), 6);
            bool hdr_flag = hdr[0]=='L' && hdr[1]=='S' && hdr[2]=='B' && hdr[3]=='1' && (hdr[5] & 1u);
            bool trailer_magic = t[16]=='L' && t[17]=='S' && t[18]=='I' && t[19]=='X';
            if (hdr_flag || trailer_magic) {
                if (!trailer_magic) return "LociSSD v4.1: inline-index trailer magic not found";
                uint64_t ioff, ilen;
                std::memcpy(&ioff, t, 8); std::memcpy(&ilen, t + 8, 8);
                if ((int64_t)ioff < 0 || (int64_t)ilen < 0 ||
                    (int64_t)(ioff + ilen) + 24 > fsize)
                    return "LociSSD v4.1: inline-index offset/length out of range";
                auto pb = self->data_->ReadAt((int64_t)ioff, (int64_t)ilen);
                if (!pb.ok()) return "LociSSD v4.1: cannot read inline index: " + pb.status().ToString();
                idx.assign((const char*)(*pb)->data(), (size_t)(*pb)->size());
                inline_idx = true;
            }
        }
    }
    if (!inline_idx) {
        std::string ip = path + ".idx";
        std::ifstream idxf(ip, std::ios::binary);
        if (!idxf) return "LociSSD: '" + path + "' is not a colblock file — no "
                          "inline index trailer and no sidecar '" + ip + "'";
        idx.assign((std::istreambuf_iterator<char>(idxf)),
                   std::istreambuf_iterator<char>());
    }
    if (idx.size() < 24 || idx.compare(0, 4, "LSI1") != 0)
        return "LociSSD v4: bad index magic"
               + std::string(inline_idx ? " (inline)" : " in sidecar '" + path + ".idx'");
    const uint8_t* ib = (const uint8_t*)idx.data();
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, ib + o, 4); return v; };
    uint32_t n_blocks = u32(8), n_cols = u32(12), flags = u32(16), meta_len = u32(20);
    if ((size_t)24 + meta_len > idx.size()) return "LociSSD v4: truncated index meta";
    std::string meta = idx.substr(24, meta_len);
    self->n_blocks_ = (int)n_blocks; self->n_cols_ = (int)n_cols;
    bool coords64 = (flags & 1u) != 0;
    self->cw_ = coords64 ? 8 : 4;
    int isz = coords64 ? 8 : 4;

    // ── meta JSON ────────────────────────────────────────────────────────────
    if (!lociss_v4::json_string_array(meta, "stored", &self->stored_) ||
        (int)self->stored_.size() != (int)n_cols)
        return "LociSSD v4: missing/mismatched 'stored' in index meta";
    self->codecs_.resize(n_cols);
    self->col_types_.resize(n_cols);
    for (int c = 0; c < (int)n_cols; ++c) {
        std::string ty, cd;
        lociss_v4::json_object_value(meta, "schema", self->stored_[(size_t)c], &ty);
        lociss_v4::json_object_value(meta, "codecs", self->stored_[(size_t)c], &cd);
        self->col_types_[(size_t)c] = lociss_v4::arrow_type_of(ty);
        self->codecs_[(size_t)c] = cd.empty() ? 0 : std::atoi(cd.c_str());
    }
    self->start_si_ = self->stored_index("Start");
    self->end_si_   = self->stored_index("End");
    std::string rc;
    if (lociss_manifest_value(meta, "row_count", &rc)) self->row_count_ = std::atoll(rc.c_str());
    lociss_manifest_value(meta, "assembly", &self->assembly_);
    if (!lociss_manifest_value(meta, "species", &self->species_) && !self->assembly_.empty())
        self->species_ = assembly_to_species(self->assembly_);

    // ── fixed-stride index arrays ────────────────────────────────────────────
    size_t p = 24 + meta_len;
    auto take_coord = [&](std::vector<int64_t>& dst) -> bool {
        if (p + (size_t)n_blocks * isz > idx.size()) return false;
        dst.resize(n_blocks);
        for (uint32_t i = 0; i < n_blocks; ++i)
            dst[i] = lociss_v4::rd_int(ib + p + (size_t)i * isz, isz);
        p += (size_t)n_blocks * isz; return true;
    };
    if (!take_coord(self->cids_) || !take_coord(self->min_start_) ||
        !take_coord(self->max_end_))
        return "LociSSD v4: truncated index (coord arrays)";
    if (p + (size_t)n_blocks * 4 > idx.size()) return "LociSSD v4: truncated index (n_rows)";
    self->n_rows_.resize(n_blocks);
    for (uint32_t i = 0; i < n_blocks; ++i) self->n_rows_[i] = u32(p + (size_t)i * 4);
    p += (size_t)n_blocks * 4;
    if (!take_coord(self->prefix_max_end_))
        return "LociSSD v4: truncated index (prefix_max_end)";
    size_t npc = (size_t)n_blocks * n_cols;
    if (p + npc * 8 > idx.size()) return "LociSSD v4: truncated index (col_offset)";
    self->col_offset_.resize(npc);
    for (size_t i = 0; i < npc; ++i) std::memcpy(&self->col_offset_[i], ib + p + i * 8, 8);
    p += npc * 8;
    if (p + npc * 4 > idx.size()) return "LociSSD v4: truncated index (col_clen)";
    self->col_clen_.resize(npc);
    for (size_t i = 0; i < npc; ++i) std::memcpy(&self->col_clen_[i], ib + p + i * 4, 4);

    // block_first_row + chromosome name maps.
    self->block_first_row_.resize(n_blocks);
    int64_t acc = 0;
    for (uint32_t i = 0; i < n_blocks; ++i) { self->block_first_row_[i] = acc; acc += self->n_rows_[i]; }
    for (uint32_t b = 0; b < n_blocks; ++b) {
        int64_t cid = self->cids_[b];
        if (self->rank_to_name_.count(cid)) continue;
        std::string nm;
        lociss_v4::json_object_value(meta, "rank_to_name", std::to_string(cid), &nm);
        if (nm.empty()) nm = std::to_string(cid);
        self->rank_to_name_[cid] = nm;
        self->name_to_rank_[nm]  = cid;
    }

    // ── display schema: Chromosome (synthesized) + stored columns ───────────
    arrow::FieldVector fields;
    fields.push_back(arrow::field("Chromosome", arrow::utf8()));
    for (int c = 0; c < (int)n_cols; ++c)
        fields.push_back(arrow::field(self->stored_[(size_t)c], self->col_types_[(size_t)c]));
    self->schema_ = arrow::schema(fields);

    if (!cfg.region.empty()) {
        std::string err = self->build_region(cfg);
        if (!err.empty()) return err;
    }
    *out = std::move(self);
    return "";
}

// Region (-r) — block-prune via the index zone-map (§5), then per-row overlap
// in read_chunk. Mirrors ParquetSource's region_mode_ exact-count pass.
std::string LocissV4Source::build_region(const Config& cfg) {
    // A region query decodes Start/End to mask rows; without both stored columns
    // the mask/count path would index them out of range.
    if (start_si_ < 0 || end_si_ < 0)
        return "LociSSD v4: region query needs Start and End columns";
    region_mode_ = true;
    auto windows = parse_region_list(cfg.region, cfg.coords_one_based);
    resolve_region_chroms(windows,
        [&](const std::string& n){ return name_to_rank_.count(n) > 0; }, path_);
    for (const auto& w : windows) {
        auto it = name_to_rank_.find(w.chrom);
        if (it == name_to_rank_.end()) continue;          // chrom not present
        int64_t cid = it->second;
        int lo_c = (int)(std::lower_bound(cids_.begin(), cids_.end(), cid) - cids_.begin());
        int hi_c = (int)(std::upper_bound(cids_.begin(), cids_.end(), cid) - cids_.begin());
        if (lo_c >= hi_c) continue;
        int64_t hi = w.end, lo = w.start;
        int ub = (hi == INT64_MAX) ? hi_c
            : (int)(std::lower_bound(min_start_.begin() + lo_c,
                                     min_start_.begin() + hi_c, hi) - min_start_.begin());
        int lb = (lo == INT64_MIN) ? lo_c
            : (int)(std::upper_bound(prefix_max_end_.begin() + lo_c,
                                     prefix_max_end_.begin() + hi_c, lo) - prefix_max_end_.begin());
        for (int b = lb; b < ub; ++b)
            if (lo == INT64_MIN || max_end_[(size_t)b] > lo)
                slices_.push_back({b, w});
    }
    slice_first_row_.assign(slices_.size(), 0);
    slice_count_.assign(slices_.size(), 0);
    int64_t total = 0;
    for (size_t i = 0; i < slices_.size(); ++i) {
        slice_first_row_[i] = total;
        std::shared_ptr<arrow::Table> t;
        if (read_chunk((int)i, {1}, &t).ok() && t) slice_count_[i] = t->num_rows();
        total += slice_count_[i];
    }
    region_total_ = total;
    return "";
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

// (ChunkMeta + TabularSource are defined in vv/vvcore.hpp, included near the
// top. Concrete sources below subclass TabularSource.)

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

// Resolve `cfg.select_cols` into source field indices.
//
// Empty spec: the visible-only set (human-facing views, default) or all
// fields (`include_hidden=true`, for export paths — `--tsv` / `--csv` /
// `--json` / `--parquet` / `--arrow`).
//
// Otherwise the spec is a comma-separated list of terms, resolved in order.
// Output order follows the spec, so `--select End,Start` also reorders:
//
//   Chr           an exact field name
//   chr*, ?_pct   a glob (fnmatch(3), case-sensitive) — `*` and `?` only
//   2-4, 5-       a 1-based, inclusive index range (`N-` runs to the end)
//   @numeric      a type class: @numeric, @string, @list, @bool, @temporal
//   !TERM         exclusion — remove everything TERM matches
//
// An exact field name ALWAYS wins over pattern interpretation, so a column
// literally called `3-9` or `chr*` stays addressable by name.
//
// Unresolvable terms are appended to `*unknown_out`: a plain name that
// doesn't exist, or a pattern that matched nothing (the latter prefixed so
// the caller can word it differently — a silently-empty `!pct_*` typo would
// otherwise be data loss in `--parquet`).
static constexpr const char kNoMatchPrefix[] = "\x01";   // internal marker
// Reported when a non-empty spec resolved cleanly but left nothing selected
// (e.g. `--select 'Chr,!C*'`). Every output path would otherwise write an
// empty, zero-column result and exit 0 — `--parquet` even announces "[20 rows
// → out.parquet]" over a 0x0 file. Pre-existing for `--select ','`; the
// pattern syntax makes it easy to reach by accident, so it is an error now.
static constexpr const char kEmptyResultMarker[] = "\x02";

static bool select_type_class_matches(const std::string& cls,
                                      const arrow::DataType& t) {
    if (cls == "numeric")  return is_numeric_type(t.id());
    if (cls == "string")   return t.id() == arrow::Type::STRING ||
                                  t.id() == arrow::Type::LARGE_STRING;
    if (cls == "bool")     return t.id() == arrow::Type::BOOL;
    if (cls == "list")     return t.id() == arrow::Type::LIST ||
                                  t.id() == arrow::Type::LARGE_LIST ||
                                  t.id() == arrow::Type::FIXED_SIZE_LIST ||
                                  t.id() == arrow::Type::MAP;
    if (cls == "temporal") return t.id() == arrow::Type::DATE32 ||
                                  t.id() == arrow::Type::DATE64 ||
                                  t.id() == arrow::Type::TIME32 ||
                                  t.id() == arrow::Type::TIME64 ||
                                  t.id() == arrow::Type::TIMESTAMP ||
                                  t.id() == arrow::Type::DURATION;
    return false;
}

// Does this token merely LOOK like an index range ("digits-digits" or
// "digits-")? Deliberately unbounded, unlike select_parse_range: a header
// called "0-10" or "500-1000" is range-shaped even when those numbers are
// nowhere near the column count.
static bool select_looks_like_range(const std::string& t) {
    auto dash = t.find('-');
    if (dash == std::string::npos || dash == 0) return false;
    const std::string a = t.substr(0, dash), b = t.substr(dash + 1);
    auto all_digits = [](const std::string& s) {
        return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
    };
    return all_digits(a) && (b.empty() || all_digits(b));
}

// Parse "N-M" / "N-" as a 1-based inclusive range. Returns false unless the
// whole token is consumed, so "2-4x" and "-3" are not ranges.
static bool select_parse_range(const std::string& t, int n_fields,
                               int* lo, int* hi) {
    auto dash = t.find('-');
    if (dash == std::string::npos || dash == 0) return false;
    const std::string a = t.substr(0, dash), b = t.substr(dash + 1);
    auto all_digits = [](const std::string& s) {
        return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
    };
    if (!all_digits(a)) return false;
    if (!b.empty() && !all_digits(b)) return false;
    long la = std::strtol(a.c_str(), nullptr, 10);
    long lb = b.empty() ? n_fields : std::strtol(b.c_str(), nullptr, 10);
    if (la < 1 || lb < la) return false;
    *lo = (int)std::min<long>(la, n_fields);
    *hi = (int)std::min<long>(lb, n_fields);
    return la <= n_fields;
}

static std::vector<int> select_field_indices(
        const TabularSource& src, const Config& cfg,
        std::vector<std::string>* unknown_out = nullptr,
        bool include_hidden = false) {
    auto schema = src.schema();
    const int n_fields = schema->num_fields();

    // The set a bare `!exclusion` starts from. This MUST mirror the empty-spec
    // branch: all fields for export paths, the visible set otherwise.
    // Seeding unconditionally from the visible set would silently drop hidden
    // columns (e.g. LociSSD's derived MaxEndSoFar) from --parquet output —
    // the worst failure mode for a converter.
    auto seed_all = [&]() {
        if (include_hidden) {
            std::vector<int> v;
            v.reserve((size_t)n_fields);
            for (int i = 0; i < n_fields; ++i) v.push_back(i);
            return v;
        }
        return visible_field_indices(src, 0);   // no -c clamp; applied below
    };

    if (cfg.select_cols.empty()) {
        if (include_hidden) {
            int limit = (cfg.max_cols > 0) ? std::min(cfg.max_cols, n_fields)
                                           : n_fields;
            std::vector<int> out;
            for (int i = 0; i < limit; ++i) out.push_back(i);
            return out;
        }
        return visible_field_indices(src, cfg.max_cols);
    }

    // Exact-name lookup that also handles DUPLICATE field names: Arrow's
    // GetFieldIndex returns -1 when a name is ambiguous, which would drop the
    // term through to pattern interpretation and silently select something
    // else entirely. Scan for every field with this name instead.
    auto exact_matches = [&](const std::string& name) {
        std::vector<int> hits;
        for (int i = 0; i < n_fields; ++i)
            if (schema->field(i)->name() == name) hits.push_back(i);
        return hits;
    };

    // Does this file's own header namespace look like index ranges? Binned
    // matrices (Hi-C bins, age/distance bins) really do have columns named
    // "0-10", "10-20", ... On such a file a bare `N-M` term must NOT be
    // reinterpreted positionally: a typo'd bin name would then silently
    // select three unrelated columns instead of erroring. The file's own
    // names win over vv's syntax.
    const bool schema_has_range_names = [&] {
        for (int i = 0; i < n_fields; ++i)
            if (select_looks_like_range(schema->field(i)->name())) return true;
        return false;
    }();

    // Resolve one term to the field indices it names. `matched` is false when
    // a pattern/range/class matched nothing (distinct from an unknown name).
    auto resolve = [&](const std::string& term, bool* matched) {
        std::vector<int> hits;
        *matched = false;
        // 1. Exact name always wins, whatever the term looks like.
        hits = exact_matches(term);
        if (!hits.empty()) { *matched = true; return hits; }
        // 2. Type class.
        if (term.size() > 1 && term[0] == '@') {
            std::string cls = term.substr(1);
            for (char& c : cls) c = (char)std::tolower((unsigned char)c);
            for (int i = 0; i < n_fields; ++i)
                if (select_type_class_matches(cls, *schema->field(i)->type()))
                    hits.push_back(i);
            *matched = !hits.empty();
            return hits;
        }
        // 3. 1-based inclusive index range — unless this file's own column
        //    names are range-shaped, in which case the namespace wins.
        int lo = 0, hi = 0;
        if (!schema_has_range_names &&
            select_parse_range(term, n_fields, &lo, &hi)) {
            for (int i = lo; i <= hi; ++i) hits.push_back(i - 1);
            *matched = !hits.empty();
            return hits;
        }
        // 4. Glob.
        if (term.find_first_of("*?") != std::string::npos) {
            for (int i = 0; i < n_fields; ++i)
                if (fnmatch(term.c_str(), schema->field(i)->name().c_str(),
                            0) == 0)
                    hits.push_back(i);
            *matched = !hits.empty();
            return hits;
        }
        return hits;   // plain name, not found
    };

    std::vector<int> out;
    // Whether any term has been applied yet. A bare leading `!` starts from
    // the full set, but only ONCE — using out.empty() as the proxy would
    // re-seed after an exclusion had emptied the accumulator, so
    // `--select 'Chr,!Chr,!Score'` would resurrect the very column the user
    // excluded. `!X` must never be able to add X back.
    bool seeded = false;
    auto add = [&](int idx) {
        for (int have : out) if (have == idx) return;   // dedupe, keep first
        out.push_back(idx);
    };

    size_t pos = 0;
    while (pos <= cfg.select_cols.size()) {
        size_t comma = cfg.select_cols.find(',', pos);
        std::string term = cfg.select_cols.substr(pos,
            comma == std::string::npos ? std::string::npos : comma - pos);
        while (!term.empty() && std::isspace((unsigned char)term.front())) term.erase(0, 1);
        while (!term.empty() && std::isspace((unsigned char)term.back()))  term.pop_back();
        if (!term.empty()) {
            // An exact field named "!x" is still reachable by name, so check
            // for that before treating a leading '!' as exclusion.
            bool exclude = term[0] == '!' && term.size() > 1 &&
                           schema->GetFieldIndex(term) < 0;
            const std::string body = exclude ? term.substr(1) : term;
            bool matched = false;
            std::vector<int> hits = resolve(body, &matched);
            if (!matched) {
                if (unknown_out) {
                    // Distinguish "no such column" from "pattern matched
                    // nothing": both are errors, but the wording differs.
                    // resolve() already ruled out an exact name, so anything
                    // shaped like a pattern IS one.
                    int rlo = 0, rhi = 0;
                    bool is_pattern =
                        body.find_first_of("*?") != std::string::npos ||
                        (body.size() > 1 && body[0] == '@') ||
                        (!schema_has_range_names &&
                         select_parse_range(body, n_fields, &rlo, &rhi));
                    unknown_out->push_back(
                        (is_pattern ? std::string(kNoMatchPrefix) : std::string()) + term);
                }
            } else if (exclude) {
                if (!seeded) { out = seed_all(); seeded = true; }
                for (int h : hits)
                    out.erase(std::remove(out.begin(), out.end(), h), out.end());
            } else {
                seeded = true;
                for (int h : hits) add(h);
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    // Every term resolved, yet nothing is selected. Don't hand back an empty
    // column set that each output path would render as a valid empty result.
    if (out.empty() && unknown_out && unknown_out->empty())
        unknown_out->push_back(kEmptyResultMarker);
    return out;
}

// Levenshtein distance, capped: we only care whether a name is *close* to a
// real one, so bail out as soon as the whole row exceeds `max`.
static int edit_distance(const std::string& a, const std::string& b, int max) {
    if (std::abs((int)a.size() - (int)b.size()) > max) return max + 1;
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = (int)i;
        int row_min = cur[0];
        for (size_t j = 1; j <= b.size(); ++j) {
            int cost = (std::tolower((unsigned char)a[i - 1]) ==
                        std::tolower((unsigned char)b[j - 1])) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > max) return max + 1;
        prev.swap(cur);
    }
    return prev[b.size()];
}

// Format the "unknown column(s) in --select" message, appending a did-you-mean
// for each name that is close to a real field. Shared by every --select
// consumer (delimited / markdown / json / parquet / arrow / describe) so the
// wording is identical wherever a typo is caught.
static std::string unknown_columns_error(const TabularSource& src,
                                         const std::vector<std::string>& unknown) {
    auto schema = src.schema();
    // Two distinct failures share this path: a name that doesn't exist, and a
    // pattern that matched nothing. Report them separately — a silently-empty
    // `!pct_*` typo is data loss in --parquet, not a no-op.
    std::vector<std::string> names, patterns;
    for (const auto& u : unknown) {
        if (!u.empty() && u[0] == kEmptyResultMarker[0])
            return "--select resolved to no columns (every term was excluded "
                   "or empty); nothing would be written";
        if (!u.empty() && u[0] == kNoMatchPrefix[0]) patterns.push_back(u.substr(1));
        else                                         names.push_back(u);
    }
    if (names.empty() && !patterns.empty()) {
        std::string msg = "--select pattern(s) matched no column: ";
        for (size_t k = 0; k < patterns.size(); ++k) {
            if (k) msg += ", ";
            msg += patterns[k];
        }
        return msg;
    }
    std::string msg = "unknown column(s) in --select: ";
    for (size_t k = 0; k < names.size(); ++k) {
        if (k) msg += ", ";
        msg += names[k];
        // Compare the NAME, not the `!` that marks an exclusion — otherwise
        // `--select '!Chrr'` never gets a suggestion because the leading '!'
        // eats the whole edit budget.
        const std::string& probe =
            (names[k].size() > 1 && names[k][0] == '!') ? names[k].substr(1)
                                                        : names[k];
        // Allow one edit per 4 characters (min 1, max 3) — tight enough that
        // an unrelated name never gets suggested.
        int budget = std::min(3, std::max(1, (int)probe.size() / 4));
        int best = budget + 1;
        std::string best_name;
        for (int i = 0; i < schema->num_fields(); ++i) {
            const std::string& f = schema->field(i)->name();
            int d = edit_distance(probe, f, budget);
            if (d < best) { best = d; best_name = f; }
        }
        if (!best_name.empty()) msg += " (did you mean '" + best_name + "'?)";
    }
    for (size_t k = 0; k < patterns.size(); ++k) {
        msg += (k || !names.empty()) ? ", " : "";
        msg += patterns[k] + " (pattern matched no column)";
    }
    return msg;
}

// ── Parquet source ────────────────────────────────────────────────────────────

class ParquetSource : public TabularSource {
    std::unique_ptr<parquet::arrow::FileReader> reader_;
    std::shared_ptr<parquet::FileMetaData>      meta_;
    std::shared_ptr<arrow::Schema>              schema_;
    std::string                                  path_;
    std::vector<int64_t>                         chunk_start_;
    bool                                         is_lociss_ = false;
    std::string                                  lociss_assembly_;   // e.g. "hg38"
    std::string                                  lociss_species_;    // e.g. "Homo sapiens"

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
    std::vector<int64_t>          slice_count_;      // exact post-filter rows per slice
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
        if (self->is_lociss_ && !lociss_manifest_json.empty()) {
            // Genome assembly + species for the header banner (see top_banner()).
            lociss_manifest_value(lociss_manifest_json, "assembly",
                                  &self->lociss_assembly_);
            if (!lociss_manifest_value(lociss_manifest_json, "species",
                                       &self->lociss_species_) &&
                !self->lociss_assembly_.empty())
                self->lociss_species_ = assembly_to_species(self->lociss_assembly_);
        }

        int64_t acc = 0;
        for (int i = 0; i < self->meta_->num_row_groups(); ++i) {
            self->chunk_start_.push_back(acc);
            acc += self->meta_->RowGroup(i)->num_rows();
        }

        // ── Region pruning (LociSSD-aware, otherwise generic) ────────────────
        if (!cfg.region.empty()) {
            // Parquet column statistics are indexed by *leaf* column, not Arrow
            // field: a nested column (e.g. a list) before chrom/start/end
            // shifts the two apart, so a raw field index would read the wrong
            // column's min/max and mis-prune row groups. Map field → leaf.
            auto field_to_leaf = [&](int field_idx) -> int {
                auto lv = self->arrow_to_leaf_indices({field_idx});
                return lv.empty() ? -1 : lv[0];
            };
            // Read a row group's int-column min/max from Parquet statistics.
            auto col_stats_minmax_int = [&](int rg, int field_idx,
                                            int64_t* lo, int64_t* hi) -> bool {
                int leaf = field_to_leaf(field_idx);
                auto md = self->meta_->RowGroup(rg);
                if (leaf < 0 || leaf >= md->num_columns()) return false;
                auto cc = md->ColumnChunk(leaf);
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
                int leaf = field_to_leaf(field_idx);
                auto md = self->meta_->RowGroup(rg);
                if (leaf < 0 || leaf >= md->num_columns()) return false;
                auto cc = md->ColumnChunk(leaf);
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

            // The slice lengths above are only row-group/manifest-pruned bounds;
            // read_chunk further applies the per-row overlap predicate, so they
            // over-report the visible rows — badly for plain Parquet, where a
            // slice spans a whole row group. Run that same filter once per slice
            // (single-column projection) to record the exact counts, so
            // total_rows() / chunk_meta() agree with read_chunk and the
            // TUI/table view shows no phantom trailing rows.
            self->slice_count_.assign(self->slices_.size(), 0);
            int64_t exact = 0;
            for (size_t i = 0; i < self->slices_.size(); ++i) {
                self->slice_first_row_[i] = exact;
                std::shared_ptr<arrow::Table> t;
                if (self->read_chunk((int)i, {self->j_chrom_}, &t).ok() && t)
                    self->slice_count_[i] = t->num_rows();
                exact += self->slice_count_[i];
            }
            self->region_total_rows_ = exact;
        }

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    bool region_applied() const override { return region_mode_; }
    int64_t total_rows() const override {
        // Region mode: the exact post-filter total (computed at open by running
        // the overlap predicate once per slice), so it matches the rows
        // read_chunk actually yields.
        return region_mode_ ? region_total_rows_ : meta_->num_rows();
    }
    int     num_chunks() const override {
        return region_mode_ ? (int)slices_.size() : meta_->num_row_groups();
    }
    ChunkMeta chunk_meta(int i) const override {
        if (region_mode_) return {slice_first_row_[i], slice_count_[i]};
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
        if (!region_mode_) {
            // The Result-returning ReadRowGroups overload landed in Arrow 24.0.0
            // (when the Status one was deprecated); the static build still pins
            // Arrow 23.0.1, so keep the old call there.
#if ARROW_VERSION_MAJOR >= 24
            ARROW_ASSIGN_OR_RAISE(
                *out, reader_->ReadRowGroups({i}, arrow_to_leaf_indices(cols)));
            return arrow::Status::OK();
#else
            return reader_->ReadRowGroups({i}, arrow_to_leaf_indices(cols), out);
#endif
        }

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
#if ARROW_VERSION_MAJOR >= 24
        ARROW_ASSIGN_OR_RAISE(raw, reader_->ReadRowGroups(
            {s.row_group}, arrow_to_leaf_indices(need)));
#else
        ARROW_RETURN_NOT_OK(reader_->ReadRowGroups(
            {s.row_group}, arrow_to_leaf_indices(need), &raw));
#endif
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
        // Genomic coordinate columns use whatever integer width is most
        // compact (UInt32 is common for positions) and may be dictionary-
        // encoded. Reading only Int32/Int64 returned 0 for every other type,
        // which made the window test (en <= w.start) reject every row and
        // silently empty the region result.
        auto plain_int = [](const arrow::Array& a, int64_t r) -> int64_t {
            switch (a.type_id()) {
                case arrow::Type::INT8:   return static_cast<const arrow::Int8Array&>(a).Value(r);
                case arrow::Type::INT16:  return static_cast<const arrow::Int16Array&>(a).Value(r);
                case arrow::Type::INT32:  return static_cast<const arrow::Int32Array&>(a).Value(r);
                case arrow::Type::INT64:  return static_cast<const arrow::Int64Array&>(a).Value(r);
                case arrow::Type::UINT8:  return static_cast<const arrow::UInt8Array&>(a).Value(r);
                case arrow::Type::UINT16: return static_cast<const arrow::UInt16Array&>(a).Value(r);
                case arrow::Type::UINT32: return static_cast<const arrow::UInt32Array&>(a).Value(r);
                case arrow::Type::UINT64: return (int64_t)static_cast<const arrow::UInt64Array&>(a).Value(r);
                default: return 0;
            }
        };
        auto cell_int = [&](const std::shared_ptr<arrow::Array>& a, int64_t r) -> int64_t {
            if (a->type_id() == arrow::Type::DICTIONARY) {
                const auto& d = static_cast<const arrow::DictionaryArray&>(*a);
                if (d.IsNull(r)) return 0;
                return plain_int(*d.dictionary(), d.GetValueIndex(r));
            }
            return plain_int(*a, r);
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
    // Prominent top banner for LociSSD: genome assembly (+ species) and the
    // total element count. Empty for plain Parquet.
    std::string top_banner() const override {
        if (!is_lociss_) return "";
        std::string s = "LociSSD";
        if (!lociss_assembly_.empty()) {
            s += "  \xe2\x80\xa2  " + lociss_assembly_;
            if (!lociss_species_.empty()) s += " (" + lociss_species_ + ")";
        }
        int64_t n = meta_ ? meta_->num_rows() : -1;
        if (n >= 0)
            s += "  \xe2\x80\xa2  " + digits_with_sep(std::to_string(n)) + " elements";
        return s;
    }
    // Render the banner above the table in non-interactive views (the TUI draws
    // it as a reserved top row).
    std::vector<std::string> preamble_above() const override {
        std::string b = top_banner();
        return b.empty() ? std::vector<std::string>{} : std::vector<std::string>{b};
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

enum class DelimKind { CSV, TSV, BED, VCF, GFF, SAM, PAF, Mpileup };

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

// True for a leading-zero integer token like "007" / "00" / "012" — a code or
// ID where the zeros are meaningful. "0", "10", "0.5" and "" are NOT flagged, so
// ordinary numeric data is never forced to string. Used to keep such columns as
// utf8 instead of letting Arrow's CSV inference drop the zeros ("007" -> 7).
static bool is_leading_zero_int(const std::string& s) {
    if (s.size() < 2 || s[0] != '0') return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

// True if `s` is a plain decimal / scientific number token (digits, sign, dot,
// e/E only) that strtod fully consumes — rejects the word-like / hex forms strtod
// also accepts (nan, inf, 0x…). Shared by the headerless-CSV heuristic and the
// leading-zero pre-scan so both agree on whether row 0 is a header.
static bool looks_like_plain_number(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isdigit(c) && c != '+' && c != '-' &&
            c != '.' && c != 'e' && c != 'E')
            return false;
    char* ep; std::strtod(s.c_str(), &ep);
    return *ep == '\0';
}

// Split one delimited line into fields, honouring Arrow's default CSV quoting: a
// field that starts with '"' is quoted until the next unescaped '"', a doubled
// '""' inside is a literal quote, and the delimiter is literal inside quotes.
// So a quoted field containing the delimiter doesn't shift column positions.
static void split_delimited_line(const std::string& line, char delim,
                                 std::vector<std::string>* out) {
    out->clear();
    std::string field;
    size_t i = 0, n = line.size();
    while (i < n) {
        field.clear();
        if (line[i] == '"') {                 // quoted field
            ++i;
            while (i < n) {
                if (line[i] == '"') {
                    if (i + 1 < n && line[i + 1] == '"') { field += '"'; i += 2; }
                    else { ++i; break; }       // closing quote
                } else field += line[i++];
            }
            while (i < n && line[i] != delim) ++i;   // skip to delimiter
        } else {
            while (i < n && line[i] != delim) field += line[i++];
        }
        out->push_back(field);
        if (i < n && line[i] == delim) {
            ++i;
            if (i == n) out->push_back("");   // trailing delimiter → empty field
        }
    }
}

// From a sample of delimited data lines + column names, return the names of
// columns that contain a leading-zero integer value (so they should be read as
// utf8, not inferred numeric). Lines are tokenised quote-aware; a column is
// flagged if any sampled value is is_leading_zero_int().
static std::vector<std::string>
leading_zero_columns(const std::vector<std::string>& sample_lines, char delim,
                     const std::vector<std::string>& col_names) {
    std::vector<char> flagged(col_names.size(), 0);
    std::vector<std::string> fields;
    for (const auto& line : sample_lines) {
        split_delimited_line(line, delim, &fields);
        for (size_t c = 0; c < fields.size() && c < flagged.size(); ++c)
            if (!flagged[c] && is_leading_zero_int(fields[c])) flagged[c] = 1;
    }
    std::vector<std::string> out;
    for (size_t c = 0; c < col_names.size(); ++c)
        if (flagged[c]) out.push_back(col_names[c]);
    return out;
}

class DelimitedSource : public TabularSource {
    std::string                           path_;
    char                                  delimiter_;
    DelimKind                             kind_;
    std::shared_ptr<arrow::Schema>        schema_;
    std::vector<std::string>              preamble_lines_;
    int                                   bed_level_ = 3; // detected BED standard cols (3..9)
    BedVariant                            bed_variant_ = BedVariant::None;
    int                                   mpileup_samples_ = 0; // samtools mpileup samples (>=1)

    // Decoded batches in a bounded trailing window: older entries are freed
    // (set null) once retain_all_ is false and the window overflows. Per-batch
    // metadata below is kept for every batch, evicted or not.
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>          batch_first_row_;
    mutable std::vector<int64_t>          batch_num_rows_;  // retained after eviction
    mutable int64_t                       rows_so_far_ = 0;
    mutable bool                          all_read_    = false;
    mutable bool                          retain_all_  = false; // pinned by full-pass ops
    mutable bool                          evicted_any_ = false; // any batch freed?
    // True when the data stream was replaced by a tabix iterator for -r.
    bool                                  region_applied_ = false;
    mutable arrow::Status                 read_status_;   // sticky stream error

    // Append a freshly-decoded batch via the shared bounded-window helper.
    void retain_(std::shared_ptr<arrow::RecordBatch> batch) const {
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
    }

    mutable std::shared_ptr<arrow::csv::StreamingReader> reader_;

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        std::shared_ptr<arrow::RecordBatch> batch;
        arrow::Status st = reader_->ReadNext(&batch);
        if (!st.ok()) {
            // A malformed row (bad column count, encoding, …) anywhere past the
            // first block surfaces here. Record it stickily and stop: callers
            // that ignore advance()'s result (ensure()) must not spin forever,
            // and the CLI can report the truncation and exit non-zero instead
            // of silently emitting a partial result with status 0.
            read_status_ = st;
            all_read_ = true;
            return st;
        }
        if (!batch) { all_read_ = true; return arrow::Status::OK(); }
        retain_(std::move(batch));
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
                bool autogen_names, const std::vector<std::string>& col_names,
                const std::vector<std::string>& force_string_cols = {}) {
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
        auto copts = arrow::csv::ConvertOptions::Defaults();
        // Force the detected leading-zero-ID columns to utf8 so inference can't
        // drop the zeros ("007" -> 7). Keyed by name (Arrow has no by-index
        // override); a no-op for a column Arrow would have made string anyway.
        for (const auto& name : force_string_cols)
            copts.column_types[name] = arrow::utf8();
        return arrow::csv::StreamingReader::Make(
            arrow::io::default_io_context(), input, ropts, popts, copts);
    }

    // Re-read a small sample of the file and return the names of columns that
    // hold a leading-zero integer (so they can be forced to utf8 — Arrow's CSV
    // inference would otherwise turn "007" into 7). Mirrors the primary reader's
    // naming so the forced-string names line up: a `#`-header supplies the names
    // (header_names), otherwise the first non-comment line is the header. The
    // leading-zero scan reads the data lines after it. Best-effort: {} on any
    // I/O / open problem; column_types entries that don't match are simply
    // ignored by Arrow, never an error.
    static std::vector<std::string>
    detect_leading_zero_columns(const std::string& path, bool is_gz, char delim,
                                const std::vector<std::string>& header_names) {
        std::shared_ptr<arrow::io::ReadableFile> raw;
        std::shared_ptr<arrow::io::InputStream>  input;
        if (!open_stream(path, is_gz, &raw, &input).empty()) return {};
        LineReader lr(input);
        std::vector<std::string> names = header_names;   // from a `#`-header, if any
        bool have_names = !names.empty();
        std::vector<std::string> sample;
        std::string line;
        const int kMaxData = 200;
        for (;;) {
            bool ok = lr.read_line(&line);
            if (!ok && line.empty()) break;              // true EOF
            if (!line.empty() && line[0] != '#') {       // skip blank + comment
                if (!have_names) {                       // first data line = header
                    split_delimited_line(line, delim, &names);
                    have_names = true;
                } else {
                    sample.push_back(line);
                    if ((int)sample.size() >= kMaxData) break;
                }
            }
            if (!ok) break;
        }
        if (names.empty()) return {};
        return leading_zero_columns(sample, delim, names);
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

        bool is_gz = fends_ci(path, ".gz");

        std::shared_ptr<arrow::io::ReadableFile>  raw;
        std::shared_ptr<arrow::io::InputStream>   input;
        {
            std::string err = open_stream(path, is_gz, &raw, &input);
            if (!err.empty()) return err;
        }
        return continue_open(std::move(self), std::move(input),
                             std::move(raw), is_gz, kind, region, out);
    }

    // Apply ENCODE peak-family naming on top of the vanilla BED schema.
    // Called by open_source's BED dispatch when the file extension picks
    // a specific variant (narrowPeak / broadPeak / gappedPeak / bedGraph
    // / tagAlign). Rewrites schema_ in place; safe to call after open().
    void apply_bed_variant(BedVariant v) {
        bed_variant_ = v;
        if (v == BedVariant::None) return;
        const int nf = schema_->num_fields();
        std::vector<std::string> names;
        switch (v) {
            case BedVariant::NarrowPeak:
                names = {"Chr","[Beg","End)","Name","Score","Str",
                         "signalValue","pValue","qValue","peak"};
                break;
            case BedVariant::BroadPeak:
                names = {"Chr","[Beg","End)","Name","Score","Str",
                         "signalValue","pValue","qValue"};
                break;
            case BedVariant::GappedPeak:
                names = {"Chr","[Beg","End)","Name","Score","Str",
                         "ThBeg","ThEnd","RGB","NBlk","BlkSz","BlkSt",
                         "signalValue","pValue","qValue"};
                break;
            case BedVariant::BedGraph:
                names = {"Chr","[Beg","End)","value"};
                break;
            case BedVariant::TagAlign:
                names = {"Chr","[Beg","End)","sequence","Score","Str"};
                break;
            case BedVariant::None: return;
        }
        arrow::FieldVector fields;
        fields.reserve(nf);
        for (int i = 0; i < nf; ++i) {
            auto f = schema_->field(i);
            std::string nm = (i < (int)names.size())
                ? names[i]
                : ("+" + std::to_string(i - (int)names.size() + 1));
            fields.push_back(arrow::field(nm, f->type(), f->nullable()));
        }
        schema_ = arrow::schema(fields);
        // bed_level_ drives footer formatting; track the "standard cols" count.
        bed_level_ = (int)names.size();
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
        // Format-specific preamble stripping and column-name determination.
        // GFF and SAM wrap the stream in TruncateFieldsStream to handle variable columns.
        std::vector<std::string> col_names;
        std::string put_back;

        switch (kind) {
            case DelimKind::BED:
                self->preamble_lines_ = strip_bed_preamble(input, &put_back);
                break;
            case DelimKind::VCF:
                self->preamble_lines_ = strip_vcf_preamble(input, &col_names, &put_back);
                break;
            case DelimKind::GFF: {
                self->preamble_lines_ = strip_prefix_preamble(input, '#', &put_back);
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
                self->preamble_lines_ = strip_prefix_preamble(input, '@', &put_back);
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
                    input, self->delimiter_, &put_back, &col_names);
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
            // `region` is canonical 0-based half-open; tbx_itr_querys reads its
            // string argument as 1-based inclusive, so convert at the boundary.
            std::shared_ptr<TabixInputStream> tabix;
            std::string err =
                TabixInputStream::open(path, regions_to_htslib(region), &tabix);
            if (!err.empty()) return err;
            std::shared_ptr<arrow::io::InputStream> ti = tabix;
            if (kind == DelimKind::GFF) ti = std::make_shared<TruncateFieldsStream>(ti, 9);
            if (kind == DelimKind::SAM) ti = std::make_shared<TruncateFieldsStream>(ti, 11);
            input = ti;
            self->region_applied_ = true;
        }

        bool autogen = (kind == DelimKind::BED) ||
                       (kind == DelimKind::Mpileup);
        // Leading-zero IDs: a CSV/TSV column like "007" would otherwise be
        // inferred as int and lose the zeros. Pre-scan a sample (keyed by the
        // same column names the reader uses) and force those columns to utf8.
        // Other formats have fixed schemas where this can't arise.
        std::vector<std::string> force_string;
        if (kind == DelimKind::CSV || kind == DelimKind::TSV)
            force_string = detect_leading_zero_columns(path, is_gz,
                                                       self->delimiter_, col_names);
        auto r = make_reader(input, self->delimiter_, autogen, col_names,
                             force_string);
        if (!r.ok()) {
            // A region query whose window overlaps no records leaves the tabix
            // stream empty, and Arrow's CSV reader rejects empty input with
            // "Empty CSV file". The Parquet/BCF/BAM paths return a valid empty
            // result (exit 0) in this situation, so match them: recover the
            // column layout from the full file (region-free) and present zero
            // rows. Reading the whole file region-free is unnecessary — the
            // probe only needs the first block to settle names/types (and, for
            // BED, the column level). Falls through to the original error if
            // the file itself cannot be opened.
            if (!region.empty() &&
                r.status().ToString().find("Empty CSV") != std::string::npos) {
                std::unique_ptr<DelimitedSource> probe;
                if (open(path, kind, /*region=*/"", &probe).empty()) {
                    self->schema_          = probe->schema_;
                    self->bed_level_       = probe->bed_level_;
                    self->bed_variant_     = probe->bed_variant_;
                    self->mpileup_samples_ = probe->mpileup_samples_;
                    self->all_read_        = true;   // zero matching rows
                    *out = std::move(self);
                    return "";
                }
            }
            return "Cannot open '" + path + "': " + r.status().ToString();
        }
        self->reader_ = r.ValueOrDie();
        self->schema_ = self->reader_->schema();

        // For CSV/TSV: if all column names are numeric, the file has no header → retry.
        // strtod() also accepts "nan", "inf"/"infinity" and "0x…" hex floats —
        // a column literally named one of those is far more likely a real
        // header than headerless numeric data, so require a plain decimal /
        // scientific token (digits, sign, dot, e/E exponent only) and let
        // strtod confirm it actually parses. (A header of bare numbers like
        // "1,2,3" is genuinely ambiguous and still treated as headerless data.)
        if (kind == DelimKind::CSV || kind == DelimKind::TSV) {
            bool all_numeric = self->schema_->num_fields() > 0;
            for (int i = 0; i < self->schema_->num_fields() && all_numeric; ++i) {
                const std::string& nm = self->schema_->field(i)->name();
                if (!looks_like_plain_number(nm)) all_numeric = false;
            }
            if (all_numeric) {
                std::shared_ptr<arrow::io::ReadableFile>  raw2;
                std::shared_ptr<arrow::io::InputStream>   input2;
                if (open_stream(path, is_gz, &raw2, &input2).empty()) {
                    auto r2 = make_reader(input2, self->delimiter_,
                                          /*autogen=*/true, {}, force_string);
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
                if (lvl == 4 && !self->batches_.empty() && self->batches_[0]) {
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

        // samtools mpileup: per-sample triplet (depth, bases, qualities)
        // after the fixed chrom/pos/ref prefix. Single-sample files get
        // unsuffixed names; multi-sample files get `_1`, `_2`, … so the
        // user can still --select / --filter by column.
        if (kind == DelimKind::Mpileup) {
            int nf = self->schema_->num_fields();
            if (nf >= 6 && (nf - 3) % 3 == 0) {
                int samples = (nf - 3) / 3;
                self->mpileup_samples_ = samples;
                arrow::FieldVector fields;
                fields.reserve(nf);
                auto add = [&](const std::string& nm, int i) {
                    auto t = self->schema_->field(i)->type();
                    bool n = self->schema_->field(i)->nullable();
                    fields.push_back(arrow::field(nm, t, n));
                };
                add("chrom", 0);
                add("pos",   1);
                add("ref",   2);
                for (int s = 0; s < samples; ++s) {
                    std::string sfx = (samples == 1) ? std::string{}
                                        : "_" + std::to_string(s + 1);
                    add("depth"  + sfx, 3 + s * 3);
                    add("bases"  + sfx, 4 + s * 3);
                    add("quals"  + sfx, 5 + s * 3);
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
    arrow::Status read_status() const override { return read_status_; }
    bool region_applied() const override { return region_applied_; }
    ChunkMeta chunk_meta(int i) const override {
        // num_rows is read from retained metadata, not the batch, so it stays
        // valid after the batch's data is evicted from the trailing window.
        return {batch_first_row_[i], batch_num_rows_[i]};
    }

    // Pin retention: stop evicting (used by full-pass ops — search / sort /
    // filter / stats — that must read every row). Already-evicted batches can't
    // be recovered (forward-only stream); evicted_any() reports that case.
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }

    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
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
            case DelimKind::Mpileup: {
                std::string s = "Format: mpileup";
                if (mpileup_samples_ > 0) {
                    s += "  |  Samples: " + std::to_string(mpileup_samples_);
                }
                return s;
            }
            case DelimKind::BED: {
                int nf = schema_->num_fields();
                if (bed_variant_ != BedVariant::None) {
                    switch (bed_variant_) {
                        case BedVariant::NarrowPeak: return "Format: narrowPeak (BED6+4)";
                        case BedVariant::BroadPeak:  return "Format: broadPeak (BED6+3)";
                        case BedVariant::GappedPeak: return "Format: gappedPeak (BED12+3)";
                        case BedVariant::BedGraph:   return "Format: bedGraph";
                        case BedVariant::TagAlign:   return "Format: tagAlign (BED6)";
                        case BedVariant::None: break;
                    }
                }
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
    // -r: index + multi-region iterator. When iter_ is set, advance() walks
    // only the records overlapping the requested windows.
    mutable hts_idx_t* idx_  = nullptr;
    mutable hts_itr_t* iter_ = nullptr;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>          batch_first_row_;
    mutable std::vector<int64_t>          batch_num_rows_;   // kept after eviction
    mutable int64_t                       rows_so_far_ = 0;
    mutable bool                          all_read_    = false;
    mutable bool                          retain_all_  = false;
    mutable bool                          evicted_any_ = false;
    mutable arrow::Status                 read_status_;      // sticky stream error

    static constexpr int BATCH_SIZE = 32768;

    static constexpr const char NT16[] = "=ACMGRSVTWYHKDBN";

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();

        arrow::StringBuilder qname_b, rname_b, cigar_b, rnext_b, seq_b, qual_b;
        arrow::Int32Builder  flag_b, mapq_b;
        arrow::Int64Builder  pos_b, pnext_b, tlen_b;

        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        int count = 0, ret = 0;
        // In region mode the multi-region iterator yields only the records
        // overlapping the requested windows; otherwise walk the whole file.
        auto next_record = [&]() {
            return iter_ ? sam_itr_multi_next(hts_, iter_, rec_)
                         : sam_read1(hts_, hdr_, rec_);
        };
        while (count < cap && (ret = next_record()) >= 0) {
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

        // A read error (ret < -1) is distinct from EOF (-1). ensure() discards
        // advance()'s return, so record it stickily too or a truncated /
        // corrupt file yields a partial result with exit 0.
        if (ret < -1) {
            all_read_ = true;
            if (read_status_.ok())
                read_status_ = arrow::Status::IOError(
                    "Error reading ", fmt_name_, " record from ", path_);
            return read_status_;
        }
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
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BamSource() {
        // sam_itr_regarray() builds a MULTI iterator — it must be freed with
        // hts_itr_multi_destroy, not hts_itr_destroy.
        if (iter_) { hts_itr_multi_destroy(iter_); iter_ = nullptr; }
        if (idx_)  { hts_idx_destroy(idx_);  idx_  = nullptr; }
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

        // CRAM stores bases as differences from a reference, so decoding needs
        // one. htslib otherwise falls back to $REF_PATH / $REF_CACHE (often a
        // network fetch, or nothing at all). -f/--fasta points it at a local
        // FASTA. Must be set before the header read triggers any decode.
        if (!cfg.pileup_ref.empty()) {
            if (hts_set_fai_filename(self->hts_, cfg.pileup_ref.c_str()) < 0)
                return "Cannot use reference '" + cfg.pileup_ref +
                       "' for '" + path + "' (need a .fai index; run "
                       "`samtools faidx`)";
        }

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

        // -r: restrict the scan to the requested windows. Needs a
        // coordinate-sorted file with an index (.bai / .csi / .crai). Without
        // this the flag was silently ignored and vv answered with the whole
        // file — including for a contig the file doesn't even have.
        if (!cfg.region.empty()) {
            if (self->fmt_name_ == "SAM")
                return "'" + path + "': -r needs an indexed BAM/CRAM; plain "
                       "SAM has no index (convert with `samtools view -b`)";
            self->idx_ = sam_index_load(self->hts_, path.c_str());
            if (!self->idx_)
                return "'" + path + "': -r needs an index (.bai/.csi/.crai); "
                       "build one with `samtools index`";
            // cfg.region is canonical 0-based half-open; sam_itr_regarray reads
            // region strings as 1-based inclusive, so convert at the boundary.
            std::vector<Region> windows = parse_region_list(cfg.region);
            resolve_region_chroms(windows,
                [&](const std::string& nm) {
                    return bam_name2id(self->hdr_, nm.c_str()) >= 0;
                }, path);
            std::vector<std::string> regs;
            regs.reserve(windows.size());
            for (const auto& w : windows) regs.push_back(region_to_htslib(w));
            std::vector<const char*> regp;
            regp.reserve(regs.size());
            for (auto& r : regs) regp.push_back(r.c_str());
            self->iter_ = sam_itr_regarray(self->idx_, self->hdr_,
                                            const_cast<char**>(regp.data()),
                                            (unsigned)regp.size());
            if (!self->iter_)
                return "'" + path + "': cannot build iterator for region '" +
                       cfg.region + "'";
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
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    bool region_applied() const override { return iter_ != nullptr; }
    arrow::Status read_status() const override { return read_status_; }
    void ensure(int i) override {
        // advance()'s error used to be discarded here, so a truncated or
        // corrupt file produced a partial result with exit 0. Keep it.
        while (!all_read_ && (int)batches_.size() <= i) {
            auto st = advance();
            if (!st.ok()) { if (read_status_.ok()) read_status_ = st; break; }
        }
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
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

// ── BAM/CRAM pileup source (`vv x.bam --pileup`) ─────────────────────────────
//
// Walks a sorted BAM/CRAM through htslib's bam_plp_auto engine, emitting one
// row per covered position in a schema identical to DelimKind::Mpileup
// (`chrom, pos, ref, depth, bases, quals`). The output is byte-for-byte
// compatible with `samtools mpileup` invoked without a reference FASTA: ref
// is set to 'N' on every row, and bases are rendered as literal letters
// (uppercase forward, lowercase reverse) — there's no `.` / `,` match
// notation because we have no reference to match against.
//
// Region queries (`-r chrom:start-end`) pipe through htslib's sam_itr_querys
// so we only walk the requested span — essential for whole-genome BAMs where
// `vv x.bam --pileup` over everything would emit ~3 billion rows. Composes
// with `--decode-pileup`, which materialises the typed allele-count view on
// top of this source.

class BamPileupSource : public TabularSource {
    std::string                              path_;
    std::shared_ptr<arrow::Schema>           schema_;
    std::string                              fmt_name_;     // BAM / CRAM / SAM

    // htslib handles. The plp iterator owns the read-callback closure.
    samFile*                                 fp_   = nullptr;
    sam_hdr_t*                               hdr_  = nullptr;
    hts_idx_t*                               idx_  = nullptr;   // optional
    hts_itr_multi_t*                         iter_ = nullptr;   // optional
    bam_plp_t                                plp_  = nullptr;
    bam1_t*                                  rec_  = nullptr;   // scratch for callback

    // Reference FASTA for -f/--pileup (ref column + ./, match notation). The
    // current contig's sequence is fetched once and cached (freed on tid change).
    faidx_t*                                 fai_  = nullptr;
    mutable char*                            ref_cache_ = nullptr;
    mutable int                              ref_cache_tid_ = -1;
    mutable hts_pos_t                        ref_cache_len_ = 0;

    // Reference bases for the current pileup column (nullptr = no -f); `ref_for`
    // fetches + caches the contig for `tid`, returning the whole-contig sequence.
    const char* ref_for(int tid) const {
        if (!fai_) return nullptr;
        if (tid == ref_cache_tid_) return ref_cache_;
        if (ref_cache_) { free(ref_cache_); ref_cache_ = nullptr; }
        ref_cache_tid_ = tid;
        ref_cache_len_ = 0;
        const char* name = sam_hdr_tid2name(hdr_, tid);
        if (!name) return nullptr;
        hts_pos_t clen = sam_hdr_tid2len(hdr_, tid);
        if (clen <= 0) return nullptr;
        ref_cache_ = faidx_fetch_seq64(fai_, name, 0, clen - 1, &ref_cache_len_);
        return ref_cache_;   // nullptr if the contig is absent from the FASTA
    }

    // Requested regions parsed at open time. bam_plp_auto emits every
    // position covered by the iterator's fetched reads, so a query like
    // `-r chr1:105-105` would otherwise spill the full span of any read
    // touching pos 105. Match `samtools mpileup`'s behaviour by filtering
    // emitted positions to the requested ranges.
    struct RegionRange { int tid; hts_pos_t beg; hts_pos_t end; };
    std::vector<RegionRange>                 regions_;

    // Streaming-source plumbing (same shape as BamSource / BcfSource).
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             all_read_    = false;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;
    mutable int64_t                          num_rows_total_ = -1;
    mutable arrow::Status                    read_status_;   // sticky stream error

    static constexpr int BATCH_ROWS = 16384;

    // Per-pileup-iterator state. We give one of these to bam_plp_init so
    // the read-callback knows where to pull alignments from. last_rc records
    // the most recent underlying read return: bam_plp_auto() returns nullptr
    // on both EOF and error, so this is how advance() tells them apart.
    struct PlpData {
        samFile*           fp;
        sam_hdr_t*         hdr;
        hts_itr_multi_t*   iter;     // nullptr → full-file scan
        int                last_rc = 0;  // >=0 record, -1 EOF, <-1 read error
    };
    PlpData plp_data_;

    static int plp_callback(void* data, bam1_t* b) {
        auto* d = static_cast<PlpData*>(data);
        int rc = d->iter ? sam_itr_multi_next(d->fp, d->iter, b)
                         : sam_read1(d->fp, d->hdr, b);
        d->last_rc = rc;
        return rc;
    }

    // Render one position's bases / quals strings from the bam_pileup1_t
    // array. `ref` is the current contig's sequence (nullptr without -f) and
    // `pos` the 0-based column: with a reference, a read base matching the ref
    // becomes `.` (forward) / `,` (reverse) and deleted bases are filled from
    // the reference — matching `samtools mpileup -f`. Without one, bases are the
    // literal letters (uppercase forward, lowercase reverse) and there is no
    // match notation.
    static void format_pileup_row(const bam_pileup1_t* plp, int n,
                                   std::string& bases, std::string& quals,
                                   const char* ref, hts_pos_t ref_len,
                                   hts_pos_t pos) {
        bases.clear();
        quals.clear();
        for (int i = 0; i < n; ++i) {
            const bam_pileup1_t* p = &plp[i];
            // Read-start marker: ^<mapq+33> precedes the base.
            if (p->is_head) {
                bases += '^';
                int mq = p->b->core.qual;
                if (mq > 93) mq = 93;
                bases += (char)(mq + 33);
            }
            // Base column: '*' for a deletion, '>' / '<' (forward/reverse) for
            // a reference skip — CIGAR N, e.g. an RNA-seq intron — else the
            // read base (uppercase forward, lowercase reverse). htslib sets
            // is_del for both deletions and refskips; is_refskip distinguishes
            // the two. (Previously refskips were rendered as '*', mislabelling
            // spliced reads as deletions.)
            if (p->is_del) {
                if (p->is_refskip) bases += bam_is_rev(p->b) ? '<' : '>';
                else               bases += '*';
            } else {
                uint8_t bnt = bam_seqi(bam_get_seq(p->b), p->qpos);
                char nt = seq_nt16_str[bnt];             // uppercase A/C/G/T/N/=
                bool rev = bam_is_rev(p->b);
                if (ref) {
                    int rb = (pos < ref_len) ? (unsigned char)ref[pos] : 'N';
                    if (nt == '=' ||
                        seq_nt16_table[(uint8_t)nt] == seq_nt16_table[(uint8_t)rb])
                        bases += rev ? ',' : '.';        // matches the reference
                    else
                        bases += rev ? (char)std::tolower(nt) : nt;
                } else {
                    bases += rev ? (char)std::tolower(nt) : nt;
                }
            }
            // Quality column: samtools emits the base quality at qpos for every
            // element — including deletions and reference skips — never '*'.
            // BAM stores Phred directly; pileup format is Phred+33.
            {
                int q = (p->qpos < p->b->core.l_qseq)
                            ? bam_get_qual(p->b)[p->qpos] : 0;
                if (q == 0xff) q = 0;
                quals += (char)(q + 33);
            }
            // Indel description on the current base. Insertion bases come
            // from the read at qpos+1 .. qpos+indel; deletion bases come
            // from the reference (we use 'N' since -f isn't supported).
            if (p->indel > 0) {
                bases += '+';
                bases += std::to_string(p->indel);
                bool rev = bam_is_rev(p->b);
                for (int k = 1; k <= p->indel; ++k) {
                    uint8_t b = bam_seqi(bam_get_seq(p->b), p->qpos + k);
                    char nt = seq_nt16_str[b];
                    if (rev) nt = (char)std::tolower(nt);
                    bases += nt;
                }
            } else if (p->indel < 0) {
                int n_del = -p->indel;
                bases += '-';
                bases += std::to_string(n_del);
                bool rev = bam_is_rev(p->b);
                for (int k = 1; k <= n_del; ++k) {
                    // Deleted bases come from the reference (or 'N' without -f).
                    char rc = (ref && pos + k < ref_len)
                                  ? ref[pos + k] : 'N';
                    bases += rev ? (char)std::tolower((unsigned char)rc)
                                 : (char)std::toupper((unsigned char)rc);
                }
            }
            if (p->is_tail) bases += '$';
        }
    }

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder b_chrom, b_ref, b_bases, b_quals;
        arrow::Int64Builder  b_pos, b_depth;

        int count = 0;
        int tid, pos, n_plp;
        const bam_pileup1_t* plp_arr;
        std::string bases_str, quals_str;
        while (count < BATCH_ROWS &&
               (plp_arr = bam_plp_auto(plp_, &tid, &pos, &n_plp)) != nullptr) {
            if (tid < 0) continue;
            // Match `samtools mpileup`'s region-trimming: bam_plp_auto
            // returns every position covered by the iterator-fetched
            // reads, but only those whose pos falls inside a requested
            // range should be emitted.
            if (!regions_.empty()) {
                bool in_any = false;
                for (const auto& r : regions_) {
                    if (r.tid == tid && pos >= r.beg && pos < r.end) {
                        in_any = true; break;
                    }
                }
                if (!in_any) continue;
            }
            const char* ref = ref_for(tid);   // nullptr without -f
            format_pileup_row(plp_arr, n_plp, bases_str, quals_str,
                              ref, ref_cache_len_, pos);
            const char* chrom = sam_hdr_tid2name(hdr_, tid);
            ARROW_RETURN_NOT_OK(b_chrom.Append(chrom ? chrom : "*"));
            ARROW_RETURN_NOT_OK(b_pos.Append((int64_t)pos + 1));  // 1-based
            // Ref column: the FASTA base as-is (case preserved, like samtools),
            // 'N' where there's no reference.
            char rb = (ref && pos < ref_cache_len_) ? ref[pos] : 'N';
            ARROW_RETURN_NOT_OK(b_ref.Append(std::string(1, rb)));
            ARROW_RETURN_NOT_OK(b_depth.Append(n_plp));
            ARROW_RETURN_NOT_OK(b_bases.Append(bases_str));
            ARROW_RETURN_NOT_OK(b_quals.Append(quals_str));
            ++count;
        }
        // bam_plp_auto() returned nullptr for one of two reasons: clean EOF
        // (last_rc == -1) or a read error (< -1, e.g. a truncated/corrupt BAM).
        // Record the latter stickily so the CLI reports a truncated file rather
        // than silently emitting a partial pileup with exit 0.
        if (plp_data_.last_rc < -1) {
            read_status_ = arrow::Status::IOError(
                "error reading BAM record from ", path_);
            all_read_ = true;
        }
        if (count == 0) {
            all_read_ = true;
            num_rows_total_ = rows_so_far_;
            return read_status_;
        }
        std::shared_ptr<arrow::Array> a_chrom, a_pos, a_ref, a_depth, a_bases, a_quals;
        ARROW_RETURN_NOT_OK(b_chrom.Finish(&a_chrom));
        ARROW_RETURN_NOT_OK(b_pos.Finish(&a_pos));
        ARROW_RETURN_NOT_OK(b_ref.Finish(&a_ref));
        ARROW_RETURN_NOT_OK(b_depth.Finish(&a_depth));
        ARROW_RETURN_NOT_OK(b_bases.Finish(&a_bases));
        ARROW_RETURN_NOT_OK(b_quals.Finish(&a_quals));
        auto batch = arrow::RecordBatch::Make(schema_, count,
            {a_chrom, a_pos, a_ref, a_depth, a_bases, a_quals});
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    ~BamPileupSource() {
        if (plp_)  { bam_plp_destroy(plp_);  plp_  = nullptr; }
        if (iter_) { hts_itr_multi_destroy(iter_); iter_ = nullptr; }
        if (rec_)  { bam_destroy1(rec_);     rec_  = nullptr; }
        if (idx_)  { hts_idx_destroy(idx_);  idx_  = nullptr; }
        if (hdr_)  { sam_hdr_destroy(hdr_);  hdr_  = nullptr; }
        if (fp_)   { sam_close(fp_);         fp_   = nullptr; }
        if (ref_cache_) { free(ref_cache_);  ref_cache_ = nullptr; }
        if (fai_)  { fai_destroy(fai_);      fai_  = nullptr; }
    }

    static std::string open(const std::string& path, const Config& cfg,
                             std::unique_ptr<BamPileupSource>* out) {
        auto self = std::make_unique<BamPileupSource>();
        self->path_ = path;

        self->fp_ = sam_open(path.c_str(), "r");
        if (!self->fp_) return "Cannot open '" + path + "'";
        int n = effective_threads(cfg);
        if (n > 1) hts_set_threads(self->fp_, n);

        self->hdr_ = sam_hdr_read(self->fp_);
        if (!self->hdr_)
            return "Cannot read BAM/SAM header from '" + path + "'";

        // Optional reference FASTA (-f): enables the ref column and the ./,
        // match notation, matching `samtools mpileup -f`.
        if (!cfg.pileup_ref.empty()) {
            self->fai_ = fai_load(cfg.pileup_ref.c_str());
            if (!self->fai_)
                return "Cannot load reference FASTA '" + cfg.pileup_ref +
                       "' (need a .fai index; run `samtools faidx`)";
        }

        // Detect format for the footer string.
        const htsFormat* fmt = hts_get_format(self->fp_);
        switch (fmt ? fmt->format : unknown_format) {
            case cram: self->fmt_name_ = "CRAM"; break;
            case sam:  self->fmt_name_ = "SAM";  break;
            default:   self->fmt_name_ = "BAM";  break;
        }

        // Optional region query. Needs a coordinate-sorted file with an
        // index (.bai / .csi / .crai). We accept comma-separated regions
        // via the same `chrom:start-end[,…]` syntax tabix uses.
        if (!cfg.region.empty()) {
            self->idx_ = sam_index_load(self->fp_, path.c_str());
            if (!self->idx_)
                return "'" + path + "': --pileup -r needs an index (.bai/.csi/.crai)";
            // cfg.region is canonical 0-based half-open; both sam_itr_regarray
            // and the hts_parse_region filter below read region strings as
            // 1-based inclusive, so convert each window at the boundary.
            std::vector<Region> windows = parse_region_list(cfg.region);
            resolve_region_chroms(windows,
                [&](const std::string& n){ return bam_name2id(self->hdr_, n.c_str()) >= 0; },
                path);
            std::vector<std::string> regs;
            for (const auto& w : windows) regs.push_back(region_to_htslib(w));
            std::vector<const char*> regp;
            regp.reserve(regs.size());
            for (auto& r : regs) regp.push_back(r.c_str());
            self->iter_ = sam_itr_regarray(self->idx_, self->hdr_,
                                            const_cast<char**>(regp.data()),
                                            (unsigned)regp.size());
            if (!self->iter_)
                return "'" + path + "': cannot build iterator for region '" +
                       cfg.region + "'";

            // Also resolve each region into a tid + half-open [beg, end)
            // span for the per-position filter applied during pileup
            // emission. hts_parse_region accepts `chrom`, `chrom:N`, and
            // `chrom:beg-end` (beg/end 1-based inclusive, returned as
            // 0-based half-open).
            for (auto& r : regs) {
                hts_pos_t beg = 0, end = 0;
                int tid = -1;
                const char* rest = hts_parse_region(
                    r.c_str(), &tid, &beg, &end,
                    (hts_name2id_f)bam_name2id, self->hdr_,
                    HTS_PARSE_THOUSANDS_SEP);
                if (rest && tid >= 0)
                    self->regions_.push_back({tid, beg, end});
            }
        }

        self->plp_data_.fp   = self->fp_;
        self->plp_data_.hdr  = self->hdr_;
        self->plp_data_.iter = self->iter_;
        self->plp_ = bam_plp_init(&BamPileupSource::plp_callback,
                                    &self->plp_data_);
        if (!self->plp_) return "Out of memory initialising pileup iterator";
        // Match `samtools mpileup` default behaviour (no max-depth cap;
        // INT_MAX yields the full per-position depth).
        bam_plp_set_maxcnt(self->plp_, INT_MAX);

        self->rec_ = bam_init1();

        self->schema_ = arrow::schema({
            arrow::field("chrom", arrow::utf8()),
            arrow::field("pos",   arrow::int64()),
            arrow::field("ref",   arrow::utf8()),
            arrow::field("depth", arrow::int64()),
            arrow::field("bases", arrow::utf8()),
            arrow::field("quals", arrow::utf8()),
        });

        // Eager first batch so schema-only views see a populated source.
        auto st = self->advance();
        if (!st.ok())
            return "Error pileup-walking '" + path + "': " + st.ToString();

        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        return all_read_ ? num_rows_total_ : -1;
    }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    bool region_applied() const override { return iter_ != nullptr; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    arrow::Status read_status() const override { return read_status_; }
    std::string footer() const override {
        return "Format: mpileup (from " + fmt_name_ + ")";
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
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             all_read_ = false;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;
    mutable arrow::Status                    read_status_;   // sticky stream error

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
                if (r < -1) return r;   // genuine read error — propagate (a
                                        // truncated/corrupt file, not iter EOF)
                ++cur_iter_;            // r == -1: this iterator done; next one
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
            // After the eight fixed fields (CHROM..INFO), `start` points at the
            // FORMAT field. Everything from there on — FORMAT plus the
            // per-sample columns — is kept verbatim as the FORMAT_SAMPLES value.
            // (Previously this skipped to after the next tab, dropping the
            // FORMAT spec such as GT:AD:DP entirely.)
            if (fi < 8) {
                // Pathological short line — fill remaining columns with ".".
                size_t end = line.find('\t', start);
                f[fi++] = (end == std::string_view::npos)
                          ? line.substr(start)
                          : line.substr(start, end - start);
                while (fi < 8) f[fi++] = ".";
                f[8] = std::string_view{};
            } else {
                f[8] = line.substr(start);   // FORMAT + samples, verbatim
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
                // std::from_chars<float> is missing in Apple's libc++ on
                // some macOS runners; strtof works everywhere. f[5] is a
                // string_view (not nul-terminated), so copy to a temp.
                std::string qbuf(f[5]);
                float q = std::strtof(qbuf.c_str(), nullptr);
                ARROW_RETURN_NOT_OK(qual_b.Append(q));
            }
            ARROW_RETURN_NOT_OK(filter_b.Append(f[6].data(), (int32_t)f[6].size()));
            ARROW_RETURN_NOT_OK(info_b.Append  (f[7].data(), (int32_t)f[7].size()));
            if (n_samples_ > 0)
                ARROW_RETURN_NOT_OK(samples_b.Append(f[8].data(), (int32_t)f[8].size()));

            ++count;
        }
        if (s.s) free(s.s);

        if (ret < -1) {
            // Record the error stickily (callers ignore advance()'s return) so
            // the CLI reports a truncated/corrupt file instead of silently
            // emitting a partial result with exit 0.
            read_status_ = arrow::Status::IOError(
                "error reading BCF record from ", path_);
            all_read_ = true;
        }
        if (count == 0) { all_read_ = true; return read_status_; }
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
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
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
            // cfg.region is canonical 0-based half-open; bcf_itr_querys reads
            // its string argument as 1-based inclusive, so convert per window.
            std::vector<Region> windows = parse_region_list(cfg.region);
            resolve_region_chroms(windows,
                [&](const std::string& n){ return bcf_hdr_name2id(self->hdr_, n.c_str()) >= 0; },
                path);
            for (const auto& r : windows) {
                std::string rstr = region_to_htslib(r);
                hts_itr_t* it =
                    bcf_itr_querys(self->idx_, self->hdr_, rstr.c_str());
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
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    bool region_applied() const override { return region_mode_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
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
    arrow::Status read_status() const override { return read_status_; }
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
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;

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
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
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
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    bool region_applied() const override { return region_mode_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
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
    mutable std::vector<int64_t>           batch_num_rows_;   // kept after eviction
    mutable int64_t                        rows_so_far_ = 0;
    mutable bool                           all_read_    = false;
    mutable bool                           retain_all_  = false;
    mutable bool                           evicted_any_ = false;
    mutable arrow::Status                  read_status_;   // sticky stream error

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
        if (ret < -1) {
            // Malformed record (kseq_read returns < -1). Record it stickily and
            // stop so ensure()'s (void)advance() loop can't spin forever.
            read_status_ = arrow::Status::IOError(
                "Error reading FASTA/FASTQ from ", path_);
            all_read_ = true;
            return read_status_;
        }
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
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
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
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i)
            (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        if (batches_.empty() && !all_read_ && rows > 0)
            ARROW_RETURN_NOT_OK(advance(rows));
        return TabularSource::read_first(rows, col_indices, out);
    }
    arrow::Status read_status() const override { return read_status_; }
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

// ── Plain-text source ────────────────────────────────────────────────────────
//
// A text file is modelled as one utf8 column named `line`, one row per line.
// That is what buys tabs, `/` search, `&` filter, themes, per-tab state, the
// status bar and the multi-file loop for free: every one of those walks Arrow
// columns through cell_to_string(), which works unchanged over a single string
// column. The TUI then renders a text tab without the header row and without
// truncation (see TableTUI::text_view_).
//
// Streaming and forward-only, like DelimitedSource — logs are the point, so
// slurping (md::slurp_file, which has no size cap) is not an option.
//
// Deliberately NOT LineReader: that strips '\r' anywhere in a line, so a CRLF
// file would not round-trip. This splits on '\n' only and keeps every other
// byte, including a trailing '\r'.
//
// Line terminators are dropped from the stored value and re-added on output,
// so `vv f.txt > copy` is byte-identical to f.txt. A file whose last line has
// no trailing newline is remembered (final_newline_ = false) and printed
// without one.

// Result of the binary heuristic.
enum class TextSniffResult { Text, Binary, Utf16, Utf32 };

// Decide whether a buffer (the first ~8 KiB of a file) is plain text.
//
// Order matters. A UTF-16 file is full of NUL bytes, so the BOM test has to
// come before the NUL test or every Windows-exported file gets the generic
// "binary" message instead of one naming iconv.
static TextSniffResult sniff_text(const char* p, size_t n) {
    const unsigned char* d = (const unsigned char*)p;
    if (n == 0) return TextSniffResult::Text;          // empty file is text
    // 1. Byte-order marks, longest first (UTF-32LE starts with the UTF-16LE
    //    BOM, so testing UTF-16 first would misreport it).
    if (n >= 4 && d[0]==0xFF && d[1]==0xFE && d[2]==0x00 && d[3]==0x00)
        return TextSniffResult::Utf32;
    if (n >= 4 && d[0]==0x00 && d[1]==0x00 && d[2]==0xFE && d[3]==0xFF)
        return TextSniffResult::Utf32;
    if (n >= 3 && d[0]==0xEF && d[1]==0xBB && d[2]==0xBF)
        return TextSniffResult::Text;                  // UTF-8 BOM
    if (n >= 2 && ((d[0]==0xFF && d[1]==0xFE) || (d[0]==0xFE && d[1]==0xFF)))
        return TextSniffResult::Utf16;
    // 2. Any NUL → binary.
    if (std::memchr(p, 0, n) != nullptr) return TextSniffResult::Binary;
    // 3. Too many C0 control bytes → binary. \t \n \r \f and ESC are all
    //    normal in text; excluding ESC matters or an ANSI-coloured log gets
    //    refused. DEL (0x7F) counts as a control byte.
    size_t ctrl = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = d[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0x1b)
            continue;
        if (c < 0x20 || c == 0x7f) ++ctrl;
    }
    return (ctrl * 20 > n) ? TextSniffResult::Binary   // > 5%
                           : TextSniffResult::Text;
}

// The error text for a file the sniffer refused. Split out so stdin and the
// path ladder word it identically. The caller already prefixes "vv: <path>: ",
// so `what` appears only inside the suggested command line.
static std::string text_binary_error(const std::string& what,
                                     TextSniffResult r) {
    if (r == TextSniffResult::Utf16 || r == TextSniffResult::Utf32) {
        const char* enc = (r == TextSniffResult::Utf16) ? "UTF-16" : "UTF-32";
        return std::string(enc) + " text is not supported. Convert it first: "
               "`iconv -f " + enc + " -t UTF-8 " + what + " | vv -`.";
    }
    return "binary file, not shown. vv views text and the formats listed by "
           "`vv --formats`; it has no hex view.";
}

class TextSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    std::shared_ptr<arrow::io::InputStream> in_;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>           batch_first_row_;
    mutable std::vector<int64_t>           batch_num_rows_;
    mutable int64_t                        rows_so_far_   = 0;
    mutable bool                           all_read_      = false;
    mutable bool                           retain_all_    = false;
    mutable bool                           evicted_any_   = false;
    mutable bool                           final_newline_ = true;
    mutable std::string                    pending_;      // partial last line
    mutable arrow::Status                  read_status_;
    bool                                   gz_ = false;

    static constexpr int    BATCH_SIZE = 4096;
    static constexpr size_t READ_SIZE  = 64 * 1024;

    arrow::Status advance() const {
        if (all_read_) return arrow::Status::OK();
        arrow::StringBuilder b;
        int count = 0;
        std::string buf;
        buf.resize(READ_SIZE);
        while (count < BATCH_SIZE) {
            // Emit every complete line already buffered.
            size_t start = 0;
            for (;;) {
                size_t nl = pending_.find('\n', start);
                if (nl == std::string::npos) break;
                ARROW_RETURN_NOT_OK(b.Append(pending_.data() + start,
                                             (int32_t)(nl - start)));
                start = nl + 1;
                if (++count >= BATCH_SIZE) break;
            }
            if (start) pending_.erase(0, start);
            if (count >= BATCH_SIZE) break;

            auto got = in_->Read((int64_t)READ_SIZE, buf.data());
            if (!got.ok()) {
                read_status_ = got.status();
                all_read_ = true;
                break;
            }
            if (*got == 0) {
                // EOF. A non-empty remainder is a final line with no newline.
                if (!pending_.empty()) {
                    ARROW_RETURN_NOT_OK(b.Append(pending_.data(),
                                                 (int32_t)pending_.size()));
                    ++count;
                    pending_.clear();
                    final_newline_ = false;
                }
                all_read_ = true;
                break;
            }
            pending_.append(buf.data(), (size_t)*got);
        }
        if (count == 0) { all_read_ = true; return read_status_; }

        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(b.Finish(&arr));
        auto batch = arrow::RecordBatch::Make(schema_, count, {arr});
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_, std::move(batch));
        return arrow::Status::OK();
    }

public:
    static std::string open(const std::string& path, bool gz,
                            const Config& /*cfg*/,
                            std::unique_ptr<TextSource>* out) {
        auto self = std::make_unique<TextSource>();
        self->path_ = path;
        self->gz_   = gz;
        self->schema_ = arrow::schema({arrow::field("line", arrow::utf8())});

        auto rf = arrow::io::ReadableFile::Open(path);
        if (!rf.ok())
            return "Cannot open '" + path + "': " + rf.status().ToString();
        std::shared_ptr<arrow::io::InputStream> in = rf.ValueOrDie();
        if (gz) {
            auto codec = arrow::util::Codec::Create(arrow::Compression::GZIP);
            if (!codec.ok()) return codec.status().ToString();
            auto ci = arrow::io::CompressedInputStream::Make(codec->get(), in);
            if (!ci.ok()) return ci.status().ToString();
            in = ci.ValueOrDie();
        }
        self->in_ = std::move(in);

        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + path + "': " + st.ToString();
        *out = std::move(self);
        return "";
    }

    // Same, over an already-open stream (stdin).
    static std::string open_stream(const std::string& label,
                                   std::shared_ptr<arrow::io::InputStream> in,
                                   std::unique_ptr<TextSource>* out) {
        auto self = std::make_unique<TextSource>();
        self->path_   = label;
        self->in_     = std::move(in);
        self->schema_ = arrow::schema({arrow::field("line", arrow::utf8())});
        auto st = self->advance();
        if (!st.ok()) return "Error reading '" + label + "': " + st.ToString();
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i) (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
    arrow::Status read_status() const override { return read_status_; }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return std::string("Format: text") + (gz_ ? " (gzip)" : "");
    }
    // True when the file's last line carries no terminator, so a verbatim
    // dump can reproduce that.
    bool final_newline() const { return final_newline_; }
    bool is_text() const override { return true; }
};

// ── 2bit (UCSC) source ────────────────────────────────────────────────────────
//
// 2bit is the UCSC binary container for genome-scale DNA sequences
// (typically a hg38.2bit or mm10.2bit reference). Each base is packed
// into 2 bits, with side tables for N-runs and soft-masked regions.
// Spec: https://genome.ucsc.edu/FAQ/FAQformat.html#format7
//
// vv exposes the file's sequence *index*, not the bases themselves —
// chromosomes are massive (the human reference decodes to ~3 GB of
// strings) and the typical use of `vv hg38.2bit` is "what's in this
// file?" The columns are name / length_bp / n_blocks / mask_blocks
// (the last two are counts of unknown-base and lowercase-soft-mask
// runs, useful for spotting unmasked or contig-poor assemblies).
class TwoBitSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    std::shared_ptr<arrow::RecordBatch>    batch_;
    int64_t                                seq_count_ = 0;

public:
    static std::string open(const std::string& path, const Config& /*cfg*/,
                             std::unique_ptr<TwoBitSource>* out) {
        auto self = std::make_unique<TwoBitSource>();
        self->path_ = path;

        FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp)
            return "Cannot open '" + path + "': " + std::strerror(errno);

        // Header: signature, version, seqCount, reserved (4 × 4 bytes).
        uint8_t hdr[16];
        if (std::fread(hdr, 1, 16, fp) != 16) {
            std::fclose(fp);
            return "Cannot read 2bit header from '" + path + "'";
        }
        auto le32 = [](const uint8_t* p) -> uint32_t {
            return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                 | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        };
        auto be32 = [](const uint8_t* p) -> uint32_t {
            return (uint32_t)p[3] | ((uint32_t)p[2] << 8)
                 | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24);
        };
        uint32_t sig_le = le32(hdr);
        bool be = false;
        if (sig_le != 0x1A412743u) {
            uint32_t sig_be = be32(hdr);
            if (sig_be != 0x1A412743u) {
                std::fclose(fp);
                return "Not a 2bit file (bad signature) at '" + path + "'";
            }
            be = true;
        }
        auto r32 = [&](const uint8_t* p) { return be ? be32(p) : le32(p); };
        uint32_t version    = r32(hdr + 4);
        uint32_t seq_count  = r32(hdr + 8);
        if (version != 0) {
            std::fclose(fp);
            return "2bit version " + std::to_string(version) +
                   " (long-offset variant) is not supported by vv";
        }

        // Pass 1: read the sequence index (name + 32-bit offset per seq).
        struct Idx { std::string name; uint32_t offset; };
        std::vector<Idx> idx;
        // seq_count is an attacker-controllable uint32 (up to ~4.3e9). Each
        // index entry needs at least 5 bytes on disk (1-byte name length + ≥0
        // name + 4-byte offset), so reject a count the file cannot possibly
        // hold rather than let reserve() request ~170 GB and abort the process.
        long fsize = (std::fseek(fp, 0, SEEK_END) == 0) ? std::ftell(fp) : -1;
        std::fseek(fp, 16, SEEK_SET);   // rewind to just past the header
        uint64_t max_seqs = (fsize >= 16) ? ((uint64_t)fsize - 16) / 5 : 0;
        if (fsize >= 16 && seq_count > max_seqs) {
            std::fclose(fp);
            return "2bit sequence count " + std::to_string(seq_count) +
                   " exceeds the size of '" + path + "'";
        }
        idx.reserve(std::min<uint32_t>(seq_count, 1u << 20));
        for (uint32_t i = 0; i < seq_count; ++i) {
            uint8_t name_size;
            if (std::fread(&name_size, 1, 1, fp) != 1) {
                std::fclose(fp);
                return "Truncated 2bit sequence index at '" + path + "'";
            }
            std::string name(name_size, '\0');
            if (std::fread(name.data(), 1, name_size, fp) != name_size) {
                std::fclose(fp);
                return "Truncated 2bit sequence name at '" + path + "'";
            }
            uint8_t off_buf[4];
            if (std::fread(off_buf, 1, 4, fp) != 4) {
                std::fclose(fp);
                return "Truncated 2bit offset at '" + path + "'";
            }
            idx.push_back({std::move(name), r32(off_buf)});
        }

        // Pass 2: jump to each seqRecord header and read dna_size,
        // n_block_count, mask_block_count. The packed DNA payload after
        // the mask block tables is skipped entirely.
        arrow::StringBuilder name_b;
        arrow::UInt32Builder length_b, nb_b, mb_b;
        ARROW_UNUSED(name_b);  // suppress unused-warning until we Finish()
        for (auto& s : idx) {
            if (std::fseek(fp, (long)s.offset, SEEK_SET) != 0) {
                std::fclose(fp);
                return "Cannot seek to 2bit seqRecord at offset " +
                       std::to_string(s.offset);
            }
            uint8_t buf[4];
            auto read32 = [&](uint32_t* v) -> bool {
                if (std::fread(buf, 1, 4, fp) != 4) return false;
                *v = r32(buf); return true;
            };
            uint32_t dna_size, n_block_count, mask_block_count;
            if (!read32(&dna_size) || !read32(&n_block_count)) {
                std::fclose(fp);
                return "Truncated 2bit seqRecord (header)";
            }
            // Skip the N-block start/size arrays. Compute the byte count in
            // 64-bit: n_block_count*8 in uint32 would overflow for a crafted
            // count > 0x1FFFFFFF and seek to the wrong offset.
            if (std::fseek(fp, (long)((uint64_t)n_block_count * 8), SEEK_CUR) != 0) {
                std::fclose(fp);
                return "Cannot skip 2bit N-block table";
            }
            if (!read32(&mask_block_count)) {
                std::fclose(fp);
                return "Truncated 2bit seqRecord (mask count)";
            }
            // We have everything we want; no need to scan the rest.

            auto st = name_b.Append(s.name);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
            st = length_b.Append(dna_size);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
            st = nb_b.Append(n_block_count);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
            st = mb_b.Append(mask_block_count);
            if (!st.ok()) { std::fclose(fp); return st.ToString(); }
        }
        std::fclose(fp);

        std::shared_ptr<arrow::Array> a_name, a_len, a_nb, a_mb;
        auto st = name_b.Finish(&a_name);
        if (!st.ok()) return st.ToString();
        st = length_b.Finish(&a_len);
        if (!st.ok()) return st.ToString();
        st = nb_b.Finish(&a_nb);
        if (!st.ok()) return st.ToString();
        st = mb_b.Finish(&a_mb);
        if (!st.ok()) return st.ToString();

        self->schema_ = arrow::schema({
            arrow::field("name",        arrow::utf8()),
            arrow::field("length",      arrow::uint32()),
            arrow::field("n_blocks",    arrow::uint32()),
            arrow::field("mask_blocks", arrow::uint32()),
        });
        self->batch_ = arrow::RecordBatch::Make(self->schema_, idx.size(),
            {a_name, a_len, a_nb, a_mb});
        self->seq_count_ = (int64_t)idx.size();
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return seq_count_; }
    int     num_chunks() const override { return 1; }
    ChunkMeta chunk_meta(int /*i*/) const override { return {0, seq_count_}; }
    void ensure(int /*i*/) override {}
    arrow::Status read_chunk(int /*i*/, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        *out = batch_slice_to_table(*batch_, col_indices, schema_);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: 2bit  |  Sequences: " + std::to_string(seq_count_);
    }
    int min_col_width(int col_idx) const override {
        switch (col_idx) {
            case 0: return 8;   // name
            case 1: return 12;  // length
            default: return 6;
        }
    }
};

// ── SQLite source ─────────────────────────────────────────────────────────────
//
// One SqliteSource = one table in a SQLite file. Multi-table databases
// expand into one tab per table via SqliteSource::open_sibling_tables()
// which shares the underlying sqlite3 handle through a shared_ptr.
//
// Type-affinity mapping follows the SQLite documentation's substring
// rules (https://sqlite.org/datatype3.html#determination_of_column_affinity):
//   contains "INT"               → int64
//   contains CHAR/CLOB/TEXT      → string
//   contains BLOB / blank        → binary
//   contains REAL/FLOA/DOUB      → double
//   anything else (NUMERIC, …)   → string
// Column accessors (sqlite3_column_int64 / _double / _text / _blob)
// gracefully convert at read time, so a declared-INT column that
// actually holds a string still produces a sensible integer (or 0).
//
// The "anything else" bucket (NUMERIC affinity) covers NUMERIC, DECIMAL,
// DATE, DATETIME, TIMESTAMP, BOOLEAN, … — declarations that may legitimately
// store text (ISO dates), 64-bit integers beyond a double's 2^53 exact range,
// or booleans. Reading those through sqlite3_column_double silently corrupts
// them ('2026-06-10' → 2026.0; a big id rounded). SQLite is dynamically typed
// and such a column can hold mixed storage classes, so the only lossless fixed
// Arrow type is string: sqlite3_column_text renders each value faithfully.

static arrow::Type::type sqlite_type_to_arrow(const std::string& declared) {
    std::string u = declared;
    for (auto& c : u) c = (char)std::toupper((unsigned char)c);
    if (u.find("INT")  != std::string::npos) return arrow::Type::INT64;
    if (u.find("CHAR") != std::string::npos
     || u.find("CLOB") != std::string::npos
     || u.find("TEXT") != std::string::npos) return arrow::Type::STRING;
    if (u.find("BLOB") != std::string::npos || u.empty()) return arrow::Type::BINARY;
    if (u.find("REAL") != std::string::npos
     || u.find("FLOA") != std::string::npos
     || u.find("DOUB") != std::string::npos) return arrow::Type::DOUBLE;
    return arrow::Type::STRING;   // NUMERIC affinity — preserve verbatim
}
// Type id -> Arrow type, for the three places that carry a type id around
// instead of a DataType: the SQLite column mapper, the NumPy dtype mapper and
// ExpandedSource's declared-key schema.
//
// This MUST cover every id its callers can produce. It used to handle only
// INT64/DOUBLE/BINARY and fall through to utf8(), which meant the schema said
// `string` while the chunk carried the real array — an inconsistency Arrow does
// not check on the write path. `--parquet` failed loudly, but `--arrow` exited
// 0 and wrote an IPC file nothing could read back ("buffer_index out of
// range"), for 7 of the 9 NumPy dtypes and for every VCF Flag INFO key.
static std::shared_ptr<arrow::DataType> arrow_type_for_id(arrow::Type::type t) {
    switch (t) {
        case arrow::Type::BOOL:   return arrow::boolean();
        case arrow::Type::INT8:   return arrow::int8();
        case arrow::Type::INT16:  return arrow::int16();
        case arrow::Type::INT32:  return arrow::int32();
        case arrow::Type::INT64:  return arrow::int64();
        case arrow::Type::UINT8:  return arrow::uint8();
        case arrow::Type::UINT16: return arrow::uint16();
        case arrow::Type::UINT32: return arrow::uint32();
        case arrow::Type::UINT64: return arrow::uint64();
        case arrow::Type::FLOAT:  return arrow::float32();
        case arrow::Type::DOUBLE: return arrow::float64();
        case arrow::Type::BINARY: return arrow::binary();
        default:                  return arrow::utf8();
    }
}

// Quote a SQLite identifier (table name), escaping any embedded double quote by
// doubling it, per SQL. Table names come from the database's own catalog, so a
// table created as `a"b` would otherwise build the malformed — and, for an
// untrusted .sqlite, injectable — SQL `"a"b"`.
static std::string sqlite_quote_ident(const std::string& id) {
    std::string out = "\"";
    for (char c : id) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

class SqliteSource : public TabularSource {
    std::string                              path_;
    std::string                              table_;
    std::shared_ptr<sqlite3>                 db_;
    std::shared_ptr<arrow::Schema>           schema_;
    std::vector<arrow::Type::type>           col_types_;
    mutable int64_t                          total_rows_     = -1;  // lazy COUNT(*)
    mutable bool                             total_counted_  = false;
    std::vector<std::string>                 sibling_tables_;  // others in same DB

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             all_read_   = false;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;
    mutable arrow::Status                    read_status_;   // sticky stream error
    mutable sqlite3_stmt*                    stmt_       = nullptr;

    static constexpr int BATCH_SIZE = 4096;

    static std::shared_ptr<arrow::ArrayBuilder> make_builder(arrow::Type::type t) {
        switch (t) {
            case arrow::Type::INT64:  return std::make_shared<arrow::Int64Builder>();
            case arrow::Type::DOUBLE: return std::make_shared<arrow::DoubleBuilder>();
            case arrow::Type::BINARY: return std::make_shared<arrow::BinaryBuilder>();
            default:                  return std::make_shared<arrow::StringBuilder>();
        }
    }

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();
        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        std::vector<std::shared_ptr<arrow::ArrayBuilder>> builders;
        builders.reserve(col_types_.size());
        for (auto t : col_types_) builders.push_back(make_builder(t));

        int count = 0;
        while (count < cap) {
            int rc = sqlite3_step(stmt_);
            if (rc == SQLITE_DONE) { all_read_ = true; break; }
            if (rc != SQLITE_ROW) {
                // A step error mid-scan: record it stickily and stop so
                // ensure()'s (void)advance() loop can't spin forever.
                read_status_ = arrow::Status::IOError(
                    std::string("SQLite step error: ") + sqlite3_errmsg(db_.get()));
                all_read_ = true;
                return read_status_;
            }
            for (int i = 0; i < (int)col_types_.size(); ++i) {
                if (sqlite3_column_type(stmt_, i) == SQLITE_NULL) {
                    ARROW_RETURN_NOT_OK(builders[i]->AppendNull());
                    continue;
                }
                switch (col_types_[i]) {
                    case arrow::Type::INT64: {
                        auto b = static_cast<arrow::Int64Builder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(sqlite3_column_int64(stmt_, i)));
                        break;
                    }
                    case arrow::Type::DOUBLE: {
                        auto b = static_cast<arrow::DoubleBuilder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(sqlite3_column_double(stmt_, i)));
                        break;
                    }
                    case arrow::Type::BINARY: {
                        const void* p = sqlite3_column_blob(stmt_, i);
                        int n = sqlite3_column_bytes(stmt_, i);
                        auto b = static_cast<arrow::BinaryBuilder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(reinterpret_cast<const uint8_t*>(p), n));
                        break;
                    }
                    default: {
                        const unsigned char* p = sqlite3_column_text(stmt_, i);
                        int n = sqlite3_column_bytes(stmt_, i);
                        auto b = static_cast<arrow::StringBuilder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(
                            reinterpret_cast<const char*>(p), n));
                        break;
                    }
                }
            }
            ++count;
        }
        if (count == 0) return arrow::Status::OK();

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(builders.size());
        for (auto& b : builders) {
            std::shared_ptr<arrow::Array> a;
            ARROW_RETURN_NOT_OK(b->Finish(&a));
            arrays.push_back(a);
        }
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_,
                      arrow::RecordBatch::Make(schema_, count, arrays));
        return arrow::Status::OK();
    }

    static std::string build_one(const std::string& path,
                                  std::shared_ptr<sqlite3> db,
                                  const std::string& table,
                                  const std::vector<std::string>& siblings,
                                  std::unique_ptr<SqliteSource>* out) {
        auto self = std::make_unique<SqliteSource>();
        self->path_  = path;
        self->table_ = table;
        self->db_    = db;
        self->sibling_tables_ = siblings;

        // Schema from PRAGMA table_info. Quote the table name (escaping embedded
        // quotes) in case it contains spaces or special chars.
        std::string q = "PRAGMA table_info(" + sqlite_quote_ident(table) + ")";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.get(), q.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return std::string("SQLite prepare failed: ") + sqlite3_errmsg(db.get());
        }
        arrow::FieldVector fields;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* name = (const char*)sqlite3_column_text(st, 1);
            const char* type = (const char*)sqlite3_column_text(st, 2);
            int notnull      = sqlite3_column_int(st, 3);
            auto at = sqlite_type_to_arrow(type ? type : "");
            fields.push_back(arrow::field(name ? name : "",
                                          arrow_type_for_id(at), notnull == 0));
            self->col_types_.push_back(at);
        }
        sqlite3_finalize(st);
        if (fields.empty())
            return "SQLite table '" + table + "' has no columns (or doesn't exist)";
        self->schema_ = arrow::schema(fields);

        // Total row count is computed lazily (see total_rows()) — a COUNT(*) is
        // a full table scan, wasteful for an `-n` preview or a thumbnail that
        // never asks for the total.

        // Prepare the streaming SELECT and read the first batch.
        std::string sq = "SELECT * FROM " + sqlite_quote_ident(table);
        if (sqlite3_prepare_v2(db.get(), sq.c_str(), -1, &self->stmt_, nullptr) != SQLITE_OK)
            return std::string("SQLite prepare failed: ") + sqlite3_errmsg(db.get());

        auto astat = self->advance();
        if (!astat.ok()) return astat.ToString();
        *out = std::move(self);
        return "";
    }

public:
    ~SqliteSource() {
        if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
    }

    // Open the first table of a SQLite file as a SqliteSource. Remembers
    // the other user-table names so the caller can later request siblings
    // via open_sibling_tables() (for the multi-tab TUI path).
    static std::string open_first(const std::string& path,
                                   std::unique_ptr<SqliteSource>* out) {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(path.c_str(), &raw,
                            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            std::string err = raw ? sqlite3_errmsg(raw) : "(null handle)";
            if (raw) sqlite3_close(raw);
            return "Cannot open '" + path + "' as SQLite: " + err;
        }
        std::shared_ptr<sqlite3> db(raw, [](sqlite3* p){ sqlite3_close(p); });

        // List user tables (skip the sqlite_* internal tables).
        sqlite3_stmt* lst = nullptr;
        if (sqlite3_prepare_v2(db.get(),
                "SELECT name FROM sqlite_master "
                "WHERE type='table' AND name NOT LIKE 'sqlite_%' "
                "ORDER BY name", -1, &lst, nullptr) != SQLITE_OK) {
            return std::string("SQLite list-tables failed: ") + sqlite3_errmsg(db.get());
        }
        std::vector<std::string> tables;
        while (sqlite3_step(lst) == SQLITE_ROW)
            tables.emplace_back((const char*)sqlite3_column_text(lst, 0));
        sqlite3_finalize(lst);
        if (tables.empty())
            return "'" + path + "': SQLite database has no user tables";

        std::vector<std::string> siblings(tables.begin() + 1, tables.end());
        return build_one(path, db, tables.front(), siblings, out);
    }

    // Build SqliteSource instances for every table OTHER than the one
    // returned by open_first(). The shared sqlite3 handle is reused.
    std::vector<std::unique_ptr<TabularSource>> open_sibling_tables() const {
        std::vector<std::unique_ptr<TabularSource>> out;
        for (const auto& t : sibling_tables_) {
            std::unique_ptr<SqliteSource> s;
            std::string err = build_one(path_, db_, t, {}, &s);
            if (!err.empty()) {
                std::fprintf(stderr, "vv: SQLite table '%s': %s\n",
                             t.c_str(), err.c_str());
                continue;
            }
            out.push_back(std::move(s));
        }
        return out;
    }
    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        return open_sibling_tables();
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        // Once we've streamed the whole table the exact count is free.
        if (all_read_) return rows_so_far_;
        // Otherwise run COUNT(*) once, on first ask — so a preview / thumbnail
        // that never needs the total doesn't trigger a full table scan.
        if (!total_counted_) {
            total_counted_ = true;
            std::string cq = "SELECT COUNT(*) FROM " + sqlite_quote_ident(table_);
            sqlite3_stmt* st = nullptr;
            if (db_ && sqlite3_prepare_v2(db_.get(), cq.c_str(), -1, &st,
                                          nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW)
                    total_rows_ = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
            }
        }
        return total_rows_ >= 0 ? total_rows_ : (all_read_ ? rows_so_far_ : -1);
    }
    int     num_chunks() const override { return (int)batches_.size(); }
    arrow::Status read_status() const override { return read_status_; }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i) (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
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
    std::string tab_label() const override { return table_; }
    std::string footer() const override {
        std::string s = "Format: SQLite  |  Table: " + table_;
        // total_rows() runs the lazy COUNT(*); the footer is a "real" view
        // (table / TUI) where the count is wanted, unlike a thumbnail.
        int64_t tr = total_rows();
        if (tr >= 0) s += "  |  Rows: " + std::to_string(tr);
        if (!sibling_tables_.empty())
            s += "  |  +" + std::to_string(sibling_tables_.size()) + " more table(s)";
        return s;
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
    mutable arrow::Status                             read_status_;  // sticky error

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
        auto maybe_b = rdr_->ReadRecordBatch(i);
        if (!maybe_b.ok()) {
            // A batch that fails to decode (truncated/corrupt IPC) must not
            // leave ensure() spinning: record the error stickily and stop.
            read_status_ = maybe_b.status();
            all_read_ = true;
            return maybe_b.status();
        }
        auto b = maybe_b.ValueOrDie();
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
        if (is_feather_) return (int)batches_.size();
        // A 0-batch Arrow IPC seeds one zero-row batch (see open) so its schema
        // still renders; surface that instead of the raw 0, which would make
        // the seeded batch unreachable and the table view draw nothing.
        return num_record_batches_ > 0 ? num_record_batches_
                                       : (int)batches_.size();
    }
    arrow::Status read_status()                 const override { return read_status_; }
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

// ── Apache ORC source ────────────────────────────────────────────────────────
//
// Compiled in only when the Arrow build we link against has the ORC adapter
// (`arrow/adapters/orc/adapter.h`). CMake detects the header at configure
// time and defines VV_HAVE_ORC. The Apache Arrow apt repo and Homebrew's
// apache-arrow ship Arrow with ORC enabled; the AlmaLinux 8 static build
// passes -DARROW_ORC=ON to the arrow-build stage. Local dev builds need
// the same flag — without it `vv file.orc` reports a build-time message.

#if VV_HAVE_ORC
class OrcSource : public TabularSource {
    std::string                                              path_;
    std::shared_ptr<arrow::Schema>                           schema_;
    std::unique_ptr<arrow::adapters::orc::ORCFileReader>     reader_;
    int64_t                                                  num_stripes_ = 0;
    int64_t                                                  num_rows_total_ = 0;
    arrow::Compression::type                                 compression_ =
        arrow::Compression::UNCOMPRESSED;
    int64_t                                                  file_size_ = 0;
    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>                             batch_first_row_;
    mutable int64_t                                          rows_so_far_ = 0;

    arrow::Status load_stripe(int i) const {
        ARROW_ASSIGN_OR_RAISE(auto b,
            const_cast<arrow::adapters::orc::ORCFileReader*>(reader_.get())
                ->ReadStripe(i));
        batch_first_row_.push_back(rows_so_far_);
        rows_so_far_ += b->num_rows();
        batches_.push_back(std::move(b));
        return arrow::Status::OK();
    }

    static std::string fmt_size(int64_t sz) {
        char buf[32];
        if      (sz < 1024)             std::snprintf(buf,sizeof(buf),"%lld B",(long long)sz);
        else if (sz < 1024*1024)        std::snprintf(buf,sizeof(buf),"%.1f KiB",sz/1024.0);
        else if (sz < 1024LL*1024*1024) std::snprintf(buf,sizeof(buf),"%.2f MiB",sz/(1024.0*1024));
        else                            std::snprintf(buf,sizeof(buf),"%.2f GiB",sz/(1024.0*1024*1024));
        return buf;
    }
    static const char* codec_name(arrow::Compression::type c) {
        switch (c) {
            case arrow::Compression::UNCOMPRESSED: return "uncompressed";
            case arrow::Compression::SNAPPY:       return "snappy";
            case arrow::Compression::GZIP:         return "gzip";
            case arrow::Compression::LZO:          return "lzo";
            case arrow::Compression::LZ4:          return "lz4";
            case arrow::Compression::LZ4_FRAME:    return "lz4_frame";
            case arrow::Compression::ZSTD:         return "zstd";
            default:                                return "?";
        }
    }

public:
    static std::string open(const std::string& path,
                             std::unique_ptr<OrcSource>* out) {
        auto self = std::make_unique<OrcSource>();
        self->path_ = path;

        auto maybe_file = arrow::io::ReadableFile::Open(path);
        if (!maybe_file.ok())
            return "Cannot open '" + path + "': " + maybe_file.status().ToString();
        auto file = maybe_file.ValueOrDie();

        auto maybe_rdr = arrow::adapters::orc::ORCFileReader::Open(
            file, arrow::default_memory_pool());
        if (!maybe_rdr.ok())
            return "Not a valid ORC file: " + maybe_rdr.status().ToString();
        self->reader_ = std::move(*maybe_rdr);

        auto sch_or = self->reader_->ReadSchema();
        if (!sch_or.ok())
            return "ORC schema read failed: " + sch_or.status().ToString();
        self->schema_         = *sch_or;
        self->num_stripes_    = self->reader_->NumberOfStripes();
        self->num_rows_total_ = self->reader_->NumberOfRows();
        self->file_size_      = self->reader_->GetFileLength();
        auto cmp_or = self->reader_->GetCompression();
        if (cmp_or.ok()) self->compression_ = *cmp_or;

        // Empty file: seed an empty batch so schema-only views still work.
        if (self->num_stripes_ == 0) {
            self->batch_first_row_.push_back(0);
            self->batches_.push_back(arrow::RecordBatch::Make(
                self->schema_, 0, std::vector<std::shared_ptr<arrow::Array>>(
                    self->schema_->num_fields(),
                    arrow::MakeArrayOfNull(arrow::utf8(), 0).ValueOrDie())));
        }
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return num_rows_total_; }
    int     num_chunks() const override {
        return num_stripes_ == 0 ? 1 : (int)num_stripes_;
    }
    ChunkMeta chunk_meta(int i) const override {
        if (i >= (int)batches_.size())
            const_cast<OrcSource*>(this)->ensure(i);
        return {batch_first_row_[i], batches_[i]->num_rows()};
    }
    void ensure(int i) override {
        while ((int)batches_.size() <= i &&
               (int64_t)batches_.size() < num_stripes_) {
            auto st = load_stripe((int)batches_.size());
            if (!st.ok()) break;
        }
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
        std::string s = "Format: ORC  |  Stripes: " +
            std::to_string(num_stripes_);
        s += "  |  Compressed: " + fmt_size(file_size_);
        s += "  |  Codec: " + std::string(codec_name(compression_));
        return s;
    }
};
#endif  // VV_HAVE_ORC

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

class WorkbookSource : public MemoryTableSource {
public:
    using MemoryTableSource::MemoryTableSource;
    virtual std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const = 0;
    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        return open_sibling_sheets();
    }
};

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
static arrow::Result<std::shared_ptr<arrow::Table>>
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
    } else if (s->in_sheet && std::strcmp(name, "table:table-cell") == 0) {
        s->cell_depth = 1;
        s->cell_repeat = 1;
        s->cell_value_type.clear();
        s->cell_typed_value.clear();
        s->cell_text.clear();
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
        if (std::strcmp(name, "table:table-cell") == 0) {
            // Pick the canonical value: typed attribute wins for numeric /
            // date / boolean cells; text is the fallback (string cells +
            // anything without office:value).
            std::string v;
            if (!s->cell_typed_value.empty() &&
                s->cell_value_type != "string" &&
                !s->cell_value_type.empty())
                v = std::move(s->cell_typed_value);
            else
                v = std::move(s->cell_text);

            // Multiple inline <text:p> children → newlines. The simplest
            // sanitisation for CSV is to swap them for spaces; preserving
            // them would require CR/LF quoting which Arrow CSV handles
            // but few biology-data consumers will look for.
            for (char& c : v) if (c == '\n' || c == '\r') c = ' ';

            s->row_cells.push_back(std::move(v));
            s->row_repeat.push_back(s->cell_repeat);
            s->cell_depth = 0;
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
            for (int r = 0; r < reps; ++r) {
                if (emitted) line += ',';
                csv_append_quoted(line, s->row_cells[i].c_str());
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
    static std::string open_first(const std::string& path,
                                   std::unique_ptr<OdsSource>* out) {
        std::string xml;
        std::string err = ods_unzip_content_xml(path, &xml);
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

// ── HDF5 / AnnData viewer (`.h5ad` / `.h5` / `.hdf5` / `.loom`) ──────────────
//
// One Hdf5Source instance == one TUI tab. Tab 0 is either:
//   - For AnnData: a "summary" tab (shape, X encoding, layer count, …).
//   - For generic HDF5: a "hierarchy" tab — one row per H5Lvisit object
//     with path / kind / shape / dtype / compression / n_attrs columns.
// Sibling tabs (one per AnnData component or one per 1-/2-D HDF5 dataset)
// are constructed lazily via the existing WorkbookSource sibling-expansion
// path in main(). The HDF5 file handle is refcounted across siblings so
// the file stays open exactly as long as any tab references it (mirrors
// SqliteSource's shared_ptr<sqlite3> trick).
//
// All read-time densification happens through small helpers that build
// arrow::Tables column by column from hyperslab selections — Arrow
// columns become typed (int64 / double / utf8) per HDF5 datatype class,
// so `--filter`, `--describe`, sort, and search all work out of the box.

namespace h5v {

// RAII for HDF5 ids. H5Fclose etc. are idempotent on negative ids, so
// the deleter handles the "open failed" case naturally.
struct H5Closer {
    int (*fn)(hid_t);
    void operator()(hid_t* p) const noexcept {
        if (p) { if (*p >= 0) fn(*p); delete p; }
    }
};
using H5FilePtr  = std::shared_ptr<hid_t>;
template <typename Fn>
static std::unique_ptr<hid_t, H5Closer> own(hid_t id, Fn closer) {
    return std::unique_ptr<hid_t, H5Closer>(new hid_t(id), H5Closer{closer});
}

// Look up a string attribute on an HDF5 object. Returns "" when absent
// or non-string.
static std::string read_string_attr(hid_t obj, const char* name) {
    if (H5Aexists(obj, name) <= 0) return std::string{};
    hid_t a = H5Aopen(obj, name, H5P_DEFAULT);
    if (a < 0) return std::string{};
    // H5Aread writes one element per dataspace point, so an array-valued
    // attribute (npoints > 1) would overflow the single-element buffers below
    // (and, in the variable-length case, leak every pointer past the first).
    // We only support a scalar string here; treat anything else as absent.
    hid_t sp = H5Aget_space(a);
    hssize_t npoints = (sp >= 0) ? H5Sget_simple_extent_npoints(sp) : -1;
    if (sp >= 0) H5Sclose(sp);
    if (npoints != 1) { H5Aclose(a); return std::string{}; }
    hid_t t = H5Aget_type(a);
    H5T_class_t cls = H5Tget_class(t);
    std::string out;
    if (cls == H5T_STRING) {
        if (H5Tis_variable_str(t)) {
            char* buf = nullptr;
            hid_t mt = H5Tcopy(H5T_C_S1);
            H5Tset_size(mt, H5T_VARIABLE);
            H5Tset_cset(mt, H5T_CSET_UTF8);
            if (H5Aread(a, mt, &buf) >= 0 && buf) {
                out = buf;
                H5free_memory(buf);
            }
            H5Tclose(mt);
        } else {
            size_t sz = H5Tget_size(t);
            std::string buf(sz, '\0');
            if (H5Aread(a, t, buf.data()) >= 0) {
                size_t n = strnlen(buf.data(), sz);
                out.assign(buf.data(), n);
            }
        }
    }
    H5Tclose(t);
    H5Aclose(a);
    return out;
}

// Read up to two int64s from a "shape"-style attribute (e.g. [n_rows, n_cols])
// into out[2]. H5Aread writes one value per dataspace point, so reading
// straight into a fixed two-slot buffer overflows the stack when a malformed
// or hostile file declares a shape attribute with more than two elements.
// Size the read buffer to the attribute's actual point count and copy back
// only the first two; leave out = {0, 0} for absent / empty / absurd shapes.
static void read_shape2(hid_t obj, const char* name, int64_t out[2]) {
    out[0] = 0; out[1] = 0;
    if (H5Aexists(obj, name) <= 0) return;
    hid_t a = H5Aopen(obj, name, H5P_DEFAULT);
    if (a < 0) return;
    hid_t sp = H5Aget_space(a);
    hssize_t n = (sp >= 0) ? H5Sget_simple_extent_npoints(sp) : -1;
    if (sp >= 0) H5Sclose(sp);
    // A real shape has a handful of dims; reject empty / negative / absurd
    // counts rather than allocate on an attacker-controlled length.
    if (n >= 1 && n <= 1024) {
        std::vector<int64_t> tmp((size_t)n, 0);
        if (H5Aread(a, H5T_NATIVE_INT64, tmp.data()) >= 0) {
            out[0] = tmp[0];
            if (n >= 2) out[1] = tmp[1];
        }
    }
    H5Aclose(a);
}

// Whether a child link exists directly under `parent`.
static bool link_exists(hid_t parent, const char* name) {
    return H5Lexists(parent, name, H5P_DEFAULT) > 0;
}

// Whether the named child is a group (rather than dataset / datatype).
static bool is_group(hid_t parent, const char* name) {
    VV_H5O_INFO_T info;
    if (VV_H5Oget_info_by_name(parent, name, &info, H5O_INFO_BASIC,
                              H5P_DEFAULT) < 0) return false;
    return info.type == H5O_TYPE_GROUP;
}

// Human-readable description of an HDF5 datatype.
static std::string dtype_to_string(hid_t t) {
    H5T_class_t cls = H5Tget_class(t);
    size_t size = H5Tget_size(t);
    switch (cls) {
        case H5T_INTEGER: {
            H5T_sign_t s = H5Tget_sign(t);
            return (s == H5T_SGN_NONE ? "uint" : "int") +
                   std::to_string(size * 8);
        }
        case H5T_FLOAT:    return "float" + std::to_string(size * 8);
        case H5T_STRING:   return H5Tis_variable_str(t) ? "string"
                                                          : "string[" +
                              std::to_string(size) + "]";
        case H5T_COMPOUND: return "compound";
        case H5T_ENUM:     return "enum";
        case H5T_REFERENCE:return "ref";
        case H5T_OPAQUE:   return "opaque";
        case H5T_BITFIELD: return "bitfield";
        default:           return "?";
    }
}

// Format a dimension list like "10000 × 2" for the hierarchy table.
static std::string shape_to_string(const std::vector<hsize_t>& dims) {
    std::string s;
    for (size_t i = 0; i < dims.size(); ++i) {
        if (i) s += " \xc3\x97 ";   // ×
        s += std::to_string(dims[i]);
    }
    return s;
}

// Read all immediate children of a group. Order: as returned by HDF5.
static std::vector<std::string> list_children(hid_t group) {
    std::vector<std::string> names;
    H5G_info_t info;
    if (H5Gget_info(group, &info) < 0) return names;
    for (hsize_t i = 0; i < info.nlinks; ++i) {
        ssize_t len = H5Lget_name_by_idx(group, ".", H5_INDEX_NAME,
                                           H5_ITER_INC, i, nullptr, 0,
                                           H5P_DEFAULT);
        if (len <= 0) continue;
        std::string nm((size_t)len, '\0');
        H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i,
                            nm.data(), len + 1, H5P_DEFAULT);
        names.push_back(std::move(nm));
    }
    return names;
}

// ── Generic-HDF5 hierarchy walker ───────────────────────────────────────────
//
// One row per object reachable from the root via H5Lvisit_by_name. Skips
// soft-link cycles. Builds an arrow::Table directly (string columns).

struct HierarchyRow {
    std::string path;
    std::string kind;        // "Group" | "Dataset"
    std::string shape;       // empty for groups
    std::string dtype;       // empty for groups
    int         n_attrs = 0;
};

struct HierarchyState {
    std::vector<HierarchyRow> rows;
    int max_depth = 1024;     // safety
};

static herr_t hierarchy_cb(hid_t loc_id, const char* name,
                            const VV_H5L_INFO_T* /*linfo*/, void* data) {
    auto* st = static_cast<HierarchyState*>(data);
    if ((int)st->rows.size() > 1000000) return -1;  // 1 M nodes hard cap
    VV_H5O_INFO_T info;
    if (VV_H5Oget_info_by_name(loc_id, name, &info, H5O_INFO_BASIC | H5O_INFO_NUM_ATTRS,
                              H5P_DEFAULT) < 0) return 0;
    HierarchyRow row;
    row.path = std::string("/") + name;
    row.n_attrs = (int)info.num_attrs;
    if (info.type == H5O_TYPE_GROUP) {
        row.kind = "Group";
    } else if (info.type == H5O_TYPE_DATASET) {
        row.kind = "Dataset";
        hid_t d = H5Dopen2(loc_id, name, H5P_DEFAULT);
        if (d >= 0) {
            hid_t s = H5Dget_space(d);
            int   nd = H5Sget_simple_extent_ndims(s);
            std::vector<hsize_t> dims((size_t)nd);
            if (nd > 0) H5Sget_simple_extent_dims(s, dims.data(), nullptr);
            row.shape = shape_to_string(dims);
            hid_t t = H5Dget_type(d);
            row.dtype = dtype_to_string(t);
            H5Tclose(t);
            H5Sclose(s);
            H5Dclose(d);
        }
    } else {
        row.kind = "Other";
    }
    st->rows.push_back(std::move(row));
    return 0;
}

static std::shared_ptr<arrow::Table>
build_hierarchy_table(hid_t file_id) {
    HierarchyState st;
    // Add the root.
    HierarchyRow root{"/", "Group", "", "", 0};
    VV_H5O_INFO_T rinfo;
    if (VV_H5Oget_info(file_id, &rinfo, H5O_INFO_BASIC | H5O_INFO_NUM_ATTRS) >= 0)
        root.n_attrs = (int)rinfo.num_attrs;
    st.rows.push_back(std::move(root));
    VV_H5Lvisit(file_id, H5_INDEX_NAME, H5_ITER_NATIVE, hierarchy_cb, &st);
    arrow::StringBuilder b_path, b_kind, b_shape, b_dtype;
    arrow::Int32Builder  b_attrs;
    for (const auto& r : st.rows) {
        (void)b_path.Append(r.path);
        (void)b_kind.Append(r.kind);
        (void)b_shape.Append(r.shape);
        (void)b_dtype.Append(r.dtype);
        (void)b_attrs.Append(r.n_attrs);
    }
    std::shared_ptr<arrow::Array> a_path, a_kind, a_shape, a_dtype, a_attrs;
    (void)b_path.Finish(&a_path);
    (void)b_kind.Finish(&a_kind);
    (void)b_shape.Finish(&a_shape);
    (void)b_dtype.Finish(&a_dtype);
    (void)b_attrs.Finish(&a_attrs);
    auto schema = arrow::schema({
        arrow::field("path",    arrow::utf8()),
        arrow::field("kind",    arrow::utf8()),
        arrow::field("shape",   arrow::utf8()),
        arrow::field("dtype",   arrow::utf8()),
        arrow::field("n_attrs", arrow::int32()),
    });
    return arrow::Table::Make(schema, {a_path, a_kind, a_shape, a_dtype, a_attrs});
}

// Render a small HDF5 dataset's value(s) as a display string for the uns tab:
// scalars and short 1-D arrays show their actual values (joined with ", "),
// anything larger / multi-dimensional shows a "<dtype>  <shape>" descriptor.
// Mirrors read_1d_dataset_table's type handling but also accepts 0-D (scalar)
// datasets — which uns is full of (a title string, an int n_pcs, a float
// threshold) and read_1d_dataset_table rejects.
static std::string h5_value_to_string(hid_t dset, int max_elems = 10) {
    hid_t space = H5Dget_space(dset);
    int nd = H5Sget_simple_extent_ndims(space);
    hssize_t np = H5Sget_simple_extent_npoints(space);
    std::vector<hsize_t> dims(nd > 0 ? (size_t)nd : 0);
    if (nd > 0) H5Sget_simple_extent_dims(space, dims.data(), nullptr);
    H5Sclose(space);
    hid_t t = H5Dget_type(dset);
    H5T_class_t cls = H5Tget_class(t);
    size_t tsz = H5Tget_size(t);
    auto descriptor = [&]() {
        std::string d = dtype_to_string(t);
        if (nd > 0) d += "  " + shape_to_string(dims);
        return d;
    };
    std::string out;
    if (np < 0 || np > max_elems || nd > 1) {
        out = descriptor();
    } else {
        size_t n = (size_t)np;
        if (cls == H5T_INTEGER) {
            std::vector<int64_t> buf(n);
            if (n) H5Dread(dset, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
            for (size_t i = 0; i < n; ++i) { if (i) out += ", "; out += std::to_string(buf[i]); }
        } else if (cls == H5T_FLOAT) {
            std::vector<double> buf(n);
            if (n) H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
            for (size_t i = 0; i < n; ++i) {
                if (i) out += ", ";
                char tmp[32]; std::snprintf(tmp, sizeof tmp, "%.6g", buf[i]); out += tmp;
            }
        } else if (cls == H5T_STRING && H5Tis_variable_str(t)) {
            std::vector<char*> ptrs(n, nullptr);
            hid_t mt = H5Tcopy(H5T_C_S1);
            H5Tset_size(mt, H5T_VARIABLE); H5Tset_cset(mt, H5T_CSET_UTF8);
            if (n) H5Dread(dset, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, ptrs.data());
            for (size_t i = 0; i < n; ++i) { if (i) out += ", "; out += ptrs[i] ? ptrs[i] : ""; }
            if (n) { hid_t ms = H5Dget_space(dset);
                     H5Dvlen_reclaim(mt, ms, H5P_DEFAULT, ptrs.data()); H5Sclose(ms); }
            H5Tclose(mt);
        } else if (cls == H5T_STRING) {
            std::vector<char> buf(n * tsz, '\0');
            if (n) H5Dread(dset, t, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
            for (size_t i = 0; i < n; ++i) {
                if (i) out += ", ";
                size_t len = strnlen(buf.data() + i * tsz, tsz);
                out.append(buf.data() + i * tsz, len);
            }
        } else {
            out = descriptor();
        }
        if (out.empty() && n == 0) out = "(empty)";
    }
    H5Tclose(t);
    return out;
}

// Walk an AnnData /uns group recursively, collecting (dotted-key, value) rows:
// scalars / short arrays show their values, plain nested dicts recurse, and an
// encoded sub-object (dataframe / categorical / sparse …) shows its
// encoding-type rather than being expanded. Bounded by depth + a row cap.
static void walk_uns(hid_t group, const std::string& prefix,
                     std::vector<std::pair<std::string, std::string>>* rows,
                     int depth) {
    if (depth > 16 || rows->size() >= 10000) return;
    for (const auto& name : list_children(group)) {
        std::string key = prefix.empty() ? name : prefix + "." + name;
        if (is_group(group, name.c_str())) {
            hid_t sub = H5Gopen2(group, name.c_str(), H5P_DEFAULT);
            if (sub < 0) continue;
            std::string enc = read_string_attr(sub, "encoding-type");
            if (enc.empty() || enc == "dict")
                walk_uns(sub, key, rows, depth + 1);    // recurse into plain dicts
            else
                rows->push_back({key, enc});            // dataframe / categorical / …
            H5Gclose(sub);
        } else {
            hid_t d = H5Dopen2(group, name.c_str(), H5P_DEFAULT);
            if (d < 0) continue;
            std::string v = h5_value_to_string(d);
            for (char& c : v) if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            rows->push_back({key, v});
            H5Dclose(d);
        }
    }
}

static std::shared_ptr<arrow::Table> build_uns_table(hid_t uns_group) {
    std::vector<std::pair<std::string, std::string>> rows;
    walk_uns(uns_group, "", &rows, 0);
    arrow::StringBuilder kb, vb;
    for (auto& kv : rows) { (void)kb.Append(kv.first); (void)vb.Append(kv.second); }
    std::shared_ptr<arrow::Array> ka, va;
    (void)kb.Finish(&ka); (void)vb.Finish(&va);
    auto sch = arrow::schema({arrow::field("key",   arrow::utf8()),
                              arrow::field("value", arrow::utf8())});
    return arrow::Table::Make(sch, {ka, va});
}

// ── Forward declarations ────────────────────────────────────────────────────
struct OpenSpec;
class Hdf5Source;
// Preview row caps. AnnData components can be enormous (a Tahoe-100M plate has
// ~4.7M-row obs and a 4.7M×62k X); reading one in full stalls for minutes on a
// slow mount and burns GBs of RAM. Cap every component to a head preview (the
// CSR X path already did); the real row count is still reported in the footer.
static constexpr int64_t kDense2DRowCap   = 1000;   // dense 2-D matrix rows
static constexpr int64_t kDense2DColCap   = 200;    // dense 2-D matrix cols
static constexpr int64_t kDataFrameRowCap = 1000;   // obs / var DataFrame rows
// A categorical column whose dictionary exceeds this is shown as integer codes
// rather than decoded to strings: a per-cell-unique categorical (e.g. a barcode
// column with millions of categories) would otherwise force reading the whole
// dictionary — minutes over a slow mount — just to render a preview. The default
// (1,000,000) comfortably covers real high-cardinality categoricals.
inline int64_t category_dict_cap() {
    static const int64_t cap = [] {
        if (const char* e = std::getenv("VV_CATEGORY_DICT_CAP")) {
            long long v = std::atoll(e);
            if (v > 0) return (int64_t)v;
        }
        return (int64_t)1000000;
    }();
    return cap;
}
// `row_cap` < 0 means "all rows"; otherwise only the first row_cap rows are
// read (HDF5 hyperslab). The full pre-cap length is reported via full_rows.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_anndata_dataframe(hid_t group, int64_t row_cap = -1,
                       int64_t* full_rows = nullptr);
static arrow::Result<std::shared_ptr<arrow::Table>>
read_2d_dataset_table(hid_t dataset, int64_t row_cap, int64_t col_cap = -1,
                      int64_t* full_rows = nullptr,
                      int64_t* full_cols = nullptr);
static arrow::Result<std::shared_ptr<arrow::Table>>
read_1d_dataset_table(hid_t dataset, int64_t row_cap = -1,
                      int64_t* full_rows = nullptr);
static arrow::Result<std::shared_ptr<arrow::Table>>
read_sparse_preview(hid_t group, int64_t row_cap);
// Label an AnnData X preview with its obs (row) / var (column) identifiers.
// Which AnnData axes a dense 2-D tab is indexed by. X and layers/* are
// (n_obs x n_var), but obsm/* is (n_obs x d) and varm/* is (n_var x d), where
// d is an embedding dimension — not a gene. Labelling all three the same way
// put gene names on UMAP coordinates.
enum class AnnMatrixAxes { ObsByVar, ObsByDim, VarByDim };

static void apply_anndata_matrix_labels(hid_t file_id, AnnMatrixAxes axes,
                                        const std::string& key,
                                        std::shared_ptr<arrow::Table>* tbl);

// Tab specification: identifies which named object inside the HDF5 file
// this tab should view, and how to render it.
struct OpenSpec {
    enum class Kind { Hierarchy, DataFrame, Matrix2D, Sparse, Dataset1D,
                      Dataset2D, Summary, Uns };
    Kind        kind;
    std::string h5_path;     // group / dataset path inside the file
    std::string display;     // tab label
    std::string footer_hint; // additional footer text
    // Matrix2D only: how to label the axes, and the obsm/varm key whose name
    // the dimension columns are derived from ("X_umap" -> X_umap1, X_umap2).
    AnnMatrixAxes axes = AnnMatrixAxes::ObsByVar;
    std::string   key;
};

// Read the AnnData layout and produce one OpenSpec per visible tab.
static std::vector<OpenSpec> scan_anndata(hid_t file_id);
// Same but for a generic HDF5 file (one spec per 1D/2D dataset).
static std::vector<OpenSpec> scan_generic(hid_t file_id);

// ── Hdf5Source: one tab's view of the file ──────────────────────────────────

class Hdf5Source : public WorkbookSource {
    H5FilePtr   file_;
    std::string h5_path_;
    OpenSpec    spec_;
    // Every sibling tab's OpenSpec, shared across the original + sibling
    // instances. The original holds the full list; siblings keep it
    // alive for symmetry / future drill-down.
    std::shared_ptr<std::vector<OpenSpec>> all_specs_;
    // Sibling specs *other* than this one — emitted via
    // open_sibling_sheets().
    std::vector<OpenSpec> siblings_;
    // Lazy materialization: a sibling tab's table is built only on first
    // access (ensure_built). The eager constructor sets built_ = true so its
    // overrides are no-ops; the lazy constructor leaves it false.
    mutable bool built_ = true;
    // Row cap for DataFrame (obs/var) tabs: the preview cap for the TUI/table
    // view, or -1 (all) / an explicit -n for a delimited dump — set from cfg in
    // open_source and inherited by sibling tabs. Matrix / sparse X ignore it.
    int64_t df_row_cap_ = kDataFrameRowCap;

    Hdf5Source(std::shared_ptr<arrow::Table> tbl,
                std::string path,
                std::string footer,
                H5FilePtr file,
                OpenSpec spec,
                std::shared_ptr<std::vector<OpenSpec>> all_specs,
                std::vector<OpenSpec> siblings,
                int64_t df_row_cap = kDataFrameRowCap)
        : WorkbookSource(std::move(tbl), std::move(path), std::move(footer)),
          file_(std::move(file)),
          h5_path_(spec.h5_path),
          spec_(std::move(spec)),
          all_specs_(std::move(all_specs)),
          siblings_(std::move(siblings)),
          df_row_cap_(df_row_cap) {}

    // Lazy constructor: no table yet. ensure_built() reads it from `spec` on
    // first access, so opening a multi-component file (e.g. AnnData) doesn't
    // materialise every component up-front — only the tab(s) actually viewed.
    Hdf5Source(std::string path,
                H5FilePtr file,
                OpenSpec spec,
                std::shared_ptr<std::vector<OpenSpec>> all_specs,
                int64_t df_row_cap = kDataFrameRowCap)
        : WorkbookSource(nullptr, std::move(path), std::string{}),
          file_(std::move(file)),
          h5_path_(spec.h5_path),
          spec_(std::move(spec)),
          all_specs_(std::move(all_specs)),
          built_(false),
          df_row_cap_(df_row_cap) {}

    // Build this tab's table from its spec on first access (lazy ctor only).
    void ensure_built() const {
        if (built_) return;
        built_ = true;
        auto* self = const_cast<Hdf5Source*>(this);
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        std::string err = build_table(*file_, spec_, &tbl, &footer, df_row_cap_);
        if (!err.empty() || !tbl) {
            // Surface the failure as a one-cell table instead of crashing a
            // null-table access (matches the eager path's graceful skip).
            std::string msg = err.empty()
                ? ("'" + spec_.h5_path + "': decoded to empty table") : err;
            arrow::StringBuilder b; (void)b.Append(msg);
            std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
            tbl = arrow::Table::Make(
                arrow::schema({arrow::field("error", arrow::utf8())}), {a});
            footer = "Format: HDF5 " + spec_.display + "  |  error: " + msg;
        }
        self->replace_table(std::move(tbl), std::move(footer));
    }

    // Resolve an OpenSpec to a populated arrow::Table.
    static std::string build_table(hid_t file_id,
                                    const OpenSpec& spec,
                                    std::shared_ptr<arrow::Table>* out,
                                    std::string* footer,
                                    int64_t df_row_cap = kDataFrameRowCap) {
        switch (spec.kind) {
            case OpenSpec::Kind::Hierarchy: {
                *out = build_hierarchy_table(file_id);
                *footer = "Format: HDF5 (hierarchy)";
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                return "";
            }
            case OpenSpec::Kind::Summary: {
                // The summary table is built directly by scan_anndata
                // and stuffed into spec.footer_hint as JSON-ish text;
                // we just turn it back into rows here.
                arrow::StringBuilder kb, vb;
                std::stringstream ss(spec.footer_hint);
                std::string line;
                while (std::getline(ss, line)) {
                    auto eq = line.find('\t');
                    if (eq == std::string::npos) continue;
                    (void)kb.Append(line.substr(0, eq));
                    (void)vb.Append(line.substr(eq + 1));
                }
                std::shared_ptr<arrow::Array> ka, va;
                (void)kb.Finish(&ka); (void)vb.Finish(&va);
                auto sch = arrow::schema({
                    arrow::field("key",   arrow::utf8()),
                    arrow::field("value", arrow::utf8())});
                *out = arrow::Table::Make(sch, {ka, va});
                *footer = "Format: AnnData (summary)";
                return "";
            }
            case OpenSpec::Kind::Uns: {
                hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (g < 0) return "Cannot open group " + spec.h5_path;
                *out = build_uns_table(g);
                H5Gclose(g);
                *footer = "Format: AnnData (uns)  |  " +
                          std::to_string(*out ? (*out)->num_rows() : 0) +
                          " entries";
                return "";
            }
            case OpenSpec::Kind::DataFrame: {
                hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (g < 0) return "Cannot open group " + spec.h5_path;
                int64_t full = 0;
                // df_row_cap is the preview cap for the TUI/table view, or -1
                // (all rows) / an explicit -n in delimited export — see
                // open_source. Only obs/var (DataFrame) is uncapped on export;
                // matrix / sparse X stay bounded below.
                auto r = read_anndata_dataframe(g, df_row_cap, &full);
                H5Gclose(g);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                int64_t shown = *out ? (*out)->num_rows() : 0;
                int64_t ncol  = *out ? (*out)->num_columns() : 0;
                *footer = "Format: AnnData " + spec.display +
                          "  |  Cols: " + std::to_string(ncol);
                if (shown < full)   // preview note so the cap isn't read as the real size
                    *footer += "  |  preview: first " + std::to_string(shown) +
                               " of " + std::to_string(full) + " rows";
                else
                    *footer += "  |  Rows: " + std::to_string(shown);
                return "";
            }
            case OpenSpec::Kind::Matrix2D:
            case OpenSpec::Kind::Dataset2D: {
                hid_t d = H5Dopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (d < 0) return "Cannot open dataset " + spec.h5_path;
                int64_t fr = 0, fc = 0;
                auto r = read_2d_dataset_table(d, kDense2DRowCap, kDense2DColCap,
                                               &fr, &fc);
                H5Dclose(d);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                *footer = "Format: HDF5 2D " + spec.display;
                // Note any preview truncation so the cap isn't mistaken for the
                // real shape.
                int64_t sr = *out ? (*out)->num_rows() : 0;
                int64_t sc = *out ? (*out)->num_columns() : 0;
                if (sr < fr || sc < fc) {
                    std::string note = "preview: ";
                    if (sr < fr)
                        note += "first " + std::to_string(sr) + " of " +
                                std::to_string(fr) + " rows";
                    if (sr < fr && sc < fc) note += ", ";
                    if (sc < fc)
                        note += "first " + std::to_string(sc) + " of " +
                                std::to_string(fc) + " cols";
                    *footer += "  |  " + note;
                }
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                // A dense AnnData matrix (Matrix2D, never generic Dataset2D)
                // gets obs/var identifiers, per its own axes.
                if (spec.kind == OpenSpec::Kind::Matrix2D)
                    apply_anndata_matrix_labels(file_id, spec.axes, spec.key, out);
                return "";
            }
            case OpenSpec::Kind::Dataset1D: {
                hid_t d = H5Dopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (d < 0) return "Cannot open dataset " + spec.h5_path;
                // Cap the read: a generic HDF5 1-D dataset can be millions of
                // elements (e.g. a per-read array), and reading it in full
                // stalls / OOMs. Preview the head; report the real length.
                int64_t full = 0;
                auto r = read_1d_dataset_table(d, kDense2DRowCap, &full);
                H5Dclose(d);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                *footer = "Format: HDF5 1D " + spec.display;
                int64_t shown = *out ? (*out)->num_rows() : 0;
                if (shown < full)   // preview note so the cap isn't read as the real size
                    *footer += "  |  preview: first " + std::to_string(shown) +
                               " of " + std::to_string(full) + " rows";
                else
                    *footer += "  |  Rows: " + std::to_string(shown);
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                return "";
            }
            case OpenSpec::Kind::Sparse: {
                hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (g < 0) return "Cannot open sparse group " + spec.h5_path;
                auto r = read_sparse_preview(g, 1000);
                H5Gclose(g);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                *footer = "Format: AnnData " + spec.display +
                          "  |  preview: first " +
                          std::to_string((*out)->num_rows()) + " rows";
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                // Sparse is always X, i.e. genuinely (n_obs x n_var): name
                // columns by var (genes), prepend obs (cells) row labels.
                apply_anndata_matrix_labels(file_id, AnnMatrixAxes::ObsByVar,
                                            spec.key, out);
                return "";
            }
        }
        return "Unknown OpenSpec kind";
    }

    static std::string build_one(const std::string& path,
                                  H5FilePtr file,
                                  OpenSpec spec,
                                  std::shared_ptr<std::vector<OpenSpec>> all,
                                  std::vector<OpenSpec> siblings,
                                  std::unique_ptr<Hdf5Source>* out,
                                  int64_t df_row_cap = kDataFrameRowCap) {
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        std::string err = build_table(*file, spec, &tbl, &footer, df_row_cap);
        if (!err.empty()) return err;
        if (!tbl)
            return "'" + spec.h5_path + "': decoded to empty table";
        if (!siblings.empty()) {
            footer += "  |  +" + std::to_string(siblings.size()) +
                       " more tab(s)";
        }
        out->reset(new Hdf5Source(std::move(tbl), path, std::move(footer),
                                    std::move(file), std::move(spec),
                                    std::move(all), std::move(siblings),
                                    df_row_cap));
        return "";
    }

public:
    static std::string open_first(const std::string& path,
                                    std::unique_ptr<Hdf5Source>* out,
                                    int64_t df_row_cap = kDataFrameRowCap);

    // tab_label() reads only the spec — no build, so the tab strip and the
    // --tab selector can list/match components without materialising them.
    std::string tab_label() const override { return spec_.display; }

    // Data accessors force the lazy build first; for an eagerly-built source
    // (built_ == true) ensure_built() is a no-op.
    std::shared_ptr<arrow::Schema> schema() const override {
        ensure_built(); return MemoryTableSource::schema();
    }
    int64_t total_rows() const override {
        ensure_built(); return MemoryTableSource::total_rows();
    }
    int num_chunks() const override {
        ensure_built(); return MemoryTableSource::num_chunks();
    }
    ChunkMeta chunk_meta(int i) const override {
        ensure_built(); return MemoryTableSource::chunk_meta(i);
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols,
                             std::shared_ptr<arrow::Table>* out) override {
        ensure_built(); return MemoryTableSource::read_chunk(i, cols, out);
    }
    std::string footer() const override {
        ensure_built(); return MemoryTableSource::footer();
    }
    std::vector<std::string> hidden_for_display() const override {
        ensure_built(); return MemoryTableSource::hidden_for_display();
    }

    std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> result;
        // Construct each sibling lazily: its table is read only when the tab is
        // first viewed (ensure_built), so opening a 14-component AnnData file
        // over a slow mount doesn't read every component up-front.
        for (const auto& sp : siblings_)
            result.push_back(std::unique_ptr<TabularSource>(
                new Hdf5Source(path(), file_, sp, all_specs_, df_row_cap_)));
        return result;
    }
};

// ── Read helpers — defined after Hdf5Source so they can be referenced
// from build_table. ─────────────────────────────────────────────────────────

// Read a 1-D dataset as a single-column Arrow table. Only the first `row_cap`
// elements are read (a hyperslab) when row_cap >= 0; the full length is
// reported via full_rows. Bounds the read for huge obs/var columns.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_1d_dataset_table(hid_t dset, int64_t row_cap, int64_t* full_rows) {
    hid_t space = H5Dget_space(dset);
    int nd = H5Sget_simple_extent_ndims(space);
    if (nd != 1) {
        H5Sclose(space);
        return arrow::Status::Invalid("expected 1-D dataset");
    }
    hsize_t dim;
    H5Sget_simple_extent_dims(space, &dim, nullptr);
    H5Sclose(space);
    if (full_rows) *full_rows = (int64_t)dim;
    hsize_t n = dim;
    if (row_cap >= 0 && (hsize_t)row_cap < dim) n = (hsize_t)row_cap;

    // Read the first `n` elements of `dset` into `buf` via a hyperslab; `ms`
    // (the matching memory dataspace) is returned so vlen strings can be
    // reclaimed against it.
    auto read_first_n = [&](hid_t memtype, void* buf) -> hid_t {
        hid_t fs = H5Dget_space(dset);
        hsize_t start = 0, count = n;
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
        hid_t ms = H5Screate_simple(1, &count, nullptr);
        if (n > 0) H5Dread(dset, memtype, ms, fs, H5P_DEFAULT, buf);
        H5Sclose(fs);
        return ms;   // caller closes
    };

    hid_t t = H5Dget_type(dset);
    H5T_class_t cls = H5Tget_class(t);
    size_t tsz = H5Tget_size(t);
    arrow::FieldVector fields = { arrow::field("value", arrow::utf8()) };
    std::shared_ptr<arrow::Array> arr;
    if (cls == H5T_INTEGER) {
        std::vector<int64_t> buf((size_t)n);
        hid_t ms = read_first_n(H5T_NATIVE_INT64, buf.data()); H5Sclose(ms);
        arrow::Int64Builder b;
        for (auto v : buf) (void)b.Append(v);
        (void)b.Finish(&arr);
        fields[0] = arrow::field("value", arrow::int64());
    } else if (cls == H5T_FLOAT) {
        std::vector<double> buf((size_t)n);
        hid_t ms = read_first_n(H5T_NATIVE_DOUBLE, buf.data()); H5Sclose(ms);
        arrow::DoubleBuilder b;
        for (auto v : buf) (void)b.Append(v);
        (void)b.Finish(&arr);
        fields[0] = arrow::field("value", arrow::float64());
    } else if (cls == H5T_STRING) {
        arrow::StringBuilder b;
        if (H5Tis_variable_str(t)) {
            std::vector<char*> ptrs((size_t)n, nullptr);
            hid_t mt = H5Tcopy(H5T_C_S1);
            H5Tset_size(mt, H5T_VARIABLE);
            H5Tset_cset(mt, H5T_CSET_UTF8);
            hid_t ms = read_first_n(mt, ptrs.data());
            for (auto* p : ptrs) (void)b.Append(p ? std::string(p) : std::string{});
            // Free the vlens read into the first-n buffer.
            if (n > 0) H5Dvlen_reclaim(mt, ms, H5P_DEFAULT, ptrs.data());
            H5Sclose(ms);
            H5Tclose(mt);
        } else {
            std::vector<char> buf((size_t)n * tsz, '\0');
            hid_t ms = read_first_n(t, buf.data()); H5Sclose(ms);
            for (hsize_t i = 0; i < n; ++i) {
                size_t len = strnlen(buf.data() + i * tsz, tsz);
                (void)b.Append(std::string(buf.data() + i * tsz, len));
            }
        }
        (void)b.Finish(&arr);
    } else if (cls == H5T_ENUM) {
        // Map enum codes to their member names — h5py stores a bool column as
        // an int enum {FALSE=0, TRUE=1}, which otherwise hit the "?" fallback.
        // Member values read into a zeroed int64 are correct for little-endian
        // base types <= 8 bytes (what numpy / AnnData produce).
        std::map<int64_t, std::string> names;
        int nmem = H5Tget_nmembers(t);
        for (int m = 0; m < nmem; ++m) {
            int64_t val = 0;
            H5Tget_member_value(t, (unsigned)m, &val);
            char* mn = H5Tget_member_name(t, (unsigned)m);
            if (mn) { names[val] = mn; H5free_memory(mn); }
        }
        std::vector<int64_t> buf((size_t)n);
        hid_t ms = read_first_n(H5T_NATIVE_INT64, buf.data()); H5Sclose(ms);
        arrow::StringBuilder b;
        for (auto v : buf) {
            auto it = names.find(v);
            (void)b.Append(it != names.end() ? it->second : std::to_string(v));
        }
        (void)b.Finish(&arr);
    } else {
        // Fallback: unsupported type (compound, opaque, …).
        arrow::StringBuilder b;
        for (hsize_t i = 0; i < n; ++i) (void)b.Append("?");
        (void)b.Finish(&arr);
    }
    H5Tclose(t);
    auto sch = arrow::schema(fields);
    return arrow::Table::Make(sch, {arr});
}

// Read a 2-D numeric dataset as an Arrow table. Columns are named col0,
// col1, … unless the dataset has a "column_names" attribute. row_cap / col_cap
// < 0 mean "all"; only the first row_cap rows and col_cap columns are read
// (the corner hyperslab), bounding memory. The full pre-cap dimensions are
// reported through full_rows / full_cols when those pointers are non-null.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_2d_dataset_table(hid_t dset, int64_t row_cap, int64_t col_cap,
                      int64_t* full_rows, int64_t* full_cols) {
    hid_t space = H5Dget_space(dset);
    int nd = H5Sget_simple_extent_ndims(space);
    if (nd != 2) {
        H5Sclose(space);
        return arrow::Status::Invalid("expected 2-D dataset");
    }
    hsize_t dims[2];
    H5Sget_simple_extent_dims(space, dims, nullptr);
    int64_t n_rows = (int64_t)dims[0];
    int64_t n_cols = (int64_t)dims[1];
    if (full_rows) *full_rows = n_rows;
    if (full_cols) *full_cols = n_cols;
    if (row_cap > 0 && row_cap < n_rows) n_rows = row_cap;
    if (col_cap > 0 && col_cap < n_cols) n_cols = col_cap;
    H5Sclose(space);

    hid_t t = H5Dget_type(dset);
    H5T_class_t cls = H5Tget_class(t);
    H5Tclose(t);

    // Build column names from the dataset's "column_names" attribute if
    // present (AnnData uses it for some embeddings).
    std::vector<std::string> names((size_t)n_cols);
    for (int64_t c = 0; c < n_cols; ++c) names[(size_t)c] = "col" + std::to_string(c);

    arrow::FieldVector fields((size_t)n_cols);
    std::vector<std::shared_ptr<arrow::Array>> cols((size_t)n_cols);
    if (cls == H5T_FLOAT) {
        std::vector<double> buf((size_t)(n_rows * n_cols));
        hid_t fs = H5Dget_space(dset);
        hsize_t start[2] = {0, 0};
        hsize_t count[2] = {(hsize_t)n_rows, (hsize_t)n_cols};
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr);
        hid_t ms = H5Screate_simple(2, count, nullptr);
        H5Dread(dset, H5T_NATIVE_DOUBLE, ms, fs, H5P_DEFAULT, buf.data());
        H5Sclose(ms); H5Sclose(fs);
        for (int64_t c = 0; c < n_cols; ++c) {
            arrow::DoubleBuilder b;
            for (int64_t r = 0; r < n_rows; ++r)
                (void)b.Append(buf[(size_t)(r * n_cols + c)]);
            (void)b.Finish(&cols[(size_t)c]);
            fields[(size_t)c] = arrow::field(names[(size_t)c], arrow::float64());
        }
    } else if (cls == H5T_INTEGER) {
        std::vector<int64_t> buf((size_t)(n_rows * n_cols));
        hid_t fs = H5Dget_space(dset);
        hsize_t start[2] = {0, 0};
        hsize_t count[2] = {(hsize_t)n_rows, (hsize_t)n_cols};
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr);
        hid_t ms = H5Screate_simple(2, count, nullptr);
        H5Dread(dset, H5T_NATIVE_INT64, ms, fs, H5P_DEFAULT, buf.data());
        H5Sclose(ms); H5Sclose(fs);
        for (int64_t c = 0; c < n_cols; ++c) {
            arrow::Int64Builder b;
            for (int64_t r = 0; r < n_rows; ++r)
                (void)b.Append(buf[(size_t)(r * n_cols + c)]);
            (void)b.Finish(&cols[(size_t)c]);
            fields[(size_t)c] = arrow::field(names[(size_t)c], arrow::int64());
        }
    } else {
        // String / compound 2-D: not supported in v1, emit empty.
        for (int64_t c = 0; c < n_cols; ++c) {
            arrow::StringBuilder b;
            (void)b.Finish(&cols[(size_t)c]);
            fields[(size_t)c] = arrow::field(names[(size_t)c], arrow::utf8());
        }
    }
    return arrow::Table::Make(arrow::schema(fields), cols);
}

// Is the mask true (= NA) at row i? The mask is an HDF5 bool, which vv decodes
// to its enum member name ("TRUE"/"FALSE") — so accept the string form as well
// as a genuine Arrow boolean.
static bool anndata_mask_bit(const arrow::Array& m, int64_t i) {
    if (i >= m.length() || m.IsNull(i)) return false;
    if (m.type_id() == arrow::Type::BOOL)
        return static_cast<const arrow::BooleanArray&>(m).Value(i);
    std::string s = cell_to_string(m, i);
    return s == "TRUE" || s == "True" || s == "true" || s == "1";
}

// Null out `values` where the `mask` is true (anndata's nullable encodings: mask
// bit set = NA). Returns `values` unchanged when there are no NAs (the common
// case) or the value array isn't a string array.
static std::shared_ptr<arrow::Array> anndata_apply_null_mask(
        const std::shared_ptr<arrow::Array>& values,
        const std::shared_ptr<arrow::Array>& mask) {
    if (!mask) return values;
    bool any = false;
    for (int64_t i = 0; i < mask->length(); ++i)
        if (anndata_mask_bit(*mask, i)) { any = true; break; }
    if (!any) return values;
    auto vs = std::dynamic_pointer_cast<arrow::StringArray>(values);
    if (!vs) return values;   // large_utf8 etc. — leave values, skip the mask
    arrow::StringBuilder b;
    for (int64_t i = 0; i < vs->length(); ++i) {
        if (anndata_mask_bit(*mask, i) || vs->IsNull(i)) (void)b.AppendNull();
        else (void)b.Append(vs->GetString(i));
    }
    std::shared_ptr<arrow::Array> out; (void)b.Finish(&out); return out;
}

// Decode an open `nullable-string-array` group (anndata >= 0.13's on-disk form
// for string columns / the DataFrame _index): a `values` string dataset plus a
// boolean `mask` for NA. Returns the resulting string array.
static arrow::Result<std::shared_ptr<arrow::Array>>
read_nullable_string_array(hid_t sub, int64_t cap, int64_t* full_rows) {
    if (!link_exists(sub, "values"))
        return arrow::Status::Invalid("nullable-string-array: no 'values'");
    hid_t vd = H5Dopen2(sub, "values", H5P_DEFAULT);
    if (vd < 0) return arrow::Status::Invalid("nullable-string-array: open 'values'");
    auto vt = read_1d_dataset_table(vd, cap, full_rows);
    H5Dclose(vd);
    if (!vt.ok() || (*vt)->num_columns() == 0)
        return arrow::Status::Invalid("nullable-string-array: empty 'values'");
    std::shared_ptr<arrow::Array> arr = (*vt)->column(0)->chunk(0);
    if (link_exists(sub, "mask")) {
        hid_t md = H5Dopen2(sub, "mask", H5P_DEFAULT);
        if (md >= 0) {
            auto mt = read_1d_dataset_table(md, cap, nullptr);
            H5Dclose(md);
            if (mt.ok() && (*mt)->num_columns() > 0)
                arr = anndata_apply_null_mask(arr, (*mt)->column(0)->chunk(0));
        }
    }
    return arr;
}

// Read AnnData's obs / var DataFrame layout — one column per non-special
// child link, categoricals expanded via the codes / categories sub-group.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_anndata_dataframe(hid_t group, int64_t row_cap, int64_t* full_rows) {
    auto names = list_children(group);
    // Identify the index column from the _index attribute if present.
    std::string idx_name = read_string_attr(group, "_index");
    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols;
    int64_t maxfull = 0;   // longest pre-cap child length (the real row count)

    // Helper to convert one child into a (name, array) pair.
    auto add_child = [&](const std::string& name) -> arrow::Status {
        // Skip private / reserved children.
        if (name == "__categories__") return arrow::Status::OK();
        std::string display = name;
        VV_H5O_INFO_T info;
        if (VV_H5Oget_info_by_name(group, name.c_str(), &info, H5O_INFO_BASIC,
                                   H5P_DEFAULT) < 0)
            return arrow::Status::OK();
        if (info.type == H5O_TYPE_GROUP) {
            // AnnData modern categorical: group with "categories" + "codes" datasets.
            hid_t sub = H5Gopen2(group, name.c_str(), H5P_DEFAULT);
            if (sub < 0) return arrow::Status::OK();
            std::string enc = read_string_attr(sub, "encoding-type");
            if (enc == "categorical" &&
                link_exists(sub, "categories") &&
                link_exists(sub, "codes")) {
                hid_t cats_d = H5Dopen2(sub, "categories", H5P_DEFAULT);
                hid_t codes_d = H5Dopen2(sub, "codes", H5P_DEFAULT);
                // `codes` is row-length (cap it); `categories` is the lookup
                // table — read in full only if it's small enough.
                int64_t cats_len = 0;
                { hid_t sp = H5Dget_space(cats_d);
                  hsize_t dd = 0;
                  if (H5Sget_simple_extent_ndims(sp) == 1)
                      H5Sget_simple_extent_dims(sp, &dd, nullptr);
                  H5Sclose(sp); cats_len = (int64_t)dd; }
                int64_t cf = 0;
                auto codes_t = read_1d_dataset_table(codes_d, row_cap, &cf);
                maxfull = std::max(maxfull, cf);

                if (cats_len > category_dict_cap()) {
                    // High-cardinality (e.g. per-cell barcodes): decoding would
                    // require reading the whole multi-million-entry dictionary.
                    // Show the integer codes instead for the preview.
                    H5Dclose(cats_d); H5Dclose(codes_d); H5Gclose(sub);
                    if (codes_t.ok() && (*codes_t)->num_columns() > 0) {
                        cols.push_back((*codes_t)->column(0)->chunk(0));
                        fields.push_back(arrow::field(
                            display + " (codes)",
                            (*codes_t)->schema()->field(0)->type()));
                    }
                    return arrow::Status::OK();
                }

                auto cats_t = read_1d_dataset_table(cats_d);
                H5Dclose(cats_d); H5Dclose(codes_d); H5Gclose(sub);
                if (cats_t.ok() && codes_t.ok()) {
                    auto cats_arr = (*cats_t)->column(0)->chunk(0);
                    auto codes_arr = (*codes_t)->column(0)->chunk(0);
                    auto cats_s = std::dynamic_pointer_cast<arrow::StringArray>(cats_arr);
                    auto codes_i = std::dynamic_pointer_cast<arrow::Int64Array>(codes_arr);
                    arrow::StringBuilder b;
                    int64_t n = codes_i ? codes_i->length() : 0;
                    int64_t nc = cats_s ? cats_s->length() : 0;
                    for (int64_t i = 0; i < n; ++i) {
                        int64_t code = codes_i->Value(i);
                        if (code < 0 || code >= nc)
                            (void)b.AppendNull();
                        else
                            (void)b.Append(cats_s->GetString(code));
                    }
                    std::shared_ptr<arrow::Array> a;
                    (void)b.Finish(&a);
                    cols.push_back(a);
                    fields.push_back(arrow::field(display, arrow::utf8()));
                }
                return arrow::Status::OK();
            }
            if (enc == "nullable-string-array") {
                // anndata >= 0.13: a string column is a {values, mask} group.
                int64_t cf = 0;
                auto a = read_nullable_string_array(sub, row_cap, &cf);
                H5Gclose(sub);
                maxfull = std::max(maxfull, cf);
                if (a.ok()) {
                    cols.push_back(*a);
                    fields.push_back(arrow::field(display, (*a)->type()));
                }
                return arrow::Status::OK();
            }
            H5Gclose(sub);
            return arrow::Status::OK();
        }
        if (info.type != H5O_TYPE_DATASET) return arrow::Status::OK();
        hid_t d = H5Dopen2(group, name.c_str(), H5P_DEFAULT);
        if (d < 0) return arrow::Status::OK();
        int64_t cf = 0;
        auto t = read_1d_dataset_table(d, row_cap, &cf);
        maxfull = std::max(maxfull, cf);
        H5Dclose(d);
        if (t.ok() && (*t)->num_columns() > 0) {
            cols.push_back((*t)->column(0)->chunk(0));
            fields.push_back(arrow::field(display, (*t)->schema()->field(0)->type()));
        }
        return arrow::Status::OK();
    };

    // Emit the _index column first if it exists, then the rest in
    // discovered order.
    if (!idx_name.empty()) {
        auto it = std::find(names.begin(), names.end(), idx_name);
        if (it != names.end()) {
            ARROW_RETURN_NOT_OK(add_child(*it));
            names.erase(it);
        }
    }
    for (const auto& n : names)
        ARROW_RETURN_NOT_OK(add_child(n));

    if (full_rows) *full_rows = maxfull;

    // Every column of a DataFrame must share the same length. A malformed
    // AnnData (children of differing sizes, or a categorical whose codes are a
    // different length than its siblings) would otherwise produce an invalid
    // Arrow table and out-of-bounds reads when the TUI pages a row that exists
    // in one column but not another. Normalise to the longest column: pad
    // short ones with trailing nulls, slice over-long ones.
    int64_t target = 0;
    for (const auto& a : cols) target = std::max(target, a->length());
    for (auto& a : cols) {
        if (a->length() == target) continue;
        if (a->length() > target) { a = a->Slice(0, target); continue; }
        auto nulls = arrow::MakeArrayOfNull(a->type(), target - a->length());
        if (!nulls.ok()) continue;
        auto cat = arrow::Concatenate({a, *nulls});
        if (cat.ok()) a = *cat;
    }
    return arrow::Table::Make(arrow::schema(fields), cols, target);
}

// Number of elements in a 1-D HDF5 dataset (0 if not rank-1 / on error).
static int64_t h5_len_1d(hid_t d) {
    hid_t sp = H5Dget_space(d);
    if (sp < 0) return 0;
    hsize_t dd = 0;
    if (H5Sget_simple_extent_ndims(sp) == 1)
        H5Sget_simple_extent_dims(sp, &dd, nullptr);
    H5Sclose(sp);
    return (int64_t)dd;
}

// Densify the first `row_cap` rows (and first 200 columns) of an AnnData sparse
// matrix into an Arrow table (one float64 column per matrix column). Handles
// both CSR (indptr per row, indices are columns) and CSC (indptr per column,
// indices are rows) — the output is rows × columns either way.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_sparse_preview(hid_t group, int64_t row_cap) {
    // shape attribute = [n_rows, n_cols]
    int64_t shape[2] = {0, 0};
    read_shape2(group, "shape", shape);
    std::string enc = read_string_attr(group, "encoding-type");
    bool is_csr = (enc == "csr_matrix");
    bool is_csc = (enc == "csc_matrix");
    if (!is_csr && !is_csc)
        return arrow::Status::Invalid("not a CSR/CSC sparse group");

    int64_t n_rows = shape[0], n_cols = shape[1];
    if (n_rows < 0) n_rows = 0;          // shape attribute is untrusted
    if (n_cols < 0) n_cols = 0;
    if (row_cap > 0 && row_cap < n_rows) n_rows = row_cap;
    if (n_cols > 200) n_cols = 200;     // wide-table sanity cap

    hid_t indptr_d = H5Dopen2(group, "indptr", H5P_DEFAULT);
    hid_t indices_d = H5Dopen2(group, "indices", H5P_DEFAULT);
    hid_t data_d    = H5Dopen2(group, "data",    H5P_DEFAULT);
    if (indptr_d < 0 || indices_d < 0 || data_d < 0) {
        if (indptr_d >= 0) H5Dclose(indptr_d);
        if (indices_d >= 0) H5Dclose(indices_d);
        if (data_d >= 0) H5Dclose(data_d);
        return arrow::Status::IOError("sparse: missing indptr/indices/data");
    }

    // The compressed (indptr) axis is rows for CSR and columns for CSC. The
    // 'shape' attribute is untrusted: indptr has exactly (compressed_len + 1)
    // entries, so clamp that axis to what indptr actually holds — otherwise the
    // hyperslab below reads past the dataset extent.
    int64_t indptr_len = h5_len_1d(indptr_d);
    if (indptr_len < 1) {
        H5Dclose(indptr_d); H5Dclose(indices_d); H5Dclose(data_d);
        return arrow::Status::Invalid("sparse: empty/!1-D indptr");
    }
    int64_t& n_major = is_csr ? n_rows : n_cols;   // axis the indptr indexes
    if (n_major + 1 > indptr_len) n_major = indptr_len - 1;
    const int64_t nnz_avail = std::min(h5_len_1d(indices_d), h5_len_1d(data_d));

    // Read indptr[0 .. n_major].
    std::vector<int64_t> indptr((size_t)(n_major + 1));
    {
        hid_t fs = H5Dget_space(indptr_d);
        hsize_t start = 0, count = (hsize_t)(n_major + 1);
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
        hid_t ms = H5Screate_simple(1, &count, nullptr);
        H5Dread(indptr_d, H5T_NATIVE_INT64, ms, fs, H5P_DEFAULT, indptr.data());
        H5Sclose(ms); H5Sclose(fs);
    }
    // indptr values are untrusted too: the read window [front, back) into
    // indices/data must stay inside their actual extent, or the hyperslab
    // reads out of bounds.
    int64_t front = indptr.front();
    if (front < 0) front = 0;
    if (front > nnz_avail) front = nnz_avail;
    int64_t nnz_span = indptr.back() - front;
    if (nnz_span < 0) nnz_span = 0;
    if (nnz_span > nnz_avail - front) nnz_span = nnz_avail - front;

    std::vector<int64_t> indices((size_t)nnz_span);
    std::vector<double>  data((size_t)nnz_span);
    if (nnz_span > 0) {
        hsize_t start = (hsize_t)front;
        hsize_t count = (hsize_t)nnz_span;
        {
            hid_t fs = H5Dget_space(indices_d);
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
            hid_t ms = H5Screate_simple(1, &count, nullptr);
            H5Dread(indices_d, H5T_NATIVE_INT64, ms, fs, H5P_DEFAULT, indices.data());
            H5Sclose(ms); H5Sclose(fs);
        }
        {
            hid_t fs = H5Dget_space(data_d);
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
            hid_t ms = H5Screate_simple(1, &count, nullptr);
            H5Dread(data_d, H5T_NATIVE_DOUBLE, ms, fs, H5P_DEFAULT, data.data());
            H5Sclose(ms); H5Sclose(fs);
        }
    }
    H5Dclose(indptr_d); H5Dclose(indices_d); H5Dclose(data_d);

    // Densify into a column-major buffer (always rows × cols). Walk each
    // compressed-axis slice and scatter its values: for CSR `m` is a row and
    // the index is a column; for CSC `m` is a column and the index is a row.
    std::vector<std::vector<double>> colbuf((size_t)n_cols,
                                            std::vector<double>((size_t)n_rows, 0.0));
    for (int64_t m = 0; m < n_major; ++m) {
        // Offsets are relative to the clamped read window `front`, and bounded
        // to [0, nnz_span] so a non-monotonic / corrupt indptr can't index the
        // indices/data vectors out of range.
        int64_t s = indptr[(size_t)m] - front;
        int64_t e = indptr[(size_t)(m + 1)] - front;
        if (s < 0) s = 0;             if (s > nnz_span) s = nnz_span;
        if (e < s) e = s;             if (e > nnz_span) e = nnz_span;
        for (int64_t k = s; k < e; ++k) {
            int64_t idx = indices[(size_t)k];        // minor-axis index
            if (is_csr) {                            // m = row, idx = column
                if (idx >= 0 && idx < n_cols)
                    colbuf[(size_t)idx][(size_t)m] = data[(size_t)k];
            } else {                                 // m = column, idx = row
                if (idx >= 0 && idx < n_rows)
                    colbuf[(size_t)m][(size_t)idx] = data[(size_t)k];
            }
        }
    }
    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols;
    for (int64_t c = 0; c < n_cols; ++c) {
        arrow::DoubleBuilder b;
        (void)b.AppendValues(colbuf[(size_t)c]);
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        cols.push_back(std::move(a));
        fields.push_back(arrow::field("col" + std::to_string(c), arrow::float64()));
    }
    return arrow::Table::Make(arrow::schema(fields), cols, n_rows);
}

// Read up to `cap` values of an AnnData component group's index dataset (the
// dataset named by the group's `_index` attribute, e.g. obs/var cell & gene
// identifiers) as display strings. `*index_name` receives that dataset's name
// for use as a column header. Leaves the outputs empty on any problem.
static void read_anndata_index_labels(hid_t file_id, const char* group_path,
                                      int64_t cap,
                                      std::vector<std::string>* out,
                                      std::string* index_name) {
    out->clear();
    if (index_name) index_name->clear();
    hid_t g = H5Gopen2(file_id, group_path, H5P_DEFAULT);
    if (g < 0) return;
    std::string idx = read_string_attr(g, "_index");
    if (idx.empty()) idx = "_index";           // anndata's conventional default
    if (!link_exists(g, idx.c_str())) { H5Gclose(g); return; }
    // _index is a string dataset (legacy) or a nullable-string-array group
    // (anndata >= 0.13).
    std::shared_ptr<arrow::Array> col;
    VV_H5O_INFO_T info;
    if (VV_H5Oget_info_by_name(g, idx.c_str(), &info, H5O_INFO_BASIC, H5P_DEFAULT) >= 0
        && info.type == H5O_TYPE_GROUP) {
        hid_t sub = H5Gopen2(g, idx.c_str(), H5P_DEFAULT);
        if (sub >= 0) {
            auto a = read_nullable_string_array(sub, cap, nullptr);
            H5Gclose(sub);
            if (a.ok()) col = *a;
        }
    } else {
        hid_t d = H5Dopen2(g, idx.c_str(), H5P_DEFAULT);
        if (d >= 0) {
            auto t = read_1d_dataset_table(d, cap);
            H5Dclose(d);
            if (t.ok() && (*t)->num_columns() > 0) col = (*t)->column(0)->chunk(0);
        }
    }
    H5Gclose(g);
    if (!col) return;
    if (index_name) *index_name = idx;
    for (int64_t i = 0; i < col->length(); ++i)
        out->push_back(cell_to_string(*col, i));
}

// Label a dense AnnData 2-D preview with its obs / var identifiers.
//
// The three shapes are NOT interchangeable, and treating them as one put gene
// names on UMAP coordinates:
//
//   X, layers/*  (n_obs x n_var)  rows <- obs (cells),  cols <- var (genes)
//   obsm/*       (n_obs x d)      rows <- obs (cells),  cols <- dimensions
//   varm/*       (n_var x d)      rows <- var (genes),  cols <- dimensions
//
// `d` is an embedding width (2 for a UMAP, 50 for a PCA), unrelated to the
// number of genes — so the columns are named after the key that holds them,
// 1-based: X_umap -> X_umap1, X_umap2.
//
// No-op for a non-AnnData file (no obs/var groups -> empty labels).
static void apply_anndata_matrix_labels(hid_t file_id, AnnMatrixAxes axes,
                                        const std::string& key,
                                        std::shared_ptr<arrow::Table>* tbl) {
    if (!tbl || !*tbl) return;
    auto t = *tbl;
    const int64_t ncols = t->num_columns();
    const int64_t nrows = t->num_rows();

    // ── Columns ─────────────────────────────────────────────────────────────
    std::vector<std::string> col_names;
    if (axes == AnnMatrixAxes::ObsByVar) {
        // Gene identifiers, from the var index.
        std::string var_idx_name;
        read_anndata_index_labels(file_id, "/var", ncols, &col_names,
                                  &var_idx_name);
    } else {
        // Embedding dimensions. Derived from the key so the header says where
        // the numbers came from; falls back to the existing generated names if
        // the key is somehow empty.
        if (!key.empty())
            for (int64_t i = 0; i < ncols; ++i)
                col_names.push_back(key + std::to_string(i + 1));
    }
    if (!col_names.empty()) {
        std::vector<std::string> names;
        names.reserve((size_t)ncols);
        for (int64_t i = 0; i < ncols; ++i)
            names.push_back(i < (int64_t)col_names.size()
                                ? col_names[(size_t)i]
                                : t->field((int)i)->name());
        auto r = t->RenameColumns(names);
        if (r.ok()) t = *r;
    }

    // ── Rows: prepended as a leading label column ───────────────────────────
    // varm is indexed by gene, everything else by cell.
    const char* row_group = (axes == AnnMatrixAxes::VarByDim) ? "/var" : "/obs";
    const char* row_default = (axes == AnnMatrixAxes::VarByDim) ? "var" : "obs";
    std::vector<std::string> row_labels;
    std::string row_idx_name;
    read_anndata_index_labels(file_id, row_group, nrows, &row_labels,
                              &row_idx_name);
    if (!row_labels.empty()) {
        arrow::StringBuilder b;
        for (int64_t i = 0; i < nrows; ++i) {
            if (i < (int64_t)row_labels.size()) (void)b.Append(row_labels[(size_t)i]);
            else                                (void)b.AppendNull();
        }
        std::shared_ptr<arrow::Array> a;
        if (b.Finish(&a).ok()) {
            std::string header = row_idx_name.empty() || row_idx_name == "_index"
                                     ? row_default : row_idx_name;
            auto col = std::make_shared<arrow::ChunkedArray>(a);
            auto r = t->AddColumn(0, arrow::field(header, arrow::utf8()), col);
            if (r.ok()) t = *r;
        }
    }
    *tbl = t;
}

// ── Scanners — decide which tabs to emit ────────────────────────────────────

static std::vector<OpenSpec> scan_generic(hid_t file_id) {
    std::vector<OpenSpec> specs;
    // Tab 0 = hierarchy.
    specs.push_back({OpenSpec::Kind::Hierarchy, "/", "hierarchy", ""});
    // For each 1-D or 2-D dataset, add a tab.
    struct Scan { std::vector<OpenSpec>* out; int n_dsets = 0; };
    Scan ctx{&specs, 0};
    auto cb = [](hid_t loc_id, const char* name, const VV_H5L_INFO_T*, void* data) -> herr_t {
        auto* sc = static_cast<Scan*>(data);
        if (sc->n_dsets > 32) return 0;          // cap to keep tab count sane
        VV_H5O_INFO_T info;
        if (VV_H5Oget_info_by_name(loc_id, name, &info, H5O_INFO_BASIC, H5P_DEFAULT) < 0)
            return 0;
        if (info.type != H5O_TYPE_DATASET) return 0;
        hid_t d = H5Dopen2(loc_id, name, H5P_DEFAULT);
        if (d < 0) return 0;
        hid_t s = H5Dget_space(d);
        int nd = H5Sget_simple_extent_ndims(s);
        std::vector<hsize_t> dims((size_t)nd);
        if (nd > 0) H5Sget_simple_extent_dims(s, dims.data(), nullptr);
        H5Sclose(s); H5Dclose(d);
        if (nd == 1) {
            sc->out->push_back({OpenSpec::Kind::Dataset1D,
                                  std::string("/") + name,
                                  std::string("/") + name,
                                  shape_to_string(dims)});
            ++sc->n_dsets;
        } else if (nd == 2 && dims[1] <= 32) {
            sc->out->push_back({OpenSpec::Kind::Dataset2D,
                                  std::string("/") + name,
                                  std::string("/") + name,
                                  shape_to_string(dims)});
            ++sc->n_dsets;
        }
        return 0;
    };
    VV_H5Lvisit(file_id, H5_INDEX_NAME, H5_ITER_NATIVE,
               (VV_H5L_ITERATE_T)cb, &ctx);
    return specs;
}

static std::vector<OpenSpec> scan_anndata(hid_t file_id) {
    std::vector<OpenSpec> specs;

    // Summary tab (key/value rows).
    std::string summary;
    auto add = [&](const std::string& k, const std::string& v) {
        summary += k; summary += '\t'; summary += v; summary += '\n';
    };
    add("format", "AnnData");
    std::string enc = read_string_attr(file_id, "encoding-type");
    if (!enc.empty()) add("root-encoding", enc);

    // X (matrix). Either a dataset (dense) or a group with
    // encoding-type ∈ {csr_matrix, csc_matrix}.
    bool x_is_sparse = false;
    int64_t x_rows = 0, x_cols = 0;
    if (link_exists(file_id, "X")) {
        if (is_group(file_id, "X")) {
            hid_t g = H5Gopen2(file_id, "X", H5P_DEFAULT);
            std::string xenc = read_string_attr(g, "encoding-type");
            if (xenc == "csr_matrix" || xenc == "csc_matrix") {
                x_is_sparse = true;
                int64_t shape[2] = {0, 0};
                read_shape2(g, "shape", shape);
                x_rows = shape[0]; x_cols = shape[1];
                add("X", xenc + "  (" + std::to_string(x_rows) +
                          " \xc3\x97 " + std::to_string(x_cols) + ")");
                // Both CSR and CSC densify to the same rows × columns preview.
                specs.push_back({OpenSpec::Kind::Sparse, "/X",
                                  "X (preview)",
                                  xenc + "  shape: " +
                                  std::to_string(x_rows) + " \xc3\x97 " +
                                  std::to_string(x_cols)});
            }
            H5Gclose(g);
        } else {
            hid_t d = H5Dopen2(file_id, "X", H5P_DEFAULT);
            hid_t s = H5Dget_space(d);
            int nd = H5Sget_simple_extent_ndims(s);
            std::vector<hsize_t> dims((size_t)nd);
            if (nd > 0) H5Sget_simple_extent_dims(s, dims.data(), nullptr);
            H5Sclose(s); H5Dclose(d);
            if (nd == 2) {
                x_rows = (int64_t)dims[0]; x_cols = (int64_t)dims[1];
                add("X", "dense  (" + std::to_string(x_rows) +
                          " \xc3\x97 " + std::to_string(x_cols) + ")");
                specs.push_back({OpenSpec::Kind::Matrix2D, "/X",
                                  "X", "dense"});
            }
        }
    }

    if (link_exists(file_id, "obs") && is_group(file_id, "obs")) {
        hid_t g = H5Gopen2(file_id, "obs", H5P_DEFAULT);
        H5G_info_t gi; H5Gget_info(g, &gi);
        add("obs", std::to_string(x_rows) + " rows, " +
                    std::to_string(gi.nlinks) + " columns");
        H5Gclose(g);
        specs.push_back({OpenSpec::Kind::DataFrame, "/obs", "obs", ""});
    }
    if (link_exists(file_id, "var") && is_group(file_id, "var")) {
        hid_t g = H5Gopen2(file_id, "var", H5P_DEFAULT);
        H5G_info_t gi; H5Gget_info(g, &gi);
        add("var", std::to_string(x_cols) + " rows, " +
                    std::to_string(gi.nlinks) + " columns");
        H5Gclose(g);
        specs.push_back({OpenSpec::Kind::DataFrame, "/var", "var", ""});
    }

    // obsm / varm / layers — each child becomes its own tab.
    auto add_subgroup_tabs = [&](const char* parent_name,
                                   OpenSpec::Kind k,
                                   const char* footer_kind,
                                   AnnMatrixAxes axes) {
        if (!link_exists(file_id, parent_name) ||
            !is_group(file_id, parent_name)) return;
        hid_t g = H5Gopen2(file_id, parent_name, H5P_DEFAULT);
        auto names = list_children(g);
        H5Gclose(g);
        for (const auto& nm : names) {
            specs.push_back({k,
                              std::string("/") + parent_name + "/" + nm,
                              std::string(parent_name) + "[" + nm + "]",
                              footer_kind, axes, nm});
        }
        if (!names.empty())
            add(parent_name, std::to_string(names.size()) + " entries");
    };
    // layers/* mirror X's shape, so they keep gene columns; obsm/varm do not.
    add_subgroup_tabs("obsm",   OpenSpec::Kind::Matrix2D, "obsm",
                      AnnMatrixAxes::ObsByDim);
    add_subgroup_tabs("varm",   OpenSpec::Kind::Matrix2D, "varm",
                      AnnMatrixAxes::VarByDim);
    add_subgroup_tabs("layers", OpenSpec::Kind::Matrix2D, "layer",
                      AnnMatrixAxes::ObsByVar);

    // uns (unstructured): one key/value tab surfacing scalars, strings and
    // small arrays (nested dicts flattened with dotted keys). Previously skipped.
    if (link_exists(file_id, "uns") && is_group(file_id, "uns")) {
        hid_t g = H5Gopen2(file_id, "uns", H5P_DEFAULT);
        auto names = list_children(g);
        H5Gclose(g);
        if (!names.empty()) {
            specs.push_back({OpenSpec::Kind::Uns, "/uns", "uns", ""});
            add("uns", std::to_string(names.size()) + " entries");
        }
    }

    // Prepend the summary tab.
    OpenSpec sum_spec{OpenSpec::Kind::Summary, "/", "summary", summary};
    specs.insert(specs.begin(), sum_spec);
    return specs;
}

// ── Hdf5Source::open_first ──────────────────────────────────────────────────

std::string Hdf5Source::open_first(const std::string& path,
                                      std::unique_ptr<Hdf5Source>* out,
                                      int64_t df_row_cap) {
    // Silence HDF5's stderr error spew for missing attrs etc.
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    hid_t fid = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fid < 0) return "Cannot open '" + path + "' as HDF5";
    H5FilePtr file(new hid_t(fid), [](hid_t* p){
        if (*p >= 0) H5Fclose(*p); delete p;
    });

    // AnnData detection: root attribute encoding-type == "anndata"
    // OR the modern heuristic /obs + /var + X presence.
    std::string root_enc = read_string_attr(fid, "encoding-type");
    bool is_anndata = (root_enc == "anndata") ||
        (link_exists(fid, "obs") && link_exists(fid, "var") &&
         link_exists(fid, "X"));

    // Refuse the legacy (pre-anndata-0.7) layout up-front. Signature:
    // root has no encoding-type, and obs/var are compound *datasets*
    // instead of groups-of-columns. Decoding compound types + the
    // legacy h5sparse X format is more work than it's worth without
    // a real demand signal, and silently rendering an empty summary
    // is worse than refusing.
    if (is_anndata && root_enc != "anndata" &&
        link_exists(fid, "obs") && !is_group(fid, "obs")) {
        return "'" + path + "': legacy AnnData layout (pre-0.7) is not "
               "supported. Re-save with a recent anndata: "
               "`python -c \"import anndata; "
               "anndata.read_h5ad('" + path + "')"
               ".write_h5ad('out.h5ad')\"`";
    }

    std::vector<OpenSpec> specs = is_anndata ? scan_anndata(fid)
                                                : scan_generic(fid);
    if (specs.empty()) return "'" + path + "': no viewable HDF5 datasets";

    auto all = std::make_shared<std::vector<OpenSpec>>(specs);
    OpenSpec first = specs.front();
    std::vector<OpenSpec> siblings(specs.begin() + 1, specs.end());
    return build_one(path, std::move(file), std::move(first),
                       std::move(all), std::move(siblings), out, df_row_cap);
}

}  // namespace h5v

// ── NumPy .npz viewer ────────────────────────────────────────────────────────
//
// .npz is a ZIP archive of .npy files. Each .npy carries its own header
// (shape + dtype + fortran-order flag) followed by raw little-endian
// binary data. We mirror the HDF5 multi-tab pattern: one summary tab
// listing every array, then one tab per displayable array. 1-D arrays
// render as a single column; 2-D as a full table; 3-D+ as a 2-D slice
// along the leading axis (the user can step the slice index with
// `[` / `]` or jump to a specific slice with `:slice N`). Object
// (pickled) arrays show up in the summary but don't get a tab — they'd
// require a pickle decoder we don't have.

namespace npz {

struct NpyHeader {
    std::vector<int64_t> shape;
    arrow::Type::type    dtype_id = arrow::Type::NA;
    bool                 fortran_order = false;
    bool                 unsupported = false;  // object/string/structured
    std::string          dtype_str;            // raw "<f4" / "|O" etc.
    size_t               data_offset = 0;
    size_t               item_size = 0;        // bytes per element (0 = unknown)
};

// Map numpy dtype letter codes onto Arrow types. Endianness must be
// little-endian or native (we'd need to byteswap for big-endian, which
// scientific Python virtually never produces).
static arrow::Type::type npy_letter_to_arrow(char endian, char kind, int size) {
    if (endian == '>') return arrow::Type::NA;       // big-endian: skip
    // Kind:  i=int, u=uint, f=float, b=bool, O=object, U=unicode, S=bytes
    if (kind == 'b') return arrow::Type::BOOL;
    if (kind == 'i') {
        if (size == 1) return arrow::Type::INT8;
        if (size == 2) return arrow::Type::INT16;
        if (size == 4) return arrow::Type::INT32;
        if (size == 8) return arrow::Type::INT64;
    }
    if (kind == 'u') {
        if (size == 1) return arrow::Type::UINT8;
        if (size == 2) return arrow::Type::UINT16;
        if (size == 4) return arrow::Type::UINT32;
        if (size == 8) return arrow::Type::UINT64;
    }
    if (kind == 'f') {
        if (size == 4) return arrow::Type::FLOAT;
        if (size == 8) return arrow::Type::DOUBLE;
    }
    return arrow::Type::NA;
}

// Parse the small ASCII header dict (a Python literal). We don't need a
// real Python parser — find the values for the three keys we care about.
// Tolerate single/double quotes, optional whitespace.
static std::string find_dict_value(const std::string& hdr, const std::string& key) {
    // Look for '<key>': ...  (quoted)
    for (char q : {'\'', '"'}) {
        std::string needle = std::string(1, q) + key + q;
        size_t p = hdr.find(needle);
        if (p == std::string::npos) continue;
        p = hdr.find(':', p + needle.size());
        if (p == std::string::npos) continue;
        ++p;
        while (p < hdr.size() && std::isspace((unsigned char)hdr[p])) ++p;
        // Capture until the next comma at depth 0 (track parens/brackets).
        int depth = 0;
        size_t start = p;
        while (p < hdr.size()) {
            char c = hdr[p];
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') --depth;
            else if (c == ',' && depth == 0) break;
            else if (c == '}' && depth == 0) break;
            ++p;
        }
        std::string v = hdr.substr(start, p - start);
        while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
        return v;
    }
    return "";
}

// Bytes per element that the readers (slab_to_arrow / make_column) actually read
// for each supported Arrow type — numpy bool is 1 byte, not Arrow's 1 bit. 0 for
// unsupported ids. Used to bound-check against what is really read, not the
// header's (attacker-controlled) declared item size.
static size_t npy_element_bytes(arrow::Type::type id) {
    switch (id) {
        case arrow::Type::BOOL:
        case arrow::Type::INT8:  case arrow::Type::UINT8:                       return 1;
        case arrow::Type::INT16: case arrow::Type::UINT16:                      return 2;
        case arrow::Type::INT32: case arrow::Type::UINT32: case arrow::Type::FLOAT:  return 4;
        case arrow::Type::INT64: case arrow::Type::UINT64: case arrow::Type::DOUBLE: return 8;
        default: return 0;
    }
}

static std::string parse_npy_header(const uint8_t* buf, size_t n, NpyHeader* out) {
    if (n < 10) return "npy: too short";
    if (std::memcmp(buf, "\x93NUMPY", 6) != 0) return "npy: bad magic";
    uint8_t major = buf[6], minor = buf[7]; (void)minor;
    size_t hdr_len, hdr_start;
    if (major == 1) {
        uint16_t l = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
        hdr_len = l; hdr_start = 10;
    } else if (major == 2 || major == 3) {
        if (n < 12) return "npy: too short";
        uint32_t l = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8)
                   | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
        hdr_len = l; hdr_start = 12;
    } else {
        return "npy: unsupported version " + std::to_string(major);
    }
    if (n < hdr_start + hdr_len) return "npy: header truncated";
    std::string hdr((const char*)(buf + hdr_start), hdr_len);

    std::string descr = find_dict_value(hdr, "descr");
    // Strip the surrounding quotes — but only when both ends are the *same*
    // quote. A malformed/unterminated value (e.g. "'<f8" with no closing quote)
    // would otherwise have its real last character chopped by the blind
    // substr(1, size-2).
    if (descr.size() >= 2 &&
        (descr.front() == '\'' || descr.front() == '"') &&
        descr.back() == descr.front()) {
        descr = descr.substr(1, descr.size() - 2);
    }
    out->dtype_str = descr;
    if (descr.size() >= 2) {
        // Two-char prefix (endian + kind) + size, or kind + N for |O / |Sn / |UN
        char endian = descr[0];
        char kind   = descr[1];
        int sz = 0;
        if (descr.size() >= 3) {
            try { sz = std::stoi(descr.substr(2)); } catch (...) { sz = 0; }
        }
        if (kind == 'O' || kind == 'S' || kind == 'U' || kind == 'V'
            || kind == 'M' || kind == 'm') {
            out->unsupported = true;
        } else {
            out->dtype_id = npy_letter_to_arrow(endian, kind, sz);
            if (out->dtype_id == arrow::Type::NA) out->unsupported = true;
            out->item_size = (size_t)sz;
        }
    } else {
        out->unsupported = true;
    }

    std::string fo = find_dict_value(hdr, "fortran_order");
    out->fortran_order = (fo.find("True") != std::string::npos);

    std::string sh = find_dict_value(hdr, "shape");
    // shape is a tuple like (100, 167, 512) or () or (100,)
    if (!sh.empty() && sh.front() == '(') sh.erase(0, 1);
    if (!sh.empty() && sh.back()  == ')') sh.pop_back();
    out->shape.clear();
    std::stringstream ss(sh);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // strip whitespace
        size_t a = 0; while (a < tok.size() && std::isspace((unsigned char)tok[a])) ++a;
        size_t b = tok.size(); while (b > a && std::isspace((unsigned char)tok[b-1])) --b;
        if (a >= b) continue;
        try { out->shape.push_back(std::stoll(tok.substr(a, b - a))); }
        catch (...) { return "npy: bad shape '" + sh + "'"; }
    }
    out->data_offset = hdr_start + hdr_len;

    // The readers read a fixed number of bytes per element from the Arrow type,
    // not the header's declared item size — so a crafted dtype whose declared
    // size differs (e.g. "|b0" → item_size 0 for a 1-byte bool) would slip past
    // the shape-fits check below (item_size 0 skips it) and read out of bounds.
    // Pin item_size to what is actually read for a supported dtype.
    if (!out->unsupported && out->dtype_id != arrow::Type::NA) {
        size_t real = npy_element_bytes(out->dtype_id);
        if (real == 0) out->unsupported = true;   // supported id with no known size
        else           out->item_size = real;
    }

    // Validate the declared shape against the data actually present. .npy/.npz
    // input is untrusted (zip members), and the downstream readers derive
    // element counts and byte offsets straight from the shape. Without this, a
    // header like (1000000000,), a negative dimension, or dims whose product
    // overflows would drive out-of-bounds reads, huge allocations, or
    // wrapped-offset pointer arithmetic. Unsupported dtypes are never read, so
    // they skip the check.
    if (!out->unsupported && out->item_size > 0) {
        uint64_t elems = 1;
        // Product of the NON-ZERO dimensions, tracked separately. A single
        // zero dim makes `elems` zero, after which every later overflow guard
        // is vacuous and the total-size check passes for free — but the
        // readers still multiply sub-ranges of the shape to compute strides
        // and slice sizes, and those products can overflow int64. A crafted
        // shape like (1, 392361265078550784, 29, 0) declares an empty array,
        // sails through the size check, then overflows when the 3-D+ path
        // collapses the trailing dims. Every sub-product divides this one, so
        // bounding it bounds all of them.
        uint64_t nz = 1;
        for (int64_t d : out->shape) {
            if (d < 0) return "npy: negative dimension in shape (" + sh + ")";
            uint64_t dd = (uint64_t)d;
            if (dd != 0 && elems > UINT64_MAX / dd)
                return "npy: shape too large (" + sh + ")";
            elems *= dd;
            if (dd != 0) {
                if (nz > (uint64_t)INT64_MAX / dd)
                    return "npy: shape too large (" + sh + ")";
                nz *= dd;
            }
        }
        if (elems > UINT64_MAX / (uint64_t)out->item_size)
            return "npy: shape too large (" + sh + ")";
        uint64_t need  = elems * (uint64_t)out->item_size;
        uint64_t avail = (uint64_t)n - (uint64_t)out->data_offset;  // data_offset<=n
        if (need > avail)
            return "npy: declared shape needs " + std::to_string(need) +
                   " bytes but only " + std::to_string(avail) + " present";
    }
    return "";
}

static std::string shape_str(const std::vector<int64_t>& s) {
    std::string r = "(";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) r += ", ";
        r += std::to_string(s[i]);
    }
    if (s.size() == 1) r += ",";
    r += ")";
    return r;
}

// Build an Arrow Column from a contiguous slab of N elements of dtype_id. The
// slab starts at the .npy data offset, which is not aligned to CType — read each
// element via memcpy (a typed load would be a misaligned access: UB, and it
// faults on aarch64).
template <typename CType, typename ArrowBuilder>
static std::shared_ptr<arrow::Array> make_column(const uint8_t* data, int64_t n) {
    ArrowBuilder b;
    (void)b.Reserve(n);
    for (int64_t i = 0; i < n; ++i) {
        CType v;
        std::memcpy(&v, data + (size_t)i * sizeof(CType), sizeof(CType));
        (void)b.UnsafeAppend(v);
    }
    std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
    return a;
}

static std::shared_ptr<arrow::Array>
slab_to_arrow(arrow::Type::type id, const uint8_t* data, int64_t n) {
    using T = arrow::Type;
    switch (id) {
        case T::BOOL: {
            // numpy bool is 1 byte (0/1). Arrow BoolBuilder appends bool.
            arrow::BooleanBuilder b; (void)b.Reserve(n);
            for (int64_t i = 0; i < n; ++i) (void)b.UnsafeAppend(data[i] != 0);
            std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
            return a;
        }
        case T::INT8:   return make_column<int8_t,   arrow::Int8Builder>(data, n);
        case T::INT16:  return make_column<int16_t,  arrow::Int16Builder>(data, n);
        case T::INT32:  return make_column<int32_t,  arrow::Int32Builder>(data, n);
        case T::INT64:  return make_column<int64_t,  arrow::Int64Builder>(data, n);
        case T::UINT8:  return make_column<uint8_t,  arrow::UInt8Builder>(data, n);
        case T::UINT16: return make_column<uint16_t, arrow::UInt16Builder>(data, n);
        case T::UINT32: return make_column<uint32_t, arrow::UInt32Builder>(data, n);
        case T::UINT64: return make_column<uint64_t, arrow::UInt64Builder>(data, n);
        case T::FLOAT:  return make_column<float,    arrow::FloatBuilder>(data, n);
        case T::DOUBLE: return make_column<double,   arrow::DoubleBuilder>(data, n);
        default: return nullptr;
    }
}

static std::shared_ptr<arrow::DataType> arrow_dt(arrow::Type::type id) {
    return arrow_type_for_id(id);
}

// Convert a 1-D slab to a single-column table.
static std::shared_ptr<arrow::Table>
build_1d_table(const std::string& name, arrow::Type::type id,
                const uint8_t* data, int64_t n) {
    auto col = slab_to_arrow(id, data, n);
    if (!col) return nullptr;
    auto schema = arrow::schema({arrow::field(name, arrow_dt(id))});
    return arrow::Table::Make(schema, {col}, n);
}

// Convert a 2-D slab (rows × cols) C-contiguous to an Arrow table.
// Cap on the number of columns rendered from a 2-D array: one Arrow Array +
// Field is built per column, so a genuinely-wide array (or a hostile one that
// passes the shape/buffer bounds check) would otherwise allocate unboundedly.
// The full width is reported to the caller so the footer can flag truncation.
static constexpr int64_t kNpzMaxCols = 4096;

// fortran_order arrays would have transposed memory layout — we handle
// the common (False) case; F-order falls back to column-major read.
// Only the first kNpzMaxCols columns are materialised; `full_cols_out` (when
// non-null) receives the declared width so callers can note any truncation.
static std::shared_ptr<arrow::Table>
build_2d_table(arrow::Type::type id, const uint8_t* data,
                int64_t rows, int64_t cols, size_t item_size,
                bool fortran_order, int64_t* full_cols_out = nullptr) {
    if (full_cols_out) *full_cols_out = cols;
    const int64_t full_cols = cols;        // row stride in the C-order buffer
    if (cols > kNpzMaxCols) cols = kNpzMaxCols;
    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols_out;
    auto dt = arrow_dt(id);
    std::vector<uint8_t> tmp;
    for (int64_t c = 0; c < cols; ++c) {
        // Gather column c. C-order: stride = full_cols * item_size between rows
        // (the declared width, even when capped). F-order: column is contiguous
        // (stride = item_size).
        tmp.assign(rows * item_size, 0);
        // rows == 0 leaves tmp empty, so tmp.data() is null and the F-order
        // gather would call memcpy(nullptr, …, 0) — a zero length does not
        // make a null argument legal (the parameters are declared
        // non-null), and UBSan traps it. The C-order loop below is already a
        // no-op at rows == 0; guard both so the intent is explicit.
        if (rows > 0) {
            if (fortran_order) {
                std::memcpy(tmp.data(), data + c * rows * item_size,
                            rows * item_size);
            } else {
                for (int64_t r = 0; r < rows; ++r) {
                    std::memcpy(tmp.data() + r * item_size,
                                data + (r * full_cols + c) * item_size,
                                item_size);
                }
            }
        }
        auto a = slab_to_arrow(id, tmp.data(), rows);
        if (!a) return nullptr;
        cols_out.push_back(a);
        fields.push_back(arrow::field("c" + std::to_string(c), dt));
    }
    return arrow::Table::Make(arrow::schema(fields), cols_out, rows);
}

// One ZIP entry (one .npy) — name, full extracted bytes, parsed header.
struct Entry {
    std::string                       name;     // without .npy suffix
    std::shared_ptr<std::vector<uint8_t>> bytes;
    NpyHeader                         header;
};

// Read all entries from the .npz archive into memory. The .npy bodies are
// kept compressed-in / decompressed-out: minizip handles inflation on
// read. We don't stream — once the file's on disk, materialising the
// arrays in RAM is the simplest path (and they're typically small).
static std::string load_archive(const std::string& path,
                                  std::vector<Entry>* out) {
    unzFile zf = unzOpen(path.c_str());
    if (!zf) return "Cannot open '" + path + "' as NPZ (zip)";

    if (unzGoToFirstFile(zf) != UNZ_OK) {
        unzClose(zf);
        return "'" + path + "': NPZ archive is empty";
    }

    do {
        char name_buf[1024] = {0};
        unz_file_info info{};
        if (unzGetCurrentFileInfo(zf, &info, name_buf, sizeof(name_buf) - 1,
                                   nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(zf);
            return "'" + path + "': cannot read NPZ entry metadata";
        }
        std::string name = name_buf;
        // Strip .npy suffix.
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".npy") == 0)
            name.resize(name.size() - 4);

        if (unzOpenCurrentFile(zf) != UNZ_OK) {
            unzClose(zf);
            return "'" + path + "': cannot open '" + name + "' inside NPZ";
        }
        auto buf = std::make_shared<std::vector<uint8_t>>();
        // `info.uncompressed_size` is from the zip central directory — i.e.
        // attacker-controllable. Reserving it blindly lets a tiny crafted entry
        // that claims gigabytes force a huge allocation (OOM / crash). It's only
        // a pre-sizing hint (the read loop grows the vector as needed), so clamp
        // it to a sane ceiling; a genuinely large array still loads via the loop.
        constexpr uint64_t kMaxReserve = 64u << 20;   // 64 MiB
        buf->reserve((size_t)std::min<uint64_t>(
            (uint64_t)info.uncompressed_size, kMaxReserve));
        uint8_t chunk[64 * 1024];
        while (true) {
            int n = unzReadCurrentFile(zf, chunk, sizeof(chunk));
            if (n < 0) {
                unzCloseCurrentFile(zf); unzClose(zf);
                return "'" + path + "': read error in '" + name + "'";
            }
            if (n == 0) break;
            buf->insert(buf->end(), chunk, chunk + n);
        }
        unzCloseCurrentFile(zf);

        NpyHeader h;
        std::string err = parse_npy_header(buf->data(), buf->size(), &h);
        if (!err.empty()) {
            unzClose(zf);
            return "'" + path + "' / '" + name + "': " + err;
        }
        out->push_back({std::move(name), std::move(buf), std::move(h)});
    } while (unzGoToNextFile(zf) == UNZ_OK);

    unzClose(zf);
    return "";
}

// Spec for one tab: either the summary or one named array. Slice index
// applies only to 3-D+ arrays; otherwise -1.
struct OpenSpec {
    // A bare .npy has no container, so its footer says NPY rather than NPZ
    // and it has no summary tab.
    bool        bare_npy   = false;
    bool        is_summary = false;
    std::string entry_name;          // key into archive
    int64_t     slice_idx = 0;
};

#ifdef VV_FUZZ
// Fuzz entry (see tests/fuzz/fuzz_npy.cpp): parse an untrusted .npy buffer and
// build its table, mirroring NpzSource's shape dispatch. parse_npy_header
// validates the declared shape against the buffer size, so an accepted header
// never drives the builders out of bounds — the harness fuzzes the real
// parse + build path (header parsing, slab_to_arrow, build_1d/2d_table).
void npy_fuzz_one(const uint8_t* buf, size_t n) {
    NpyHeader h;
    if (!parse_npy_header(buf, n, &h).empty()) return;
    if (h.unsupported) return;
    const uint8_t* data = buf + h.data_offset;
    std::shared_ptr<arrow::Table> tbl;
    if (h.shape.empty()) {
        (void)slab_to_arrow(h.dtype_id, data, 1);
    } else if (h.shape.size() == 1) {
        tbl = build_1d_table("x", h.dtype_id, data, h.shape[0]);
    } else if (h.shape.size() == 2) {
        int64_t fc = 0;
        tbl = build_2d_table(h.dtype_id, data, h.shape[0], h.shape[1],
                             h.item_size, h.fortran_order, &fc);
    } else {
        int64_t leading = h.shape[0];
        if (leading > 0) {
            int64_t rest = 1;   // product of the trailing dims (bounded: the full
            for (size_t i = 1; i < h.shape.size(); ++i) rest *= h.shape[i]; // product fit `n`)
            tbl = build_2d_table(h.dtype_id, data, leading, rest,
                                 h.item_size, h.fortran_order);
        } else {
            tbl = build_2d_table(h.dtype_id, data, 0, 1, h.item_size, h.fortran_order);
        }
    }
    (void)tbl;
}
#endif

class NpzSource : public WorkbookSource {
    std::shared_ptr<std::vector<Entry>> archive_;   // shared across siblings
    OpenSpec    spec_;
    std::vector<OpenSpec> siblings_;

    NpzSource(std::shared_ptr<arrow::Table> table,
               std::string path,
               std::string footer,
               std::shared_ptr<std::vector<Entry>> archive,
               OpenSpec spec,
               std::vector<OpenSpec> siblings)
        : WorkbookSource(std::move(table), std::move(path),
                          std::move(footer)),
          archive_(std::move(archive)),
          spec_(std::move(spec)),
          siblings_(std::move(siblings)) {}

    // Locate an entry by name. Returns nullptr if not found.
    static const Entry* find_entry(const std::vector<Entry>& a,
                                     const std::string& nm) {
        for (const auto& e : a) if (e.name == nm) return &e;
        return nullptr;
    }

    // Build the table for a given spec. Returns "" on success.
    static std::string build_table(const std::vector<Entry>& a,
                                    const OpenSpec& spec,
                                    std::shared_ptr<arrow::Table>* tbl,
                                    std::string* footer) {
        if (spec.is_summary) {
            arrow::StringBuilder nb, sb, db, kb;
            for (const auto& e : a) {
                (void)nb.Append(e.name);
                (void)sb.Append(shape_str(e.header.shape));
                (void)db.Append(e.header.dtype_str);
                std::string kind;
                if (e.header.unsupported) kind = "(pickled / object — skipped)";
                else if (e.header.shape.empty()) kind = "scalar";
                else if (e.header.shape.size() == 1) kind = "1-D";
                else if (e.header.shape.size() == 2) kind = "2-D";
                else kind = std::to_string(e.header.shape.size()) + "-D";
                (void)kb.Append(kind);
            }
            std::shared_ptr<arrow::Array> na, sa, da, ka;
            (void)nb.Finish(&na); (void)sb.Finish(&sa);
            (void)db.Finish(&da); (void)kb.Finish(&ka);
            auto schema = arrow::schema({
                arrow::field("name",  arrow::utf8()),
                arrow::field("shape", arrow::utf8()),
                arrow::field("dtype", arrow::utf8()),
                arrow::field("kind",  arrow::utf8()),
            });
            *tbl = arrow::Table::Make(schema, {na, sa, da, ka},
                                       (int64_t)a.size());
            *footer = "Format: NumPy NPZ  |  Tab: summary  |  Arrays: "
                    + std::to_string(a.size());
            return "";
        }

        const Entry* e = find_entry(a, spec.entry_name);
        if (!e) return "NPZ: entry '" + spec.entry_name + "' not found";
        const auto& h = e->header;
        if (h.unsupported) {
            return "'" + spec.entry_name + "': dtype " + h.dtype_str +
                   " (object/string/structured) not displayable. "
                   "Convert to a fixed numeric dtype with python.";
        }
        const uint8_t* data = e->bytes->data() + h.data_offset;

        if (h.shape.empty()) {
            // 0-D scalar — render as a single-cell table.
            auto col = slab_to_arrow(h.dtype_id, data, 1);
            if (!col) return "NPZ: dtype not supported for '" + e->name + "'";
            auto schema = arrow::schema({arrow::field(e->name, arrow_dt(h.dtype_id))});
            *tbl = arrow::Table::Make(schema, {col}, 1);
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  scalar  |  dtype: " + h.dtype_str;
            return "";
        }

        if (h.shape.size() == 1) {
            *tbl = build_1d_table(e->name, h.dtype_id, data, h.shape[0]);
            if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  " + shape_str(h.shape) +
                      "  |  dtype: " + h.dtype_str;
            return "";
        }

        if (h.shape.size() == 2) {
            int64_t full_c = 0;
            *tbl = build_2d_table(h.dtype_id, data, h.shape[0], h.shape[1],
                                   h.item_size, h.fortran_order, &full_c);
            if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  " + shape_str(h.shape) +
                      "  |  dtype: " + h.dtype_str;
            if ((*tbl)->num_columns() < full_c)
                *footer += "  |  showing first " +
                           std::to_string((*tbl)->num_columns()) + " of " +
                           std::to_string(full_c) + " columns";
            return "";
        }

        // 3-D+. Render a 2-D slice along the leading axis.
        int64_t leading = h.shape[0];
        if (leading <= 0) {
            // Empty leading axis (e.g. shape (0, …)): nothing to slice, and a
            // negative idx clamp would otherwise form a wild pointer.
            *tbl = build_2d_table(h.dtype_id, data, /*rows=*/0, /*cols=*/1,
                                   h.item_size, h.fortran_order);
            if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";
            *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                      "  |  " + shape_str(h.shape) +
                      "  |  dtype: " + h.dtype_str + "  |  empty";
            return "";
        }
        int64_t idx = spec.slice_idx;
        if (idx < 0) idx = 0;
        if (idx >= leading) idx = leading - 1;
        // Treat dims [1:] as (rows, cols). For >3-D we collapse the
        // tail into a single column dimension.
        int64_t inner_rows = h.shape[1];
        int64_t inner_cols = 1;
        for (size_t i = 2; i < h.shape.size(); ++i) inner_cols *= h.shape[i];
        size_t slice_bytes = (size_t)inner_rows * inner_cols * h.item_size;
        const uint8_t* slice_data = data + (size_t)idx * slice_bytes;

        int64_t full_c = 0;
        *tbl = build_2d_table(h.dtype_id, slice_data,
                               inner_rows, inner_cols,
                               h.item_size, h.fortran_order, &full_c);
        if (!*tbl) return "NPZ: dtype not supported for '" + e->name + "'";

        std::string slice_desc = "[" + std::to_string(idx) + ", :, :";
        for (size_t i = 3; i < h.shape.size(); ++i) slice_desc += ", :";
        slice_desc += "]";
        *footer = std::string("Format: NumPy ") + (spec.bare_npy ? "NPY" : "NPZ") +
                      "  |  Array: " + e->name +
                  "  |  " + shape_str(h.shape) +
                  "  |  dtype: " + h.dtype_str +
                  "  |  slice " + slice_desc +
                  " (" + std::to_string(idx + 1) + "/" + std::to_string(leading)
                  + ")  |  [ / ] step slice  •  :slice N jumps";
        if ((*tbl)->num_columns() < full_c)
            *footer += "  |  showing first " +
                       std::to_string((*tbl)->num_columns()) + " of " +
                       std::to_string(full_c) + " columns";
        return "";
    }

    static std::string build_one(const std::string& path,
                                   std::shared_ptr<std::vector<Entry>> archive,
                                   OpenSpec spec,
                                   std::vector<OpenSpec> siblings,
                                   std::unique_ptr<NpzSource>* out) {
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        std::string err = build_table(*archive, spec, &tbl, &footer);
        if (!err.empty()) return err;
        if (!siblings.empty())
            footer += "  |  +" + std::to_string(siblings.size()) +
                       " more tab(s)";
        out->reset(new NpzSource(std::move(tbl), path, std::move(footer),
                                   std::move(archive), std::move(spec),
                                   std::move(siblings)));
        return "";
    }

public:
    // A bare .npy is a single array with no container around it. Wrap it as a
    // one-entry archive and reuse the whole NPZ path — same parser, same
    // hardening, no second implementation to keep in step.
    //
    // README has documented `.npy` since the NumPy viewer landed, but no
    // dispatch branch ever existed, so `vv x.npy` answered "unrecognised file
    // extension". Building the format registry is what surfaced that.
    static std::string open_npy(const std::string& path,
                                 std::unique_ptr<NpzSource>* out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return "Cannot open '" + path + "'";
        auto bytes = std::make_shared<std::vector<uint8_t>>(
            std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (bytes->empty()) return "'" + path + "': empty file";

        Entry e;
        // Tab label: the basename without the .npy suffix, mirroring how an
        // in-archive member is named.
        {
            std::string base = path;
            auto slash = base.find_last_of('/');
            if (slash != std::string::npos) base.erase(0, slash + 1);
            if (base.size() > 4 &&
                fends_ci(base, ".npy")) base.erase(base.size() - 4);
            e.name = base.empty() ? std::string("array") : base;
        }
        e.bytes = bytes;
        std::string err = parse_npy_header(bytes->data(), bytes->size(),
                                            &e.header);
        if (!err.empty()) return "'" + path + "': " + err;
        if (e.header.unsupported)
            return "'" + path + "': unsupported dtype '" + e.header.dtype_str +
                   "' (object / structured arrays are not displayed)";

        auto archive = std::make_shared<std::vector<Entry>>();
        archive->push_back(std::move(e));
        OpenSpec spec;
        spec.bare_npy   = true;
        spec.entry_name = (*archive)[0].name;
        return build_one(path, std::move(archive), std::move(spec), {}, out);
    }

    static std::string open_first(const std::string& path,
                                    std::unique_ptr<NpzSource>* out) {
        auto archive = std::make_shared<std::vector<Entry>>();
        std::string err = load_archive(path, archive.get());
        if (!err.empty()) return err;
        if (archive->empty()) return "'" + path + "': NPZ has no arrays";

        // Tab order: summary, then one tab per non-object array in the
        // order they appear in the archive.
        std::vector<OpenSpec> specs;
        specs.push_back({/*bare_npy=*/false, /*is_summary=*/true, "", 0});
        for (const auto& e : *archive) {
            if (!e.header.unsupported)
                specs.push_back({false, false, e.name, 0});
        }

        OpenSpec first = specs.front();
        std::vector<OpenSpec> siblings(specs.begin() + 1, specs.end());
        return build_one(path, std::move(archive), std::move(first),
                          std::move(siblings), out);
    }

    std::string tab_label() const override {
        return spec_.is_summary ? std::string("summary") : spec_.entry_name;
    }

    std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> result;
        for (const auto& s : siblings_) {
            std::unique_ptr<NpzSource> src;
            std::string err = build_one(path(), archive_, s,
                                         /*siblings=*/{}, &src);
            if (!err.empty()) {
                std::fprintf(stderr, "vv: NPZ tab '%s': %s\n",
                              s.entry_name.c_str(), err.c_str());
                continue;
            }
            result.push_back(std::move(src));
        }
        return result;
    }

    // Slice-axis navigation. Returns true if the slice changed.
    bool change_slice(int delta, bool absolute, int64_t target) override {
        if (spec_.is_summary) return false;
        const Entry* e = find_entry(*archive_, spec_.entry_name);
        if (!e || e->header.shape.size() < 3) return false;
        int64_t max = e->header.shape[0];
        int64_t new_idx = absolute ? target : spec_.slice_idx + delta;
        if (new_idx < 0) new_idx = 0;
        if (new_idx >= max) new_idx = max - 1;
        if (new_idx == spec_.slice_idx) return false;
        spec_.slice_idx = new_idx;
        // Rebuild the underlying table + footer in place.
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        std::string err = build_table(*archive_, spec_, &tbl, &footer);
        if (!err.empty()) return false;
        replace_table(std::move(tbl), std::move(footer));
        return true;
    }
};

}  // namespace npz

// ── mpileup --decode-pileup helpers ──────────────────────────────────────────
//
// Explodes the packed `bases` cell of an mpileup row into per-allele
// counts. Format: . = match on forward strand, , = match on reverse
// strand, [ACGTNacgtn] = mismatch on that strand, * = deletion
// placeholder (counts toward depth, consumes a quality char), ^X = start
// of read where X-33 is mapping quality (the char *after* X is the
// actual base), $ = end of read (postfix on previous base), +N<seq> =
// insertion of N bases after the previous base (consumes N bases from
// the string but no quality chars), -N<seq> = deletion of N bases
// likewise. We count A/C/G/T/N (case-insensitive; matches map to the
// reference allele), ins / del events, deletion placeholders, forward /
// reverse strand reads, and mean Phred quality across the real bases.

struct PileupCounts {
    int64_t A = 0, C = 0, G = 0, T = 0, N = 0;
    int64_t del_placeholder = 0;   // count of '*' characters
    int64_t ins = 0;               // count of +N<seq> events
    int64_t del = 0;               // count of -N<seq> events
    int64_t fwd = 0, rev = 0;
    double  mean_qual = -1.0;      // -1 when no quality chars consumed
};

static PileupCounts decode_pileup_cell(const std::string_view bases,
                                        const std::string_view quals,
                                        char ref) {
    PileupCounts c;
    char ref_upper = (char)std::toupper((unsigned char)ref);
    size_t qual_idx = 0;
    int64_t qual_sum = 0;
    int     qual_count = 0;
    auto consume_qual = [&]() {
        if (qual_idx < quals.size()) {
            qual_sum += (int)(unsigned char)quals[qual_idx] - 33;
            ++qual_count;
        }
        ++qual_idx;
    };
    auto bump_base = [&](char b, bool reverse) {
        char u = (char)std::toupper((unsigned char)b);
        if      (u == 'A') ++c.A;
        else if (u == 'C') ++c.C;
        else if (u == 'G') ++c.G;
        else if (u == 'T') ++c.T;
        else               ++c.N;
        if (reverse) ++c.rev; else ++c.fwd;
        consume_qual();
    };
    for (size_t i = 0; i < bases.size(); ) {
        char ch = bases[i];
        if (ch == '^') {                       // ^<mapq><base>: skip ^ and mapq
            i += 2; continue;
        }
        if (ch == '$') { ++i; continue; }      // postfix end-of-read marker
        if (ch == '*') {                        // deletion placeholder
            ++c.del_placeholder;
            consume_qual();
            ++i;
            continue;
        }
        if (ch == '+' || ch == '-') {           // indel description on prev base
            ++i;
            int n = 0;
            while (i < bases.size() && std::isdigit((unsigned char)bases[i])) {
                n = n * 10 + (bases[i] - '0');
                ++i;
            }
            if (ch == '+') ++c.ins; else ++c.del;
            // Skip the indel sequence; no quality chars belong to it.
            i = std::min(i + (size_t)n, bases.size());
            continue;
        }
        if (ch == '.' || ch == ',') {
            bump_base(ref_upper, /*reverse=*/ch == ',');
            ++i;
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            bool reverse = (ch >= 'a' && ch <= 'z');
            bump_base(ch, reverse);
            ++i;
            continue;
        }
        // Unknown char (rare; '>' '<' refskip markers fall here) — skip silently.
        ++i;
    }
    c.mean_qual = (qual_count > 0)
                  ? (double)qual_sum / qual_count
                  : -1.0;
    return c;
}

// Read every row from the underlying mpileup DelimitedSource, decode each
// packed bases/quals cell, and return a TabularSource backed by an Arrow
// Table with the typed per-allele schema. The decoded view is materialised
// in memory; range queries on bgzipped mpileup files (-r chr:start-end)
// still go through the underlying source first, so only the queried rows
// hit the decoder.
static std::string decode_mpileup_to_memory(
        TabularSource& src,
        const std::string& path,
        std::unique_ptr<TabularSource>* out) {
    auto schema = src.schema();
    int nf = schema->num_fields();
    if (nf < 6 || (nf - 3) % 3 != 0)
        return "decode-pileup: unexpected column count (" +
               std::to_string(nf) + ") for an mpileup file";
    int samples = (nf - 3) / 3;

    // Output schema: chrom/pos/ref + 11 typed columns per sample
    // (depth + A/C/G/T/N + del_placeholder + ins/del + fwd/rev + mean_qual).
    arrow::FieldVector fields;
    fields.push_back(arrow::field("chrom", arrow::utf8()));
    fields.push_back(arrow::field("pos",   arrow::int64()));
    fields.push_back(arrow::field("ref",   arrow::utf8()));
    static const char* kNames[] = {
        "A", "C", "G", "T", "N",
        "del_placeholder", "ins", "del",
        "fwd", "rev", "mean_qual"
    };
    for (int s = 0; s < samples; ++s) {
        std::string sfx = (samples == 1) ? std::string{}
                                            : "_" + std::to_string(s + 1);
        fields.push_back(arrow::field("depth" + sfx, arrow::int64()));
        for (int k = 0; k < 11; ++k) {
            auto t = (std::string(kNames[k]) == "mean_qual")
                     ? arrow::float64() : arrow::int64();
            fields.push_back(arrow::field(std::string(kNames[k]) + sfx, t));
        }
    }
    auto out_schema = arrow::schema(fields);

    // Builders in matching order.
    arrow::StringBuilder  b_chrom;
    arrow::Int64Builder   b_pos;
    arrow::StringBuilder  b_ref;
    struct SampleBuilders {
        arrow::Int64Builder   depth;
        arrow::Int64Builder   A, C, G, T, N;
        arrow::Int64Builder   del_p, ins, del, fwd, rev;
        arrow::DoubleBuilder  mean_qual;
    };
    std::vector<SampleBuilders> sb(samples);

    int nc = src.num_chunks();
    std::vector<int> all_cols(nf);
    std::iota(all_cols.begin(), all_cols.end(), 0);

    for (int ci = 0; ci < nc; ++ci) {
        std::shared_ptr<arrow::Table> chunk;
        auto st = src.read_chunk(ci, all_cols, &chunk);
        if (!st.ok()) return "decode-pileup: " + st.ToString();
        if (!chunk || chunk->num_rows() == 0) continue;
        auto cmb = chunk->CombineChunks();
        if (!cmb.ok()) return "decode-pileup: " + cmb.status().ToString();
        chunk = *cmb;

        auto col_chrom = std::dynamic_pointer_cast<arrow::StringArray>(
            chunk->column(0)->chunk(0));
        auto col_pos   = std::dynamic_pointer_cast<arrow::Int64Array>(
            chunk->column(1)->chunk(0));
        auto col_ref   = std::dynamic_pointer_cast<arrow::StringArray>(
            chunk->column(2)->chunk(0));
        if (!col_chrom || !col_pos || !col_ref)
            return "decode-pileup: unexpected types in chrom/pos/ref columns";

        std::vector<std::shared_ptr<arrow::Int64Array>>  col_depth(samples);
        std::vector<std::shared_ptr<arrow::StringArray>> col_bases(samples);
        std::vector<std::shared_ptr<arrow::StringArray>> col_quals(samples);
        for (int s = 0; s < samples; ++s) {
            col_depth[s] = std::dynamic_pointer_cast<arrow::Int64Array>(
                chunk->column(3 + s*3)->chunk(0));
            col_bases[s] = std::dynamic_pointer_cast<arrow::StringArray>(
                chunk->column(4 + s*3)->chunk(0));
            col_quals[s] = std::dynamic_pointer_cast<arrow::StringArray>(
                chunk->column(5 + s*3)->chunk(0));
            if (!col_depth[s] || !col_bases[s] || !col_quals[s])
                return "decode-pileup: sample " + std::to_string(s+1) +
                       " has unexpected types";
        }

        int64_t n = chunk->num_rows();
        for (int64_t r = 0; r < n; ++r) {
            (void)b_chrom.Append(col_chrom->GetView(r));
            (void)b_pos.Append(col_pos->Value(r));
            std::string_view rv = col_ref->IsNull(r)
                                  ? std::string_view{}
                                  : col_ref->GetView(r);
            (void)b_ref.Append(rv);
            char ref_char = rv.empty() ? 'N' : rv[0];
            for (int s = 0; s < samples; ++s) {
                int64_t depth_val = col_depth[s]->IsNull(r)
                                    ? 0 : col_depth[s]->Value(r);
                std::string_view bases = col_bases[s]->IsNull(r)
                                          ? std::string_view{}
                                          : col_bases[s]->GetView(r);
                std::string_view quals = col_quals[s]->IsNull(r)
                                          ? std::string_view{}
                                          : col_quals[s]->GetView(r);
                auto c = decode_pileup_cell(bases, quals, ref_char);
                (void)sb[s].depth.Append(depth_val);
                (void)sb[s].A.Append(c.A);
                (void)sb[s].C.Append(c.C);
                (void)sb[s].G.Append(c.G);
                (void)sb[s].T.Append(c.T);
                (void)sb[s].N.Append(c.N);
                (void)sb[s].del_p.Append(c.del_placeholder);
                (void)sb[s].ins.Append(c.ins);
                (void)sb[s].del.Append(c.del);
                (void)sb[s].fwd.Append(c.fwd);
                (void)sb[s].rev.Append(c.rev);
                if (c.mean_qual < 0)
                    (void)sb[s].mean_qual.AppendNull();
                else
                    (void)sb[s].mean_qual.Append(c.mean_qual);
            }
        }
    }

    std::vector<std::shared_ptr<arrow::Array>> arrs;
    auto finish = [&](auto& bldr) -> std::string {
        std::shared_ptr<arrow::Array> a;
        auto st = bldr.Finish(&a);
        if (!st.ok()) return st.ToString();
        arrs.push_back(std::move(a));
        return "";
    };
    std::string ferr;
    ferr = finish(b_chrom); if (!ferr.empty()) return "decode-pileup: " + ferr;
    ferr = finish(b_pos);   if (!ferr.empty()) return "decode-pileup: " + ferr;
    ferr = finish(b_ref);   if (!ferr.empty()) return "decode-pileup: " + ferr;
    for (int s = 0; s < samples; ++s) {
        ferr = finish(sb[s].depth);   if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].A);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].C);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].G);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].T);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].N);       if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].del_p);   if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].ins);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].del);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].fwd);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].rev);     if (!ferr.empty()) return "decode-pileup: " + ferr;
        ferr = finish(sb[s].mean_qual); if (!ferr.empty()) return "decode-pileup: " + ferr;
    }

    auto table = arrow::Table::Make(out_schema, arrs);
    std::string footer = "Format: mpileup (decoded)";
    if (samples > 1)
        footer += "  |  Samples: " + std::to_string(samples);
    *out = std::make_unique<MemoryTableSource>(table, path, std::move(footer));
    return "";
}

// ── Markdown viewer (.md / .markdown / .mdown / .mkd) ────────────────────────
//
// Renders CommonMark + GFM (tables, strikethrough, task lists, autolinks)
// using the vendored md4c parser. The output model is a flat sequence of
// MdLine values; each line is a list of styled MdSegment runs. Two output
// modes consume the same model:
//   1. Non-interactive: serialise to ANSI on stdout (one segment → SGR
//      codes + literal text).
//   2. TUI: ncurses drawing with attron/color_set per segment.
// GFM tables are pulled out of the prose stream and surfaced as separate
// MemoryTableSource entries (column types inferred via the existing
// csv_buffer_to_table helper). The markdown viewer's main entry point
// (view_markdown, near main()) wires these tables as additional tabs
// alongside the prose tab.

namespace md {

// Style mask for a single text run. Bit-OR composable.
enum MdStyleFlag : uint16_t {
    MD_BOLD   = 1 << 0,
    MD_ITALIC = 1 << 1,
    MD_UNDER  = 1 << 2,
    MD_STRIKE = 1 << 3,
    MD_CODE   = 1 << 4,
    MD_DIM    = 1 << 5,
    MD_REV    = 1 << 6,    // reversed video (used for level-1 headings)
};

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

// ── ANSI helpers ────────────────────────────────────────────────────────────
//
// Map a (style, role) pair to an SGR escape opener. Used in
// non-interactive output. Reads the live g_color palette so theme
// switches at runtime don't need re-rendering.

static std::string ansi_open(uint16_t style, uint8_t role) {
    // g_color.reset is empty when colour is disabled (no TTY without
    // --color=always). In that mode we emit no SGR at all — keeps the
    // output plain-text-pipe-friendly.
    if (g_color.reset == nullptr || *g_color.reset == '\0') return std::string();
    std::string s;
    auto add = [&](const char* code) {
        if (s.empty()) s = "\x1b[";
        else           s += ';';
        s += code;
    };
    if (style & MD_BOLD)   add("1");
    if (style & MD_DIM)    add("2");
    if (style & MD_ITALIC) add("3");
    if (style & MD_UNDER)  add("4");
    if (style & MD_REV)    add("7");
    if (style & MD_STRIKE) add("9");
    if (!s.empty()) s += 'm';
    // Role colour overlays the SGR codes — extracted from g_color so
    // the user's --theme picks the right shade. Roles map to themed
    // strings (eg g_color.header); we just paste the escape verbatim.
    switch (role) {
        case ROLE_H1: case ROLE_H2: case ROLE_H3: case ROLE_H4_PLUS:
            s += g_color.header; break;
        case ROLE_CODE:      s += g_color.row_idx;  break;
        case ROLE_QUOTE:     s += g_color.trunc;    break;
        case ROLE_LINK:      s += g_color.number;   break;
        case ROLE_LINK_URL:  s += g_color.trunc;    break;
        case ROLE_LIST_MARK: s += g_color.number;   break;
        case ROLE_HR:        s += g_color.border;   break;
        case ROLE_IMAGE:     s += g_color.trunc;    break;
        default: break;
    }
    return s;
}

// Strip terminal control bytes from text that originates in a viewed file
// (markdown body, link/image URLs and alt text). Without this a hostile
// document could embed raw escape sequences — ESC/CSI/OSC/BEL — and hijack the
// terminal (rewrite the title, move the cursor, on some terminals worse) the
// moment it's rendered. Drop C0 controls (0x00–0x1F) and DEL (0x7F); keep TAB
// and newline, which are legitimate layout. `keep_ws=false` also strips TAB and
// newline — used for a URL embedded in an OSC 8 escape, which must be a single
// clean token that can't break out of the sequence.
static std::string sanitize_terminal(const std::string& s, bool keep_ws = true) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\t' || c == '\n') { if (keep_ws) out += (char)c; continue; }
        if (c < 0x20 || c == 0x7F) continue;   // drop C0 control bytes + DEL
        out += (char)c;
    }
    return out;
}

static void emit_line_ansi(std::string& out, const MdLine& line) {
    // Batch SGR opens — only emit a fresh sequence when the (style, role)
    // pair changes. Close once at end-of-line. Cuts the per-line ANSI byte
    // count by ~5× on a paragraph of bold text and matches what humans /
    // less expect to see in well-formed escape streams.
    uint16_t cur_style = 0;
    uint8_t  cur_role  = ROLE_NONE;
    bool     opened    = false;
    for (const auto& r : line.runs) {
        if (r.verbatim) {
            // Already a raw escape sequence — emit as-is without
            // disturbing the current SGR window.
            out += r.text;
            continue;
        }
        if (r.style != cur_style || r.role != cur_role) {
            if (opened) out += g_color.reset;
            std::string esc = ansi_open(r.style, r.role);
            out += esc;
            cur_style = r.style;
            cur_role  = r.role;
            opened    = (r.style || r.role) && !esc.empty();
        }
        // Non-verbatim runs are file-derived display text — never let it carry
        // raw terminal control bytes. (Verbatim runs are our own OSC 8 / image
        // escapes, handled above.)
        out += sanitize_terminal(r.text);
    }
    if (opened) out += g_color.reset;
    out += '\n';
}

// ── Word-wrap ────────────────────────────────────────────────────────────────
//
// Wrap a list of styled runs to the given column width, preserving SGR
// state across the wrap boundary. Splits on whitespace; falls back to
// hard-cut for runs longer than the line. Style attributes carry over
// per-run, so re-opening SGR after a wrap is automatic on emit.

static void wrap_runs(const std::vector<MdSegment>& src,
                      int width, int indent,
                      std::vector<MdLine>* out) {
    if (width <= 0) width = 80;
    int pos = 0;
    MdLine cur;
    std::string lead(indent, ' ');
    if (!lead.empty()) {
        cur.runs.push_back({lead, 0, ROLE_NONE});
        pos = indent;
    }
    auto push_line = [&]() {
        out->push_back(std::move(cur));
        cur = MdLine{};
        pos = 0;
        if (!lead.empty()) {
            cur.runs.push_back({lead, 0, ROLE_NONE});
            pos = indent;
        }
    };
    // Tracks whether the previous tokens / segments ended with collapsible
    // whitespace. Survives across segment boundaries so a heading whose
    // lead-text ends in " " still gets a space before the next segment's
    // first word.
    bool pending_space = false;
    uint16_t pending_space_style = 0;
    uint8_t  pending_space_role  = 0;

    for (const auto& seg : src) {
        if (seg.verbatim) {
            // Pure escape sequence (OSC 8 open / close etc.). Doesn't
            // contribute to the line's display width and doesn't break
            // on whitespace.
            cur.runs.push_back(seg);
            continue;
        }
        const std::string& t = seg.text;
        size_t i = 0;
        while (i < t.size()) {
            // Skip whitespace; record that we'd want a space before the
            // next token.
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) {
                pending_space      = true;
                pending_space_style = seg.style;
                pending_space_role  = seg.role;
                ++i;
            }
            if (i >= t.size()) break;
            if (t[i] == '\n') {
                push_line();
                pending_space = false;
                ++i;
                continue;
            }
            // Read the next word (one whitespace-delimited token).
            size_t j = i;
            while (j < t.size() && t[j] != ' ' && t[j] != '\t' && t[j] != '\n')
                ++j;
            std::string word = t.substr(i, j - i);
            int wlen = display_width(word);

            // Emit the pending space iff we're past the leading indent.
            // Drop a pending space at a line break (avoids leading spaces
            // on wrapped lines).
            if (pending_space && pos > indent) {
                if (pos + 1 + wlen > width) {
                    push_line();
                    pending_space = false;
                } else {
                    cur.runs.push_back({" ", pending_space_style,
                                         pending_space_role});
                    ++pos;
                    pending_space = false;
                }
            } else {
                pending_space = false;
            }

            if (pos + wlen > width && pos > indent) push_line();
            cur.runs.push_back({word, seg.style, seg.role});
            pos += wlen;
            i = j;
        }
    }
    if (!cur.runs.empty() && !(cur.runs.size() == 1 && cur.runs[0].text == lead))
        push_line();
}

// ── OSC 8 (clickable hyperlinks) detection ──────────────────────────────────
//
// OSC 8 lets us render link text as the visible string while the URL is
// hidden from the visual flow but available on click. Supported in
// modern terminals: kitty, iTerm2, WezTerm, foot, Alacritty, GNOME
// Terminal 3.26+, Konsole 21.12+, xterm 360+, recent Apple Terminal,
// and tmux 3.3+ passes them through.
//
// There's no reliable runtime query for OSC 8 support, so we go by env
// vars. Conservative bias: a positive ID only when we're confident. The
// fallback is the visible "(url)" tail in a contrast colour — readable
// either way.
static bool detect_osc8_support() {
    auto eq = [](const char* env, const char* val) {
        const char* v = std::getenv(env);
        return v && std::string(v) == val;
    };
    auto has = [](const char* env) {
        const char* v = std::getenv(env);
        return v && *v;
    };
    // Strong positive signals.
    if (eq("TERM_PROGRAM", "iTerm.app"))   return true;
    if (eq("TERM_PROGRAM", "WezTerm"))     return true;
    if (eq("TERM_PROGRAM", "vscode"))      return true;
    if (eq("TERM_PROGRAM", "Apple_Terminal")) return true;
    if (has("KITTY_WINDOW_ID"))            return true;
    if (has("ALACRITTY_LOG"))              return true;
    if (has("WEZTERM_PANE"))               return true;
    if (has("VTE_VERSION")) {              // GNOME Terminal, Tilix, ...
        // VTE >= 0.50 (≈May 2017) supports OSC 8.
        return std::atoi(std::getenv("VTE_VERSION")) >= 5000;
    }
    if (has("KONSOLE_VERSION")) {
        // Konsole 21.12 (December 2021) onwards. KONSOLE_VERSION is
        // YYYYMMDD-style ('200000' = 2.0, '210800' = 21.08, '212200' = 21.22).
        return std::atoi(std::getenv("KONSOLE_VERSION")) >= 211200;
    }
    if (has("KONSOLE_DBUS_SESSION"))       return true;  // older Konsole; try it
    const char* term = std::getenv("TERM");
    if (term) {
        std::string t = term;
        if (t.find("kitty") != std::string::npos) return true;
        if (t.find("foot")  != std::string::npos) return true;
        // tmux 3.3+ passes OSC 8 by default. Older tmux strips it but
        // that's tolerable — the text still shows, just without a click
        // target.
        if (t.rfind("tmux", 0) == 0)              return true;
        if (t.rfind("screen", 0) == 0 && has("TMUX")) return true;
    }
    return false;
}

// ── Inline-image protocol detection (kitty / iTerm2) ────────────────────────
//
// Detect once at parse time and stash the chosen protocol on the
// renderer. Both kitty's graphics protocol and iTerm2's OSC 1337 inline-
// images scheme accept the raw image file bytes — we don't need to
// decode or resample, just base64-encode and emit. Falls back to a stub
// `[image: alt]` when neither is available (works for SSH terminals
// that won't ever display pixels).

enum class ImageProto { None, Kitty, ITerm2 };

static ImageProto detect_image_proto() {
    const char* term_program = std::getenv("TERM_PROGRAM");
    const char* lc_term      = std::getenv("LC_TERMINAL");
    if ((term_program && std::string(term_program) == "iTerm.app") ||
        (lc_term      && std::string(lc_term)      == "iTerm2") ||
        (term_program && std::string(term_program) == "WezTerm"))
        return ImageProto::ITerm2;
    const char* term            = std::getenv("TERM");
    const char* kitty_window    = std::getenv("KITTY_WINDOW_ID");
    if (kitty_window ||
        (term && std::string(term).find("kitty") != std::string::npos))
        return ImageProto::Kitty;
    return ImageProto::None;
}

static bool is_remote_url(const std::string& url) {
    return url.rfind("http://",  0) == 0 ||
           url.rfind("https://", 0) == 0 ||
           url.rfind("data:",    0) == 0 ||
           url.rfind("ftp://",   0) == 0;
}

// Recognise PNG / JPEG / GIF magic bytes. Returns nullptr for anything
// else (SVG, BMP, WebP…) — we'll fall back to the alt-text stub there.
static const char* sniff_image_kind(const std::string& bytes) {
    if (bytes.size() >= 8 && (unsigned char)bytes[0] == 0x89 &&
        bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') return "png";
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xFF &&
        (unsigned char)bytes[1] == 0xD8 && (unsigned char)bytes[2] == 0xFF)
        return "jpeg";
    if (bytes.size() >= 6 && (bytes.compare(0, 6, "GIF87a") == 0 ||
                              bytes.compare(0, 6, "GIF89a") == 0)) return "gif";
    return nullptr;
}

// Append the terminal-protocol escape that triggers an inline image
// to `out`. Returns true on success; on failure (file missing / too
// big / unsupported format / colour-off), leaves `out` untouched and
// the caller falls back to the alt-text stub.
static bool render_image_inline(const std::string& abs_path,
                                 const std::string& alt,
                                 ImageProto proto,
                                 MdLine* out) {
    if (proto == ImageProto::None) return false;
    if (g_color.reset == nullptr || *g_color.reset == '\0')
        return false;   // colour-off → keep output plain-text
    std::ifstream f(abs_path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len <= 0 || len > 5 * 1024 * 1024) return false;  // skip huge files
    f.seekg(0, std::ios::beg);
    std::string buf((size_t)len, '\0');
    f.read(buf.data(), len);
    if (!sniff_image_kind(buf)) return false;   // PNG/JPEG/GIF only
    std::string b64 = base64_encode(buf);
    std::string esc;
    if (proto == ImageProto::ITerm2) {
        // OSC 1337 ; File=name=<b64name>;inline=1;preserveAspectRatio=1
        //          : <b64data> BEL
        std::string fname_b64 = base64_encode(
            abs_path.substr(abs_path.find_last_of('/') + 1));
        esc = "\033]1337;File=name=" + fname_b64 +
              ";inline=1;preserveAspectRatio=1:" + b64 + "\a";
    } else {
        // kitty: a=T (transmit+display), f=100 (PNG/JPEG/GIF); the
        // payload is chunked into 4096-byte b64 segments per spec.
        const size_t CHUNK = 4096;
        for (size_t i = 0; i < b64.size(); i += CHUNK) {
            size_t end   = std::min(i + CHUNK, b64.size());
            bool   first = (i == 0);
            bool   last  = (end == b64.size());
            esc += "\033_G";
            if (first) esc += "a=T,f=100,";
            esc += "m=";
            esc += (last ? "0" : "1");
            esc += ';';
            esc.append(b64.data() + i, end - i);
            esc += "\033\\";
        }
    }
    // Prefix with the alt-text stub for terminal-protocol-blind consumers
    // (e.g. `less -R`); the escape itself is a no-op there.
    out->runs.push_back({"\xf0\x9f\x96\xbc  [", MD_DIM, ROLE_IMAGE}); // 🖼
    out->runs.push_back({alt, MD_DIM, ROLE_IMAGE});
    out->runs.push_back({"]", MD_DIM, ROLE_IMAGE});
    // The image-protocol payload is a raw escape (and pure base64 + fixed
    // tokens — no file-derived text): pass it through verbatim so the
    // terminal-control sanitiser in emit_line_ansi doesn't strip it.
    MdSegment img_seg{esc, 0, ROLE_NONE};
    img_seg.verbatim = true;
    out->runs.push_back(std::move(img_seg));
    return true;
}

// ── Tiny HTML token walker (for raw HTML in CommonMark) ─────────────────────
//
// GitHub READMEs lean heavily on raw HTML — `<p align="center">` banners,
// `<a href><b>Link</b></a>` link bars, `<picture><source><img></picture>`
// for dark/light logos, `<sub>` / `<sup>` for sub/superscripts. md4c
// forwards these as MD_BLOCK_HTML (the whole block as one string) and
// MD_TEXT_HTML (inline tags interleaved with normal text). The default
// "dump as dim text" handling left them visually noisy.
//
// This tokeniser handles the tags we care about; everything else (style,
// script, attributes we don't recognise) is dropped. The tokens drive
// the existing Renderer style stack and OSC 8 hyperlink machinery, so
// `<b>` is bold, `<a href>` becomes an OSC 8 link, `<img>` lands as our
// usual image stub or kitty/iTerm2 protocol.

struct HtmlTag {
    std::string name;                   // lowercased, e.g. "a", "p", "br"
    bool        closing    = false;     // </p>
    bool        selfclose  = false;     // <br/>
    std::map<std::string, std::string> attrs;   // lowercased keys
};

// Decode the (small) subset of HTML entities likely to show up in
// README text. Anything we don't recognise passes through verbatim.
static std::string html_decode_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '&') { out += s[i++]; continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += s[i++]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if      (ent == "amp")   out += '&';
        else if (ent == "lt")    out += '<';
        else if (ent == "gt")    out += '>';
        else if (ent == "quot")  out += '"';
        else if (ent == "apos" || ent == "#39") out += '\'';
        else if (ent == "nbsp")  out += ' ';
        else if (ent == "mdash") out += "\xe2\x80\x94";  // —
        else if (ent == "ndash") out += "\xe2\x80\x93";  // –
        else if (ent == "hellip") out += "\xe2\x80\xa6"; // …
        else if (ent.size() > 1 && ent[0] == '#') {
            // Numeric character reference (decimal or hex).
            unsigned cp = 0;
            try {
                cp = (ent[1] == 'x' || ent[1] == 'X')
                       ? (unsigned)std::stoul(ent.substr(2), nullptr, 16)
                       : (unsigned)std::stoul(ent.substr(1));
            } catch (...) { cp = 0; }
            if (cp == 0) { out += s.substr(i, semi - i + 1); }
            else {
                // UTF-8 encode the codepoint.
                if      (cp < 0x80)    out += (char)cp;
                else if (cp < 0x800)   { out += (char)(0xC0 | (cp >> 6));
                                          out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12));
                                          out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                          out += (char)(0x80 | (cp & 0x3F)); }
                else                   { out += (char)(0xF0 | (cp >> 18));
                                          out += (char)(0x80 | ((cp >> 12) & 0x3F));
                                          out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                          out += (char)(0x80 | (cp & 0x3F)); }
            }
        }
        else                     out += s.substr(i, semi - i + 1);
        i = semi + 1;
    }
    return out;
}

// Parse a single tag starting at s[*i] == '<'. Updates *i to point past
// the closing '>'. On failure (malformed), advances *i by 1 and returns
// false so the caller treats the '<' as literal.
static bool html_parse_tag(const std::string& s, size_t* i, HtmlTag* out) {
    size_t end = s.find('>', *i);
    if (end == std::string::npos) { ++*i; return false; }
    *out = HtmlTag{};
    size_t p = *i + 1;
    if (p < s.size() && s[p] == '/') { out->closing = true; ++p; }
    // Tag name.
    size_t name_start = p;
    while (p < s.size() && !std::isspace((unsigned char)s[p]) &&
           s[p] != '/' && s[p] != '>')
        ++p;
    if (p == name_start) { ++*i; return false; }
    out->name = s.substr(name_start, p - name_start);
    for (char& c : out->name) c = (char)std::tolower((unsigned char)c);
    // Attributes.
    while (p < end) {
        while (p < end && std::isspace((unsigned char)s[p])) ++p;
        if (p < end && s[p] == '/') { out->selfclose = true; ++p; continue; }
        if (p >= end) break;
        size_t kstart = p;
        while (p < end && s[p] != '=' && s[p] != '/' &&
               !std::isspace((unsigned char)s[p]))
            ++p;
        if (p == kstart) { ++p; continue; }
        std::string key = s.substr(kstart, p - kstart);
        for (char& c : key) c = (char)std::tolower((unsigned char)c);
        std::string val;
        while (p < end && std::isspace((unsigned char)s[p])) ++p;
        if (p < end && s[p] == '=') {
            ++p;
            while (p < end && std::isspace((unsigned char)s[p])) ++p;
            if (p < end && (s[p] == '"' || s[p] == '\'')) {
                char q = s[p++];
                size_t vstart = p;
                while (p < end && s[p] != q) ++p;
                val = s.substr(vstart, p - vstart);
                if (p < end) ++p;
            } else {
                size_t vstart = p;
                while (p < end && !std::isspace((unsigned char)s[p]) &&
                       s[p] != '/')
                    ++p;
                val = s.substr(vstart, p - vstart);
            }
            val = html_decode_entities(val);
        }
        out->attrs[key] = val;
    }
    *i = end + 1;
    return true;
}

// Drive the HTML token-walker. Translates tags into Renderer style/role
// transitions; emits any literal text as styled segments using the
// renderer's current style mask. Forward-declared because Renderer
// references it.
struct Renderer;
static void html_apply(Renderer& r, const std::string& html);

// ── md4c renderer state ─────────────────────────────────────────────────────

struct Renderer {
    MarkdownDoc* doc;
    int          width;          // terminal width for word wrap
    ImageProto   img_proto = ImageProto::None;
    bool         osc8       = false;  // emit OSC 8 hyperlinks for links

    // Span-style stack — md4c may nest spans (e.g., bold inside link).
    uint16_t     style = 0;
    uint8_t      role  = ROLE_NONE;

    // Current block accumulator.
    MdBlockKind  cur_block = MdBlockKind::Paragraph;
    std::vector<MdSegment> cur_runs;    // text being collected for the block
    int          list_depth = 0;
    std::vector<int> list_index;        // per-nesting ordinal counter (0=ul)
    std::vector<bool> list_is_ordered;
    bool         li_first_text = false; // emit bullet before first text

    int          heading_level = 0;
    int          quote_depth   = 0;
    int          code_lang_role = 0;

    // Table accumulation.
    bool                 in_table = false;
    bool                 in_thead = false;

    // HTML block accumulation. md4c sends `enter_block(HTML)` and then
    // streams the block body via `text(HTML, …)`; we buffer it and run
    // the full string through `html_apply()` once on `leave_block(HTML)`.
    bool                 in_html_block = false;
    std::string          html_buf;
    int                  html_a_depth  = 0;     // <a> OSC 8 closes pending
    int                  html_in_drop  = 0;     // inside <script>/<style>
    unsigned             tbl_cols = 0;
    std::vector<std::string> tbl_headers;
    std::vector<std::vector<std::string>> tbl_rows;
    std::vector<std::string> cur_row;
    std::string          cur_cell;

    // Pending link/image href captured at enter_span(A) / IMG.
    std::string          pending_href;
    std::string          pending_img_src;
    std::string          pending_img_alt;
    bool                 collecting_img_alt = false;

    // ── helpers ────────────────────────────────────────────────────────────
    void finish_block(MdBlockKind kind, int indent = 0) {
        MdBlock b;
        b.kind = kind;
        b.level = (kind == MdBlockKind::Heading) ? heading_level : 0;
        if (!cur_runs.empty())
            wrap_runs(cur_runs, width, indent, &b.lines);
        doc->blocks.push_back(std::move(b));
        cur_runs.clear();
    }

    void push_spacer() {
        // Blank-line separator between blocks, never doubled.
        if (!doc->blocks.empty() &&
            doc->blocks.back().kind != MdBlockKind::Spacer)
            doc->blocks.push_back({MdBlockKind::Spacer, 0, {MdLine{}}, -1, -1});
    }

    void emit_quote_prefix() {
        if (quote_depth <= 0) return;
        std::string s;
        for (int i = 0; i < quote_depth; ++i) s += "\xe2\x96\x8c ";  // ▌
        cur_runs.push_back({s, MD_DIM, ROLE_QUOTE});
    }

    void emit_list_bullet() {
        if (list_depth <= 0) return;
        std::string lead((list_depth - 1) * 2, ' ');
        if (list_is_ordered.back()) {
            list_index.back()++;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d. ", list_index.back());
            cur_runs.push_back({lead, 0, ROLE_NONE});
            cur_runs.push_back({buf,  0, ROLE_LIST_MARK});
        } else {
            cur_runs.push_back({lead + "\xe2\x80\xa2 ", 0, ROLE_LIST_MARK}); // •
        }
    }

    void open_heading(int lvl) {
        finish_paragraph();
        push_spacer();
        heading_level = lvl;
        cur_block = MdBlockKind::Heading;
        // Heading lead: "# " coloured + bold for visual prefix.
        std::string lead;
        for (int i = 0; i < lvl; ++i) lead += '#';
        lead += ' ';
        cur_runs.push_back({lead, MD_BOLD, role_for_heading(lvl)});
        // Subsequent text() calls will compose into the same block;
        // we toggle MD_BOLD via style stack for the inner content.
        style |= MD_BOLD;
        if (lvl == 1) style |= MD_UNDER;
        role = role_for_heading(lvl);
    }
    void close_heading() {
        style = 0;
        role = ROLE_NONE;
        finish_block(MdBlockKind::Heading);
    }
    static uint8_t role_for_heading(int lvl) {
        switch (lvl) {
            case 1: return ROLE_H1;
            case 2: return ROLE_H2;
            case 3: return ROLE_H3;
            default: return ROLE_H4_PLUS;
        }
    }

    void finish_paragraph() {
        if (cur_runs.empty()) return;
        // Drop a trailing space that wrap_runs would emit otherwise.
        MdBlockKind k = (quote_depth > 0) ? MdBlockKind::Quote
                                          : MdBlockKind::Paragraph;
        int indent = quote_depth * 2;
        // For block quotes we put the ▌ glyphs at the start of every
        // wrapped line, not just the first — easier to inject before wrap.
        finish_block(k, indent);
    }

    // ── md4c callbacks ─────────────────────────────────────────────────────
    int enter_block(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
            case MD_BLOCK_DOC:        break;
            case MD_BLOCK_QUOTE:
                push_spacer();
                quote_depth++;
                break;
            case MD_BLOCK_UL: {
                push_spacer();
                list_depth++;
                list_is_ordered.push_back(false);
                list_index.push_back(0);
                break;
            }
            case MD_BLOCK_OL: {
                push_spacer();
                list_depth++;
                list_is_ordered.push_back(true);
                auto* d = (MD_BLOCK_OL_DETAIL*)detail;
                list_index.push_back((int)d->start - 1);
                break;
            }
            case MD_BLOCK_LI:
                li_first_text = true;
                // For tight lists md4c doesn't wrap items in MD_BLOCK_P,
                // so we have to seed the bullet here. The MD_BLOCK_P
                // handler below also calls emit_list_bullet under the
                // same guard, but li_first_text becomes false after
                // either path, so we emit exactly one bullet per item.
                emit_quote_prefix();
                emit_list_bullet();
                li_first_text = false;
                break;
            case MD_BLOCK_HR: {
                push_spacer();
                MdBlock b{MdBlockKind::HRule, 0, {}, -1, -1};
                MdLine line;
                std::string rule;
                int w = width > 0 ? width : 80;
                for (int i = 0; i < w; ++i) rule += "\xe2\x94\x80"; // ─
                line.runs.push_back({rule, 0, ROLE_HR});
                b.lines.push_back(std::move(line));
                doc->blocks.push_back(std::move(b));
                push_spacer();
                break;
            }
            case MD_BLOCK_H: {
                auto* d = (MD_BLOCK_H_DETAIL*)detail;
                open_heading((int)d->level);
                break;
            }
            case MD_BLOCK_CODE: {
                push_spacer();
                cur_block = MdBlockKind::Code;
                break;
            }
            case MD_BLOCK_P:
                if (list_depth > 0 && li_first_text) {
                    emit_quote_prefix();
                    emit_list_bullet();
                    li_first_text = false;
                } else if (quote_depth > 0) {
                    emit_quote_prefix();
                }
                cur_block = MdBlockKind::Paragraph;
                break;
            case MD_BLOCK_TABLE: {
                push_spacer();
                auto* d = (MD_BLOCK_TABLE_DETAIL*)detail;
                in_table = true;
                tbl_cols = d->col_count;
                tbl_headers.clear();
                tbl_rows.clear();
                cur_row.clear();
                cur_cell.clear();
                break;
            }
            case MD_BLOCK_THEAD:
                in_thead = true;
                break;
            case MD_BLOCK_TBODY:
                in_thead = false;
                break;
            case MD_BLOCK_TR:
                cur_row.clear();
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                cur_cell.clear();
                break;
            case MD_BLOCK_HTML:
                // Accumulate the raw HTML; the actual rendering happens
                // on `leave_block(HTML)` so we can drive a stateful
                // tokeniser over the complete block.
                finish_paragraph();
                in_html_block = true;
                html_buf.clear();
                cur_block = MdBlockKind::Paragraph;
                break;
            default: break;
        }
        return 0;
    }

    int leave_block(MD_BLOCKTYPE type, void* detail) {
        switch (type) {
            case MD_BLOCK_DOC:
                finish_paragraph();
                break;
            case MD_BLOCK_QUOTE:
                quote_depth--;
                if (quote_depth == 0) push_spacer();
                break;
            case MD_BLOCK_UL:
            case MD_BLOCK_OL:
                list_depth--;
                list_is_ordered.pop_back();
                list_index.pop_back();
                if (list_depth == 0) push_spacer();
                break;
            case MD_BLOCK_LI:
                finish_paragraph();
                break;
            case MD_BLOCK_H:
                close_heading();
                push_spacer();
                break;
            case MD_BLOCK_CODE: {
                // Emit fenced code as its own block. cur_runs holds raw
                // text with embedded newlines from text(CODE).
                std::string buf;
                for (const auto& seg : cur_runs) buf += seg.text;
                cur_runs.clear();
                MdBlock b{MdBlockKind::Code, 0, {}, -1, -1};
                size_t pos = 0;
                while (pos <= buf.size()) {
                    size_t nl = buf.find('\n', pos);
                    if (nl == std::string::npos) nl = buf.size();
                    if (pos == buf.size() && nl == buf.size()) break;
                    MdLine line;
                    line.runs.push_back({buf.substr(pos, nl - pos),
                                          0, ROLE_CODE});
                    b.lines.push_back(std::move(line));
                    if (nl == buf.size()) break;
                    pos = nl + 1;
                }
                // Drop a trailing empty line (md4c emits one for trailing
                // newline inside the fence).
                if (!b.lines.empty() &&
                    b.lines.back().runs.size() == 1 &&
                    b.lines.back().runs[0].text.empty())
                    b.lines.pop_back();
                doc->blocks.push_back(std::move(b));
                push_spacer();
                break;
            }
            case MD_BLOCK_P:
                finish_paragraph();
                if (list_depth == 0) push_spacer();
                break;
            case MD_BLOCK_TABLE: {
                in_table = false;
                // Build an in-memory CSV buffer from collected headers /
                // rows and hand to Arrow's CSV reader for type inference.
                std::string csv;
                auto append_quoted = [&](const std::string& s) {
                    bool quote = false;
                    for (char c : s)
                        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                            quote = true; break;
                        }
                    if (!quote) { csv += s; return; }
                    csv += '"';
                    for (char c : s) {
                        if (c == '"') csv += '"';
                        csv += c;
                    }
                    csv += '"';
                };
                for (size_t i = 0; i < tbl_headers.size(); ++i) {
                    if (i) csv += ',';
                    append_quoted(tbl_headers[i]);
                }
                csv += '\n';
                for (const auto& row : tbl_rows) {
                    for (size_t i = 0; i < row.size(); ++i) {
                        if (i) csv += ',';
                        append_quoted(row[i]);
                    }
                    // Pad short rows out to the header column count.
                    for (size_t i = row.size(); i < tbl_headers.size(); ++i)
                        csv += ',';
                    csv += '\n';
                }
                auto tbl_or = csv_buffer_to_table(csv);
                if (tbl_or.ok()) {
                    doc->tables.push_back(*tbl_or);
                    // Caption: the most recent heading text (if any), or
                    // "Table N".
                    std::string cap = "Table " +
                        std::to_string(doc->tables.size());
                    for (auto it = doc->blocks.rbegin();
                         it != doc->blocks.rend(); ++it) {
                        if (it->kind == MdBlockKind::Heading) {
                            std::string h;
                            for (auto& line : it->lines)
                                for (auto& r : line.runs)
                                    h += r.text;
                            // Strip the leading "## " prefix.
                            while (!h.empty() && (h[0] == '#' || h[0] == ' '))
                                h.erase(h.begin());
                            if (!h.empty()) cap = h + " (table " +
                                std::to_string(doc->tables.size()) + ")";
                            break;
                        }
                    }
                    doc->table_captions.push_back(cap);
                    MdBlock b{MdBlockKind::TablePlaceholder, 0, {}, -1, -1};
                    b.table_idx = (int)doc->tables.size() - 1;
                    MdLine line;
                    line.runs.push_back({"\xe2\x96\xb6 [" + cap + "]",
                                          MD_BOLD, ROLE_LINK});
                    b.lines.push_back(std::move(line));
                    doc->blocks.push_back(std::move(b));
                    push_spacer();
                }
                tbl_headers.clear();
                tbl_rows.clear();
                break;
            }
            case MD_BLOCK_THEAD: in_thead = false; break;
            case MD_BLOCK_TR:
                if (!in_thead && !cur_row.empty())
                    tbl_rows.push_back(std::move(cur_row));
                cur_row.clear();
                break;
            case MD_BLOCK_TH:
                tbl_headers.push_back(std::move(cur_cell));
                cur_cell.clear();
                break;
            case MD_BLOCK_TD:
                cur_row.push_back(std::move(cur_cell));
                cur_cell.clear();
                break;
            case MD_BLOCK_HTML:
                // Drive the HTML token-walker over the buffered block.
                in_html_block = false;
                if (!html_buf.empty()) {
                    html_apply(*this, html_buf);
                    html_buf.clear();
                }
                // Close any unbalanced <a> OSC 8 wrappers.
                while (html_a_depth > 0) {
                    if (osc8 && g_color.reset != nullptr &&
                        *g_color.reset != '\0') {
                        MdSegment seg;
                        seg.text     = "\033]8;;\033\\";
                        seg.verbatim = true;
                        cur_runs.push_back(std::move(seg));
                    }
                    --html_a_depth;
                }
                finish_paragraph();
                push_spacer();
                break;
            default: break;
        }
        return 0;
    }

    int enter_span(MD_SPANTYPE type, void* detail) {
        switch (type) {
            case MD_SPAN_EM:        style |= MD_ITALIC; break;
            case MD_SPAN_STRONG:    style |= MD_BOLD; break;
            case MD_SPAN_U:         style |= MD_UNDER; break;
            case MD_SPAN_DEL:       style |= MD_STRIKE; break;
            case MD_SPAN_CODE:      style |= MD_CODE; role = ROLE_CODE; break;
            case MD_SPAN_A: {
                auto* d = (MD_SPAN_A_DETAIL*)detail;
                pending_href.assign(d->href.text, d->href.size);
                role = ROLE_LINK;
                style |= MD_UNDER;
                // OSC 8: emit the open escape before the link text. The
                // close is paired in leave_span(A). With OSC 8 we can
                // drop the visible ` (url)` tail entirely — the terminal
                // shows the link text as a clickable target.
                if (osc8 && g_color.reset != nullptr && *g_color.reset != '\0' &&
                    !pending_href.empty()) {
                    MdSegment seg;
                    seg.text     = "\033]8;;" +
                                   sanitize_terminal(pending_href, false) +
                                   "\033\\";
                    seg.verbatim = true;
                    cur_runs.push_back(std::move(seg));
                }
                break;
            }
            case MD_SPAN_IMG: {
                auto* d = (MD_SPAN_IMG_DETAIL*)detail;
                pending_img_src.assign(d->src.text, d->src.size);
                pending_img_alt.clear();
                collecting_img_alt = true;
                break;
            }
            default: break;
        }
        return 0;
    }

    int leave_span(MD_SPANTYPE type, void* detail) {
        switch (type) {
            case MD_SPAN_EM:    style &= ~MD_ITALIC; break;
            case MD_SPAN_STRONG: style &= ~MD_BOLD; break;
            case MD_SPAN_U:     style &= ~MD_UNDER; break;
            case MD_SPAN_DEL:   style &= ~MD_STRIKE; break;
            case MD_SPAN_CODE:
                style &= ~MD_CODE;
                role = ROLE_NONE;
                break;
            case MD_SPAN_A:
                style &= ~MD_UNDER;
                role = ROLE_NONE;
                if (!pending_href.empty()) {
                    bool autolink = ((MD_SPAN_A_DETAIL*)detail)->is_autolink;
                    if (osc8 && g_color.reset != nullptr &&
                        *g_color.reset != '\0') {
                        // Close the OSC 8 hyperlink. No visible URL stub
                        // — the link text is clickable in the terminal.
                        MdSegment seg;
                        seg.text     = "\033]8;;\033\\";
                        seg.verbatim = true;
                        cur_runs.push_back(std::move(seg));
                    } else if (!autolink) {
                        // No OSC 8 support → show " (url)" after the
                        // link text. Use ROLE_LINK (bright cyan from
                        // g_color.number) so it stays readable on dark
                        // backgrounds — g_color.trunc would be invisible
                        // on a dark Konsole.
                        cur_runs.push_back({" (" + pending_href + ")",
                                             MD_DIM, ROLE_LINK});
                    }
                }
                pending_href.clear();
                break;
            case MD_SPAN_IMG: {
                collecting_img_alt = false;
                std::string text = pending_img_alt.empty()
                    ? pending_img_src : pending_img_alt;
                bool inlined = false;
                // Local file + supported terminal protocol → render via
                // OSC 1337 / kitty graphics. Stub fallback for everything
                // else (remote URLs, SVGs, files >5 MiB, terminals
                // without graphics support, --color=never).
                if (!pending_img_src.empty() &&
                    !is_remote_url(pending_img_src) &&
                    img_proto != ImageProto::None) {
                    std::string abs_path = pending_img_src;
                    if (!abs_path.empty() && abs_path[0] != '/')
                        abs_path = doc->source_dir + "/" + abs_path;
                    MdLine line;
                    if (render_image_inline(abs_path, text, img_proto, &line)) {
                        // The terminal-protocol escape is per-line; emit
                        // any in-progress paragraph first so the image
                        // doesn't fuse mid-word with surrounding prose.
                        finish_paragraph();
                        MdBlock b{MdBlockKind::Image, 0, {std::move(line)},
                                   -1, -1};
                        doc->blocks.push_back(std::move(b));
                        inlined = true;
                    }
                }
                if (!inlined) {
                    cur_runs.push_back({"\xf0\x9f\x96\xbc  [", MD_DIM, ROLE_IMAGE});
                    cur_runs.push_back({text, MD_DIM, ROLE_IMAGE});
                    cur_runs.push_back({"]", MD_DIM, ROLE_IMAGE});
                    if (!pending_img_src.empty() &&
                        pending_img_src != text) {
                        cur_runs.push_back({" (" + pending_img_src + ")",
                                             MD_DIM, ROLE_LINK_URL});
                    }
                }
                pending_img_src.clear();
                pending_img_alt.clear();
                break;
            }
            default: break;
        }
        return 0;
    }

    int text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size) {
        std::string s(text, size);
        if (collecting_img_alt) {
            pending_img_alt += s;
            return 0;
        }
        if (in_table) {
            if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) cur_cell += ' ';
            else                                                cur_cell += s;
            return 0;
        }
        if (cur_block == MdBlockKind::Code) {
            // Raw code: preserve newlines verbatim. md4c sends each line
            // of a code block as a separate text(CODE, "...\n") call.
            cur_runs.push_back({s, 0, ROLE_CODE});
            return 0;
        }
        // Block-level HTML — buffer; leave_block(HTML) runs html_apply.
        if (in_html_block && type == MD_TEXT_HTML) {
            html_buf += s;
            return 0;
        }
        // Inline HTML inside a paragraph: walk immediately so the next
        // text(NORMAL, …) inherits the right style stack.
        if (type == MD_TEXT_HTML) {
            html_apply(*this, s);
            return 0;
        }
        // Decode entities ("&amp;" → "&", "&mdash;" → "—") in normal text.
        if (type == MD_TEXT_ENTITY) {
            cur_runs.push_back({html_decode_entities(s), style, role});
            return 0;
        }
        if (type == MD_TEXT_BR) {
            cur_runs.push_back({"\n", style, role});
        } else if (type == MD_TEXT_SOFTBR) {
            cur_runs.push_back({" ", style, role});
        } else {
            cur_runs.push_back({s, style, role});
        }
        return 0;
    }
};

// ── HTML tag handler implementation ─────────────────────────────────────────
//
// Walks a string of HTML and emits style transitions / segments via the
// Renderer's existing machinery. Block-level tags (<p>, <h1>–<h6>,
// <div>, <hr>) flush the in-progress paragraph and start a new block of
// the appropriate kind. Inline tags toggle style/role bits. <a href>
// drives our OSC 8 hyperlink emitter; <img>, <picture>, <source> route
// through the inline-image protocol path that markdown's ![](url) uses.
// Unknown tags are dropped (only their text content is kept). <script>
// and <style> bodies are dropped wholesale.

static void html_apply(Renderer& r, const std::string& html) {
    size_t i = 0;
    while (i < html.size()) {
        // Drop the body of a still-open <script>/<style> block.
        if (r.html_in_drop > 0) {
            // Look for the closing tag.
            size_t lt = html.find('<', i);
            if (lt == std::string::npos) { i = html.size(); break; }
            // Try to parse it; if it's the matching close, drop the
            // bytes and pop.
            HtmlTag t;
            size_t probe = lt;
            if (html_parse_tag(html, &probe, &t) &&
                t.closing && (t.name == "script" || t.name == "style")) {
                r.html_in_drop = 0;
                i = probe;
                continue;
            }
            i = lt + 1;
            continue;
        }
        if (html[i] != '<') {
            // Literal text run — find next tag boundary.
            size_t next = html.find('<', i);
            if (next == std::string::npos) next = html.size();
            std::string txt = html.substr(i, next - i);
            txt = html_decode_entities(txt);
            // Collapse whitespace (the HTML block usually has lots of it
            // between centred-banner tags). Skip pure-whitespace runs
            // sandwiched between tags so we don't emit blank padding.
            bool all_ws = true;
            for (char c : txt) if (!std::isspace((unsigned char)c))
                { all_ws = false; break; }
            if (!all_ws) {
                // Convert internal runs of whitespace into a single
                // space so wrap_runs has clean tokens to work with.
                std::string clean;
                clean.reserve(txt.size());
                bool prev_ws = false;
                for (char c : txt) {
                    if (std::isspace((unsigned char)c)) {
                        if (!prev_ws) clean += ' ';
                        prev_ws = true;
                    } else {
                        clean += c;
                        prev_ws = false;
                    }
                }
                r.cur_runs.push_back({clean, r.style, r.role});
            }
            i = next;
            continue;
        }
        // Comment fast-path.
        if (i + 4 <= html.size() && html.compare(i, 4, "<!--") == 0) {
            size_t end = html.find("-->", i + 4);
            i = (end == std::string::npos) ? html.size() : end + 3;
            continue;
        }
        // Skip <!DOCTYPE ...> and any other declarations / processing
        // instructions.
        if (i + 1 < html.size() && (html[i + 1] == '!' || html[i + 1] == '?')) {
            size_t end = html.find('>', i);
            i = (end == std::string::npos) ? html.size() : end + 1;
            continue;
        }
        HtmlTag t;
        if (!html_parse_tag(html, &i, &t)) {
            // Malformed — emit the literal '<' and move on.
            r.cur_runs.push_back({"<", r.style, r.role});
            continue;
        }
        const std::string& n = t.name;
        // ── Block-level transitions ───────────────────────────────────
        auto open_paragraph = [&]() { r.finish_paragraph(); };
        auto open_section_break = [&]() {
            r.finish_paragraph();
            r.push_spacer();
        };
        if (n == "p" || n == "div" || n == "center" || n == "section" ||
            n == "article" || n == "header" || n == "footer" || n == "nav" ||
            n == "main" || n == "aside")
        {
            if (t.closing) open_section_break();
            else           open_paragraph();
            continue;
        }
        if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') {
            int lvl = n[1] - '0';
            if (t.closing) { r.close_heading(); r.push_spacer(); }
            else            { r.open_heading(lvl); }
            continue;
        }
        if (n == "br") {
            r.finish_paragraph();
            continue;
        }
        if (n == "hr") {
            r.finish_paragraph();
            r.push_spacer();
            MdBlock b{MdBlockKind::HRule, 0, {}, -1, -1};
            MdLine line;
            std::string rule;
            int w = r.width > 0 ? r.width : 80;
            for (int k = 0; k < w; ++k) rule += "\xe2\x94\x80"; // ─
            line.runs.push_back({rule, 0, ROLE_HR});
            b.lines.push_back(std::move(line));
            r.doc->blocks.push_back(std::move(b));
            r.push_spacer();
            continue;
        }
        // ── Inline style transitions ──────────────────────────────────
        auto toggle_style = [&](uint16_t bit, bool on) {
            if (on) r.style |= bit; else r.style &= ~bit;
        };
        if (n == "b" || n == "strong") {
            toggle_style(MD_BOLD,   !t.closing);
            continue;
        }
        if (n == "i" || n == "em") {
            toggle_style(MD_ITALIC, !t.closing);
            continue;
        }
        if (n == "u") {
            toggle_style(MD_UNDER,  !t.closing);
            continue;
        }
        if (n == "del" || n == "s" || n == "strike") {
            toggle_style(MD_STRIKE, !t.closing);
            continue;
        }
        if (n == "code" || n == "kbd" || n == "samp" || n == "tt") {
            if (t.closing) { r.style &= ~MD_CODE; r.role = ROLE_NONE; }
            else           { r.style |= MD_CODE;  r.role = ROLE_CODE; }
            continue;
        }
        if (n == "mark") {     // emit as reverse-video, no role colour
            toggle_style(MD_REV, !t.closing);
            continue;
        }
        if (n == "sup" || n == "sub") {
            // Terminals can't render super/sub; just dim them.
            toggle_style(MD_DIM, !t.closing);
            continue;
        }
        if (n == "small") {
            toggle_style(MD_DIM, !t.closing);
            continue;
        }
        // ── Links ─────────────────────────────────────────────────────
        if (n == "a") {
            if (t.closing) {
                if (r.html_a_depth > 0) {
                    r.style &= ~MD_UNDER;
                    r.role  = ROLE_NONE;
                    if (r.osc8 && g_color.reset != nullptr &&
                        *g_color.reset != '\0') {
                        MdSegment seg;
                        seg.text     = "\033]8;;\033\\";
                        seg.verbatim = true;
                        r.cur_runs.push_back(std::move(seg));
                    } else {
                        // Stash the URL for the fallback stub. The href
                        // for an open <a> lived on the renderer via
                        // pending_href.
                        if (!r.pending_href.empty()) {
                            r.cur_runs.push_back(
                                {" (" + r.pending_href + ")",
                                  MD_DIM, ROLE_LINK});
                        }
                    }
                    r.pending_href.clear();
                    --r.html_a_depth;
                }
            } else {
                auto it_href = t.attrs.find("href");
                std::string href = (it_href != t.attrs.end()) ? it_href->second : "";
                r.pending_href = href;
                r.role  = ROLE_LINK;
                r.style |= MD_UNDER;
                if (r.osc8 && !href.empty() && g_color.reset != nullptr &&
                    *g_color.reset != '\0') {
                    MdSegment seg;
                    seg.text     = "\033]8;;" +
                                   sanitize_terminal(href, false) + "\033\\";
                    seg.verbatim = true;
                    r.cur_runs.push_back(std::move(seg));
                }
                ++r.html_a_depth;
            }
            continue;
        }
        // ── Images ────────────────────────────────────────────────────
        // <img src=…> on its own or wrapped in <picture><source>…<img>.
        // We don't try to honour <source srcset=…> selection; the <img>
        // src is the canonical fallback and that's what we render.
        if (n == "img") {
            std::string src = t.attrs.count("src") ? t.attrs["src"] : "";
            std::string alt = t.attrs.count("alt") ? t.attrs["alt"] : "";
            if (src.empty()) continue;
            bool inlined = false;
            if (!is_remote_url(src) && r.img_proto != ImageProto::None) {
                std::string abs_path = src;
                if (!abs_path.empty() && abs_path[0] != '/')
                    abs_path = r.doc->source_dir + "/" + abs_path;
                MdLine line;
                if (render_image_inline(abs_path, alt, r.img_proto, &line)) {
                    r.finish_paragraph();
                    MdBlock b{MdBlockKind::Image, 0, {std::move(line)},
                              -1, -1};
                    r.doc->blocks.push_back(std::move(b));
                    inlined = true;
                }
            }
            if (!inlined) {
                std::string text = alt.empty() ? src : alt;
                r.cur_runs.push_back({"\xf0\x9f\x96\xbc  [", MD_DIM, ROLE_IMAGE});
                r.cur_runs.push_back({text, MD_DIM, ROLE_IMAGE});
                r.cur_runs.push_back({"]", MD_DIM, ROLE_IMAGE});
                if (!src.empty() && src != text) {
                    r.cur_runs.push_back({" (" + src + ")",
                                           MD_DIM, ROLE_LINK_URL});
                }
            }
            continue;
        }
        // <picture> / <source> are wrappers; drop the tags but keep their
        // <img> children visible.
        if (n == "picture" || n == "source") continue;
        // Drop the bodies of these — we render no markup but text inside
        // would be wrong to surface.
        if (!t.closing && (n == "script" || n == "style"))
            r.html_in_drop = 1;
        // Anything else — ignore the tag, keep walking. Text content
        // (children) will be picked up by the surrounding literal-text
        // handler in the next iteration.
    }
}

// C-callable trampolines.
static int cb_enter_block(MD_BLOCKTYPE type, void* detail, void* ud) {
    return static_cast<Renderer*>(ud)->enter_block(type, detail);
}
static int cb_leave_block(MD_BLOCKTYPE type, void* detail, void* ud) {
    return static_cast<Renderer*>(ud)->leave_block(type, detail);
}
static int cb_enter_span(MD_SPANTYPE type, void* detail, void* ud) {
    return static_cast<Renderer*>(ud)->enter_span(type, detail);
}
static int cb_leave_span(MD_SPANTYPE type, void* detail, void* ud) {
    return static_cast<Renderer*>(ud)->leave_span(type, detail);
}
static int cb_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    return static_cast<Renderer*>(ud)->text(type, text, size);
}

// Read the entire file. Returns "" on success, error message on failure.
static std::string slurp_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "Cannot open '" + path + "'";
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return "";
}

// Public entry: parse a markdown file into a MarkdownDoc.
static std::string parse_markdown_file(const std::string& path,
                                        int width,
                                        MarkdownDoc* out) {
    std::string text;
    std::string err = slurp_file(path, &text);
    if (!err.empty()) return err;

    out->source_path = path;
    // Derive source_dir for relative image resolution.
    auto slash = path.find_last_of('/');
    out->source_dir = (slash == std::string::npos) ? "."
                                                    : path.substr(0, slash);

    Renderer r;
    r.doc = out;
    r.width = width;
    r.img_proto = detect_image_proto();
    r.osc8      = detect_osc8_support();

    MD_PARSER parser;
    std::memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_UNDERLINE;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span  = cb_enter_span;
    parser.leave_span  = cb_leave_span;
    parser.text        = cb_text;

    int rc = md_parse(text.data(), (MD_SIZE)text.size(), &parser, &r);
    if (rc != 0) return "md4c: parse failed (" + std::to_string(rc) + ")";
    return "";
}

// Pipe the rendered output through `less -R -F -X --tabs=4` when stdout
// is a TTY — gives the user proper scroll / search without us building
// a markdown-specific ncurses TUI. `emit_fn` does the actual writing
// (via existing print_table / printf / fwrite calls); we just redirect
// stdout to the pipe write end while it runs.
//
// `-R` interprets raw ANSI control sequences (colours, hyperlinks
// thanks to less 632+), `-F` exits if the content fits on one screen
// (so short READMEs don't need a `q`), `-X` keeps the screen contents
// after exit, `--tabs=4` matches our render width assumptions.
//
// Falls back to direct emit if (a) fork / pipe fails, (b) less isn't
// on $PATH (execlp returns; the child cat's its stdin to the original
// terminal stdout so the user still sees the content). SIGPIPE is
// ignored during emit because the user may quit less mid-render.
static int g_pager_pid = -1;
static void emit_via_pager(const std::function<void()>& emit_fn) {
    int pfd[2];
    if (pipe(pfd) != 0) { emit_fn(); return; }
    // Set this BEFORE forking. setenv() takes a libc lock and is not
    // async-signal-safe, so calling it in the child of a fork() from a
    // multithreaded process (Arrow's CPU pool is live by now) can deadlock on
    // a lock another thread held at fork time. Doing it in the parent is
    // harmless: the value only matters to the exec'd `less`, and setenv(...,0)
    // still leaves a user-provided LESSANSIENDCHARS alone.
    setenv("LESSANSIENDCHARS", "mK", 0);
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        emit_fn();
        return;
    }
    if (pid == 0) {
        // Child: stdin from pipe read; stdout stays attached to the
        // user's terminal (inherited from parent BEFORE the parent
        // redirected its own stdout to the pipe).
        close(pfd[1]);
        if (dup2(pfd[0], STDIN_FILENO) < 0) _exit(126);
        close(pfd[0]);
        execlp("less", "less", "-R", "-F", "-X", "--tabs=4", (char*)nullptr);
        // exec failed (less not installed) — cat stdin → stdout.
        char buf[8192]; ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(STDOUT_FILENO, buf + off, n - off);
                if (w < 0) { if (errno == EINTR) continue; break; }
                off += w;
            }
        }
        _exit(0);
    }
    // Parent: redirect own stdout to pipe write, run emit, restore.
    g_pager_pid = pid;
    int saved = dup(STDOUT_FILENO);
    close(pfd[0]);
    std::fflush(stdout);
    if (saved < 0 || dup2(pfd[1], STDOUT_FILENO) < 0) {
        close(pfd[1]);
        if (saved >= 0) close(saved);
        emit_fn();
        waitpid(pid, nullptr, 0);
        g_pager_pid = -1;
        return;
    }
    close(pfd[1]);
    // User-quit (q in less) closes the pipe read end — guard our writes
    // so the resulting SIGPIPE doesn't terminate vv.
    auto prev = signal(SIGPIPE, SIG_IGN);
    emit_fn();
    std::fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    signal(SIGPIPE, prev);
    waitpid(pid, nullptr, 0);
    g_pager_pid = -1;
}

// Emit the prose body of `doc` to stdout as ANSI. Tables are referenced
// by their inline `▶ [Table N: ...]` placeholder and printed afterwards
// using the existing print_table pipeline (caller's responsibility — we
// just emit the prose here).
static void emit_markdown_stdout(const MarkdownDoc& doc) {
    std::string out;
    out.reserve(64 * 1024);
    for (const auto& b : doc.blocks) {
        if (b.kind == MdBlockKind::Spacer) {
            out += '\n';
            continue;
        }
        for (const auto& line : b.lines) emit_line_ansi(out, line);
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
}

}  // namespace md

// ── Packed key=value columns (VCF INFO, GFF/GTF attributes) ─────────────────
// These live ABOVE the VV_CORE_LIB guard on purpose. They used to sit inside
// the ncurses frontend, which meant the TUI could show INFO as virtual columns
// while every export path (--tsv/--json/--parquet/--arrow), libvvcore and the
// Qt GUI saw only the raw blob. ExpandedSource below needs them too.

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
        std::string id, type, number;
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
            if (k == "ID")     id = v;
            if (k == "Type")   type = v;
            if (k == "Number") number = v;
        }
        if (id.empty()) continue;
        // Number is load-bearing: A / R / G / . mean "one value per allele /
        // per genotype / variable", so a declared Integer like AD or PL holds
        // "12,4" per record. Typing that INT64 makes every value null. Only
        // Number=1 (or a Flag, which has none) gets a scalar Arrow type.
        const bool scalar = (number == "1" || number.empty());
        arrow::Type::type t = arrow::Type::STRING;
        if      (type == "Flag")               t = arrow::Type::BOOL;
        else if (!scalar)                      t = arrow::Type::STRING;
        else if (type == "Integer")            t = arrow::Type::INT64;
        else if (type == "Float")              t = arrow::Type::DOUBLE;
        out.emplace_back(std::move(id), t);
    }
    return out;
}
// ── ExpandedSource: packed key=value column → real columns ───────────────────
//
// A decorator over another source. VCF INFO and GFF/GTF attributes carry the
// actual payload of those formats as one opaque string, so `--filter`,
// `--select`, `--parquet`, `--unique` and the Qt GUI could not see any of it.
// The TUI had its own display-only expansion; this replaces that with a real
// schema-level one that every consumer inherits.
//
// The expanded columns are APPENDED, so existing column indices are unchanged
// and the raw blob is still there — a projection or a region-column
// auto-detection that worked before keeps working.
class ExpandedSource : public TabularSource {
    std::unique_ptr<TabularSource>  inner_;
    int                             src_col_ = -1;   // the packed column
    std::vector<std::string>        keys_;           // appended, in order
    std::vector<arrow::Type::type>  types_;
    std::shared_ptr<arrow::Schema>  schema_;
    int                             n_inner_ = 0;

    // Build one expanded column's array from the packed strings of a chunk.
    arrow::Status build_key_array(const arrow::ChunkedArray& packed,
                                   size_t ki,
                                   std::shared_ptr<arrow::Array>* out) const {
        const std::string& key = keys_[ki];
        const arrow::Type::type t = types_[ki];
        arrow::StringBuilder sb;
        arrow::Int64Builder  ib;
        arrow::DoubleBuilder db;
        arrow::BooleanBuilder bb;
        for (const auto& chunk : packed.chunks()) {
            for (int64_t r = 0; r < chunk->length(); ++r) {
                bool found = false;
                std::string val;
                if (!chunk->IsNull(r)) {
                    const std::string raw = cell_to_string(*chunk, r);
                    if (raw != NULL_SYMBOL) {
                        for (auto& kv : parse_kv_list(raw)) {
                            if (kv.first == key) { found = true; val = kv.second; break; }
                        }
                    }
                }
                switch (t) {
                    case arrow::Type::BOOL:
                        // A VCF Flag is present-or-absent, never null.
                        ARROW_RETURN_NOT_OK(bb.Append(found));
                        break;
                    case arrow::Type::INT64:
                        if (!found || val.empty()) ARROW_RETURN_NOT_OK(ib.AppendNull());
                        else {
                            try { ARROW_RETURN_NOT_OK(ib.Append(std::stoll(val))); }
                            catch (...) { ARROW_RETURN_NOT_OK(ib.AppendNull()); }
                        }
                        break;
                    case arrow::Type::DOUBLE:
                        if (!found || val.empty()) ARROW_RETURN_NOT_OK(db.AppendNull());
                        else {
                            try { ARROW_RETURN_NOT_OK(db.Append(std::stod(val))); }
                            catch (...) { ARROW_RETURN_NOT_OK(db.AppendNull()); }
                        }
                        break;
                    default:
                        // A key that is absent is null; a bare flag-like key
                        // that is present with no value is an empty string.
                        if (!found) ARROW_RETURN_NOT_OK(sb.AppendNull());
                        else        ARROW_RETURN_NOT_OK(sb.Append(val));
                }
            }
        }
        switch (t) {
            case arrow::Type::BOOL:   return bb.Finish(out);
            case arrow::Type::INT64:  return ib.Finish(out);
            case arrow::Type::DOUBLE: return db.Finish(out);
            default:                  return sb.Finish(out);
        }
    }

public:
    // Column names that would collide with an inner field get a suffix rather
    // than silently shadowing it.
    static std::string open(std::unique_ptr<TabularSource> inner,
                             const std::string& col_name,
                             std::unique_ptr<TabularSource>* out) {
        auto in_schema = inner->schema();
        int idx = in_schema->GetFieldIndex(col_name);
        if (idx < 0)
            return "--expand: no column named '" + col_name + "'";
        auto t = in_schema->field(idx)->type()->id();
        if (t != arrow::Type::STRING && t != arrow::Type::LARGE_STRING)
            return "--expand: column '" + col_name + "' is " +
                   in_schema->field(idx)->type()->ToString() +
                   ", not text — nothing to unpack";

        auto self = std::unique_ptr<ExpandedSource>(new ExpandedSource());
        self->src_col_ = idx;
        self->n_inner_ = in_schema->num_fields();

        // Key discovery. A VCF declares its INFO keys and their types in the
        // header, which is authoritative and cheap. GFF/GTF declares nothing,
        // so the keys come from the first chunk — see the caveat in --help:
        // a `-n 10` preview and a full scan CAN disagree on the schema.
        std::vector<std::pair<std::string, arrow::Type::type>> decl =
            parse_vcf_info_headers(inner->preamble_below());
        if (decl.empty())
            decl = parse_vcf_info_headers(inner->preamble_above());

        // The ##INFO declarations describe the VCF INFO column and nothing
        // else, so only that column may use them. Taking this branch for any
        // column of any VCF meant `--expand REF` / `--expand FILTER` skipped
        // the shape gate below and appended every declared key as an all-null
        // (all-false, for Flag) column, exit 0, no diagnostic — while
        // man/vv.1 promises such a column is refused. Anything else falls
        // through to discovery, which gates properly.
        const bool decl_applies =
            !decl.empty() && col_name.size() == 4 &&
            (col_name[0]=='I'||col_name[0]=='i') && (col_name[1]=='N'||col_name[1]=='n') &&
            (col_name[2]=='F'||col_name[2]=='f') && (col_name[3]=='O'||col_name[3]=='o');

        if (decl_applies) {
            for (auto& [k, ty] : decl) { self->keys_.push_back(k); self->types_.push_back(ty); }
        } else {
            inner->ensure(0);
            std::shared_ptr<arrow::Table> first;
            if (inner->num_chunks() > 0 &&
                inner->read_chunk(0, {idx}, &first).ok() && first &&
                first->num_columns() == 1) {
                std::set<std::string> seen;
                auto col = first->column(0);
                // Gate first. parse_kv_list() treats a bare token as a flag,
                // so without this a plain text column (BED's Name, say) would
                // manufacture one column per distinct value — 20 junk columns
                // from `--expand Name`. Require that most non-null cells
                // actually carry `=` or `;`.
                int64_t sampled = 0, kv_like = 0;
                for (const auto& chunk : col->chunks()) {
                    for (int64_t r = 0; r < chunk->length(); ++r) {
                        if (chunk->IsNull(r)) continue;
                        const std::string raw = cell_to_string(*chunk, r);
                        if (raw == NULL_SYMBOL) continue;
                        ++sampled;
                        if (raw.find('=') != std::string::npos ||
                            raw.find(';') != std::string::npos) ++kv_like;
                    }
                }
                if (sampled > 0 && kv_like * 2 < sampled)
                    return "--expand: column '" + col_name + "' does not look "
                           "like a key=value list (no '=' or ';' in most "
                           "values) — nothing to unpack";
                for (const auto& chunk : col->chunks()) {
                    for (int64_t r = 0; r < chunk->length(); ++r) {
                        if (chunk->IsNull(r)) continue;
                        const std::string raw = cell_to_string(*chunk, r);
                        if (raw == NULL_SYMBOL) continue;
                        for (auto& kv : parse_kv_list(raw)) {
                            // parse_kv_list returns duplicates (gencode repeats
                            // `tag=`); first occurrence wins, order is stable.
                            if (seen.insert(kv.first).second &&
                                (int)self->keys_.size() < kMaxExpandKeys) {
                                self->keys_.push_back(kv.first);
                                self->types_.push_back(arrow::Type::STRING);
                            }
                        }
                    }
                }
            }
        }
        if (self->keys_.empty())
            return "--expand: found no key=value pairs in column '" + col_name + "'";

        arrow::FieldVector fields;
        for (int i = 0; i < in_schema->num_fields(); ++i)
            fields.push_back(in_schema->field(i));
        for (size_t k = 0; k < self->keys_.size(); ++k) {
            std::string nm = self->keys_[k];
            if (in_schema->GetFieldIndex(nm) >= 0) nm += "_" + col_name;
            fields.push_back(arrow::field(nm, arrow_type_for_id(self->types_[k])));
        }
        self->schema_ = arrow::schema(fields);
        self->inner_  = std::move(inner);
        *out = std::move(self);
        return "";
    }

    static constexpr int kMaxExpandKeys = 256;

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        // Split the request. Any expanded column also needs the packed source
        // column read, even when the caller did not ask for it.
        std::vector<int> inner_req;
        bool want_expanded = false;
        for (int c : col_indices) {
            if (c < n_inner_) inner_req.push_back(c);
            else              want_expanded = true;
        }
        if (want_expanded &&
            std::find(inner_req.begin(), inner_req.end(), src_col_) == inner_req.end())
            inner_req.push_back(src_col_);

        std::shared_ptr<arrow::Table> in_tbl;
        ARROW_RETURN_NOT_OK(inner_->read_chunk(i, inner_req, &in_tbl));
        if (!in_tbl) { *out = nullptr; return arrow::Status::OK(); }

        auto pos_of = [&](int inner_idx) {
            for (size_t k = 0; k < inner_req.size(); ++k)
                if (inner_req[k] == inner_idx) return (int)k;
            return -1;
        };

        // Assemble in the caller's requested order — write_delimited and
        // friends index the result by position within col_indices.
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        for (int c : col_indices) {
            if (c < n_inner_) {
                int p = pos_of(c);
                if (p < 0) return arrow::Status::Invalid("expand: lost inner column");
                fields.push_back(schema_->field(c));
                cols.push_back(in_tbl->column(p));
            } else {
                int p = pos_of(src_col_);
                if (p < 0) return arrow::Status::Invalid("expand: missing packed column");
                std::shared_ptr<arrow::Array> arr;
                ARROW_RETURN_NOT_OK(build_key_array(*in_tbl->column(p),
                                                     (size_t)(c - n_inner_), &arr));
                fields.push_back(schema_->field(c));
                cols.push_back(std::make_shared<arrow::ChunkedArray>(arr));
            }
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, in_tbl->num_rows());
        return arrow::Status::OK();
    }

    // ── Everything else forwards. chunk boundaries are preserved, so the
    //    streaming-eviction contract, chunk_meta() and the exact region
    //    row-count contract all survive unchanged.
    int64_t total_rows()      const override { return inner_->total_rows(); }
    int     num_chunks()      const override { return inner_->num_chunks(); }
    ChunkMeta chunk_meta(int i) const override { return inner_->chunk_meta(i); }
    void    ensure(int i)           override { inner_->ensure(i); }
    void    set_retain_all(bool b)  override { inner_->set_retain_all(b); }
    bool    evicted_any()     const override { return inner_->evicted_any(); }
    arrow::Status read_status() const override { return inner_->read_status(); }
    bool    region_applied()  const override { return inner_->region_applied(); }
    bool    change_slice(int d, bool abs, int64_t t) override {
        return inner_->change_slice(d, abs, t);
    }
    const std::string& path() const override { return inner_->path(); }
    std::string tab_label()   const override { return inner_->tab_label(); }
    std::string created_by()  const override { return inner_->created_by(); }
    std::string top_banner()  const override { return inner_->top_banner(); }
    std::vector<std::string> preamble_above() const override {
        return inner_->preamble_above();
    }
    std::vector<std::string> preamble_below() const override {
        return inner_->preamble_below();
    }
    std::vector<std::string> hidden_for_display() const override {
        return inner_->hidden_for_display();
    }
    std::string format_cell(int col_idx, std::string val) const override {
        return (col_idx < n_inner_) ? inner_->format_cell(col_idx, std::move(val))
                                     : val;
    }
    int min_col_width(int col_idx) const override {
        return (col_idx < n_inner_) ? inner_->min_col_width(col_idx) : 4;
    }
    std::string footer() const override {
        return inner_->footer() + "  |  expanded " +
               schema_->field(src_col_)->name() + " → " +
               std::to_string(keys_.size()) + " columns";
    }
};

// The dispatch ladder. open_source() wraps this so a decorator (--expand)
// applies to every one of its ~25 success paths at once, including the
// multi-file TUI loop, instead of each `*out = std::move(src)` needing a patch.
// ── Plain-text entry points ──────────────────────────────────────────────────
//
// The extension list is deliberately SHORT. `.py`, `.c`, `.conf`, `.toml`,
// `.rst` need no entry: the content sniff at the bottom of the ladder routes
// them to text identically, and claiming them in `--formats` would advertise
// vv as a code viewer with no highlighting. `.json` is left out on purpose so
// a real JSON reader stays possible later.
static const char* kTextExts[] = { ".txt", ".text", ".log" };

static bool text_ext(const std::string& p) {
    for (const char* e : kTextExts) {
        if (fends_ci(p, e)) return true;
        if (fends_ci(p, std::string(e) + ".gz")) return true;
    }
    return false;
}

// True when `path` has no extension at all (a bare `README`, `Makefile`,
// `CHANGELOG`). Such files get no "sniffed as text" note — a note on every
// README would be nagging. A dotfile (`.bashrc`) counts as extension-less.
static bool has_no_extension(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path
                                                    : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos || dot == 0;
}

// Sniff `path` and, if it is text, open it as a TextSource. Returns an error
// string otherwise. `note` asks for the one-line stderr note that says text
// mode was reached by content-sniffing rather than by extension.
static std::string open_text(const std::string& path, const Config& cfg,
                             std::unique_ptr<TabularSource>* out,
                             bool note = false) {
    auto rf = arrow::io::ReadableFile::Open(path);
    if (!rf.ok())
        return "Cannot open '" + path + "': " + rf.status().ToString();
    std::shared_ptr<arrow::io::InputStream> in = rf.ValueOrDie();

    // gzip is detected by magic, not by suffix, so `syslog.1.gz` and a bare
    // `dump.gz` work as well as `notes.txt.gz`.
    bool gz = false;
    {
        auto head = in->Read(2);
        if (head.ok() && (*head)->size() >= 2) {
            const uint8_t* m = (*head)->data();
            gz = (m[0] == 0x1f && m[1] == 0x8b);
        }
        auto st = rf.ValueOrDie()->Seek(0);
        if (!st.ok()) return "Cannot rewind '" + path + "': " + st.ToString();
    }
    if (gz) {
        auto codec = arrow::util::Codec::Create(arrow::Compression::GZIP);
        if (!codec.ok()) return codec.status().ToString();
        auto ci = arrow::io::CompressedInputStream::Make(codec->get(), in);
        if (!ci.ok()) return ci.status().ToString();
        in = ci.ValueOrDie();
    }

    std::string buf(8192, '\0');
    auto got = in->Read(8192, buf.data());
    if (!got.ok()) return "Cannot read '" + path + "': " + got.status().ToString();
    (void)rf.ValueOrDie()->Close();

    TextSniffResult r = sniff_text(buf.data(), (size_t)*got);
    if (r != TextSniffResult::Text) return text_binary_error(path, r);

    if (note)
        std::fprintf(stderr,
                     "vv: %s: no known format claims this extension; "
                     "shown as plain text\n", path.c_str());

    std::unique_ptr<TextSource> src;
    std::string err = TextSource::open(path, gz, cfg, &src);
    if (!err.empty()) return err;
    *out = std::move(src);
    return "";
}

static std::string open_source_dispatch(const std::string& path, const Config& cfg,
                                std::unique_ptr<TabularSource>* out) {
    // ── Determine file kind ──────────────────────────────────────────────────
    bool        is_parquet = false;
    DelimKind   dk         = DelimKind::TSV;

    // --text: read it as plain text whatever the extension says. The escape
    // hatch for a textual-but-tabular file — `vv --text notes.md` shows the
    // markdown source, `vv --text data.csv` shows the raw lines. Still
    // sniffed, so `vv --text foo.bam` is refused rather than dumped.
    // (stdin is handled inside the `-` branch below: it has no path to
    // sniff and its stream is already open.)
    if (cfg.force_text && path != "-") return open_text(path, cfg, out);

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

        // Binary refusal, at the second entry point. Piped binary used to
        // reach Arrow's CSV reader, which echoed the raw bytes back inside a
        // parse error ("Expected 2 columns, got 1: Mu\xef\xbf\xbdm…") —
        // control characters and all, straight at the user's terminal.
        // Sniffing here rather than on the raw fd covers gzip'd input too.
        {
            std::string head(8192, '\0');
            auto got = input->Read(8192, head.data());
            if (!got.ok()) return got.status().ToString();
            head.resize((size_t)*got);
            TextSniffResult tr = sniff_text(head.data(), head.size());
            if (tr != TextSniffResult::Text)
                return text_binary_error("stdin", tr);
            input = std::make_shared<PrependInputStream>(std::move(head), input);
        }

        // --text on stdin: `cat server.log | vv --text -` must behave like
        // `vv server.log`. Without this the flag was a silent no-op and the
        // stream went to the CSV reader, which promotes line 1 to a column
        // header and drops it from the data — the exact lossy rendering the
        // plain-text feature exists to remove. TextSource reads any
        // InputStream, so the already-sniffed, already-gunzipped stream goes
        // straight in.
        if (cfg.force_text) {
            std::unique_ptr<TextSource> tsrc;
            std::string terr = TextSource::open_stream("-", std::move(input), &tsrc);
            if (!terr.empty()) return terr;
            *out = std::move(tsrc);
            return "";
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

    if (fends_ci(path, ".parquet")) {
        is_parquet = true;
    } else if (fends_ci(path, ".arrow")) {
        std::unique_ptr<IpcSource> src;
        std::string err = IpcSource::open(path, false, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".feather")) {
        std::unique_ptr<IpcSource> src;
        std::string err = IpcSource::open(path, true, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".orc")) {
#if VV_HAVE_ORC
        std::unique_ptr<OrcSource> src;
        std::string err = OrcSource::open(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
#else
        return "'" + path + "': vv was built without Apache ORC support "
               "(Arrow needs -DARROW_ORC=ON; rebuild Arrow and vv)";
#endif
        return "";
    } else if (fends_ci(path, ".bam") || fends_ci(path, ".cram")) {
        if (cfg.pileup) {
            // --pileup: walk the alignments through htslib's bam_plp engine
            // and emit mpileup-style per-base rows instead of alignment
            // records. Run the decoded view on top if --decode-pileup is
            // also set.
            std::unique_ptr<BamPileupSource> src;
            std::string err = BamPileupSource::open(path, cfg, &src);
            if (!err.empty()) return err;
            if (cfg.decode_pileup) {
                std::unique_ptr<TabularSource> decoded;
                std::string err2 = decode_mpileup_to_memory(*src, path, &decoded);
                if (!err2.empty()) return err2;
                *out = std::move(decoded);
            } else {
                *out = std::move(src);
            }
            return "";
        }
        std::unique_ptr<BamSource> src;
        std::string err = BamSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".bcf")) {
        std::unique_ptr<BcfSource> src;
        std::string err = BcfSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".bb") || fends_ci(path, ".bigBed") ||
               fends_ci(path, ".bw") || fends_ci(path, ".bigWig")) {
        std::unique_ptr<BigSource> src;
        std::string err = BigSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".2bit")) {
        std::unique_ptr<TwoBitSource> src;
        std::string err = TwoBitSource::open(path, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".sqlite")  || fends_ci(path, ".sqlite3") ||
               fends_ci(path, ".db")) {
        std::unique_ptr<SqliteSource> src;
        std::string err = SqliteSource::open_first(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".xlsx") || fends_ci(path, ".xlsm")) {
        std::unique_ptr<XlsxSource> src;
        std::string err = XlsxSource::open_first(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".fods")) {
        // Flat ODF is a single uncompressed XML document, not a zip container,
        // so OdsSource (minizip -> content.xml -> expat) can never read it.
        // It was advertised in the registry and in --help regardless, and the
        // failure was the unhelpful "Cannot open as ODS (zip)".
        return "'" + path + "': flat OpenDocument (.fods) is a single XML "
               "document, not a zipped .ods, and is not supported. Convert it: "
               "`libreoffice --headless --convert-to ods \"" + path + "\"`.";
    } else if (fends_ci(path, ".ods")) {
        std::unique_ptr<OdsSource> src;
        std::string err = OdsSource::open_first(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".h5ad") || fends_ci(path, ".h5") ||
               fends_ci(path, ".hdf5") || fends_ci(path, ".loom")) {
        std::unique_ptr<h5v::Hdf5Source> src;
        // In delimited export (--tsv/--csv) dump obs/var in full (or honour an
        // explicit -n); the TUI/table view keep the bounded preview. Sparse /
        // dense X and generic datasets stay capped regardless (see build_table).
        int64_t df_cap = cfg.delimiter
            ? (cfg.head_rows_set ? (int64_t)cfg.head_rows : -1)
            : h5v::kDataFrameRowCap;
        std::string err = h5v::Hdf5Source::open_first(path, &src, df_cap);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".npz")) {
        std::unique_ptr<npz::NpzSource> src;
        std::string err = npz::NpzSource::open_first(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".npy")) {
        std::unique_ptr<npz::NpzSource> src;
        std::string err = npz::NpzSource::open_npy(path, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".fa")    || fends_ci(path, ".fa.gz")    ||
               fends_ci(path, ".fasta") || fends_ci(path, ".fasta.gz") ||
               fends_ci(path, ".fna")   || fends_ci(path, ".fna.gz")   ||
               fends_ci(path, ".faa")   || fends_ci(path, ".faa.gz")   ||
               fends_ci(path, ".ffn")   || fends_ci(path, ".ffn.gz")   ||
               fends_ci(path, ".frn")   || fends_ci(path, ".frn.gz")) {
        std::unique_ptr<FastxSource> src;
        std::string err = FastxSource::open(path, /*is_fastq=*/false, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".fq")    || fends_ci(path, ".fq.gz")    ||
               fends_ci(path, ".fastq") || fends_ci(path, ".fastq.gz")) {
        std::unique_ptr<FastxSource> src;
        std::string err = FastxSource::open(path, /*is_fastq=*/true, cfg, &src);
        if (!err.empty()) return err;
        *out = std::move(src);
        return "";
    } else if (fends_ci(path, ".vcf")   || fends_ci(path, ".vcf.gz")) {
        dk = DelimKind::VCF;
    } else if (fends_ci(path, ".gff")   || fends_ci(path, ".gff.gz")  ||
               fends_ci(path, ".gff3")  || fends_ci(path, ".gff3.gz") ||
               fends_ci(path, ".gtf")   || fends_ci(path, ".gtf.gz")) {
        dk = DelimKind::GFF;
    } else if (fends_ci(path, ".sam")) {
        dk = DelimKind::SAM;
    } else if (fends_ci(path, ".paf") || fends_ci(path, ".paf.gz")) {
        dk = DelimKind::PAF;
    } else if (fends_ci(path, ".bed")        || fends_ci(path, ".bed.gz")
            || fends_ci(path, ".narrowPeak") || fends_ci(path, ".narrowPeak.gz")
            || fends_ci(path, ".broadPeak")  || fends_ci(path, ".broadPeak.gz")
            || fends_ci(path, ".gappedPeak") || fends_ci(path, ".gappedPeak.gz")
            || fends_ci(path, ".bedGraph")   || fends_ci(path, ".bedGraph.gz")
            || fends_ci(path, ".bg")         || fends_ci(path, ".bg.gz")
            || fends_ci(path, ".tagAlign")   || fends_ci(path, ".tagAlign.gz")) {
        dk = DelimKind::BED;
    } else if (fends_ci(path, ".pileup")  || fends_ci(path, ".pileup.gz")
            || fends_ci(path, ".mpileup") || fends_ci(path, ".mpileup.gz")
            || fends_ci(path, ".pile")    || fends_ci(path, ".pile.gz")) {
        dk = DelimKind::Mpileup;
    } else if (fends_ci(path, ".tsv")   || fends_ci(path, ".tsv.gz")) {
        dk = DelimKind::TSV;
    } else if (fends_ci(path, ".csv")   || fends_ci(path, ".csv.gz")) {
        dk = DelimKind::CSV;
    } else if (text_ext(path)) {
        // Known text extension. Still sniffed: a `.log` holding a core dump
        // is binary whatever it is called, and the sniff is what keeps the
        // "vv never dumps binary to your terminal" promise true.
        return open_text(path, cfg, out);
    } else {
        // Unknown extension: only accept it if the magic bytes positively
        // identify a known binary format. The previous fallback ran a
        // tabs-vs-commas count on the first 4 KiB and routed everything
        // else through the CSV reader — which produced cryptic parse
        // errors on binary files (`Expected 1 columns, got 2: …<garbage>`)
        // when the user's file was e.g. `.bigwig` (case mismatch) or a
        // format we just don't know.
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
        // LociSSD v4 "colblock": data magic "LSB1". (v3 .lociss is Parquet → the
        // PAR1 sniff above; dispatch is by magic, per the spec.)
        if (buf.ok() && (*buf)->size() >= 4) {
            const uint8_t* mm = (*buf)->data();
            if (mm[0]=='L' && mm[1]=='S' && mm[2]=='B' && mm[3]=='1') {
                std::unique_ptr<LocissV4Source> src;
                std::string err = LocissV4Source::open(path, cfg, &src);
                if (!err.empty()) return err;
                *out = std::move(src);
                return "";
            }
        }
        if (!is_parquet) {
            // No known format claims it. Before giving up, look at the
            // content: if it is text, show it as text. Binary is refused —
            // deliberately unlike less, which offers to dump it anyway.
            return open_text(path, cfg, out, !has_no_extension(path));
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
    // ENCODE peak-family variants ride on top of DelimKind::BED — the
    // dispatch detected them by extension; apply variant-specific naming
    // now that the schema is materialised.
    if (dk == DelimKind::BED) {
        BedVariant bv = BedVariant::None;
        if      (fends_ci(path, ".narrowPeak") || fends_ci(path, ".narrowPeak.gz")) bv = BedVariant::NarrowPeak;
        else if (fends_ci(path, ".broadPeak")  || fends_ci(path, ".broadPeak.gz"))  bv = BedVariant::BroadPeak;
        else if (fends_ci(path, ".gappedPeak") || fends_ci(path, ".gappedPeak.gz")) bv = BedVariant::GappedPeak;
        else if (fends_ci(path, ".bedGraph")   || fends_ci(path, ".bedGraph.gz")
              || fends_ci(path, ".bg")         || fends_ci(path, ".bg.gz"))         bv = BedVariant::BedGraph;
        else if (fends_ci(path, ".tagAlign")   || fends_ci(path, ".tagAlign.gz"))   bv = BedVariant::TagAlign;
        if (bv != BedVariant::None) src->apply_bed_variant(bv);
    }
    // --decode-pileup: materialise the typed allele-count view from the
    // underlying mpileup source. The original streaming source is consumed
    // in full, decoded row by row, and replaced with a MemoryTableSource
    // over the resulting Arrow table.
    if (dk == DelimKind::Mpileup && cfg.decode_pileup) {
        std::unique_ptr<TabularSource> decoded;
        std::string err2 = decode_mpileup_to_memory(*src, path, &decoded);
        if (!err2.empty()) return err2;
        *out = std::move(decoded);
        return "";
    }
    *out = std::move(src);
    return "";
}

// The public entry point: dispatch, then apply --expand once. Doing it here
// rather than at each `*out = std::move(src)` means every format and every
// caller (CLI, multi-file TUI loop, Qt GUI, KDE plugins) gets it, and no
// future dispatch branch can forget to.
std::string open_source(const std::string& path, const Config& cfg,
                         std::unique_ptr<TabularSource>* out) {
    std::string err = open_source_dispatch(path, cfg, out);
    if (!err.empty() || cfg.expand_col.empty() || !*out) return err;
    std::unique_ptr<TabularSource> wrapped;
    err = ExpandedSource::open(std::move(*out), cfg.expand_col, &wrapped);
    if (!err.empty()) return err;
    *out = std::move(wrapped);
    return "";
}

// ── Terminal heatmap / image emission (kitty · sixel · half-block) ──────────
//
// Proof-of-concept "pop a plot in the terminal" path: rasterise the source's
// numeric matrix to a palette-indexed image, then emit it with the best
// terminal-graphics method available. Designed for the remote-bioinformatics
// case (view a Hi-C matrix / coverage / track over SSH). The emit layer is
// reusable for any image vv generates; the codec-rich decode side is left to
// an external viewer (e.g. moderncore's vv) — see docs.
//
// Half-block (▀ + 24-bit fg/bg, 2 px per character row) is the universal
// fallback: it needs only a truecolor terminal, which is ~everything modern.
// kitty's graphics protocol is used when detected (best fidelity); sixel is
// available on request and emits straight from our palette (no quantisation).

namespace img {

struct PalImage {                    // palette-indexed image
    int w = 0, h = 0;
    std::vector<uint8_t>  px;        // w*h indices into pal
    std::vector<uint32_t> pal;       // 0x00RRGGBB, ≤256 entries
};

enum class Mode { Auto, Kitty, Sixel, HalfBlock, Ascii };

// Viridis-ish palette: interpolate a handful of anchors into `n` entries.
static std::vector<uint32_t> viridis_palette(int n) {
    static const int A[][3] = {
        { 68,  1, 84}, { 72, 40,120}, { 62, 73,137}, { 49,104,142},
        { 38,130,142}, { 31,158,137}, { 53,183,121}, {110,206, 88},
        {181,222, 43}, {253,231, 37},
    };
    const int na = (int)(sizeof(A)/sizeof(A[0]));
    std::vector<uint32_t> pal((size_t)n);
    for (int i = 0; i < n; ++i) {
        double t = (n == 1) ? 0.0 : (double)i / (n - 1);
        double f = t * (na - 1);
        int a = (int)f, b = std::min(a + 1, na - 1);
        double g = f - a;
        int r = (int)std::lround(A[a][0] + (A[b][0] - A[a][0]) * g);
        int gg= (int)std::lround(A[a][1] + (A[b][1] - A[a][1]) * g);
        int bl= (int)std::lround(A[a][2] + (A[b][2] - A[a][2]) * g);
        pal[(size_t)i] = ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)bl;
    }
    return pal;
}

// Terminal size in character cells (fallback 80x24).
static void term_cells(int* cols, int* rows) {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *cols = ws.ws_col; *rows = ws.ws_row > 0 ? ws.ws_row : 24;
    } else { *cols = 80; *rows = 24; }
}

// Nearest-neighbour resample of a palette-indexed image to (tw, th).
static PalImage resample(const PalImage& s, int tw, int th) {
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    PalImage d; d.w = tw; d.h = th; d.pal = s.pal; d.px.resize((size_t)tw * th);
    for (int y = 0; y < th; ++y) {
        int sy = (int)((int64_t)y * s.h / th);
        for (int x = 0; x < tw; ++x) {
            int sx = (int)((int64_t)x * s.w / tw);
            d.px[(size_t)y * tw + x] = s.px[(size_t)sy * s.w + sx];
        }
    }
    return d;
}

// ── emitters ────────────────────────────────────────────────────────────────
// Plain-text intensity grid: one character per pixel, no colour, no escape
// sequences — safe to write to a file or pipe. The palette index (0 = low,
// last = high) maps onto a 10-step density ramp.
static void emit_ascii(const PalImage& im) {
    static const char ramp[] = " .:-=+*#%@";          // 10 levels, low → high
    const int steps = (int)sizeof(ramp) - 2;          // 9 (exclude the NUL)
    const uint32_t last = im.pal.empty() ? 0 : (uint32_t)im.pal.size() - 1;
    std::string out;
    out.reserve((size_t)(im.w + 1) * im.h);
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            uint32_t idx = im.px[(size_t)y * im.w + x];
            int level = last ? (int)((uint64_t)idx * steps / last) : 0;
            out += ramp[level];
        }
        out += '\n';
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
}

static void emit_halfblock(const PalImage& im) {
    auto rgb = [&](uint8_t i){ return im.pal[i]; };
    std::string out;
    for (int y = 0; y < im.h; y += 2) {
        for (int x = 0; x < im.w; ++x) {
            uint32_t top = rgb(im.px[(size_t)y * im.w + x]);
            char buf[64];
            if (y + 1 < im.h) {
                uint32_t bot = rgb(im.px[(size_t)(y + 1) * im.w + x]);
                std::snprintf(buf, sizeof buf,
                    "\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um\xe2\x96\x80",
                    (top>>16)&255,(top>>8)&255,top&255,
                    (bot>>16)&255,(bot>>8)&255,bot&255);
            } else {  // odd final row: top half over default background
                std::snprintf(buf, sizeof buf,
                    "\033[49m\033[38;2;%u;%u;%um\xe2\x96\x80",
                    (top>>16)&255,(top>>8)&255,top&255);
            }
            out += buf;
        }
        out += "\033[0m\n";
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
}

static void emit_kitty(const PalImage& im, int cell_cols, int cell_rows) {
    std::string rgba((size_t)im.w * im.h * 4, '\0');
    for (size_t i = 0; i < im.px.size(); ++i) {
        uint32_t c = im.pal[im.px[i]];
        rgba[i*4+0] = (char)((c>>16)&255);
        rgba[i*4+1] = (char)((c>>8)&255);
        rgba[i*4+2] = (char)(c&255);
        rgba[i*4+3] = (char)255;
    }
    std::string b64 = base64_encode(rgba);
    const size_t CHUNK = 4096;
    std::string out;
    for (size_t off = 0; off < b64.size(); off += CHUNK) {
        size_t n = std::min(CHUNK, b64.size() - off);
        bool first = (off == 0), last = (off + n >= b64.size());
        out += "\033_G";
        if (first) {
            char hdr[96];
            std::snprintf(hdr, sizeof hdr, "a=T,f=32,s=%d,v=%d,c=%d,r=%d,",
                          im.w, im.h, cell_cols, cell_rows);
            out += hdr;
        }
        out += "m="; out += (last ? '0' : '1'); out += ';';
        out.append(b64, off, n);
        out += "\033\\";
    }
    out += "\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

static void emit_sixel(const PalImage& im) {
    std::string out = "\033Pq";                     // sixel start
    for (size_t i = 0; i < im.pal.size(); ++i) {    // register palette (0-100)
        uint32_t c = im.pal[i];
        char buf[48];
        std::snprintf(buf, sizeof buf, "#%zu;2;%u;%u;%u", i,
            (((c>>16)&255)*100+127)/255, (((c>>8)&255)*100+127)/255,
            ((c&255)*100+127)/255);
        out += buf;
    }
    std::vector<uint8_t> seen(im.pal.size());
    for (int band = 0; band * 6 < im.h; ++band) {
        // colours present in this 6-row band
        std::fill(seen.begin(), seen.end(), 0);
        for (int k = 0; k < 6; ++k) {
            int row = band*6 + k; if (row >= im.h) break;
            for (int x = 0; x < im.w; ++x) seen[im.px[(size_t)row*im.w + x]] = 1;
        }
        for (size_t c = 0; c < im.pal.size(); ++c) {
            if (!seen[c]) continue;
            out += '#'; out += std::to_string(c);
            for (int x = 0; x < im.w; ++x) {
                int bits = 0;
                for (int k = 0; k < 6; ++k) {
                    int row = band*6 + k;
                    if (row < im.h && im.px[(size_t)row*im.w + x] == c) bits |= (1<<k);
                }
                out += (char)(0x3F + bits);
            }
            out += '$';                              // graphics CR (overlay next colour)
        }
        out += '-';                                  // graphics NL (next band)
    }
    out += "\033\\";                                 // sixel end
    std::fwrite(out.data(), 1, out.size(), stdout);
}

// Auto-select and emit, scaling to fit the terminal. Assumes a ~8x16 px cell
// for the pixel-based protocols.
static void emit(const PalImage& src, Mode mode) {
    int cols, rows; term_cells(&cols, &rows);
    if (mode == Mode::Auto)
        mode = (md::detect_image_proto() == md::ImageProto::Kitty) ? Mode::Kitty
                                                                   : Mode::HalfBlock;
    // Fit a cell box preserving aspect (image px aspect vs ~2:1 cell aspect).
    int box_cols = std::min(cols, std::max(1, src.w));
    int box_rows = std::max(1, (int)std::lround(
        (double)box_cols * src.h / src.w / 2.0));
    if (box_rows > rows - 1) {
        box_rows = std::max(1, rows - 1);
        box_cols = std::min(cols, std::max(1, (int)std::lround(
            (double)box_rows * 2.0 * src.w / src.h)));
    }
    if (mode == Mode::Ascii) {
        emit_ascii(resample(src, box_cols, box_rows));        // one char per cell
    } else if (mode == Mode::HalfBlock) {
        emit_halfblock(resample(src, box_cols, box_rows * 2));
    } else if (mode == Mode::Sixel) {
        emit_sixel(resample(src, box_cols * 8, box_rows * 16));
    } else { // Kitty
        emit_kitty(resample(src, box_cols * 8, box_rows * 16), box_cols, box_rows);
    }
}

}  // namespace img

// --heatmap: build a colour heatmap from the source's numeric matrix (rows ×
// numeric columns), globally normalised, and emit it to the terminal.
static std::string render_heatmap(TabularSource& src, const Config& cfg) {
    auto schema = src.schema();
    std::vector<int> ncols;                          // numeric source columns
    for (int i = 0; i < schema->num_fields(); ++i)
        if (is_numeric_type(schema->field(i)->type()->id())) ncols.push_back(i);
    if (ncols.empty()) return "--heatmap: no numeric columns to plot";

    // Source-resolution caps. The image is resampled down to the terminal box
    // anyway, so a few thousand rows/cols is ample; the cap also bounds the
    // scan buffer (<= 2048*2048*8 B ≈ 32 MiB worst case, vs 128 MiB before).
    const int   MAXROWS = 2048, MAXCOLS = 2048;
    const int    W = std::min((int)ncols.size(), MAXCOLS);
    std::vector<int> use(ncols.begin(), ncols.begin() + W);

    std::vector<double> vals;                         // row-major, W per row
    vals.reserve((size_t)W * 256);                    // avoid early reallocations
    int rows_read = 0;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (int c = 0; rows_read < MAXROWS; ++c) {
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, use, &tbl).ok() || !tbl) continue;
        int64_t n = tbl->num_rows();
        for (int64_t r = 0; r < n && rows_read < MAXROWS; ++r, ++rows_read) {
            for (int j = 0; j < W; ++j) {
                double d;
                bool ok = cell_as_double(*tbl, j, r, &d);
                // Treat missing *and* non-finite (Inf/NaN) cells as gaps: they
                // must not skew the min/max range (an Inf would make every
                // other value normalise to 0) nor reach lround() below, where
                // a non-finite argument is undefined behaviour.
                if (!ok || !std::isfinite(d)) d = std::nan("");
                else { lo = std::min(lo, d); hi = std::max(hi, d); }
                vals.push_back(d);
            }
        }
    }
    if (rows_read == 0) return "--heatmap: no rows to plot";
    if (!std::isfinite(lo))                            // every cell was a gap
        return "--heatmap: no finite numeric values to plot";
    if (!(hi > lo)) hi = lo + 1.0;                    // flat matrix → avoid /0

    img::PalImage im;
    im.w = W; im.h = rows_read;
    im.pal = img::viridis_palette(240);
    im.px.resize(vals.size());
    const uint32_t last = (uint32_t)im.pal.size() - 1;
    for (size_t i = 0; i < vals.size(); ++i) {
        double d = vals[i];
        if (!std::isfinite(d)) { im.px[i] = 0; continue; }  // gap → palette floor
        double t = (d - lo) / (hi - lo);
        uint32_t idx = (uint32_t)std::lround(t * last);
        im.px[i] = (uint8_t)std::min(last, idx);
    }

    img::Mode mode = img::Mode::Auto;
    if      (cfg.image_mode.empty() || cfg.image_mode == "auto") mode = img::Mode::Auto;
    else if (cfg.image_mode == "kitty")     mode = img::Mode::Kitty;
    else if (cfg.image_mode == "sixel")     mode = img::Mode::Sixel;
    else if (cfg.image_mode == "halfblock") mode = img::Mode::HalfBlock;
    else if (cfg.image_mode == "ascii")     mode = img::Mode::Ascii;
    else return "--image-mode: unknown mode '" + cfg.image_mode +
                "' (use auto|kitty|sixel|halfblock|ascii)";

    // The graphical backends write raw terminal escape/control sequences. If
    // stdout isn't a terminal (redirected to a file or a pipe) and the user
    // didn't force a backend, fall back to the plain-text grid so we don't
    // corrupt the output.
    if (mode == img::Mode::Auto && !isatty(STDOUT_FILENO))
        mode = img::Mode::Ascii;

    std::string note;
    if (rows_read >= MAXROWS)        note += "  (first " + std::to_string(MAXROWS) + " rows)";
    if (W < (int)ncols.size())       note += "  (first " + std::to_string(W) +
                                             " of " + std::to_string(ncols.size()) + " numeric cols)";
    std::fprintf(stderr, "heatmap: %d rows \xc3\x97 %d cols  range [%g, %g]%s\n",
                 rows_read, W, lo, hi, note.c_str());
    img::emit(im, mode);
    return "";
}

// ── Delimited output ──────────────────────────────────────────────────────────
// (write_csv_field is defined above)

// Returns "" on success, an error message otherwise — same contract as
// write_json / write_parquet, so a bad --select or --filter reaches main()'s
// report() and exits non-zero instead of printing to stderr and exiting 0.
static std::string write_delimited(TabularSource& src, const Config& cfg) {
    char sep = cfg.delimiter;
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
            return "";
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
static std::string write_markdown(TabularSource& src, const Config& cfg) {
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

// Build an mkstemps() template under $TMPDIR (falling back to /tmp). The
// `--parquet -` / `--arrow -` spools need a seekable file; hardcoding /tmp
// breaks containers whose /tmp is tiny or read-only.
static std::string spool_template(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string d = (dir && *dir) ? dir : "/tmp";
    if (d.back() == '/') d.pop_back();
    return d + "/" + name;
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
        std::string tmpl_s = spool_template("vv-parquet-XXXXXX.parquet");
        std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end());
        tmpl.push_back('\0');
        tmp_fd = mkstemps(tmpl.data(), 8);  // suffix length = ".parquet" = 8
        if (tmp_fd < 0)
            return std::string("Cannot create temp file for --parquet -: ") +
                   std::strerror(errno);
        out_path = tmpl.data();
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

// Stream the source's chunks into an Arrow IPC file (a.k.a. Feather v2) at
// cfg.arrow_out. Same shape as write_parquet — column projection + --filter, an
// "[N rows → path]" stderr summary, and a temp-file spool for `-` (stdout).
static std::string write_arrow(TabularSource& src, const Config& cfg) {
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
        std::string tmpl_s = spool_template("vv-arrow-XXXXXX.arrow");
        std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end());
        tmpl.push_back('\0');
        int fd = mkstemps(tmpl.data(), 6);   // suffix ".arrow" = 6
        if (fd < 0)
            return std::string("Cannot create temp file for --arrow -: ") +
                   std::strerror(errno);
        out_path = tmpl.data();
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
    } else {
        std::fprintf(stderr, "%s[%lld rows → %s, %s]%s\n",
                     g_color.meta_key, (long long)total,
                     cfg.arrow_out.c_str(), clabel, g_color.reset);
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
// Structured per-column statistics for one source column. Drains streaming
// sources and scans every value. Shares the per-value logic with the
// (multi-column, text-formatting) print_describe below; kept separate so the
// GUI gets a clean struct without the ASCII table.
ColStats compute_col_stats(TabularSource& src, int src_col) {
    ColStats cs;
    if (src_col < 0 || src_col >= src.schema()->num_fields()) return cs;
    auto f = src.schema()->field(src_col);
    cs.name = f->name();
    cs.type = f->type()->ToString();
    cs.is_numeric = is_numeric_type(f->type()->id());
    cs.valid = true;

    double dmin = std::numeric_limits<double>::infinity();
    double dmax = -std::numeric_limits<double>::infinity();
    long double sum = 0.0L;
    std::set<std::string> distinct;

    src.set_retain_all(true);  // re-reads every chunk after draining
    while (true) { int n = src.num_chunks(); src.ensure(n); if (src.num_chunks() == n) break; }
    for (int c = 0; c < src.num_chunks(); ++c) {
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, {src_col}, &tbl).ok() || !tbl) continue;
        auto col = tbl->column(0);
        for (auto& ch : col->chunks()) {
            int64_t n = ch->length();
            for (int64_t r = 0; r < n; ++r) {
                if (ch->IsNull(r)) { cs.nulls++; continue; }
                cs.count++;
                if (cs.is_numeric) {
                    double d;
                    if (!array_value_as_double(*ch, r, &d)) continue;
                    if (d < dmin) dmin = d;
                    if (d > dmax) dmax = d;
                    sum += d;
                } else {
                    std::string s = cell_to_string(*ch, r);
                    if (cs.count == 1 || s < cs.s_min) cs.s_min = s;
                    if (cs.count == 1 || s > cs.s_max) cs.s_max = s;
                    if (!cs.distinct_overflow) {
                        distinct.insert(s);
                        if (distinct.size() > 16) { cs.distinct_overflow = true; distinct.clear(); }
                    }
                }
            }
        }
    }
    if (cs.is_numeric && cs.count > 0) {
        cs.min = dmin; cs.max = dmax;
        cs.mean = (double)(sum / (long double)cs.count);
    }
    cs.distinct.assign(distinct.begin(), distinct.end());
    return cs;
}

// `Mean` only filled for numeric columns; `Distinct` only when small.
static std::string print_describe(TabularSource& src, const Config& cfg) {
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

    // --describe summarises the WHOLE table by default (Config::head_rows
    // defaults to 10, the table-view preview size — but a describe over only
    // the first 10 rows would be misleading). Honour -n only when the user set
    // it explicitly, matching the --json/--md/--parquet output paths.
    int64_t rows_left = (!cfg.head_rows_set || cfg.head_rows <= 0)
                        ? INT64_MAX : (int64_t)cfg.head_rows;
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
                        if (!array_value_as_double(*ch, r, &d)) continue;
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
                        cs.is_num ? "true" : "false",
                        (long long)cs.count, (long long)cs.nulls);
            if (cs.count == 0) {
                std::printf(",\"min\":null,\"max\":null");
                if (cs.is_num) std::printf(",\"mean\":null");
            } else if (cs.is_num) {
                std::printf(",\"min\":%s,\"max\":%s,\"mean\":%s",
                            jnum(cs.d_min).c_str(), jnum(cs.d_max).c_str(),
                            jnum((double)(cs.sum / (long double)cs.count)).c_str());
            } else {
                std::printf(",\"min\":"); json_emit_string(cs.s_min);
                std::printf(",\"max\":"); json_emit_string(cs.s_max);
            }
            if (!cs.is_num) {
                if (cs.distinct_overflow)
                    std::printf(",\"distinct\":null,\"distinct_overflow\":true");
                else
                    std::printf(",\"distinct\":%zu", cs.distinct.size());
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
// Everything from here through the end of TableTUI is the ncurses CLI
// frontend; excluded from libvvcore (the headless reader core).
#ifndef VV_CORE_LIB

// One run of a text line that shares a single attribute + colour.
struct AnsiRun {
    std::string text;
    attr_t      attr = A_NORMAL;
    int         fg   = -1;   // 256-colour index, -1 = terminal default
    int         bg   = -1;
    int         col0 = 0;    // display column where this run starts
    int         width = 0;   // display columns it occupies
};

// Split a line of a text file into paintable runs, interpreting SGR colour
// and dropping everything else.
//
// The pipe path is verbatim — `vv f.log > copy` round-trips byte for byte —
// but the screen path must not be. A .log can carry an OSC title sequence, a
// cursor-move, or a raw BEL, and ncurses would hand all of those straight to
// the terminal (same reasoning as md_no_osc_title_injection). Stripping only
// the ESC byte is not enough either: the tail of the sequence would show up
// as literal "]0;PWNED" / "[31m" garbage the user cannot tell from content.
// So a non-SGR escape is dropped WHOLE, and SGR becomes a real attribute —
// which is `less -R` behaviour, minus the parts that can drive the terminal.
//
// Tabs expand to the next 8-column stop, like less.
static std::vector<AnsiRun> ansi_runs(const std::string& in) {
    std::vector<AnsiRun> out;
    AnsiRun cur;
    int col = 0;
    auto flush = [&]() {
        if (!cur.text.empty()) { out.push_back(cur); cur.text.clear(); }
        cur.col0 = col; cur.width = 0;
    };
    auto set_style = [&](attr_t a, int fg, int bg) {
        if (a == cur.attr && fg == cur.fg && bg == cur.bg) return;
        flush();
        cur.attr = a; cur.fg = fg; cur.bg = bg;
    };
    attr_t attr = A_NORMAL;
    int fg = -1, bg = -1;

    for (size_t i = 0; i < in.size(); ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0x1b) {
            // CSI: ESC [ params final. Only 'm' (SGR) is honoured; every
            // other final byte (cursor moves, erases, scroll regions) is
            // dropped along with its parameters.
            if (i + 1 < in.size() && in[i+1] == '[') {
                size_t j = i + 2;
                while (j < in.size() &&
                       ((unsigned char)in[j] < 0x40 || (unsigned char)in[j] > 0x7e))
                    ++j;
                if (j < in.size() && in[j] == 'm') {
                    // Parse the SGR parameter list.
                    std::vector<int> ps;
                    int v = 0; bool any = false;
                    for (size_t k = i + 2; k < j; ++k) {
                        if (in[k] >= '0' && in[k] <= '9') { v = v*10 + (in[k]-'0'); any = true; }
                        else { ps.push_back(any ? v : 0); v = 0; any = false; }
                    }
                    ps.push_back(any ? v : 0);
                    for (size_t k = 0; k < ps.size(); ++k) {
                        int q = ps[k];
                        if (q == 0)                  { attr = A_NORMAL; fg = bg = -1; }
                        else if (q == 1)             attr |= A_BOLD;
                        else if (q == 2)             attr |= A_DIM;
                        else if (q == 4)             attr |= A_UNDERLINE;
                        else if (q == 7)             attr |= A_REVERSE;
                        else if (q == 22)            attr &= ~(attr_t)(A_BOLD | A_DIM);
                        else if (q == 24)            attr &= ~(attr_t)A_UNDERLINE;
                        else if (q == 27)            attr &= ~(attr_t)A_REVERSE;
                        else if (q >= 30 && q <= 37) fg = q - 30;
                        else if (q == 39)            fg = -1;
                        else if (q >= 40 && q <= 47) bg = q - 40;
                        else if (q == 49)            bg = -1;
                        else if (q >= 90 && q <= 97) fg = q - 90 + 8;
                        else if (q >= 100 && q <= 107) bg = q - 100 + 8;
                        else if (q == 38 || q == 48) {
                            int* slot = (q == 38) ? &fg : &bg;
                            if (k + 2 < ps.size() && ps[k+1] == 5) {
                                *slot = ps[k+2]; k += 2;
                            } else if (k + 4 < ps.size() && ps[k+1] == 2) {
                                *slot = nearest_256(ps[k+2], ps[k+3], ps[k+4]);
                                k += 4;
                            }
                        }
                    }
                    set_style(attr, fg, bg);
                }
                i = (j < in.size()) ? j + 1 : in.size();
                continue;
            }
            // OSC: ESC ] ... terminated by BEL or ST (ESC \). This is the
            // window-title injection vector; drop the whole thing.
            if (i + 1 < in.size() && in[i+1] == ']') {
                size_t j = i + 2;
                while (j < in.size() && (unsigned char)in[j] != 0x07 &&
                       !(in[j] == 0x1b && j + 1 < in.size() && in[j+1] == '\\'))
                    ++j;
                if (j < in.size() && in[j] == 0x1b) ++j;
                i = (j < in.size()) ? j + 1 : in.size();
                continue;
            }
            // Any other two-byte escape (charset select, ESC 7/8, …).
            i += (i + 1 < in.size()) ? 2 : 1;
            continue;
        }
        if (c == '\t') {
            int n = 8 - (col % 8);
            cur.text.append((size_t)n, ' ');
            cur.width += n; col += n; ++i;
            continue;
        }
        if (c < 0x20 || c == 0x7f) { ++i; continue; }   // BEL, DEL, stray C0
        int len = 1;
        int cw = codepoint_width(utf8_decode(in, i, &len));
        cur.text.append(in, i, (size_t)len);
        cur.width += cw; col += cw;
        i += (size_t)len;
    }
    if (!cur.text.empty()) out.push_back(cur);
    return out;
}


// Terminal restoration on fatal signals. While the TUI owns the terminal
// (raw/no-echo/alt-screen), a SIGINT (Ctrl-C), SIGTERM or SIGHUP would kill the
// process before endwin() runs, leaving the user's shell unusable. The handler
// runs endwin() then re-raises with the default disposition so the exit status
// still reflects the signal (130 for SIGINT, etc.).
//
// endwin() is not async-signal-safe (it allocates/frees) — if the signal lands
// while the program is inside malloc (e.g. mid-draw()), running endwin() from
// the handler re-enters the allocator and aborts the process. run() therefore
// BLOCKS these signals around everything except the blocking getch() call, so
// delivery (and thus endwin) can only happen while parked in read() — never
// mid-allocation. getch() is reliably interrupted there, which the
// set-a-flag-and-poll approach can't guarantee (ncurses restarts on EINTR).
static volatile sig_atomic_t g_tui_active = 0;
static void tui_signal_restore(int sig) {
    if (g_tui_active) { g_tui_active = 0; endwin(); }
    signal(sig, SIG_DFL);
    raise(sig);
}

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

class TableTUI {
    // Multiple files become tabs. `src_` always points at the currently
    // active tab's source; `sources_` owns them. The snapshot vector
    // (`tabs_`, declared further down) saves per-tab view state — sort,
    // filter, scroll position, column visibility, etc. — across switches.
    std::vector<std::unique_ptr<TabularSource>> sources_;
    TabularSource* src_;
    int   active_tab_ = 0;
    int   num_cols_   = 0;
    int   max_col_w_;
    bool  no_index_;
    int   max_cols_cfg_;          // remembered from cfg.max_cols

    // SearchMode lives at the top of the class so per-tab snapshots (and
    // FilterMode below) can reference it. The state fields themselves are
    // declared further down.
    enum class SearchMode { None, Input, Active };

    std::vector<std::string> col_names_;
    std::vector<std::string> col_types_str_;  // short label rendered under the column name
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

    // Colour pairs for SGR-coloured text lines. Distinct from get_rgb_pair(),
    // which allocates BACKGROUND swatches for RGB columns; this one sets the
    // foreground (and optionally the background) the way a coloured log means
    // it. Bounded by COLOR_PAIRS, and shared with the RGB allocator's counter
    // so the two cannot collide.
    std::map<int, int> fg_pair_;
    int get_fg_pair(int fg, int bg) {
        if (next_rgb_pair_ >= COLOR_PAIRS) return 0;
        int key = ((fg + 1) << 9) | (bg + 1);
        auto it = fg_pair_.find(key);
        if (it != fg_pair_.end()) return it->second;
        int pair = next_rgb_pair_++;
        init_pair(pair, (short)fg, (short)bg);
        fg_pair_[key] = pair;
        return pair;
    }

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

    // Per-frame formatted-cell memo, keyed by frame_key(source_row, virt_col).
    // The width-fitting pass and the render pass both want the formatted text
    // of every visible integer cell; without this they'd each format it once
    // (integer columns — Start/End genomic positions — are the common case).
    // Cleared at the top of every draw(); bounded by the visible cell count.
    std::unordered_map<int64_t, std::string> frame_cells_;
    int64_t frame_key(int64_t srow, int vc) const {
        return srow * (int64_t)num_cols_ + vc;
    }

    int64_t top_row_  = 0;
    int     left_col_ = 0;
    // Cell cursor. top_row_/left_col_ are the viewport; these are where the
    // user is. Every per-cell action (S, s, y, Enter, , / .) used to read the
    // top-left corner instead, which is why the help text had to say "the
    // leftmost visible column". cur_row_ is a DISPLAY row (like top_row_, so
    // it survives sort/filter); cur_col_ is a VIRTUAL column (like left_col_).
    // Plain text: the active source is one utf8 column of lines, rendered as
    // a document — no header row, no truncation, chopped at the screen edge
    // with h/l scrolling sideways (less -S). Per-tab, so a text file and a
    // Parquet file can sit side by side in the same tab strip.
    bool    text_view_ = false;
    int     hscroll_   = 0;    // first displayed column of the line, text only
    int64_t cur_row_  = 0;
    int     cur_col_  = 0;
    // Rows to keep between the cursor and the top/bottom edge while scrolling,
    // à la vim's 'scrolloff'. Overridable from the config file.
    int     scrolloff_ = 3;
    int     scr_r_ = 24, scr_c_ = 80;
    bool    freeze_first_col_ = false;   // toggle with `z` — keep col 0 pinned left
    bool    help_open_        = false;   // overlay shown via `?` / F1 / H

    // ── Stats popup (`S` over the column under the cursor) ───────────────────
    struct TuiColStat {
        std::string name;
        std::string type;
        bool        is_num = false;
        int64_t     count  = 0;
        int64_t     nulls  = 0;
        double      d_min  = std::numeric_limits<double>::infinity();
        double      d_max  = -std::numeric_limits<double>::infinity();
        long double sum    = 0.0L;
        std::string s_min, s_max;
        std::set<std::string> distinct;
        bool        distinct_overflow = false;
    };
    bool                       stats_open_   = false;
    int                        stats_col_    = -1;   // virtual column index
    std::optional<TuiColStat>  stats_data_;

    // ── Column show/hide picker (`c`) ────────────────────────────────────────
    bool                       col_picker_open_     = false;
    int                        col_picker_cursor_   = 0;
    std::vector<bool>          col_visible_;        // [virt_col] — true = shown

    // ── Theme picker (`T`) ───────────────────────────────────────────────────
    bool                       theme_picker_open_   = false;
    int                        theme_picker_cursor_ = 0;

    // ── Multi-file tabs (`Tab` / `Shift+Tab`) ────────────────────────────────
    //
    // Each tab owns one TabularSource (via sources_) plus a snapshot of the
    // per-file view state (sort, filter, scroll position, column metadata).
    // The TableTUI's "live" member fields are always the active tab's
    // values; switching tabs swaps the live fields with another snapshot.
    struct TabState {
        std::string                     path;          // file path
        std::string                     label;         // short label for the tab bar
        bool                            initialised = false;
        // Column metadata (built once per source by setup_for_active_source).
        int                             num_cols = 0;
        int                             src_num_cols = 0;
        int                             idx_w = 1;
        std::vector<std::string>        col_names;
        std::vector<std::string>        col_types_str;
        std::vector<int>                col_widths;
        std::vector<bool>               right_align, is_bool, is_rgb, is_integer;
        std::vector<int>                virt_src_col;
        std::vector<std::string>        virt_info_key;
        std::vector<bool>               col_visible;
        // View state
        int64_t                         top_row = 0;
        int                             left_col = 0;
        int64_t                         cur_row = 0;    // cell cursor
        int                             cur_col = 0;
        bool                            freeze_first_col = false;
        // Chunk LRU cache
        std::map<int, CachedRG>         cache;
        std::list<int>                  lru;
        // Search state (per-tab so each file has its own match position).
        SearchMode                      search_mode = SearchMode::None;
        std::string                     search_query;
        int64_t                         search_row = -1;
        bool                            search_wrap = false;
        bool                            search_fail = false;
        bool                            search_dir_forward = true;
        // Sort + filter (per-tab — share the display→source indirection).
        int                             sort_col = -1;
        bool                            sort_desc = false;
        std::vector<int64_t>            sort_order;
        bool                            filter_active = false;
        std::string                     filter_expr_str;
        FilterExpr                      filter_fx;
        int64_t                         filter_total = 0;
        // Sticky: a full-file pass (search / sort / filter / stats) ran after
        // the streaming source had already released batches, so its answer
        // covers only part of the file.
        bool                            partial_pass = false;
    };
    std::vector<TabState>      tabs_;

    // ── Sort by column (`s`) + Live filter (`&`) ─────────────────────────────
    //
    // Both features funnel through the same display→source indirection.
    // `sort_order_[display_row] = source_row` when non-empty; empty means
    // identity (no sort, no filter). The presence of sort_order_ also flips
    // total_rows() over to the filtered/sorted count so the status bar,
    // scroll clamping, and search loops all see the user's view-of-the-data.
    std::vector<int64_t>       sort_order_;
    int                        sort_col_    = -1;   // virtual column, -1 = no sort
    bool                       sort_desc_   = false;

    // Live filter state (`&`).
    enum class FilterMode { None, Input };
    FilterMode                 filter_mode_   = FilterMode::None;  // input bar state
    std::string                filter_input_;        // text being typed
    std::string                filter_expr_str_;     // committed expression
    FilterExpr                 filter_fx_;           // compiled, valid when active
    bool                       filter_active_ = false;
    std::string                filter_err_;          // last compile error, if any
    int64_t                    filter_total_   = 0;  // rows after filter (for status)
    // See TabState::partial_pass. Set by note_full_pass(), shown in the status
    // bar and the stats overlay until the tab is closed.
    bool                       partial_pass_   = false;

    // Copy-cell (`y`): transient status shown on the bottom bar after a copy.
    // Cleared automatically on the next non-`y` keypress.
    std::string                copy_status_;

    // Command-line (`:`): vim-style typed-command prompt at the bottom.
    //   :<N>              jump to row N (0-based, matches the index column)
    //   :q  / :quit       quit
    //   :theme NAME       switch theme (same names as --theme / T overlay)
    // Parse errors stay in the bar so the user can edit + retry.
    enum class CmdMode { None, Input };
    CmdMode                    cmd_mode_   = CmdMode::None;
    std::string                cmd_input_;          // text being typed
    std::string                cmd_err_;            // last parse error

    // Translate a display-row index to the underlying source-row when a
    // sort or filter is active. All cache lookups, search row scans, and
    // load_full_row calls go through this helper so the rest of the TUI
    // can stay row-indexing-agnostic.
    int64_t source_row(int64_t display) const {
        if (sort_order_.empty()) return display;
        if (display < 0 || display >= (int64_t)sort_order_.size()) return display;
        return sort_order_[display];
    }

    // ── Search state ─────────────────────────────────────────────────────────
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

    // Column-oriented keys that a text tab has nothing to do with. Answering
    // on the status bar rather than silently ignoring the press is the same
    // rule as #86's exit codes: never let a key look like it worked.
    static std::string TEXT_NA(const char* key) {
        return std::string(key) + ": not available for a text file "
               "(it has one column of lines)";
    }
    static constexpr int HDR_H = 3;   // column-name row + type row + rule
    static constexpr int FTR_H = 1;   // status bar
    // Banner row (row 0) + tab-bar row, both above the column header and
    // recomputed at the top of draw(). The banner is the active source's
    // top_banner() (LociSSD assembly/species/count); the tab bar appears only
    // with more than one tab. Vertical layout stacks: banner, tabs, header, data.
    int                  banner_h_ = 0;
    int                  tabbar_h_ = 0;
    // A text tab has no column-name / type / rule rows: `line / string` is
    // meaningless furniture over a document, and the three rows are three
    // fewer lines of the file.
    int hdr_h()         const { return text_view_ ? 0 : HDR_H; }
    // Horizontal scroll step for a text tab: half the text area, so `l` moves
    // by a useful amount on a 200-column log line without overshooting.
    int text_hstep()    const {
        int avail = scr_c_ - (no_index_ ? 0 : idx_w_ + 2);
        return std::max(1, avail / 2);
    }
    int data_top_y()    const { return banner_h_ + tabbar_h_ + hdr_h(); }
    int data_lines() const {
        return std::max(0, scr_r_ - hdr_h() - tabbar_h_ - banner_h_ - FTR_H);
    }

    // When a sort or filter is active the visible row count is the size of
    // sort_order_, not the underlying source size. Search wrap-around, the
    // status bar's row range, and scroll clamping all depend on this.
    int64_t total_rows() const {
        if (!sort_order_.empty()) return (int64_t)sort_order_.size();
        return src_->total_rows();
    }
    int     num_chunks()  const { return src_->num_chunks(); }

    // ── Search ───────────────────────────────────────────────────────────────

    // Search forward (forward=true) or backward through all loaded chunks.
    // Returns absolute row index of first match >= from_row (forward) or
    // <= from_row (backward), or -1 if not found.
    // Shows "Searching…" in the status line while scanning large files.
    int64_t find_next(int64_t from_row, bool forward) {
        if (search_query_.empty()) return -1;
        src_->set_retain_all(true);  // search re-reads every chunk: keep them
        drain_to_eof();   // search must cover the whole streaming file
        note_full_pass();
        std::string q = search_query_;
        for (auto& c : q) c = (char)std::tolower((unsigned char)c);

        std::vector<int> all_cols;
        for (int i = 0; i < src_num_cols_; ++i) all_cols.push_back(i);

        int nc = src_->num_chunks();

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

        // Sort active: scan display rows one at a time, translating each to
        // its source row via sort_order_. Consecutive display rows can land in
        // different row groups, so route every lookup through the shared LRU
        // chunk cache (ensure_cols / cache_) rather than a single last-chunk
        // slot — a sort that bounces between a handful of row groups then
        // re-decodes each at most once per eviction, not once per row.
        if (!sort_order_.empty()) {
            int64_t N = (int64_t)sort_order_.size();
            int64_t r = from_row;
            int64_t end = forward ? N : -1;
            int64_t step = forward ? +1 : -1;
            int64_t scanned = 0;
            for (; r != end; r += step) {
                if (r < 0 || r >= N) break;
                // Throttle the progress repaint: the decode is now cached, so
                // an un-throttled refresh per row would dominate the scan.
                if ((scanned++ & 8191) == 0) {
                    mvprintw(scr_r_-1, 0, " Searching (sorted)… row %lld ",
                             (long long)r);
                    clrtoeol(); refresh();
                }
                int64_t srow = sort_order_[r];
                int c = chunk_for_row(srow);
                ensure_cols(c, all_cols);
                auto it = cache_.find(c);
                if (it == cache_.end() || !it->second.ok) continue;
                const CachedRG& cr = it->second;
                int64_t local = srow - cr.first_row;
                if (local < 0 || local >= cr.num_rows) continue;
                if (cached_row_matches(cr, local, q)) return r;
            }
            return -1;
        }

        if (forward) {
            for (int c = 0; c < nc; ++c) {
                auto meta = src_->chunk_meta(c);
                if (meta.first_row + meta.num_rows <= from_row) continue;
                // Show progress for slow sources
                mvprintw(scr_r_-1, 0, " Searching… chunk %d/%d ", c+1, nc);
                clrtoeol(); refresh();
                std::shared_ptr<arrow::Table> tbl;
                if (!src_->read_chunk(c, all_cols, &tbl).ok()) continue;
                int64_t start = std::max<int64_t>(0, from_row - meta.first_row);
                for (int64_t r = start; r < tbl->num_rows(); ++r)
                    if (row_matches(tbl, r)) return meta.first_row + r;
            }
        } else {
            for (int c = nc - 1; c >= 0; --c) {
                auto meta = src_->chunk_meta(c);
                if (meta.first_row > from_row) continue;
                mvprintw(scr_r_-1, 0, " Searching… chunk %d/%d ", c+1, nc);
                clrtoeol(); refresh();
                std::shared_ptr<arrow::Table> tbl;
                if (!src_->read_chunk(c, all_cols, &tbl).ok()) continue;
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
            int64_t wrap_start = forward ? 0 : (total_rows() >= 0 ? total_rows()-1 : src_->chunk_meta(num_chunks()-1).first_row + src_->chunk_meta(num_chunks()-1).num_rows - 1);
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

    // True if any column of cached chunk `cr` matches the lowercased query `q`
    // at row offset `local`. Shared by the sorted find scan and the
    // highlight check, so both consult the same decoded row groups.
    bool cached_row_matches(const CachedRG& cr, int64_t local,
                            const std::string& q) const {
        for (auto& arr : cr.cols) {
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

    // True if `row` matches the active search, *without* loading new chunks.
    // Used by draw_data_row to highlight every visible match — only consults
    // already-cached cells. Returns false if the row's chunk isn't loaded.
    bool row_matches_search(int64_t row) const {
        if (search_mode_ != SearchMode::Active || search_query_.empty()) return false;
        if (src_->num_chunks() == 0) return false;
        int64_t srow = source_row(row);
        int c = chunk_for_row(srow);
        auto it = cache_.find(c);
        if (it == cache_.end() || !it->second.ok) return false;
        const CachedRG& cr = it->second;
        int64_t local = srow - cr.first_row;
        if (local < 0 || local >= cr.num_rows) return false;
        std::string q = search_query_;
        for (auto& c2 : q) c2 = (char)std::tolower((unsigned char)c2);
        return cached_row_matches(cr, local, q);
    }

    // ── Cache ────────────────────────────────────────────────────────────────

    // Ensure cache entry for chunk `c` exists and has all requested source
    // columns loaded.  A single read_chunk() call fetches whatever's missing.
    // Ensure cache entry for chunk `c` exists and has all requested source
    // columns loaded. A single read_chunk() call fetches whatever's missing.
    //
    // `need_rows = -1` (default) means "decode the entire chunk". A positive
    // value caps the decode at the first N rows from the start of the chunk
    // — only useful for chunk 0, where `ParquetSource::read_first()` has a
    // fast `GetRecordBatchReader`-based path that skips decoding the rest of
    // the row group. The TUI passes the visible-window size plus a buffer
    // for its first paint, so opening a multi-GB parquet with ~64K rows in
    // its first row group draws in ~100 ms instead of the ~10 s the full
    // row-group decode used to take. When the user later scrolls past the
    // partial window, ensure_cols upgrades the entry by re-decoding every
    // previously-loaded column at the full row-group size.
    void ensure_cols(int c, const std::vector<int>& src_cols,
                      int64_t need_rows = -1) {
        const int64_t chunk_total = src_->chunk_meta(c).num_rows;
        const int64_t target =
            (need_rows < 0 || need_rows > chunk_total) ? chunk_total : need_rows;

        auto it = cache_.find(c);
        if (it == cache_.end()) {
            if ((int)cache_.size() >= MAX_CACHE) {
                cache_.erase(lru_.back()); lru_.pop_back();
            }
            CachedRG cr;
            cr.first_row = src_->chunk_meta(c).first_row;
            cr.num_rows  = 0;        // nothing loaded yet
            cr.cols.assign(src_num_cols_, nullptr);
            it = cache_.emplace(c, std::move(cr)).first;
            lru_.push_front(c);
        } else {
            lru_.remove(c); lru_.push_front(c);
        }
        CachedRG& cr = it->second;

        // Columns to (re)load: never-loaded, OR previously loaded but with
        // fewer rows than the new target. When target > cr.num_rows we have
        // to also re-decode every column that's already in the cache, or we'd
        // end up with a chunk whose columns have mismatched row counts.
        std::vector<int> need;
        for (int sc : src_cols) {
            if (sc < 0 || sc >= src_num_cols_) continue;
            if (!cr.cols[sc] ||
                (int64_t)cr.cols[sc]->length() < target)
                need.push_back(sc);
        }
        if (target > cr.num_rows) {
            for (int sc = 0; sc < src_num_cols_; ++sc) {
                if (!cr.cols[sc]) continue;
                if ((int64_t)cr.cols[sc]->length() >= target) continue;
                if (std::find(need.begin(), need.end(), sc) == need.end())
                    need.push_back(sc);
            }
        }
        if (need.empty()) return;

        src_->ensure(c);
        std::shared_ptr<arrow::Table> tbl;
        // Fast path: for chunk 0 we have a slice-read that decodes only the
        // first N rows of the row group. read_first falls back to read_chunk
        // for non-Parquet sources, so this is safe regardless of format.
        if (c == 0 && target < chunk_total) {
            if (!src_->read_first(target, need, &tbl).ok()) return;
        } else {
            if (!src_->read_chunk(c, need, &tbl).ok()) return;
        }
        cr.ok       = true;
        cr.num_rows = std::max<int64_t>(cr.num_rows,
                                          tbl ? tbl->num_rows() : 0);
        for (size_t i = 0; i < need.size() && (int)i < tbl->num_columns(); ++i) {
            int sc = need[i];
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
                std::string formatted = src_->format_cell(sc, std::move(val));
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
        int lo = 0, hi = src_->num_chunks() - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (src_->chunk_meta(mid).first_row <= r) lo = mid; else hi = mid - 1;
        }
        return lo;
    }

    // Prefetch the source columns the visible virtual columns need, for the
    // chunks that currently intersect the viewport.  Cheap when already cached.
    // Fit each visible virtual column's width to the rows currently on screen.
    // Called after prefetch so cached chunks are available; columns whose data
    // is not yet loaded keep their previous width.
    //
    // Integer columns are fitted without an upper bound — digits must stay
    // readable, and cell_at() returns them untruncated. Every other type is
    // clamped to max_col_w_ (`-w`, default 32), because a long string does
    // legitimately need truncating; cell_at() has already applied that, so the
    // measurement can't exceed it anyway. The floor is the source's own
    // min_col_width(), so formats that ask for a minimum (LociSSD) still get
    // it and a column never collapses below its header.
    //
    // Before this, non-integer columns kept the type-based guess made at
    // setup time (string -> 12, float -> 8, list -> 14) forever, so a `name`
    // column holding 2-character values sat at 12 and a `val` column of `0.5`
    // at 8 — on a wide table that is most of the screen.
    void fit_widths_to_visible(const std::vector<int>& visible_virt_cols) {
        if (src_->num_chunks() == 0) return;
        int64_t bot = top_row_ + (int64_t)data_lines() - 1;
        if (total_rows() > 0) bot = std::min(bot, total_rows() - 1);
        if (bot < top_row_) return;
        for (int vc : visible_virt_cols) {
            if (vc < 0 || vc >= num_cols_) continue;
            // The header block is three rows — name, type, rule — and the type
            // row is drawn at the same width. Fitting to the data alone made
            // `int64` render as `i…`, so the type string is a floor too. Long
            // ones (list<element: string>) are still truncated, as before; the
            // max_col_w_ clamp below bounds this.
            int w = (int)display_width(col_names_[vc]);
            // Capped at 14 — the allowance this code already used as the
            // list-type minimum. That covers every scalar type name (`int64`,
            // `double`, `timestamp[us]`) without letting a verbose nested type
            // (`list<element: string>`, 21) widen a column past what its data
            // needs; those were truncated in the type row before this change
            // too.
            if (vc < (int)col_types_str_.size())
                w = std::max(w, std::min(14, (int)display_width(col_types_str_[vc])));
            for (int64_t r = top_row_; r <= bot; ++r) {
                int64_t srow = source_row(r);
                int c = chunk_for_row(srow);
                auto it = cache_.find(c);
                if (it == cache_.end() || !it->second.ok) continue;
                const CachedRG& cr = it->second;
                int64_t local = srow - cr.first_row;
                if (local < 0 || local >= cr.num_rows) continue;
                // Format the cell through the same path draw_data_row uses and
                // memoize it for the render pass — cell_at returns integers
                // untruncated, so this is the true display width and matches
                // what's painted (format_cell included). Nulls render as the
                // null glyph; skip them from the width like the old code did.
                std::string val = cell_at(cr, local, vc, nullptr, nullptr, 0);
                frame_cells_[frame_key(srow, vc)] = val;
                if (val != NULL_SYMBOL) {
                    int ww = display_width(val);
                    if (ww > w) w = ww;
                }
            }
            if (!is_integer_[vc]) {
                int sc = virt_src_col_[vc];
                int floor_w = (sc >= 0) ? src_->min_col_width(sc) : 4;
                w = std::max(w, floor_w);
                w = std::min(w, max_col_w_);
            }
            col_widths_[vc] = w;
        }
    }

    void prefetch_visible(const std::vector<int>& visible_virt_cols) {
        if (src_->num_chunks() == 0) { src_->ensure(0); return; }
        std::vector<int> src_cols = src_cols_for_virt(visible_virt_cols);
        int top_chunk = chunk_for_row(top_row_);
        // First paint of a Parquet file: only need rows within (and just
        // past) the visible window. ensure_cols's read_first fast path
        // skips decoding the rest of the row group. The +256 gives the
        // user some scrolling headroom before we have to upgrade to a
        // full row-group decode.
        int64_t top_local = top_row_ -
            src_->chunk_meta(top_chunk).first_row;
        int64_t need = top_local + (int64_t)data_lines() + 256;
        ensure_cols(top_chunk, src_cols, need);
        int64_t bot = top_row_ + (int64_t)data_lines() - 1;
        if (total_rows() > 0) bot = std::min(bot, total_rows() - 1);
        if (total_rows() < 0)
            src_->ensure(src_->num_chunks());
        else
            src_->ensure(chunk_for_row(std::max(bot, top_row_)));
        if (bot > top_row_) {
            int bot_chunk = chunk_for_row(bot);
            if (bot_chunk != top_chunk) ensure_cols(bot_chunk, src_cols);
        }
    }

    // ── Layout ───────────────────────────────────────────────────────────────

    struct ColVis { int col, x, w; };

    bool col_is_visible(int c) const {
        return c >= 0 && c < (int)col_visible_.size() ? col_visible_[c] : true;
    }

    std::vector<ColVis> visible_cols() const {
        std::vector<ColVis> v;
        int x = no_index_ ? 0 : (idx_w_ + 2);
        // Frozen first column: pin col 0 at the left edge whenever the user
        // has scrolled past it. Skipped when col 0 is already in the natural
        // window (left_col_ == 0) — that path renders column 0 normally.
        int start = left_col_;
        if (freeze_first_col_ && num_cols_ > 0 && left_col_ > 0 && col_is_visible(0)) {
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
            if (!col_is_visible(c)) continue;            // hidden by user (`c` picker)
            int w = col_widths_[c];
            if (x + w + 2 > scr_c_) break;
            v.push_back({c, x, w});
            x += w + 2;
        }
        // A single column wider than the screen makes the loop break on its first
        // candidate, leaving `v` empty → a completely blank table. Force-emit that
        // first column, clamped to the remaining width, so there's always content.
        if (v.empty()) {
            for (int c = start; c < num_cols_; ++c) {
                if (freeze_first_col_ && c == 0) continue;
                if (!col_is_visible(c)) continue;
                int avail = scr_c_ - x - 2;
                if (avail < 1) avail = 1;
                v.push_back({c, x, std::min(col_widths_[c], avail)});
                break;
            }
        }
        return v;
    }

    // Nearest column at or after `c` that the user hasn't hidden; falls back
    // to searching backwards, then to c itself.
    int next_visible_col(int c, int dir) const {
        if (num_cols_ <= 0) return 0;
        for (int i = c; i >= 0 && i < num_cols_; i += dir)
            if (col_is_visible(i)) return i;
        for (int i = c; i >= 0 && i < num_cols_; i -= dir)
            if (col_is_visible(i)) return i;
        return c;
    }

    // Move the viewport so the cursor is on screen. Called from draw() AFTER
    // top_row_ has been clamped and BEFORE the first visible_cols(), so the
    // frame that gets painted already reflects the cursor.
    void ensure_cursor_visible() {
        // ── Rows ────────────────────────────────────────────────────────────
        int64_t tr = total_rows();
        if (cur_row_ < 0) cur_row_ = 0;
        if (tr >= 0 && cur_row_ > tr - 1) cur_row_ = std::max<int64_t>(0, tr - 1);
        int dl = data_lines();
        if (dl > 0) {
            // Clamp scrolloff so it can't exceed half the viewport (otherwise
            // the two bounds cross and the row oscillates).
            int so = std::min(scrolloff_, (dl - 1) / 2);
            if (so < 0) so = 0;
            if (cur_row_ < top_row_ + so)          top_row_ = cur_row_ - so;
            if (cur_row_ > top_row_ + dl - 1 - so) top_row_ = cur_row_ - dl + 1 + so;
            if (top_row_ < 0) top_row_ = 0;
            if (tr >= 0) {
                int64_t mt = std::max<int64_t>(0, tr - dl);
                if (top_row_ > mt) top_row_ = mt;
            }
        }

        // ── Columns ─────────────────────────────────────────────────────────
        if (num_cols_ <= 0) return;
        if (cur_col_ < 0) cur_col_ = 0;
        if (cur_col_ >= num_cols_) cur_col_ = num_cols_ - 1;
        if (!col_is_visible(cur_col_)) cur_col_ = next_visible_col(cur_col_, +1);
        if (cur_col_ < left_col_) left_col_ = cur_col_;
        // Scroll right until the cursor column is in the rendered set. The
        // bound is what makes this safe: visible_cols() special-cases the
        // frozen column and can force-emit a single clamped column when
        // nothing fits, so "is it visible yet?" is not guaranteed monotonic —
        // without the counter this loop could spin forever and hang the TUI.
        for (int guard = 0; guard <= num_cols_; ++guard) {
            bool on_screen = false;
            for (const auto& cv : visible_cols())
                if (cv.col == cur_col_) { on_screen = true; break; }
            if (on_screen || left_col_ >= cur_col_) break;
            ++left_col_;
        }
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

    // Hit zones for the multi-tab tab bar; rebuilt every draw() so that
    // mouse clicks land on the correct tab even after window resizes.
    struct TabHit { int x0; int x1; int idx; };
    std::vector<TabHit>        tab_hit_zones_;

    // Browser-style tab bar on row 0 (only when more than one tab is open).
    // The active tab is rendered in reverse video so it "pops" out of the
    // bar; inactive tabs are dimmed. If the bar overflows, we scroll so
    // the active tab is always visible and append "›" / "‹" markers.
    // Row 0: the active source's top banner (LociSSD assembly/species/count),
    // bold and padded across the width. banner_h_ reserves the row in draw().
    void draw_banner() {
        if (banner_h_ == 0) return;
        std::string b = src_ ? src_->top_banner() : "";
        if ((int)display_width(b) > scr_c_) b = truncate(b, scr_c_);
        int pad = scr_c_ - (int)display_width(b);
        if (pad > 0) b += std::string(pad, ' ');
        nc_str(0, 0, b, A_BOLD, NCP_HEADER);
    }

    void draw_tabbar() {
        tab_hit_zones_.clear();
        if (tabbar_h_ == 0) return;

        const int row = banner_h_;   // sits just below the banner row (if any)
        const std::string hint = " [Tab] next  [⇧Tab] prev ";
        const int hint_w = (int)display_width(hint);

        // Available width for the tabs themselves (leave room for the hint
        // on the right if it fits; otherwise drop the hint and use the full width).
        int max_tabs_w = scr_c_ - 1 - (hint_w + 1);
        if (max_tabs_w < 12) max_tabs_w = scr_c_;  // hint hidden — too narrow

        // Render each tab as " label " with a thin separator between.
        // Build the full string first so we can scroll horizontally.
        struct Rendered { std::string text; int idx; bool active; };
        std::vector<Rendered> rendered;
        rendered.reserve(tabs_.size());
        for (int i = 0; i < (int)tabs_.size(); ++i) {
            std::string lab = tabs_[i].label.empty()
                ? "(unnamed)" : tabs_[i].label;
            // Cap individual tab labels so a single very long sheet name
            // doesn't push every other tab off-screen.
            if ((int)display_width(lab) > 24) {
                while ((int)display_width(lab) > 21) lab.pop_back();
                lab += "…";
            }
            std::string txt = " " + lab + " ";
            rendered.push_back({std::move(txt), i, i == active_tab_});
        }

        // Compute each tab's x range in an imaginary infinite-width bar.
        std::vector<int> starts(rendered.size()+1, 0);
        for (size_t i = 0; i < rendered.size(); ++i) {
            starts[i+1] = starts[i] + (int)display_width(rendered[i].text);
            if (i + 1 < rendered.size()) starts[i+1] += 1;  // separator
        }
        int total_w = starts.back();

        // Scroll so the active tab is fully visible.
        int scroll = 0;
        if (total_w > max_tabs_w) {
            int aL = starts[active_tab_];
            int aR = starts[active_tab_+1];
            // Push left edge so active tab fits in [scroll, scroll+max_tabs_w].
            if (aR - scroll > max_tabs_w) scroll = aR - max_tabs_w;
            if (aL < scroll)               scroll = aL;
            if (scroll < 0) scroll = 0;
        }

        // Paint background of the bar so unfilled space picks up theme bg.
        nc_str(row, 0, std::string(scr_c_, ' '), A_NORMAL, NCP_INDEX);

        // Emit each tab, clipped to the visible window.
        int win_x0 = scroll;
        int win_x1 = scroll + max_tabs_w;
        for (size_t i = 0; i < rendered.size(); ++i) {
            int tab_L = starts[i];
            int tab_R = starts[i+1];
            if (tab_R <= win_x0 || tab_L >= win_x1) continue;  // off-screen
            // Translate to screen x.
            int sx = tab_L - scroll;
            // Clip on the right if the tab is partially visible.
            std::string txt = rendered[i].text;
            int vis_w = std::min(tab_R, win_x1) - std::max(tab_L, win_x0);
            // If clipped at the start, drop leading chars.
            if (tab_L < win_x0) {
                int drop = win_x0 - tab_L;
                while (drop-- > 0 && !txt.empty()) txt.erase(txt.begin());
                sx = 0;
            }
            // If clipped at the right, drop trailing chars.
            if ((int)display_width(txt) > vis_w) {
                while ((int)display_width(txt) > vis_w && !txt.empty())
                    txt.pop_back();
            }
            attr_t a; int cp;
            if (rendered[i].active) {
                a  = (attr_t)(A_BOLD | A_REVERSE);
                cp = NCP_HEADER;
            } else {
                a  = A_DIM;
                cp = NCP_HEADER;
            }
            nc_str(row, sx, txt, a, cp);
            tab_hit_zones_.push_back({sx, sx + (int)display_width(txt),
                                       rendered[i].idx});
            // Separator between tabs.
            int sep_x = sx + (int)display_width(txt);
            if (i + 1 < rendered.size() && sep_x < max_tabs_w) {
                nc_str(row, sep_x, "│", A_DIM, NCP_SEP);
            }
        }

        // Scroll arrows when the bar overflows.
        if (scroll > 0)
            nc_str(row, 0, "‹", A_DIM, NCP_SEP);
        if (total_w - scroll > max_tabs_w && max_tabs_w >= 1)
            nc_str(row, max_tabs_w - 1, "›", A_DIM, NCP_SEP);

        // Hint on the right.
        if (max_tabs_w < scr_c_) {
            nc_str(row, scr_c_ - hint_w, hint, A_DIM, NCP_INDEX);
        }
    }

    void draw_header(const std::vector<ColVis>& vc) {
        const int y_names = banner_h_ + tabbar_h_;
        const int y_types = banner_h_ + tabbar_h_ + 1;
        const int y_rule  = banner_h_ + tabbar_h_ + 2;
        if (!no_index_) {
            std::string idx_pad = " " + std::string(idx_w_, ' ') + " ";
            nc_str(y_names, 0, idx_pad, A_BOLD, NCP_INDEX);
            nc_str(y_types, 0, idx_pad, A_NORMAL, NCP_INDEX);
            nc_str(y_rule,  0, " " + repeat_utf8(BOX_HLINE, idx_w_) + " ", A_NORMAL, NCP_SEP);
        }
        for (auto& col : vc) {
            std::string nm = truncate(col_names_[col.col], col.w);
            nc_str(y_names, col.x, " " + fit(nm, col.w, right_align_[col.col]) + " ",
                   A_BOLD, NCP_HEADER);
            std::string ty = (col.col < (int)col_types_str_.size())
                             ? col_types_str_[col.col] : std::string();
            ty = truncate(ty, col.w);
            nc_str(y_types, col.x, " " + fit(ty, col.w, right_align_[col.col]) + " ",
                   A_DIM, NCP_HEADER);
            nc_str(y_rule, col.x, " " + repeat_utf8(BOX_HLINE, col.w) + " ",
                   A_NORMAL, NCP_SEP);
        }
    }

    // Paint one line of a text tab. `hscroll_` is a horizontal offset in
    // DISPLAY COLUMNS, not bytes, so a line of CJK scrolls by what the user
    // sees; sub_display() does the UTF-8 walk.
    //
    // Control bytes are stripped rather than passed through: a .log can carry
    // an OSC title sequence or a raw ESC, and ncurses would hand those to the
    // terminal. The pipe path (emit_text_stream) is verbatim; the screen path
    // is sanitised. Tabs expand to the next multiple of 8, like less.
    void draw_text_row(int sy, const CachedRG& cr, int64_t local,
                       bool is_match, bool is_focused, bool cursor_row, int zo) {
        std::string line;
        auto arr = (!cr.cols.empty()) ? cr.cols[0] : nullptr;
        if (arr) {
            int64_t off = local;
            for (auto& chunk : arr->chunks()) {
                if (off < chunk->length()) { line = cell_to_string(*chunk, off); break; }
                off -= chunk->length();
            }
        }
        int x0 = no_index_ ? 0 : idx_w_ + 2;
        int avail = scr_c_ - x0;
        if (avail <= 0) return;

        // A highlighted row (search hit / cursor) overrides the line's own
        // colours: the highlight has to stay legible over whatever the log
        // asked for, and a half-honoured highlight reads as a rendering bug.
        attr_t row_attr = A_NORMAL; int row_cp = zo ? NCP_PLAIN + zo : 0;
        bool   override_style = false;
        if (is_match) {
            row_attr = is_focused ? (attr_t)(A_BOLD | A_REVERSE) : A_BOLD;
            row_cp = NCP_SEARCH; override_style = true;
        } else if (cursor_row) {
            row_attr = A_REVERSE; override_style = true;
        }

        for (const AnsiRun& r : ansi_runs(line)) {
            // Intersect [r.col0, r.col0+r.width) with the visible window.
            int vis_start = std::max(r.col0, hscroll_);
            int vis_end   = std::min(r.col0 + r.width, hscroll_ + avail);
            if (vis_end <= vis_start) continue;
            std::string piece =
                sub_display(r.text, vis_start - r.col0, vis_end - vis_start);
            if (piece.empty()) continue;
            attr_t at = override_style ? row_attr : (attr_t)(r.attr);
            int    cp = override_style ? row_cp
                                       : ((r.fg >= 0 || r.bg >= 0)
                                          ? get_fg_pair(r.fg, r.bg)
                                          : (zo ? NCP_PLAIN + zo : 0));
            nc_str(sy, x0 + (vis_start - hscroll_), piece, at, cp);
        }
    }

    void draw_data_row(int sy, int64_t row, const std::vector<ColVis>& vc) {
        int64_t tr = total_rows();
        if (tr >= 0 && row >= tr) return;
        // When a sort is active, `row` is a display index — translate to the
        // underlying source row for all cache / search lookups below.
        int64_t srow = source_row(row);

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
            // Show the source-row number, not the display position — it's the
            // identifier the user knows from --tsv / --parquet round-trips.
            // Text files are numbered from 1, like less -N / grep -n / every
            // editor — and like the "Line 1-19/29" the status bar already
            // shows. Tables keep vv's 0-based row index, which is the
            // identifier --tsv / --parquet round-trips use.
            int64_t shown = text_view_ ? srow + 1 : srow;
            std::string idx_s = " " + fit(digits_with_sep(std::to_string(shown)), idx_w_, true) + " ";
            if (is_match) nc_str(sy, 0, idx_s,
                                 is_focused ? (attr_t)(A_BOLD | A_REVERSE) : A_NORMAL,
                                 NCP_SEARCH);
            else          nc_str(sy, 0, idx_s, A_NORMAL, NCP_INDEX + zo);
        }

        if (src_->num_chunks() == 0) return;
        int  c  = chunk_for_row(srow);
        auto it = cache_.find(c);
        if (it == cache_.end() || !it->second.ok) return;

        int64_t local = srow - it->second.first_row;
        // Guard: row may be beyond the loaded portion of the last chunk
        // (happens while streaming and the user scrolled ahead of loaded data).
        if (local < 0 || local >= it->second.num_rows) return;

        // Text: one line, painted across the whole width from hscroll_ and
        // chopped at the screen edge. Deliberately bypasses cell_at() (which
        // truncates at max_col_w_, 32 by default) and fit() (which ellipsises
        // to the column width) — a document is not a cell.
        if (text_view_) {
            draw_text_row(sy, it->second, local, is_match, is_focused,
                          row == cur_row_, zo);
            return;
        }

        std::unordered_map<std::string, std::string> parsed;
        int parsed_row = -1;
        const bool cursor_row = (row == cur_row_);
        for (auto& col : vc) {
            // Plain A_REVERSE on the cursor cell: no new colour pair, so it
            // needs no entry in any of the five Theme structs, and it layers
            // under the search-focused row's A_BOLD|A_REVERSE below.
            const attr_t cur_attr =
                (cursor_row && col.col == cur_col_) ? A_REVERSE : A_NORMAL;
            // Reuse the string the width-fitting pass already formatted for
            // this cell (integer columns); otherwise format it now. Keyed by
            // source row, so it hits in the common unsorted case and falls
            // back cleanly under an active sort.
            auto mit = frame_cells_.find(frame_key(srow, col.col));
            std::string val = (mit != frame_cells_.end())
                ? mit->second
                : cell_at(it->second, local, col.col,
                          &parsed, &parsed_row, local);

            if (is_match) {
                // Whole row rendered with NCP_SEARCH highlight; the n/N
                // focused row gets reverse video so it stands out among the
                // other visible matches.
                nc_str(sy, col.x, " " + fit(val, col.w, right_align_[col.col]) + " ",
                       is_focused ? (attr_t)(A_BOLD | A_REVERSE)
                                  : (attr_t)(A_BOLD | cur_attr),
                       NCP_SEARCH);
                continue;
            }

            if (is_rgb_[col.col]) {
                int r = 0, gv = 0, bv = 0;
                int pair = (val != NULL_SYMBOL && parse_rgb(val, &r, &gv, &bv))
                           ? get_rgb_pair(r, gv, bv) : 0;
                if (pair > 0)
                    nc_str(sy, col.x, " " + std::string(col.w, ' ') + " ", cur_attr, pair);
                else
                    nc_str(sy, col.x, " " + fit(val, col.w, false) + " ",
                           (attr_t)((val == NULL_SYMBOL ? A_DIM : A_NORMAL) | cur_attr),
                           zpair(val == NULL_SYMBOL ? NCP_NULL : 0));
                continue;
            }

            attr_t extra = A_NORMAL; int cp = 0;
            if (val == NULL_SYMBOL)         { extra = A_DIM; cp = NCP_NULL; }
            else if (is_bool_[col.col])     { cp = (val=="true")?NCP_BOOL_T:NCP_BOOL_F; }
            else if (right_align_[col.col]) { cp = NCP_NUMBER; }
            nc_str(sy, col.x, " " + fit(val, col.w, right_align_[col.col]) + " ",
                   (attr_t)(extra | cur_attr), zpair(cp));
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
        // ── Filter-input mode: `&<expression>`; show parse error if any ─────
        if (filter_mode_ == FilterMode::Input) {
            std::string bar = std::string("&") + filter_input_;
            if (!filter_err_.empty()) bar += "    !! " + filter_err_;
            if ((int)bar.size() < scr_c_) bar += std::string(scr_c_ - (int)bar.size(), ' ');
            mvaddnstr(scr_r_ - 1, 0, bar.c_str(), scr_c_);
            curs_set(1);
            move(scr_r_ - 1, (int)filter_input_.size() + 1);
            return;
        }
        // ── Command-line input: `:command`; show parse error if any ─────────
        if (cmd_mode_ == CmdMode::Input) {
            std::string bar = std::string(":") + cmd_input_;
            if (!cmd_err_.empty()) bar += "    !! " + cmd_err_;
            if ((int)bar.size() < scr_c_) bar += std::string(scr_c_ - (int)bar.size(), ' ');
            mvaddnstr(scr_r_ - 1, 0, bar.c_str(), scr_c_);
            curs_set(1);
            move(scr_r_ - 1, (int)cmd_input_.size() + 1);
            return;
        }
        curs_set(0);

        // ── Normal status bar ────────────────────────────────────────────────
        int64_t tr  = total_rows();
        int64_t bot = top_row_ + (int64_t)data_lines();
        if (tr >= 0) bot = std::min(bot, tr);

        std::string s = text_view_ ? " Line " : " Row ";
        s += digits_with_sep(std::to_string(top_row_ + 1)) + "-"
           + digits_with_sep(std::to_string(bot)) + "/";
        s += (tr >= 0) ? digits_with_sep(std::to_string(tr)) : "?";

        if (text_view_) {
            // "Col 1-5/5" is meaningless over one column. Report the
            // horizontal scroll offset instead, and only once it is non-zero
            // so the common case stays uncluttered.
            if (hscroll_ > 0) s += "  +" + std::to_string(hscroll_) + "c";
        } else if (!vc.empty()) {
            s += "  Col ";
            s += std::to_string(vc.front().col+1) + "-";
            s += std::to_string(vc.back().col+1)  + "/";
            s += std::to_string(num_cols_);
        }
        // Tab indicator: just the numeric position. The browser-style
        // tab bar above the column header carries the labels.
        if (sources_.size() > 1) {
            s += "  tab ";
            s += std::to_string(active_tab_ + 1);
            s += "/";
            s += std::to_string(sources_.size());
        }
        // Filter indicator (truncate the expression so the bar stays one line).
        if (filter_active_) {
            std::string expr = filter_expr_str_;
            if ((int)display_width(expr) > 28) {
                expr.resize(25);
                expr += "...";
            }
            s += "  filter:";
            s += expr;
            int64_t src_n = src_->total_rows();
            if (src_n >= 0) {
                s += "  ";
                s += digits_with_sep(std::to_string(filter_total_));
                s += "/";
                s += digits_with_sep(std::to_string(src_n));
            }
        }
        // Sort indicator
        if (!sort_order_.empty() && sort_col_ >= 0 && sort_col_ < (int)col_names_.size()) {
            s += "  sort:";
            s += col_names_[sort_col_];
            s += sort_desc_ ? " ↓" : " ↑";
        }
        // Hidden-column indicator
        if (!col_visible_.empty()) {
            int hidden = 0;
            for (auto v : col_visible_) if (!v) ++hidden;
            if (hidden > 0) {
                s += "  hidden:";
                s += std::to_string(hidden);
            }
        }
        // A search / sort / filter / stats pass ran over a stream that had
        // already released batches — say so rather than present a partial
        // answer as complete.
        if (partial_pass_) s += "  [PARTIAL]";
        // Show search state
        if (search_mode_ == SearchMode::Active && !search_query_.empty()) {
            s += search_dir_forward_ ? "  /" : "  ?";
            s += search_query_;
            if (!search_regex_valid_ && !search_query_.empty())
                s += " (literal)";
            if (search_fail_)         s += " (not found)";
            else if (search_wrap_)    s += " (wrapped)";
            s += "  [n/N]:next/prev  [Esc]:clear";
        } else if (!copy_status_.empty()) {
            s += "  " + copy_status_;
        } else {
            if (text_view_) {
                // No column keys to advertise: h/l scroll the line, and
                // sort / column-picker / stats do not apply to one column
                // of prose.
                s += "  [h/l]:←→scroll  [0]:home  [j/k]:lines  /:search  "
                     "&:filter  ::cmd  Enter:detail  y:copy  T:theme  H:help  q:quit";
            } else {
            bool need_lr = left_col_ > 0 || (!vc.empty() && vc.back().col < num_cols_-1);
            if (need_lr) s += "  [h/l]:←→col  [,/.]:narrow/widen";
            s += "  [j/k]:rows  /:search  &:filter  ::cmd  Enter:detail  S:stats  s:sort  c:cols  y:copy  T:theme  H:help  q:quit";
            }
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
        if (src_->num_chunks() == 0) return out;
        int64_t srow = source_row(row);
        int c = chunk_for_row(srow);
        std::vector<int> all_src;
        for (int i = 0; i < src_num_cols_; ++i) all_src.push_back(i);
        ensure_cols(c, all_src);
        auto it = cache_.find(c);
        if (it == cache_.end() || !it->second.ok) return out;
        const CachedRG& cr = it->second;
        int64_t local = srow - cr.first_row;
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
                    out[vc] = src_->format_cell(sc, std::move(val));
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
    // ── Stats popup helpers ──────────────────────────────────────────────────
    //
    // Computes per-column count / nulls / min / max / mean / distinct on
    // demand. Same shape as print_describe, just for a single virtual column.
    void compute_stats_for(int virt_col) {
        TuiColStat cs;
        int sc = (virt_col >= 0 && virt_col < (int)virt_src_col_.size())
                  ? virt_src_col_[virt_col] : -1;
        if (sc < 0) { stats_data_ = std::move(cs); return; }
        src_->set_retain_all(true);  // stats re-read every chunk: keep them
        drain_to_eof();   // stats must cover the whole streaming file
        note_full_pass();
        auto field = src_->schema()->field(sc);
        cs.name = col_names_[virt_col];
        cs.type = field->type()->ToString();
        cs.is_num = is_numeric_type(field->type()->id());
        const std::string& info_key = virt_info_key_[virt_col];
        std::vector<int> need = {sc};
        int nc = src_->num_chunks();
        for (int c = 0; c < nc; ++c) {
            mvprintw(scr_r_-1, 0, " Computing stats… chunk %d/%d ", c+1, nc);
            clrtoeol(); refresh();
            std::shared_ptr<arrow::Table> tbl;
            if (!src_->read_chunk(c, need, &tbl).ok()) continue;
            auto col = tbl->column(0);
            for (auto& ch : col->chunks()) {
                int64_t n = ch->length();
                for (int64_t r = 0; r < n; ++r) {
                    std::string raw;
                    if (ch->IsNull(r)) { cs.nulls++; continue; }
                    if (!info_key.empty()) {
                        // VCF INFO expansion: parse the key=value list, look
                        // up our key; absence counts as null.
                        std::string blob = cell_to_string(*ch, r);
                        auto kvs = parse_kv_list(blob);
                        bool found = false;
                        for (auto& kv : kvs) {
                            if (kv.first == info_key) {
                                raw = kv.second.empty() ? "true" : kv.second;
                                found = true; break;
                            }
                        }
                        if (!found) { cs.nulls++; continue; }
                    } else {
                        raw = cell_to_string(*ch, r);
                    }
                    cs.count++;
                    if (cs.is_num) {
                        double d;
                        if (!array_value_as_double(*ch, r, &d)) {
                            // Last resort for an is_numeric type the extractor
                            // somehow can't read: parse the string repr.
                            try { d = std::stod(raw); } catch (...) { continue; }
                        }
                        if (d < cs.d_min) cs.d_min = d;
                        if (d > cs.d_max) cs.d_max = d;
                        cs.sum += d;
                    } else {
                        if (cs.count == 1 || raw < cs.s_min) cs.s_min = raw;
                        if (cs.count == 1 || raw > cs.s_max) cs.s_max = raw;
                        if (!cs.distinct_overflow) {
                            cs.distinct.insert(raw);
                            if (cs.distinct.size() > 16) {
                                cs.distinct_overflow = true;
                                cs.distinct.clear();
                            }
                        }
                    }
                }
            }
        }
        stats_data_ = std::move(cs);
    }

    void draw_stats_overlay() {
        if (!stats_data_) return;
        const auto& cs = *stats_data_;
        auto fmt_num = [](double v) -> std::string {
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.6g", v); return buf;
        };
        auto with_sep = [](int64_t v) {
            return digits_with_sep(std::to_string(v));
        };
        std::vector<std::pair<std::string,std::string>> rows;
        rows.emplace_back("Column", cs.name);
        rows.emplace_back("Type",   cs.type);
        rows.emplace_back("Count",  with_sep(cs.count));
        rows.emplace_back("Nulls",  with_sep(cs.nulls));
        if (cs.count == 0) {
            rows.emplace_back("Min", "-");
            rows.emplace_back("Max", "-");
        } else if (cs.is_num) {
            rows.emplace_back("Min",  fmt_num(cs.d_min));
            rows.emplace_back("Max",  fmt_num(cs.d_max));
            rows.emplace_back("Mean", fmt_num((double)(cs.sum / (long double)cs.count)));
        } else {
            rows.emplace_back("Min", cs.s_min);
            rows.emplace_back("Max", cs.s_max);
            rows.emplace_back("Distinct", cs.distinct_overflow
                ? std::string(">16")
                : std::to_string(cs.distinct.size()));
        }
        // These numbers were computed over a stream that had already released
        // batches — mark them rather than let them read as whole-file stats.
        if (partial_pass_)
            rows.emplace_back("Scope", "PARTIAL (batches released)");

        int w_l = 8, w_r = 0;
        for (auto& [l, v] : rows) {
            w_l = std::max(w_l, (int)display_width(l));
            w_r = std::max(w_r, (int)display_width(v));
        }
        const std::string title = " column stats ";
        int inner = std::max(w_l + 2 + w_r, (int)display_width(title));
        int panel_w = inner + 4;
        int panel_h = (int)rows.size() + 3;
        if (panel_w > scr_c_) panel_w = scr_c_;
        if (panel_h > scr_r_) panel_h = scr_r_;
        int y0 = std::max(0, (scr_r_ - panel_h) / 2);
        int x0 = std::max(0, (scr_c_ - panel_w) / 2);

        // Top border with title.
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
            nc_str(y0, x0, top, A_BOLD);
        }
        for (int i = 1; i < panel_h - 1; ++i) {
            mvaddstr(y0 + i, x0, BOX_VLINE);
            mvhline(y0 + i, x0 + 1, ' ', panel_w - 2);
            mvaddstr(y0 + i, x0 + panel_w - 1, BOX_VLINE);
        }
        for (int i = 0; i < (int)rows.size() && i + 1 < panel_h - 1; ++i) {
            int yy = y0 + 1 + i;
            int xx = x0 + 2;
            attron(A_BOLD); mvaddstr(yy, xx, rows[i].first.c_str()); attroff(A_BOLD);
            int avail = panel_w - 2 - (xx - x0) - (w_l + 2) - 1;
            std::string v = rows[i].second;
            if ((int)display_width(v) > avail && avail > 3) {
                v.resize(avail - 3); v += ELLIPSIS;
            }
            mvaddstr(yy, xx + w_l + 2, v.c_str());
        }
        std::string bot = std::string(BOX_BL);
        for (int i = 0; i < panel_w - 2; ++i) bot += BOX_HLINE;
        bot += BOX_BR;
        nc_str(y0 + panel_h - 1, x0, bot, A_BOLD);
    }

    // ── Column show/hide picker ─────────────────────────────────────────────
    void draw_col_picker() {
        if (col_visible_.empty()) return;
        int n = (int)col_visible_.size();
        int w_name = 0;
        for (int i = 0; i < n; ++i)
            w_name = std::max(w_name, (int)display_width(col_names_[i]));
        const std::string title = " show / hide columns ";
        int inner = std::max(4 + w_name, (int)display_width(title));
        int panel_w = inner + 4;
        int visible_lines = std::min(n + 1, scr_r_ - 4);   // +1 for footer hint
        int panel_h = visible_lines + 2;
        if (panel_w > scr_c_) panel_w = scr_c_;
        if (panel_h > scr_r_) panel_h = scr_r_;
        int y0 = std::max(0, (scr_r_ - panel_h) / 2);
        int x0 = std::max(0, (scr_c_ - panel_w) / 2);

        // Borders
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
            nc_str(y0, x0, top, A_BOLD);
        }
        for (int i = 1; i < panel_h - 1; ++i) {
            mvaddstr(y0 + i, x0, BOX_VLINE);
            mvhline(y0 + i, x0 + 1, ' ', panel_w - 2);
            mvaddstr(y0 + i, x0 + panel_w - 1, BOX_VLINE);
        }

        // Scroll the list so the cursor stays visible.
        int rows_visible = panel_h - 3;  // 1 top + 1 bottom border + 1 footer line
        int scroll = 0;
        if (col_picker_cursor_ >= rows_visible)
            scroll = col_picker_cursor_ - rows_visible + 1;
        if (col_picker_cursor_ < scroll) scroll = col_picker_cursor_;

        for (int i = 0; i < rows_visible; ++i) {
            int idx = i + scroll;
            if (idx >= n) break;
            int yy = y0 + 1 + i;
            int xx = x0 + 2;
            std::string line = (col_visible_[idx] ? "[x] " : "[ ] ") + col_names_[idx];
            if ((int)display_width(line) > panel_w - 4)
                line.resize(panel_w - 4);
            attr_t a = (idx == col_picker_cursor_) ? (attr_t)(A_BOLD | A_REVERSE) : A_NORMAL;
            mvaddstr(yy, xx, "");
            attron(a);
            mvaddstr(yy, xx, line.c_str());
            attroff(a);
        }
        // Footer hint
        int yy = y0 + panel_h - 2;
        std::string hint = " j/k:move  space:toggle  c/Esc:close ";
        if ((int)display_width(hint) > panel_w - 4)
            hint.resize(panel_w - 4);
        nc_str(yy, x0 + 2, hint, A_DIM);

        std::string bot = std::string(BOX_BL);
        for (int i = 0; i < panel_w - 2; ++i) bot += BOX_HLINE;
        bot += BOX_BR;
        nc_str(y0 + panel_h - 1, x0, bot, A_BOLD);
    }

    // ── Theme picker (`T`) ──────────────────────────────────────────────────
    //
    // Centered overlay listing every built-in theme. `[*]` marks the
    // currently-active theme, the cursor row is highlighted with
    // reverse video. Enter applies and saves (XDG config); Esc closes
    // without changing.
    void draw_theme_picker() {
        int w_name = 0;
        for (int i = 0; i < kNumThemes; ++i)
            w_name = std::max(w_name, (int)display_width(kAllThemes[i]->name));
        const std::string title = " choose a theme ";
        int inner = std::max(4 + w_name, (int)display_width(title));
        int panel_w = inner + 4;
        int panel_h = kNumThemes + 4;       // top + rows + footer + bottom + 1 pad
        if (panel_w > scr_c_) panel_w = scr_c_;
        if (panel_h > scr_r_) panel_h = scr_r_;
        int y0 = std::max(0, (scr_r_ - panel_h) / 2);
        int x0 = std::max(0, (scr_c_ - panel_w) / 2);

        // Top border with centered title.
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
            nc_str(y0, x0, top, A_BOLD);
        }
        for (int i = 1; i < panel_h - 1; ++i) {
            mvaddstr(y0 + i, x0, BOX_VLINE);
            mvhline(y0 + i, x0 + 1, ' ', panel_w - 2);
            mvaddstr(y0 + i, x0 + panel_w - 1, BOX_VLINE);
        }

        for (int i = 0; i < kNumThemes; ++i) {
            int yy = y0 + 1 + i;
            int xx = x0 + 2;
            const bool active = (kAllThemes[i] == g_theme);
            std::string line = (active ? "[*] " : "[ ] ") + std::string(kAllThemes[i]->name);
            if ((int)display_width(line) > panel_w - 4)
                line.resize(panel_w - 4);
            attr_t a = (i == theme_picker_cursor_) ? (attr_t)(A_BOLD | A_REVERSE) : A_NORMAL;
            attron(a);
            mvaddstr(yy, xx, line.c_str());
            attroff(a);
        }
        int yy = y0 + panel_h - 2;
        std::string hint = " j/k:move  Enter:select+save  T/Esc:close ";
        if ((int)display_width(hint) > panel_w - 4)
            hint.resize(panel_w - 4);
        nc_str(yy, x0 + 2, hint, A_DIM);

        std::string bot = std::string(BOX_BL);
        for (int i = 0; i < panel_w - 2; ++i) bot += BOX_HLINE;
        bot += BOX_BR;
        nc_str(y0 + panel_h - 1, x0, bot, A_BOLD);
    }

    // Apply the theme at kAllThemes[idx], re-init ncurses color pairs,
    // and persist the choice to ~/.config/vv/config. The next draw()
    // re-paints automatically.
    void apply_theme(int idx) {
        if (idx < 0 || idx >= kNumThemes) return;
        g_theme = kAllThemes[idx];
        // Wipe any RGB pairs allocated dynamically for BED itemRgb — those
        // were sized to fit between the static pairs and the zebra range,
        // which both shift on theme change.
        rgb_pair_.clear();
        zebra_enabled_ = false;
        setup_colors();
        bool saved = save_user_setting("theme", g_theme->name);
        copy_status_ = std::string("theme: ") + g_theme->name +
                       (saved ? "" : "  (couldn't save)");
        // Force a full repaint so already-drawn cells pick up the new palette.
        clearok(stdscr, TRUE);
    }

    // ── Tab snapshot / restore ──────────────────────────────────────────────
    void save_active_to_snapshot() {
        auto& t = tabs_[active_tab_];
        t.num_cols       = num_cols_;
        t.src_num_cols   = src_num_cols_;
        t.idx_w          = idx_w_;
        t.col_names      = col_names_;
        t.col_types_str  = col_types_str_;
        t.col_widths     = col_widths_;
        t.right_align    = right_align_;
        t.is_bool        = is_bool_;
        t.is_rgb         = is_rgb_;
        t.is_integer     = is_integer_;
        t.virt_src_col   = virt_src_col_;
        t.virt_info_key  = virt_info_key_;
        t.col_visible    = col_visible_;
        t.top_row        = top_row_;
        t.left_col       = left_col_;
        t.freeze_first_col = freeze_first_col_;
        t.cache          = std::move(cache_);
        t.lru            = std::move(lru_);
        t.search_mode    = search_mode_;
        t.search_query   = search_query_;
        t.search_row     = search_row_;
        t.search_wrap    = search_wrap_;
        t.search_fail    = search_fail_;
        t.search_dir_forward = search_dir_forward_;
        t.sort_col       = sort_col_;
        t.sort_desc      = sort_desc_;
        t.sort_order     = std::move(sort_order_);
        t.filter_active  = filter_active_;
        t.filter_expr_str = filter_expr_str_;
        t.filter_fx      = filter_fx_;
        t.filter_total   = filter_total_;
        t.cur_row        = cur_row_;
        t.cur_col        = cur_col_;
        t.partial_pass   = partial_pass_;
    }

    void load_snapshot_into_active() {
        auto& t = tabs_[active_tab_];
        num_cols_        = t.num_cols;
        src_num_cols_    = t.src_num_cols;
        idx_w_           = t.idx_w;
        col_names_       = t.col_names;
        col_types_str_   = t.col_types_str;
        col_widths_      = t.col_widths;
        right_align_     = t.right_align;
        is_bool_         = t.is_bool;
        is_rgb_          = t.is_rgb;
        is_integer_      = t.is_integer;
        virt_src_col_    = t.virt_src_col;
        virt_info_key_   = t.virt_info_key;
        col_visible_     = t.col_visible;
        top_row_         = t.top_row;
        left_col_        = t.left_col;
        freeze_first_col_ = t.freeze_first_col;
        cache_           = std::move(t.cache);
        lru_             = std::move(t.lru);
        search_mode_     = t.search_mode;
        search_query_    = t.search_query;
        search_row_      = t.search_row;
        search_wrap_     = t.search_wrap;
        search_fail_     = t.search_fail;
        search_dir_forward_ = t.search_dir_forward;
        compile_search();
        sort_col_        = t.sort_col;
        sort_desc_       = t.sort_desc;
        sort_order_      = std::move(t.sort_order);
        filter_active_   = t.filter_active;
        filter_expr_str_ = t.filter_expr_str;
        filter_fx_       = t.filter_fx;
        filter_total_    = t.filter_total;
        cur_row_         = t.cur_row;
        cur_col_         = t.cur_col;
        partial_pass_    = t.partial_pass;
    }

public:
    // Return ownership of tab 0's source to the caller. Used by main() to
    // reclaim it if the TUI failed to start so the non-interactive
    // fall-through paths can still render the table.
    std::unique_ptr<TabularSource> take_first_source() {
        if (sources_.empty()) return {};
        return std::move(sources_[0]);
    }
private:

    // Cycle to the next (delta=+1) or previous (delta=-1) tab. Closes any
    // open overlay first so the new tab starts in a clean view.
    void switch_tab(int delta) {
        if (sources_.size() <= 1) return;
        save_active_to_snapshot();
        // Close transient overlays — they were positioned for the old tab.
        help_open_ = stats_open_ = col_picker_open_ = theme_picker_open_ = false;
        detail_row_ = -1;
        copy_status_.clear();
        cmd_mode_ = CmdMode::None; cmd_input_.clear(); cmd_err_.clear();
        filter_mode_ = FilterMode::None; filter_input_.clear(); filter_err_.clear();

        active_tab_ = (active_tab_ + delta + (int)tabs_.size()) % (int)tabs_.size();
        src_ = sources_[active_tab_].get();
        text_view_ = src_->is_text();
        hscroll_ = 0;
        if (!tabs_[active_tab_].initialised) {
            setup_for_active_source();   // lazy init the column metadata
            // Default view state: top, no filter, no sort.
            top_row_ = 0; left_col_ = 0; freeze_first_col_ = false;
            cur_row_ = 0; cur_col_ = 0;
            cache_.clear(); lru_.clear();
            sort_col_ = -1; sort_desc_ = false; sort_order_.clear();
            filter_active_ = false; filter_expr_str_.clear(); filter_total_ = 0;
            search_mode_ = SearchMode::None; search_query_.clear(); search_row_ = -1;
        } else {
            load_snapshot_into_active();
        }
    }

    // Execute a `:`-prefixed command (without the leading colon). Returns
    // true on quit, false otherwise. Sets cmd_err_ on a parse failure so
    // the input bar can keep the bad text visible for the user to edit.
    // Step the slice axis of a 3-D+ source (NPZ today). Returns true if
    // the underlying table was rebuilt — caller drops cached chunks and
    // resets the viewport so the user sees the new slice from the top.
    bool apply_slice_change(int delta, bool absolute, int64_t target) {
        if (!src_->change_slice(delta, absolute, target)) return false;
        // Source's underlying arrow::Table has been swapped. Re-derive
        // per-tab metadata and drop caches so the next draw reads from
        // the new slice.
        cache_.clear(); lru_.clear();
        sort_order_.clear(); sort_col_ = -1; sort_desc_ = false;
        filter_active_ = false; filter_expr_str_.clear(); filter_total_ = 0;
        top_row_ = 0; left_col_ = 0;
        cur_row_ = 0; cur_col_ = 0;
        search_row_ = -1;
        setup_for_active_source();
        return true;
    }

    bool execute_cmd(const std::string& raw) {
        std::string c = raw;
        strip_ws_inplace(c);
        if (c.empty()) return false;

        if (c == "q" || c == "quit") return true;

        // slice N — jump to a specific slice index in NPZ 3-D+ arrays.
        if (c.rfind("slice ", 0) == 0 || c.rfind("slice\t", 0) == 0) {
            std::string arg = c.substr(6);
            strip_ws_inplace(arg);
            try {
                int64_t n = std::stoll(arg);
                if (!apply_slice_change(0, /*absolute=*/true, n)) {
                    cmd_err_ = "no slice axis here";
                }
            } catch (...) {
                cmd_err_ = "bad slice index: " + arg;
            }
            return false;
        }

        // theme NAME — text equivalent of the `T` overlay.
        if (c.rfind("theme ", 0) == 0 || c.rfind("theme\t", 0) == 0) {
            std::string name = c.substr(6);
            strip_ws_inplace(name);
            for (int i = 0; i < kNumThemes; ++i) {
                if (name == kAllThemes[i]->name) { apply_theme(i); return false; }
            }
            if (name == "solarized") {
                for (int i = 0; i < kNumThemes; ++i)
                    if (kAllThemes[i] == &kThemeSolarizedDark) {
                        apply_theme(i); return false;
                    }
            }
            cmd_err_ = "unknown theme: " + name;
            return false;
        }

        // Pure number → jump to that row (matches the row-index column).
        if (c.find_first_not_of("0123456789") == std::string::npos) {
            try {
                int64_t r = std::stoll(c);
                int64_t tr = total_rows();
                if (tr >= 0 && r >= tr) r = tr - 1;
                if (r < 0) r = 0;
                int dl = data_lines();
                int64_t mt = (tr >= 0) ? std::max<int64_t>(0, tr - dl) : r;
                top_row_     = std::min(r, mt);
                search_row_  = -1;
                copy_status_.clear();
            } catch (...) {
                cmd_err_ = "bad row number: " + c;
            }
            return false;
        }

        cmd_err_ = "unknown command: :" + c;
        return false;
    }

    // Full-file passes (sort / filter / stats / search) iterate
    // src_->num_chunks(), which for a forward-only streaming source only
    // exposes the chunks loaded so far (the scrolled-through prefix). Drain the
    // stream to EOF first — mirroring the 'G' handler — so those passes see the
    // whole file instead of silently operating on a prefix. A no-op for
    // random-access sources (total_rows() already known).
    void drain_to_eof() {
        if (src_->total_rows() >= 0) return;
        mvaddstr(scr_r_ - 1, 0, " Loading to end of file… ");
        clrtoeol(); refresh();
        while (src_->total_rows() < 0)
            src_->ensure(src_->num_chunks());
    }

    // Call at the head of every operation that re-reads the whole file
    // (search / sort / filter / column stats), right after retention has been
    // pinned and the source drained. Forward-only streaming sources keep only
    // a bounded trailing window of decoded batches, so if any batch was
    // released before we pinned, those rows are gone and the pass cannot see
    // them. That used to produce a confidently wrong answer with no marker —
    // evicted_any() existed for exactly this and had no callers.
    void note_full_pass() {
        if (src_->evicted_any()) partial_pass_ = true;
    }

    // ── Rebuild display→source mapping (sort + filter) ──────────────────────
    //
    // Combines the two view-of-the-data features into one full-file pass:
    //   • If filter is active, each row is run through filter_fx_; only
    //     matching rows are kept.
    //   • If sort is active, kept rows are sorted by the chosen column's
    //     raw Arrow value (numeric) or string repr (otherwise). Nulls last.
    //
    // The result is stored in sort_order_, whose presence flips
    // total_rows(), search wrap-around, and the source_row() indirection
    // used by every read path. Empty sort_order_ means "identity"
    // (display row N = source row N).
    void rebuild_display_order() {
        sort_order_.clear();
        filter_total_ = 0;
        if (sort_col_ < 0 && !filter_active_) return;
        src_->set_retain_all(true);  // sort/filter re-read every row: keep them
        drain_to_eof();   // sort/filter must cover the whole streaming file
        note_full_pass();

        // Resolve sort column (if any).
        bool num = false;
        int  sc = -1;
        std::string info_key;
        std::vector<int> need;
        if (sort_col_ >= 0 && sort_col_ < (int)virt_src_col_.size()) {
            sc = virt_src_col_[sort_col_];
            if (sc >= 0) {
                num = is_numeric_type(src_->schema()->field(sc)->type()->id())
                      && virt_info_key_[sort_col_].empty();
                info_key = virt_info_key_[sort_col_];
                need.push_back(sc);
            } else {
                sort_col_ = -1;
            }
        }
        // Add the filter's referenced columns to the read projection.
        std::vector<int> filt_cols;
        if (filter_active_) {
            filt_cols = union_with_filter({}, filter_fx_);
            for (int fc : filt_cols)
                if (std::find(need.begin(), need.end(), fc) == need.end())
                    need.push_back(fc);
        }

        // Predicate: row matches the active filter, or always true when off.
        auto eval_row = [&](const arrow::Table& tbl, int64_t r) -> bool {
            if (!filter_active_) return true;
            for (const auto& clause : filter_fx_.groups) {
                bool all = true;
                for (const auto& a : clause)
                    if (!eval_atom(tbl, r, a, need)) { all = false; break; }
                if (all) return true;
            }
            return false;
        };

        struct NumK { double v; bool is_null; int64_t row; };
        struct StrK { std::string v; bool is_null; int64_t row; };
        std::vector<NumK>     nk;
        std::vector<StrK>     sk;
        std::vector<int64_t>  rows_only;  // filter-only path: source order

        int nc = src_->num_chunks();
        const char* what = (sort_col_ >= 0 && filter_active_) ? "Filter+sort"
                         : (sort_col_ >= 0)                   ? "Sorting"
                                                              : "Filtering";
        for (int c = 0; c < nc; ++c) {
            mvprintw(scr_r_-1, 0, " %s… chunk %d/%d ", what, c+1, nc);
            clrtoeol(); refresh();
            std::shared_ptr<arrow::Table> tbl;
            if (!src_->read_chunk(c, need, &tbl).ok()) continue;
            auto meta = src_->chunk_meta(c);
            // Locate the sort column within the projected table.
            int p_sort = -1;
            if (sc >= 0) {
                for (size_t k = 0; k < need.size(); ++k)
                    if (need[k] == sc) { p_sort = (int)k; break; }
            }
            int64_t n = tbl->num_rows();
            for (int64_t r = 0; r < n; ++r) {
                int64_t srow = meta.first_row + r;
                if (!eval_row(*tbl, r)) continue;
                if (sort_col_ < 0) { rows_only.push_back(srow); continue; }
                // Sort key extraction. ReadRowGroups returns a single-chunk
                // table for each column, so walking the chunked array is
                // strictly speaking unnecessary — but stay defensive.
                auto col = tbl->column(p_sort);
                int64_t off = r;
                std::shared_ptr<arrow::Array> ch_arr;
                int64_t ch_off = 0;
                for (auto& chunk : col->chunks()) {
                    if (off < chunk->length()) {
                        ch_arr = chunk; ch_off = off; break;
                    }
                    off -= chunk->length();
                }
                if (!ch_arr) continue;
                bool is_null = ch_arr->IsNull(ch_off);
                if (!info_key.empty()) {
                    std::string raw = is_null ? std::string()
                                              : cell_to_string(*ch_arr, ch_off);
                    if (!is_null) {
                        auto kvs = parse_kv_list(raw);
                        bool found = false;
                        for (auto& kv : kvs)
                            if (kv.first == info_key) {
                                raw = kv.second.empty() ? "true" : kv.second;
                                found = true; break;
                            }
                        if (!found) is_null = true;
                    }
                    if (num) {
                        double d = 0; bool nn = is_null;
                        if (!nn) { try { d = std::stod(raw); } catch (...) { nn = true; } }
                        nk.push_back({d, nn, srow});
                    } else {
                        sk.push_back({raw, is_null, srow});
                    }
                    continue;
                }
                if (num) {
                    double d = 0;
                    if (!is_null) {
                        if (!array_value_as_double(*ch_arr, ch_off, &d)) {
                            try { d = std::stod(cell_to_string(*ch_arr, ch_off)); }
                            catch (...) { is_null = true; }
                        }
                    }
                    nk.push_back({d, is_null, srow});
                } else {
                    std::string s = is_null ? std::string()
                                            : cell_to_string(*ch_arr, ch_off);
                    sk.push_back({s, is_null, srow});
                }
            }
        }

        if (sort_col_ < 0) {
            sort_order_ = std::move(rows_only);
        } else if (num) {
            std::sort(nk.begin(), nk.end(), [this](const NumK& a, const NumK& b) {
                if (a.is_null != b.is_null) return !a.is_null;
                if (a.is_null) return false;
                return sort_desc_ ? a.v > b.v : a.v < b.v;
            });
            sort_order_.reserve(nk.size());
            for (auto& e : nk) sort_order_.push_back(e.row);
        } else {
            std::sort(sk.begin(), sk.end(), [this](const StrK& a, const StrK& b) {
                if (a.is_null != b.is_null) return !a.is_null;
                if (a.is_null) return false;
                return sort_desc_ ? a.v > b.v : a.v < b.v;
            });
            sort_order_.reserve(sk.size());
            for (auto& e : sk) sort_order_.push_back(e.row);
        }
        filter_total_ = (int64_t)sort_order_.size();
    }

    void draw_help_overlay() {
        struct Row { const char* keys; const char* desc; };
        static const Row rows[] = {
            {"q  Esc",       "quit  (Esc clears search / closes overlays)"},
            {"↑↓  j k",      "move the cell cursor one row"},
            {"PgUp PgDn  ␣ b","scroll one page"},
            {"g  G  Home End","first / last row"},
            {"←→  h l",      "move the cell cursor one column"},
            {",  .",          "narrow / widen the cursor's column"},
            {"z",            "toggle frozen first column"},
            {"/  ?",          "search forward / backward (regex, icase)"},
            {"n  N",          "next / previous match (direction-aware)"},
            {"S",             "per-column stats (count/min/max/mean/distinct)"},
            {"s",             "sort by the cursor's column (toggle asc/desc; u to clear)"},
            {"&",             "live filter: hide non-matching rows; empty input clears"},
            {"c",             "show / hide columns (overlay)"},
            {"y",             "copy the top-left visible cell to the clipboard (OSC52)"},
            {"T",             "pick a color theme (saved to ~/.config/vv/config)"},
            {":",             "command line: :N (jump), :q, :theme NAME, :slice N"},
            {"Tab  Shift-Tab","next / previous tab (with multiple files)"},
            {"[  ]",          "step slice axis (NPZ 3-D+ arrays only)"},
            {"Enter",         "open detail pane for the top-visible row"},
            {"mouse wheel",   "scroll rows"},
            {"mouse click",   "header → sort by column; row → scroll to top"},
            {"mouse 2-click", "row → open detail pane"},
            {"Shift+drag",    "select text for OS clipboard (terminal-side)"},
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
        // Reserve one screen row for the browser-style tab bar when more
        // than one tab is open. data_lines() picks this up automatically.
        tabbar_h_ = (sources_.size() > 1) ? 1 : 0;
        // Reserve row 0 for the active source's top banner (LociSSD only).
        banner_h_ = (src_ && !src_->top_banner().empty()) ? 1 : 0;
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
        // Scroll the viewport to the cursor before anything measures the
        // window — the width-fit pass and both visible_cols() calls below
        // must see the columns we are about to paint.
        ensure_cursor_visible();
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
            frame_cells_.clear();   // per-frame; populated by the fit pass below
            fit_widths_to_visible(virt);
        }
        vc = visible_cols();
        draw_banner();
        draw_tabbar();
        if (!text_view_) draw_header(vc);
        int dl = data_lines();
        for (int y = 0; y < dl; ++y)
            draw_data_row(data_top_y() + y, top_row_ + y, vc);
        draw_status(vc);
        draw_detail_pane();  // overlay if detail_row_ >= 0
        if (stats_open_)      draw_stats_overlay();
        if (col_picker_open_)   draw_col_picker();
        if (theme_picker_open_) draw_theme_picker();
        if (help_open_)       draw_help_overlay();
        refresh();
    }

    void setup_colors() {
        if (!has_colors()) return;
        start_color(); use_default_colors();

        const bool c256 = COLORS >= 256;
        const Theme& t  = *g_theme;

        // Pick from the theme's 256-color palette when available; otherwise
        // fall back to its 16-color twins (every theme provides both).
        const int fg_header = c256 ? t.nc_fg_header : t.nc16_fg_header;
        const int fg_index  = c256 ? t.nc_fg_index  : t.nc16_fg_index;
        const int fg_null   = c256 ? t.nc_fg_null   : t.nc16_fg_null;
        const int fg_number = c256 ? t.nc_fg_number : t.nc16_fg_number;
        const int fg_boolt  = c256 ? t.nc_fg_boolt  : t.nc16_fg_boolt;
        const int fg_boolf  = c256 ? t.nc_fg_boolf  : t.nc16_fg_boolf;
        const int fg_sep    = c256 ? t.nc_fg_sep    : t.nc16_fg_sep;
        const int fg_search = c256 ? t.nc_fg_search : t.nc16_fg_search;
        const int bg_search = c256 ? t.nc_bg_search : t.nc16_bg_search;

        init_pair(NCP_HEADER,  fg_header, -1);
        init_pair(NCP_INDEX,   fg_index,  -1);
        init_pair(NCP_NULL,    fg_null,   -1);
        init_pair(NCP_NUMBER,  fg_number, -1);
        init_pair(NCP_BOOL_T,  fg_boolt,  -1);
        init_pair(NCP_BOOL_F,  fg_boolf,  -1);
        init_pair(NCP_SEP,     fg_sep,    -1);
        init_pair(NCP_SEARCH,  fg_search, bg_search);
        init_pair(NCP_PLAIN,   -1,        -1);

        // Zebra twins (256-color only). Each theme picks its own bg shade;
        // -1 disables zebra altogether (e.g. for the light theme on bright
        // backgrounds where any tint over the default looks muddy).
        if (c256 && t.nc_bg_zebra >= 0) {
            int bg_zebra = t.nc_bg_zebra;
            // A dark near-black stripe (default/dark themes) is unreadable on a
            // detected light terminal — the default-foreground text goes dark on
            // dark. Swap it for a subtle light grey (what the light themes use),
            // so the stripe stays "only a little" off the background.
            if (g_term_bg == TermBg::Light && bg_zebra < 244)
                bg_zebra = 254;
            init_pair(NCP_HEADER + ZEBRA_OFFSET, fg_header, bg_zebra);
            init_pair(NCP_INDEX  + ZEBRA_OFFSET, fg_index,  bg_zebra);
            init_pair(NCP_NULL   + ZEBRA_OFFSET, fg_null,   bg_zebra);
            init_pair(NCP_NUMBER + ZEBRA_OFFSET, fg_number, bg_zebra);
            init_pair(NCP_BOOL_T + ZEBRA_OFFSET, fg_boolt,  bg_zebra);
            init_pair(NCP_BOOL_F + ZEBRA_OFFSET, fg_boolf,  bg_zebra);
            init_pair(NCP_SEP    + ZEBRA_OFFSET, fg_sep,    bg_zebra);
            init_pair(NCP_PLAIN  + ZEBRA_OFFSET, -1,        bg_zebra);
            zebra_enabled_ = true;
            next_rgb_pair_ = NCP_PLAIN + ZEBRA_OFFSET + 1;
        }
    }

public:
    TableTUI(std::vector<std::unique_ptr<TabularSource>> sources, const Config& cfg)
        : sources_(std::move(sources)),
          src_(sources_.empty() ? nullptr : sources_[0].get()),
          max_col_w_(cfg.max_col_w),
          no_index_(cfg.no_index),
          max_cols_cfg_(cfg.max_cols)
    {
        if (cfg.scrolloff >= 0) scrolloff_ = cfg.scrolloff;
        // Build per-tab snapshot slots up front. We materialise the active
        // tab's column metadata now; other tabs init lazily on first switch.
        tabs_.resize(sources_.size());
        for (size_t i = 0; i < sources_.size(); ++i) {
            tabs_[i].path  = sources_[i]->path();
            tabs_[i].label = sources_[i]->tab_label();
        }
        if (!sources_.empty()) setup_for_active_source();
    }

    // (Re)compute every per-tab field from the currently-active source.
    // Used by the constructor and by the lazy-init path when switching to
    // a tab that hasn't been visited yet.
    void setup_for_active_source() {
        auto& src = *src_;
        text_view_ = src.is_text();
        num_cols_ = (max_cols_cfg_ > 0)
                    ? std::min(max_cols_cfg_, src.schema()->num_fields())
                    : src.schema()->num_fields();

        // Compute index column width from total rows (or a guess if unknown).
        // Account for digit-grouping underscores in the rendered row number.
        int64_t tr = src.total_rows();
        int64_t tr_for_width = (tr >= 0) ? tr : 999999;
        idx_w_ = (int)display_width(digits_with_sep(
            std::to_string(std::max<int64_t>(tr_for_width - 1, 0))));

        src_num_cols_ = num_cols_;

        // Detect VCF INFO expansion: need both an INFO source column and
        // ##INFO=<...> declarations in the preamble.
        //
        // Stand down entirely when the source is already an ExpandedSource:
        // --expand did this at the schema level, so the keys are real columns
        // now and building display-only virtual ones on top would show every
        // key twice.
        const bool pre_expanded = dynamic_cast<const ExpandedSource*>(&src) != nullptr;
        int info_col_idx = -1;
        if (!pre_expanded) {
            for (int ci = 0; ci < src_num_cols_; ++ci)
                if (src.schema()->field(ci)->name() == "INFO") { info_col_idx = ci; break; }
        }
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

        // Short type label rendered between the column name and the rule.
        // VCF INFO virtuals don't have a real Arrow field — synthesise one
        // from the declared INFO type so the row stays consistent.
        col_types_str_.assign(num_cols_, "");
        for (int vc = 0; vc < num_cols_; ++vc) {
            if (virt_info_key_[vc].empty()) {
                col_types_str_[vc] = src.schema()->field(virt_src_col_[vc])
                                         ->type()->ToString();
            } else {
                col_types_str_[vc] = arrow_type_for_id(v_types[vc])->ToString();
            }
        }

        col_widths_.assign(num_cols_, 0);
        right_align_.assign(num_cols_, false);
        is_bool_.assign(num_cols_, false);
        is_rgb_.assign(num_cols_, false);
        is_integer_.assign(num_cols_, false);
        col_visible_.assign(num_cols_, true);
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
            int base = std::max((int)display_width(col_names_[vc]), min_w);
            if (t == arrow::Type::FLOAT || t == arrow::Type::DOUBLE)
                base = std::max(base, 8);
            if (t == arrow::Type::LIST || t == arrow::Type::LARGE_LIST
             || t == arrow::Type::FIXED_SIZE_LIST || t == arrow::Type::MAP)
                base = std::max(base, 14);
            if (t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING)
                base = std::max(base, 12);
            col_widths_[vc]  = is_integer_[vc] ? base : std::min(base, max_col_w_);
            right_align_[vc] = is_numeric_type(t);
            is_bool_[vc]     = v_is_bool[vc];
            is_rgb_[vc]      = (col_names_[vc] == "RGB");
        }
        // Mark the active tab as initialised so subsequent switches load
        // from the snapshot rather than re-running setup.
        tabs_[active_tab_].initialised = true;
    }

    // Returns false if the terminal type is not supported (missing terminfo).
    bool run() {
        setlocale(LC_ALL, "");
        detect_term_bg();   // before ncurses takes the tty (OSC 11 query)
        SCREEN* scr = newterm(nullptr, stdout, stdin);
        if (!scr) return false;
        set_term(scr);
        // Restore the terminal if we're killed while owning it (see above).
        // Handlers stay installed for the whole TUI; the signals are blocked
        // except around getch() so endwin() only runs from a safe context.
        g_tui_active = 1;
        auto prev_int  = signal(SIGINT,  tui_signal_restore);
        auto prev_term = signal(SIGTERM, tui_signal_restore);
        auto prev_hup  = signal(SIGHUP,  tui_signal_restore);
        sigset_t tui_sigs;
        sigemptyset(&tui_sigs);
        sigaddset(&tui_sigs, SIGINT);
        sigaddset(&tui_sigs, SIGTERM);
        sigaddset(&tui_sigs, SIGHUP);
        sigprocmask(SIG_BLOCK, &tui_sigs, nullptr);
        noecho(); cbreak(); keypad(stdscr, TRUE); curs_set(0);
        set_escdelay(25); setup_colors();
        // Mouse: scroll wheel (BUTTON4 / BUTTON5) + click and double-click.
        // Adding click events means the terminal switches into application
        // mouse mode and stops handling its own drag-to-select; Shift+drag
        // still works as the escape hatch in every modern emulator.
        mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED
                | BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
        // 200 ms is long enough for an unhurried double-click but short
        // enough that a deliberate pair-of-clicks isn't mistaken for one.
        mouseinterval(200);

        // A late terminal reply must not read as keystrokes. detect_term_bg()
        // sends an OSC 11 background query before ncurses starts and waits
        // ~80 ms; over a slow transport (a jupyter-lab web console proxied
        // through kubernetes, tmux over a laggy ssh) the terminal's reply
        // can outrun that budget and land here instead — where its leading
        // ESC used to hit the Esc-quits binding, so vv exited "by itself"
        // and the tail leaked to the shell as `11;rgb:ffff/ffff/ffff`.
        // After a bare ESC, peek: a string introducer (OSC/DCS/APC/SOS/PM)
        // or a CSI start means the terminal is talking, not the user —
        // swallow through the terminator and read on. Anything else is
        // pushed back, so Esc, double-Esc and Alt+key behave as before.
        // (Signals are allowed only while parked in the blocking getch():
        // one arriving during draw() is delivered there — in read(), not
        // mid-malloc — where the endwin() in the handler is safe.)
        auto tui_getch = [&]() -> int {
            for (;;) {
                sigprocmask(SIG_UNBLOCK, &tui_sigs, nullptr);
                int ch = getch();
                sigprocmask(SIG_BLOCK, &tui_sigs, nullptr);
                if (ch != 27) return ch;
                timeout(0);
                int nxt = getch();
                if (nxt == ERR) { timeout(-1); return 27; }        // lone Esc
                bool str_seq = nxt == ']' || nxt == 'P' || nxt == '_' ||
                               nxt == 'X' || nxt == '^';
                if (!str_seq && nxt != '[') {                      // Alt+key…
                    ungetch(nxt);
                    timeout(-1);
                    return 27;
                }
                // The reply may still be trickling in over the transport
                // that delayed it; allow 50 ms between bytes, cap the total.
                timeout(50);
                if (str_seq) {                    // …until BEL or ST (ESC \)
                    int prev = 0;
                    for (int i = 0; i < 4096; ++i) {
                        int c = getch();
                        if (c == ERR || c == '\a' || (prev == 27 && c == '\\'))
                            break;
                        prev = c;
                    }
                } else {                          // CSI: …until a final byte
                    for (int i = 0; i < 256; ++i) {
                        int c = getch();
                        if (c == ERR || (c >= 0x40 && c <= 0x7e)) break;
                    }
                }
                timeout(-1);
            }
        };

        bool quit = false;
        while (!quit) {
            draw();
            int ch = tui_getch();
            int dl = data_lines();

            // ── Help overlay: any key dismisses it (and is consumed) ─────────
            if (help_open_) {
                help_open_ = false;
                continue;
            }

            // ── Stats overlay: any key dismisses it ──────────────────────────
            if (stats_open_) {
                stats_open_ = false;
                stats_data_.reset();
                continue;
            }

            // ── Theme picker overlay: dedicated input handling ───────────────
            if (theme_picker_open_) {
                switch (ch) {
                    case 'q': case 'Q': case 'T': case 27:  // Esc
                        theme_picker_open_ = false;
                        break;
                    case KEY_DOWN: case 'j':
                        if (theme_picker_cursor_ + 1 < kNumThemes)
                            ++theme_picker_cursor_;
                        break;
                    case KEY_UP: case 'k':
                        if (theme_picker_cursor_ > 0) --theme_picker_cursor_;
                        break;
                    case 'g': case KEY_HOME: theme_picker_cursor_ = 0; break;
                    case 'G': case KEY_END:  theme_picker_cursor_ = kNumThemes - 1; break;
                    case ' ': case '\n': case '\r': case KEY_ENTER:
                        apply_theme(theme_picker_cursor_);
                        theme_picker_open_ = false;
                        break;
                    default: break;
                }
                continue;
            }

            // ── Column picker overlay: dedicated input handling ──────────────
            if (col_picker_open_) {
                int n = (int)col_visible_.size();
                switch (ch) {
                    case 'q': case 'Q': case 'c': case 27:  // Esc
                        col_picker_open_ = false;
                        break;
                    case KEY_DOWN: case 'j':
                        if (col_picker_cursor_ + 1 < n) ++col_picker_cursor_;
                        break;
                    case KEY_UP: case 'k':
                        if (col_picker_cursor_ > 0) --col_picker_cursor_;
                        break;
                    case 'g': case KEY_HOME: col_picker_cursor_ = 0; break;
                    case 'G': case KEY_END:  col_picker_cursor_ = std::max(0, n - 1); break;
                    case ' ': case '\n': case '\r': case KEY_ENTER:
                        if (col_picker_cursor_ >= 0 && col_picker_cursor_ < n) {
                            col_visible_[col_picker_cursor_] = !col_visible_[col_picker_cursor_];
                            // Ensure at least one column stays visible to avoid an
                            // empty header line.
                            bool any = false;
                            for (auto v : col_visible_) if (v) { any = true; break; }
                            if (!any) col_visible_[col_picker_cursor_] = true;
                        }
                        break;
                    default: break;
                }
                continue;
            }

            // ── Mouse ───────────────────────────────────────────────────────
            // Wheel scrolls; click on a column header sorts by that column;
            // click on a data row scrolls it to the top of the viewport;
            // double-click on a data row opens the detail pane.
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
                    } else if (me.bstate & (BUTTON1_CLICKED |
                                            BUTTON1_DOUBLE_CLICKED)) {
                        auto vc = visible_cols();
                        // Hit-test (y, x) against the current layout. y=0
                        // is the multi-tab bar (when visible); next two
                        // rows are the column-name row + rule; rows below
                        // are data; y = scr_r_-1 is the status bar.
                        int  hit_col = -1;            // virt col, -1 if none
                        for (const auto& v : vc) {
                            if (me.x >= v.x && me.x < v.x + v.w + 2) {
                                hit_col = v.col; break;
                            }
                        }
                        const int  data_y0   = data_top_y();
                        // Rows stack as: [banner_h_] [tabbar_h_] [header] [data].
                        const bool in_tabbar = (tabbar_h_ > 0 && me.y == banner_h_);
                        const bool in_header = (me.y >= banner_h_ + tabbar_h_ &&
                                                me.y < data_y0);
                        const bool in_data   = (me.y >= data_y0 &&
                                                me.y < scr_r_ - 1);

                        // Tab-bar click: switch to that tab.
                        if (in_tabbar) {
                            for (const auto& z : tab_hit_zones_) {
                                if (me.x >= z.x0 && me.x < z.x1) {
                                    int delta = z.idx - active_tab_;
                                    if (delta) switch_tab(delta);
                                    break;
                                }
                            }
                        } else if (me.bstate & BUTTON1_DOUBLE_CLICKED) {
                            // Drill in: open detail pane on the clicked row.
                            if (in_data) {
                                int64_t r = top_row_ + (me.y - data_y0);
                                int64_t tr = total_rows();
                                if (tr < 0 || r < tr) {
                                    detail_row_    = r;
                                    detail_scroll_ = 0;
                                }
                            }
                        } else if (in_header && hit_col >= 0) {
                            // Sort by the clicked column. Repeat-clicking
                            // the same header toggles ascending → descending.
                            if (sort_col_ == hit_col && !sort_order_.empty())
                                sort_desc_ = !sort_desc_;
                            else { sort_col_ = hit_col; sort_desc_ = false; }
                            left_col_ = hit_col;  // also focus for S / y
                            rebuild_display_order();
                            top_row_    = 0;
                            search_row_ = -1;
                        } else if (in_data) {
                            // Single click puts the cursor on the clicked cell.
                            // (It used to scroll that row to the top and set
                            // left_col_, because there was no cursor to move —
                            // the comment here shipped as the workaround.)
                            int64_t r = top_row_ + (me.y - data_y0);
                            int64_t tr = total_rows();
                            if (tr < 0 || r < tr) {
                                cur_row_ = r;
                                if (hit_col >= 0) cur_col_ = hit_col;
                                copy_status_.clear();
                            }
                        }
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

            // ── Live-filter input mode ──────────────────────────────────────
            if (filter_mode_ == FilterMode::Input) {
                if (ch == '\n' || ch == KEY_ENTER) {
                    if (filter_input_.empty()) {
                        // Empty commit = clear the filter.
                        filter_mode_   = FilterMode::None;
                        filter_active_ = false;
                        filter_expr_str_.clear();
                        filter_err_.clear();
                        rebuild_display_order();
                        top_row_ = 0;
                        search_row_ = -1;
                    } else {
                        FilterExpr fx;
                        std::string err;
                        if (!parse_filter_expr(filter_input_, *src_->schema(), &fx, &err)) {
                            filter_err_ = err;
                            // Stay in input mode so the user can edit.
                        } else {
                            filter_fx_       = std::move(fx);
                            filter_expr_str_ = filter_input_;
                            filter_active_   = true;
                            filter_err_.clear();
                            filter_mode_     = FilterMode::None;
                            rebuild_display_order();
                            top_row_ = 0;
                            search_row_ = -1;
                        }
                    }
                } else if (ch == 27) {    // Esc — cancel edit
                    filter_mode_ = FilterMode::None;
                    filter_input_.clear();
                    filter_err_.clear();
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                    if (!filter_input_.empty()) filter_input_.pop_back();
                    filter_err_.clear();
                } else if (ch >= 32 && ch < 127) {
                    filter_input_ += (char)ch;
                    filter_err_.clear();
                }
                continue;
            }

            // ── Command-line input mode (`:`) ───────────────────────────────
            if (cmd_mode_ == CmdMode::Input) {
                if (ch == '\n' || ch == KEY_ENTER) {
                    if (execute_cmd(cmd_input_)) quit = true;
                    if (cmd_err_.empty()) {
                        cmd_mode_ = CmdMode::None;
                        cmd_input_.clear();
                    }
                    // Else stay in input mode so the user can edit + retry.
                } else if (ch == 27) {           // Esc — cancel
                    cmd_mode_ = CmdMode::None;
                    cmd_input_.clear();
                    cmd_err_.clear();
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                    if (!cmd_input_.empty()) cmd_input_.pop_back();
                    cmd_err_.clear();
                } else if (ch >= 32 && ch < 127) {
                    cmd_input_ += (char)ch;
                    cmd_err_.clear();
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

            // The "copied: …" indicator stays on the status bar until the
            // next non-`y` key.
            if (ch != 'y' && ch != KEY_RESIZE) copy_status_.clear();

            switch (ch) {
                case 'q': case 'Q': quit = true; break;
                case '\n': case '\r': case KEY_ENTER:  // Open detail pane for top-visible row
                    detail_row_    = cur_row_;
                    detail_scroll_ = 0;
                    break;
                case 27:  // Esc: clear search/filter if active, else quit
                    if (search_mode_ == SearchMode::Active) {
                        search_mode_  = SearchMode::None;
                        search_query_.clear();
                        search_regex_.reset();
                        search_regex_valid_ = false;
                        search_row_   = -1;
                        search_fail_  = false;
                    } else if (filter_active_) {
                        filter_active_ = false;
                        filter_expr_str_.clear();
                        rebuild_display_order();
                        top_row_   = 0;
                        search_row_ = -1;
                    } else {
                        quit = true;
                    }
                    break;
                case KEY_DOWN: case 'j':
                    // Moving the cursor past the last loaded row is what pulls
                    // more of a streaming source in, so allow it while tr < 0.
                    if (tr < 0 || cur_row_ + 1 < tr) ++cur_row_; break;
                case KEY_UP: case 'k':
                    if (cur_row_ > 0) --cur_row_; break;
                case KEY_NPAGE: case ' ':
                    cur_row_ = (tr >= 0) ? std::min(cur_row_ + dl, tr - 1)
                                         : cur_row_ + dl;
                    if (cur_row_ < 0) cur_row_ = 0;
                    break;
                case KEY_PPAGE: case 'b':
                    cur_row_ = std::max<int64_t>(0, cur_row_ - dl); break;
                case 'g': case KEY_HOME: cur_row_ = 0; top_row_ = 0; break;
                case 'G': case KEY_END:
                    // For streaming sources we must read to EOF before we know
                    // the last row.
                    if (tr < 0) {
                        drain_to_eof();
                        tr = total_rows();
                    }
                    cur_row_ = std::max<int64_t>(0, tr - 1);
                    top_row_ = std::max<int64_t>(0, tr - dl);
                    break;
                case KEY_RIGHT: case 'l':
                    // Text: there is one column, so h/l scroll the line
                    // sideways instead (less -S). Half a screen per press,
                    // which is what makes a wide log navigable at all.
                    if (text_view_) { hscroll_ += std::max(1, text_hstep()); break; }
                    if (cur_col_ + 1 < num_cols_) {
                        int n = next_visible_col(cur_col_ + 1, +1);
                        if (n > cur_col_) cur_col_ = n;
                    }
                    break;
                case KEY_LEFT: case 'h':
                    if (text_view_) {
                        hscroll_ = std::max(0, hscroll_ - std::max(1, text_hstep()));
                        break;
                    }
                    if (cur_col_ > 0) {
                        int n = next_visible_col(cur_col_ - 1, -1);
                        if (n < cur_col_) cur_col_ = n;
                    }
                    break;
                case '0':
                    if (text_view_) hscroll_ = 0;
                    break;
                case 'z':
                    if (text_view_) { copy_status_ = TEXT_NA("z"); break; }
                    freeze_first_col_ = !freeze_first_col_; break;
                case 'H': case KEY_F(1):
                    help_open_ = true; break;
                case 'S':
                    if (text_view_) { copy_status_ = TEXT_NA("S"); break; }
                    // Compute stats for the column under the cursor,
                    // then open the overlay.
                    stats_col_ = cur_col_;
                    compute_stats_for(stats_col_);
                    stats_open_ = true;
                    break;
                case 's': {
                    if (text_view_) { copy_status_ = TEXT_NA("s"); break; }
                    // Sort by the active column. Re-pressing on the same column
                    // toggles ascending → descending. Switching columns starts
                    // ascending again.
                    int sc = (cur_col_ >= 0 && cur_col_ < (int)virt_src_col_.size())
                              ? virt_src_col_[cur_col_] : -1;
                    if (sc < 0) break;
                    if (sort_col_ == cur_col_ && !sort_order_.empty()) {
                        sort_desc_ = !sort_desc_;
                    } else {
                        sort_col_ = cur_col_;
                        sort_desc_ = false;
                    }
                    rebuild_display_order();
                    top_row_ = 0;
                    cur_row_ = 0;
                    search_row_ = -1;   // search anchor is now stale
                    break;
                }
                case 'u':
                    // Undo / clear active sort. Filter (if any) stays applied.
                    sort_col_  = -1;
                    sort_desc_ = false;
                    rebuild_display_order();
                    top_row_   = 0;
                    search_row_ = -1;
                    break;
                case 'c':
                    if (text_view_) { copy_status_ = TEXT_NA("c"); break; }
                    col_picker_open_   = true;
                    col_picker_cursor_ = cur_col_;
                    break;
                case 'T':
                    // Open the theme picker. Position the cursor on the
                    // currently-active theme so Enter is a no-op confirmation
                    // and j/k navigate from "where we are".
                    theme_picker_open_   = true;
                    theme_picker_cursor_ = 0;
                    for (int i = 0; i < kNumThemes; ++i)
                        if (kAllThemes[i] == g_theme) {
                            theme_picker_cursor_ = i; break;
                        }
                    break;
                case '\t':         // Tab → next file tab
                    switch_tab(+1);
                    break;
                case KEY_BTAB:     // Shift+Tab → previous file tab
                    switch_tab(-1);
                    break;
                case '[':          // step the slice axis (NPZ 3-D+)
                    apply_slice_change(-1, false, 0);
                    break;
                case ']':
                    apply_slice_change(+1, false, 0);
                    break;
                case '&':
                    // Open the live-filter input bar. Same shape as the
                    // search input bar: collect into filter_input_, commit
                    // on Enter, abort with Esc.
                    filter_mode_   = FilterMode::Input;
                    filter_input_  = filter_expr_str_;  // pre-fill with current
                    filter_err_.clear();
                    break;
                case ':':
                    // Vim-style typed-command prompt at the bottom. Supports
                    // :<N> (jump to row), :q / :quit, :theme NAME. Errors
                    // stay in the bar so the user can edit and retry.
                    cmd_mode_  = CmdMode::Input;
                    cmd_input_.clear();
                    cmd_err_.clear();
                    break;
                case 'y': {
                    // Copy the "active cell" — the cell at the top-left of
                    // the visible window — to the system clipboard via
                    // OSC52. The user picks the cell by scrolling: h/l/,/.
                    // for the column, j/k/PgUp/PgDn for the row, then `y`.
                    if (left_col_ < 0 || left_col_ >= num_cols_) break;
                    auto vals = load_full_row(cur_row_);
                    if (cur_col_ >= (int)vals.size()) break;
                    const std::string& v = vals[cur_col_];
                    if (v.empty()) break;
                    osc52_copy(v);
                    std::string preview = v;
                    if (display_width(preview) > 50) {
                        preview.resize(47);
                        preview += "...";
                    }
                    copy_status_ = "copied: " + preview;
                    break;
                }
                case '.':
                    col_widths_[cur_col_] = std::min(256, col_widths_[cur_col_] + 4);
                    break;
                case ',':
                    col_widths_[cur_col_] = std::max(1, col_widths_[cur_col_] - 4);
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
        g_tui_active = 0;
        endwin();
        // Hand the signals back to whatever was there before the TUI ran and
        // restore the original mask.
        signal(SIGINT,  prev_int);
        signal(SIGTERM, prev_term);
        signal(SIGHUP,  prev_hup);
        sigprocmask(SIG_UNBLOCK, &tui_sigs, nullptr);
        delscreen(scr);
        return true;
    }
};

#endif  // VV_CORE_LIB (end of ncurses TUI frontend)

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

// Returns "" on success, an error message otherwise, so a bad --select /
// --filter exits non-zero instead of printing and exiting 0.
static std::string print_vertical_table(TabularSource& src, const Config& cfg) {
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
    return "";
}

// Returns "" on success, an error message otherwise, so a bad --select /
// --filter exits non-zero instead of printing and exiting 0.
static std::string print_table(TabularSource& src, const Config& cfg) {
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
    if (!data) return "";
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
    return "";
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
static std::string format_label_of(TabularSource& src) {
    const std::string f = src.footer();
    const std::string key = "Format: ";
    auto p = f.find(key);
    if (p == std::string::npos) return "";
    std::string rest = f.substr(p + key.size());
    auto bar = rest.find("  |");
    if (bar != std::string::npos) rest.erase(bar);
    while (!rest.empty() && std::isspace((unsigned char)rest.back())) rest.pop_back();
    return rest;
}

static void emit_schema_json(TabularSource& src, const std::string& fmt_name) {
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
        json_emit_string(f->type()->ToString());
        std::printf(", \"nullable\": %s", f->nullable() ? "true" : "false");
        std::printf(", \"hidden\": %s", hidden.count(f->name()) ? "true" : "false");
        std::printf("}");
    }
    std::printf("]}\n");
}

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
enum class DocKind { Markdown = 1, Text = 2 };

static std::string document_flag_error(const Config& cfg, DocKind kind) {
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

static std::string shorten_reader_error(std::string msg);

// Will the interactive viewer take over? The single source of truth for that
// question — main() asks it twice (once to decide whether --tail may stream,
// once to actually launch), and the two must not drift.
static bool tui_wanted(const Config& cfg) {
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
static int emit_text_stream(TabularSource& src, const Config& cfg) {
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
    auto* ts = dynamic_cast<TextSource*>(&src);

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
        if (!ring.empty() && (!ts || ts->final_newline())) std::fputc('\n', stdout);
    } else if (emitted > 0) {
        // The newline after the final line: present unless the file itself
        // ended without one AND we printed all the way to the end.
        if (truncated || !ts || ts->final_newline()) std::fputc('\n', stdout);
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

#ifndef VV_CORE_LIB   // CLI entry point — excluded from libvvcore
int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

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
        auto lc = [](std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        const std::string want = lc(cfg.tab);
        auto matches = [&](const std::string& label) {
            std::string a = lc(label);
            if (a == want) return true;
            return a.size() > want.size() &&
                   a.compare(0, want.size(), want) == 0 &&
                   (a[want.size()] == ' ' || a[want.size()] == '(' ||
                    a[want.size()] == '[');
        };
        std::vector<std::string> avail{ src->tab_label() };
        if (!matches(src->tab_label())) {
            std::unique_ptr<TabularSource> chosen;
            for (auto& sib : src->expand_tabs()) {
                avail.push_back(sib->tab_label());
                if (!chosen && matches(sib->tab_label())) chosen = std::move(sib);
            }
            if (!chosen) {
                std::string list;
                for (auto& a : avail) { if (!list.empty()) list += ", "; list += a; }
                report(cfg.path, "no tab named '" + cfg.tab +
                                 "'; available: " + list);
                return 1;
            }
            src = std::move(chosen);
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
        if (cfg.json_array || cfg.json_lines)
            emit_schema_json(*src, format_label_of(*src));
        else
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

    // --count: row count and exit. Reflects any -r region (baked into
    // total_rows()); with --filter, counts the matching rows via a scan.
    if (cfg.count) {
        int64_t total = 0;
        if (cfg.filter_expr.empty()) {
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
                tbl = apply_filter(tbl, fx, read_set);
                if (tbl) total += tbl->num_rows();
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
            if (auto* sq = dynamic_cast<SqliteSource*>(tab_srcs.back().get())) {
                for (auto& s : sq->open_sibling_tables())
                    tab_srcs.push_back(std::move(s));
            }
            // Spreadsheet (.xlsx today, future .ods): one tab per sheet,
            // sharing the underlying workbook handle.
            if (auto* wb = dynamic_cast<WorkbookSource*>(tab_srcs.back().get())) {
                for (auto& s : wb->open_sibling_sheets())
                    tab_srcs.push_back(std::move(s));
            }
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
                if (auto* sq = dynamic_cast<SqliteSource*>(tab_srcs.back().get())) {
                    for (auto& es : sq->open_sibling_tables())
                        tab_srcs.push_back(std::move(es));
                }
                if (auto* wb = dynamic_cast<WorkbookSource*>(tab_srcs.back().get())) {
                    for (auto& es : wb->open_sibling_sheets())
                        tab_srcs.push_back(std::move(es));
                }
            }
            // The TUI takes ownership of the sources while it runs. If
            // tui.run() fails (e.g. unsupported terminal), reclaim the
            // first source so the non-interactive fall-through paths
            // below can still use *src.
            TableTUI tui(std::move(tab_srcs), cfg);
            if (tui.run()) return 0;
            src = tui.take_first_source();
            if (cfg.interactive)
                std::fprintf(stderr, "error: cannot initialize terminal (missing terminfo?)\n");
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
