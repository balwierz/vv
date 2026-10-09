// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Terminal images: --heatmap and inline image protocols.

#include "internal.hpp"

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

enum class Mode { Auto, Kitty, ITerm2, Sixel, HalfBlock, Ascii };

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
    if (term_size(STDOUT_FILENO, cols, rows)) { if (*rows <= 0) *rows = 24; }
    else { *cols = 80; *rows = 24; }
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

// iTerm2's inline-image protocol (OSC 1337 File=), which WezTerm also speaks:
// the image travels as a PNG file — RGB, unfiltered scanlines deflated with
// zlib — scaled by the terminal to `cell_cols` × `cell_rows` cells.
static std::string png_rgb(const PalImage& im) {
    auto be32 = [](std::string& o, uint32_t v) {
        for (int k = 3; k >= 0; --k) o += (char)((v >> (8 * k)) & 255);
    };
    auto chunk = [&](std::string& o, const char* type, const std::string& data) {
        be32(o, (uint32_t)data.size());
        std::string td(type, 4);
        td += data;
        o += td;
        be32(o, (uint32_t)crc32(0L, (const Bytef*)td.data(), (uInt)td.size()));
    };
    std::string raw;
    raw.reserve((size_t)im.h * ((size_t)im.w * 3 + 1));
    for (int y = 0; y < im.h; ++y) {
        raw += '\0';                                 // filter: none
        for (int x = 0; x < im.w; ++x) {
            uint32_t c = im.pal[im.px[(size_t)y * im.w + x]];
            raw += (char)((c >> 16) & 255); raw += (char)((c >> 8) & 255); raw += (char)(c & 255);
        }
    }
    uLongf zlen = compressBound((uLong)raw.size());
    std::string z(zlen, '\0');
    if (compress2((Bytef*)z.data(), &zlen, (const Bytef*)raw.data(), (uLong)raw.size(), 6) != Z_OK)
        return "";
    z.resize(zlen);
    std::string ihdr;
    be32(ihdr, (uint32_t)im.w); be32(ihdr, (uint32_t)im.h);
    ihdr += (char)8; ihdr += (char)2;                 // 8-bit RGB
    ihdr += '\0'; ihdr += '\0'; ihdr += '\0';         // deflate, adaptive filter, no interlace
    std::string png = "\x89PNG\r\n\x1a\n";
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

static void emit_iterm2(const PalImage& im, int cell_cols, int cell_rows) {
    const std::string png = png_rgb(im);
    if (png.empty()) return;
    std::string out = "\033]1337;File=inline=1;size=" + std::to_string(png.size()) +
                      ";width=" + std::to_string(cell_cols) +
                      ";height=" + std::to_string(cell_rows) +
                      ";preserveAspectRatio=0:" + base64_encode(png) + "\a\n";
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
    // One pass per 6-row band: each pixel sets its row's bit in its colour's
    // column pattern. Then every colour present in the band is written as one
    // line of sixel characters, runs of four or more as `!<n><char>`.
    // O(w × h + colours in band × w) instead of a rescan of the band per
    // palette entry.
    const size_t npal = im.pal.size();
    std::vector<int>     slot(npal, -1);            // palette index → band slot
    std::vector<size_t>  used;                      // palette indices in the band
    std::vector<uint8_t> bits;                      // slot × w six-bit patterns
    auto put_run = [&](char ch, int n) {
        if (n >= 4) { out += '!'; out += std::to_string(n); out += ch; }
        else        out.append((size_t)n, ch);
    };
    for (int band = 0; band * 6 < im.h; ++band) {
        for (size_t c : used) slot[c] = -1;
        used.clear();
        bits.clear();
        for (int k = 0; k < 6; ++k) {
            const int row = band*6 + k; if (row >= im.h) break;
            const uint8_t* line = &im.px[(size_t)row * im.w];
            for (int x = 0; x < im.w; ++x) {
                const size_t c = line[x];
                if (c >= npal) continue;
                if (slot[c] < 0) {
                    slot[c] = (int)used.size();
                    used.push_back(c);
                    bits.resize(used.size() * (size_t)im.w, 0);
                }
                bits[(size_t)slot[c] * im.w + x] |= (uint8_t)(1 << k);
            }
        }
        std::vector<size_t> order(used);             // palette order, as before
        std::sort(order.begin(), order.end());
        for (size_t c : order) {
            out += '#'; out += std::to_string(c);
            const uint8_t* pat = &bits[(size_t)slot[c] * im.w];
            int run = 0; char prev = 0;
            for (int x = 0; x < im.w; ++x) {
                const char ch = (char)(0x3F + pat[x]);
                if (run && ch == prev) { ++run; continue; }
                if (run) put_run(prev, run);
                prev = ch; run = 1;
            }
            if (run && prev != 0x3F) put_run(prev, run);   // trailing blanks add nothing
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
    if (mode == Mode::Auto) {
        // kitty graphics in kitty; iTerm2's protocol in iTerm2 and WezTerm;
        // half-blocks (truecolour text) everywhere else.
        const md::ImageProto proto = md::detect_image_proto();
        mode = proto == md::ImageProto::Kitty  ? Mode::Kitty
             : proto == md::ImageProto::ITerm2 ? Mode::ITerm2
                                               : Mode::HalfBlock;
    }
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
    } else if (mode == Mode::ITerm2) {
        emit_iterm2(resample(src, box_cols * 8, box_rows * 16), box_cols, box_rows);
    } else { // Kitty
        emit_kitty(resample(src, box_cols * 8, box_rows * 16), box_cols, box_rows);
    }
}

}  // namespace img

// --heatmap: build a colour heatmap from the source's numeric matrix (rows ×
// numeric columns), globally normalised, and emit it to the terminal.
std::string render_heatmap(TabularSource& src, const Config& cfg) {
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
    else if (cfg.image_mode == "iterm")     mode = img::Mode::ITerm2;
    else if (cfg.image_mode == "sixel")     mode = img::Mode::Sixel;
    else if (cfg.image_mode == "halfblock") mode = img::Mode::HalfBlock;
    else if (cfg.image_mode == "ascii")     mode = img::Mode::Ascii;
    else return "--image-mode: unknown mode '" + cfg.image_mode +
                "' (use auto|kitty|iterm|sixel|halfblock|ascii)";

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
