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
- Live filter (`&` in `less` style) — toggle to hide non-matching
  rows interactively. Users keep asking.
- Multi-file tabs — open many files, switch with `Tab` / `Shift-Tab`.
- Marks & recall — vim-style `m{a-z}` / `'{a-z}`.
- Copy cell — `y` copies the cell under the cursor (OSC52 / xclip).

### Done
- Stats popup (`S`) — count/nulls/min/max/mean/distinct for the
  active column.
- Sort by column (`s` toggles asc/desc; `u` clears).
- Column show/hide picker (`c`).

## Format gaps

### Open
- mpileup — samtools workflow staple; ragged columns make it a
  custom parser.
- 2bit — UCSC sequence storage. Small format, custom parser.
- HDF5 / AnnData / Loom — single-cell omics. Heavy dep (libhdf5).
- Galaxy `.dat` / Galaxy archive — niche but visible.

### Done
- bigBed / bigWig — vendored libBigWig (`-DNOCURL`, no libcurl
  dep); autoSql parsed into typed Arrow columns; range queries
  via libBigWig overlap APIs.

## Performance / build

### Open
- LTO build — rebuild all static deps with
  `-flto -ffunction-sections -fdata-sections` and link with
  `--gc-sections`. Expected 40–60 % text-segment savings vs the
  current 15 MB static binary.
- Parquet decompressor thread-pool tunable — `--threads N` already
  drives Arrow's CPU pool; a dedicated knob (e.g. `--decode-threads N`)
  could give another 1.5–2× on cold reads on machines where
  decompress contends with I/O.
- ARM64 static binary in the release workflow (CI matrix + release
  artifact). Also satisfies the same item under "Quality / signal".
- `ccache` in the AlmaLinux 8 Dockerfile to make rebuilds faster.

## Convenience

### Open
- Color themes — `--theme dark|light|solarized` and / or
  `~/.config/vv/theme.toml`.
- Better nested rendering — multi-line wrap for long `list`/`map`
  cells instead of `[first, …]`.

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
