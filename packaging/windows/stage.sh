#!/usr/bin/env bash
# Collect vv.exe, vvg.exe and everything they load from the MSYS2 UCRT64
# prefix into one directory: the contents of the MSI (packaging/windows/vv.wxs)
# and of C:\Program Files\vv. Run in the MSYS2 UCRT64 shell after the build:
#
#   packaging/windows/stage.sh build stage
#
# Qt's DLLs and plugins come from windeployqt6; every other DLL is found by
# following ldd from the executables and the deployed plugins until nothing
# new turns up (only DLLs under the MSYS2 prefix; system DLLs stay out).
set -euo pipefail

build=${1:?usage: stage.sh BUILD_DIR STAGE_DIR}
out=${2:?usage: stage.sh BUILD_DIR STAGE_DIR}
prefix=${MINGW_PREFIX:?run in the MSYS2 UCRT64 shell}
here=$(cd "$(dirname "$0")" && pwd)

rm -rf "$out"
mkdir -p "$out"
cp "$build/vv.exe" "$build/gui/vvg.exe" "$out/"

# Qt: its DLLs, the platform / style / image-format / icon-engine plugins
# (SVG for the window icon), no translations or software OpenGL.
windeployqt6 --release --no-translations --no-opengl-sw --no-system-d3d-compiler \
    --dir "$out" "$out/vvg.exe" >/dev/null
# The offscreen platform as well: headless use (the self-test, thumbnails).
mkdir -p "$out/platforms"
cp "$prefix/share/qt6/plugins/platforms/qoffscreen.dll" "$out/platforms/"

# Every DLL the binaries load from the prefix, transitively.
prefix_win=$(cygpath -u "$prefix")
for _ in 1 2 3 4 5 6 7 8; do
    added=0
    while IFS= read -r dll; do
        name=$(basename "$dll")
        [ -e "$out/$name" ] && continue
        cp "$dll" "$out/"
        added=$((added + 1))
    done < <(find "$out" -iname '*.exe' -o -iname '*.dll' | while read -r f; do ldd "$f"; done |
             awk '{print $3}' | grep -i "^$prefix_win/bin/" | sort -u)
    [ "$added" -eq 0 ] && break
done

cp "$here/../../LICENSE" "$out/LICENSE.txt"
echo "staged $(find "$out" -type f | wc -l) files, $(du -sh "$out" | cut -f1), in $out"
