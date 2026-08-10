#!/usr/bin/env bash
# Build the vv + vv-gui RPMs from the current checkout, on a Fedora system
# (the release CI runs this inside a fedora:latest container; a Fedora box
# with the BuildRequires installed works the same).
#
# Source0 is a `git archive` of HEAD — not the GitHub tag tarball — so the
# same script serves PR CI (untagged trees) and the release build. Version
# comes from CMakeLists.txt' project() header, overriding the Version: in
# vv.spec (which is kept current for standalone rpmbuild users; see
# CONTRIBUTING.md's version-bump table).
#
# When run as root with dnf available (the container case), build deps are
# installed right here via `dnf builddep vv.spec` — the spec stays the single
# source of truth for what the build needs.
#
# Usage: packaging/rpm/build-rpm.sh [output-dir]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$REPO/dist}"
SPEC="$REPO/packaging/rpm/vv.spec"

if command -v dnf >/dev/null 2>&1 && [ "$(id -u)" = 0 ]; then
    # dnf5 keeps builddep in dnf5-plugins; dnf4 in dnf-plugins-core.
    dnf install -y rpm-build git-core dnf5-plugins 2>/dev/null \
        || dnf install -y rpm-build git-core dnf-plugins-core
    dnf builddep -y "$SPEC"
    # A bind-mounted checkout is owned by the host user; git refuses to read
    # it as root without this.
    git config --global --add safe.directory "$REPO"
fi
for tool in rpmbuild git curl cmake; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool not found (dnf install rpm-build git-core curl cmake," >&2
        echo "       then 'dnf builddep packaging/rpm/vv.spec')" >&2
        exit 1; }
done

ver="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9.]+).*$/\1/p' "$REPO/CMakeLists.txt" | head -1)"
[ -n "$ver" ] || { echo "error: no VERSION in $REPO/CMakeLists.txt" >&2; exit 1; }
xlsxio_ver="$(sed -nE 's/^%global xlsxio_version[[:space:]]+([0-9.]+).*$/\1/p' "$SPEC")"
[ -n "$xlsxio_ver" ] || { echo "error: no %global xlsxio_version in $SPEC" >&2; exit 1; }

top="$(mktemp -d)"; trap 'rm -rf "$top"' EXIT
mkdir -p "$top/SOURCES" "$top/SPECS"

git -C "$REPO" archive --format=tar.gz --prefix="vv-$ver/" \
    -o "$top/SOURCES/vv-$ver.tar.gz" HEAD
curl -fsSL --retry 3 -o "$top/SOURCES/xlsxio-$xlsxio_ver.tar.gz" \
    "https://github.com/brechtsanders/xlsxio/archive/refs/tags/$xlsxio_ver.tar.gz"

# The spec's Version: is only as fresh as the last release-checklist pass;
# the checkout is the truth here.
sed -E "s/^Version:.*/Version:        $ver/" "$SPEC" > "$top/SPECS/vv.spec"

rpmbuild -bb --define "_topdir $top" "$top/SPECS/vv.spec"

mkdir -p "$OUT"
find "$top/RPMS" -name '*.rpm' -exec cp {} "$OUT/" \;
echo "built:"
find "$top/RPMS" -name '*.rpm' -printf '  %f\n'
