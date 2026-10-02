// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Colours and themes, user settings, and cell / width formatting.

#include "internal.hpp"

// ── Colors ────────────────────────────────────────────────────────────────────










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
void detect_term_bg() {
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


const Theme* find_theme(const std::string& name) {
    for (auto* t : kAllThemes) if (name == t->name) return t;
    // Accept a few synonyms for muscle memory.
    if (name == "solarized") return &kThemeSolarizedDark;
    return nullptr;
}

void init_colors() {
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

void strip_ws_inplace(std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    s = s.substr(a, b - a + 1);
}

// Read $XDG/vv/config and populate Config fields whose CLI flag wasn't given.
// Today: only `theme` (empty cfg.theme means "no explicit --theme").
// Missing file / unreadable file is not an error — config is optional.
// Defined out-of-line after the Config struct is fully visible.

// Write key=value into the config file, preserving any existing lines
// (comments, other keys). Atomic via .tmp + rename. Returns true on
// success — best-effort: silent on permission errors / quota exhaustion
// so a failed write doesn't crash the TUI.
bool save_user_setting(const std::string& key, const std::string& value) {
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
const char* type_color(arrow::Type::type t) {
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
arrow::Type::type display_type(const arrow::Field& f) {
    auto t = f.type();
    if (t->id() == arrow::Type::DICTIONARY)
        return std::static_pointer_cast<arrow::DictionaryType>(t)->value_type()->id();
    return t->id();
}

// A column type as vv shows it: Arrow's name, except that a dictionary type
// (a pandas / AnnData categorical, a Parquet dictionary column) reads
// "category[<value type>]" — "category[string]", "category[bool]", with
// ", ordered" for an ordered one — instead of Arrow's
// "dictionary<values=string, indices=int32, ordered=0>".
std::string type_label(const arrow::DataType& t) {
    if (t.id() != arrow::Type::DICTIONARY) return t.ToString();
    const auto& d = static_cast<const arrow::DictionaryType&>(t);
    return "category[" + type_label(*d.value_type()) + (d.ordered() ? ", ordered" : "") + "]";
}

// ── CLI args ─────────────────────────────────────────────────────────────────
// (ColorMode + Config are defined in vv/vvcore.hpp, included near the top.)

// Out-of-line definition of load_user_config — declared further up
// (near the other XDG-config helpers) but needs the full Config struct.
void load_user_config(Config& cfg) {
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
        else if (key == "json_view" && (val == "tree" || val == "table")) {
            // How a JSON file opens on a terminal; --tree / --no-tree win.
            cfg.json_view = val;
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
int effective_threads(const Config& cfg) {
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
int effective_decode_threads(const Config& cfg) {
    if (cfg.decode_threads > 0) {
        unsigned hc = std::thread::hardware_concurrency();
        int cap = hc > 0 ? (int)(hc * 2) : 32;
        return std::min(cfg.decode_threads, cap);
    }
    return effective_threads(cfg);
}


// ── Value formatting ──────────────────────────────────────────────────────────





// True when the environment's effective locale is a UTF-8 one. Checked by
// reading LC_ALL / LC_CTYPE / LANG directly (in POSIX precedence) rather than
// calling setlocale(), which vv leaves at "C" for the non-interactive path and
// which would change locale-dependent formatting globally. No locale set → the
// C locale is in effect, i.e. not UTF-8.
static bool env_locale_is_utf8() {
    for (const char* var : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* v = std::getenv(var);
        if (!v || !*v) continue;
        std::string s;
        for (const char* p = v; *p; ++p) s += (char)std::tolower((unsigned char)*p);
        return s.find("utf-8") != std::string::npos ||
               s.find("utf8")  != std::string::npos;
    }
    return false;
}

// Resolve --box (and the locale default) to the glyph set for the ASCII table.
// Returns an error string for an unknown style.
std::string select_box_glyphs(const std::string& style) {
    if (style.empty())         g_box = env_locale_is_utf8() ? &kBoxUnicode : &kBoxAscii;
    else if (style == "unicode") g_box = &kBoxUnicode;
    else if (style == "ascii")   g_box = &kBoxAscii;
    else return "unknown --box style '" + style + "'; use 'unicode' or 'ascii'";
    return "";
}

std::string repeat_utf8(const char* glyph, int n) {
    std::string s;
    if (n <= 0) return s;
    size_t gl = std::strlen(glyph);
    s.reserve(gl * (size_t)n);
    for (int i = 0; i < n; ++i) s.append(glyph, gl);
    return s;
}

// Base64 (RFC 4648). Used by the OSC52 clipboard escape — small enough
// to avoid pulling in a dependency.
std::string base64_encode(const std::string& in) {
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
void osc52_copy(const std::string& s) {
    std::string esc = "\033]52;c;" + base64_encode(s) + "\a";
    std::fwrite(esc.data(), 1, esc.size(), stdout);
    std::fflush(stdout);
}

bool is_integer_type(arrow::Type::type t) {
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

bool is_numeric_type(arrow::Type::type t) {
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
uint32_t utf8_decode(const std::string& s, size_t i, int* len) {
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
int codepoint_width(uint32_t cp) {
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
std::string sub_display(const std::string& s, int start, int width) {
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
size_t utf8_prefix_for_width(const std::string& s, int max_cols) {
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

// Does truncate(s, max_w) shorten s?
bool truncate_cuts(const std::string& s, int max_w) {
    return (int)display_width(s) > std::max(2, max_w);
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
                // The candidate for n kept elements grows with n, so its display
                // width is monotone — binary-search the largest element count
                // that still fits rather than scanning every count from K down
                // (O(log K) width evaluations instead of O(K), each O(cand len)).
                auto make_cand = [&](size_t n) {
                    std::string cand;
                    cand += open;
                    cand.append(s, 1, commas[n - 1] - 1);  // up to "eN"
                    cand += ", ";
                    cand += g_box->ell;
                    cand += close;
                    return cand;
                };
                size_t lo = 1, hi = commas.size(), best = 0;
                while (lo <= hi) {
                    size_t mid = lo + (hi - lo) / 2;
                    if (display_width(make_cand(mid)) <= max_w) {
                        best = mid; lo = mid + 1;
                    } else {
                        hi = mid - 1;   // mid >= 1, so hi bottoms out at 0
                    }
                }
                if (best >= 1) return make_cand(best);
            }
        }
    }

    // Keep content up to (max_w - marker width) display columns, then append the
    // truncation marker so the whole cell is max_w wide. "…" reserves 1 column,
    // the ASCII "..." reserves 3. Cut on a terminal-column boundary — a
    // byte-based substr would split a multibyte codepoint (invalid UTF-8) and a
    // codepoint-based one would overshoot the column budget for wide chars.
    int ew = display_width(g_box->ell);
    int keep = max_w - ew;
    if (keep < 0) keep = 0;
    return s.substr(0, utf8_prefix_for_width(s, keep)) + g_box->ell;
}

// See vvcore.hpp. Pure width-planning math shared by the TUI and Qt GUI; it is
// unit-agnostic (terminal columns or pixels).
std::vector<int> plan_column_widths(
    const std::vector<std::vector<int>>& per_column_samples,
    const std::vector<int>& floors,
    int viewport,
    const WidthPlanOptions& opt) {
    const size_t n = per_column_samples.size();
    std::vector<int> floor(n), desired(n);
    for (size_t i = 0; i < n; ++i) {
        int fl = (i < floors.size()) ? floors[i] : 0;
        if (fl < opt.min_floor) fl = opt.min_floor;
        floor[i] = fl;

        const std::vector<int>& smp = per_column_samples[i];
        if (smp.empty()) { desired[i] = fl; continue; }

        // Nearest-rank percentile over a sorted copy.
        std::vector<int> w(smp);
        std::sort(w.begin(), w.end());
        int mx = w.back();
        // rank = ceil(p/100 * n), 1-based; index rank-1.
        size_t rank = (size_t)(((int64_t)opt.percentile * (int64_t)w.size() + 99) / 100);
        if (rank < 1) rank = 1;
        if (rank > w.size()) rank = w.size();
        int pw = w[rank - 1];
        // If the longest value is barely over the percentile, showing everyone
        // is nearly free — round up to the max rather than elide a hair.
        int want = (mx <= pw + opt.slack) ? mx : pw;
        if (want > opt.c_max) want = opt.c_max;
        if (want < fl) want = fl;              // never below the header/floor
        desired[i] = want;
    }

    if (viewport <= 0) return desired;         // unbounded — no budget step

    int64_t sum_desired = 0, sum_floor = 0;
    for (size_t i = 0; i < n; ++i) { sum_desired += desired[i]; sum_floor += floor[i]; }
    if (sum_desired <= viewport) return desired;   // everything fits
    if (sum_floor   >= viewport) return floor;     // even floors overflow → scroll

    // Max-min fair water-filling on the excess (desired - floor): find a level L
    // such that sum(min(excess_i, L)) == budget R. Columns whose excess is below
    // L are fully satisfied; the widest columns are capped at floor + L.
    int64_t R = (int64_t)viewport - sum_floor;
    std::vector<int> excess(n);
    for (size_t i = 0; i < n; ++i) excess[i] = desired[i] - floor[i];   // >= 0
    std::vector<int> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b){ return excess[a] < excess[b]; });

    // Walk columns from the smallest excess up. At each step, if raising every
    // still-unsatisfied column to the current column's excess fits R, satisfy it
    // fully; otherwise split the remaining R evenly across the rest.
    std::vector<int> width(floor);
    int64_t remaining = R;
    size_t left = n;
    int prev = 0;
    for (size_t k = 0; k < n; ++k) {
        int e = excess[order[k]];
        int step = e - prev;
        if (step > 0 && (int64_t)step * (int64_t)left > remaining) {
            // Can't raise all `left` columns by `step`; share `remaining` evenly.
            int share = (int)(remaining / (int64_t)left);
            int64_t rem = remaining - (int64_t)share * (int64_t)left;
            for (size_t j = k; j < n; ++j) {
                int col = order[j];
                width[col] = floor[col] + prev + share;
                if (rem > 0) { width[col] += 1; --rem; }   // spread the remainder
            }
            return width;
        }
        remaining -= (int64_t)step * (int64_t)left;
        prev = e;
        width[order[k]] = floor[order[k]] + e;   // fully satisfied
        --left;
    }
    return width;   // all satisfied (shouldn't happen given sum_desired > viewport)
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
bool parse_rgb(const std::string& s, int* r, int* g, int* b) {
    return std::sscanf(s.c_str(), "%d,%d,%d", r, g, b) == 3 &&
           (unsigned)*r <= 255 && (unsigned)*g <= 255 && (unsigned)*b <= 255;
}

// Map an RGB triplet to the nearest xterm 256-color index (6×6×6 cube, indices 16-231).
int nearest_256(int r, int g, int b) {
    auto q = [](int v) -> int {
        if (v < 48)  return 0;
        if (v < 115) return 1;
        return (v - 35) / 40;   // 115→2, 155→3, 195→4, 235→5
    };
    return 16 + 36 * q(r) + 6 * q(g) + q(b);
}

// Binary cell text. Bytes that are printable UTF-8 (older Parquet writers store
// strings as BINARY with no UTF8 annotation) are shown as that text; anything
// else — control bytes, invalid UTF-8 — as 0x + hex, so raw bytes never reach
// the terminal and distinct blobs render distinctly.
static std::string binary_cell_text(const uint8_t* p, int64_t n) {
    bool text = true;
    for (int64_t i = 0; i < n && text;) {
        const uint8_t c = p[i];
        if (c < 0x20 || c == 0x7f) { text = false; break; }
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
        if (!len || i + len > n) { text = false; break; }
        for (int k = 1; k < len; ++k)
            if ((p[i + k] & 0xc0) != 0x80) { text = false; break; }
        i += len;
    }
    if (text) return std::string(reinterpret_cast<const char*>(p), (size_t)n);
    static const char* hex = "0123456789abcdef";
    std::string out = "0x";
    out.reserve(2 + 2 * (size_t)n);
    for (int64_t i = 0; i < n; ++i) { out += hex[p[i] >> 4]; out += hex[p[i] & 15]; }
    return out;
}

static std::string exact_float_text(double v, bool single) {
    char buf[40];
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
#if defined(__cpp_lib_to_chars) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 11)
    // Shortest round-trip form (Ryu): 2.5x faster than the search below on a
    // column of random doubles, which need all 17 digits.
    auto r = single ? std::to_chars(buf, buf + sizeof buf - 1, (float)v)
                    : std::to_chars(buf, buf + sizeof buf - 1, v);
    if (r.ec == std::errc()) return std::string(buf, r.ptr);
#endif
    for (int p = single ? 6 : 15; p <= (single ? 9 : 17); ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (single ? std::strtof(buf, nullptr) == (float)v : std::strtod(buf, nullptr) == v)
            break;
    }
    return buf;
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
            const float v = static_cast<const arrow::FloatArray&>(arr).Value(row);
            if (t_exact_floats) return exact_float_text(v, /*single=*/true);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", (double)v);
            return buf;
        }
        case arrow::Type::DOUBLE: {
            const double v = static_cast<const arrow::DoubleArray&>(arr).Value(row);
            if (t_exact_floats) return exact_float_text(v, /*single=*/false);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            return buf;
        }
        case arrow::Type::STRING:
            return static_cast<const arrow::StringArray&>(arr).GetString(row);
        case arrow::Type::LARGE_STRING:
            return static_cast<const arrow::LargeStringArray&>(arr).GetString(row);
        case arrow::Type::BINARY: {
            auto v = static_cast<const arrow::BinaryArray&>(arr).GetView(row);
            return binary_cell_text(reinterpret_cast<const uint8_t*>(v.data()), (int64_t)v.size());
        }
        case arrow::Type::LARGE_BINARY: {
            auto v = static_cast<const arrow::LargeBinaryArray&>(arr).GetView(row);
            return binary_cell_text(reinterpret_cast<const uint8_t*>(v.data()), (int64_t)v.size());
        }
        case arrow::Type::FIXED_SIZE_BINARY: {
            auto& a = static_cast<const arrow::FixedSizeBinaryArray&>(arr);
            return binary_cell_text(a.GetValue(row), a.byte_width());
        }
        case arrow::Type::BINARY_VIEW: {
            auto v = static_cast<const arrow::BinaryViewArray&>(arr).GetView(row);
            return binary_cell_text(reinterpret_cast<const uint8_t*>(v.data()), (int64_t)v.size());
        }
        case arrow::Type::STRING_VIEW:
            return std::string(static_cast<const arrow::StringViewArray&>(arr).GetView(row));
        case arrow::Type::EXTENSION: {
            // An extension type renders as its storage (arrow.json: the JSON
            // text), except where the storage alone reads badly: arrow.uuid
            // (16 raw bytes) in canonical 8-4-4-4-12 form, arrow.bool8 (int8)
            // as true / false.
            auto& ea = static_cast<const arrow::ExtensionArray&>(arr);
            const std::string name =
                static_cast<const arrow::ExtensionType&>(*arr.type()).extension_name();
            const auto& st = *ea.storage();
            if (name == "arrow.uuid" && st.type_id() == arrow::Type::FIXED_SIZE_BINARY &&
                static_cast<const arrow::FixedSizeBinaryArray&>(st).byte_width() == 16) {
                const uint8_t* b = static_cast<const arrow::FixedSizeBinaryArray&>(st).GetValue(row);
                static const char* hex = "0123456789abcdef";
                std::string u;
                for (int i = 0; i < 16; ++i) {
                    if (i == 4 || i == 6 || i == 8 || i == 10) u += '-';
                    u += hex[b[i] >> 4]; u += hex[b[i] & 15];
                }
                return u;
            }
            if (name == "arrow.bool8" && st.type_id() == arrow::Type::INT8)
                return static_cast<const arrow::Int8Array&>(st).Value(row) ? "true" : "false";
            return cell_to_string(st, row);
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
        case arrow::Type::STRUCT: {
            // {name: value, ...} like a map. A null field is left out, as a
            // VCF sample leaves out the FORMAT keys it lacks; a null struct
            // is the null symbol.
            auto& sa = static_cast<const arrow::StructArray&>(arr);
            if (sa.IsNull(row)) return NULL_SYMBOL;
            const auto& st = static_cast<const arrow::StructType&>(*sa.type());
            std::string s = "{";
            bool first = true;
            for (int f = 0; f < st.num_fields(); ++f) {
                auto child = sa.field(f);            // offset-adjusted
                if (child->IsNull(row)) continue;
                if (!first) s += ", ";
                first = false;
                s += st.field(f)->name();
                s += ": ";
                s += cell_to_string(*child, row);
            }
            s += "}";
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
            // Arrow's scalar ToString can span lines ("[\n  1\n]"); a cell is
            // one line, so fold the breaks and their indentation to a space.
            auto res = arr.GetScalar(row);
            if (!res.ok()) return "?";
            std::string t = res.ValueOrDie()->ToString(), out;
            out.reserve(t.size());
            for (size_t k = 0; k < t.size(); ++k) {
                if (t[k] == '\n' || t[k] == '\r') {
                    while (k + 1 < t.size() && (t[k + 1] == ' ' || t[k + 1] == '\n')) ++k;
                    if (!out.empty() && out.back() != ' ' && out.back() != '[' && out.back() != '{')
                        out += ' ';
                    continue;
                }
                out += t[k];
            }
            return out;
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
