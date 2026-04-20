# TODO

## Rebuild everything with LTO

Rebuild all static dependencies (Arrow, absl, thrift, re2, utf8proc, snappy, zstd, lz4,
bz2, lzma, deflate, brotli, zlib, ncurses, htslib) and the main binary with
`-flto -ffunction-sections -fdata-sections`, then link with `--gc-sections`.

This allows the linker to eliminate unused functions *inside* the static libraries
(currently only whole unused object files are dropped). The Arrow compute kernels,
CSV reader internals, and absl utilities we never call are the main targets.
Expected savings: 40–60% of the 18 MB text segment.

Steps:
- Add `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` (CMake's LTO flag) to all
  dependency builds in `/home/piotr/Sources/arrow-build/` and `static-libs/` builds.
- Rebuild Arrow with `-DARROW_ENABLE_TIMING_TESTS=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`.
- Rebuild all `static-libs/` (snappy, zstd, lz4, bz2, lzma, deflate, brotli, zlib,
  re2, utf8proc, thrift, absl, ncurses) with LTO.
- Enable LTO in `CMakeLists.txt` via `set_property(TARGET parquet_viewer PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)`.
- Use lld (`-fuse-ld=lld`) for faster LTO link and `--icf=all`.
