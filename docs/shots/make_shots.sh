#!/usr/bin/env bash
# Regenerate the documentation screenshots from synthetic data:
#   docs/img/vv/*.svg   terminal screens (tmux capture → ansi_to_svg.py)
#   docs/img/vvg/*.png  GUI windows (vvg VVG_SCREENSHOT, headless)
#
#   docs/shots/make_shots.sh [VV] [VVG]     (defaults: build/vv, build-gui/gui/vvg)
#
# Needs tmux, python3 with rich (+ pyarrow, pysam, anndata, openpyxl for the
# data), and ImageMagick for the PNG8 step. ONLY=name1,name2 limits the run.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
VV=$(realpath "${1:-$root/build/vv}")
VVG=${2:-$root/build-gui/gui/vvg}
[ -x "$VVG" ] && VVG=$(realpath "$VVG")
KDETEST=$root/build-gui/gui/kde/vvkdetest     # Dolphin thumbnail renderer
out_vv=$root/docs/img/vv
out_vvg=$root/docs/img/vvg
mkdir -p "$out_vv" "$out_vvg"

work=$(mktemp -d)
trap 'tmux -L vvshots kill-server 2>/dev/null || true; rm -rf "$work"' EXIT
python3 "$here/showcase_data.py" "$work/data" >/dev/null
mkdir -p "$work/cfg" "$work/home"
cd "$work/data"
# A clean environment: no user theme / config, a fixed locale and terminal.
export XDG_CONFIG_HOME=$work/cfg HOME=$work/home LANG=C.UTF-8 LC_ALL=C.UTF-8
unset NO_COLOR VV_THEME 2>/dev/null || true

want() { [ -z "${ONLY:-}" ] || [[ ",$ONLY," == *",$1,"* ]]; }

# tui NAME COLS ROWS "KEYS" ARGS... — open vv ARGS in a COLS×ROWS tmux pane,
# send KEYS (space-separated tmux key names), capture the screen.
tui() {
    local name=$1 cols=$2 rows=$3 keys=$4; shift 4
    want "$name" || return 0
    tmux -L vvshots -f /dev/null new-session -d -s s -x "$cols" -y "$rows" \
        "env TERM=xterm-256color $(printf '%q ' "$VV" "$@"); sleep 60"
    sleep 1.5
    for k in $keys; do tmux -L vvshots send-keys -t s "$k"; sleep 0.4; done
    sleep 0.6
    tmux -L vvshots capture-pane -e -p -t s > "$work/$name.ansi"
    tmux -L vvshots kill-server
    python3 "$here/ansi_to_svg.py" "$work/$name.ansi" "$out_vv/$name.svg" "vv $*" "$cols"
    echo "vv/$name.svg"
}

# cli NAME COLS ARGS... — a non-interactive command's output on a TTY
# (a tall pane, so nothing scrolls off; trailing blank lines are trimmed).
# MAXL=N before the call keeps the first N lines (the cut is marked).
cli() {
    local name=$1 cols=$2; shift 2
    want "$name" || return 0
    tmux -L vvshots -f /dev/null new-session -d -s s -x "$cols" -y 120 \
        "env TERM=xterm-256color $(printf '%q ' "$VV" "$@"); sleep 60"
    sleep 1.5
    tmux -L vvshots capture-pane -e -p -t s > "$work/$name.ansi"
    tmux -L vvshots kill-server
    python3 "$here/ansi_to_svg.py" "$work/$name.ansi" "$out_vv/$name.svg" "vv $*" "$cols" "${MAXL:-}"
    echo "vv/$name.svg"
}

# gui NAME "ENV=… …" ARGS... — vvg window (1100×700) after ARGS / VVG_* setup.
gui() {
    local name=$1 envs=$2; shift 2
    want "$name" || return 0
    if [ ! -x "$VVG" ]; then echo "skip vvg/$name.png (no vvg)"; return 0; fi
    # shellcheck disable=SC2086
    env QT_QPA_PLATFORM=offscreen VVG_WINTEST=1 VVG_SCREENSHOT="$work/$name.png" $envs \
        "$VVG" "$@" >/dev/null
    MAGICK_THREAD_LIMIT=1 magick "$work/$name.png" -strip +dither -colors 256 "PNG8:$out_vvg/$name.png"
    echo "vvg/$name.png"
}

# ── Terminal: interactive viewer ─────────────────────────────────────────────
tui vcf            110 26 ""            cohort.vcf.gz
tui vcf-detail     110 26 "j j Enter"   cohort.vcf.gz
tui json-tree      110 26 "j j j l"     run.json
tui parquet        110 26 ""            atac_peaks.parquet
tui parquet-stats  110 26 "l l l l l l S" atac_peaks.parquet
tui bam            110 26 ""            reads.bam
tui fastq          110 20 "l l"         reads.fq.gz
tui narrowpeak     110 20 ""            atac_peaks.narrowPeak
tui gff3           110 20 ""            genes.gff3
tui paf            110 20 ""            contigs.paf
tui anndata        110 24 ""            pbmc.h5ad
tui anndata-obs    110 24 "Tab Tab"     pbmc.h5ad
tui sqlite         110 16 ""            lab.sqlite
tui log            110 16 ""            pipeline.log

# ── Terminal: non-interactive output ─────────────────────────────────────────
cli markdown       90  README.md
cli table          110 -n 8 samples.tsv
cli xlsx           110 -n 6 samples.xlsx
cli bigwig         110 -n 6 coverage.bw
cli npz            110 -n 6 embedding.npz
cli mtx            110 -n 6 matrix.mtx
cli dataset        110 -n 8 measurements
cli fasta          110 -n 7 regions.fa
cli seq-stats      110 --no-interactive --seq-stats reads.fq.gz
MAXL=13 cli gt-stats 110 --gt-stats --select CHROM,POS,REF,ALT,n_het,n_hom_alt,n_missing,AF,call_rate -n 8 cohort.vcf.gz
MAXL=11 cli region 110 -n 8 -r chr17:43044295-43060000 --select CHROM,POS,REF,ALT,NA12878,NA12891 cohort.vcf.gz
cli schema         110 --schema atac_peaks.parquet
cli describe       110 --describe --select signal,pvalue,qvalue,summit atac_peaks.parquet
MAXL=22 cli json-paths 110 --json-paths run.json
cli ndjson         110 -n 6 events.ndjson

# ── GUI ──────────────────────────────────────────────────────────────────────
gui anndata    "VVG_DETAIL=3,0"  --tab obs pbmc.h5ad
gui vcf        "VVG_DETAIL=2,1"  cohort.vcf.gz
gui parquet    "VVG_DETAIL=0,0"  --filter "signal > 10" --sort signal:desc atac_peaks.parquet
gui bam        "VVG_DETAIL=0,0"  reads.bam
gui sqlite     "VVG_DETAIL=2,0"  --tab runs lab.sqlite
gui json       "VVG_DETAIL=14,1" events.ndjson

# ── KDE Dolphin thumbnails (when the KF6 plugins were built) ─────────────────
if want kde-thumbnails && [ -x "$KDETEST" ]; then
    for f in atac_peaks.parquet cohort.vcf.gz pbmc.h5ad reads.bam; do
        QT_QPA_PLATFORM=offscreen "$KDETEST" "$f" "$work/thumb-${f%%.*}.png" >/dev/null
    done
    MAGICK_THREAD_LIMIT=1 magick montage "$work"/thumb-{atac_peaks,cohort,pbmc,reads}.png \
        -tile 4x1 -geometry +8+8 -background '#e8e8e8' "$work/thumbs.png"
    MAGICK_THREAD_LIMIT=1 magick "$work/thumbs.png" -strip +dither -colors 256 \
        "PNG8:$out_vvg/kde-thumbnails.png"
    echo "vvg/kde-thumbnails.png"
fi
