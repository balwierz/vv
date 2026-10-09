// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// The Markdown renderer.

#include "internal.hpp"

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

            int content_w = width - indent;   // widest a wrapped line can hold
            if (content_w < 1) content_w = 1;
            if (wlen <= content_w) {
                // Fits on a line: move to a fresh one if it won't fit here.
                if (pos + wlen > width && pos > indent) push_line();
                cur.runs.push_back({word, seg.style, seg.role});
                pos += wlen;
            } else {
                // Longer than any line — hard-cut on codepoint boundaries so it
                // never overflows the wrap width (this function's contract).
                size_t off = 0;
                while (off < word.size()) {
                    if (pos >= width) push_line();
                    int rem = width - pos;
                    // Longest codepoint-aligned prefix of word[off:] fitting rem.
                    size_t k = off; int used = 0;
                    while (k < word.size()) {
                        int len = 1;
                        int cw = codepoint_width(utf8_decode(word, k, &len));
                        if (used + cw > rem) break;
                        used += cw; k += (size_t)len;
                    }
                    if (k == off) {            // nothing fit on this line
                        if (pos > indent) { push_line(); continue; }
                        int len = 1;           // 1-col line vs a wide char:
                        used = codepoint_width(utf8_decode(word, off, &len));
                        k = off + (size_t)len; // force one codepoint to progress
                    }
                    cur.runs.push_back({word.substr(off, k - off),
                                         seg.style, seg.role});
                    pos += used;
                    off = k;
                }
            }
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


ImageProto detect_image_proto() {
    // tmux (and screen) keep the outer terminal's TERM_PROGRAM / LC_TERMINAL
    // / KITTY_WINDOW_ID but drop image escapes that are not wrapped for
    // passthrough, so an image sent from inside them never shows.
    if (std::getenv("TMUX") || std::getenv("STY")) return ImageProto::None;
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
    // Saved role per open span/tag. `style` is a bitmask that each span
    // toggles independently, but `role` is a single value: a nested span that
    // sets its own role (inline code inside a link) must restore the outer
    // role on close, not blank it. Pushed on span/tag enter, popped on leave;
    // cleared at block boundaries so an unbalanced inline tag can't leak.
    std::vector<uint8_t> role_stack_;

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

    // Close any inline <a> hyperlink left open in the current block (an
    // unbalanced <a> with no matching </a>) so its OSC 8 link doesn't bleed
    // past the block.
    void close_dangling_html_links() {
        while (html_a_depth > 0) {
            if (osc8 && g_color.reset != nullptr && *g_color.reset != '\0') {
                MdSegment seg;
                seg.text     = "\033]8;;\033\\";
                seg.verbatim = true;
                cur_runs.push_back(std::move(seg));
            }
            --html_a_depth;
        }
    }

    void finish_paragraph() {
        close_dangling_html_links();
        if (!cur_runs.empty()) {
            // Drop a trailing space that wrap_runs would emit otherwise.
            MdBlockKind k = (quote_depth > 0) ? MdBlockKind::Quote
                                              : MdBlockKind::Paragraph;
            int indent = quote_depth * 2;
            // For block quotes we put the ▌ glyphs at the start of every
            // wrapped line, not just the first — easier to inject before wrap.
            finish_block(k, indent);
        }
        // Inline style/role never span blocks; drop anything left set by an
        // unbalanced inline tag (e.g. a <b> with no </b>) so it can't leak
        // into the next block.
        style = 0;
        role  = ROLE_NONE;
        role_stack_.clear();
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
                // finish_paragraph() closes any unbalanced <a> OSC 8 wrappers.
                finish_paragraph();
                push_spacer();
                break;
            default: break;
        }
        return 0;
    }

    int enter_span(MD_SPANTYPE type, void* detail) {
        role_stack_.push_back(role);   // restored by the paired leave_span
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
                break;
            case MD_SPAN_A:
                style &= ~MD_UNDER;
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
        // Restore the role that was active before this span (md4c pairs
        // enter/leave, so the stack stays balanced).
        if (!role_stack_.empty()) {
            role = role_stack_.back();
            role_stack_.pop_back();
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
            if (t.closing) {
                r.style &= ~MD_CODE;
                if (!r.role_stack_.empty()) {
                    r.role = r.role_stack_.back(); r.role_stack_.pop_back();
                } else r.role = ROLE_NONE;
            } else {
                r.role_stack_.push_back(r.role);
                r.style |= MD_CODE; r.role = ROLE_CODE;
            }
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
                    if (!r.role_stack_.empty()) {
                        r.role = r.role_stack_.back(); r.role_stack_.pop_back();
                    } else r.role = ROLE_NONE;
                    // Only emit the OSC 8 close when the open emitted an OSC 8
                    // open — i.e. the link had a non-empty href. A hrefless <a>
                    // opened no hyperlink, so a close here would be a stray,
                    // unmatched escape (and could truncate a surrounding link).
                    if (r.osc8 && !r.pending_href.empty() &&
                        g_color.reset != nullptr && *g_color.reset != '\0') {
                        MdSegment seg;
                        seg.text     = "\033]8;;\033\\";
                        seg.verbatim = true;
                        r.cur_runs.push_back(std::move(seg));
                    } else if (!r.pending_href.empty()) {
                        // No OSC 8 (or unsupported): show the URL stub. The href
                        // for an open <a> lived on the renderer via pending_href.
                        r.cur_runs.push_back(
                            {" (" + r.pending_href + ")", MD_DIM, ROLE_LINK});
                    }
                    r.pending_href.clear();
                    --r.html_a_depth;
                }
            } else {
                auto it_href = t.attrs.find("href");
                std::string href = (it_href != t.attrs.end()) ? it_href->second : "";
                r.pending_href = href;
                r.role_stack_.push_back(r.role);
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
std::string parse_markdown_file(const std::string& path,
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
[[maybe_unused]] static int g_pager_pid = -1;
void emit_via_pager(const std::function<void()>& emit_fn) {
#ifdef _WIN32
    emit_fn();   // no fork / exec: written straight to the console
#else
    int pfd[2];
    if (pipe(pfd) != 0) { emit_fn(); return; }
    // Set this BEFORE forking. setenv() takes a libc lock and is not
    // async-signal-safe, so calling it in the child of a fork() from a
    // multithreaded process (Arrow's CPU pool is live by now) can deadlock on
    // a lock another thread held at fork time. Doing it in the parent is
    // harmless: the value only matters to the exec'd `less`, and setenv(...,0)
    // still leaves a user-provided LESSANSIENDCHARS alone.
    set_env("LESSANSIENDCHARS", "mK", false);
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
#endif
}

// Emit the prose body of `doc` to stdout as ANSI. Tables are referenced
// by their inline `▶ [Table N: ...]` placeholder and printed afterwards
// using the existing print_table pipeline (caller's responsibility — we
// just emit the prose here).
void emit_markdown_stdout(const MarkdownDoc& doc) {
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
