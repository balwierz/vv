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
- ~~`--sample N`~~ — random N rows instead of just the first N.
  *Done — reservoir sampling, honours `--filter`.*
- ~~`--unique COL[,COL,...]`~~ — distinct value counts.
  *Done — top-50 per column with overflow indicator, honours
  `--filter`.*

## Range / region (genomics core)

- ~~`--regions-file foo.bed`~~ — batch many windows at once.
  *Done — first three TSV columns of a BED, comments / track lines
  skipped.*
- ~~`--slop N`~~ — pad each window by N bp. *Done — applied after
  --regions-file; start clamped at 0.*
- ~~BCF range queries via `bcf_itr_querys`~~ — *Done — requires
  `.csi`/`.tbi` index; friendly error pointing at `bcftools index`
  if missing.*
- ~~Generic Parquet range queries~~ — *Done — auto-detects
  Chromosome / Start / End by name (common variants:
  `Chromosome`/`chrom`/`Chr`, `Start`/`POS`/`chromStart`,
  `End`/`Stop`/`chromEnd`); override via
  `--region-cols Chr,Start,End`. Pruning uses Parquet
  ByteArray statistics on chrom plus Int statistics on
  Start/End; per-row filtering inside surviving row groups
  is correct for unsorted files too (just less efficient).
  Handles dict-encoded chrom columns transparently.*
- LociSSD `lociSSD_interval_index` consumption (spec §6.5) — helps
  when the window is much smaller than a row group.

## Output / pipelines

- **★ wow** JSON / JSON-Lines output — `--json` writes a JSON array;
  `--ndjson` writes one JSON object per row. Common for piping to
  `jq` or feeding data pipelines.
- ~~Markdown output~~ — `--md` / `--markdown`. *Done — emits a
  GitHub-flavored table; honours --select / --filter / -n /
  --no-header; pipes inside cells get escaped, newlines become
  `<br>`. Numbers keep `_` grouping for readability.*
- ~~`--stats`~~ — Parquet metadata footer dump (row groups, sizes,
  codecs, per-column compressed/uncompressed sizes, null counts)
  without reading any data. *Done.*
- ~~`--validate`~~ — LociSSD invariants check. *Done — verifies the
  manifest covers all rows contiguously, the required columns
  exist with correct types, rows are sorted by (Start, End) within
  each chromosome, MaxEndSoFar matches the running max(End), and
  each row's Chromosome label agrees with the manifest's window.
  Prints PASS / FAIL per check, caps repeat violations at 5 to
  keep output bounded, exits non-zero if any check failed.*
- ~~Parquet to stdout~~ — `--parquet -`. *Done — spools to a
  `mkstemps`-managed temp file under `/tmp`, then streams the
  finished file to stdout and unlinks. Bit-identical to the
  on-disk variant; suitable for `vv ... --parquet - | ...`
  pipelines.*

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

- ~~bigBed / bigWig~~ — *Done — vendored libBigWig under
  `vendored/libBigWig/`, built as a private static lib with
  `-DNOCURL` (no libcurl dep). bigBed's embedded autoSql definition
  is parsed into typed Arrow columns (signalValue, pValue, qValue,
  …); rare types fall through to `string`. Range queries reuse the
  existing `-r` plumbing via libBigWig's overlap APIs.
  Remaining edge cases: complex nested autoSql structs and
  non-trivial `enum{...}` value validation.*
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

- ~~`--tail N`~~ — *Done — last-N rows; reuses the `MemoryTableSource`
  adapter (full scan, slice from the end), honours `--filter`.*
- ~~`--coords UCSC|NCBI`~~ — *Done — UCSC (0-based half-open, BED-
  style, default) vs NCBI (1-based inclusive, GenBank/VCF/tabix
  style). Conversion happens once in `apply_region_modifiers`;
  downstream sources see normalized UCSC. `--regions-file` entries
  are always BED (UCSC) per the spec.*
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
