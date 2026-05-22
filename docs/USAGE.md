---
title: "vv — User manual"
subtitle: "Universal genomic file viewer"
author: "Piotr Balwierz and contributors"
date: "v1.5.0"
papersize: a4
fontsize: 10pt
geometry: margin=2.4cm
mainfont: "DejaVu Sans"
monofont: "DejaVu Sans Mono"
colorlinks: true
linkcolor: blue
urlcolor: blue
toc: true
toc-depth: 2
header-includes:
  - \usepackage{fvextra}
  - \DefineVerbatimEnvironment{Highlighting}{Verbatim}{breaklines,commandchars=\\\{\}}
---

# Introduction

`vv` is a fast command-line viewer for tabular and bioinformatics file
formats. The same binary covers Parquet / Arrow IPC / Feather / LociSSD,
the htslib formats BAM / CRAM / SAM / VCF / BCF, the genomics text
formats GFF3 / GTF / BED / PAF / FASTA / FASTQ, and plain delimited
text (TSV / CSV) — gzip-decompressing on the fly where it makes sense.

This manual covers every flag with a concrete example. The full flag
reference also lives in `vv --help` and `man vv`.

Sample fixtures live under `tests/data/` in the repository; commands
below use them so they're reproducible from a fresh clone.

# Quick start

```sh
# Compile
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run on the smallest fixture
build/vv tests/data/tiny.parquet
```

When stdout is a terminal `vv` opens an ncurses row browser; otherwise
it prints an ASCII table to stdout. Use `--no-interactive` to force
the table view in any context.

A symlinked binary named **`vh`** ("vertical head") flips the default
preview to a transposed, vertical layout — useful for wide tables that
would need horizontal scrolling.

# Supported formats

| Family            | Extensions                                                  |
|-------------------|-------------------------------------------------------------|
| Apache Parquet    | `.parquet`                                                  |
| Arrow IPC, Feather| `.arrow`, `.feather`                                        |
| **LociSSD**       | `.lociss` (auto-detected via the `lociSSD_manifest` footer; `MaxEndSoFar` hidden from views) |
| Sequence alignments | `.bam`, `.cram`, `.sam`, `.paf` / `.paf.gz`               |
| Variant calls     | `.vcf`, `.vcf.gz`, `.bcf` (with `.csi` / `.tbi` for range queries) |
| Genome annotation | `.gff`, `.gff3`, `.gtf` (plus `.gz`)                        |
| Genomic intervals | `.bed`, `.bed.gz`                                           |
| ENCODE peaks / signal | `.narrowPeak`, `.broadPeak`, `.gappedPeak`, `.bedGraph` / `.bg`, `.tagAlign` (plus `.gz`); BED-family with `signalValue` / `pValue` / `qValue` / `peak` / `value` named columns |
| UCSC big files    | `.bb` / `.bigBed`, `.bw` / `.bigWig` (vendored libBigWig — bigBed's autoSql definition is parsed into typed columns) |
| UCSC 2bit         | `.2bit` (sequence index: `name`, `length`, `n_blocks`, `mask_blocks`) |
| SQLite            | `.sqlite`, `.sqlite3`, `.db`; each table becomes a TUI tab; types follow SQLite affinity (TEXT → string, INTEGER → int64, REAL → double, BLOB → binary) |
| Excel             | `.xlsx`, `.xlsm`; each sheet becomes a TUI tab. Column types are inferred from cell text via Arrow's CSV reader (int / float / bool / ISO 8601 date / string). Requires `libxlsxio` (`xlsxio` AUR, `libxlsxio-dev` on Debian/Ubuntu, `brew install xlsxio` on macOS). `.xls` (legacy binary) is not supported. |
| OpenDocument      | `.ods`; each sheet becomes a TUI tab. Hand-rolled parser: `content.xml` is inflated with minizip and SAX-parsed with expat. The cell's typed value attribute (`office:value` / `office:date-value` / `office:boolean-value`) wins over its display text for numeric / date / boolean cells, so locale-formatted thousands separators and currency glyphs don't confuse type inference. Honours `table:number-columns-repeated`. Reuses the WorkbookSource framework introduced for Excel — both go through the same in-memory CSV → Arrow CSV reader path. |
| samtools mpileup  | `.pileup`, `.mpileup`, `.pile` (plus `.gz`). The per-base pileup format that `samtools mpileup` produces — every covered position becomes one row with `chrom`, `pos`, `ref`, then a `depth` / `bases` / `quals` triplet per sample. vv counts tabs on the first row to infer the sample count: single-sample files get unsuffixed names; multi-sample files get `depth_1`, `bases_1`, `quals_1`, `depth_2`, … Bgzipped + tabix-indexed `.mpileup.gz` files support range queries via `-r chrom:start-end` (index with `tabix -s 1 -b 2 -e 2`). Pass `--decode-pileup` to replace the packed `bases` / `quals` columns with typed per-allele counts (`A`, `C`, `G`, `T`, `N`, `del_placeholder`, `ins`, `del`, `fwd`, `rev`, `mean_qual`), so `--filter 'A >= 5 and mean_qual >= 30'` etc. works on the parsed data. |
| BAM/CRAM pileup   | `vv x.bam --pileup` (or `.cram`) walks the alignments through htslib's `bam_plp_auto` engine and emits mpileup-style rows directly — equivalent to `samtools mpileup x.bam` without `-f` / `-B`. The output schema matches the file-based mpileup reader, so `--decode-pileup` composes naturally. Range queries via `-r chrom:start-end` need a BAM index (`.bai` / `.csi`) or CRAM index (`.crai`); positions emitted are trimmed to the requested span just like `samtools mpileup -r`. No reference FASTA support yet (ref is set to `N`, all bases render as their literal letter case-by-strand). |
| Apache ORC        | `.orc`; columnar Hadoop/Hive/Spark format. Each ORC stripe is treated as one chunk for lazy loading. Read via Arrow's ORC adapter — Arrow must be built with `-DARROW_ORC=ON` (the default in the Apache Arrow apt repo, Homebrew `apache-arrow`, and the conda-forge wheel). The AlmaLinux 8 static binary currently ships without ORC support; rebuild from source against a system Arrow with ORC for that platform. |
| Markdown          | `.md`, `.markdown`, `.mdown`, `.mkd` — CommonMark + GFM (tables, strikethrough, task lists, autolinks) via the vendored [md4c](https://github.com/mity/md4c) parser. Renders the prose body as ANSI on stdout — headings get `g_color.header` styling, bold/italic/strike map to SGR codes, links show as `text (url)` (or via OSC 8 hyperlinks on terminals that advertise them), block quotes get a `▌` lead glyph, lists get `•` / `1.` markers. GFM `\|` tables are pulled out of the prose stream and rendered separately through vv's normal table renderer, complete with column-type inference (so a benchmark column of `121.7` / `1240.3` becomes `double`). Local PNG/JPEG/GIF images render inline on kitty / iTerm2 / WezTerm terminals via their respective graphics protocols; remote URLs, SVGs, and other terminals fall back to a `🖼 [alt-text]` stub. Pipe to `less -R` to scroll. |
| Sequences (FASTA) | `.fa`, `.fasta`, `.fna`, `.faa`, `.ffn`, `.frn` (plus `.gz`)|
| Sequencing reads  | `.fq`, `.fastq` (plus `.gz`)                                |
| Delimited text    | `.tsv`, `.csv` (plus `.gz`)                                 |
| Stdin             | `vv -` reads any text format from stdin (auto-gunzip)       |

Unknown extensions are auto-detected by magic bytes (Parquet, Arrow IPC,
Feather, BAM/BCF/CRAM) or delimiter heuristic (TSV vs. CSV).

# Output modes

Pick one. Default is **interactive TUI** when stdout is a terminal and
`-n` was not given; **ASCII table** otherwise; or a delimited / JSON /
Parquet writer when its flag is set.

## ASCII table (`--no-interactive`)

```sh
$ vv --no-interactive tests/data/tiny.parquet
╭───┬──────┬───────┬───────┬───────┬──────────────────╮
│   │ Chr  │ Start │ End   │ Score │ Tags             │
├───┼──────┼───────┼───────┼───────┼──────────────────┤
│ 0 │ chr1 │   100 │   200 │     0 │ [promoter]       │
│ 1 │ chr1 │ 1_100 │ 1_200 │  0.05 │ [enhancer, open] │
...
```

Integer columns get `_` digit grouping (PEP 515). Numbers right-align;
lists / fixed-size lists / maps render Python-style as
`[v1, v2, …]`, `(v1, v2, …)`, `{key: value, …}`. The first element of
a collection is always preserved when truncation kicks in
(`[0.0904, …]`, not `[0.0…`).

## Vertical-head (`vh`, or `--vertical`)

```sh
$ vh tests/data/tiny.lociss
╭─────────────┬─────────────────────────────────┬───
│ field       │ #0                              │ #1
├─────────────┼─────────────────────────────────┼───
│ Chromosome  │                            chr1 │  c
│ Start       │                             100 │  5
│ End         │                             200 │  8
│ Name        │                          peak_0 │  p
│ Score       │                             0.5 │  0
╰─────────────┴─────────────────────────────────┴───
```

Each input field is a row; each record becomes a column. Useful for
files with many columns. Hidden-by-default columns (e.g. LociSSD's
`MaxEndSoFar`) are still hidden in this view.

## Interactive TUI

```sh
$ vv tests/data/tiny.lociss        # default when stdout is a terminal
```

| Key            | Action                                                  |
|----------------|---------------------------------------------------------|
| `h j k l` / ←↓↑→ | scroll one column / row                              |
| `Space` / `PgDn` / `b` / `PgUp` | scroll one page                       |
| `g` / `G`      | top / bottom of file                                    |
| `Enter`        | open detail pane for the top-visible row                |
| `/` / `?`      | search forward / backward (case-insensitive ECMAScript regex with literal fallback) |
| `n` / `N`      | next / previous match (direction-aware)                 |
| `,` / `.`      | narrow / widen the leftmost visible column              |
| `z`            | toggle frozen first column                              |
| `S`            | per-column stats popup (count, nulls, min, max, mean, distinct) |
| `s`            | sort by the leftmost visible column (toggle asc / desc) |
| `u`            | undo / clear the active sort                            |
| `&`            | live filter: hide non-matching rows (empty input clears)|
| `c`            | open the show/hide columns picker                       |
| `H` / `F1`     | help overlay with every keybinding                      |
| mouse wheel    | scroll three rows                                       |
| mouse click    | column header → sort by column; data row → scroll to top|
| mouse 2-click  | data row → open detail pane (same as `Enter`)           |
| Shift + drag   | select text for the OS clipboard (terminal-side)        |
| `:`            | command line — `:N` (jump to row), `:q` (quit), `:theme NAME` |
| Tab / Shift+Tab| next / previous file tab (when multiple files are opened) |
| `q`            | quit (Esc clears search / filter / closes overlays)     |

All visible matches are highlighted; the n/N target gets reverse video.

**`S` (stats popup).** Computes count, null count, min, max, mean (numeric
columns), and distinct-value count (strings, capped at 16) for the active
column. Triggers a full-file scan with a progress line in the status bar.
Any key dismisses the overlay. The active column is the leftmost visible
column — the same column that `,` and `.` resize.

**`s` (sort).** Builds an in-memory permutation of the source rows ordered
by the active column. Numeric types sort by raw value, others by string
comparison; nulls go last regardless of direction. Pressing `s` again on
the same column toggles ascending / descending; pressing `s` on a
different column re-sorts ascending. `u` clears the sort. Search (`/`)
continues to work — it follows the sorted display order.

**`c` (column picker).** Opens an overlay listing every column with a
checkbox; `j` / `k` move the cursor, `Space` / `Enter` toggle the column,
`Esc` / `q` / `c` closes the overlay. Hidden columns are dropped from the
table layout; the status bar shows a `hidden:N` indicator. At least one
column always stays visible.

**`&` (live filter).** Opens a `&<expression>` input bar at the bottom
of the screen. The grammar is the same as the `--filter` CLI flag —
`<col> <op> <literal>` joined with `AND` / `OR`, ops
`== != < <= > >=`, string literals quoted with `'` or `"`. Examples:

```
& Score > 0.5
& Chromosome == "chr1" AND Start > 1000000
```

Hit `Enter` to commit. Rows not matching the predicate are hidden
from the display — the status bar shows `filter:<expr>  N/M` (rows
kept / total). Committing an empty input clears the filter; `Esc` in
normal mode also clears an active filter (before quitting).

Live filter composes with sort: build a filter, then press `s` on a
column to sort the visible rows. The combined view is rebuilt in
one pass through the file. Search (`/`) follows the filtered view.

## Delimited output (`--tsv`, `--csv`, `--delimiter`)

```sh
$ vv --tsv --no-header tests/data/tiny.bed.gz
chr1	100	200	peak_0	0.0
chr1	1100	1200	peak_1	0.05
...
```

* `--tsv` and `--csv` use RFC 4180 quoting.
* `--delimiter <char>` lets you pick any single character.
* `--no-header` omits the header row.
* In delimited mode `-n` defaults to all rows, `-c` still applies.
* For Parquet input, `--tsv -n 100` uses a fast-path single-row-group
  decode — multi-GB files preview in milliseconds.

## JSON / NDJSON (`--json`, `--ndjson`)

```sh
$ vv --ndjson --select Chromosome,Start tests/data/tiny.lociss
{"Chromosome": "chr1", "Start": 100}
{"Chromosome": "chr1", "Start": 500}
{"Chromosome": "chr1", "Start": 1000}
{"Chromosome": "chr2", "Start": 200}
{"Chromosome": "chr2", "Start": 1500}
```

* `--json` writes one big JSON array of row objects.
* `--ndjson` writes one JSON object per line (JSON Lines), the
  pipe-friendly form. Combine with `jq`:

```sh
vv --ndjson reads.fastq.gz | jq 'select(.seq | length > 50)'
```

* Scalar Arrow types emit as proper JSON; nested types (list / struct /
  map) emit as their Python-style string inside a JSON string.

## Markdown output (`--md`, `--markdown`)

```sh
$ vv --md -n 3 tests/data/tiny.lociss
| Chromosome | Start | End | Name | Score | MaxEndSoFar |
| --- | --- | --- | --- | --- | --- |
| chr1 | 100 | 200 | peak_0 | 0.5 | 200 |
| chr1 | 500 | 800 | peak_1 | 0.7 | 800 |
| chr1 | 1_000 | 1_200 | peak_2 | 0.2 | 1_200 |
```

* GitHub-flavored table for pasting into issues, READMEs, and
  reports. Renders natively on GitHub / GitLab / most static-site
  generators.
* Pipes inside cells are backslash-escaped (`\|`); newlines become
  `<br>`.
* Honours `--select`, `--filter`, `-n`, `--no-header`, `-r`,
  `--sample`. Numbers keep their `_` thousands separators for
  human readability — switch to `--tsv` for machine-consumable
  output without grouping.

## Parquet output (`--parquet`, `--compression`)

```sh
$ vv --parquet peaks.parquet --compression zstd tests/data/tiny.bed
[20 rows → peaks.parquet, zstd]
```

* Converts any supported input into a Parquet file.
* Streams chunk-by-chunk; multi-GB conversions don't need to fit in RAM.
* `--compression {zstd, snappy, gzip, lz4, none}`, default `zstd`.
* Honours `--select` (column subset) and `--filter` (row predicate).
* `--parquet -` writes to stdout. Parquet's footer-at-end requires
  seekable writes, so the data is spooled to a temp file under
  `/tmp` first and then streamed out — the result is
  bit-identical to writing to a file path:
  ```sh
  vv --parquet - --filter 'Score > 0.5' big.lociss | duckdb -c "..."
  ```

# Column projection (`--select`)

Pick specific columns by name. Comma-separated:

```sh
$ vv --no-interactive --select Chromosome,Start,Score tests/data/tiny.lociss
╭───┬────────────┬───────┬───────╮
│   │ Chromosome │ Start │ Score │
├───┼────────────┼───────┼───────┤
│ 0 │ chr1       │   100 │   0.5 │
...
```

* Unknown names produce a clear error.
* In display modes (table, vh, TUI) the format's hidden columns
  (e.g. LociSSD's `MaxEndSoFar`) stay hidden.
* In export modes (`--tsv` / `--csv` / `--json` / `--parquet`) the
  user's explicit list is honoured exactly — conversions round-trip
  the user's choice.
* Numeric count form `-c 5` (first 5 columns) still works.

# Row filtering (`--filter`)

Value-predicate filter. Grammar:

```
<column> <op> <literal>  joined by AND / OR
```

* Operators: `==`, `!=`, `<`, `<=`, `>`, `>=`.
* Literals: integer, float, single- or double-quoted string.
* `AND` / `OR` case-insensitive.

```sh
$ vv --tsv --no-header --filter 'Score > 0.4' tests/data/tiny.lociss
chr1	100	200	peak_0	0.5	200
chr1	500	800	peak_1	0.7	800
chr2	200	400	peak_3	0.9	400

$ vv --tsv --no-header \
      --filter 'Chromosome == "chr1" AND Score > 0.4' \
      tests/data/tiny.lociss
chr1	100	200	peak_0	0.5	200
chr1	500	800	peak_1	0.7	800
```

Filter is evaluated per row in C++ after the source's own pruning
(tabix iterator / LociSSD row-group stats / BCF iterator), so
combining with `-r` is fine and fast.

# Range queries (`-r`, `--regions-file`, `--slop`)

```sh
$ vv -r chr1:78-99 file.lociss
```

Supported sources:

* **tabix-indexed text**: `.vcf.gz`, `.bed.gz`, `.gff.gz`, `.tsv.gz`
  (requires a `.tbi`);
* **BCF**: requires `.csi` / `.tbi` from `bcftools index`;
* **LociSSD Parquet** (`.lociss`): pruning uses the embedded manifest
  to locate each chromosome's row range, then Parquet's column
  statistics on `Start` and `MaxEndSoFar` to skip row groups outside
  the window, then a per-row predicate inside surviving row groups.
* **Plain Parquet** with chrom/start/end columns: auto-detected by
  name (see `--region-cols` below). Pruning uses Parquet's
  ByteArray statistics on chrom and Int statistics on Start/End;
  per-row filtering inside surviving row groups handles unsorted
  files correctly too. No `MaxEndSoFar` is needed.
* **bigBed / bigWig** (`.bb`, `.bigBed`, `.bw`, `.bigWig`): native
  block-level overlap queries via the vendored libBigWig — no
  external index needed.

Plain `.parquet` with chrom/start/end columns works without a LociSSD
manifest; see `--region-cols` below if the auto-detection misses
your column names.

Coordinate convention: **UCSC** by default — 0-based, half-open intervals
(the convention introduced by Jim Kent's UCSC Genome Browser source tree
in 2000 and used by BED, bigBed, bigWig, BAM, and LociSSD). For
samtools / tabix / VCF / GFF style 1-based inclusive coordinates, pass
`--coords NCBI`. See [Coordinate convention](#coordinate-convention-coords).

## Column names (`--region-cols`)

For plain Parquet, `vv` auto-detects the chrom / start / end columns
from the following priority list:

| Role  | Names tried (first match wins)                              |
|-------|-------------------------------------------------------------|
| chrom | `Chromosome`, `chromosome`, `Chrom`, `chrom`, `Chr`, `chr`, `CHROM`, `#CHROM`, `seqname`, `seqid`, `contig` |
| start | `Start`, `start`, `chromStart`, `POS`, `pos`, `Position`, `position`, `txStart`, `begin`, `Begin` |
| end   | `End`, `end`, `chromEnd`, `Stop`, `stop`, `chromStop`, `txEnd` |

When auto-detection picks the wrong column (or the schema uses
unusual names), override with:

```sh
$ vv -r chr1:1000-2000 --region-cols MyChr,MyStart,MyEnd file.parquet
```

Three comma-separated names, in that order. An unknown name produces
a clear error.

LociSSD files use their canonical names (`Chromosome`/`Start`/`End`)
regardless of override.

## Region syntax

| Form              | Meaning                                             |
|-------------------|-----------------------------------------------------|
| `chr1:100-200`    | range                                                |
| `chr1:`           | whole chromosome                                     |
| `chr1:78-`        | from 78 to end                                       |
| `chr1:-99`        | start to 99                                          |
| `chr1:100`        | single point (position 100)                          |
| `chr1:100-200,chr2:0-1000` | multiple windows                            |

## Many windows (`--regions-file`)

```sh
$ cat windows.bed
chr1	100	900
chr2	1400	1900

$ vv --tsv --no-header --regions-file windows.bed tests/data/tiny.lociss
chr1	100	200	peak_0	0.5	200
chr1	500	800	peak_1	0.7	800
chr2	1500	1800	peak_4	0.1	1800
```

Reads the first three TSV columns of a BED. Comment / `track` /
`browser` lines are skipped. Combines with `-r`; both lists are taken.

## Pad each window (`--slop`)

```sh
$ vv --tsv --no-header -r chr1:1000-1100 --slop 500 tests/data/tiny.lociss
chr1	500	800	peak_1	0.7	800
chr1	1000	1200	peak_2	0.2	1200
```

`bedtools slop` inline: every window grows by N bp on each side.
`start` is clamped at 0; open bounds stay open. Applied after
`--regions-file`.

## Coordinate convention (`--coords`)

Two conventions coexist in bioinformatics file formats:

* **UCSC** — *0-based, half-open* intervals `[start, end)`. Introduced
  by Jim Kent's UCSC Genome Browser source tree in 2000 (Kent et al.,
  *Genome Research* 12:996, 2002). Used by BED, bigBed, bigWig,
  BAM, and LociSSD Parquet. Arithmetic-friendly: `length = end - start`,
  with no off-by-one.
* **NCBI** — *1-based, fully-closed* intervals `[start, end]`. The
  older convention, inherited from how positions were written by hand
  in pre-database biology and crystallised in the GenBank Feature
  Table (1982) and EMBL-Bank (1980). Used by VCF, GFF/GTF, tabix
  region strings, and the samtools / bcftools command line.

`vv -r` defaults to **UCSC** (matching BED). Switch to NCBI with
`--coords NCBI`:

```sh
$ vv -r chr1:100-200          file.parquet              # UCSC default
$ vv -r chr1:101-200 --coords NCBI file.parquet         # same window
$ vv -r chr1:101-200 --coords GenBank file.parquet      # alias
$ vv -r chr1:101-200 --coords tabix   file.parquet      # alias
```

All four commands above match the same rows: NCBI `[101, 200]`
inclusive == UCSC `[100, 200)`. Aliases accepted:

| Convention | Aliases                              |
|------------|--------------------------------------|
| UCSC       | `UCSC`, `Kent`, `0-based`, `bed`     |
| NCBI       | `NCBI`, `GenBank`, `1-based`, `tabix`|

`Kent` is accepted as an alias for `UCSC` in honour of Jim Kent, who
introduced the 0-based half-open convention with the UCSC Genome
Browser source tree in 2000.

`--regions-file` entries are always parsed as BED (UCSC) regardless
of `--coords`, per the BED spec.

# Data exploration

## `--schema`

```sh
$ vv --schema tests/data/tiny.lociss

Column       Type    Nullable
-----------  ------  --------
Chromosome   string  yes
Start        int32   yes
End          int32   yes
Name         string  yes
Score        double  yes
MaxEndSoFar  int32   yes

File: tests/data/tiny.lociss
Format: LociSSD  |  Row groups: 3  |  Compressed: 1.7 KiB
Created by: parquet-cpp-arrow version 24.0.0
```

Prints column names, Arrow types, nullability, and the file-info
footer; reads no data.

## `--describe`

```sh
$ vv --describe tests/data/tiny.lociss
Column      Type    Count  Nulls  Min     Max     Mean  Distinct
----------  ------  -----  -----  ------  ------  ----  --------
Chromosome  string      5      0  chr1    chr2          2       
Start       int32       5      0  100     1500    660           
End         int32       5      0  200     1800    880           
Name        string      5      0  peak_0  peak_4        5       
Score       double      5      0  0.1     0.9     0.48          
```

Pandas-style per-column summary. Numeric columns get min / max / mean;
string columns get distinct count (capped at 16). Respects
`--select` and `--filter`.

## `--stats` (Parquet-only)

```sh
$ vv --stats tests/data/tiny.lociss
File:          tests/data/tiny.lociss
Format:        Parquet
Rows:          5
Row groups:    3
Compressed:    1.7 KiB
Uncompressed:  1.4 KiB  (ratio: 0.82x)
Created by:    parquet-cpp-arrow version 24.0.0

Column       Type    Codec  Compressed  Uncompressed   Ratio  Nulls
-----------  ------  -----  ----------  ------------  ------  -----
Chromosome   string  zstd        260 B         206 B  0.792x      0
Start        int32   zstd        284 B         230 B  0.809x      0
End          int32   zstd        284 B         230 B  0.809x      0
Name         string  zstd        290 B         236 B  0.813x      0
Score        double  zstd        352 B         298 B  0.846x      0
MaxEndSoFar  int32   zstd        284 B         230 B  0.809x      0
```

Reads no data — just the Parquet footer. Use it on inherited
multi-GB files to find out how they were written.

## `--unique`

```sh
$ vv --unique Chromosome tests/data/tiny.lociss
Chromosome — 2 distinct value(s) (of 5)
  chr1       3
  chr2       2
```

Distinct value counts per column, sorted descending. Top-50 values
per column; further values summarised. Multiple columns
comma-separated. Honours `--filter`.

## `--sample N`

```sh
$ vv --tsv --no-header --sample 2 tests/data/tiny.lociss
chr2	200	400	peak_3	0.9	400
chr1	1000	1200	peak_2	0.2	1200
```

Reservoir sampling of N rows uniformly without replacement. Reads the
whole source (applying `--filter` if set), then samples from the
filtered total. Combines with every view / export mode.

## `--tail N`

```sh
$ vv --tail 3 --tsv --no-header tests/data/tiny.parquet
chr2	5100	5200	0.85	[]
chr2	6100	6200	0.9	[TF]
chr2	7100	7200	0.95	[promoter, TF]
```

The last N rows instead of the first N. Reads the source through any
active `--filter`, then slices the tail. Combines with every view /
export mode. For streaming sources (BAM, BCF, FASTX, …) this implies
a full scan.

# Stdin

```sh
$ cat tests/data/tiny.tsv | vv -                 # interactive on TTY
$ zcat huge.tsv.gz | vv --tsv --no-header -      # plain text pipeline
```

* Bare `-` reads stdin.
* Text formats only — Parquet / Arrow IPC / BAM / BCF need seekable
  files, so `vv -` rejects them with a hint pointing at process
  substitution (`vv <(zcat foo.bam)`).
* Auto-detects gzip via magic bytes.

# Color themes (`--theme`)

```sh
$ vv --theme solarized-dark   peaks.lociss     # interactive
$ vv --theme light --color=always huge.parquet | less -R
```

| Theme              | Use case                                         |
|--------------------|--------------------------------------------------|
| `default`          | the original vv palette; works on most terminals |
| `dark`             | punchier accents; assumes a near-black bg        |
| `light`            | mid-saturation accents for light terminals; disables zebra stripes so the table reads cleanly on white |
| `solarized-dark`   | Solarized palette on a dark background           |
| `solarized-light`  | Solarized palette on a light background          |

`--theme solarized` is accepted as a synonym for `solarized-dark`.
The flag affects both the non-interactive ASCII table (via ANSI
escapes) and the ncurses TUI. On terminals with fewer than 256
colors, each theme falls back to a 16-color twin. An unknown theme
name produces a clear error listing the available choices.

## Picking a theme interactively (`T` in the TUI)

Inside the interactive viewer, press `T` to open a theme-picker
overlay. `j` / `k` (or arrows) move the cursor; `[*]` marks the
currently-active theme; `Enter` applies the choice. The new colors
take effect on the next redraw and the choice is persisted to
`~/.config/vv/config` so future runs start with the same theme.
`Esc` / `q` / `T` close the overlay without changing anything.

## Multi-file tabs

`vv` accepts multiple positional arguments; each one becomes a tab in
the interactive viewer. Switch tabs with `Tab` (next) / `Shift+Tab`
(previous). Each tab keeps its own scroll position, sort, filter,
column-hide selection, search anchor, and chunk cache — switching
back is instant.

```sh
$ vv variants.vcf samples.tsv peaks.bed
```

The status bar shows `tab 1/3: variants.vcf` so you always know
which file is active.

Non-interactive output modes (`--tsv`, `--csv`, `--md`, `--json`,
`--parquet`, `--schema`, `--describe`, …) process only the **first**
positional argument. Multi-file is a TUI-only feature; for batch
conversion use a shell loop.

## User config file (`~/.config/vv/config`)

`vv` reads its config from `$XDG_CONFIG_HOME/vv/config` (default
`~/.config/vv/config`) at startup. Format is plain INI-style
`key = value`; lines starting with `#` are comments. Today the
only recognised key is `theme`; the format is extensible, so future
preferences slot in without breaking existing files.

```ini
# ~/.config/vv/config
theme = solarized-dark
```

Resolution order, highest priority first:

1. `--theme NAME` on the command line.
2. `theme = NAME` in the config file.
3. Built-in `default`.

The TUI theme picker (`T`) writes to this file. Edits are atomic
(`.tmp` + rename) and preserve any existing comments or unrelated
keys you've added by hand.

# Performance (`-@` / `--threads`)

```sh
$ vv -@ 4 -n 1000 alignments.bam            # multi-threaded BAM decode
```

| Source         | Threading                                            |
|----------------|------------------------------------------------------|
| Parquet        | parallel column decode (`use_threads = true`); 4 MiB buffered stream |
| CSV / TSV / VCF / GFF / SAM / BED | Arrow CSV `use_threads = true` + `block_size = 16 MiB` |
| BAM / CRAM     | `hts_set_threads(N)`                                 |
| FASTA / FASTQ  | `bgzf_mt(fp, N, 256)`                                |
| Arrow IPC      | lazy: footer only at open; batches decoded on demand |

`--threads 0` (default) auto-picks `min(8, max(2, cores/2))`.

## `--decode-threads N`

Separate knob for Arrow's CPU thread pool, which handles Parquet column
decode and CSV / TSV parallel parsing. Defaults to `--threads`; raise it
when cold Parquet reads bottleneck on decompression and the rest of the
system has idle cores:

```sh
$ vv -@ 4 --decode-threads 16 -n 100 huge.parquet   # 4 I/O, 16 decode
```

Bounded at twice `hardware_concurrency()` so a typo doesn't wreck the
machine. Doesn't affect htslib's thread count (BAM/CRAM/BCF/FASTQ stay
on `--threads`).

# LociSSD specifics

LociSSD is a Parquet variant for sorted genomic intervals (see
[`FORMAT_SPEC.md`](https://github.com/balwierz/LociSSD/blob/main/FORMAT_SPEC.md)).
`vv` detects it via the `lociSSD_manifest` key in the Parquet footer:

* Footer reads "Format: LociSSD".
* The derived `MaxEndSoFar` column is hidden from human-facing views
  (table, vh, TUI) but kept in `--tsv` / `--csv` / `--json` /
  `--parquet` output so the data round-trips losslessly.
* `-r chr1:78-99 file.lociss` uses the manifest + Parquet row-group
  statistics for pruning (spec §7). No external index needed.

## `--validate`

Verify a LociSSD file is internally consistent — useful when integrating
a new writer or debugging a broken pipeline:

```sh
$ vv --validate peaks.lociss
Validating LociSSD invariants for peaks.lociss
  PASS  manifest present and parses (24 chromosomes)
  PASS  Chromosome column is string-like
  PASS  Start column is integer
  PASS  End column is integer
  PASS  MaxEndSoFar column is integer
  PASS  manifest covers all 1_245_318 rows contiguously
  PASS  rows are sorted by (Start, End) within each chromosome
  PASS  MaxEndSoFar matches running max(End) within each chromosome
  PASS  Chromosome labels match the manifest at every row

9 check(s) passed, 0 failed
```

Checks performed:

1. The Parquet file's `lociSSD_manifest` KV footer key exists and parses.
2. Required columns (`Chromosome`, `Start`, `End`, `MaxEndSoFar`) exist
   with usable types (string / dict-of-string, integer).
3. Manifest per-chromosome `row_offset` values are contiguous from 0 and
   `sum(rows)` equals the Parquet `num_rows`.
4. Within each chromosome, rows are sorted lexicographically by
   `(Start, End)`.
5. `MaxEndSoFar[i] == max(End[chrom_first..i])` (the spec's running
   max-end invariant; resets at each chromosome boundary).
6. The `Chromosome` label at each row matches the manifest's window
   for that row index.

Up to 5 violations per category are printed; any more are summarised
on the next line. Exit code is non-zero if any check failed, so
`--validate` is suitable as a CI gate after a writer change.

# bigBed / bigWig specifics

bigBed (`.bb`, `.bigBed`) and bigWig (`.bw`, `.bigWig`) are read via a
vendored copy of [libBigWig](https://github.com/dpryan79/libBigWig)
compiled into the binary — no external library dependency.

```sh
$ vv --schema tests/data/tiny.bb
+-----+-------------+--------+----------+
|  #  | name        | type   | nullable |
+-----+-------------+--------+----------+
|  0  | chrom       | string | no       |
|  1  | start       | uint32 | no       |
|  2  | end         | uint32 | no       |
|  3  | name        | string | yes      |
|  4  | score       | uint32 | yes      |
|  5  | strand      | string | yes      |
|  6  | signalValue | float  | yes      |
|  7  | pValue      | float  | yes      |
|  8  | qValue      | float  | yes      |
+-----+-------------+--------+----------+
```

For bigBed, the embedded **autoSql** definition is parsed into typed
Arrow columns. Common types map as follows:

| autoSql                          | Arrow                  |
|----------------------------------|------------------------|
| `byte` / `ubyte`                 | int8 / uint8           |
| `short` / `ushort`               | int16 / uint16         |
| `int` / `uint`                   | int32 / uint32         |
| `bigint`                         | int64                  |
| `float`                          | float                  |
| `double`                         | double                 |
| `char[N]` / `string` / `lstring` | string                 |
| `enum{...}` / `set{...}`         | string                 |
| `<type>[N]` / `<type>[field]`    | string (comma-list)    |

Range queries reuse `-r` and call `bbGetOverlappingEntries` /
`bwGetOverlappingIntervals` directly — no `.tbi`/`.csi` needed:

```sh
$ vv -r chr1:300-1100 tests/data/tiny.bb
$ vv --filter 'signalValue > 10' tests/data/tiny.bb
$ vv -r chr1:0-1000 tests/data/tiny.bw
```

bigWig schema is fixed: `chrom`, `start`, `end`, `value`.

# Examples by workflow

## Look at an inherited file first

```sh
vv --schema     huge.parquet         # what's in here?
vv --stats      huge.parquet         # row groups, codecs, sizes
vv --describe   huge.parquet         # min / max / nulls / distinct
vh             huge.parquet          # peek with the right shape for wide tables
```

## Quick region preview

```sh
vv -r chr1:1000000-1100000 variants.vcf.gz    # tabix VCF
vv -r chr1:78-99 peaks.lociss                  # LociSSD Parquet
vv -r chr1:78-99 calls.bcf                     # BCF with .csi
vv -r chr1:1000000-1100000 peaks.bb            # bigBed (no index needed)
vv -r chr1:0-1000 coverage.bw                  # bigWig
vv -r 'chr1:200,chr2:500-1500' regions.bed.gz  # multiple
vv -r chr1:1000-1100 --slop 500 peaks.lociss   # widen
vv --regions-file targets.bed peaks.lociss      # batch
```

## Convert formats

```sh
vv --parquet peaks.parquet                 peaks.bed.gz
vv --parquet vars.parquet --compression snappy   variants.vcf.gz
vv --tsv reads.fq.gz > reads.tsv
```

## Pipe through `jq`

```sh
vv --ndjson reads.fastq.gz \
    | jq 'select(.seq | length > 50) | .name'
```

## Filter + project + export

```sh
vv --parquet hot_peaks.parquet \
   --select Chromosome,Start,End,Score \
   --filter 'Score > 0.8' \
   peaks.lociss
```

## Random preview of huge file

```sh
vv --sample 100 huge.lociss      # 100 random rows in the TUI
vv --tsv --sample 1000 huge.parquet | head    # head of a random sample
```

# Building and installing

See `INSTALL.md` for the complete matrix (Bioconda, Homebrew, AUR,
static binary, source build). Quick start:

```sh
# Build from source
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build

# Or install the portable static binary
curl -L https://github.com/balwierz/vv/releases/latest/download/vv-linux-x86_64.tar.gz | tar -xz
sudo install vv-*-linux-x86_64/vv /usr/local/bin/
```

# Where to look next

* [Project README](../README.md) — quick install + feature highlights.
* [`man vv`](../man/vv.1) — flag reference.
* [`CHANGELOG.md`](../CHANGELOG.md) — release notes.
* [`TODO.md`](../TODO.md) — the planned-feature backlog.
* [LociSSD spec](https://github.com/balwierz/LociSSD/blob/main/FORMAT_SPEC.md).

# Reporting bugs

Open an issue at <https://github.com/balwierz/vv/issues>. Include
`vv --version`, the OS / distribution, the command line, and (if
possible) a minimal input that reproduces the problem.

# License

MIT. See [`LICENSE`](../LICENSE).
