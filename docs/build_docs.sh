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

# 1) HTML — self-contained, suitable for sharing or hosting.
echo "→ HTML"
pandoc "$SRC" \
    --standalone \
    --embed-resources \
    --toc --toc-depth=2 \
    --highlight-style=tango \
    --metadata=lang:en \
    -o USAGE.html
echo "    $(pwd)/USAGE.html ($(stat -c%s USAGE.html) bytes)"

# 2) PDF.
echo "→ PDF"
PDF_OK=0

try_tex() {
    local engine="$1"
    command -v "$engine" >/dev/null 2>&1 || return 1
    pandoc "$SRC" \
        --pdf-engine="$engine" \
        --toc --toc-depth=2 \
        --highlight-style=tango \
        -V geometry:margin=2.4cm \
        -V mainfont="DejaVu Sans" \
        -V monofont="DejaVu Sans Mono" \
        -V monofontoptions="Scale=0.85" \
        -o USAGE.pdf 2>/dev/null
}

try_browser() {
    local browser
    for browser in chromium google-chrome chrome; do
        if command -v "$browser" >/dev/null 2>&1; then
            "$browser" --headless --no-sandbox --disable-gpu \
                       --no-pdf-header-footer \
                       --print-to-pdf=USAGE.pdf USAGE.html >/dev/null 2>&1 \
                && return 0
        fi
    done
    return 1
}

if try_tex xelatex 2>/dev/null; then
    PDF_OK=1; ENGINE="xelatex"
elif try_tex lualatex 2>/dev/null; then
    PDF_OK=1; ENGINE="lualatex"
elif try_browser; then
    PDF_OK=1; ENGINE="chromium (via HTML)"
fi

if [ "$PDF_OK" = "1" ]; then
    echo "    $(pwd)/USAGE.pdf ($(stat -c%s USAGE.pdf) bytes, engine: $ENGINE)"
else
    echo "    skipped — no PDF engine available." >&2
    echo "    install one of: texlive-fontsrecommended, chromium, " >&2
    echo "    google-chrome, weasyprint, wkhtmltopdf" >&2
    exit 1
fi
