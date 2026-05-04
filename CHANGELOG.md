# Changelog

All notable changes to `vv` are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [1.4.0] - 2026-05-04

### Added
- **FASTA / FASTQ** support via htslib `kseq.h` (`.fa`, `.fasta`, `.fna`, `.faa`,
  `.ffn`, `.frn`, `.fq`, `.fastq`, plus `.gz` variants).
- **Tabix-indexed range queries** — `-r` / `--region <REGION>` filters
  `.vcf.gz`, `.bed.gz`, `.gff.gz`, and any tabix-indexed TSV/CSV. Multiple
  comma-separated regions accepted.
- **Threading** — new `-@` / `--threads <N>` flag (samtools convention).
  Auto-detects worker count when unset. Drives Arrow's CPU pool plus
  `hts_set_threads` (BAM/CRAM) and `bgzf_mt` (FASTA/FASTQ).
- **Lazy Arrow IPC loading** — `.arrow` files now load only the footer at
  open time; record batches decoded on demand. Opening a 10 GB file drops
  from minutes to milliseconds.
- **Parquet column-decode threading** + buffered-stream tuning (4 MiB
  window) for cold reads.
- **CSV / TSV block_size = 16 MiB** with `use_threads = true` for parallel
  parsing on multi-GB delimited files.
- `write_delimited` Parquet fast path: `--tsv -n N` now uses
  `read_first()` to fetch only the row groups needed for `N` rows.

### Changed
- **List, fixed-size list, and map rendering** now use Python-style brackets:
  `[v1, v2, ...]`, `(v1, v2, ...)`, `{key: value, ...}` — no more verbose
  `list<element: double>[...]` Arrow type prefix.
- **Smart truncation**: collections that don't fit in a column are rendered
  as `<open>first, …<close>` so the first element is always visible.
- **Float formatting**: `%.6g` (6 significant digits) for compact ~8-char
  output instead of `0.090418812` (11 chars).
- **Adaptive integer column widths** — TUI integer columns size to fit the
  rows currently visible, growing/shrinking on scroll.
- **Smarter initial column widths** — float columns floor at 8 chars, list/
  map columns at 14 chars (room for `[first, …]`), strings at 12 chars
  (11 chars + ellipsis on overflow).
- **Digit grouping in TUI status bar and row index column** — large row
  numbers display with `_` separators (e.g. `1_234_567`).
- TUI column-width keys changed from `<` / `>` to `,` / `.` (no Shift).
- Help-text header now reads `vv -- universal genomic file viewer`.

### Fixed
- **Crash on Parquet files with nested types** (struct/list/map). Parquet's
  `ReadRowGroups` takes leaf column indices, not Arrow field indices —
  the fix walks the schema manifest to expand them.
- **CSV/TSV with `#` comment lines** (e.g. CADD-format TSVs) no longer
  crash the reader.

## [1.3.x]

Patch releases preceding 1.4. See `git log` for details.

## [1.3.0] - 2026-04-30
### Added
- Fast partial read for non-interactive Parquet table view (`-n` previews
  no longer decode whole row groups).

## [1.2.0] - 2026-04-21
### Added
- AlmaLinux 8 Docker build for portable static binary.
- TUI polish: zebra rows, 256-color palette, rounded box glyphs, VCF INFO
  expansion.
- `--version` / `-V` flag.
- Lazy per-column loading in TUI for large Parquet files.
- Prettier, shorter error messages for bad input paths.
