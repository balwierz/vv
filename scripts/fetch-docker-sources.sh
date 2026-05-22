#!/usr/bin/env bash
# Download every source tarball that Dockerfile.almalinux8 expects.
#
# Why this script exists
# ----------------------
# `docker-sources/` is gitignored — the AlmaLinux 8 static build pulls
# in ~58 MB of bundled tarballs, which we don't want to bloat the repo
# with. CI gets them via this script before invoking docker build;
# local developers can run it too.
#
# Output
# ------
# Files land in `docker-sources/` at the repository root with the
# exact basenames Dockerfile.almalinux8 expects (zlib.tar.gz etc.).
# Existing files are kept — re-running the script after a fresh
# clone is fast for everything already on disk.
#
# Versions are pinned in this file; bump them deliberately when
# moving deps forward.
set -euo pipefail
cd "$(dirname "$0")/.."

DST="docker-sources"
mkdir -p "$DST"

fetch() {
    local out="$1" url="$2"
    if [ -s "$DST/$out" ]; then
        echo "  ok    $out (already present)"
        return
    fi
    echo "  fetch $out ← $url"
    curl -fsSL --retry 3 -o "$DST/$out.part" "$url"
    mv "$DST/$out.part" "$DST/$out"
}

# ── direct upstream releases (canonical URLs, archive bytes stable) ─────────
fetch absl.tar.gz       https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz
fetch arrow.tar.gz      https://github.com/apache/arrow/archive/refs/tags/apache-arrow-23.0.1.tar.gz
fetch brotli.tar.gz     https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz
fetch bzip2.tar.gz      https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
fetch deflate.tar.gz    https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.22.tar.gz
fetch htslib.tar.bz2    https://github.com/samtools/htslib/releases/download/1.21/htslib-1.21.tar.bz2
fetch lz4.tar.gz        https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
fetch mimalloc.tar.gz   https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.9.tar.gz
fetch ncurses.tar.gz    https://invisible-mirror.net/archives/ncurses/ncurses-6.5.tar.gz
fetch re2.tar.gz        https://github.com/google/re2/archive/refs/tags/2024-07-02.tar.gz
fetch snappy.tar.gz     https://github.com/google/snappy/archive/refs/tags/1.2.1.tar.gz
fetch thrift.tar.gz     https://github.com/apache/thrift/archive/refs/tags/v0.21.0.tar.gz
fetch utf8proc.tar.gz   https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.10.0.tar.gz
fetch xz.tar.gz         https://github.com/tukaani-project/xz/releases/download/v5.4.6/xz-5.4.6.tar.gz
fetch zlib.tar.gz       https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
fetch zstd.tar.gz       https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz
fetch sqlite.tar.gz     https://sqlite.org/2025/sqlite-autoconf-3490100.tar.gz
fetch expat.tar.gz      https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.gz
fetch minizip-ng.tar.gz https://github.com/zlib-ng/minizip-ng/archive/refs/tags/4.0.7.tar.gz
fetch xlsxio.tar.gz     https://github.com/brechtsanders/xlsxio/archive/refs/tags/0.2.36.tar.gz
fetch hdf5.tar.gz       https://support.hdfgroup.org/releases/hdf5/v1_14/v1_14_4/downloads/hdf5-1.14.4.tar.gz

# ── header-only deps repacked from upstream releases ───────────────────────
# The Dockerfile expects these tarballs to extract directly under
# `/usr/include/` with the top-level dir being `boost/`, `rapidjson/`,
# `xsimd/`. Upstream archives wrap those in a versioned parent
# directory, so we repack on the fly.
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

repack_header_only() {
    # $1 = output tarball name in docker-sources/
    # $2 = upstream archive URL
    # $3 = path within the extracted upstream tree to keep
    # $4 = name of the top-level directory in the output tarball
    local out="$1" url="$2" subpath="$3" topdir="$4"
    if [ -s "$DST/$out" ]; then
        echo "  ok    $out (already present)"
        return
    fi
    echo "  fetch $out ← $url (repacking $subpath → $topdir/)"
    curl -fsSL --retry 3 -o "$TMP/raw.tar.gz" "$url"
    mkdir -p "$TMP/extract"
    tar -xzf "$TMP/raw.tar.gz" -C "$TMP/extract"
    # The upstream tarball has exactly one top-level dir.
    local upstream_top
    upstream_top=$(ls "$TMP/extract" | head -1)
    # Move the subpath out and rename to the expected top dir.
    rm -rf "$TMP/staging"
    mkdir -p "$TMP/staging"
    cp -r "$TMP/extract/$upstream_top/$subpath" "$TMP/staging/$topdir"
    tar -czf "$DST/$out.part" -C "$TMP/staging" "$topdir"
    mv "$DST/$out.part" "$DST/$out"
    rm -rf "$TMP/extract" "$TMP/staging" "$TMP/raw.tar.gz"
}

repack_header_only rapidjson-headers.tar.gz \
    https://github.com/Tencent/rapidjson/archive/refs/tags/v1.1.0.tar.gz \
    include/rapidjson  rapidjson

# xsimd is header-only but the Dockerfile expects the *installed* layout —
# the upstream archive ships `xsimdConfig.cmake.in` (a CMake template) but
# not the final `xsimdConfig.cmake` / `xsimdConfigVersion.cmake` /
# `xsimdTargets.cmake` files that `cmake --install` generates. So we
# actually run a CMake install into a temp prefix and harvest the result.
repack_xsimd() {
    local out="xsimd.tar.gz"
    if [ -s "$DST/$out" ]; then
        echo "  ok    $out (already present)"
        return
    fi
    local url="https://github.com/xtensor-stack/xsimd/archive/refs/tags/14.1.0.tar.gz"
    echo "  fetch $out ← $url (cmake-install to harvest headers + Config)"
    curl -fsSL --retry 3 -o "$TMP/xsimd.tar.gz" "$url"
    mkdir -p "$TMP/x-extract"
    tar -xzf "$TMP/xsimd.tar.gz" -C "$TMP/x-extract"
    local upstream_top
    upstream_top=$(ls "$TMP/x-extract" | head -1)
    rm -rf "$TMP/x-build" "$TMP/x-install"
    cmake -S "$TMP/x-extract/$upstream_top" -B "$TMP/x-build" \
        -DCMAKE_INSTALL_PREFIX="$TMP/x-install" \
        -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF \
        -DENABLE_XTL_COMPLEX=OFF > /dev/null
    cmake --install "$TMP/x-build" > /dev/null
    # `cmake --install` produces:
    #   $TMP/x-install/include/xsimd/         (headers)
    #   $TMP/x-install/share/cmake/xsimd/     (xsimdConfig.cmake, …)
    # The Dockerfile wants both flattened into a single `xsimd/` tar.
    rm -rf "$TMP/x-staging/xsimd"
    mkdir -p "$TMP/x-staging/xsimd"
    cp -r "$TMP/x-install/include/xsimd/." "$TMP/x-staging/xsimd/"
    cp "$TMP/x-install"/share/cmake/xsimd/*.cmake "$TMP/x-staging/xsimd/"
    tar -czf "$DST/$out.part" -C "$TMP/x-staging" xsimd
    mv "$DST/$out.part" "$DST/$out"
    rm -rf "$TMP/x-extract" "$TMP/x-staging" "$TMP/x-build" "$TMP/x-install" "$TMP/xsimd.tar.gz"
}
repack_xsimd

# Boost is the heaviest dep — the full release archive is ~150 MB,
# of which we want just the boost/ headers subdirectory (~30 MB
# uncompressed). The repacked tarball has top-level dir `boost/` to
# match the Dockerfile's `tar xf … -C /usr/include/`.
repack_boost() {
    local out="boost-headers.tar.gz"
    if [ -s "$DST/$out" ]; then
        echo "  ok    $out (already present)"
        return
    fi
    local url="https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.gz"
    echo "  fetch $out ← $url (≈150 MB download; extracting headers only)"
    curl -fsSL --retry 3 -o "$TMP/boost.tar.gz" "$url"
    mkdir -p "$TMP/b-extract"
    tar -xzf "$TMP/boost.tar.gz" -C "$TMP/b-extract"
    local upstream_top
    upstream_top=$(ls "$TMP/b-extract" | head -1)
    rm -rf "$TMP/b-staging/boost"
    mkdir -p "$TMP/b-staging"
    # Copy the boost/ header tree (NOT the full source; libs/ etc. are dropped).
    cp -r "$TMP/b-extract/$upstream_top/boost" "$TMP/b-staging/boost"
    tar -czf "$DST/$out.part" -C "$TMP/b-staging" boost
    mv "$DST/$out.part" "$DST/$out"
    rm -rf "$TMP/b-extract" "$TMP/b-staging" "$TMP/boost.tar.gz"
}
repack_boost

echo
echo "All tarballs ready in $DST/"
du -sh "$DST"
