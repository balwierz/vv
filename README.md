# vv — universal genomic file viewer

[![CI](https://github.com/balwierz/vv/actions/workflows/ci.yml/badge.svg)](https://github.com/balwierz/vv/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Bioconda](https://img.shields.io/conda/dn/bioconda/vv.svg?label=Bioconda)](https://anaconda.org/bioconda/vv)
[![Latest release](https://img.shields.io/github/v/release/balwierz/vv)](https://github.com/balwierz/vv/releases)

A fast, self-contained command-line viewer for tabular and bioinformatics
file formats. One binary covers Parquet, Arrow IPC / Feather, LociSSD,
BAM/CRAM/SAM, VCF/BCF, GFF/GTF, BED, FASTA/FASTQ, PAF, UCSC bigBed /
bigWig / 2bit, and plain TSV/CSV — with gzip / bgzip on the fly,
range queries, a full ncurses browser, and an optional Qt6 / KDE desktop
app (`vvg`). On a terminal it opens an interactive viewer; elsewhere it
prints to stdout.

```
$ vv variants.vcf
╭───┬────────┬───────────┬──────┬──────┬──────┬──────┬────────┬──────────────╮
│   │ CHROM  │ POS       │ ID   │ REF  │ ALT  │ QUAL │ FILTER │ INFO         │
├───┼────────┼───────────┼──────┼──────┼──────┼──────┼────────┼──────────────┤
│ 0 │ chr1   │       100 │ rs1  │ A    │ G    │   30 │ PASS   │ AF=0.5       │
│ 1 │ chr1   │       500 │ .    │ C    │ T    │   40 │ PASS   │ AF=0.1       │
│ 2 │ chr1   │     1_500 │ .    │ G    │ A    │   50 │ PASS   │ AF=0.3       │
│ 3 │ chr2   │       200 │ .    │ T    │ C    │   35 │ PASS   │ AF=0.2       │
╰───┴────────┴───────────┴──────┴──────┴──────┴──────┴────────┴──────────────╯

[4 rows x 8 columns]
```

## Quick install

```sh
# Bioconda
conda install -c bioconda vv

# Homebrew (macOS / Linuxbrew)
brew install balwierz/tap/vv

# Static Linux binary (glibc ≥ 2.28). x86_64 and aarch64 published on
# every release; swap `x86_64 → aarch64` for ARM (AWS Graviton, RPi 5, …).
ver=$(curl -fsSL https://api.github.com/repos/balwierz/vv/releases/latest \
        | grep -oP '"tag_name":\s*"\K[^"]+')
curl -L "https://github.com/balwierz/vv/releases/${ver}/download/vv-${ver#v}-linux-x86_64.tar.gz" | tar -xz
sudo install vv-*-linux-x86_64/vv /usr/local/bin/
```

See [INSTALL.md](INSTALL.md) for source builds, AUR, and the static AlmaLinux
8 Docker build.

## Supported formats

| Family            | Extensions                                                 |
|-------------------|------------------------------------------------------------|
| Apache Parquet    | `.parquet`                                                 |
| Arrow IPC, Feather| `.arrow`, `.feather`                                       |
| LociSSD           | `.lociss` (sorted-interval Parquet; `MaxEndSoFar` auto-hidden) |
| Sequence alignments | `.bam`, `.cram`, `.sam`, `.paf` / `.paf.gz` (minimap2)   |
| Variant calls     | `.vcf`, `.vcf.gz`, `.bcf` (binary VCF via htslib)          |
| Genome annotation | `.gff`, `.gff3`, `.gtf` (plus `.gz`)                       |
| Genomic intervals | `.bed`, `.bed.gz`                                          |
| ENCODE peaks / signal | `.narrowPeak`, `.broadPeak`, `.gappedPeak`, `.bedGraph` (`.bg`), `.tagAlign` (plus `.gz`); BED-family with typed extra columns named `signalValue` / `pValue` / `qValue` / `peak` / `value` |
| UCSC big files    | `.bb` / `.bigBed`, `.bw` / `.bigWig` (vendored libBigWig; bigBed's embedded autoSql is parsed into typed columns) |
| UCSC 2bit         | `.2bit` (sequence index: name / length / N-blocks / mask-blocks) |
| SQLite            | `.sqlite`, `.sqlite3`, `.db` (each table → one TUI tab; types follow SQLite affinity) |
| Excel             | `.xlsx`, `.xlsm` (each sheet → one TUI tab; column types inferred from cell text via Arrow's CSV reader) |
| OpenDocument      | `.ods`, `.fods` (flat XML ODS) (each sheet → one TUI tab; hand-rolled minizip + expat SAX parser; types inferred from cell content via Arrow's CSV reader) |
| AnnData / HDF5    | `.h5ad`, `.h5`, `.hdf5`, `.loom` (single-cell + generic). AnnData files surface as a summary tab plus obs / var / X-preview / obsm / varm / layers tabs; sparse X gets a first-N-row dense preview. Generic HDF5 opens with a hierarchy table and one tab per 1D / 2D dataset. |
| NumPy arrays      | `.npz` (archive → a summary tab plus one tab per array), `.npy` (a single array, opened through the same reader). 1-D renders as a column, 2-D as a table, 3-D+ as 2-D slices stepped with `[` / `]`. Fixed numeric dtypes (int / uint / float / bool); object / structured arrays are listed but not displayed. |
| samtools mpileup  | `.pileup`, `.mpileup`, `.pile` (plus `.gz`); per-base pileup with auto-named columns; multi-sample files get per-sample `depth_i` / `bases_i` / `quals_i` triplets; range queries on bgzipped + tabix-indexed files |
| Apache ORC        | `.orc` (columnar; one stripe → one chunk; via Arrow's ORC adapter — requires Arrow built with `-DARROW_ORC=ON`, which apt/brew Arrow packages have by default) |
| Markdown          | `.md`, `.markdown`, `.mdown`, `.mkd` — CommonMark + GFM via vendored md4c. Renders as ANSI on stdout (pipe to `less -R`). GFM tables are extracted and rendered through vv's regular table renderer with column-type inference. Local PNG/JPEG/GIF images inline on kitty / iTerm2 / WezTerm terminals via their graphics protocols. |
| Sequences (FASTA) | `.fa`, `.fasta`, `.fna`, `.faa`, `.ffn`, `.frn` (plus `.gz`) |
| Sequencing reads  | `.fq`, `.fastq` (plus `.gz`)                               |
| Delimited text    | `.tsv`, `.csv` (plus `.gz`)                                |
| Stdin             | `vv -` reads any text format from stdin (auto-gunzip)      |

Unknown extensions are auto-detected by magic bytes (Parquet, Arrow IPC,
Feather, BAM/BCF) or delimiter heuristic (TSV vs. CSV).

## Output modes

`vv` picks its output style automatically: on a terminal it opens the
interactive ncurses browser; with `-n N` or a `--tsv`/`--csv`/`--json`/
`--md`/`--parquet` flag, or when stdout is piped, it prints non-interactively.

### Default — ASCII table

The first 10 rows as a Unicode-box table, followed by the schema and a
metadata footer. The same view appears in any non-terminal context (pipes,
redirects, the `--no-interactive` flag).

```
$ vv -n 6 peaks.parquet
╭───┬──────┬───────┬───────┬───────┬──────────────────╮
│   │ Chr  │ Start │ End   │ Score │ Tags             │
├───┼──────┼───────┼───────┼───────┼──────────────────┤
│ 0 │ chr1 │   100 │   200 │     0 │ [promoter]       │
│ 1 │ chr1 │ 1_100 │ 1_200 │  0.05 │ [enhancer, open] │
│ 2 │ chr1 │ 2_100 │ 2_200 │   0.1 │ []               │
│ 3 │ chr1 │ 3_100 │ 3_200 │  0.15 │ [TF]             │
│ 4 │ chr1 │ 4_100 │ 4_200 │   0.2 │ [promoter, TF]   │
│ 5 │ chr1 │ 5_100 │ 5_200 │  0.25 │ [promoter]       │
╰───┴──────┴───────┴───────┴───────┴──────────────────╯

[20 rows x 5 columns]

Column  Type                   Nullable
------  ---------------------  --------
Chr     string                 yes
Start   int64                  yes
End     int64                  yes
Score   float                  yes
Tags    list<element: string>  yes

File: peaks.parquet
Row groups: 4  |  Compressed: 2.1 KiB
Created by: parquet-cpp-arrow version 24.0.0
```

Integer columns auto-group digits with `_` (PEP 515 style). Floats render
to 6 significant figures. Lists / maps render Python-style and keep as
many leading elements visible as fit the column. On a 256-color terminal
the table picks up zebra striping and column-type coloring; pipes get
plain ASCII automatically.

### `vh` — vertical head (transposed preview)

For wide tables — VCF with hundreds of INFO fields, Parquet from Spark
with deep schemas — the transposed view turns each field into a row and
each record into a column. As many records as fit in the terminal are
shown side-by-side.

```
$ vh -n 3 variants.vcf
╭────────┬────────┬────────┬────────╮
│ field  │ #0     │ #1     │ #2     │
├────────┼────────┼────────┼────────┤
│ CHROM  │   chr1 │   chr1 │   chr1 │
│ POS    │    100 │    500 │  1_500 │
│ ID     │    rs1 │      . │      . │
│ REF    │      A │      C │      G │
│ ALT    │      G │      T │      A │
│ QUAL   │     30 │     40 │     50 │
│ FILTER │   PASS │   PASS │   PASS │
│ INFO   │ AF=0.5 │ AF=0.1 │ AF=0.3 │
╰────────┴────────┴────────┴────────╯

[3 rows x 8 columns]  vertical: 3 record(s) shown
```

`vh` is a symlink to the same binary; `vv --vertical` is equivalent.
The view is non-interactive — for full record exploration use the TUI
below and press `Enter` on a row.

### Interactive TUI

The default when stdout is a terminal. Same Unicode-box table, but
infinite-scroll, with overlays for stats / sort / filter / column
picker. Key bindings (also visible in-app via `H` / `F1`):

| Key            | Action                                                  |
|----------------|---------------------------------------------------------|
| arrows / hjkl  | move the cell cursor one row / column (view follows)    |
| Space / PgDn / b / PgUp | scroll one page                                |
| g / G          | top / bottom of file                                    |
| Enter          | detail pane for the cursor's row (every field, untruncated) |
| `/` / `?`      | search forward / backward (case-insensitive regex)      |
| n / N          | next / previous match (direction-aware)                 |
| `,` / `.`      | narrow / widen the column under the cursor              |
| z              | freeze first column                                     |
| S              | column-stats popup (count / nulls / min / max / mean / distinct) |
| s              | sort by the cursor's column (toggle asc/desc; `u` clears) |
| `&`            | live filter — same grammar as `--filter`                |
| c              | show / hide columns overlay                             |
| y              | copy the cell under the cursor via OSC52                |
| mouse wheel    | scroll rows                                             |
| mouse click    | column header → sort; data cell → put the cursor there  |
| mouse 2-click  | data row → open detail pane (= Enter)                   |
| Shift + drag   | select text for the OS clipboard (terminal-side)        |
| T              | pick a color theme (overlay; saved to `~/.config/vv/config`) |
| `:`            | command line — `:N` jump to row, `:q` quit, `:theme NAME` |
| Tab / Shift+Tab| next / previous file tab (multi-file mode)              |
| `--theme`      | `default` / `dark` / `light` / `solarized-dark` / `solarized-light` |
| q / Esc        | quit (Esc clears search / filter first)                 |

### Graphical mode — `vvg` (Qt6 / KDE)

A desktop viewer for when you want a window instead of a terminal. Same
reader core as the CLI, so it opens every supported format:

```sh
vvg data.parquet            # or any supported file
vvg a.bam b.vcf.gz c.h5ad   # multiple files → one tab each
```

- **Application shell** — menu bar, **File ▸ Open** (multiple files → tabs),
  drag-and-drop, a recent-files list, and error dialogs. The **multi-tab**
  strip also expands multi-sheet / multi-dataset files (xlsx & ods sheets,
  SQLite tables, HDF5 / AnnData components, NumPy arrays).
- **Genomic region bar** — type `chr1:1000-2000` (UCSC or NCBI coordinates,
  optional slop) to re-open the file(s) over a tabix/`.csi`-indexed range; a
  **Pileup** toggle renders BAM/CRAM as mpileup rows. Mirrors the CLI `-r`.
- **Responsive on big files** — filtering, sorting and find run **off the UI
  thread** with a progress bar and a **Cancel** button, so the window never
  freezes while a multi-GB file is scanned.
- **Click a column header to sort** (typed, not lexical); two-line
  *name + type* headers. **Filter bar** using the same grammar as `--filter`
  (`score > 5 and chrom == "chr1"`), and a **regex find** bar with match
  highlighting.
- **View menu** — show/hide columns, go-to-row, and a shortcuts/filter-DSL
  help overlay. **Σ Stats** per column, a **row-detail** dock, **Ctrl+C**
  copy-as-TSV, and **◀/▶ slice** stepping for 3-D NumPy arrays.

On **KDE Plasma**, installing the `vv-gui` package also wires vv into
Dolphin: double-click (or *Open With*) launches `vvg`, the icon view
shows **table-snapshot thumbnails**, and the **Information Panel** shows
row/column counts, schema, codec, and generator. Build it yourself with
`-DVV_BUILD_GUI=ON` — `vvg` itself needs only Qt6; the KF6 `kio` /
`kcoreaddons` / `kfilemetadata` modules are optional and, when present,
add the Dolphin thumbnailer and Information-Panel plugins.

### Color themes & user config

Five built-in themes ship: `default`, `dark`, `light`, `solarized-dark`,
`solarized-light` (`solarized` is a synonym for `solarized-dark`).
Pass via `--theme NAME` on the command line, or press `T` inside the
TUI to open a picker overlay — `j` / `k` move the cursor, `Enter`
applies the choice. Each theme works on both the non-interactive
ASCII table (ANSI escapes) and the ncurses TUI; on terminals with
fewer than 256 colors, each theme falls back to a 16-color twin.

Settings are persisted to `$XDG_CONFIG_HOME/vv/config` (default
`~/.config/vv/config`) in plain INI-style `key = value` format —
the same idiom every other modern Linux app uses (KDE,
gnome-terminal, vlc, …). Today the `theme` and `scrolloff` keys are read, but
the format is forward-compatible: future preferences slot in
without breaking existing files. Edits are atomic (`.tmp` + rename)
and preserve hand-added comments.

```ini
# ~/.config/vv/config
theme = solarized-dark
scrolloff = 3     # rows kept between the cell cursor and the viewport edge
```

Resolution order, highest priority first: `--theme NAME` on the CLI →
`theme = NAME` in the config file → built-in `default`.

### Schema only — `--schema`

Cheap "what's in this file?" view, no data read.

```
$ vv --schema huge.parquet
Column  Type                   Nullable
------  ---------------------  --------
Chr     string                 yes
Start   int64                  yes
End     int64                  yes
Score   float                  yes
Tags    list<element: string>  yes

File: huge.parquet
Row groups: 4  |  Compressed: 2.1 KiB
Created by: parquet-cpp-arrow version 24.0.0
```

Add `--json` for the machine-readable form. `rows` is `null` when the file
hasn't been fully scanned — this mode is meant to be cheap, so it never drains
a streaming source to produce a number (`--count` is there for that):

```
$ vv --schema --json reads.bam | jq '{format, rows, cols: (.columns|length)}'
{ "format": "BAM", "rows": null, "cols": 11 }

$ vv --count --json reads.bam
{"rows": 3}
```

`--list-columns` and `--list-tabs` print one name per line, for shell
completions and pipelines:

```
$ vv --list-columns cells.h5ad
$ vv --list-tabs cells.h5ad
summary
X (preview)
obs
var
obsm[X_umap]
```

### Supported formats — `--formats`

The authoritative table of what vv reads, with capability columns. The shell
completions are checked against it in CI, so the two can't drift:

```
$ vv --formats
Format                    gz       region tabs streaming extensions
Apache Parquet            -        yes    -    random    .parquet
BAM / CRAM alignments     -        yes    -    stream    .bam .cram
SQLite                    -        -      yes  stream    .sqlite .sqlite3 .db
...

$ vv --formats --json | jq -r '.[] | select(.region) | .name'
```

### Per-column statistics — `--describe`

Pandas-style summary across the loaded chunks. Respects `--select` and
`--filter`.

```
$ vv --describe peaks.parquet
Column  Type                   Count  Nulls  Min   Max         Mean   Distinct
------  ---------------------  -----  -----  ----  ----------  -----  --------
Chr     string                    10      0  chr1  chr1               1
Start   int64                     10      0  100   9100        4600
End     int64                     10      0  200   9200        4700
Score   float                     10      0  0     0.45        0.225
Tags    list<element: string>     10      0  [TF]  [promoter]         5
```

### Pipeline-friendly text — `--tsv` / `--csv` / `--json` / `--ndjson` / `--md`

Stream the file (or a `-n N` head, or a `--sample N` reservoir sample,
or a `-r REGION` window) in the requested format. RFC 4180 quoting for
CSV, GitHub-flavored markdown for `--md`, one JSON object per line for
`--ndjson` (pipe-friendly for `jq`):

```sh
$ vv --tsv -n 3 peaks.parquet
Chr     Start   End     Score   Tags
chr1    100     200     0       [promoter]
chr1    1100    1200    0.05    [enhancer, open]
chr1    2100    2200    0.1     []

$ vv --md -n 3 peaks.parquet
| Chr | Start | End | Score | Tags |
| --- | --- | --- | --- | --- |
| chr1 | 100 | 200 | 0 | [promoter] |
| chr1 | 1_100 | 1_200 | 0.05 | [enhancer, open] |
| chr1 | 2_100 | 2_200 | 0.1 | [] |

$ vv --ndjson reads.fastq.gz | jq 'select(.seq | length > 50)' | head -1
{"name": "read_3142", "comment": "", "seq": "ACGT…", "qual": "IIII…"}
```

### Parquet output — `--parquet`

Convert any supported input into a Parquet file (BED → Parquet,
VCF → Parquet, …). Streams chunk-by-chunk; multi-GB conversions don't
need to fit in RAM. `--parquet -` writes to stdout (spooled through a
temp file because Parquet's footer is at the end).

```sh
$ vv --parquet peaks.parquet --compression zstd peaks.bed
[20 rows → peaks.parquet, zstd]

$ vv --filter 'Score > 0.5' --parquet - big.lociss | duckdb -c "..."
```

### Terminal heatmap — `--heatmap`

Render the numeric columns as a colour heatmap right in the terminal
(rows × numeric-columns, globally normalised, viridis palette) — a quick look
at the shape of a matrix without leaving the shell. `--image-mode` picks the
backend: `auto` (kitty graphics if the terminal supports it, else Unicode
half-blocks), `kitty`, `sixel`, `halfblock`, or `ascii`. When stdout is not a
terminal a plain ASCII intensity grid is written instead of raw escape
sequences, so redirection and `| less` stay clean. Non-finite cells (`NaN` /
`Inf`) are treated as gaps.

```sh
$ vv --heatmap counts.parquet              # colour heatmap in the terminal
$ vv --heatmap --image-mode ascii embedding.npy > grid.txt
```

## Features

- **Range queries** — `-r chr1:1000-2000` on indexed `.bam` / `.cram`
  (`.bai` / `.csi` / `.crai`), tabix-indexed
  `.vcf.gz` / `.bed.gz` / `.gff.gz` / `.tsv.gz`, indexed BCF
  (`.csi` / `.tbi`), LociSSD Parquet, plain Parquet with chrom/start/end
  columns (auto-detected, or via `--region-cols`), and bigBed / bigWig.
  A format with no region index warns on stderr and shows the whole file
  instead of silently ignoring `-r`.
  Multiple windows comma-separated; open-ended (`chr1:`, `chr1:78-`)
  supported. `--regions-file foo.bed` for batch queries. `--slop N`
  pads each window. `--coords UCSC` (0-based half-open, default) or
  `--coords NCBI` (1-based inclusive, tabix / VCF / samtools style).
- **Column projection** — `--select Chromosome,Start,Score`, and a small
  pattern language: globs (`--select 'chr*'`), 1-based index ranges
  (`2-4`, `5-`), type classes (`@numeric`, `@string`, `@list`, `@bool`,
  `@temporal`) and exclusions (`--select '*,!*_pct'`). Output follows the
  order given, so `--select End,Start` also reorders. An exact column name
  always wins over pattern interpretation, so `log2-ratio` and `2-4` stay
  addressable by name. Unknown names — and patterns that match nothing —
  are errors, never silent no-ops. Works across every view and export mode.
- **Value filter** — `--filter 'Chromosome == "chr1" AND Score > 0.5'`.
  Grammar: `<col> <op> <value>` joined by `AND` / `OR`. Operators:
  `== != < <= > >=`, regex `~` / `!~`, `contains` / `startswith` /
  `endswith`, `in (a, b, c)` / `not in (…)`, and `is null` / `is not null`
  — the rows `--describe` counts but nothing could previously select.
  The word operators are operators only in operator position, so a column
  genuinely named `in` or `is` stays filterable. The same grammar drives
  the TUI live-filter (`&`) and the Qt filter box.
- **`--sample N`** — reservoir sample uniformly; honours `--filter`.
- **`--unique COL[,COL,...]`** — distinct-value counts per column.
- **`--tail N`** — last N rows instead of head-N.
- **`--validate`** — LociSSD invariants check (sort order,
  `MaxEndSoFar`, manifest consistency). Non-zero exit on failure;
  suitable as a CI gate.
- **Multi-threaded I/O** — `-@ N` (samtools convention) drives Arrow's
  CPU pool, BAM/CRAM htslib threads, and BGZF for FASTA/FASTQ;
  `--decode-threads N` separately sizes Arrow's decoder pool. Defaults
  auto-detect.
- **Full Arrow type support** — integers, floats, booleans, strings,
  timestamps, dates, decimals, binary, lists, structs, maps,
  dictionary-encoded columns (decoded transparently). Lists, fixed-size
  lists, and maps render with Python-style brackets and smart truncation.
- **Format-specific niceties** — VCF `##INFO=<...>` fields expand into
  individual columns; BED `itemRgb` renders as a colored bar; TSV/CSV
  with `##` headers (CADD, dbSNP) handled; CADD-style numeric headers
  detected and auto-numbered.
- **Stdin** — `vv -` reads any text format from stdin (auto-gunzips).
  Binary formats require seekable files and are rejected with a hint.
- **One binary, zero runtime deps** — the static Linux build links
  Arrow + Parquet + htslib + ncurses + the compression stack
  statically. ~14 MB stripped, glibc ≥ 2.28.

## Usage

Run `vv --help` for the full flag reference (or `man vv` once installed).
A worked example-driven manual lives in [docs/USAGE.md](docs/USAGE.md);
build self-contained HTML and PDF with `docs/build_docs.sh` (requires
`pandoc` plus either a TeX install with `texlive-fontsrecommended` or
a headless browser such as `chromium`).

### Common flags

| Flag                       | Purpose                                              |
|----------------------------|------------------------------------------------------|
| `-n <rows>`                | rows to display (default 10; `0` = all)              |
| `--tail <N>`               | last N rows instead of the first N                   |
| `-w <width>`               | max cell width in the table (default 32)             |
| `-c <cols>`                | max columns to show                                  |
| `--select <names>`         | project columns by name (comma-separated)            |
| `--filter <expr>`          | row predicate (`<col> <op> <literal>` ; AND / OR)    |
| `-r`, `--region <REGION>`  | range query (multi-region comma-separated)           |
| `--coords UCSC|NCBI`       | coordinate convention for `-r` (UCSC default)        |
| `-@`, `--threads <N>`      | worker threads (default auto, capped at 8)           |
| `--tsv` / `--csv` / `--json` / `--ndjson` / `--md` | non-interactive output |
| `--parquet OUT`            | convert input to a Parquet file (or `-` for stdout)  |
| `--heatmap`                | render numeric columns as a terminal heatmap (`--image-mode auto/kitty/sixel/halfblock/ascii`) |
| `--tab <name>`             | view a named component tab from the CLI (AnnData `obs`/`var`/`X`, a workbook sheet) — e.g. `vv cells.h5ad --tab obs -n 20` |
| `--schema` / `--describe` / `--stats` / `--unique` / `--sample` | data-exploration modes |
| `--validate`               | LociSSD invariants check; exits non-zero on failure  |
| `--vertical`               | transposed (vh) preview                              |
| `--theme <name>`           | `default`/`dark`/`light`/`solarized-dark`/`solarized-light` |
| `--color=auto/always/never`| color output mode                                    |
| `-V`, `--version`          | print version and exit                               |

### Examples

```sh
# Interactive browse a Parquet file
vv data.parquet

# Stream a 100-row TSV preview from a multi-GB Parquet (uses fast path)
vv --tsv -n 100 huge.parquet

# Region query on a tabix-indexed VCF
vv -r chr1:1000000-1100000 variants.vcf.gz

# Region preview on a LociSSD Parquet file (no tabix needed)
vv -r chr1:78-99 peaks.lociss

# Plain Parquet with chrom/start/end (auto-detected, or via --region-cols)
vv -r chr1:1000-2000 big.parquet

# Multi-region tabix query on a BED file
vv -r 'chr1:100-200,chr2:500-1000' regions.bed.gz

# 1-based inclusive (tabix / samtools / VCF style) coordinates
vv -r chr1:1000-2000 --coords NCBI variants.vcf.gz

# Multi-threaded scan of a 5 GB BAM
vv -@ 4 -n 1000 alignments.bam

# Export FASTQ as a TSV table
vv --tsv reads.fq.gz > reads.tsv

# Pipe TSV through vv from stdin (auto-detects gzip)
zcat huge.tsv.gz | vv -

# Filter, project, and convert in one pass
vv --filter 'Score > 0.5' --select Chromosome,Start,End,Score \
   --parquet hits.parquet peaks.lociss

# CSV with a custom column count
vv -c 5 -n 20 metadata.csv
```

## Citation

If you use `vv` in published work, please cite it via the
[`CITATION.cff`](CITATION.cff) file (GitHub renders it as a "Cite this
repository" button on the sidebar).

## Contributing

Bug reports, feature requests, and PRs are welcome.
See [CONTRIBUTING.md](CONTRIBUTING.md) for build/test details and coding
style. Behaviour is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
For security issues, see [SECURITY.md](SECURITY.md).

## License

`vv` is released under the [MIT license](LICENSE).

It links against Apache Arrow (Apache 2.0), htslib (MIT), ncurses (MIT),
mimalloc (MIT), and several compression libraries (zlib, zstd, lz4, etc.).
The static binary distribution bundles all of these; their licenses are
included in the source distribution under each `docker-sources/` archive.
