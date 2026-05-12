#!/usr/bin/env bash
# Render docs/USAGE.md → docs/USAGE.html and docs/USAGE.pdf via pandoc.
#
# Requires pandoc plus one of:
#   - a working TeX install (xelatex or lualatex) for "native" PDF, or
#   - chromium / chrome (or any other browser with --headless), used by
#     converting Markdown → HTML → PDF.
#
# The script tries texlive first (better typography, native code blocks),
# then falls back to a headless-browser route on systems where TeX fonts
# are not installed.
set -euo pipefail

cd "$(dirname "$0")"
SRC="USAGE.md"

# Inline CSS that tightens the chromium-rendered PDF: smaller font,
# narrower page margins, and a body that fills the page width (pandoc's
# default html5 template caps body width at 36em which leaves huge
# side gutters when printed to A4).
PRINT_CSS=$(mktemp --suffix=.css)
trap 'rm -f "$PRINT_CSS"' EXIT
cat > "$PRINT_CSS" <<'CSS'
@page { size: A4; margin: 1.2cm 1.4cm; }
@media print {
    html { font-size: 10.5pt; }
    body { max-width: none; margin: 0; padding: 0; }
    pre, code { font-size: 0.9em; }
    h1 { font-size: 1.55em; margin-top: 0.8em; }
    h2 { font-size: 1.25em; margin-top: 0.8em; }
    h3 { font-size: 1.08em; }
    /* Avoid awkward column-1 indents on the TOC */
    nav#TOC ul { padding-left: 1.2em; }
}
CSS

# 1) HTML — self-contained, suitable for sharing or hosting.
echo "→ HTML"
pandoc "$SRC" \
    --standalone \
    --embed-resources \
    --toc --toc-depth=2 \
    --highlight-style=tango \
    --metadata=lang:en \
    --css="$PRINT_CSS" \
    -o USAGE.html
echo "    $(pwd)/USAGE.html ($(stat -c%s USAGE.html) bytes)"

# 2) PDF.
echo "→ PDF"
PDF_OK=0

try_tex() {
    local engine="$1"
    command -v "$engine" >/dev/null 2>&1 || return 1
    # Don't swallow stderr — if texlive is incomplete the user needs to
    # see why (missing fonts, missing packages) instead of silently
    # falling through to an HTML-based engine.
    pandoc "$SRC" \
        --pdf-engine="$engine" \
        --toc --toc-depth=2 \
        --highlight-style=tango \
        -V geometry:margin=2.4cm \
        -V mainfont="DejaVu Sans" \
        -V monofont="DejaVu Sans Mono" \
        -V monofontoptions="Scale=0.85" \
        -o USAGE.pdf
}

try_pandoc_engine() {
    local engine="$1"
    command -v "$engine" >/dev/null 2>&1 || return 1
    pandoc "$SRC" \
        --pdf-engine="$engine" \
        --toc --toc-depth=2 \
        --highlight-style=tango \
        -o USAGE.pdf 2>/dev/null
}

try_browser() {
    local browser
    for browser in chromium google-chrome chrome chromium-browser; do
        if command -v "$browser" >/dev/null 2>&1; then
            "$browser" --headless --no-sandbox --disable-gpu \
                       --no-pdf-header-footer \
                       --print-to-pdf=USAGE.pdf \
                       "file://$PWD/USAGE.html" >/dev/null 2>&1 \
                && return 0
        fi
    done
    return 1
}

# Preference order:
#   1. xelatex / lualatex — best typography when texlive-fontsrecommended
#      (and DejaVu) is installed.
#   2. chromium / chrome --headless — renders the already-built HTML;
#      mature, fast, produces clean PDFs.
#   3. wkhtmltopdf — webkit-based, mature but unmaintained.
#   4. weasyprint — last resort: WeasyPrint 68.x has a horizontal-advance
#      bug with the embedded DejaVu Sans subset that pandoc's HTML
#      template ships, producing visibly spaced-out letters
#      ("S u p p o r t e d  f o r m a t s"). Tracked upstream; until
#      fixed, prefer chromium when both are installed.
if try_tex xelatex; then
    PDF_OK=1; ENGINE="xelatex"
elif try_tex lualatex; then
    PDF_OK=1; ENGINE="lualatex"
elif try_browser; then
    PDF_OK=1; ENGINE="chromium (via HTML)"
elif try_pandoc_engine wkhtmltopdf; then
    PDF_OK=1; ENGINE="wkhtmltopdf"
elif try_pandoc_engine weasyprint; then
    PDF_OK=1; ENGINE="weasyprint"
fi

if [ "$PDF_OK" = "1" ]; then
    echo "    $(pwd)/USAGE.pdf ($(stat -c%s USAGE.pdf) bytes, engine: $ENGINE)"
else
    echo "    skipped — no PDF engine available." >&2
    echo "    install one of: texlive-fontsrecommended, chromium, " >&2
    echo "    google-chrome, weasyprint, wkhtmltopdf" >&2
    exit 1
fi
