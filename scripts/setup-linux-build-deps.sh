#!/usr/bin/env bash
# Shared-library build dependencies of the vv reader core on Debian/Ubuntu:
# the distro packages, the Apache Arrow apt repository (with the
# libcurl4-openssl-dev / -gnutls-dev conflict patched out of the -dev
# packages), and libxlsxio built from source.
#
# Called by the .github/actions/setup-linux-deps composite action (Ubuntu
# GitHub runners, via sudo) and by
# packaging/debian/build-gui-deb-in-container.sh (Debian containers, as
# root) — one copy, no drift. lsb_release feeds the right distro path of
# Apache's repository, which carries every current Debian and Ubuntu release.
set -euo pipefail

SUDO="sudo"
[ "$(id -u)" = 0 ] && SUDO=""
export DEBIAN_FRONTEND=noninteractive

$SUDO apt-get update
# Phase 1 — install everything except libarrow-dev / libparquet-dev.
# libhts-dev hard-Depends on libcurl4-gnutls-dev; runners come
# with the openssl variant preinstalled. Remove openssl-dev
# first so gnutls-dev (and libhts-dev) install cleanly.
#
# git + curl: preinstalled on GitHub runners, absent from bare Debian
# containers (git is needed by CMake's mimalloc FetchContent and the
# packaging scripts' version derivation). libncurses-dev, not
# libncursesw5-dev: the transitional -w5 name was REMOVED in Debian 13,
# while libncurses-dev exists on both distros (see the 1.17.0 changelog —
# INSTALL.md tripped over exactly this).
$SUDO apt-get remove -y libcurl4-openssl-dev || true
$SUDO apt-get install -y -V \
  cmake g++ git curl libcurl4-gnutls-dev \
  libhts-dev libncurses-dev libsqlite3-dev \
  libexpat1-dev libminizip-dev libhdf5-dev pkg-config \
  python3 python3-venv python3-pip \
  python3-pysam tabix bcftools ca-certificates lsb-release wget

# Phase 2 — Apache Arrow apt repo. Bound the retries: wget defaults
# to --tries=20, so an outage at Apache's artifact host burns ~45
# minutes before the job goes red (seen 2026-07-27, when
# apache.jfrog.io timed out on all three of its IPs). Failing in
# ~2 minutes says the same thing and frees the runner. Matches the
# `curl --retry 3` already used for xlsxio below.
wget --tries=3 --timeout=20 --waitretry=10 \
  https://apache.jfrog.io/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
$SUDO apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
$SUDO apt-get update

# Phase 3 — Arrow's libarrow-dev / libparquet-dev hard-Depend
# on libcurl4-openssl-dev. There's no alternative apt will
# take when openssl-dev conflicts with libhts-dev's
# libcurl4-gnutls-dev. Runtime libs (libarrowNNN /
# libparquetNNN) only depend on libcurl4 itself, not the
# -dev package, so they install cleanly. Patch the bogus
# build-only libcurl4-openssl-dev dep out of the -dev .debs
# before installing those.
ARROW_RT=$(apt-cache pkgnames libarrow | grep -E '^libarrow[0-9]+$' | sort -V | tail -1)
PARQ_RT=$(apt-cache pkgnames libparquet | grep -E '^libparquet[0-9]+$' | sort -V | tail -1)
echo "Runtime libs: $ARROW_RT / $PARQ_RT"
$SUDO apt-get install -y -V "$ARROW_RT" "$PARQ_RT"

mkdir -p /tmp/arrow-debs && cd /tmp/arrow-debs
apt-get download libarrow-dev libparquet-dev
patched=()
for deb in libarrow-dev_*.deb libparquet-dev_*.deb; do
  mkdir -p extract
  dpkg-deb -R "$deb" extract
  # Drop the libcurl4-openssl-dev token (and its leading
  # ", " if any) from the Depends line.
  sed -i -E 's/(, *)?libcurl4-openssl-dev( *\([^)]*\))?//g' extract/DEBIAN/control
  out="/tmp/arrow-debs/$(basename "$deb" .deb).patched.deb"
  dpkg-deb -b extract "$out"
  patched+=("$out")
  rm -rf extract
done
# apt-get install on local .debs resolves the rest of the
# transitive deps (libabsl-dev, libsnappy-dev, libthrift-dev, …)
# from configured repos.
$SUDO apt-get install -y -V --no-install-recommends "${patched[@]}"
cd -

# libxlsxio isn't packaged on Debian or Ubuntu. It's small (~1500 LOC C);
# build a shared lib against libexpat / libminizip and install to
# /usr/local. The version here matches the AlmaLinux 8 Docker
# build (scripts/fetch-docker-sources.sh).
cd /tmp
curl -fsSL --retry 3 -o xlsxio.tar.gz \
  https://github.com/brechtsanders/xlsxio/archive/refs/tags/0.2.36.tar.gz
tar xf xlsxio.tar.gz
cmake -S xlsxio-* -B xlsxio-bld \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DBUILD_SHARED=ON -DBUILD_STATIC=OFF \
  -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF \
  -DBUILD_DOCUMENTATION=OFF \
  -DWITH_LIBZIP=OFF -DWITH_MINIZIP=ON
cmake --build xlsxio-bld -j$(nproc)
$SUDO cmake --install xlsxio-bld
$SUDO ldconfig
cd -
