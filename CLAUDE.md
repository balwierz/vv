# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary lands at `build/parquet_viewer`. Requires `libarrow-dev`, `libparquet-dev`, `libhts-dev`, `cmake`, and `g++` (C++20).

For a debug build:
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

There are no tests and no linter configured.

## Architecture

The entire program is a single file: `main.cpp`. No headers, no subdirectories.

The code flows top-to-bottom through these sections:

1. **`Colors` / `init_colors()`** — ANSI escape codes stored in a global `g_color` struct. Colors are only populated when output is a TTY (or `--color=always`). All rendering code checks `*g_color.reset` to decide whether to emit color.

2. **`Config` / `parse_args()`** — CLI flags parsed into a `Config` struct. Three output modes: interactive TUI (default on TTY), table view (`-n`, `--no-interactive`), and delimited (`--tsv`/`--csv`/`--delimiter`).

3. **`cell_to_string()`** — converts a single Arrow array cell to a `std::string`. Handles all Arrow primitive types plus `DICTIONARY` (decodes to the value type recursively). Falls back to `GetScalar()->ToString()` for unknown types.

4. **Table drawing** (`Column`, `draw_separator()`, `emit_cell()`, `draw_row()`) — builds `Column` structs with pre-rendered cell strings and computed widths, then draws the ASCII box table in three passes: top border, header row, data rows.

5. **`write_delimited()`** — streams the file chunk-by-chunk using a `ChunkCursor` per column to avoid materializing the full dataset. RFC 4180 quoting via `write_csv_field()`.

6. **Preamble helpers** — `PrependInputStream`, `read_stream_line()`, `strip_bed_preamble()`, `strip_prefix_preamble()`, `strip_vcf_preamble()`, `TruncateFieldsStream` — shared infrastructure for stripping format-specific header lines and handling ragged-column inputs before passing data to the Arrow CSV reader.

7. **`TabularSource`** — abstract interface over all formats. Subclasses: `ParquetSource`, `DelimitedSource` (CSV/TSV/BED/VCF/GFF/SAM), `BamSource`. All expose the same chunk-based read API. `format_cell()` and `min_col_width()` allow per-format display customisation (e.g. coordinate digit-grouping, BED itemRgb colour bars).

8. **`DelimitedSource`** — uses Arrow's streaming CSV reader. `DelimKind` enum selects behaviour: BED autogenerates then renames columns; VCF extracts names from `#CHROM`; GFF and SAM use fixed 9/11 column schemas and wrap the stream in `TruncateFieldsStream` to discard extra fields.

9. **`BamSource`** — reads BAM/SAM via htslib (`sam_read1`), builds Arrow arrays with `StringBuilder`/`Int32Builder`/`Int64Builder`, batched 32 768 records at a time. Decodes CIGAR from packed `uint32_t`, SEQ from 4-bit NT16 encoding, QUAL from raw Phred to Phred+33 ASCII.

10. **`TableTUI`** (ncurses interactive viewer) — a batch cache keyed by chunk index. Column widths are computed from header names only (no data pre-scan). `draw()` calls `visible_cols()` to fit the terminal width, renders header, data rows, and a status bar. Keys: arrows/hjkl scroll rows/cols, PgUp/PgDn, g/G top/bottom, q quit. Active automatically when stdout+stdin are both TTYs and `-n` was not passed; disable with `--no-interactive`.

11. **`main()`** — opens the file via `open_source()`, dispatches to interactive → delimited → table mode, and for table mode appends the schema summary + file metadata footer.

## Key design decisions

- **Shared linking**: Arrow/Parquet prefer `_static` CMake targets but fall back to `_shared` automatically. htslib is always shared (system `.so`). mimalloc is fetched from source and linked statically.
- **Interactive mode auto-detection**: `main()` enables the TUI when `isatty(STDOUT_FILENO) && isatty(STDIN_FILENO) && !cfg.delimiter && !cfg.head_rows_set`. Passing `-n` or a delimiter flag bypasses it.
- **Chunk lazy loading**: all three source types cache decoded chunks; `ensure(i)` reads forward until chunk `i` is available. `BamSource` and `DelimitedSource` are strictly forward-only (no seeking after the first read).
- **GFF/SAM column truncation**: `TruncateFieldsStream` caps lines at N tab-separated fields before the Arrow CSV reader sees them, making the schema uniform despite ragged rows.
- **BAM batch size**: 32 768 records per Arrow batch — large enough to amortise builder overhead, small enough that the TUI can page through a sorted BAM without reading the whole file.
- **Default row limit**: 10 rows in table mode; all rows in delimited mode. The heuristic checks `head_rows_set` to detect "user didn't pass `-n`".
- **Dictionary columns**: decoded transparently — `cell_to_string()` looks up the index in the dictionary array and recurses.
