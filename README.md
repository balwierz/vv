# parquet_viewer

A fast, self-contained command-line tool for inspecting and converting tabular data files.
It prints the first *N* rows as an ASCII table — like `pandas.DataFrame.head()` — followed by a schema summary and file metadata. Supports Parquet, BAM/SAM, VCF, GFF3/GTF, BED, TSV, CSV, and gzip-compressed variants.

## Features

- **Multiple input formats** — `.parquet`, `.bam`, `.sam`, `.vcf`, `.gff/.gff3/.gtf`, `.bed`, `.tsv`, `.csv` (and `.gz` variants); unknown extensions are auto-detected by magic bytes / delimiter sniffing
- **Intelligent header detection** — TSV/CSV files with numeric column names are treated as headerless and auto-numbered
- **BED files** — always treated as headerless tab-separated; itemRgb column renders as a color bar in the terminal
- **BAM/CRAM/SAM** — mandatory 11 columns (QNAME…QUAL); optional alignment tags are dropped; genomic coordinates shown with `_` digit grouping
- **Interactive TUI** — ncurses row browser with arrow/hjkl navigation, activated automatically when output is a terminal; lazy-loads only the rows currently on screen
- **Pandas-style table** with aligned columns, right-aligned numbers, `∅` for nulls, `…` truncation marker
- **Full Arrow type support** — integers, floats, booleans, strings, timestamps, dates, decimals, binary, lists, structs, maps, and **dictionary-encoded columns** (values decoded, not raw indices)
- **Color output** with auto-detection — rich colors in the terminal, plain text when piped
- **TSV / CSV export** — streams the full file (or first *N* rows) as delimited text with RFC 4180 quoting
- **Schema summary** — column names, Arrow types, nullability
- **File metadata** — row count, row groups / format info, compressed size, creator string
- **Fast** — reads only as many row groups / batches as needed to satisfy the requested row count

## Usage

```
parquet_viewer [options] <file>

Supported formats:
  .parquet
  .bam  .cram                  binary/compressed sequence alignments (htslib)
  .sam                        text sequence alignments
  .vcf  .vcf.gz               variant calls
  .gff  .gff3  .gtf           and .gz variants  genome annotations
  .bed  .tsv  .csv            and .gz variants
  (unknown extensions: sniffed by magic bytes / delimiter)

Interactive viewer (default when stdout is a terminal):
  -i / --interactive  open the ncurses row browser
  --no-interactive    force plain table output even on a terminal
  Keys: arrows/hjkl navigate, PgUp/PgDn, g/G top/bot, q quit

Table options:
  -n <rows>           rows to display  (default: 10, 0 = all)
  -w <width>          max cell width   (default: 32)
  -c <cols>           max columns to show (default: all)
  --no-index          suppress the row-index column
  --color[=WHEN]      colorize output: auto (default), always, never

Delimited output (replaces table view):
  --tsv               write tab-separated values to stdout
  --csv               write comma-separated values to stdout
  --delimiter <sep>   write with a custom single-character delimiter
  --no-header         omit the header row
  (-n defaults to all rows in this mode; -c still applies)

  -h                  show this help
```

## Example output

```
$ parquet_viewer data.parquet
+---+------+----------+----------+--------+-------------------------+----------------------------------+
|   | id   | name     | score    | active | ts                      | notes                            |
+---+------+----------+----------+--------+-------------------------+----------------------------------+
| 0 |    0 | Person 0 |        0 | true   | 2024-01-01 00:00:00.000 | ∅                                |
| 1 |    1 | Person 1 |  1.23456 | false  | 2024-01-01 01:00:00.000 | Note with some longer text fo…   |
| 2 |    2 | Person 2 |  2.46912 | true   | 2024-01-01 02:00:00.000 | Note with some longer text fo…   |
...
+---+------+----------+----------+--------+-------------------------+----------------------------------+

[100 rows x 6 columns]
```

```
$ parquet_viewer --csv -n 3 data.parquet
id,name,score,active,ts,notes
0,Person 0,0,true,2024-01-01 00:00:00.000,
1,Person 1,1.23456,false,2024-01-01 01:00:00.000,Note with some longer text for row 1
2,Person 2,2.46912,true,2024-01-01 02:00:00.000,Note with some longer text for row 2
```

## Building

Requires CMake 3.16+, g++ with C++20 support, and the Arrow/Parquet development libraries.

On Debian/Ubuntu:
```sh
sudo apt-get install libarrow-dev libparquet-dev libhts-dev cmake g++
```

Then:
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary is at `build/parquet_viewer`. It is statically linked — copy it anywhere.

### Dependencies

All dependencies except glibc are linked statically:

| Library | Version | How |
|---|---|---|
| Apache Arrow + Parquet | system | static `.a` |
| htslib | system | shared `.so` |
| mimalloc | v2.1.9 | built from source via FetchContent |
| snappy, lz4, zstd, zlib, bz2, brotli | system | static `.a` |
| thrift, re2, xxhash, utf8proc, absl | system | static `.a` |
| libc, libm | system | dynamic (glibc ABI is stable) |
