# Installing `vv`

Prebuilt packages cover most users; build-from-source is below.

## Prebuilt binaries

### Bioconda (recommended for bioinformatics environments)

```sh
conda install -c bioconda vv
# or via mamba
mamba install -c bioconda vv
```

A matching Docker / Apptainer image is available at `quay.io/biocontainers/vv`.

### Homebrew (macOS / Linuxbrew)

```sh
brew install balwierz/tap/vv
```

### Static binary (any modern Linux)

```sh
curl -L https://github.com/balwierz/vv/releases/latest/download/vv-linux-x86_64.tar.gz \
  | tar -xz
sudo install vv-*-linux-x86_64/vv /usr/local/bin/
```

The static binary requires glibc ≥ 2.28 (RHEL/CentOS/Rocky/AlmaLinux 8+,
Debian 10+, Ubuntu 18.04+). All other dependencies are linked statically.

### Arch Linux (AUR)

```sh
paru -S vv      # or: yay -S vv
```

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
brew install cmake apache-arrow htslib ncurses
```

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
