# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: vv

`vv` — universal genomic file viewer. Single-file C++ program reading Parquet,
Arrow IPC/Feather, BAM/CRAM/SAM, VCF, BED, GFF/GTF, FASTA, FASTQ, TSV/CSV
(plus `.gz` variants and tabix-indexed range queries).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary lands at `build/vv`. Requires `libarrow-dev`, `libparquet-dev`,
`libhts-dev`, `libncurses-dev`, `cmake`, and `g++` with C++20.

For a debug build use `-DCMAKE_BUILD_TYPE=Debug`.

A static portable binary is built via `Dockerfile.almalinux8` (glibc ≥ 2.28).
See `INSTALL.md` for distribution channels.

### GUI build (optional)

```sh
cmake -S . -B build-gui -DCMAKE_BUILD_TYPE=Release -DVV_BUILD_GUI=ON
cmake --build build-gui -j$(nproc)
```

`VV_BUILD_GUI=ON` additionally builds `libvvcore` (the headless reader core),
the Qt6 desktop viewer `gui/vvg`, and — when KF6 is present and the build uses
shared deps — the Dolphin thumbnailer (`gui/kde/vvthumbnail.so`) and
KFileMetaData extractor (`gui/kde/vvextractor.so`). Needs `qt6-base`,
`extra-cmake-modules`, and the `kio` / `kcoreaddons` / `kfilemetadata` KF6
modules. The KF6 plugins are skipped automatically in the static-libs
configuration (a `.so` needs PIC deps; the vendored archives aren't PIC).
`gui/kde/vvkdetest` is a headless harness that validates the plugin payloads
without a display (`QT_QPA_PLATFORM=offscreen`).

## Tests

```sh
python3 tests/data/generate.py   # one-time, regenerates fixtures
tests/run_tests.sh
```

Smoke tests diff `vv` output against checked-in goldens under `tests/golden/`.
A missing golden fails the suite; it is never created from the binary under
test. To accept an intentional output change, delete the affected golden,
re-run, and use the `cp` command the failure prints — after reading the new
output.

## Architecture

The reader core and the CLI/TUI live in one file, `main.cpp`, which flows
top-to-bottom through the sections below. The public reader surface (`Config`,
`TabularSource`, `FilterExpr`, the cell formatters, `open_source`,
`filter_rows`, `compute_col_stats`) is declared in `include/vv/vvcore.hpp` and
defined in `main.cpp`. Compiling `main.cpp` with `-DVV_CORE_LIB` excludes
`main()` and the ncurses TUI, yielding `libvvcore` — the headless core that the
Qt GUI (`gui/`) and the KF6 plugins (`gui/kde/`) link. The CLI build is
unchanged by this split (the guards are inactive without the macro).

The code flows top-to-bottom through these sections:

1. **`Colors` / `init_colors()`** — ANSI escape codes stored in a global
   `g_color` struct. Colors are only populated when output is a TTY (or
   `--color=always`).

2. **`Config` / `parse_args()`** — CLI flags parsed into a `Config` struct.
   Three output modes: interactive TUI (default on TTY), table view
   (`-n`/`--no-interactive`), and delimited (`--tsv`/`--csv`/`--delimiter`).
   Threading via `-@`/`--threads`; tabix range queries via `-r`/`--region`.

3. **Formatting helpers** — `cell_to_string()` (raw Arrow → text),
   `cell_to_display_string()` (adds digit-grouping for integers),
   `digits_with_sep()` (PEP 515 `_` separators), `truncate()` (smart
   truncation that preserves `[first, …]` form for lists/maps/tuples),
   `display_width()` (UTF-8 codepoint count).

4. **Table drawing** (`Column`, `draw_separator()`, `emit_cell()`,
   `draw_row()`) — builds `Column` structs with pre-rendered cell strings and
   computed widths, then draws the ASCII box table.

5. **`write_delimited()`** — streams chunk-by-chunk via `ChunkCursor`. For
   Parquet with `-n N`, dispatches through `read_first()` (single row group
   instead of full scan).

6. **Preamble helpers** — `PrependInputStream`, `LineReader`,
   `strip_bed_preamble()`, `strip_prefix_preamble()`, `strip_vcf_preamble()`,
   `strip_tsv_csv_preamble()`, `TruncateFieldsStream`. Strip format-specific
   header lines before the Arrow CSV reader sees the data.

7. **`TabixInputStream`** — wraps an htslib tabix iterator as an Arrow
   `InputStream` so `DelimitedSource` can be fed only the records overlapping
   `cfg.region`.

8. **`TabularSource`** — abstract interface over all formats. Subclasses:
   `ParquetSource`, `DelimitedSource`, `BamSource`, `IpcSource`,
   `FastxSource`. All expose the same chunk-based read API plus
   `read_first(rows, cols, *out)` for fast `-n N` previews.

9. **`ParquetSource`** — uses `parquet::arrow::FileReader` with
   `pre_buffer=true`, `use_threads=true`, and a 4-MiB buffered stream window.
   `read_first()` selects only the row groups containing the requested rows
   and decodes them through `GetRecordBatchReader`. Critical: the API takes
   Parquet *leaf* column indices, not Arrow field indices, so
   `arrow_to_leaf_indices()` walks the schema manifest.

10. **`DelimitedSource`** — uses Arrow's streaming CSV reader with
    `block_size = 16 MiB` and `use_threads = true`. `DelimKind` enum selects
    behaviour: BED autogenerates then renames; VCF extracts names from
    `#CHROM`; GFF/SAM wrap the stream in `TruncateFieldsStream`.

11. **`BamSource`** — reads BAM/SAM/CRAM via htslib (`sam_read1`), batches
    32 768 records. `hts_set_threads()` is enabled when `cfg.threads > 1`.

12. **`FastxSource`** — reads FASTA/FASTQ via htslib's `kseq.h` over BGZF.
    `bgzf_mt()` enables multi-threaded BGZF decompression.

13. **`IpcSource`** — Arrow IPC reads only the footer at `open()`; record
    batches are decoded on demand via `ensure(i)`. Feather v1 stays eager
    (single-table format).

14. **`TableTUI`** (ncurses) — chunk cache, `fit_integer_widths_to_visible()`
    sizes integer columns to the rows currently on screen, two-pass
    `visible_cols()`. Keys: arrows/hjkl, PgUp/PgDn, g/G, `,`/`.` narrow/widen,
    `/` search, Enter detail pane, q quit.

15. **`main()`** — sizes Arrow's CPU thread pool, opens the file via
    `open_source()`, dispatches by mode, and for table mode appends the
    schema summary + metadata footer.

## Key design decisions

- **Shared linking**: Arrow/Parquet prefer `_static` CMake targets but fall
  back to `_shared` automatically. htslib is shared by default; statically
  linked in the AlmaLinux 8 Docker build.
- **Interactive mode auto-detection**: `main()` enables the TUI when
  `isatty(STDOUT_FILENO) && isatty(STDIN_FILENO) && !cfg.delimiter &&
  !cfg.head_rows_set`.
- **Chunk lazy loading**: every source caches decoded chunks via
  `ensure(i)` which reads forward until chunk `i` is available. Streaming
  sources (BAM, FASTX, Delimited, IPC) are forward-only.
- **Tabix integration**: when `-r` is passed, the data stream is replaced
  with a `TabixInputStream` after preamble stripping; the original file
  still supplies preamble + column names.
- **Default thread pool**: `effective_threads(cfg)` returns
  `min(8, max(2, hardware_concurrency()/2))` when `-@` is not given.
  Drives Arrow's CPU pool, htslib BAM/CRAM threads, and BGZF threads.
- **Smart truncation**: bracket/paren/brace collections render as
  `<open>first, …<close>` rather than slicing mid-element.
- **Adaptive integer widths**: TUI integer columns are fitted to the rows
  currently visible (not to the whole loaded chunk).
- **Dictionary columns**: decoded transparently via index lookup in
  `cell_to_string()`.

## Project layout

```
main.cpp                  reader core + CLI/TUI (one file)
include/vv/vvcore.hpp     public reader surface (shared by CLI + GUI + plugins)
gui/                      Qt6 desktop viewer (vvg): ArrowTableModel + window
gui/kde/                  KF6 thumbnailer + KFileMetaData plugins, MIME/.desktop
CMakeLists.txt            build + install rules (VV_BUILD_GUI opt-in for the GUI)
Dockerfile.almalinux8     static binary build (glibc ≥ 2.28)
docker-sources/           bundled deps for the static build (gitignored)
completions/              bash, fish, zsh tab completion
man/vv.1                  groff man page
tests/data/               small test fixtures (committed)
tests/golden/             expected outputs (committed)
tests/run_tests.sh        smoke test harness
packaging/                bioconda, homebrew, arch (vv + vv-gui), debian (vv + vv-gui), rpm, docker
.github/workflows/        CI + release workflows
```
