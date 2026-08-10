#!/usr/bin/env bash
# Build the vv-gui Debian package (.deb) from a -DVV_BUILD_GUI=ON build tree.
#
# Unlike the CLI .deb (build-deb.sh), this is NOT a portable static artifact:
# a GUI has to link the distro's shared Qt6, so the package targets the distro
# release it was built on and says so in its version (…+ubuntu24.04). The
# libraries the target release's archive does not carry — on Ubuntu 24.04,
# Apache's Arrow/Parquet and libxlsxio — are bundled privately under
# /usr/lib/vv-gui and found through DT_RPATH, so `apt install ./vv-gui_….deb`
# needs no third-party repository.
# Everything else becomes a normal package dependency, computed below from the
# ldd closure of the shipped binaries.
#
# Run on the distro you are targeting, with the GUI built against its shared
# libs (the Apache Arrow apt repo, qt6-base-dev, … — see INSTALL.md):
#
#   cmake -S . -B build-gui -DCMAKE_BUILD_TYPE=Release -DVV_BUILD_GUI=ON
#   cmake --build build-gui -j$(nproc)
#   packaging/debian/build-gui-deb.sh          # → dist/vv-gui_<ver>_<arch>.deb
#
# Usage: packaging/debian/build-gui-deb.sh [build-dir] [output-dir]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BLD="${1:-$REPO/build-gui}"
OUT="${2:-$REPO/dist}"

VVG="$BLD/gui/vvg"
[ -x "$VVG" ] || { echo "error: vvg not found at $VVG" >&2
                   echo "       build with -DVV_BUILD_GUI=ON first." >&2
                   exit 1; }
for tool in dpkg-deb dpkg-query ldd patchelf strip; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool not found (sudo apt install dpkg patchelf binutils)" >&2
        exit 1; }
done

# Upstream version from the project() header — vvg has no --version flag.
# Same git-snapshot suffix rule as build-deb.sh so an untagged build sorts
# between the releases it sits between.
ver="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9.]+).*$/\1/p' "$REPO/CMakeLists.txt" | head -1)"
[ -n "$ver" ] || { echo "error: no VERSION in $REPO/CMakeLists.txt" >&2; exit 1; }
debver="$ver"
if desc="$(cd "$REPO" && git describe --tags --match "v$ver" 2>/dev/null)"; then
    if [ "$desc" != "v$ver" ]; then            # ahead of the tag
        n="${desc#v$ver-}"; debver="$ver+git${n%-g*}.${desc##*-g}"
    fi
fi
# Suffix the revision with the distro this build targets: the shared-lib
# package names in Depends are only valid there, and the name should say so.
# Rolling releases (Debian sid) have no VERSION_ID; fall back to the codename.
distro="$(. /etc/os-release && echo "${ID}${VERSION_ID:-${VERSION_CODENAME:-}}")"
debver="${debver}-1+${distro}"
arch="$(dpkg --print-architecture)"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
root="$work/vv-gui"
bundle_dir="/usr/lib/vv-gui"

install -Dm755 "$VVG" "$root/usr/bin/vvg"
strip --strip-unneeded "$root/usr/bin/vvg"

# Desktop integration — mirrors gui/kde/CMakeLists.txt's install rules and
# the Arch split package.
install -Dm644 "$REPO/gui/kde/org.vv.Viewer.desktop" \
        "$root/usr/share/applications/org.vv.Viewer.desktop"
install -Dm644 "$REPO/gui/kde/org.vv.Viewer.metainfo.xml" \
        "$root/usr/share/metainfo/org.vv.Viewer.metainfo.xml"
install -Dm644 "$REPO/gui/kde/vv-formats.xml" \
        "$root/usr/share/mime/packages/vv-formats.xml"
install -Dm644 "$REPO/gui/kde/icons/vv.svg" \
        "$root/usr/share/icons/hicolor/scalable/apps/vv.svg"

# KF6 plugins, when the build produced them. They need the KF6 dev packages
# (Debian 13+ / Ubuntu 25.10+); on Ubuntu 24.04 CMake skips them and so do
# we — the deb then ships the plain Qt GUI only. Nothing here has to change
# when the builder distro gains KF6: the files appear and get packaged.
multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || gcc -print-multiarch)"
qt_plugdir="/usr/lib/$multiarch/qt6/plugins"
payloads=("$root/usr/bin/vvg")
for plug in kf6/thumbcreator/vvthumbnail.so kf6/kfilemetadata/vvextractor.so; do
    built="$BLD/gui/kde/$(basename "$plug")"
    if [ -f "$built" ]; then
        install -Dm644 "$built" "$root$qt_plugdir/$plug"
        strip --strip-unneeded "$root$qt_plugdir/$plug"
        payloads+=("$root$qt_plugdir/$plug")
    fi
done

# ── Classify the ldd closure: archive dependency vs private bundle ────────────
# For every shared library the payloads need (transitively): if a distro
# archive package owns it, it becomes a Depends entry; if it comes from the
# Apache Arrow repository (libarrow*/libparquet* — not in Debian ≤ trixie or
# Ubuntu ≤ 25.04) or from no package at all (libxlsxio's source build installs
# to /usr/local), it is bundled. Anything the classifier gets wrong fails the
# pristine-container install test (test-gui-deb.sh), not the user.
owner_pkg() {
    # dpkg-query -S wants the path spelled the way the package shipped it;
    # ldd may hand us the /lib alias of a usr-merged path, or a symlink.
    # Success is a non-empty first line, NOT the pipeline status — head exits
    # 0 whether or not dpkg-query matched. Skip degenerate candidates ("",
    # "/usr"): dpkg-query -S on a bare directory matches half the archive.
    local p q out
    for p in "$1" "$(realpath -- "$1" 2>/dev/null || true)"; do
        [ -n "$p" ] || continue
        for q in "$p" "/usr$p" "${p#/usr}"; do
            case "$q" in */*/*) ;; *) continue ;; esac
            out="$(dpkg-query -S "$q" 2>/dev/null \
                   | grep -v ' diversion ' | grep -v '^diversion ' \
                   | head -1)" || true
            if [ -n "$out" ]; then
                echo "${out%%:*}"        # "pkg:arch: /path" → "pkg"
                return 0
            fi
        done
    done
    return 1
}

declare -A dep_pkgs=() bundle=()
missing=0
for elf in "${payloads[@]}"; do
    while read -r name path; do
        if [ "$name" = "MISSING" ]; then
            echo "error: $elf needs $path, which ldd cannot resolve" >&2
            missing=1
            continue
        fi
        if pkg="$(owner_pkg "$path")"; then
            case "$pkg" in
                libarrow*|libparquet*|libgandiva*|arrow*) bundle["$name"]="$path" ;;
                *) dep_pkgs["$pkg"]=1 ;;
            esac
        else
            bundle["$name"]="$path"
        fi
    done < <(ldd "$elf" \
             | awk '/=> not found/ { print "MISSING", $1; next }
                    /=>/           { print $1, $3 }')
done
[ "$missing" -eq 0 ] || exit 1

if [ "${#bundle[@]}" -gt 0 ]; then
    install -d "$root$bundle_dir"
    for name in "${!bundle[@]}"; do
        install -m644 "$(realpath -- "${bundle[$name]}")" "$root$bundle_dir/$name"
        # A bundled lib may need a bundled sibling (libparquet → libarrow).
        patchelf --force-rpath --set-rpath '$ORIGIN' "$root$bundle_dir/$name"
        echo "bundled: $name ($(owner_pkg "${bundle[$name]}" || echo 'no package'))"
    done
    # DT_RPATH, not RUNPATH: --force-rpath makes the entry apply to the whole
    # resolution chain, so libparquet's own NEEDED libarrow also resolves from
    # the bundle when loaded via vvg (RUNPATH would cover direct deps only).
    for elf in "${payloads[@]}"; do
        patchelf --force-rpath --set-rpath "$bundle_dir" "$elf"
    done
fi

# vvg dlopens its Qt platform plugin, so ldd cannot see it. On Ubuntu 24.04
# and Debian 13 both the xcb and the offscreen platform plugins ship inside
# libqt6gui6* itself, which IS in the closure — assert that rather than
# trusting it. Wayland users get their plugin via the qt6-wayland Recommends.
mapfile -t deps_sorted < <(printf '%s\n' "${!dep_pkgs[@]}" | LC_ALL=C sort -u)
printf -v depends '%s, ' "${deps_sorted[@]}"; depends="${depends%, }"
case "$depends" in
    *libqt6gui6*) ;;
    *) echo "error: libqt6gui6 missing from computed Depends ($depends) —" >&2
       echo "       the Qt platform plugins would not be installed" >&2
       exit 1 ;;
esac

install -d "$root/usr/share/doc/vv-gui"
{ echo "vv-gui — Qt6 desktop viewer for vv"
  echo "Upstream: https://github.com/balwierz/vv"
  echo
  echo "Bundled under $bundle_dir:"
  echo "  Apache Arrow / Parquet — Apache License 2.0"
  echo "    (text: /usr/share/common-licenses/Apache-2.0)"
  echo "  libxlsxio — MIT"
  echo
  echo "Copyright and license (MIT):"; echo
  cat "$REPO/LICENSE"; } > "$root/usr/share/doc/vv-gui/copyright"
chmod 644 "$root/usr/share/doc/vv-gui/copyright"   # not the builder's umask

instkb="$(du -sk "$root" | cut -f1)"
install -d "$root/DEBIAN"
cat > "$root/DEBIAN/control" <<CTL
Package: vv-gui
Version: $debver
Architecture: $arch
Maintainer: Piotr Balwierz <nikt@tuta.com>
Section: science
Priority: optional
Homepage: https://github.com/balwierz/vv
Installed-Size: $instkb
Depends: $depends
Recommends: qt6-wayland
Suggests: vv
Description: Qt6 desktop viewer for vv (Parquet, Arrow, HDF5, BAM, VCF, ...)
 vvg is the graphical frontend to vv, the universal genomic and tabular
 file viewer: a windowed table view over Parquet, Arrow IPC/Feather,
 HDF5/AnnData, BAM/CRAM/SAM, VCF/BCF, BED, GFF/GTF, FASTA/FASTQ,
 spreadsheets, NumPy and TSV/CSV, with background filtering and search.
 .
 Built for the distro release named in the package version. Apache
 Arrow/Parquet and libxlsxio are bundled under /usr/lib/vv-gui; every
 other dependency resolves from the distribution archive.
CTL

mkdir -p "$OUT"
deb="$OUT/vv-gui_${debver}_${arch}.deb"
dpkg-deb --root-owner-group --build "$root" "$deb"
echo "built: $deb"
