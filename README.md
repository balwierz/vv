# vv — universal genomic file viewer

[![CI](https://github.com/balwierz/vv/actions/workflows/ci.yml/badge.svg)](https://github.com/balwierz/vv/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Bioconda](https://img.shields.io/conda/dn/bioconda/vv.svg?label=Bioconda)](https://anaconda.org/bioconda/vv)
[![Latest release](https://img.shields.io/github/v/release/balwierz/vv)](https://github.com/balwierz/vv/releases)

A fast, self-contained command-line viewer for tabular and bioinformatics
file formats. `vv` prints the first *N* rows as an ASCII table — like
`pandas.DataFrame.head()` — followed by a schema summary and file metadata.
On a terminal it opens an interactive ncurses row browser instead.

```
$ vv variants.vcf.gz
+----+--------+-----------+------+------+------+------+--------+--------+
|    | CHROM  | POS       | ID   | REF  | ALT  | QUAL | FILTER | INFO   |
+----+--------+-----------+------+------+------+------+--------+--------+
|  0 | chr1   |   100_352 | rs1  | A    | G    |   30 | PASS   | AF=0.5 |
|  1 | chr1   |   500_117 | .    | C    | T    |   40 | PASS   | AF=0.1 |
| ...
```

## Quick install

```sh
# Bioconda
conda install -c bioconda vv

# Homebrew (macOS / Linuxbrew)
brew install balwierz/tap/vv

# Static Linux binary (glibc ≥ 2.28)
curl -L https://github.com/balwierz/vv/releases/latest/download/vv-linux-x86_64.tar.gz | tar -xz
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
| Sequence alignments | `.bam`, `.cram`, `.sam`, `.paf`/`.paf.gz` (minimap2)     |
| Variant calls     | `.vcf`, `.vcf.gz`, `.bcf` (binary VCF via htslib)          |
| Genome annotation | `.gff`, `.gff3`, `.gtf` (plus `.gz`)                       |
| Genomic intervals | `.bed`, `.bed.gz`                                          |
| Sequences (FASTA) | `.fa`, `.fasta`, `.fna`, `.faa`, `.ffn`, `.frn` (plus `.gz`) |
| Sequencing reads  | `.fq`, `.fastq` (plus `.gz`)                               |
| Delimited text    | `.tsv`, `.csv` (plus `.gz`)                                |
| Stdin             | `vv -` reads any text format from stdin (auto-gunzip)      |

Unknown extensions are auto-detected by magic bytes (Parquet, Arrow IPC,
Feather) or delimiter heuristic (TSV vs. CSV).

## Features

- **Smart formatting** — integer columns auto-fit visible digits with `_`
  thousands separators (Python PEP 515 style); floats render compactly with
  6 significant digits; lists `[a, b, ...]`, fixed-size lists `(a, b, ...)`,
  and maps `{key: value, ...}` use Python-style brackets and preserve at
  least the first element when truncated.
- **Interactive TUI** — ncurses row browser with hjkl/arrows navigation,
  PgUp/PgDn, `g`/`G`, `less`-style search with `/` (forward) and `?`
  (backward), case-insensitive ECMAScript regex with literal fallback,
  `n`/`N` next/prev (direction-aware), all visible matches highlighted,
  Enter for row detail, `,`/`.` to narrow / widen the current column,
  `z` to freeze the first column, `H` / `F1` for an overlay listing all
  keybindings, mouse wheel to scroll. Lazy-loads only the chunks
  currently on screen.
- **Range queries** — `-r chr1:1000-2000` (also `--window`) filters
  tabix-indexed `.vcf.gz` / `.bed.gz` / `.gff.gz` / `.tsv.gz`,
  indexed **BCF** (`.csi`/`.tbi`), and **LociSSD Parquet** (`.lociss`).
  LociSSD pruning uses the manifest plus Parquet row-group statistics
  on `Start` and `MaxEndSoFar`; BCF uses `bcf_itr_querys`; tabix uses
  its iterator. Multiple windows accepted comma-separated; open-ended
  forms (`chr1:`, `chr1:78-`, `chr1:-99`) supported.
  `--regions-file foo.bed` reads many windows from a BED's first
  three columns. `--slop N` pads each window by N bp on each side
  (`bedtools slop` inline).
- **Multi-threaded I/O** — `-@ N` (samtools convention) drives Arrow's CPU
  pool, BAM/CRAM htslib threads, and BGZF threads for FASTA/FASTQ; auto-
  detects threads when unset.
- **`vh` (vertical head)** — `vh file.parquet` (or `vv --vertical`) shows a
  transposed preview where each field is a row and each record is a
  column; as many records as fit in the terminal width are displayed.
  Wide tables can be browsed by scrolling vertically instead of
  horizontally. `vh` is a symlink to the same binary.
- **Fast partial reads** — `-n 100` on a 5 GB Parquet decodes only the row
  groups it needs. Lazy Arrow IPC opens 10 GB files in milliseconds.
- **Full Arrow type support** — integers, floats, booleans, strings,
  timestamps, dates, decimals, binary, lists, structs, maps, dictionary-
  encoded columns (decoded transparently).
- **Column projection by name** — `--select Chromosome,Start,Score`
  picks specific columns; honours unknown-name errors. Works across
  all view and export paths.
- **Value filter** — `--filter "Score > 0.5"` (also AND / OR and
  string comparisons: `--filter 'Chromosome == "chr1" AND Score > 0.5'`).
  Evaluated per row in C++ on the loaded chunk.
- **`--schema`** — print column names, Arrow types, nullability, and
  the file metadata footer, then exit. Useful for inheriting a file
  and just wanting to know what's in it.
- **`--describe`** — pandas-style per-column statistics (count,
  nulls, min, max, mean for numerics, distinct count for strings).
  Respects `--select` and `--filter`.
- **`--stats`** — print Parquet metadata (row groups, codecs,
  per-column compressed / uncompressed sizes, null counts) without
  reading any data. Falls back to `--schema` for non-Parquet sources.
- **`--unique Chromosome,Strand`** — distinct value counts per column
  (top-50 values per column with overflow indicator).
- **`--sample N`** — uniformly random N rows (reservoir sampling)
  instead of head-N. Combines with the view / export modes and
  `--filter`.
- **TSV / CSV export** — `--tsv` / `--csv` streams the file (or first *N*
  rows) with RFC 4180 quoting.
- **JSON / NDJSON output** — `--json` writes a JSON array;
  `--ndjson` writes one JSON object per line. Pipe-friendly for
  `jq` and downstream tooling.
- **Parquet output** — `--parquet OUT` converts any supported input
  (BED, TSV, CSV, VCF, BCF, FASTA, FASTQ, BAM, …) into a Parquet file.
  Compression via `--compression {zstd,snappy,gzip,lz4,none}` (default
  zstd). Streams chunk-by-chunk so multi-GB inputs don't need to fit in
  RAM.
- **Schema + metadata footer** — column types, nullability, row count,
  row groups, compressed size, creator string.
- **Color output** — auto-detected; rich colors on a terminal, plain text
  when piped.
- **BED `itemRgb`** — renders as a colored bar in the terminal.
- **VCF `INFO` field expansion** — declared `##INFO` fields appear as
  individual columns.
- **Header detection** — TSV/CSV files with `##` comments and `#header`
  lines are handled (CADD, dbSNP, etc.); files where all column names look
  numeric are treated as headerless and auto-numbered.

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
| `-w <width>`               | max cell width in the ASCII table (default 32)       |
| `-c <cols>`                | max columns to show                                  |
| `-r`, `--region <REGION>`  | tabix range query, e.g. `chr1:1000-2000`             |
| `-@`, `--threads <N>`      | worker threads (default auto, capped at 8)           |
| `--tsv` / `--csv`          | stream delimited output instead of an ASCII table    |
| `--no-interactive`         | force the ASCII table even on a terminal             |
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

# Multi-region tabix query on a BED file
vv -r 'chr1:100-200,chr2:500-1000' regions.bed.gz

# Multi-threaded scan of a 5 GB BAM
vv -@ 4 -n 1000 alignments.bam

# Export FASTQ as a TSV table
vv --tsv reads.fq.gz > reads.tsv

# Pipe TSV through vv from stdin (auto-detects gzip)
zcat huge.tsv.gz | vv -

# Convert a BED file to Parquet (zstd by default)
vv --parquet peaks.parquet peaks.bed

# Convert a tabix-filtered VCF region to Parquet, snappy-compressed
vv -r chr1:1000000-2000000 --parquet region.parquet --compression snappy variants.vcf.gz

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
