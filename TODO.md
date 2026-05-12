# TODO — feature backlog

Features users are likely to ask for, grouped by category. The five
items tagged **★ wow** are the next implementation batch — biggest
visible-value-per-line ratio.

## Data exploration

- **★ wow** `--schema` — print column names, Arrow types, nullability,
  then exit. Cheap, asked for constantly on big files where the user
  just wants to know "what's in here?" before writing any code.
- **★ wow** `--describe` — pandas-style per-column summary: count,
  null count, min/max, mean (numeric), distinct count, top-N for
  low-cardinality string columns. Use Parquet column statistics when
  available; otherwise scan loaded chunks. Defer quantiles to v2.
- **★ wow** Column projection by name — `-c Chromosome,Start,Score`
  in addition to `-c 5` (count). Big win on wide LociSSD with
  embeddings / signals where the user wants just the loci columns.
- **★ wow** `--filter "Score > 0.5"` — value-predicate filter, the
  cousin of `-r`. Tiny grammar: `<col> <op> <literal>` chained with
  AND/OR; ops `== != < <= > >=`; literals: int / float / quoted
  string. Reuse the manual filter machinery built for LociSSD ranges.
- `--sample N` — random N rows instead of just the first N. Useful for
  a representative preview on huge sorted files.
- `--unique COL[,COL,...]` — distinct value counts. Cheap on
  dictionary-encoded Parquet columns.

## Range / region (genomics core)

- `--regions-file foo.bed` — batch many windows at once. Equivalent to
  passing a long comma-separated `-r`.
- `--slop N` — pad each window by N bp. `bedtools slop` inline.
- BCF range queries via `bcf_itr_querys` — currently BCF reads, but
  `-r` only works on the text formats via tabix.
- Generic Parquet range queries — sniff a schema with sorted
  `Chromosome/Start/End` columns; `--region-cols Chromosome,Start,End`
  if auto-detection isn't clear.
- LociSSD `lociSSD_interval_index` consumption (spec §6.5) — helps
  when the window is much smaller than a row group.

## Output / pipelines

- **★ wow** JSON / JSON-Lines output — `--json` writes a JSON array;
  `--ndjson` writes one JSON object per row. Common for piping to
  `jq` or feeding data pipelines.
- Markdown output — `--md` for pasting into reports / issues.
- `--stats-only` — Parquet metadata footer dump (row groups, sizes,
  codecs, per-column stats) without reading any data. Lots of demand
  for understanding inherited Parquet files.
- `--validate` — LociSSD invariants check (sort order monotone,
  MaxEndSoFar consistent with End, manifest matches schema and
  per-chromosome row counts).
- Parquet to stdout — `--parquet -` currently requires a file path
  because of footer-at-end; could support stdout with a buffered
  spool to disk. Useful in pipelines.

## TUI

- Live filter (`&` in `less` style) — toggle to hide non-matching
  rows interactively. Skipped earlier; users keep asking.
- Stats popup — `S` over the column under cursor shows the same
  data as `--describe`.
- Sort by column — `s` over a column. Requires a full file scan;
  document the cost.
- Column show/hide picker — `c` opens a checkbox list of columns.
- Multi-file tabs — open many files, switch with `Tab` / `Shift-Tab`.
- Marks & recall — vim-style `m{a-z}` / `'{a-z}`.
- Copy cell — `y` copies the cell under the cursor (OSC52 / xclip).

## Format gaps

- bigBed / bigWig — UCSC indexed binaries. Needs libBigWig as a new
  dependency (Bioconda, Arch, conda-forge ship it; Ubuntu doesn't).
- mpileup — samtools workflow staple; ragged columns make it a
  custom parser.
- 2bit — UCSC sequence storage. Small format, custom parser.
- HDF5 / AnnData / Loom — single-cell omics. Heavy dep (libhdf5).
- Galaxy `.dat` / Galaxy archive — niche but visible.

## Performance / build

- LTO build (originally the only item in this file): rebuild all
  static deps with `-flto -ffunction-sections -fdata-sections` and
  link with `--gc-sections`. Expected 40-60% text-segment savings.
- `--threads N` already exists, but a Parquet-specific decompressor
  thread pool tunable could give another 1.5–2× on cold reads.
- ARM64 static binary in the release workflow.
- `ccache` in the Dockerfile to make rebuilds faster.

## Convenience

- `--tail N` — symmetric to `-n`. Streaming sources require a full
  scan; document.
- `--coords 1-based` — accept tabix-style 1-based coordinates.
- Color themes — `--theme dark|light|solarized` and / or
  `~/.config/vv/theme.toml`.
- Better nested rendering — multi-line wrap for long `list`/`map`
  cells instead of `[first, …]`.

## Quality / signal

- Sanitizer CI job (ASan + UBSan) — bioinformatics files are weird;
  fuzz-friendly.
- libFuzzer harness for the binary parsers (BAM, Parquet, BCF,
  FASTA/FASTQ).
- Code coverage badge.
- Reproducible builds.
- ARM64 static binary in release artifacts.
