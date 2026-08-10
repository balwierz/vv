#!/usr/bin/env bash
# Install-test a vv-gui .deb inside a PRISTINE container of the distro it
# targets. This proves what no build-host test can: that the computed Depends
# resolve from the distro archive alone (no Apache Arrow repo, no /usr/local),
# that the bundled /usr/lib/vv-gui libraries are found through DT_RPATH, and
# that vvg starts and reads a file with nothing but the package's own
# dependency closure installed.
#
#   deb="$(ls vv-gui_*.deb)"
#   docker run --rm -v "$PWD:/mnt" ubuntu:24.04 \
#     bash /mnt/packaging/debian/test-gui-deb.sh "/mnt/$deb" /mnt/tests/data/tiny.parquet
set -euxo pipefail

deb="$1"        # absolute path to the .deb inside the container
fixture="$2"    # a small data file vvg should open (any supported format)

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q "$deb"

# Nothing may be left dangling after install, and the bundled Arrow — not a
# system copy — must be the one in use (a pristine container has no other).
ldd /usr/bin/vvg
if ldd /usr/bin/vvg | grep 'not found'; then
    echo "error: unresolved libraries after install" >&2
    exit 1
fi
ldd /usr/bin/vvg | grep -q '/usr/lib/vv-gui/'

# End-to-end through libvvcore, headless: the model path, then the app-shell
# path the menus and drag-drop use (mirrors ci.yml's "Verify GUI artifacts").
export QT_QPA_PLATFORM=offscreen
VVG_SELFTEST=1 vvg "$fixture"
test "$(VVG_WINTEST=1 vvg "$fixture")" = "win_tabs=1"

echo "vv-gui deb install test: OK"
