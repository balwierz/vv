# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary lands at `build/parquet_viewer`. Requires `libarrow-dev`, `libparquet-dev`, `cmake`, and `g++` (C++20).

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

5. **`write_delimited()`** — streams the file row-group by row-group using a `ChunkCursor` per column to avoid materializing the full dataset. RFC 4180 quoting via `write_csv_field()`.

6. **`ParquetTUI`** (ncurses interactive viewer) — an LRU cache of up to 4 decoded row groups (`CachedRG`). Column widths are computed from header names only (no data pre-scan). `prefetch()` loads the row groups covering the current viewport. `draw()` calls `visible_cols()` to compute which columns fit the terminal, then renders header, data rows, and a status bar. Keys: arrows/hjkl scroll rows/cols, PgUp/PgDn, g/G top/bottom, q quit. Active automatically when stdout+stdin are both TTYs and `-n` was not passed; disable with `--no-interactive`.

7. **`main()`** — opens the Parquet file via Arrow's `FileReaderBuilder`, dispatches to interactive → delimited → table mode, and for table mode appends the schema summary + file metadata footer.

## Key design decisions

- **Shared linking**: The system Arrow/Parquet libraries are `.so` only; CMakeLists.txt prefers `_static` CMake targets but falls back to `_shared` automatically. mimalloc is still fetched from source and linked statically.
- **Interactive mode auto-detection**: `main()` enables the TUI when `isatty(STDOUT_FILENO) && isatty(STDIN_FILENO) && !cfg.delimiter && !cfg.head_rows_set`. Passing `-n` or a delimiter flag bypasses it.
- **Row-group lazy loading**: The TUI's `ParquetTUI::load_rg()` reads one row group at a time and evicts the least-recently-used when the cache exceeds 4 groups. Column widths in TUI mode are derived from header names, not data (no full-file scan needed).
- **Row-group streaming in delimited mode**: `write_delimited()` reads one row group at a time to keep memory bounded on large files. Table mode reads all needed row groups at once (bounded by `-n`).
- **Default row limit**: 10 rows in table mode; all rows in delimited mode. The heuristic checks `head_rows == 10` to detect "user didn't pass `-n`" when switching to delimited mode.
- **Dictionary columns**: decoded transparently — `cell_to_string()` looks up the index in the dictionary array and recurses.
