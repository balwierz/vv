// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// The ncurses viewers: the table viewer and the JSON tree (CLI only).

#include "internal.hpp"

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
    remove_spooled_files();
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
// The last colour pair is kept for the JSON viewer's string colour; the
// table viewer's on-demand pairs stop below it. Pairs are applied with
// COLOR_PAIR(), whose attribute field holds 8 bits: with the wide library
// COLOR_PAIRS is 65536 on xterm-256color, and a higher pair would alias pair
// n & 255 (uninitialised: black on black), so the range stops at 255.
static int tui_reserved_pair() { return std::min(COLOR_PAIRS, 256) - 1; }

// One terminal session for the ncurses viewers: the SCREEN, the handlers that
// restore the terminal on SIGINT / SIGTERM / SIGHUP, and the key reader that
// swallows late terminal replies. A session can host several viewers in turn
// (the JSON tree and the table view it switches to) without the screen
// flickering back to the shell in between.
class TuiSession {
    SCREEN*          scr_ = nullptr;
    std::FILE*       in_  = nullptr;
    bool             own_in_ = false;
    sigset_t         sigs_;
    void (*prev_int_)(int)  = SIG_DFL;
    void (*prev_term_)(int) = SIG_DFL;
    void (*prev_hup_)(int)  = SIG_DFL;
public:
    TuiSession() = default;
    TuiSession(const TuiSession&) = delete;
    TuiSession& operator=(const TuiSession&) = delete;
    ~TuiSession() { close(); }

    // Start ncurses on stdout, reading keys from `in` (stdin when null; the
    // controlling terminal when stdin carries the data). false: the terminal
    // could not be initialised.
    bool open(std::FILE* in = nullptr) {
        setlocale(LC_ALL, "");
        static bool bg_detected = false;
        if (!bg_detected) { detect_term_bg(); bg_detected = true; }   // OSC 11 query, once
        in_ = in ? in : stdin;
        scr_ = newterm(nullptr, stdout, in_);
        if (!scr_) return false;
        set_term(scr_);
        // Restore the terminal if we're killed while owning it. Handlers stay
        // installed for the whole session; the signals are blocked except
        // around getch() so endwin() only runs from a safe context.
        g_tui_active = 1;
        prev_int_  = signal(SIGINT,  tui_signal_restore);
        prev_term_ = signal(SIGTERM, tui_signal_restore);
        prev_hup_  = signal(SIGHUP,  tui_signal_restore);
        sigemptyset(&sigs_);
        sigaddset(&sigs_, SIGINT);
        sigaddset(&sigs_, SIGTERM);
        sigaddset(&sigs_, SIGHUP);
        sigprocmask(SIG_BLOCK, &sigs_, nullptr);
        noecho(); cbreak(); keypad(stdscr, TRUE); curs_set(0);
        set_escdelay(25);
        // Mouse: scroll wheel (BUTTON4 / BUTTON5) + click and double-click.
        // Adding click events means the terminal switches into application
        // mouse mode and stops handling its own drag-to-select; Shift+drag
        // still works as the escape hatch in every modern emulator.
        mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED
                | BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
        // 200 ms is long enough for an unhurried double-click but short
        // enough that a deliberate pair-of-clicks isn't mistaken for one.
        mouseinterval(200);
        return true;
    }
    // Open the controlling terminal for keys (the data arrives on stdin).
    bool open_tty() {
        std::FILE* tty = std::fopen("/dev/tty", "r");
        if (!tty) return false;
        own_in_ = true;
        if (!open(tty)) { std::fclose(tty); in_ = nullptr; own_in_ = false; return false; }
        return true;
    }
    void close() {
        if (!scr_) return;
        g_tui_active = 0;
        endwin();
        // Hand the signals back to whatever was there before the TUI ran and
        // restore the original mask.
        signal(SIGINT,  prev_int_);
        signal(SIGTERM, prev_term_);
        signal(SIGHUP,  prev_hup_);
        sigprocmask(SIG_UNBLOCK, &sigs_, nullptr);
        delscreen(scr_);
        scr_ = nullptr;
        if (own_in_ && in_) std::fclose(in_);
        in_ = nullptr;
        own_in_ = false;
    }

    // The next key. A late terminal reply must not read as keystrokes.
    // detect_term_bg() sends an OSC 11 background query before ncurses
    // starts and waits ~80 ms; over a slow transport (a jupyter-lab web
    // console proxied through kubernetes, tmux over a laggy ssh) the
    // terminal's reply can outrun that budget and land here instead — where
    // its leading ESC used to hit the Esc-quits binding, so vv exited "by
    // itself" and the tail leaked to the shell as `11;rgb:ffff/ffff/ffff`.
    // After a bare ESC, peek: a string introducer (OSC/DCS/APC/SOS/PM) or a
    // CSI start means the terminal is talking, not the user — swallow
    // through the terminator and read on. Anything else is pushed back, so
    // Esc, double-Esc and Alt+key behave as before. (Signals are allowed
    // only while parked in the blocking getch(): one arriving during draw()
    // is delivered there — in read(), not mid-malloc — where the endwin() in
    // the handler is safe.)
    int read_key() {
        for (;;) {
            sigprocmask(SIG_UNBLOCK, &sigs_, nullptr);
            int ch = ::getch();
            sigprocmask(SIG_BLOCK, &sigs_, nullptr);
            if (ch != 27) return ch;
            timeout(0);
            int nxt = ::getch();
            if (nxt == ERR) { timeout(-1); return 27; }        // lone Esc
            bool str_seq = nxt == ']' || nxt == 'P' || nxt == '_' ||
                           nxt == 'X' || nxt == '^';
            if (!str_seq && nxt != '[') {                      // Alt+key…
                ungetch(nxt);
                timeout(-1);
                return 27;
            }
            // The reply may still be trickling in over the transport that
            // delayed it; allow 50 ms between bytes, cap the total.
            timeout(50);
            if (str_seq) {                    // …until BEL or ST (ESC \)
                int prev = 0;
                for (int i = 0; i < 4096; ++i) {
                    int c = ::getch();
                    if (c == ERR || c == '\a' || (prev == 27 && c == '\\'))
                        break;
                    prev = c;
                }
            } else {                          // CSI: …until a final byte
                for (int i = 0; i < 256; ++i) {
                    int c = ::getch();
                    if (c == ERR || (c >= 0x40 && c <= 0x7e)) break;
                }
            }
            timeout(-1);
        }
    }
};

// Each of the above pairs has an optional zebra twin at pair + ZEBRA_OFFSET,
// identical fg but with a dim grey background — applied to odd data rows.
static constexpr int ZEBRA_OFFSET = 100;

// The theme's fixed colour pairs (header, index, null, number, booleans,
// separator, search, plain), shared by the table and the JSON viewer.
static void tui_init_base_pairs() {
    if (!has_colors()) return;
    start_color(); use_default_colors();
    const bool c256 = COLORS >= 256;
    const Theme& t  = *g_theme;
    init_pair(NCP_HEADER, c256 ? t.nc_fg_header : t.nc16_fg_header, -1);
    init_pair(NCP_INDEX,  c256 ? t.nc_fg_index  : t.nc16_fg_index,  -1);
    init_pair(NCP_NULL,   c256 ? t.nc_fg_null   : t.nc16_fg_null,   -1);
    init_pair(NCP_NUMBER, c256 ? t.nc_fg_number : t.nc16_fg_number, -1);
    init_pair(NCP_BOOL_T, c256 ? t.nc_fg_boolt  : t.nc16_fg_boolt,  -1);
    init_pair(NCP_BOOL_F, c256 ? t.nc_fg_boolf  : t.nc16_fg_boolf,  -1);
    init_pair(NCP_SEP,    c256 ? t.nc_fg_sep    : t.nc16_fg_sep,    -1);
    init_pair(NCP_SEARCH, c256 ? t.nc_fg_search : t.nc16_fg_search,
                          c256 ? t.nc_bg_search : t.nc16_bg_search);
    init_pair(NCP_PLAIN,  -1, -1);
}

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


// ── Line editing for the TUI's input bars (/ ? & :) ─────────────────────────
//
// One key applied to `text` with the cursor at byte offset `cur` (npos = end).
// Text is UTF-8: ncurses hands a multi-byte character over as its bytes, each
// inserted at the cursor in turn, and the cursor moves and deletes by whole
// characters. Keys: Left / Right (Ctrl-B / Ctrl-F), Home / End (Ctrl-A /
// Ctrl-E), Backspace, Delete (Ctrl-D), Ctrl-U (to line start), Ctrl-K (to
// line end), Ctrl-W (the word before the cursor). Returns true when the key
// was an edit or a cursor move.
static bool line_edit_key(std::string& text, size_t& cur, int ch) {
    if (cur > text.size()) cur = text.size();
    auto cont = [&](size_t i) { return i < text.size() && ((unsigned char)text[i] & 0xC0) == 0x80; };
    auto prev = [&](size_t i) { if (i == 0) return i; --i; while (i > 0 && cont(i)) --i; return i; };
    auto next = [&](size_t i) { if (i >= text.size()) return text.size(); ++i; while (cont(i)) ++i; return i; };
    switch (ch) {
        case KEY_LEFT:  case 2:  cur = prev(cur); return true;
        case KEY_RIGHT: case 6:  cur = next(cur); return true;
        case KEY_HOME:  case 1:  cur = 0; return true;
        case KEY_END:   case 5:  cur = text.size(); return true;
        case KEY_BACKSPACE: case 127: case 8: {
            size_t p = prev(cur);
            text.erase(p, cur - p); cur = p; return true;
        }
        case KEY_DC: case 4:
            text.erase(cur, next(cur) - cur); return true;
        case 21: text.erase(0, cur); cur = 0; return true;              // Ctrl-U
        case 11: text.erase(cur); return true;                          // Ctrl-K
        case 23: {                                                      // Ctrl-W
            size_t p = cur;
            while (p > 0 && text[p - 1] == ' ') --p;
            while (p > 0 && text[p - 1] != ' ') --p;
            text.erase(p, cur - p); cur = p; return true;
        }
        default: break;
    }
    if (ch >= 32 && ch < 256 && ch != 127) {       // printable ASCII or a UTF-8 byte
        text.insert(cur, 1, (char)ch);
        ++cur;
        return true;
    }
    return false;
}

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
    std::vector<bool>       width_manual_;   // per virtual col: user resized via `,`/`.`
    bool                    autosized_ = false;  // string widths fitted to content once
    int                     tui_cap_ = 50;   // per-column ceiling for auto-fit (>= -w)
    bool                    w_set_   = false; // -w given: it caps every tab
    // The cell / width cap of the active tab: none for a tab that asks for
    // its cells in full (an AnnData summary), unless -w was given.
    int cell_cap() const {
        return (!w_set_ && src_ && src_->show_cells_in_full()) ? (1 << 20) : tui_cap_;
    }
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
    bool start_applied_ = false;   // apply_start_view() ran (once per viewer)
    bool tree_return_ = false;     // opened from the JSON tree: `t` goes back
    bool back_to_tree_ = false;    // the viewer closed with `t`
    std::string tree_request_;     // `t` on a JSON file's tab: the file to view as a tree
    int get_fg_pair(int fg, int bg) {
        if (next_rgb_pair_ >= tui_reserved_pair()) return 0;
        int key = ((fg + 1) << 9) | (bg + 1);
        auto it = fg_pair_.find(key);
        if (it != fg_pair_.end()) return it->second;
        int pair = next_rgb_pair_++;
        init_pair(pair, (short)fg, (short)bg);
        fg_pair_[key] = pair;
        return pair;
    }

    int get_rgb_pair(int r, int g, int b) {
        if (COLORS < 256 || next_rgb_pair_ >= tui_reserved_pair()) return 0;
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

    // ── Value-count sheet (`F`) ──────────────────────────────────────────────
    // How often each value of the cursor's column occurs among the rows the
    // live filter keeps, most frequent first; Enter narrows the filter to one.
    struct FreqEntry { std::string value; bool is_null; int64_t count; };
    static constexpr size_t    kFreqMaxDistinct = 200000;
    bool                       freq_open_   = false;
    int                        freq_col_    = -1;    // virtual column index
    std::vector<FreqEntry>     freq_rows_;
    int64_t                    freq_total_  = 0;     // rows counted
    int64_t                    freq_other_  = 0;     // rows holding a value past the cap
    int                        freq_cursor_ = 0;
    int                        freq_top_    = 0;
    std::string                freq_note_;           // why Enter did nothing

    // ── Column show/hide picker (`c`) ────────────────────────────────────────
    bool                       col_picker_open_     = false;
    int                        col_picker_cursor_   = 0;
    std::vector<bool>          col_visible_;        // [virt_col] — true = shown
    std::vector<int>           start_select_;       // --select for the first file
    TuiStart                   start_;              // --filter / --sort, until run()

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
        std::vector<bool>               width_manual;  // columns the user resized (`,`/`.`)
        bool                            autosized = false;  // string widths fitted once
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
    size_t                     filter_cur_ = std::string::npos;  // edit point (npos = end)
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
    size_t                     cmd_cur_ = std::string::npos;
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
    size_t      search_cur_ = std::string::npos;
    std::string search_query_;   // committed query (empty = no active search)
    std::string search_query_lc_;  // search_query_ lowercased once, for matching
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
        const std::string& q = search_query_lc_;

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
        // any chunk, so a chunk is scanned whole the first time a row of it
        // comes up and its matching rows are recorded; every later row of
        // that chunk is a bit lookup. Each chunk is decoded at most once per
        // search, however the sort interleaves them, and a match near the
        // cursor still stops the scan early.
        if (!sort_order_.empty()) {
            int64_t N = (int64_t)sort_order_.size();
            int64_t src_rows = 0;
            if (nc > 0) {
                auto last = src_->chunk_meta(nc - 1);
                src_rows = last.first_row + last.num_rows;
            }
            std::vector<uint8_t> scanned((size_t)nc, 0);
            std::vector<bool>    hit((size_t)src_rows, false);
            int64_t r = from_row;
            int64_t end = forward ? N : -1;
            int64_t step = forward ? +1 : -1;
            int64_t scanned_rows = 0;
            for (; r != end; r += step) {
                if (r < 0 || r >= N) break;
                if ((scanned_rows++ & 8191) == 0) {
                    mvprintw(scr_r_-1, 0, " Searching (sorted)… row %lld ",
                             (long long)r);
                    clrtoeol(); refresh();
                }
                int64_t srow = sort_order_[r];
                if (srow < 0 || srow >= src_rows) continue;
                int c = chunk_for_row(srow);
                if (c < 0 || c >= nc) continue;
                if (!scanned[(size_t)c]) {
                    scanned[(size_t)c] = 1;
                    auto meta = src_->chunk_meta(c);
                    std::shared_ptr<arrow::Table> tbl;
                    if (src_->read_chunk(c, all_cols, &tbl).ok() && tbl)
                        for (int64_t l = 0; l < tbl->num_rows(); ++l)
                            if (meta.first_row + l < src_rows && row_matches(tbl, l))
                                hit[(size_t)(meta.first_row + l)] = true;
                }
                if (hit[(size_t)srow]) return r;
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

    // Commit a search: find the next match from the cursor, and move the cell
    // cursor onto it (ensure_cursor_visible() then scrolls the view). Moving
    // only the viewport did nothing: the next draw scrolled back to the cursor.
    void do_search(bool forward) {
        if (search_query_.empty()) return;
        // A fresh search includes the cursor's row; n / N step past it. The
        // cursor, not the last match, is the anchor, so moving it between
        // searches continues from where the user is.
        int64_t start = (search_row_ >= 0) ? (forward ? cur_row_ + 1 : cur_row_ - 1)
                                           : cur_row_;
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
            cur_row_     = found;
            // Put the cursor on a matching cell: keep its column if that cell
            // matches, else the first column that does.
            auto vals = load_full_row(found);
            const std::string& q = search_query_lc_;
            if (cur_col_ < 0 || cur_col_ >= (int)vals.size() || !cell_matches(vals[cur_col_], q))
                for (int c = 0; c < (int)vals.size(); ++c)
                    if (cell_matches(vals[c], q)) { cur_col_ = c; break; }
            // Bring a match that is off screen to the top of the view.
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
    // at row offset `local` (the highlight check; consults decoded row groups
    // only).
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
        return cached_row_matches(cr, local, search_query_lc_);
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
                return truncate(std::move(formatted), cell_cap());
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
            // Only integer columns are re-fitted per frame — their width tracks
            // the digits actually on screen, unbounded, so a huge value that is
            // not visible doesn't reserve space. String / list / other columns
            // are sized once to their content by autosize_string_columns() and
            // then left stable; a column the user resized (`,`/`.`) is pinned.
            if (!is_integer_[vc] || (vc < (int)width_manual_.size() && width_manual_[vc]))
                continue;
            // The header block is three rows — name, type, rule — and the type
            // row is drawn at the same width, so the header and (capped) type
            // string are a floor.
            int w = (int)display_width(col_names_[vc]);
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
                // Format through the same path draw_data_row uses and memoize it
                // for the render pass — cell_at returns integers untruncated, so
                // this is the true display width.
                std::string val = cell_at(cr, local, vc, nullptr, nullptr, 0);
                frame_cells_[frame_key(srow, vc)] = val;
                if (val != NULL_SYMBOL) {
                    int ww = display_width(val);
                    if (ww > w) w = ww;
                }
            }
            col_widths_[vc] = w;
        }
    }

    // Size every non-integer column once, when the first chunk is loaded, to the
    // 95th percentile of a sample of its rendered cells (capped at tui_cap_), so
    // the common value shows in full and the occasional long one elides. Integer
    // columns keep the per-frame fit above; a manually resized column is left
    // alone. Runs once per tab (autosized_), and only after data is available.
    void autosize_string_columns() {
        if (autosized_ || num_cols_ == 0) return;
        bool any = false;
        std::vector<int> floors(num_cols_);
        for (int vc = 0; vc < num_cols_; ++vc) {
            int fl = (int)display_width(col_names_[vc]);
            if (vc < (int)col_types_str_.size())
                fl = std::max(fl, std::min(14, (int)display_width(col_types_str_[vc])));
            int sc = virt_src_col_[vc];
            fl = std::max(fl, (sc >= 0) ? src_->min_col_width(sc) : 4);
            floors[vc] = fl;
            if (!is_integer_[vc] && !(vc < (int)width_manual_.size() && width_manual_[vc]))
                any = true;
        }
        if (!any) { autosized_ = true; return; }   // nothing to size

        const int64_t nrows = total_rows();
        const int64_t K = (nrows > 0) ? std::min<int64_t>(nrows, 2000) : 2000;
        std::vector<std::vector<int>> samples(num_cols_);
        bool got_data = false;
        for (int64_t r = 0; r < K; ++r) {
            int c = chunk_for_row(r);
            auto it = cache_.find(c);
            if (it == cache_.end() || !it->second.ok) continue;
            const CachedRG& cr = it->second;
            int64_t local = r - cr.first_row;
            if (local < 0 || local >= cr.num_rows) continue;
            got_data = true;
            for (int vc = 0; vc < num_cols_; ++vc) {
                if (is_integer_[vc]) continue;
                if (vc < (int)width_manual_.size() && width_manual_[vc]) continue;
                std::string val = cell_at(cr, local, vc, nullptr, nullptr, 0);
                if (val != NULL_SYMBOL) samples[vc].push_back((int)display_width(val));
            }
        }
        if (!got_data) return;   // first chunk not loaded yet — retry next frame

        WidthPlanOptions opt;
        const bool full = cell_cap() > tui_cap_;
        opt.percentile = full ? 100 : 95;
        opt.c_max      = cell_cap();
        opt.slack      = 2;
        opt.min_floor  = 4;
        std::vector<int> w = plan_column_widths(samples, floors, 0, opt);
        for (int vc = 0; vc < num_cols_ && vc < (int)w.size(); ++vc) {
            if (is_integer_[vc]) continue;
            if (vc < (int)width_manual_.size() && width_manual_[vc]) continue;
            col_widths_[vc] = w[vc];
        }
        autosized_ = true;
    }

    void prefetch_visible(const std::vector<int>& visible_virt_cols) {
        // A source that reads nothing until asked (JSON) has no chunk on the
        // first paint: read one, then load its columns like any other.
        if (src_->num_chunks() == 0) {
            src_->ensure(0);
            if (src_->num_chunks() == 0) return;
        }
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
        if (total_rows() < 0) {
            // A stream not yet read to the end: read on until the loaded rows
            // reach the bottom of the viewport (a batch can hold fewer rows
            // than the screen — long FASTQ reads close one early), or it ends.
            for (;;) {
                const int n = src_->num_chunks();
                const auto last = src_->chunk_meta(n - 1);
                if (last.first_row + last.num_rows > bot) break;
                src_->ensure(n);
                if (src_->num_chunks() == n) break;          // end of stream
            }
        } else {
            src_->ensure(chunk_for_row(std::max(bot, top_row_)));
        }
        if (bot > top_row_) {
            const int bot_chunk = chunk_for_row(bot);
            for (int c = top_chunk + 1; c <= bot_chunk; ++c) ensure_cols(c, src_cols);
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
            if (x + w + 2 > scr_c_) {
                // A full-cell tab's last column (the summary's values) shows
                // clipped at the screen edge rather than not at all; Enter
                // opens the whole row.
                if (cell_cap() > tui_cap_ && !v.empty() && scr_c_ - x - 2 >= 8)
                    v.push_back({c, x, scr_c_ - x - 2});
                break;
            }
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
            bool has_ell = val.size() >= 3 && val.compare(val.size()-3, 3, g_box->ell) == 0;
            std::string base = has_ell ? val.substr(0, val.size()-3) : val;
            if ((int)base.size() > w - 1) base.resize(w - 1);
            return base + g_box->ell;
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

    // An input bar on the bottom line: `prefix`, the text, then `err`, with
    // the terminal cursor at the edit point. Widths are display columns, and
    // text wider than the line scrolls so the cursor stays on screen.
    void draw_input_bar(char prefix, const std::string& text, size_t cur,
                        const std::string& err) {
        if (cur > text.size()) cur = text.size();
        const int avail = std::max(1, scr_c_ - 2);          // prefix + cursor cell
        size_t from = 0;                                     // first byte shown
        while ((int)display_width(text.substr(from, cur - from)) > avail) {
            ++from;
            while (from < text.size() && ((unsigned char)text[from] & 0xC0) == 0x80) ++from;
        }
        std::string shown = text.substr(from);
        if (!err.empty()) shown += "    !! " + err;
        std::string bar = std::string(1, prefix) + truncate(shown, scr_c_ - 1);
        int w = (int)display_width(bar);
        if (w < scr_c_) bar += std::string((size_t)(scr_c_ - w), ' ');
        move(scr_r_ - 1, 0);
        addstr(bar.c_str());
        curs_set(1);
        move(scr_r_ - 1, 1 + (int)display_width(text.substr(from, cur - from)));
    }

    void draw_status(const std::vector<ColVis>& vc) {
        // ── Search-input mode: show a vim-style search bar ───────────────────
        if (search_mode_ == SearchMode::Input) {
            draw_input_bar(search_dir_forward_ ? '/' : '?', search_input_, search_cur_, "");
            return;
        }
        // ── Filter-input mode: `&<expression>`; show parse error if any ─────
        if (filter_mode_ == FilterMode::Input) {
            draw_input_bar('&', filter_input_, filter_cur_, filter_err_);
            return;
        }
        // ── Command-line input: `:command`; show parse error if any ─────────
        if (cmd_mode_ == CmdMode::Input) {
            draw_input_bar(':', cmd_input_, cmd_cur_, cmd_err_);
            return;
        }
        curs_set(0);

        // ── Normal status bar ────────────────────────────────────────────────
        int64_t tr  = total_rows();
        int64_t bot = top_row_ + (int64_t)data_lines();
        if (tr >= 0) {
            bot = std::min(bot, tr);
        } else if (src_ && src_->num_chunks() > 0 && sort_order_.empty() && !filter_active_) {
            // Not fully read yet: end the range at the rows loaded so far, not
            // at the bottom of the viewport ("Row 1-40/?" over 30 rows).
            const auto last = src_->chunk_meta(src_->num_chunks() - 1);
            bot = std::min(bot, last.first_row + last.num_rows);
        }

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
        cs.type = type_label(*field->type());
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

    // Count the values of virtual column `virt_col` over the rows the live
    // filter keeps. Values are the text a filter literal would need (floats
    // in their exact form), so Enter can filter to one. Past kFreqMaxDistinct
    // distinct values, rows holding a new value are only counted in total.
    void compute_freq_for(int virt_col) {
        freq_rows_.clear(); freq_total_ = freq_other_ = 0;
        freq_cursor_ = freq_top_ = 0; freq_note_.clear();
        int sc = (virt_col >= 0 && virt_col < (int)virt_src_col_.size())
                  ? virt_src_col_[virt_col] : -1;
        if (sc < 0) return;
        src_->set_retain_all(true);  // counting reads every chunk: keep them
        drain_to_eof();
        note_full_pass();
        ExactFloats exact;
        const std::string& info_key = virt_info_key_[virt_col];
        std::vector<int> need = {sc};
        if (filter_active_)
            for (int fc : union_with_filter({}, filter_fx_))
                if (std::find(need.begin(), need.end(), fc) == need.end())
                    need.push_back(fc);
        std::unordered_map<std::string, int64_t> counts;
        int64_t nulls = 0;
        int nc = src_->num_chunks();
        for (int c = 0; c < nc; ++c) {
            mvprintw(scr_r_-1, 0, " Counting values… chunk %d/%d ", c+1, nc);
            clrtoeol(); refresh();
            std::shared_ptr<arrow::Table> tbl;
            if (!src_->read_chunk(c, need, &tbl).ok()) continue;
            auto col = tbl->column(0);
            int64_t row = 0;
            for (auto& ch : col->chunks()) {
                for (int64_t r = 0; r < ch->length(); ++r, ++row) {
                    if (filter_active_) {
                        bool keep = false;
                        for (const auto& clause : filter_fx_.groups) {
                            bool all = true;
                            for (const auto& a : clause)
                                if (!eval_atom(*tbl, row, a, need)) { all = false; break; }
                            if (all) { keep = true; break; }
                        }
                        if (!keep) continue;
                    }
                    ++freq_total_;
                    if (ch->IsNull(r)) { ++nulls; continue; }
                    std::string v = cell_to_string(*ch, r);
                    if (!info_key.empty()) {
                        bool found = false;
                        for (auto& kv : parse_kv_list(v))
                            if (kv.first == info_key) {
                                v = kv.second.empty() ? "true" : kv.second;
                                found = true; break;
                            }
                        if (!found) { ++nulls; continue; }
                    }
                    auto it = counts.find(v);
                    if (it != counts.end())                  ++it->second;
                    else if (counts.size() < kFreqMaxDistinct) counts.emplace(std::move(v), 1);
                    else                                      ++freq_other_;
                }
            }
        }
        freq_rows_.reserve(counts.size() + 1);
        for (auto& [v, n] : counts) freq_rows_.push_back({v, false, n});
        if (nulls) freq_rows_.push_back({"(null)", true, nulls});
        std::sort(freq_rows_.begin(), freq_rows_.end(),
                  [](const FreqEntry& a, const FreqEntry& b) {
                      if (a.count != b.count) return a.count > b.count;
                      if (a.is_null != b.is_null) return b.is_null;   // null last among ties
                      return a.value < b.value;
                  });
    }

    int freq_visible_lines() const { return std::max(1, std::min(scr_r_ - 2, 30) - 6); }

    void draw_freq_overlay() {
        const int lines = freq_visible_lines();
        auto with_sep = [](int64_t v) { return digits_with_sep(std::to_string(v)); };
        std::string summary = with_sep(freq_total_) + " rows" +
                              (filter_active_ ? " (filtered)" : "") + ", " +
                              with_sep((int64_t)freq_rows_.size()) + " values";
        if (freq_other_)
            summary += "; " + with_sep(freq_other_) + " rows hold values past the first " +
                       with_sep((int64_t)kFreqMaxDistinct);
        if (partial_pass_) summary += "; PARTIAL (batches released)";
        const std::string hint = freq_note_.empty()
            ? std::string("Enter: filter to value   Esc: close") : freq_note_;
        int w_n = 5;
        for (const auto& e : freq_rows_) w_n = std::max(w_n, (int)with_sep(e.count).size());
        const std::string title = " value counts: " + col_names_[freq_col_] + " ";
        int w_v = 0;
        for (int i = 0; i < (int)freq_rows_.size() && i < 500; ++i)
            w_v = std::max(w_v, (int)display_width(freq_rows_[i].value));
        int inner = std::max({w_n + 2 + 6 + 2 + w_v, (int)display_width(title),
                              (int)display_width(summary), (int)display_width(hint)});
        int panel_w = std::min(inner + 4, scr_c_);
        const int shown = std::min(lines, std::max(1, (int)freq_rows_.size()));
        int panel_h = std::min(shown + 5, scr_r_);   // borders, summary, header, hint
        int y0 = std::max(0, (scr_r_ - panel_h) / 2);
        int x0 = std::max(0, (scr_c_ - panel_w) / 2);
        {
            int title_w = (int)display_width(title);
            int rest = std::max(0, panel_w - 2 - title_w);
            std::string top = BOX_TL;
            for (int i = 0; i < rest / 2; ++i) top += BOX_HLINE;
            top += title;
            for (int i = 0; i < rest - rest / 2; ++i) top += BOX_HLINE;
            top += BOX_TR;
            nc_str(y0, x0, top, A_BOLD);
        }
        for (int i = 1; i < panel_h - 1; ++i) {
            mvaddstr(y0 + i, x0, BOX_VLINE);
            mvhline(y0 + i, x0 + 1, ' ', panel_w - 2);
            mvaddstr(y0 + i, x0 + panel_w - 1, BOX_VLINE);
        }
        const int avail = panel_w - 4;
        auto put = [&](int yy, std::string text, attr_t attr) {
            if ((int)display_width(text) > avail) text = truncate(text, avail);
            nc_str(yy, x0 + 2, text, attr);
        };
        put(y0 + 1, summary, A_NORMAL);
        char head[64];
        std::snprintf(head, sizeof(head), "%*s  %6s  ", w_n, "count", "%");
        put(y0 + 2, std::string(head) + "value", A_BOLD);
        for (int i = 0; i < shown && freq_top_ + i < (int)freq_rows_.size(); ++i) {
            const auto& e = freq_rows_[freq_top_ + i];
            char buf[64];
            double pct = freq_total_ ? 100.0 * (double)e.count / (double)freq_total_ : 0.0;
            std::snprintf(buf, sizeof(buf), "%*s  %5.1f%%  ", w_n, with_sep(e.count).c_str(), pct);
            std::string line = std::string(buf) + e.value;
            const bool cur = freq_top_ + i == freq_cursor_;
            if (cur) mvhline(y0 + 3 + i, x0 + 1, ' ', panel_w - 2);
            put(y0 + 3 + i, line, cur ? A_REVERSE : (e.is_null ? A_DIM : A_NORMAL));
        }
        put(y0 + panel_h - 2, hint, A_DIM);
        std::string bot = BOX_BL;
        for (int i = 0; i < panel_w - 2; ++i) bot += BOX_HLINE;
        bot += BOX_BR;
        nc_str(y0 + panel_h - 1, x0, bot, A_BOLD);
    }

    // Narrow the live filter to the value under the sheet's cursor: `col ==
    // value` (or `col is null`), ANDed into every OR branch of the current
    // filter. Returns false, with freq_note_ saying why, when it cannot.
    bool apply_freq_filter() {
        if (freq_cursor_ < 0 || freq_cursor_ >= (int)freq_rows_.size()) return false;
        const FreqEntry& e = freq_rows_[freq_cursor_];
        if (!virt_info_key_[freq_col_].empty()) {
            freq_note_ = "an INFO key is filtered with --expand INFO";
            return false;
        }
        auto field = src_->schema()->field(virt_src_col_[freq_col_]);
        const std::string ident = filter_quote_name(field->name());
        std::string atom;
        // A dictionary column compares by its decoded values.
        const arrow::DataType* vt = field->type().get();
        if (vt->id() == arrow::Type::DICTIONARY)
            vt = static_cast<const arrow::DictionaryType&>(*vt).value_type().get();
        const auto id = vt->id();
        const bool number = arrow::is_integer(id) || arrow::is_floating(id) ||
                            arrow::is_decimal(id) || id == arrow::Type::BOOL;
        const bool text = id == arrow::Type::STRING || id == arrow::Type::LARGE_STRING ||
                          is_date_or_timestamp(*vt);
        if (e.is_null) {
            atom = ident + " is null";
        } else if (!number && !text) {
            // Lists, structs, maps, binary and extension columns: == does
            // not compare their values.
            freq_note_ = "a " + type_label(*field->type()) + " column cannot be filtered with ==";
            return false;
        } else if (number) {
            atom = ident + " == " + e.value;
        } else if (e.value.find('"') == std::string::npos) {
            atom = ident + " == \"" + e.value + "\"";
        } else if (e.value.find('\'') == std::string::npos) {
            atom = ident + " == '" + e.value + "'";
        } else {
            freq_note_ = "this value holds both quote characters; no filter can name it";
            return false;
        }
        std::string expr = atom;
        if (filter_active_) {
            expr.clear();
            for (const auto& branch : filter_split_or(filter_expr_str_))
                expr += (expr.empty() ? "" : " OR ") + branch + " AND " + atom;
        }
        FilterExpr fx;
        std::string err;
        if (!parse_filter_expr(expr, *src_->schema(), &fx, &err)) {
            freq_note_ = err;
            return false;
        }
        // Each OR branch must have gained exactly the one new condition.
        bool shape_ok = !filter_active_ || fx.groups.size() == filter_fx_.groups.size();
        for (size_t g = 0; shape_ok && filter_active_ && g < fx.groups.size(); ++g)
            shape_ok = fx.groups[g].size() == filter_fx_.groups[g].size() + 1;
        if (!shape_ok) {
            freq_note_ = "could not combine with the current filter; edit it with &";
            return false;
        }
        filter_fx_       = std::move(fx);
        filter_expr_str_ = expr;
        filter_active_   = true;
        filter_err_.clear();
        rebuild_display_order();
        top_row_ = 0; cur_row_ = 0;
        search_row_ = -1;
        return true;
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
        int panel_w = inner + 4;            // │ label␣␣value │
        int panel_h = (int)rows.size() + 2;  // borders + one line per row
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
            // Columns from the value's start to the space before the border.
            int avail = (x0 + panel_w - 2) - (xx + w_l + 2);
            std::string v = rows[i].second;
            if ((int)display_width(v) > avail && avail > 3)
                v = truncate(v, avail);
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
        t.width_manual   = width_manual_;
        t.autosized      = autosized_;
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
        width_manual_    = t.width_manual;
        autosized_       = t.autosized;
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
        freq_open_ = false;
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
            search_mode_ = SearchMode::None; search_query_.clear(); search_query_lc_.clear(); search_row_ = -1;
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
                cur_row_     = r;   // the cursor, or the next draw scrolls back to it
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
            {"PgUp PgDn  ␣ b","move the cell cursor one page"},
            {"g  G  Home End","first / last row"},
            {"←→  h l",      "move the cell cursor one column"},
            {",  .",          "narrow / widen the cursor's column"},
            {"z",            "toggle frozen first column"},
            {"/  ?",          "search forward / backward (regex, icase)"},
            {"n  N",          "next / previous match (direction-aware)"},
            {"S",             "per-column stats (count/min/max/mean/distinct)"},
            {"F",             "value counts of the cursor's column; Enter filters to one"},
            {"s",             "sort by the cursor's column (toggle asc/desc; u to clear)"},
            {"&",             "live filter: hide non-matching rows; empty input clears"},
            {"c",             "show / hide columns (overlay)"},
            {"y",             "copy the cursor's cell to the clipboard (OSC52)"},
            {"T",             "pick a color theme (saved to ~/.config/vv/config)"},
            {"t",             "JSON: the tree view of the file (t there comes back)"},
            {":",             "command line: :N (jump), :q, :theme NAME, :slice N"},
            {"in / & : bars", "←→ ^B ^F  Home End ^A ^E  Del ^D  ^U ^K ^W (word); UTF-8"},
            {"Tab  Shift-Tab","next / previous tab (with multiple files)"},
            {"[  ]",          "step slice axis (NPZ 3-D+ arrays only)"},
            {"Enter",         "open detail pane for the cursor's row"},
            {"mouse wheel",   "scroll rows"},
            {"mouse click",   "header → sort by column; cell → move the cursor there"},
            {"mouse 2-click", "row → open detail pane"},
            {"Shift+drag",    "select text for OS clipboard (terminal-side)"},
            {"H  F1",         "toggle this help"},
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
        // A value wider than the pane wraps onto continuation lines (under
        // the value, label blank), at a space where there is one, so every
        // value can be read whole.
        std::vector<bool> cont(lines.size(), false);
        {
            const int avail_v = std::max(1, inner_w - label_w - 2);
            std::vector<std::pair<std::string, std::string>> wrapped;
            std::vector<bool> wcont;
            for (auto& [l, v] : lines) {
                if ((int)display_width(v) <= avail_v) {
                    wrapped.emplace_back(l, v); wcont.push_back(false); continue;
                }
                std::string rest = v;
                bool first = true;
                while (!rest.empty()) {
                    size_t cut = utf8_prefix_for_width(rest, avail_v);
                    if (cut < rest.size()) {
                        size_t sp = rest.rfind(' ', cut);
                        if (sp != std::string::npos && sp > cut / 2) cut = sp + 1;
                    }
                    if (cut == 0) cut = 1;
                    std::string piece = rest.substr(0, cut);
                    while (!piece.empty() && piece.back() == ' ') piece.pop_back();
                    // A continuation keeps the label (for the sub-entry
                    // attribute below) but does not print it.
                    wrapped.emplace_back(l, piece);
                    wcont.push_back(!first);
                    first = false;
                    rest.erase(0, cut);
                    while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
                }
            }
            lines = std::move(wrapped);
            cont  = std::move(wcont);
        }
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
                std::string L = cont[idx] ? std::string() : l;
                if ((int)display_width(L) > label_w) L.resize(utf8_prefix_for_width(L, label_w));
                L += std::string(std::max(0, label_w - (int)display_width(L)), ' ');
                std::string V = v;
                int avail_v = inner_w - label_w - 2;  // "label: value"
                if (avail_v < 1) avail_v = 1;
                if ((int)display_width(V) > avail_v) V.resize(utf8_prefix_for_width(V, avail_v));
                V += std::string(std::max(0, avail_v - (int)display_width(V)), ' ');
                line += L + (cont[idx] ? "  " : ": ") + V;
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
        // columns at the right edge, and a column that just came into view
        // has to be loaded too, or it paints blank until the next frame —
        // so repeat until the set is stable (narrowing converges fast).
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<int> virt;
            virt.reserve(vc.size());
            for (auto& c : vc) virt.push_back(c.col);
            prefetch_visible(virt);
            frame_cells_.clear();   // per-frame; populated by the fit pass below
            autosize_string_columns();   // once per tab, when data is available
            fit_widths_to_visible(virt);
            auto next = visible_cols();
            bool same = next.size() == vc.size();
            for (size_t i = 0; same && i < next.size(); ++i)
                same = next[i].col == vc[i].col;
            vc = std::move(next);
            if (same) break;
        }
        draw_banner();
        draw_tabbar();
        if (!text_view_) draw_header(vc);
        int dl = data_lines();
        for (int y = 0; y < dl; ++y)
            draw_data_row(data_top_y() + y, top_row_ + y, vc);
        draw_status(vc);
        draw_detail_pane();  // overlay if detail_row_ >= 0
        if (stats_open_)      draw_stats_overlay();
        if (freq_open_)       draw_freq_overlay();
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
        tui_init_base_pairs();

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
    TableTUI(std::vector<std::unique_ptr<TabularSource>> sources, const Config& cfg,
             const TuiStart& start = {})
        : sources_(std::move(sources)),
          src_(sources_.empty() ? nullptr : sources_[0].get()),
          max_col_w_(cfg.max_col_w),
          tui_cap_(cfg.max_col_w_set ? cfg.max_col_w : std::max(cfg.max_col_w, 50)),
          w_set_(cfg.max_col_w_set),
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
        start_select_ = start.select;
        if (!sources_.empty()) setup_for_active_source();
        start_select_.clear();   // the first file only
        start_ = start;          // filter / sort: applied in run(), with curses up
    }

    // --filter / --sort as the live filter and sort, so `&` shows the
    // expression and Esc / u clear them. main() has validated both. Runs once
    // the screen exists: the full-file pass draws its progress line.
    void apply_start_view() {
        if (sources_.empty()) return;
        FilterExpr fx;
        std::string ferr;
        if (!start_.filter.empty() &&
            parse_filter_expr(start_.filter, *src_->schema(), &fx, &ferr)) {
            filter_fx_       = std::move(fx);
            filter_expr_str_ = start_.filter;
            filter_active_   = true;
        }
        if (!start_.sort_col.empty())
            for (int vc = 0; vc < num_cols_; ++vc)
                if (col_names_[vc] == start_.sort_col && virt_info_key_[vc].empty()) {
                    sort_col_  = vc;
                    sort_desc_ = start_.sort_desc;
                    break;
                }
        if (filter_active_ || sort_col_ >= 0) rebuild_display_order();
        start_ = TuiStart{};
    }

    // (Re)compute every per-tab field from the currently-active source.
    // Used by the constructor and by the lazy-init path when switching to
    // a tab that hasn't been visited yet.
    void setup_for_active_source() {
        auto& src = *src_;
        text_view_ = src.is_text();
        num_cols_ = (max_cols_cfg_ > 0 && start_select_.empty())
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
        const bool pre_expanded = is_expanded_source(src);
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
        // --select (first file): its columns, in its order; a column hidden
        // for display is shown when named. Otherwise every column.
        std::vector<int> order = start_select_;
        if (order.empty())
            for (int sc = 0; sc < src_num_cols_; ++sc) order.push_back(sc);
        for (int sc : order) {
            if (start_select_.empty() && hidden.count(src.schema()->field(sc)->name())) continue;
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
                col_types_str_[vc] = type_label(*src.schema()->field(virt_src_col_[vc])
                                                    ->type());
            } else {
                col_types_str_[vc] = arrow_type_for_id(v_types[vc])->ToString();
            }
        }

        col_widths_.assign(num_cols_, 0);
        width_manual_.assign(num_cols_, false);
        autosized_ = false;
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
            col_widths_[vc]  = is_integer_[vc] ? base : std::min(base, cell_cap());
            right_align_[vc] = is_numeric_type(t);
            is_bool_[vc]     = v_is_bool[vc];
            is_rgb_[vc]      = (col_names_[vc] == "RGB");
        }
        // Mark the active tab as initialised so subsequent switches load
        // from the snapshot rather than re-running setup.
        tabs_[active_tab_].initialised = true;
    }

    // Returns false if the terminal type is not supported (missing terminfo).
    // Opened from the JSON tree viewer: `t` closes the table and goes back.
    void set_tree_return(bool on) { tree_return_ = on; }
    bool back_to_tree() const { return back_to_tree_; }
    // After run_in() returns: the JSON file whose tree `t` asked for, or "".
    const std::string& tree_request() const { return tree_request_; }
    // A message for the status line of the next frame.
    void flash(const std::string& m) { copy_status_ = m; }

    // Run in a session of its own; false: the terminal could not start.
    bool run() {
        TuiSession session;
        if (!session.open()) return false;
        run_in(session);
        return true;
    }

    // Run inside an open session (which outlives this call).
    void run_in(TuiSession& session) {
        // Colour pairs are set up afresh: another viewer may have used the
        // session's pairs, so the on-demand pair caches start empty.
        fg_pair_.clear();
        rgb_pair_.clear();
        next_rgb_pair_ = NCP_PLAIN + 1;
        setup_colors();
        if (!start_applied_) { apply_start_view(); start_applied_ = true; }
        clearok(stdscr, TRUE);
        back_to_tree_ = false;
        tree_request_.clear();

        bool quit = false;
        while (!quit) {
            draw();
            int ch = session.read_key();
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

            // ── Value-count sheet: move, Enter filters, Esc / q / F close ─────
            if (freq_open_) {
                const int n = (int)freq_rows_.size(), page = freq_visible_lines();
                switch (ch) {
                    case 27: case 'q': case 'Q': case 'F':
                        freq_open_ = false; freq_rows_.clear(); break;
                    case KEY_DOWN: case 'j': freq_cursor_ = std::min(n - 1, freq_cursor_ + 1); break;
                    case KEY_UP:   case 'k': freq_cursor_ = std::max(0, freq_cursor_ - 1); break;
                    case KEY_NPAGE: case ' ': freq_cursor_ = std::min(n - 1, freq_cursor_ + page); break;
                    case KEY_PPAGE: case 'b': freq_cursor_ = std::max(0, freq_cursor_ - page); break;
                    case 'g': case KEY_HOME: freq_cursor_ = 0; break;
                    case 'G': case KEY_END:  freq_cursor_ = std::max(0, n - 1); break;
                    case '\n': case '\r': case KEY_ENTER:
                        if (apply_freq_filter()) { freq_open_ = false; freq_rows_.clear(); }
                        break;
                    default: break;
                }
                if (freq_cursor_ < freq_top_) freq_top_ = freq_cursor_;
                if (freq_cursor_ >= freq_top_ + page) freq_top_ = freq_cursor_ - page + 1;
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
                        search_query_lc_ = search_query_;
                        for (auto& c : search_query_lc_)
                            c = (char)std::tolower((unsigned char)c);
                        compile_search();
                        search_mode_  = SearchMode::Active;
                        search_row_   = -1;
                        do_search(search_dir_forward_);
                    } else {
                        search_mode_  = SearchMode::None;
                        search_query_.clear(); search_query_lc_.clear();
                        search_regex_.reset();
                        search_regex_valid_ = false;
                        search_row_   = -1;
                    }
                } else if (ch == 27) {    // Esc — cancel
                    search_mode_  = SearchMode::None;
                    search_input_.clear();
                } else {
                    line_edit_key(search_input_, search_cur_, ch);
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
                } else if (line_edit_key(filter_input_, filter_cur_, ch)) {
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
                } else if (line_edit_key(cmd_input_, cmd_cur_, ch)) {
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
                case '\n': case '\r': case KEY_ENTER:  // Open detail pane for the cursor's row
                    detail_row_    = cur_row_;
                    detail_scroll_ = 0;
                    break;
                case 27:  // Esc: clear search/filter if active, else quit
                    if (search_mode_ == SearchMode::Active) {
                        search_mode_  = SearchMode::None;
                        search_query_.clear(); search_query_lc_.clear();
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
                case 'F':
                    if (text_view_) { copy_status_ = TEXT_NA("F"); break; }
                    freq_col_ = cur_col_;
                    compute_freq_for(freq_col_);
                    freq_open_ = true;
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
                    filter_cur_    = std::string::npos;
                    filter_err_.clear();
                    break;
                case ':':
                    // Vim-style typed-command prompt at the bottom. Supports
                    // :<N> (jump to row), :q / :quit, :theme NAME. Errors
                    // stay in the bar so the user can edit and retry.
                    cmd_mode_  = CmdMode::Input;
                    cmd_input_.clear();
                    cmd_cur_   = std::string::npos;
                    cmd_err_.clear();
                    break;
                case 'y': {
                    // Copy the cell under the cursor to the system clipboard
                    // via OSC52 (h/l, j/k, PgUp/PgDn, a search or :N move
                    // the cursor there first).
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
                    if (cur_col_ < (int)width_manual_.size()) width_manual_[cur_col_] = true;
                    break;
                case ',':
                    col_widths_[cur_col_] = std::max(1, col_widths_[cur_col_] - 4);
                    if (cur_col_ < (int)width_manual_.size()) width_manual_[cur_col_] = true;
                    break;
                case '/':
                    search_mode_  = SearchMode::Input;
                    search_input_.clear();
                    search_cur_   = std::string::npos;
                    search_dir_forward_ = true;
                    break;
                case '?':
                    search_mode_  = SearchMode::Input;
                    search_input_.clear();
                    search_cur_   = std::string::npos;
                    search_dir_forward_ = false;
                    break;
                case 'n':
                    if (!search_query_.empty()) do_search(search_dir_forward_);
                    break;
                case 'N':
                    if (!search_query_.empty()) do_search(!search_dir_forward_);
                    break;
                case 't':
                    if (tree_return_) { back_to_tree_ = true; quit = true; break; }
                    // A JSON file's tab (several files open): its tree view.
                    if (is_json_source(src_)) {
                        struct stat jst;
                        if (::stat(src_->path().c_str(), &jst) == 0 && S_ISREG(jst.st_mode)) {
                            tree_request_ = src_->path();
                            quit = true;
                        }
                    }
                    break;
                case KEY_RESIZE: break;
                default: break;
            }
        }
    }
};

// ── JsonTUI: the JSON tree viewer ────────────────────────────────────────────
//
// A folding tree over a JsonDoc, in jless's "data mode": one row per value,
// keys bare when they are identifiers, array indices shown, a collapsed
// container previewed on its row ({…} 3 keys  {"id": 1, …}), scalars printed
// as written in the file. The cursor and the top of the screen are paths
// (container, child index), so moving costs the same at any file size; only
// the containers on screen are ever indexed. `t` hands over to the table view
// (main() runs TableTUI in the same session and comes back on its `t`).

// Decode a JSON string body (escapes as written) to UTF-8.
static std::string json_unescape(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    auto put_cp = [&](uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    };
    auto hex4 = [&](size_t i, uint32_t* v) {
        if (i + 4 > raw.size()) return false;
        uint32_t x = 0;
        for (size_t k = i; k < i + 4; ++k) {
            const char c = raw[k];
            x <<= 4;
            if (c >= '0' && c <= '9') x |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') x |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') x |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        *v = x;
        return true;
    };
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) { out += c; continue; }
        const char e = raw[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(i + 1, &cp)) { out += "\\u"; break; }
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 2 < raw.size() && raw[i + 1] == '\\' &&
                    raw[i + 2] == 'u') {
                    uint32_t lo;
                    if (hex4(i + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                put_cp(cp);
                break;
            }
            default: out += e;   // \" \\ \/
        }
    }
    return out;
}

// Search: case-insensitive; a literal unless the query uses regex syntax.
using vvjson::JsonSearch;

class JsonTUI {
public:
    enum class Exit { Quit, OpenTable };

    JsonTUI(vvjson::JsonDoc& doc, std::string label) : doc_(doc), label_(std::move(label)) {
        const uint64_t sz = doc_.size();
        depth_ = doc_.root().virt ? 1 : sz <= (256u << 10) ? 99 : sz <= (16u << 20) ? 2 : 1;
        cur_.push_back({doc_.root(), -1});
        top_ = cur_;
    }
    void flash(const std::string& m) { flash_ = m; }

    // What `t` asked for: the whole document, or the array of objects at or
    // above the cursor (records nested in the document, e.g. a GeoJSON
    // .features, a notebook's .cells, a HAR's .log.entries).
    struct TableTarget { bool whole = true; vvjson::JNode node; std::string path; };
    const TableTarget& table_target() const { return table_target_; }

    Exit run_in(TuiSession& session) {
        // ASCII glyphs with the ASCII frame style, or when the active locale
        // is not UTF-8 — a LANG naming a locale the system lacks leaves "C",
        // where ncurses cannot draw them. Decided here, not at construction:
        // TuiSession::open sets the locale, and the tree may be built first.
        const char* cs = nl_langinfo(CODESET);
        ascii_ = (g_box && std::strcmp(g_box->vline, "|") == 0) ||
                 !cs || std::strcmp(cs, "UTF-8") != 0;
        init_pairs();
        clearok(stdscr, TRUE);
        for (;;) {
            draw();
            timeout(doc_.validation() == 0 ? 200 : -1);
            int ch = session.read_key();
            timeout(-1);
            if (ch == ERR) continue;
            if (help_) { help_ = false; continue; }
            if (theme_open_) { theme_key(ch); continue; }
            if (pane_) { pane_key(ch); continue; }
            if (input_) { input_key(ch); if (quit_) return Exit::Quit; continue; }
            const std::string keep_flash = flash_;
            flash_.clear();
            const int page = std::max(1, rows_ - 3);
            switch (ch) {
                case 'q': case 'Q': return Exit::Quit;
                case 27:
                    if (!query_.empty()) { query_.clear(); break; }
                    return Exit::Quit;
                case 't': pick_table_target(); return Exit::OpenTable;
                case KEY_DOWN: case 'j': step(+1); break;
                case KEY_UP:   case 'k': step(-1); break;
                case KEY_NPAGE: case 6 /* ^F */: step(+page); break;
                case KEY_PPAGE: case 'b': case 2 /* ^B */: step(-page); break;
                case 4  /* ^D */: step(+page / 2); break;
                case 21 /* ^U */: step(-page / 2); break;
                case 'g': case KEY_HOME: cur_.assign(1, {doc_.root(), -1}); top_ = cur_; break;
                case 'G': case KEY_END: go_last(); break;
                case KEY_LEFT: case 'h': left(); break;
                case KEY_RIGHT: case 'l': right(); break;
                case ' ': toggle(); break;
                case '\n': case '\r': case KEY_ENTER:
                    if (cur_.back().node.container()) toggle(); else open_pane();
                    break;
                case 'J': sibling(+1); break;
                case 'K': sibling(-1); break;
                case 'e': set_ov(cur_.back().node, 1); break;
                case 'E': clear_ov_inside(cur_.back().node); set_ov(cur_.back().node, 3); break;
                case 'c': set_ov(cur_.back().node, 2); fix_cursor(); break;
                case 'C': clear_ov_inside(cur_.back().node); set_ov(cur_.back().node, 2); fix_cursor(); break;
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    depth_ = ch - '0';
                    ov_.clear();
                    fix_cursor();
                    flash_ = ch == '0' ? "folded to the root" : std::string("folded to depth ") + (char)ch;
                    break;
                case '/': case '?':
                    input_ = true; input_fwd_ = ch == '/'; input_cmd_ = false;
                    input_buf_.clear(); input_cur_ = 0;
                    break;
                case ':':
                    input_ = true; input_cmd_ = true; input_buf_.clear(); input_cur_ = 0;
                    break;
                case '!': goto_error(); break;
                case 'm':
                    lines_ = !lines_;
                    if (!lines_) cur_.back().close = false, top_.back().close = false;
                    flash_ = lines_ ? "line mode: JSON text, % jumps between brackets"
                                    : "data mode";
                    break;
                case '%': match_bracket(); break;
                case 'T':
                    theme_open_ = true;
                    theme_cur_ = 0;
                    for (int i = 0; i < kNumThemes; ++i) if (kAllThemes[i] == g_theme) theme_cur_ = i;
                    break;
                case 'n': if (!query_.empty()) search(fwd_); break;
                case 'N': if (!query_.empty()) search(!fwd_); break;
                case 'y': copy_value(); break;
                case 'p': { const std::string p = path_str(cur_); osc52_copy(p); flash_ = "copied path: " + p; break; }
                case 'Y': copy_key(); break;
                case 'H': case KEY_F(1): help_ = true; break;
                case KEY_MOUSE: mouse(); break;
                case KEY_RESIZE: break;
                default: flash_ = keep_flash; break;
            }
        }
    }

private:
    using JNode = vvjson::JNode;
    using JKind = vvjson::JKind;
    // The last frame of a path may be a container's closing row (line mode).
    struct Frame { JNode node; int64_t idx; bool close = false; };
    using Path = std::vector<Frame>;
    struct Seg { std::string text; int pair; attr_t attr; };

    vvjson::JsonDoc&          doc_;
    std::string               label_;
    int                       depth_ = 1;          // containers above this depth start open
    std::map<uint64_t, uint8_t> ov_;               // 1 open, 2 closed, 3 open with descendants
    Path                      cur_, top_;
    int                       rows_ = 0, cols_ = 0;
    std::string               flash_;
    bool                      help_ = false;
    bool                      quit_ = false;       // ":q"
    bool                      lines_ = false;      // line mode (m): JSON text with closing rows
    TableTarget               table_target_;
    bool                      theme_open_ = false; // the T picker
    int                       theme_cur_ = 0;
    // search
    std::string               query_;
    JsonSearch                pat_;
    bool                      fwd_ = true;
    bool                      input_ = false, input_fwd_ = true, input_cmd_ = false;
    std::string               input_buf_;
    size_t                    input_cur_ = 0;
    // value pane
    bool                      pane_ = false;
    std::vector<std::string>  pane_lines_;
    std::string               pane_value_;
    int                       pane_top_ = 0;
    int                       pair_str_ = 0;
    bool                      ascii_ = false;
    const char* g(const char* utf8, const char* plain) const { return ascii_ ? plain : utf8; }

    static uint64_t ovkey(const JNode& n) { return n.virt ? UINT64_MAX : n.off; }
    int data_rows() const { return std::max(1, rows_ - 2); }

    void init_pairs() {
        tui_init_base_pairs();
        pair_str_ = 0;
        if (has_colors() && COLOR_PAIRS > 16) {
            const std::string name = g_theme ? g_theme->name : "";
            int fg = COLOR_GREEN;
            if (COLORS >= 256)
                fg = name.find("solarized") != std::string::npos ? 64
                   : name.find("light") != std::string::npos ? 28 : 114;
            init_pair((short)tui_reserved_pair(), (short)fg, -1);
            pair_str_ = tui_reserved_pair();
        }
    }

    // ── Folding ────────────────────────────────────────────────────────────
    bool is_open(const Path& p, size_t lvl) const {
        const JNode& n = p[lvl].node;
        if (!n.container() || n.count == 0) return false;
        auto it = ov_.find(ovkey(n));
        if (it != ov_.end()) return it->second != 2;
        for (size_t a = lvl; a-- > 0;) {
            auto jt = ov_.find(ovkey(p[a].node));
            if (jt != ov_.end()) { if (jt->second == 3) return true; break; }
        }
        return (int)lvl < depth_;
    }
    void set_ov(const JNode& n, uint8_t v) { if (n.container()) ov_[ovkey(n)] = v; }
    void clear_ov_inside(const JNode& n) {
        if (n.virt) { ov_.clear(); return; }
        auto it = ov_.upper_bound(n.off);
        while (it != ov_.end() && it->first < n.end && it->first != UINT64_MAX) it = ov_.erase(it);
    }
    // After a fold: keep the cursor (and the top) on a visible node.
    void fix_cursor() {
        auto trim = [&](Path& p) {
            for (size_t lvl = 1; lvl < p.size(); ++lvl)
                if (!is_open(p, lvl - 1)) { p.resize(lvl); p.back().close = false; break; }
            if (p.back().close && !is_open(p, p.size() - 1)) p.back().close = false;
        };
        trim(cur_);
        trim(top_);
    }

    // ── Walking the visible rows ───────────────────────────────────────────
    // A container with a closing row in line mode (not the virtual root of
    // a JSON sequence / JSON Lines, which has no brackets in the file).
    bool has_close_row(const Path& p) const {
        return lines_ && !p.back().node.virt && is_open(p, p.size() - 1);
    }
    bool next(Path& p) {
        if (!p.back().close && is_open(p, p.size() - 1)) {
            JNode c;
            if (doc_.child(p.back().node, 0, &c)) { p.push_back({c, 0}); return true; }
            if (has_close_row(p)) { p.back().close = true; return true; }
        }
        Path q = p;
        q.back().close = false;
        while (q.size() > 1) {
            JNode sib;
            const Frame& par = q[q.size() - 2];
            if (doc_.child(par.node, q.back().idx + 1, &sib)) {
                q.back() = {sib, q.back().idx + 1};
                p = q;
                return true;
            }
            q.pop_back();
            if (has_close_row(q)) { q.back().close = true; p = q; return true; }
        }
        return false;
    }
    // The last row of an open container: its closing row in line mode, its
    // deepest last descendant otherwise.
    void descend_last(Path& p) {
        while (is_open(p, p.size() - 1)) {
            if (has_close_row(p)) { p.back().close = true; return; }
            const JNode n = p.back().node;
            int64_t last = n.count - 1;
            JNode c;
            while (last >= 0 && !doc_.child(n, last, &c)) --last;   // a broken tail
            if (last < 0) break;
            p.push_back({c, last});
        }
    }
    bool prev(Path& p) {
        if (p.back().close) {                    // closing row → the container's last child
            p.back().close = false;
            const JNode n = p.back().node;
            int64_t last = n.count - 1;
            JNode c;
            while (last >= 0 && !doc_.child(n, last, &c)) --last;
            if (last < 0) return true;           // nothing inside: the opening row
            p.push_back({c, last});
            descend_last(p);
            return true;
        }
        if (p.size() == 1) return false;
        Frame& f = p.back();
        if (f.idx == 0) { p.pop_back(); return true; }
        JNode sib;
        if (!doc_.child(p[p.size() - 2].node, f.idx - 1, &sib)) { p.pop_back(); return true; }
        f = {sib, f.idx - 1};
        descend_last(p);
        return true;
    }
    static uint64_t row_pos(const Path& p) {
        const Frame& f = p.back();
        if (f.node.virt) return f.close ? UINT64_MAX : 0;
        return f.close ? f.node.end : f.node.start();
    }
    static bool same(const Path& a, const Path& b) {
        return a.size() == b.size() && a.back().node.start() == b.back().node.start() &&
               a.back().node.virt == b.back().node.virt && a.back().close == b.back().close;
    }
    static bool before(const Path& a, const Path& b) {
        const uint64_t x = row_pos(a), y = row_pos(b);
        return x < y || (x == y && a.size() < b.size());
    }
    void step(int n) {
        for (int i = 0; i < std::abs(n); ++i)
            if (!(n > 0 ? next(cur_) : prev(cur_))) break;
    }
    void ensure_visible() {
        if (before(cur_, top_)) { top_ = cur_; return; }
        Path p = top_;
        for (int i = 0; i < data_rows(); ++i) {
            if (same(p, cur_)) return;
            if (!next(p)) break;
        }
        top_ = cur_;
        for (int i = 0; i < data_rows() - 1; ++i) if (!prev(top_)) break;
    }
    void go_last() {
        cur_.assign(1, {doc_.root(), -1});
        descend_last(cur_);
    }
    void left() {
        if (is_open(cur_, cur_.size() - 1)) {    // also from its closing row
            cur_.back().close = false;
            set_ov(cur_.back().node, 2);
            return;
        }
        if (cur_.size() > 1) cur_.pop_back();
    }
    void right() {
        if (cur_.back().close) return;
        const JNode& n = cur_.back().node;
        if (!n.container() || n.count == 0) return;
        if (!is_open(cur_, cur_.size() - 1)) { set_ov(n, 1); return; }
        JNode c;
        if (doc_.child(n, 0, &c)) cur_.push_back({c, 0});
    }
    void toggle() {
        const JNode& n = cur_.back().node;
        if (!n.container() || n.count == 0) return;
        const bool open = is_open(cur_, cur_.size() - 1);
        cur_.back().close = false;
        set_ov(n, open ? 2 : 1);
    }
    void sibling(int d) {
        if (cur_.size() < 2) return;
        JNode s;
        if (doc_.child(cur_[cur_.size() - 2].node, cur_.back().idx + d, &s))
            cur_.back() = {s, cur_.back().idx + d};
    }
    // The nearest array of objects at or above the cursor; the whole
    // document when that is the root (or there is none).
    void pick_table_target() {
        table_target_ = TableTarget{};
        for (size_t l = cur_.size(); l-- > 1;) {         // level 0 is the root
            const JNode& n = cur_[l].node;
            if (n.kind != JKind::Array || n.count == 0) continue;
            JNode first;
            if (!doc_.child(n, 0, &first) || first.kind != JKind::Object) continue;
            Path upto(cur_.begin(), cur_.begin() + (std::ptrdiff_t)l + 1);
            upto.back().close = false;
            table_target_ = {false, n, path_str(upto)};
            return;
        }
    }
    // "%": between a container's opening and closing rows (line mode).
    void match_bracket() {
        if (!lines_) { flash_ = "% jumps between brackets in line mode (m)"; return; }
        if (cur_.back().close) { cur_.back().close = false; return; }
        if (has_close_row(cur_)) { cur_.back().close = true; return; }
        flash_ = cur_.back().node.container() ? "folded: no closing row" : "not on a bracket";
    }

    // ── Text of a row ──────────────────────────────────────────────────────
    static bool ident(std::string_view k) {
        if (k.empty() || !(std::isalpha((unsigned char)k[0]) || k[0] == '_')) return false;
        for (char c : k) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        return true;
    }
    std::string path_str(const Path& p) const {
        std::string s;
        for (size_t l = 1; l < p.size(); ++l) {
            const JNode& par = p[l - 1].node;
            if (par.kind == JKind::Object && !par.virt) {
                const std::string_view k = doc_.key(p[l].node);
                if (ident(k)) { s += '.'; s += k; }
                else { if (s.empty()) s += '.'; s += "[\""; s += k; s += "\"]"; }
            } else {
                if (s.empty()) s += '.';
                s += "[" + std::to_string(p[l].idx) + "]";
            }
        }
        return s.empty() ? "." : s;
    }
    static std::string clean(std::string_view v) {
        std::string s;
        s.reserve(v.size());
        for (char c : v) s += ((unsigned char)c < 0x20 || c == 0x7f) ? '?' : c;
        return s;
    }
    std::string count_label(const JNode& n) const {
        const char* what = n.virt ? (doc_.root_mode() == vvjson::JsonDoc::Root::Lines ? "line" : "value")
                         : n.kind == JKind::Object ? "key" : "item";
        return std::to_string(n.count) + " " + what + (n.count == 1 ? "" : "s");
    }
    int scalar_pair(JKind k) const {
        switch (k) {
            case JKind::String: return pair_str_;
            case JKind::Number: return NCP_NUMBER;
            case JKind::True:   return NCP_BOOL_T;
            case JKind::False:  return NCP_BOOL_F;
            case JKind::Null:   return NCP_NULL;
            default:            return NCP_BOOL_F;
        }
    }
    // A row as JSON text (line mode): quoted keys, the value or an opening
    // bracket, commas between members, closing rows.
    std::vector<Seg> line_segs(const Path& p, int width) const {
        std::vector<Seg> segs;
        const size_t lvl = p.size() - 1;
        const JNode& n = p.back().node;
        const bool virt_child = lvl > 0 && p[lvl - 1].node.virt;
        const int depth = (int)lvl - (lvl > 0 && p[0].node.virt ? 1 : 0);
        const int indent = std::min(std::max(0, depth) * 2, std::max(0, width / 2));
        const bool obj = n.kind == JKind::Object;
        // A comma when a sibling follows (none between top-level values).
        const bool comma = lvl > 0 && !virt_child && p.back().idx + 1 < p[lvl - 1].node.count;
        segs.push_back({std::string((size_t)indent, ' '), 0, A_NORMAL});
        if (p.back().close) {
            segs.push_back({obj ? "}" : "]", NCP_INDEX, A_NORMAL});
            if (comma) segs.push_back({",", NCP_INDEX, A_NORMAL});
            return segs;
        }
        if (n.container() && n.count > 0 && !is_open(p, lvl))
            segs.push_back({g("▸", "+"), NCP_INDEX, A_NORMAL});
        if (lvl > 0 && p[lvl - 1].node.kind == JKind::Object && !p[lvl - 1].node.virt) {
            segs.push_back({"\"" + clean(doc_.key(n)) + "\"", NCP_HEADER, A_BOLD});
            segs.push_back({": ", NCP_INDEX, A_NORMAL});
        }
        if (n.kind == JKind::Error) {
            segs.push_back({std::string(g("⚠ ", "! ")) + (doc_.has_struct_err() ? doc_.struct_err() : std::string("invalid JSON")) +
                                " at byte " + std::to_string(n.off), NCP_BOOL_F, A_BOLD});
            return segs;
        }
        if (n.container()) {
            const std::string open = obj ? "{" : "[", close = obj ? "}" : "]";
            if (n.virt) segs.push_back({count_label(n), NCP_INDEX, A_NORMAL});
            else if (n.count == 0) segs.push_back({open + close, NCP_INDEX, A_NORMAL});
            else if (is_open(p, lvl)) segs.push_back({open, NCP_INDEX, A_NORMAL});
            else {
                segs.push_back({open + g("…", "...") + close, NCP_INDEX, A_NORMAL});
                if (comma) segs.push_back({",", NCP_INDEX, A_NORMAL});
                segs.push_back({"  " + count_label(n), NCP_INDEX, A_DIM});
                return segs;
            }
            if (comma && (n.count == 0 || !is_open(p, lvl))) segs.push_back({",", NCP_INDEX, A_NORMAL});
            if (n.broken) segs.push_back({g("  ⚠ cut short", "  ! cut short"), NCP_BOOL_F, A_BOLD});
            return segs;
        }
        const std::string_view b = doc_.bytes(n);
        const size_t cap = (size_t)std::max(16, width) * 4;
        segs.push_back({clean(b.substr(0, std::min(b.size(), cap))), scalar_pair(n.kind), A_NORMAL});
        if (b.size() > cap)
            segs.push_back({std::string(g("… (", "... (")) + human_bytes((int64_t)b.size()) + ")", NCP_INDEX, A_DIM});
        if (comma) segs.push_back({",", NCP_INDEX, A_NORMAL});
        if (n.broken) segs.push_back({g("  ⚠ cut short", "  ! cut short"), NCP_BOOL_F, A_BOLD});
        return segs;
    }
    std::vector<Seg> row_segs(const Path& p, int width) const {
        if (lines_) return line_segs(p, width);
        std::vector<Seg> segs;
        const size_t lvl = p.size() - 1;
        const JNode& n = p.back().node;
        const int indent = std::min((int)lvl * 2, std::max(0, width / 2));
        segs.push_back({std::string((size_t)indent, ' '), 0, A_NORMAL});
        if (n.container() && n.count > 0)
            segs.push_back({is_open(p, lvl) ? g("▾ ", "- ") : g("▸ ", "+ "), NCP_INDEX, A_NORMAL});
        else
            segs.push_back({"  ", 0, A_NORMAL});
        if (lvl > 0) {
            const JNode& par = p[lvl - 1].node;
            if (par.kind == JKind::Object && !par.virt) {
                const std::string_view k = doc_.key(n);
                segs.push_back({ident(k) ? std::string(k) : "\"" + clean(k) + "\"", NCP_HEADER, A_BOLD});
                segs.push_back({": ", NCP_INDEX, A_NORMAL});
            } else {
                segs.push_back({std::to_string(p.back().idx) + ": ", NCP_INDEX, A_DIM});
            }
        }
        if (n.kind == JKind::Error) {
            segs.push_back({std::string(g("⚠ ", "! ")) + (doc_.has_struct_err() ? doc_.struct_err() : std::string("invalid JSON")) +
                                " at byte " + std::to_string(n.off), NCP_BOOL_F, A_BOLD});
            return segs;
        }
        if (n.container()) {
            const bool obj = n.kind == JKind::Object;
            const std::string open = n.virt ? "[" : obj ? "{" : "[";
            const std::string close = n.virt ? "]" : obj ? "}" : "]";
            if (n.count == 0) { segs.push_back({open + close, NCP_INDEX, A_NORMAL}); }
            else if (is_open(p, lvl)) {
                segs.push_back({open, NCP_INDEX, A_NORMAL});
                segs.push_back({"  " + count_label(n), NCP_INDEX, A_NORMAL});
            } else {
                segs.push_back({open + g("…", "...") + close, NCP_INDEX, A_NORMAL});
                segs.push_back({" " + count_label(n) + "  ", NCP_INDEX, A_NORMAL});
                if (!n.virt) segs.push_back({clean(doc_.compact(n, (size_t)std::max(8, width) * 2)), NCP_INDEX, A_DIM});
            }
            if (n.broken) segs.push_back({g("  ⚠ cut short", "  ! cut short"), NCP_BOOL_F, A_BOLD});
            return segs;
        }
        const std::string_view b = doc_.bytes(n);
        const size_t cap = (size_t)std::max(16, width) * 4;
        if (b.size() > cap) {
            segs.push_back({clean(b.substr(0, cap)), scalar_pair(n.kind), A_NORMAL});
            segs.push_back({std::string(g("… (", "... (")) + human_bytes((int64_t)b.size()) + ")", NCP_INDEX, A_DIM});
        } else {
            segs.push_back({clean(b), scalar_pair(n.kind), A_NORMAL});
        }
        if (n.broken) segs.push_back({g("  ⚠ cut short", "  ! cut short"), NCP_BOOL_F, A_BOLD});
        return segs;
    }
    bool row_matches(const Path& p) const {
        if (query_.empty() || p.back().close) return false;
        const JNode& n = p.back().node;
        if (p.size() > 1 && doc_.key(n).size() && pat_.match(doc_.key(n))) return true;
        if (n.container() || n.kind == JKind::Error) return false;
        std::string_view b = doc_.bytes(n);
        if (n.kind == JKind::String && b.size() >= 2) b = b.substr(1, b.size() - 2);
        return pat_.match(b.substr(0, std::min<size_t>(b.size(), 1 << 16)));
    }

    // Draw `segs` on row y from column 0, clipped to the width.
    void put_segs(int y, const std::vector<Seg>& segs, bool cursor, bool hit) {
        int x = 0;
        move_to(y, 0);
        for (const Seg& s : segs) {
            if (x >= cols_) break;
            const size_t k = utf8_prefix_for_width(s.text, cols_ - x);
            const std::string t = s.text.substr(0, k);
            attr_t a = s.attr;
            int pr = s.pair;
            if (hit && pr != NCP_SEP && pr != 0 && pr != NCP_INDEX) pr = NCP_SEARCH;
            if (pr) a |= COLOR_PAIR(pr);
            if (cursor) a |= A_REVERSE;
            attron(a);
            mvaddstr(y, x, t.c_str());
            attroff(a);
            x += display_width(t);
        }
        if (cursor && x < cols_) {
            attron(A_REVERSE);
            mvhline(y, x, ' ', cols_ - x);
            attroff(A_REVERSE);
        }
    }
    static void move_to(int y, int x) { ::move(y, x); clrtoeol(); }

    void draw() {
        getmaxyx(stdscr, rows_, cols_);
        erase();
        ensure_visible();
        // Title.
        const JNode& r = doc_.root();
        const size_t slash = label_.find_last_of('/');
        std::string title = " " + (slash == std::string::npos ? label_ : label_.substr(slash + 1)) +
                            "  " + human_bytes((int64_t)doc_.size());
        if (r.virt) title += doc_.root_mode() == vvjson::JsonDoc::Root::Lines ? "  JSON Lines" : "  JSON sequence";
        attron(A_BOLD | COLOR_PAIR(NCP_HEADER));
        mvaddnstr(0, 0, title.c_str(), cols_);
        attroff(A_BOLD | COLOR_PAIR(NCP_HEADER));
        // Rows.
        Path p = top_;
        for (int y = 1; y <= data_rows(); ++y) {
            put_segs(y, row_segs(p, cols_), same(p, cur_), row_matches(p));
            if (!next(p)) break;
        }
        draw_status();
        if (pane_) draw_pane();
        if (theme_open_) draw_theme_picker();
        if (help_) draw_help();
        refresh();
    }

    std::string type_info(const JNode& n) const {
        switch (n.kind) {
            case JKind::Object: return std::string("object") + g(" · ", " - ") + count_label(n);
            case JKind::Array:  return std::string(n.virt ? "document" : "array") + g(" · ", " - ") + count_label(n);
            case JKind::String: return "string";
            case JKind::Number: return "number";
            case JKind::True: case JKind::False: return "boolean";
            case JKind::Null:   return "null";
            default:            return "error";
        }
    }
    void draw_status() {
        const int y = rows_ - 1;
        move_to(y, 0);
        if (input_) {
            const std::string prompt = input_cmd_ ? ":" : input_fwd_ ? "/" : "?";
            mvaddnstr(y, 0, (prompt + input_buf_).c_str(), cols_);
            curs_set(1);
            ::move(y, std::min(cols_ - 1, 1 + display_width(input_buf_.substr(0, input_cur_))));
            return;
        }
        curs_set(0);
        const JNode& n = cur_.back().node;
        std::string left = " " + path_str(cur_) + "  " + type_info(n);
        if (!n.container() && n.kind != JKind::Error) left += g(" · ", " - ") + human_bytes((int64_t)(n.end - n.off));
        std::string right;
        const uint64_t pos = n.virt ? 0 : cur_.back().close && n.end > 0 ? n.end - 1 : n.start();
        const int pct = doc_.size() ? (int)(100.0 * (double)pos / (double)doc_.size()) : 0;
        right = "@ " + digits_with_sep(std::to_string(pos)) + " (" + std::to_string(pct) + "%)  ";
        const int v = doc_.validation();
        bool zeroed = false;
        if (doc_.file_changed(&zeroed) || zeroed) {
            right += std::string(g("⚠ ", "! ")) + (zeroed ? "file shrank on disk: zeros past its end"
                                                          : "file changed on disk");
        } else if (v == 0) {
            const int vp = doc_.size() ? (int)(100.0 * (double)doc_.validated_bytes() / (double)doc_.size()) : 0;
            right += "validating " + std::to_string(vp) + "%";
        } else if (v == 1) right += g("✓ valid", "valid");
        else {
            const vvjson::JsonError e = doc_.validation_error();
            right += std::string(g("⚠ invalid at ", "invalid at ")) + std::to_string(e.line) + ":" +
                     std::to_string(e.col);
        }
        right += "  H:help ";
        if (!flash_.empty()) left += "   " + flash_;
        attron(A_REVERSE);
        mvhline(y, 0, ' ', cols_);
        mvaddnstr(y, 0, left.c_str(), cols_);
        const int rw = display_width(right);
        if (display_width(left) + rw + 1 < cols_) mvaddstr(y, cols_ - rw, right.c_str());
        attroff(A_REVERSE);
    }

    // ── Search ─────────────────────────────────────────────────────────────
    void input_key(int ch) {
        if (ch == 27) { input_ = false; return; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            input_ = false;
            if (input_buf_.empty()) return;
            if (input_cmd_) { command(input_buf_); return; }
            query_ = input_buf_;
            pat_.compile(query_);
            fwd_ = input_fwd_;
            search(fwd_, /*include_cursor=*/false);
            return;
        }
        line_edit_key(input_buf_, input_cur_, ch);
    }
    void search(bool forward, bool include_cursor = false) {
        const JNode& n = cur_.back().node;
        // Resume after the cursor's own token (inside a container: after its
        // opening bracket; on a closing row: after the bracket), so a search
        // never starts in the middle of a string.
        const uint64_t from = cur_.back().close ? std::min<uint64_t>(n.end, doc_.size())
                                                : vvjson::JsonDoc::search_from(n, include_cursor);
        const uint64_t cur_start = n.virt ? 0 : n.start();
        // Any key cancels a long scan; the status line shows its progress.
        nodelay(stdscr, TRUE);
        auto tick = [&](uint64_t pos) {
            if (::getch() != ERR) return false;
            const int pct = (int)(100.0 * (double)pos / (double)std::max<uint64_t>(1, doc_.size()));
            attron(A_REVERSE);
            mvhline(rows_ - 1, 0, ' ', cols_);
            mvaddstr(rows_ - 1, 0, (std::string(" searching") + g("… ", "... ") + std::to_string(pct) +
                                    "%  (any key cancels)").c_str());
            attroff(A_REVERSE);
            refresh();
            return true;
        };
        bool cancelled = false, wrapped = false;
        const uint64_t hit = doc_.search(pat_, forward, from, cur_start, tick, &wrapped, &cancelled);
        nodelay(stdscr, FALSE);
        if (cancelled) { flash_ = "search cancelled"; return; }
        if (hit == UINT64_MAX) { flash_ = "not found: " + query_; return; }
        jump_to(path_at(hit));
        flash_ = (wrapped ? "(wrapped) " : "") + std::string("/") + query_;
    }

    // ── Jumps: a byte offset, a jq path, a line, the first error ───────────
    Path path_at(uint64_t off) {
        Path p;
        for (const auto& pr : doc_.path_to(off)) p.push_back({pr.first, pr.second});
        return p;
    }
    // Put the cursor on `p`, opening its ancestors, and centre it.
    void jump_to(const Path& p) {
        Path q = p;
        for (size_t l = 0; l + 1 < q.size(); ++l)
            if (!is_open(q, l)) set_ov(q[l].node, 1);
        cur_ = q;
        top_ = cur_;
        for (int i = 0; i < data_rows() / 2; ++i) if (!prev(top_)) break;
    }
    // ":" — a line number, a jq-style path (.a.b[3], .["odd key"], .[0]),
    // or q to quit.
    void command(const std::string& in) {
        std::string t = in;
        while (!t.empty() && t.back() == ' ') t.pop_back();
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        if (t == "q" || t == "quit") { quit_ = true; return; }
        if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            goto_line(std::strtoll(t.c_str(), nullptr, 10));
            return;
        }
        if (t.empty() || (t[0] != '.' && t[0] != '[')) {
            flash_ = "not a line number or a path (.key, [0], .[\"key\"]): " + t;
            return;
        }
        Path p;
        std::string err;
        if (!resolve(t, &p, &err)) { flash_ = err; return; }
        jump_to(p);
        flash_ = path_str(cur_);
    }
    bool resolve(const std::string& t, Path* out, std::string* err) {
        Path p;
        p.push_back({doc_.root(), -1});
        size_t i = 0;
        if (t[0] == '.') ++i;                     // the leading "." of ".", ".a", ".[0]"
        while (i < t.size()) {
            if (t[i] == '.') ++i;
            const JNode cur = p.back().node;
            std::string key;
            int64_t index = -1;
            bool is_key = false;
            if (i < t.size() && t[i] == '[') {
                const size_t close_at = t.find(']', i);
                if (i + 1 < t.size() && t[i + 1] == '"') {
                    size_t k = i + 2;
                    for (; k < t.size() && t[k] != '"'; ++k) {
                        if (t[k] == '\\' && k + 1 < t.size()) ++k;
                        key += t[k];
                    }
                    if (k + 1 >= t.size() || t[k + 1] != ']') { *err = "unclosed [\"…\"] in " + t; return false; }
                    is_key = true;
                    i = k + 2;
                } else {
                    if (close_at == std::string::npos) { *err = "unclosed [ in " + t; return false; }
                    const std::string num = t.substr(i + 1, close_at - i - 1);
                    if (num.empty() || !std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                        *err = "not an index: [" + num + "]";
                        return false;
                    }
                    index = std::strtoll(num.c_str(), nullptr, 10);
                    i = close_at + 1;
                }
            } else {
                size_t k = i;
                while (k < t.size() && t[k] != '.' && t[k] != '[') ++k;
                key = t.substr(i, k - i);
                if (key.empty()) { *err = "empty key in " + t; return false; }
                is_key = true;
                i = k;
            }
            const std::string so_far = path_str(p);
            if (is_key) {
                if (cur.kind != JKind::Object || cur.virt) { *err = so_far + " is not an object"; return false; }
                int64_t found = -1;
                JNode c;
                for (int64_t j = 0; j < cur.count; ++j) {
                    if (!doc_.child(cur, j, &c)) break;
                    if (json_unescape(doc_.key(c)) == key) { found = j; break; }
                }
                if (found < 0) { *err = "no key \"" + key + "\" in " + so_far; return false; }
                p.push_back({c, found});
            } else {
                if (cur.kind != JKind::Array) { *err = so_far + " is not an array"; return false; }
                JNode c;
                if (index >= cur.count || !doc_.child(cur, index, &c)) {
                    *err = so_far + " has " + count_label(cur) + "; no [" + std::to_string(index) + "]";
                    return false;
                }
                p.push_back({c, index});
            }
        }
        *out = p;
        return true;
    }
    void goto_line(int64_t line) {
        if (line < 1) line = 1;
        const char* d = doc_.data();
        const uint64_t n = doc_.size();
        uint64_t off = 0;
        for (int64_t l = 1; l < line && off < n; ++l) {
            const void* nl = std::memchr(d + off, '\n', (size_t)(n - off));
            if (!nl) { flash_ = "the document has " + std::to_string(l) + " line" + (l == 1 ? "" : "s"); return; }
            off = (uint64_t)(static_cast<const char*>(nl) - d) + 1;
        }
        while (off < n && (d[off] == ' ' || d[off] == '\t' || d[off] == '\r' || d[off] == '\n' ||
                           d[off] == ',' || d[off] == ']' || d[off] == '}'))
            ++off;
        jump_to(path_at(std::min<uint64_t>(off, n ? n - 1 : 0)));
        flash_ = "line " + std::to_string(line);
    }
    // "!" — the first error the structure scan or the validator found.
    void goto_error() {
        uint64_t off = UINT64_MAX;
        std::string msg;
        if (doc_.has_struct_err()) { off = doc_.struct_err_off(); msg = doc_.struct_err(); }
        if (doc_.validation() == 2) {
            const vvjson::JsonError e = doc_.validation_error();
            if (e.offset >= 0 && (uint64_t)e.offset < off) { off = (uint64_t)e.offset; msg = e.msg; }
        }
        if (off == UINT64_MAX) {
            flash_ = doc_.validation() == 1 ? "no errors: the document is valid"
                                            : "no error found yet (still validating)";
            return;
        }
        // The validator reports the byte after a bad token, often between
        // values: also try the last token before it and keep the deeper path.
        const uint64_t at = std::min<uint64_t>(off, doc_.size() ? doc_.size() - 1 : 0);
        uint64_t back = at;
        const char* d = doc_.data();
        while (back > 0 && std::strchr(" \t\r\n,:]}", d[back])) --back;
        Path p1 = path_at(at), p2 = path_at(back);
        jump_to(p2.size() > p1.size() ? p2 : p1);
        flash_ = msg + " at byte " + std::to_string(off);
    }

    // ── Copy, value pane, help, mouse ──────────────────────────────────────
    void copy_value() {
        const JNode& n = cur_.back().node;
        if (n.kind == JKind::Error) return;
        std::string v;
        std::string_view b = doc_.bytes(n);
        if (n.virt) b = std::string_view();
        const size_t cap = 1u << 20;
        const bool cut = b.size() > cap;
        if (cut) b = b.substr(0, cap);
        if (n.kind == JKind::String && b.size() >= 2) v = json_unescape(b.substr(1, b.size() - 2));
        else v = std::string(b);
        osc52_copy(v);
        flash_ = cut ? "copied the first 1 MiB of the value" : "copied " + human_bytes((int64_t)v.size());
    }
    void copy_key() {
        if (cur_.size() < 2) return;
        const JNode& par = cur_[cur_.size() - 2].node;
        std::string k = par.kind == JKind::Object && !par.virt
                            ? json_unescape(doc_.key(cur_.back().node))
                            : std::to_string(cur_.back().idx);
        osc52_copy(k);
        flash_ = "copied key: " + k;
    }
    void open_pane() {
        const JNode& n = cur_.back().node;
        if (n.container() || n.kind == JKind::Error) return;
        std::string_view b = doc_.bytes(n);
        pane_value_ = n.kind == JKind::String && b.size() >= 2 ? json_unescape(b.substr(1, b.size() - 2))
                                                              : std::string(b);
        pane_lines_.clear();
        const int w = std::max(10, cols_ - 6);
        std::string line;
        auto flush_line = [&]() { pane_lines_.push_back(clean(line)); line.clear(); };
        for (size_t i = 0; i < pane_value_.size();) {
            int len = 1;
            utf8_decode(pane_value_, i, &len);
            const std::string cp = pane_value_.substr(i, (size_t)len);
            i += (size_t)len;
            if (cp == "\n") { flush_line(); continue; }
            if (display_width(line + cp) > w) flush_line();
            line += cp;
        }
        flush_line();
        pane_top_ = 0;
        pane_ = true;
    }
    void pane_key(int ch) {
        const int h = std::max(1, rows_ - 6);
        const int maxtop = std::max(0, (int)pane_lines_.size() - h);
        switch (ch) {
            case 'q': case 27: case '\n': case '\r': case KEY_ENTER: pane_ = false; break;
            case 'j': case KEY_DOWN: pane_top_ = std::min(maxtop, pane_top_ + 1); break;
            case 'k': case KEY_UP: pane_top_ = std::max(0, pane_top_ - 1); break;
            case ' ': case KEY_NPAGE: pane_top_ = std::min(maxtop, pane_top_ + h); break;
            case 'b': case KEY_PPAGE: pane_top_ = std::max(0, pane_top_ - h); break;
            case 'g': pane_top_ = 0; break;
            case 'G': pane_top_ = maxtop; break;
            case 'y': osc52_copy(pane_value_); flash_ = "copied the value"; pane_ = false; break;
            default: break;
        }
    }
    // A frame, blank inside: Unicode, or ASCII when the tree's glyphs are
    // (--box ascii, or outside a UTF-8 locale).
    void draw_box(int y0, int x0, int h, int w, const std::string& title) {
        if (h < 2 || w < 2) return;
        const BoxGlyphs& bx = ascii_ ? kBoxAscii : kBoxUnicode;
        const std::string hl = repeat_utf8(bx.hline, w - 2);
        attron(COLOR_PAIR(NCP_INDEX));
        mvaddstr(y0, x0, (std::string(bx.tl) + hl + bx.tr).c_str());
        for (int y = y0 + 1; y < y0 + h - 1; ++y) {
            mvaddstr(y, x0, bx.vline);
            mvhline(y, x0 + 1, ' ', w - 2);
            mvaddstr(y, x0 + w - 1, bx.vline);
        }
        mvaddstr(y0 + h - 1, x0, (std::string(bx.bl) + hl + bx.br).c_str());
        attroff(COLOR_PAIR(NCP_INDEX));
        attron(A_BOLD);
        mvaddnstr(y0, x0 + 2, title.c_str(), std::max(0, w - 4));
        attroff(A_BOLD);
    }
    void draw_pane() {
        const int h = std::max(3, rows_ - 4), w = std::max(10, cols_ - 2);
        draw_box(1, 1, h, w, " " + path_str(cur_) + " ");
        for (int i = 0; i < h - 2 && pane_top_ + i < (int)pane_lines_.size(); ++i) {
            const std::string& l = pane_lines_[(size_t)(pane_top_ + i)];
            mvaddstr(2 + i, 3, l.substr(0, utf8_prefix_for_width(l, w - 4)).c_str());
        }
    }
    // ── Theme picker (T): the table viewer's themes, saved to the config ───
    void theme_key(int ch) {
        switch (ch) {
            case 'q': case 'T': case 27: theme_open_ = false; break;
            case 'j': case KEY_DOWN: theme_cur_ = std::min(kNumThemes - 1, theme_cur_ + 1); break;
            case 'k': case KEY_UP: theme_cur_ = std::max(0, theme_cur_ - 1); break;
            case 'g': case KEY_HOME: theme_cur_ = 0; break;
            case 'G': case KEY_END: theme_cur_ = kNumThemes - 1; break;
            case ' ': case '\n': case '\r': case KEY_ENTER: {
                g_theme = kAllThemes[theme_cur_];
                init_pairs();
                const bool saved = save_user_setting("theme", g_theme->name);
                flash_ = std::string("theme: ") + g_theme->name + (saved ? "" : "  (couldn't save)");
                theme_open_ = false;
                clearok(stdscr, TRUE);
                break;
            }
            default: break;
        }
    }
    void draw_theme_picker() {
        int wn = 0;
        for (int i = 0; i < kNumThemes; ++i) wn = std::max(wn, display_width(kAllThemes[i]->name));
        const std::string hint = "j/k move  Enter select+save  Esc close";
        const int w = std::min(cols_, std::max(wn + 8, display_width(hint) + 4));
        const int h = std::min(rows_, kNumThemes + 4);
        const int y0 = std::max(0, (rows_ - h) / 2), x0 = std::max(0, (cols_ - w) / 2);
        draw_box(y0, x0, h, w, " theme ");
        for (int i = 0; i < kNumThemes && i < h - 3; ++i) {
            const std::string line = std::string(kAllThemes[i] == g_theme ? "[*] " : "[ ] ") + kAllThemes[i]->name;
            const attr_t a = i == theme_cur_ ? (attr_t)(A_BOLD | A_REVERSE) : A_NORMAL;
            attron(a);
            mvaddnstr(y0 + 1 + i, x0 + 2, line.c_str(), w - 4);
            attroff(a);
        }
        attron(A_DIM);
        mvaddnstr(y0 + h - 2, x0 + 2, hint.c_str(), w - 4);
        attroff(A_DIM);
    }

    void draw_help() {
        struct Row { const char* k; const char* d; };
        static const Row rows[] = {
            {"j k  Down Up", "move"},
            {"h  Left", "collapse; on a leaf or closed node, go to the parent"},
            {"l  Right", "expand; if open, go to the first child"},
            {"Space  Enter", "toggle; Enter on a value opens it in full"},
            {"J  K", "next / previous sibling"},
            {"e  E", "expand the node / the node and everything inside"},
            {"c  C", "collapse the node / and everything inside"},
            {"1-9  0", "fold the document to depth N / to the root"},
            {"PgUp PgDn  ^U ^D", "page / half page"},
            {"g  G", "first / last row"},
            {"/  ?  n  N", "search keys and values (regex, icase); next / previous"},
            {":", "go to a line (:120) or a path (:.a.b[3]); :q quits"},
            {"!", "go to the first error"},
            {"y  p  Y", "copy the value / its jq path / its key (OSC 52)"},
            {"t", "table of the records: the array of objects at or above the cursor, else the document"},
            {"T", "choose a colour theme (saved)"},
            {"m  %", "line mode (JSON text, closing brackets); jump to the matching bracket"},
            {"q  Esc", "quit (Esc clears a search first)"},
            {"H  F1", "this help"},
        };
        const int n = (int)(sizeof(rows) / sizeof(rows[0]));
        int wk = 0, wd = 0;
        for (const Row& r : rows) { wk = std::max(wk, display_width(r.k)); wd = std::max(wd, display_width(r.d)); }
        const int w = std::min(cols_, wk + wd + 6), h = std::min(rows_, n + 2);
        const int y0 = std::max(0, (rows_ - h) / 2), x0 = std::max(0, (cols_ - w) / 2);
        draw_box(y0, x0, h, w, " vv JSON - keys ");
        for (int i = 0; i < n && i < h - 2; ++i) {
            attron(A_BOLD);
            mvaddnstr(y0 + 1 + i, x0 + 2, rows[i].k, w - 4);
            attroff(A_BOLD);
            mvaddnstr(y0 + 1 + i, x0 + 2 + wk + 2, rows[i].d, std::max(0, w - wk - 6));
        }
    }
    void mouse() {
        MEVENT ev;
        if (getmouse(&ev) == ERR) return;
        if (ev.bstate & BUTTON4_PRESSED) { step(-3); return; }
        if (ev.bstate & BUTTON5_PRESSED) { step(+3); return; }
        if (ev.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) {
            if (ev.y < 1 || ev.y > data_rows()) return;
            Path p = top_;
            for (int y = 1; y < ev.y; ++y) if (!next(p)) return;
            cur_ = p;
            if (ev.bstate & BUTTON1_DOUBLE_CLICKED) toggle();
        }
    }
};

// A JSON file's tree and the tables opened from it, kept while the viewer
// runs so the tree's cursor and each table survive switching back and forth.
struct JsonTreeView {
    vvjson::JsonDoc doc;
    std::unique_ptr<JsonTUI> tree;
    std::map<uint64_t, std::unique_ptr<TableTUI>> tables;   // UINT64_MAX: the whole document
    std::string label, table_path;
};

// Run the tree (and the tables `t` opens from it) until the user quits, or,
// when `whole_to_caller`, until `t` asks for the whole document's table —
// the caller's own table view of the file. true: that request.
static bool json_tree_loop(TuiSession& session, JsonTreeView& v, const Config& cfg,
                           bool whole_to_caller) {
    for (;;) {
        if (v.tree->run_in(session) == JsonTUI::Exit::Quit) return false;
        const JsonTUI::TableTarget& tt = v.tree->table_target();
        if (tt.whole && whole_to_caller) return true;
        const uint64_t key = tt.whole ? UINT64_MAX : tt.node.off;
        std::unique_ptr<TableTUI>& table = v.tables[key];
        if (!table) {
            std::unique_ptr<TabularSource> js;
            std::string e;
            if (tt.whole) {
                e = open_json_source(v.table_path, cfg, &js);
            } else {
                const std::string_view b = v.doc.bytes(tt.node);
                auto buf = std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t*>(b.data()),
                                                           (int64_t)b.size());
                e = open_json_stream(v.label + " " + tt.path,
                                     std::make_shared<arrow::io::BufferReader>(buf),
                                     StreamCodec::None, &js);
            }
            if (!e.empty()) {
                v.tables.erase(key);
                v.tree->flash(e.find("Empty JSON") != std::string::npos ||
                              e.find("no JSON records") != std::string::npos
                                  ? "no table view: no records"
                                  : tt.whole ? "no table view: this JSON is not a list of records "
                                               "(t on an array of objects inside it opens that)"
                                             : "no table view of " + tt.path);
                continue;
            }
            std::vector<std::unique_ptr<TabularSource>> srcs;
            srcs.push_back(std::move(js));
            table = std::make_unique<TableTUI>(std::move(srcs), cfg);
            table->set_tree_return(true);
        }
        table->run_in(session);
        if (!table->back_to_tree()) return false;
    }
}

// Open `file` (uncompressed, mappable) as a tree view; the table reader
// opens `table_path` itself (the original file, compressed or not).
static std::string open_json_tree_view(const std::string& file, const std::string& table_path,
                                       const std::string& label, bool lines,
                                       std::unique_ptr<JsonTreeView>* out) {
    auto v = std::make_unique<JsonTreeView>();
    if (auto e = v->doc.open(file, lines); !e.empty()) return e;
    v->label = label;
    v->table_path = table_path;
    v->tree = std::make_unique<JsonTUI>(v->doc, label);
    *out = std::move(v);
    return "";
}

// Run the JSON tree viewer on `file` (an uncompressed file it can map),
// switching to the table view (`t`) over `table_path`, which the table reader
// opens itself (the original file, compressed or not). Keys come from the
// controlling terminal when the data arrived on stdin. "" or an error;
// *term_failed when the terminal could not start (nothing was shown).
std::string run_json_viewer(const std::string& file, const std::string& table_path,
                            const std::string& label, bool lines, bool keys_from_tty,
                            const Config& cfg, bool* term_failed) {
    std::unique_ptr<JsonTreeView> view;
    if (auto e = open_json_tree_view(file, table_path, label, lines, &view); !e.empty()) return e;
    TuiSession session;
    if (!(keys_from_tty ? session.open_tty() : session.open())) { *term_failed = true; return ""; }
    json_tree_loop(session, *view, cfg, /*whole_to_caller=*/false);
    return "";
}

// Run the table viewer over `srcs` (several files open as tabs); `t` on a
// JSON file's tab opens that file's tree in the same terminal session, and
// `t` there on the whole document comes back. true when the viewer ran;
// false when the terminal could not start, with the first source handed
// back in *first for the non-interactive fallback.
bool run_table_viewer(std::vector<std::unique_ptr<TabularSource>> srcs, const Config& cfg,
                      const TuiStart& start, std::unique_ptr<TabularSource>* first) {
    TableTUI tui(std::move(srcs), cfg, start);
    {
        TuiSession session;
        if (session.open()) {
            // `t` on a JSON file's tab: that file's tree view, in the
            // same session; `t` there on the whole document comes back.
            std::map<std::string, std::unique_ptr<JsonTreeView>> trees;
            for (;;) {
                tui.run_in(session);
                const std::string jp = tui.tree_request();
                if (jp.empty()) return true;
                std::unique_ptr<JsonTreeView>& view = trees[jp];
                if (!view) {
                    std::string file = jp, e;
                    auto raw = arrow::io::ReadableFile::Open(jp);
                    if (raw.ok() && sniff_file_codec(*raw) != StreamCodec::None) {
                        std::shared_ptr<arrow::io::InputStream> in;
                        int64_t bytes = 0;
                        e = open_json_file(jp, &in);
                        if (e.empty()) e = spool_stream(in, ".json", &file, &bytes, jp);
                    }
                    if (e.empty()) e = open_json_tree_view(file, jp, jp, json_path_kind(jp) == 2, &view);
                    if (!e.empty()) {
                        trees.erase(jp);
                        tui.flash("no tree view: " + e);
                        continue;
                    }
                }
                if (!json_tree_loop(session, *view, cfg, /*whole_to_caller=*/true)) return true;
            }
        }
    }
    *first = tui.take_first_source();
    return false;
}

#endif  // VV_CORE_LIB (end of ncurses TUI frontend)
