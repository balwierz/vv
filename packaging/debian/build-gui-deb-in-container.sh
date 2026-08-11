#!/usr/bin/env bash
# Build the vv-gui .deb INSIDE a Debian (or Ubuntu) container, for the
# container's own distro release. GitHub has no Debian runners, so the
# Debian 13 flavor is produced this way — same pattern as the Fedora RPMs:
#
#   docker run --rm -v "$PWD:/mnt" -w /mnt debian:13 \
#     bash packaging/debian/build-gui-deb-in-container.sh .
#
# Installs the same reader deps as the Ubuntu runner jobs (one shared
# script), the Qt6 + KF6 GUI deps — Debian 13 packages KF6, so unlike the
# Ubuntu 24.04 flavor this .deb carries the Dolphin/KFileMetaData plugins —
# builds with VV_BUILD_GUI=ON, smoke-tests vvg offscreen, and hands the tree
# to build-gui-deb.sh, which is distro-agnostic (Depends are computed from
# whatever distro it runs on, and the version suffix names it).
#
# Usage: build-gui-deb-in-container.sh [output-dir]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$REPO/dist}"
export DEBIAN_FRONTEND=noninteractive

bash "$REPO/scripts/setup-linux-build-deps.sh"

SUDO="sudo"
[ "$(id -u)" = 0 ] && SUDO=""
$SUDO apt-get install -y -V qt6-base-dev extra-cmake-modules patchelf
# KF6 best-effort, same rule as everywhere else: absent → CMake (and hence
# the .deb) simply skips the plugins.
$SUDO apt-get install -y -V \
    libkf6kio-dev libkf6coreaddons-dev libkf6filemetadata-dev \
    || echo "KF6 dev packages unavailable; the .deb ships without the Dolphin/KFileMetaData plugins."

# A bind-mounted checkout is owned by the host user; git (mimalloc
# FetchContent at configure, and build-gui-deb.sh's version suffix) refuses
# to read it as root without this.
if [ "$(id -u)" = 0 ]; then
    git config --global --add safe.directory "$REPO"
fi

cmake -S "$REPO" -B "$REPO/build-gui" -DCMAKE_BUILD_TYPE=Release -DVV_BUILD_GUI=ON
cmake --build "$REPO/build-gui" -j"$(nproc)"

# Offscreen smoke before packaging, mirroring the release jobs (committed
# fixture — no generate.py here).
export QT_QPA_PLATFORM=offscreen
VVG_SELFTEST=1 "$REPO/build-gui/gui/vvg" "$REPO/tests/data/tiny.parquet"
test "$(VVG_WINTEST=1 "$REPO/build-gui/gui/vvg" "$REPO/tests/data/tiny.parquet")" = "win_tabs=1"

bash "$REPO/packaging/debian/build-gui-deb.sh" "$REPO/build-gui" "$OUT"
