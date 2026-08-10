#!/usr/bin/env bash
# Install-test the vv + vv-gui RPMs inside a PRISTINE Fedora container.
# Proves what the build host cannot: every Requires resolves from the Fedora
# repos alone (Arrow, Qt6, KF6, htslib, HDF5 — nothing from the build
# environment), the bundled-static xlsxio needs no runtime package, and both
# binaries actually run against a real file.
#
#   docker run --rm -v "$PWD:/mnt" fedora:latest \
#     bash /mnt/packaging/rpm/test-rpm.sh \
#       "/mnt/$(ls vv-[0-9]*.rpm)" "/mnt/$(ls vv-gui-[0-9]*.rpm)" \
#       /mnt/tests/data/tiny.parquet
set -euxo pipefail

cli_rpm="$1"    # absolute path to vv-<ver>....rpm inside the container
gui_rpm="$2"    # absolute path to vv-gui-<ver>....rpm
fixture="$3"    # a small data file both viewers should open

dnf install -y "$cli_rpm" "$gui_rpm"

rpm -q vv vv-gui
# The Dolphin/KFileMetaData plugins must be in the gui package — this is the
# first prebuilt artifact that carries them at all.
rpm -ql vv-gui | grep -q 'thumbcreator/vvthumbnail.so'
rpm -ql vv-gui | grep -q 'kfilemetadata/vvextractor.so'

# CLI end-to-end: version, and an actual read through the Arrow reader core.
vv --version
test -n "$(vv -n 2 --tsv "$fixture")"

# Nothing may dangle after install.
ldd /usr/bin/vvg
if ldd /usr/bin/vvg | grep 'not found'; then
    echo "error: unresolved libraries after install" >&2
    exit 1
fi

# GUI end-to-end through libvvcore, headless: the model path, then the
# app-shell path the menus and drag-drop use.
export QT_QPA_PLATFORM=offscreen
VVG_SELFTEST=1 vvg "$fixture"
test "$(VVG_WINTEST=1 vvg "$fixture")" = "win_tabs=1"

echo "vv/vv-gui RPM install test: OK"
