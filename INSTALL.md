# Installing `vv`

Prebuilt packages cover most users; build-from-source is below.

## Prebuilt binaries

Every tagged release publishes, for **x86_64** and **aarch64**:

| Asset | What it is |
|---|---|
| `vv-<ver>-linux-<arch>.tar.gz` | static binary, no runtime dependencies |
| `vv_<ver>-1_<deb-arch>.deb` | Debian package (same static binary) |
| `SHA256SUMS` | checksums for everything above |

Replace `1.16.0` below with the
[latest release](https://github.com/balwierz/vv/releases/latest) if newer.

### Debian / Ubuntu

```sh
curl -LO https://github.com/balwierz/vv/releases/download/v1.16.0/vv_1.16.0-1_amd64.deb
sudo apt install ./vv_1.16.0-1_amd64.deb        # use arm64 on ARM
```

The package installs `/usr/bin/vv`, the man page and the shell completions.

### Static binary (any modern Linux)

```sh
base=https://github.com/balwierz/vv/releases/download/v1.16.0
arch=x86_64    # aarch64 on ARM (AWS Graviton, Raspberry Pi 5, …)

curl -LO $base/vv-1.16.0-linux-$arch.tar.gz
curl -LO $base/SHA256SUMS
sha256sum --check --ignore-missing SHA256SUMS

tar -xzf vv-1.16.0-linux-$arch.tar.gz
sudo install vv-1.16.0-linux-$arch/vv /usr/local/bin/
```

Requires glibc ≥ 2.28 (RHEL/Rocky/AlmaLinux 8+, Debian 10+, Ubuntu 18.04+).
Arrow, Parquet, htslib, HDF5, SQLite, ncurses and the compression stack are
all linked statically.

### Arch Linux

A split PKGBUILD ships in the repository — `vv` (CLI/TUI) and `vv-gui` (the
Qt6 desktop viewer plus the Dolphin thumbnailer and metadata plugins). It is
**not** in the AUR; build it from the checkout:

```sh
git clone https://github.com/balwierz/vv.git
cd vv/packaging/arch
makepkg -si            # both packages; or -si vv to install just the CLI
```

### Not currently published

`packaging/` also carries Bioconda, Homebrew and RPM recipes, but **no
package exists on those channels yet** — they are prepared, not published.
Use the `.deb`, the static tarball, or a source build.

## Build from source

### Requirements

- CMake ≥ 3.16
- GCC ≥ 10 or Clang ≥ 12 (C++20)
- Apache Arrow + Parquet development libraries
- htslib development library
- ncurses development library

### Linux distros

```sh
# Debian / Ubuntu
sudo apt-get install cmake g++ libarrow-dev libparquet-dev libhts-dev libncursesw5-dev

# Fedora / RHEL / Rocky
sudo dnf install cmake gcc-c++ arrow-devel parquet-devel htslib-devel ncurses-devel

# Arch
sudo pacman -S cmake gcc arrow htslib ncurses
```

### macOS

```sh
brew install cmake apache-arrow htslib ncurses xlsxio expat minizip hdf5
```

Homebrew installs `apache-arrow` under its own prefix, so point CMake at it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix apache-arrow);$(brew --prefix htslib)"
```

macOS is compiled on every commit in CI and sanity-checked, but the test
suite runs only on Linux and no macOS binaries are published — treat it as
supported-but-unverified.

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

A fully static binary (glibc ≥ 2.28) can be built from bundled sources with
no network access inside the container:

```sh
docker build --network=host -f Dockerfile.almalinux8 -t vv_build .
mkdir -p dist && docker run --rm -v "$PWD/dist:/out" vv_build
# binary at ./dist/vv
```

The build pulls every dependency (zlib, zstd, lz4, bz2, xz, libdeflate,
brotli, snappy, utf8proc, abseil, re2, thrift, ncurses, htslib, Arrow 23.0.1)
from `./docker-sources/` tarballs and links them statically. Parquet
compression codecs (snappy, zstd, lz4, brotli, bz2) are enabled in Arrow.
First build ~30 min cold; subsequent app-only rebuilds are fast (the
dependency layers cache).

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
