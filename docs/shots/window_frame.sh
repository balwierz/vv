#!/usr/bin/env bash
# Draw a window frame around a bare widget grab, so a screenshot taken under
# QT_QPA_PLATFORM=offscreen (no window manager) reads as a desktop window:
# a Breeze-Light-style title bar (app icon, centred title, minimise / maximise
# / close) and a one-pixel border, with rounded top corners.
#
#   docs/shots/window_frame.sh IN.png OUT.png TITLE [SCALE]
#
# SCALE is the device pixel ratio of IN.png (2 for a QT_SCALE_FACTOR=2 grab).
# The buttons are drawn as strokes, so only the title depends on a font
# (fontconfig's default sans; FRAME_FONT=/path/to/font.ttf overrides it).
# FRAME_ICON=/path/to/icon.svg replaces the vv icon; FRAME_ICON= omits it.
set -euo pipefail
in=$1 out=$2 title=$3 s=${4:-1}
here=$(cd "$(dirname "$0")" && pwd)
icon=${FRAME_ICON-$here/../../gui/kde/icons/vv.svg}
font=${FRAME_FONT:-$(fc-match -f '%{file}' sans)}
export MAGICK_THREAD_LIMIT=1

bar='#dee0e2' ink='#232629' edge='#a4a8ac'
read -r w h < <(magick identify -format '%w %h\n' "$in")
tb=$((30 * s))                  # title bar height
b=$s                            # border width
r=$((6 * s))                    # top corner radius
W=$((w + 2 * b)) H=$((h + tb + 2 * b))

# Button glyphs, centred on (cx, cy): chevron down, chevron up, cross.
cy=$((b + tb / 2))
cx_close=$((W - b - 18 * s)) cx_max=$((W - b - 46 * s)) cx_min=$((W - b - 74 * s))
a=$((4 * s)) d=$((2 * s))
glyphs="polyline $((cx_min - a)),$((cy - d)) $cx_min,$((cy + d)) $((cx_min + a)),$((cy - d))
        polyline $((cx_max - a)),$((cy + d)) $cx_max,$((cy - d)) $((cx_max + a)),$((cy + d))
        line $((cx_close - a)),$((cy - a)) $((cx_close + a)),$((cy + a))
        line $((cx_close - a)),$((cy + a)) $((cx_close + a)),$((cy - a))"

icon_args=()
if [ -n "$icon" ] && [ -f "$icon" ]; then
    isz=$((18 * s))
    icon_args=( \( -background none -density $((72 * s)) "$icon" -resize "${isz}x${isz}" \)
                -gravity northwest -geometry "+$((b + 8 * s))+$((b + (tb - isz) / 2))" -composite )
fi

magick -size "${W}x${H}" "xc:$edge" \
    -fill "$bar" -draw "rectangle $b,$b $((W - b - 1)),$((b + tb - 1))" \
    "$in" -gravity northwest -geometry "+$b+$((b + tb))" -composite \
    "${icon_args[@]}" \
    -font "$font" -pointsize $((13 * s)) -fill "$ink" \
    -gravity north -annotate "+0+$((b + (tb - 17 * s) / 2))" "$title" \
    -gravity northwest -fill none -stroke "$ink" -strokewidth "$(awk "BEGIN{print 1.2 * $s}")" \
    -draw "$glyphs" -stroke none \
    \( -size "${W}x${H}" xc:black -fill white \
       -draw "roundrectangle 0,0 $((W - 1)),$((H + r)) $r,$r" \) \
    -alpha off -compose CopyOpacity -composite -compose over \
    "$out"
