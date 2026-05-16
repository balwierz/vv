# TODO — feature backlog

Features users are likely to ask for, grouped by category. Each
section lists **Open** items first; completed work is kept under
**Done** for institutional memory (see CHANGELOG for the
user-facing summary).

## Data exploration

### Done
- `--schema` — print column names, Arrow types, nullability, then exit.
- `--describe` — pandas-style per-column summary: count, null count,
  min/max, mean (numeric), distinct count.
- Column projection by name — `--select Chromosome,Start,Score`.
- `--filter "Score > 0.5"` — value-predicate filter with AND/OR.
- `--sample N` — reservoir sampling, honours `--filter`.
- `--unique COL[,COL,...]` — distinct value counts; top-50 per
  column with overflow indicator.

## Range / region (genomics core)

### Open
- LociSSD `lociSSD_interval_index` consumption (spec §6.5) — helps
  when the window is much smaller than a row group.

### Done
- `--regions-file foo.bed` — batch many windows from a BED's first
  three columns.
- `--slop N` — pad each window by N bp.
- BCF range queries via `bcf_itr_querys` (requires `.csi`/`.tbi`).
- Generic Parquet range queries — auto-detect Chromosome / Start /
  End or override with `--region-cols`. Pruning via Parquet
  statistics; dict-encoded chroms handled.

## Output / pipelines

### Done
- JSON / NDJSON output (`--json` array, `--ndjson` per-row).
- Markdown output (`--md` / `--markdown`).
- `--stats` — Parquet metadata footer dump without reading data.
- `--validate` — LociSSD invariants check.
- Parquet to stdout (`--parquet -`, temp-file spool).

## TUI

### Open
- Marks & recall — vim-style `m{a-z}` / `'{a-z}`.

### Done
- Stats popup (`S`) — count/nulls/min/max/mean/distinct for the
  active column.
- Sort by column (`s` toggles asc/desc; `u` clears).
- Column show/hide picker (`c`).
- Live filter (`&`) — same grammar as `--filter`; hides
  non-matching rows; status bar shows kept/total; composes with
  sort via the same `source_row(display)` indirection.
- Copy cell (`y`, OSC52) — copies the top-left visible cell to
  the system clipboard via an OSC52 escape; works over SSH /
  tmux 3.3+ without external clipboard helpers.
- Multi-file tabs — `vv a.vcf b.bed …`, `Tab`/`Shift+Tab` cycle;
  each tab keeps its own sort/filter/scroll/cache.
- Command line (`:` key) — `:N` jumps to row, `:q` quits,
  `:theme NAME` switches theme. Extensible verb list.

## Format gaps

### Open
- mpileup — samtools workflow staple; ragged columns make it a
  custom parser.
- HDF5 / AnnData / Loom — single-cell omics. Heavy dep (libhdf5).
- Galaxy `.dat` / Galaxy archive — niche but visible.

### Done
- bigBed / bigWig — vendored libBigWig (`-DNOCURL`, no libcurl
  dep); autoSql parsed into typed Arrow columns; range queries
  via libBigWig overlap APIs.
- 2bit — UCSC sequence container; hand-rolled parser reads only
  the per-sequence index (name / length / n_blocks /
  mask_blocks). DNA bases are not decoded — chromosome-scale
  references would blow up RAM; `twoBitToFa` is the right tool
  for that.
- ENCODE peak / signal text formats — `.narrowPeak`, `.broadPeak`,
  `.gappedPeak`, `.bedGraph` / `.bg`, `.tagAlign` (plus `.gz`)
  routed through DelimKind::BED with variant-aware column naming
  (signalValue / pValue / qValue / peak / value / sequence).

## Performance / build

### Done
- ARM64 static binary in the release workflow — the release CI
  matrices over `ubuntu-latest` (x86_64) and `ubuntu-22.04-arm`
  (aarch64); both arches are published with every tag as
  `vv-<ver>-linux-<arch>.tar.gz`.
- LTO build — every static dep + vv built with `-flto=auto` via
  CMake `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`; gcc-ar / gcc-ranlib
  preserve LTO IR in archives. Significant text-segment shrink
  vs the previous non-LTO 15 MB static binary.
- Parquet decompressor thread-pool tunable — `--decode-threads N`
  separately sizes Arrow's CPU thread pool (Parquet column decode
  / CSV parallel parsing) without touching htslib's thread count.
  Defaults to `--threads`; bounded at `2 × hardware_concurrency()`.
- `ccache` in the AlmaLinux 8 Dockerfile — installed via EPEL,
  symlinks added to PATH so every `gcc` / `g++` invocation gets
  wrapped; BuildKit cache mount (`id=vv-ccache`) persists the
  cache across docker builds.

## Convenience

### Open
- Multi-line nested rendering — wrap long `list`/`map` cells across
  multiple screen rows. Punted as "medium-sized": the current TUI
  bakes row-height=1 into chunk caching, search highlighting, sort
  indirection, and scroll math. Inline expansion (this iteration)
  takes the easy win.
- User-defined themes in `~/.config/vv/config` — would let the user
  override individual palette entries (e.g. `border = 38;5;238`)
  rather than picking from the five built-ins. The XDG config
  plumbing is already in; this is just a palette-override loader.
  Deferred until someone asks.

### Done
- Color themes — `--theme NAME` selects from `default`, `dark`,
  `light`, `solarized-dark`, `solarized-light` (built-in palettes
  covering both the ASCII table and the TUI; 256-color with 16-color
  fallback).
- Smarter inline list/map truncation — `truncate()` walks every
  top-level comma and picks the largest leading-element prefix
  that fits the column, instead of always dropping to
  `[first, …]` after the first comma.
- TUI theme picker (`T`) — overlay; saves to
  `$XDG_CONFIG_HOME/vv/config` (default `~/.config/vv/config`).
  Future runs pick up the saved theme automatically.

### Done
- `--tail N` — last-N rows; reuses the `MemoryTableSource` adapter.
- `--coords UCSC|NCBI` — UCSC (0-based half-open) vs NCBI (1-based
  inclusive); conversion happens once in `apply_region_modifiers`.

## Quality / signal

### Open
- Sanitizer CI job (ASan + UBSan).
- libFuzzer harness for the binary parsers (BAM, Parquet, BCF,
  FASTA/FASTQ).
- Code coverage badge.
- Reproducible builds.
