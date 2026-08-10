# Installing `vv`

Prebuilt packages cover most users; build-from-source is below.

## Prebuilt binaries

Every tagged release publishes, for **x86_64** and **aarch64**:

| Asset | What it is |
|---|---|
| `vv-<ver>-linux-<arch>.tar.gz` | static binary, no runtime dependencies |
| `vv_<ver>-1_<deb-arch>.deb` | Debian package (same static binary) |
| `vv-gui_<ver>-1+ubuntu24.04_<deb-arch>.deb` | Qt6 GUI (`vvg`) — **Ubuntu 24.04 only** |
| `vv-<ver>-macos-arm64.tar.gz` | Apple Silicon; **not** static — needs the Homebrew deps |
| `SHA256SUMS` | checksums for everything above |

Replace `1.18.0` below with the
[latest release](https://github.com/balwierz/vv/releases/latest) if newer.

### Debian / Ubuntu

```sh
curl -LO https://github.com/balwierz/vv/releases/download/v1.18.0/vv_1.18.0-1_amd64.deb
sudo apt install ./vv_1.18.0-1_amd64.deb        # use arm64 on ARM
```

The package installs `/usr/bin/vv`, the man page and the shell completions.

The Qt6 GUI ships as its own package, built **for Ubuntu 24.04** — unlike the
CLI `.deb` it cannot be a portable static binary, because a GUI has to link
the distro's shared Qt6. Apache Arrow/Parquet and libxlsxio (which the
Ubuntu 24.04 archive does not carry) are bundled inside the package under
`/usr/lib/vv-gui`; everything else resolves from the standard archive, so
**no third-party apt repository is needed**. Grab the
`vv-gui_<ver>-1+ubuntu24.04_<deb-arch>.deb` asset from the
[latest release](https://github.com/balwierz/vv/releases/latest) — it is
first published with the release *after* v1.18.0 — then:

```sh
sudo apt install ./vv-gui_*_amd64.deb          # use arm64 on ARM
```

It installs `/usr/bin/vvg` plus the desktop entry, MIME types and icon. The
Dolphin thumbnailer / KFileMetaData plugins are **not** in it — they need
KF6, which Ubuntu 24.04 does not package. For those (or for any other
distro), build from source with `-DVV_BUILD_GUI=ON` (below), or use the Arch
split package.

### Static binary (any modern Linux)

```sh
base=https://github.com/balwierz/vv/releases/download/v1.18.0
arch=x86_64    # aarch64 on ARM (AWS Graviton, Raspberry Pi 5, …)

curl -LO $base/vv-1.18.0-linux-$arch.tar.gz
curl -LO $base/SHA256SUMS
sha256sum --check --ignore-missing SHA256SUMS

tar -xzf vv-1.18.0-linux-$arch.tar.gz
sudo install vv-1.18.0-linux-$arch/vv /usr/local/bin/
```

Requires glibc ≥ 2.28 (RHEL/Rocky/AlmaLinux 8+, Debian 10+, Ubuntu 18.04+).
Arrow, Parquet, htslib, HDF5, SQLite, ncurses and the compression stack are
all linked statically.

### Arch Linux

A split PKGBUILD ships in the repository — `vv` (CLI/TUI) and `vv-gui` (the
Qt6 desktop viewer plus the Dolphin thumbnailer and metadata plugins). One
build produces both. It is **not** in the AUR.

Two dependencies are **not in the official repositories**: `htslib` and
`xlsxio` are AUR-only. `makepkg -s` cannot install them — it resolves
dependencies with `pacman -S`, which never looks at the AUR — so install
them first:

```sh
paru -S htslib xlsxio          # or: yay -S htslib xlsxio

# …or without an AUR helper:
for p in htslib xlsxio; do
  git clone https://aur.archlinux.org/$p.git
  (cd $p && makepkg -si)
done
```

Then:

```sh
git clone https://github.com/balwierz/vv.git
cd vv/packaging/arch
makepkg -s                                       # one build, both packages
sudo pacman -U vv-*.pkg.tar.zst                  # CLI only
sudo pacman -U vv-*.pkg.tar.zst vv-gui-*.pkg.tar.zst   # CLI + GUI
```

`makepkg -i` installs *every* member of a split package's `pkgname` array
and there is no flag to pick one, so install the built package files
directly if you only want the CLI.

The PKGBUILD builds the **tagged release tarball it pins**, not your working
tree. To package local changes, bump `pkgver` and re-run `updpkgsums`
(from `pacman-contrib`), or use the source build below.

### Not currently published

`packaging/` also carries Bioconda, Homebrew and RPM recipes, but **no
package exists on those channels yet** — they are prepared, not published.
Use the `.deb`, the static tarball, or a source build.

## Build from source

### Requirements

Every item below is a **hard** dependency — CMake aborts at configure time if
any is missing. There is no partial build.

- CMake ≥ 3.16, and `git` + network access (mimalloc is fetched during configure)
- GCC ≥ 10 or Clang ≥ 12 (C++20), and `pkg-config` (htslib is found only via it)
- Apache Arrow + Parquet development libraries (Arrow ≥ 20 recommended)
- htslib, ncurses, zlib, SQLite3
- HDF5, expat, minizip
- libxlsxio — **not packaged by any major distribution**; see below

libBigWig and md4c are vendored under `vendored/` and need no packages.
Optional and auto-detected: Arrow's ORC adapter, and KF6 for the GUI plugins.

### Debian / Ubuntu

Arrow and Parquet are **not** in Debian ≤ trixie or Ubuntu ≤ 25.04 — add
Apache's own repository first. (Skip that step on Debian forky/sid or
Ubuntu 26.04+, where the distro carries them.)

```sh
sudo apt-get install -y \
  cmake g++ git pkg-config \
  libncurses-dev libhts-dev libsqlite3-dev zlib1g-dev \
  libexpat1-dev libminizip-dev libhdf5-dev

sudo apt-get install -y ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt-get install -y ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt-get update
sudo apt-get install -y libarrow-dev libparquet-dev
```

> **Note** — `libhts-dev` depends on `libcurl4-gnutls-dev` while Apache's
> `libarrow-dev` depends on `libcurl4-openssl-dev`, and the two conflict.
> Install `libhts-dev` first on a clean system; if apt reports a conflict,
> `sudo apt-get remove libcurl4-openssl-dev` and retry.

### Fedora / RHEL / Rocky / AlmaLinux

```sh
# Fedora 43+
sudo dnf install cmake gcc-c++ git pkgconf-pkg-config \
    libarrow-devel parquet-libs-devel htslib-devel ncurses-devel \
    hdf5-devel sqlite-devel expat-devel zlib-devel minizip-ng-compat-devel

# Fedora 44+ additionally (Arrow ≥ 21 split the compute kernels out):
sudo dnf install libarrow-compute-devel
```

RHEL, AlmaLinux and Rocky need **EPEL and CRB** enabled first — Arrow,
Parquet, htslib, HDF5 and minizip come from EPEL, and EPEL's `libarrow-devel`
pulls `utf8proc-devel`, which lives only in CRB:

```sh
sudo dnf install epel-release dnf-plugins-core
sudo dnf config-manager --set-enabled crb     # EL8: --set-enabled powertools
sudo dnf install cmake gcc-c++ git pkgconf-pkg-config \
    libarrow-devel parquet-libs-devel htslib-devel ncurses-devel \
    hdf5-devel sqlite-devel expat-devel zlib-devel minizip-devel
```

On EL10 minizip is `minizip-ng-compat-devel` instead. **EL8 and EL9 are not
usable build targets**: EPEL carries Arrow 8.0.1 and 9.0.0 there, far older
than the Arrow 20–23 vv is developed against. Use the static tarball.

### Arch

See the Arch Linux section above — `htslib` and `xlsxio` come from the AUR.

```sh
sudo pacman -S --needed base-devel git cmake arrow ncurses zlib \
                        hdf5 sqlite expat minizip
paru -S htslib xlsxio
```

`arrow` supplies Parquet too (it `provides=parquet-cpp`).

### libxlsxio (every Linux distro)

`libxlsxio` is not packaged in Debian, Ubuntu, Fedora or EPEL under any name,
and it is a hard dependency. Build it from source:

```sh
curl -fsSL -o xlsxio.tar.gz https://github.com/brechtsanders/xlsxio/archive/refs/tags/0.2.36.tar.gz
tar xf xlsxio.tar.gz
cmake -S xlsxio-0.2.36 -B xlsxio-bld -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_SHARED=ON -DBUILD_STATIC=OFF \
  -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCUMENTATION=OFF \
  -DWITH_LIBZIP=OFF -DWITH_MINIZIP=ON
cmake --build xlsxio-bld -j$(nproc)
sudo cmake --install xlsxio-bld && sudo ldconfig
```

### Optional: Qt6 GUI (`-DVV_BUILD_GUI=ON`)

On Ubuntu 24.04 there is a prebuilt `vv-gui` `.deb` (see
[Debian / Ubuntu](#debian--ubuntu) above); everywhere else, build it:

```sh
sudo apt-get install -y qt6-base-dev extra-cmake-modules            # Debian/Ubuntu
sudo pacman -S qt6-base extra-cmake-modules                          # Arch
```

The Dolphin thumbnailer and KFileMetaData plugins additionally need KF6, and
are skipped with a status message if it is absent. The KF6 dev packages need
Debian 13 (trixie)+ or Ubuntu 25.10+ — they are not in Ubuntu 24.04:

```sh
sudo apt-get install -y libkf6kio-dev libkf6coreaddons-dev libkf6filemetadata-dev
sudo pacman -S kio kcoreaddons kfilemetadata                          # Arch
```

### macOS

Apple Silicon binaries are published with each release (see the table at the
top), and the full test suite runs on macOS in CI. To build from source:

```sh
brew install cmake apache-arrow htslib ncurses xlsxio expat minizip hdf5
```

Every one of those lives under its own prefix, so **each has to be named**.
Listing only some of them is the usual failure: `ncurses` in particular is
**keg-only**, so leaving it out silently resolves to Apple's bundled ncurses
5.7, which lacks `set_escdelay()` and `BUTTON5_PRESSED`. CMake now stops with
an explanation when that happens rather than emitting undefined-identifier
errors.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix apache-arrow);$(brew --prefix htslib);\
$(brew --prefix ncurses);$(brew --prefix xlsxio);$(brew --prefix expat);\
$(brew --prefix minizip);$(brew --prefix hdf5)"
cmake --build build -j$(sysctl -n hw.ncpu)
```

Two platform limitations, both reported honestly at runtime rather than
silently:

- **ORC is unavailable.** Homebrew's `apache-arrow` is built without the ORC
  adapter, so `vv file.orc` says "compiled without Apache ORC support".
- **Intel Macs are not published.** `macos-latest` is Apple Silicon; an Intel
  Mac builds from source with the same commands.

Unlike the Linux tarball, the macOS one is **not** a static binary — it links
the Homebrew libraries above, so install them before running it.

### Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary lands at `build/vv`.

### System install

```sh
sudo cmake --install build
```

This installs `vv` to `${CMAKE_INSTALL_PREFIX}/bin/vv` (default `/usr/local/bin`),
the man page to `share/man/man1/`, and shell completions to the appropriate
distro paths (`bash-completion/completions`, `fish/vendor_completions.d`,
`zsh/site-functions`).

## Portable static binary (Docker, AlmaLinux 8)

A fully static binary (glibc ≥ 2.28) is built inside AlmaLinux 8 from pinned
upstream source tarballs. `docker-sources/` is **gitignored**, so a fresh
clone does not contain them — fetch them first:

```sh
bash scripts/fetch-docker-sources.sh    # ~300 MB downloaded, ~98 MB kept
docker build --network=host -f Dockerfile.almalinux8 -t vv_build .
mkdir -p dist && docker run --rm -v "$PWD/dist:/out" vv_build
# binary at ./dist/vv (plus a `vh` symlink)
```

`scripts/fetch-docker-sources.sh` needs `curl`, `tar` and **`cmake`** on the
host — it runs a real CMake install to harvest xsimd's generated config
files. It is idempotent: tarballs already present are kept, so re-running it
after a failed build is cheap. Most of the download is the Boost release
archive (~211 MB), from which only the headers are repacked.

The container build itself **does** need network access: the base layer
installs gcc-toolset-12 with `dnf`, and BuildKit pulls the `almalinux:8`
base image. What the bundled tarballs buy is that no *library source* is
fetched during the build, so dependency versions are fully pinned.

Bundled and linked statically: zlib, zstd, lz4, bzip2, xz, libdeflate,
brotli, snappy, utf8proc, abseil, re2, thrift, ncurses, htslib 1.21, SQLite,
expat, minizip-ng, xlsxio, HDF5 1.14.4, mimalloc, the Boost / RapidJSON /
xsimd headers, and Arrow 23.0.1. Parquet compression codecs (snappy, zstd,
lz4, brotli, bz2) are enabled; ORC is **not**, so the static binary reports
"compiled without ORC support" on `.orc` files. First build ~60–90 min cold;
subsequent app-only rebuilds are fast (the dependency layers cache).

## Shell completion (manual install)

If you build from source without `cmake --install`:

```sh
# Bash (per-user)
mkdir -p ~/.local/share/bash-completion/completions
cp completions/vv.bash ~/.local/share/bash-completion/completions/vv

# Fish
cp completions/vv.fish ~/.config/fish/completions/

# Zsh — copy `_vv` into any directory in $fpath, e.g.:
cp completions/_vv ~/.zsh/completions/   # ensure this dir is in fpath
```

## Notes

- mimalloc (v2.1.9) is fetched automatically from GitHub during the CMake
  configure step. An internet connection is required on first source build.
- Arrow, Parquet, htslib, and ncurses are linked statically when the static
  AlmaLinux 8 build is used; the dynamic build uses whichever versions are
  on the system.
