#!/usr/bin/env bash
# Build a Debian package (.deb) from the static `vv` binary produced by
# Dockerfile.almalinux8 (glibc >= 2.28, self-contained — no Arrow/htslib/HDF5
# runtime deps). Run the static build first, e.g.:
#
#   bash scripts/fetch-docker-sources.sh
#   docker build --network=host -f Dockerfile.almalinux8 -t vv_build .
#   docker run --rm -v "$PWD/dist:/out" vv_build      # → dist/vv
#   packaging/debian/build-deb.sh                     # → dist/vv_<ver>_<arch>.deb
#
# Usage: packaging/debian/build-deb.sh [path/to/static/vv] [output-dir]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$REPO/dist/vv}"
OUT="${2:-$REPO/dist}"

[ -x "$BIN" ] || { echo "error: static vv binary not found at $BIN" >&2
                   echo "       build it with Dockerfile.almalinux8 first." >&2
                   exit 1; }

# Upstream version from the binary; add a git snapshot suffix when the checkout
# is ahead of the matching tag so the .deb sorts correctly (1.10.0 < snapshot
# < 1.10.1).
ver="$("$BIN" --version | awk '{print $2}')"
debver="$ver"
if desc="$(cd "$REPO" && git describe --tags --match "v$ver" 2>/dev/null)"; then
    if [ "$desc" != "v$ver" ]; then            # ahead of the tag
        n="${desc#v$ver-}"; debver="$ver+git${n%-g*}.${desc##*-g}"
    fi
fi
debver="${debver}-1"
arch="$(dpkg --print-architecture 2>/dev/null || echo amd64)"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
root="$work/vv"

install -Dm755 "$BIN" "$root/usr/bin/vv"
ln -s vv "$root/usr/bin/vh"
if [ -f "$REPO/man/vv.1" ]; then
    install -Dm644 "$REPO/man/vv.1" "$root/usr/share/man/man1/vv.1"
    gzip -9n "$root/usr/share/man/man1/vv.1"
fi
install -Dm644 "$REPO/completions/vv.bash" "$root/usr/share/bash-completion/completions/vv"
install -Dm644 "$REPO/completions/vv.fish" "$root/usr/share/fish/vendor_completions.d/vv.fish"
install -Dm644 "$REPO/completions/_vv"      "$root/usr/share/zsh/site-functions/_vv"

install -d "$root/usr/share/doc/vv"
{ echo "vv — universal genomic file viewer"
  echo "Upstream: https://github.com/balwierz/vv"
  echo; echo "Copyright and license (MIT):"; echo
  cat "$REPO/LICENSE"; } > "$root/usr/share/doc/vv/copyright"

# The static binary still links the C/C++ runtimes dynamically; declare only
# what `ldd` actually reports so the package installs on any modern Debian.
deps="libc6 (>= 2.28)"
ldd "$BIN" 2>/dev/null | grep -q 'libstdc++' && deps="$deps, libstdc++6"
ldd "$BIN" 2>/dev/null | grep -q 'libgcc_s'  && deps="$deps, libgcc-s1"

instkb="$(du -sk "$root" | cut -f1)"
install -d "$root/DEBIAN"
cat > "$root/DEBIAN/control" <<CTL
Package: vv
Version: $debver
Architecture: $arch
Maintainer: Piotr Balwierz <nikt@tuta.com>
Section: science
Priority: optional
Homepage: https://github.com/balwierz/vv
Installed-Size: $instkb
Depends: $deps
Description: Universal genomic and tabular file viewer
 vv is a fast, self-contained command-line viewer (and ncurses TUI) for
 Parquet, Arrow IPC/Feather, HDF5/AnnData, BAM/CRAM/SAM, VCF/BCF, BED,
 GFF/GTF, FASTA/FASTQ, PAF, UCSC bigBed/bigWig/2bit, SQLite, xlsx/ods,
 NumPy .npz and TSV/CSV (plus .gz variants and tabix range queries).
 .
 Ships the static, self-contained binary; \`vh\` is a symlink that defaults
 to the transposed "vertical head" view.
CTL

mkdir -p "$OUT"
deb="$OUT/vv_${debver}_${arch}.deb"
dpkg-deb --root-owner-group --build "$root" "$deb"
echo "built: $deb"
