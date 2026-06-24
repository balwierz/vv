# Changelog

All notable changes to `vv` are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **LociSSD top banner.** The viewer shows a banner above the table with the
  genome assembly and species — derived from the assembly (e.g. `hg38` →
  *Homo sapiens*) when the manifest's `species` is null — and the total element
  count, e.g. `LociSSD • hg38 (Homo sapiens) • 633_678 elements`. Appears in both
  the interactive TUI (a reserved top row) and non-interactive table output.

## [1.13.0] - 2026-06-22

A focused follow-up that makes AnnData `obs` / `var` actually dumpable as text.

### Added
- **Full obs/var text dump.** `vv file.h5ad --tab obs --tsv` (and `--tab var`)
  now exports the *complete* DataFrame instead of the 1000-row interactive
  preview — previously the dump was silently truncated at 1000 rows. `-n N`
  limits it; the TUI and plain table view keep the bounded preview. The sparse /
  dense `X` matrix stays capped (a full text dump of X is intentionally not
  enabled).

### Fixed
- **High-cardinality categorical obs/var columns now decode** to their string
  labels instead of integer codes. The category-dictionary cap was 65536, which
  wrongly showed real columns like CRISPR perturbation guides / targets
  (hundreds of thousands of categories) as raw codes; the default is now
  1,000,000, overridable with `VV_CATEGORY_DICT_CAP`.
- **Boolean obs/var columns** (stored as HDF5 enums — e.g. `highly_variable`,
  `mt`) render their values (`FALSE` / `TRUE`) instead of `?`.

## [1.12.0] - 2026-06-22

AnnData depth (CSC sparse, `uns`, labelled `X`), data-correctness fixes across
the CSV / display / numeric paths, security hardening, and broad RAM/throughput
work on the streaming and TUI paths.

### Added
- **AnnData `uns` decoding** — a new key/value tab surfaces the unstructured
  annotations: scalars, strings and short arrays show their values, nested dicts
  flatten with dotted keys (`pca.variance_ratio`), and an encoded sub-object
  (dataframe / categorical) shows its `encoding-type`. Previously skipped.
- **AnnData CSC sparse `X` preview** — `csc_matrix` matrices now densify to a
  rows × columns preview just like CSR (the compressed axis is columns, not
  rows); they previously showed only a "not implemented" note.
- **Labelled AnnData `X` preview** — the sparse / dense `X` preview names its
  value columns by the `var` index (gene names) and prepends the `obs` index
  (cell barcodes) as a row-label column, instead of generic `col0` / row numbers.

### Fixed
- **Leading-zero IDs in CSV/TSV** (and spreadsheet / markdown / HTML tables) are
  preserved — a column of values like `007` is read as text instead of being
  inferred as the integer `7`. Scientific notation stays numeric.
- **Wide and combining characters align.** `display_width` now measures terminal
  columns (CJK / fullwidth / emoji = 2, combining / zero-width = 0) instead of
  counting codepoints, so tables with such data no longer skew; truncation cuts
  on a column boundary.
- **Hostile / malformed files are handled gracefully** — AnnData sparse `shape`
  and DataFrame column lengths are validated against the real dataset extents
  (no out-of-bounds read).
- **Date / timestamp / decimal columns** are read as numeric values, so
  `--describe` min/max/mean and `--heatmap` work on them; `--describe` now
  summarises the whole table rather than the first chunk.
- **Genomic / format correctness** — Parquet region pruning indexes column
  statistics by Parquet leaf (not Arrow field); pileup honours reference skips
  (CIGAR `N`) like samtools; mid-stream htslib read errors in pileup / BCF
  region mode are surfaced instead of silently truncating; region mode reports
  exact post-filter row counts; CSV headers of `nan` / `inf` / hex are no longer
  mistaken for headerless numeric data.
- **TUI / terminal robustness** — the terminal is restored on
  SIGINT/SIGTERM/SIGHUP via an async-signal-safe handler; markdown rendering
  strips terminal control bytes; `truncate` never splits a multibyte codepoint;
  a missing flag argument reports the specific flag instead of "Unknown option".

### Performance
- **Bounded RAM on streaming sources** — DelimitedSource and the other streaming
  readers evict old chunks behind a retention window instead of retaining every
  decoded batch; SQLite computes `COUNT(*)` lazily (not at open); generic HDF5
  1-D datasets and wide NPZ arrays preview a capped head; delimited preamble
  stripping is buffered. TUI redraws format each visible cell once and reuse the
  LRU chunk cache during sorted search.

### Security
- **SQLite identifier quoting** escapes embedded double quotes, so a table named
  `a"b` no longer produces malformed / injectable SQL.
- **NPZ allocation hint** — the zip central-directory `uncompressed_size` (which
  is attacker-controllable) is no longer passed straight to `reserve()`, so a
  tiny crafted entry can't force a multi-GB allocation.

### Build / CI
- KDE plugins stop including `KDEInstallDirs`, silencing the ECM directory
  warning in the GUI build.

## [1.11.0] - 2026-06-15

Large-AnnData usability, a Debian package, and build/CI maintenance.

### Added
- **`--tab <name>`** — view a named component tab (AnnData `obs` / `var` / `X`,
  a workbook sheet, …) straight from the CLI, e.g.
  `vv cells.h5ad --tab obs -n 20`. Case-insensitive; an unknown name lists the
  available tabs. The data components were previously only reachable in the
  interactive TUI.
- **Debian package** — `packaging/debian/build-deb.sh` wraps the static
  AlmaLinux 8 binary into a self-contained `.deb` (`Depends: libc6` only), and
  the release workflow now publishes a `vv_<version>_<arch>.deb` for x86_64 and
  aarch64 alongside the tarballs.

### Fixed
- **Large AnnData components no longer stall.** Opening a multi-GB `.h5ad`
  (e.g. a 100M-cell atlas plate with a 4.7M-row `obs`) used to hang for minutes:
  the TUI eagerly materialised every component and read `obs`/`var` in full.
  Now each component tab is built lazily (only when viewed), and `obs`/`var`/`X`
  are read as a bounded first-rows preview (footer notes the true size). A
  high-cardinality categorical (e.g. a per-cell barcode with millions of
  categories) is shown as integer codes instead of reading its whole dictionary.
- **Arrow deprecation warnings** in the Parquet read paths silenced by moving to
  the `arrow::Result` overloads of `ReadRowGroups` / `GetRecordBatchReader`
  (version-guarded so the Arrow 23.0.1 static build still compiles).

### Build / CI
- ncurses is fetched from the GNU mirror (`ftp.gnu.org`) instead of
  `invisible-mirror.net`, which intermittently WAF-blocked CI runners and broke
  a release.
- GitHub Actions bumped to their Node 24 runtimes ahead of the forced migration.

## [1.10.0] - 2026-06-14

A feature and robustness release: a new terminal **heatmap** view, a major
upgrade to the Qt6 desktop GUI (`vvg`), and the remaining high-severity
correctness/OOM fixes from the code audit.

### Added
- **`--heatmap`** — render the numeric columns as a colour heatmap in the
  terminal (rows × numeric-columns, globally normalised, viridis palette).
  `--image-mode` selects the backend: `auto` (kitty graphics if supported, else
  half-block), `kitty`, `sixel`, `halfblock`, or `ascii`. When stdout is not a
  terminal a plain ASCII intensity grid is written instead of raw escape
  sequences.
- **Qt6 GUI (`vvg`)** — a real desktop application: menu bar, File▸Open
  (multi-file → tabs), drag-and-drop, recent files, and error dialogs; a
  **genomic region/tabix query bar** with a pileup toggle (re-opens BAM/VCF/BED
  over a region); a View menu (column show/hide, go-to-row, shortcuts help).
  Filtering, sorting and find now run **off the UI thread** with a progress bar
  and a Cancel button, so the window stays responsive on large files.

### Fixed
- **HDF5 / AnnData** — a dense `X` matrix tab is previewed (first 1000 rows ×
  200 columns) instead of densifying the entire matrix into RAM, which could
  OOM/abort the reader on a real dataset.
- **Workbooks (XLSX & ODS)** — a row wider than the header no longer makes
  Arrow reject the whole sheet; every row is padded to the widest and the
  overflow header columns are named `colN`.
- **ODS** — `table:number-rows-repeated` on a non-empty row now expands to N
  rows (capped) instead of silently dropping the duplicates.
- **TUI** — sort / filter / stats / search drain a streaming source to EOF
  first, so they cover the whole file instead of only the chunks scrolled
  through so far.
- **`--heatmap`** — `Inf`/`NaN` cells no longer poison normalization (they're
  treated as gaps); the scan buffer is bounded; non-TTY output is guarded.
- **KDE plugins** — the Dolphin thumbnailer and KFileMetaData extractor handle
  malformed input gracefully (empty result, no worker abort).

### Performance
- **GUI** — row→chunk mapping is `O(log chunks)` via a cumulative-offset binary
  search (was `O(chunks)` per cell); find navigates a precomputed match list in
  `O(log matches)` and uses an Arrow `match_substring` prefilter for literals.

### Docs
- Documented `--heatmap` / `--image-mode`, NumPy `.npz` / `.npy`, and `.fods`
  across `--help`, the man page, the README, and the bash/fish/zsh completions;
  clarified that the KF6 GUI plugins are optional.

## [1.9.1] - 2026-06-14

A correctness- and robustness-focused patch release: it fixes a class of
silently-wrong results, several crash/hang/OOM paths on malformed input found
in a code audit, and the release/CI pipeline. No user-facing feature changes.

### Fixed
- **Region coordinates** — every htslib-backed range query (tabix BED/VCF/GFF,
  BAM `--pileup -r`, BCF) fed vv's canonical UCSC 0-based half-open coordinates
  to htslib's 1-based-inclusive region parsers, shifting every query one base
  and disagreeing with the Parquet path for the same `-r`. Coordinates are now
  converted at the htslib boundary so all formats agree.
- **Parquet region queries** now handle every integer coordinate width
  (UInt32/Int16/…, and dictionary-encoded ints), not just Int32/Int64 — a
  UInt32 `Start`/`End` column previously made region queries return nothing.
- **SQLite** `NUMERIC`/`DATE`/`DATETIME`/`BOOLEAN` columns are preserved
  verbatim instead of being coerced through `double` (which turned dates into
  their year and rounded 64-bit integers beyond 2^53).
- **BCF** keeps the `FORMAT` field (e.g. `GT:AD:DP`) in the `FORMAT_SAMPLES`
  column instead of dropping it.
- **Delimited streaming** surfaces a mid-file parse error (a malformed row
  beyond the first 16 MiB block) as a non-zero exit instead of silently
  truncating output with status 0.
- **Streaming sources** (Arrow IPC, FASTA/FASTQ, SQLite) no longer spin or
  silently truncate when a read error occurs past the first batch.
- **Empty tabix region** (a window overlapping no records) returns an empty
  result with exit 0, matching the Parquet/BCF/BAM paths, instead of erroring.

### Security
Memory-safety fixes for malformed/untrusted input (reachable in-process via
the KDE thumbnailer / KFileMetaData extractor):
- **HDF5** — array-valued attributes no longer overflow fixed-size buffers in
  `H5Aread` (a crafted `shape`/string attribute could smash the stack — verified
  segfault, now rejected).
- **NumPy `.npz`** — the declared array shape is validated against the stored
  data (rejecting negative dimensions, overflowing products, and shapes larger
  than the buffer) instead of driving an out-of-bounds read.
- **2bit** — the header `seqCount` is bounded against the file size before
  reserving (a crafted value requested ~170 GB and aborted the process); the
  N-block table skip is computed in 64-bit.

### Build / CI
- The KF6 Dolphin/KFileMetaData GUI plugins are now optional in CI so the
  Ubuntu 24.04 job (which lacks the KF6 dev packages) builds the Qt GUI without
  them rather than failing.
- The static-binary release fetch step retries on transfer timeouts
  (`--retry-all-errors`, connect timeout), fixing the flaky-download failure
  that blocked the 1.9.0 release.

## [1.9.0] - 2026-06-02

### Added
- **Qt6/KDE graphical mode (`vvg`)** — a desktop viewer with the same
  capabilities as the terminal version: multi-tab navigation of
  multi-sheet / multi-dataset files (xlsx/ods sheets, SQLite tables,
  HDF5/AnnData components), typed click-to-sort, a live `--filter`-DSL
  bar, regex find with match highlighting, per-column statistics, a
  row-detail dock, copy-as-TSV, NPZ 3-D slice stepping, and a two-line
  name+type column header. Lazy chunk paging with an LRU cache keeps
  large files responsive. Built with `-DVV_BUILD_GUI=ON` (Qt6 Widgets).
- **Shared reader core (`libvvcore`)** — the file-format readers, Arrow
  plumbing, filter engine, and formatters are now exposed via
  `include/vv/vvcore.hpp` and compiled into a reusable static library
  (the same `main.cpp` built with `-DVV_CORE_LIB`, minus `main()` and the
  ncurses TUI). The CLI is byte-identical; the GUI and KDE plugins link
  this core in-process.
- **KDE Plasma integration** — a `KIO::ThumbnailCreator` plugin renders
  table-snapshot thumbnails in Dolphin, and a `KFileMetaData` extractor
  surfaces row/column counts, schema, codec, and generator in the
  Information Panel. Ships shared-mime-info definitions (Parquet, Arrow/
  Feather, HDF5/AnnData, NumPy `.npz`), a `.desktop` entry, AppStream
  metainfo, and an icon. Packaged as a separate `vv-gui` (Arch split
  package + RPM subpackage); the lean CLI `vv` is unchanged.

### Fixed
- CMake: use the non-deprecated `SQLite3::SQLite3` target when available
  (silences the deprecation warning on recent Arch / CMake).

## [1.8.2] - 2026-05-23

### Fixed
- Static-binary build: the vendored libhdf5 was configured with
  `--disable-deprecated-symbols`, but main.cpp uses
  `H5Dvlen_reclaim` to free variable-length strings (its 1.12+
  replacement `H5Treclaim` doesn't exist on Ubuntu 22.04's libhdf5
  1.10, so the deprecated name is the portable choice). Dropped
  the flag from the AlmaLinux 8 build so the static binary
  compiles. Packaging-only release; no source changes.

## [1.8.1] - 2026-05-23

### Fixed
- Static-binary build: the HDF Group moved the HDF5 1.14.4 tarball
  out of `support.hdfgroup.org`, breaking the AlmaLinux 8 fetch
  step in the v1.8.0 release workflow. Switched to the GitHub
  Releases mirror (`hdf5_1.14.4.3`) so the static x86_64 / aarch64
  binaries build again. No source changes vs 1.8.0; this is purely
  a packaging fix.

## [1.8.0] - 2026-05-23

### Added
- **AnnData / HDF5 viewer (`.h5ad`, `.h5`, `.hdf5`, `.loom`)** — opens
  HDF5 containers via libhdf5. AnnData files (`.h5ad`) are detected
  via the root `encoding-type="anndata"` attribute (or the modern
  `/obs` + `/var` + `X` heuristic) and decode into a multi-tab view:
  tab 0 is a summary (shape, X encoding, layer count), siblings are
  `obs`, `var`, `X` (densified first-1000-row preview for CSR sparse),
  and one tab per `obsm` / `varm` / `layers` entry. Categoricals
  (`encoding-type="categorical"` groups with `codes` + `categories`
  children) decode back to string columns. Generic HDF5 files
  (`.h5`, `.hdf5`, `.loom`) get a hierarchy table listing every
  group / dataset with `path`, `kind`, `shape`, `dtype`, `n_attrs`,
  plus a tab for every 1-D / 2-D dataset (capped at 32 datasets / 32
  columns so a chunky file doesn't explode the tab bar). 12 new
  smoke tests, 190/190 pass. CSC sparse preview and `uns`
  (unstructured) decoding are deferred to follow-ups.
- **Markdown viewer auto-pager** — `vv README.md` on a TTY now pipes
  through `less -R -F -X --tabs=4` so the user gets scroll / search
  without us building a markdown-specific ncurses TUI. `-F` makes
  short READMEs quit-on-fit; the user's own keybindings (`/`, `n`,
  `g`, `G`, `q`, …) come along for free. Falls back to direct stdout
  when less isn't on `$PATH`. Bypassed for scripted invocations
  (`--no-interactive`, `--tsv`, `--csv`, `-n N`, `--schema`,
  `--describe`, `--stats`, `--parquet OUT`) and when stdout is piped.
- **Markdown viewer (`.md`, `.markdown`, `.mdown`, `.mkd`)** — renders
  CommonMark + GFM via the vendored md4c parser. The prose body is
  written to stdout as ANSI (headings, bold / italic / strike / code,
  lists, block quotes, fenced code blocks, horizontal rules, link text
  + URLs), word-wrapped to the terminal width. GFM `|`-tables are
  extracted from the prose stream and rendered through vv's normal
  table renderer, complete with column-type inference (so a benchmark
  column of `121.7` / `1240.3` becomes `double` and `--filter` works
  against it). Local PNG/JPEG/GIF images are rendered inline on
  kitty / iTerm2 / WezTerm terminals via their graphics protocols;
  remote URLs, SVGs, and graphics-blind terminals fall back to a
  `🖼 [alt-text]` stub. Targets the "outdated Linux without root" use
  case — `scp vv user@server:` and pipe READMEs through `less -R`.
- **samtools mpileup (`.pileup`, `.mpileup`, `.pile`, plus `.gz`)** —
  per-base pileup output. The file is routed through a new
  `DelimKind::Mpileup` variant of the existing `DelimitedSource`,
  which counts tabs on the first row to derive sample-aware column
  names (`chrom`, `pos`, `ref`, then `depth` / `bases` / `quals`
  triplets — unsuffixed for single-sample files, `_1` / `_2` / … for
  multi-sample). Range queries work on bgzipped + tabix-indexed
  files (`tabix -s 1 -b 2 -e 2`).
- **`--pileup` on BAM / CRAM** — `vv x.bam --pileup` walks the
  alignments through htslib's `bam_plp_auto` engine and emits
  mpileup-style per-base rows directly, with no `samtools mpileup`
  intermediate. The output schema matches the file-based mpileup
  reader (`chrom`, `pos`, `ref`, `depth`, `bases`, `quals`) and is
  byte-identical to `samtools mpileup x.bam`. Range queries via
  `-r chrom:start-end` need a BAM index (`.bai` / `.csi`) or CRAM
  index (`.crai`); positions are trimmed to the requested span the
  same way `samtools mpileup -r` does. Composes with
  `--decode-pileup`. No reference FASTA support yet — ref is `N`,
  bases render as their literal letter case-by-strand.
- **`--decode-pileup`** — explodes the packed `bases` / `quals`
  columns into typed per-allele counts: `A`, `C`, `G`, `T`, `N`,
  `del_placeholder`, `ins`, `del`, `fwd`, `rev`, `mean_qual`. The
  decoder walks each `bases` string with a small state machine that
  handles `.` `,` `[ACGTNacgtn]` `*` `^X` `$` `+N<seq>` `-N<seq>`;
  matches roll up against the reference allele (case-insensitive),
  mismatches against their literal base, indel markers bump the
  ins/del counts without consuming a quality character. Multi-sample
  files emit the same set of counts per sample (`A_1`, `A_2`, …).
  Lets `--filter 'A >= 5 and mean_qual >= 30'` work directly on the
  parsed pileup.
- **OpenDocument Spreadsheet (`.ods`)** — hand-rolled reader on top of
  minizip + expat: `content.xml` is inflated and SAX-parsed, with each
  sheet routed through the existing `WorkbookSource` framework (one
  sheet per TUI tab, shared in-memory CSV → Arrow CSV reader pipeline
  with the Excel path). Honours `office:value-type` so numeric / date /
  boolean cells use their canonical typed attribute instead of the
  display text (which avoids locale-formatted thousands separators
  poisoning type inference). Recognises `table:number-columns-repeated`
  and trims trailing empty cells so ODS's "rest of the row" sentinel
  doesn't balloon the in-memory CSV.
- A reusable `csv_buffer_to_table` helper drops out of the xlsx
  refactor: both `XlsxSource` and `OdsSource` route their per-sheet
  CSV bytes through it.
- **Apache ORC (`.orc`)** — columnar storage (Hadoop / Hive / Spark)
  opens via Arrow's ORC adapter. One ORC stripe is one chunk in vv's
  pipeline, so multi-GB files stream lazily. Read-only. Footer shows
  stripe count, file size, and codec (`Format: ORC | Stripes: N |
  Compressed: K KiB | Codec: zstd`). Requires Arrow built with
  `-DARROW_ORC=ON`, which the Apache Arrow apt repo, Homebrew
  `apache-arrow`, and the conda-forge `pyarrow` wheel all default to.
  CMake auto-detects the adapter; without it the .orc dispatch
  reports "compiled without ORC support". The AlmaLinux 8 static
  binary currently ships without ORC (TODO: add liborc + libprotobuf
  static-build stages).
- **Excel (`.xlsx`, `.xlsm`)** — Office Open XML workbooks open natively
  via `libxlsxio`; each sheet becomes a TUI tab (`Tab` / `Shift+Tab`
  cycle). Cell text streams through Arrow's CSV reader for column-type
  inference (int / float / bool / ISO 8601 date / string), so an `int64`
  column declared in Excel survives the round-trip and `--filter
  'score > 5.0'` works as expected. Non-interactive output modes
  (`--tsv`, `--parquet`, …) process the first sheet only; the footer
  notes how many more exist (`Format: Excel | Sheet: peaks | +1 more
  sheet(s)`). The legacy binary `.xls` format is **not** supported.
  Builds against the system `libxlsxio` (Arch `xlsxio` AUR, Debian
  `libxlsxio-dev`, Homebrew `xlsxio`); the AlmaLinux 8 static binary
  builds xlsxio + its expat / minizip-ng deps from source via the
  fetch-docker-sources script.
- A new internal `WorkbookSource` base class (extending
  `MemoryTableSource`) captures the "one source per sheet, plus a list
  of sibling sheet names" pattern shared between Excel and the
  upcoming OpenDocument Spreadsheet (`.ods`) support. The
  format-specific subclass only needs to list sheet names, stream one
  sheet's rows into a CSV byte buffer, and build sibling sources that
  share the underlying library handle.
- **SQLite (`.sqlite`, `.sqlite3`, `.db`)** — single-file databases now
  open natively; each user table becomes a TUI tab (`Tab` / `Shift+Tab`
  cycle). Column types follow SQLite's type affinity: TEXT → string,
  INTEGER → int64, REAL → double, BLOB → binary; `NOT NULL` is
  preserved in the Arrow schema. The shared `sqlite3*` handle is
  refcounted across the sibling sources so a single open serves the
  whole file. Non-interactive output modes (`--tsv`, `--parquet`, …)
  process the first table only; the footer notes how many more
  exist (`Format: SQLite | Table: peaks | Rows: 3 | +1 more table(s)`).
  Local builds link against the system `libsqlite3`; the AlmaLinux 8
  static binary compiles the upstream amalgamation into a static
  `libsqlite3.a` (`SQLITE_OMIT_LOAD_EXTENSION`, no external deps).
- **ENCODE peak / signal formats** (`.narrowPeak`, `.broadPeak`,
  `.gappedPeak`, `.bedGraph` / `.bg`, `.tagAlign`, plus `.gz`
  variants). These are read as BED-family files with variant-aware
  column naming:
  - **narrowPeak** (BED6+4): `signalValue`, `pValue`, `qValue`, `peak`.
  - **broadPeak** (BED6+3): `signalValue`, `pValue`, `qValue`.
  - **gappedPeak** (BED12+3): BED12 + `signalValue`, `pValue`, `qValue`.
  - **bedGraph** (BED4): `value` (float).
  - **tagAlign** (BED6 with col-3 named `sequence`).

  Arrow types are inferred from the data, so `--filter` /
  `--describe` work directly on `signalValue > 5`, `pValue < 0.01`,
  etc. The footer reports the specific format
  (`Format: narrowPeak (BED6+4)`) and the file's preamble lines
  (`track`, `browser`) are preserved above the table just like
  vanilla BED. `vv -r` regional queries work on tabix-indexed
  `.narrowPeak.gz` etc.

  bigBed-flavored peak files (`.bb` with `narrowPeak` autoSql)
  already worked via the libBigWig path shipped in v1.5; this
  closes the gap for the plaintext variants.

### Changed
- AnnData files in the pre-0.7 layout (root has no `encoding-type`,
  `obs` / `var` stored as compound HDF5 datasets, `X` keyed by
  `h5sparse_format` instead of `encoding-type`) now refuse to open
  with a clear migration hint, instead of rendering an almost-empty
  summary. Re-save with a recent anndata to use the file with vv.

## [1.7.0] - 2026-05-16

Interactive-viewer focus on top of 1.6.0. Major additions to the
ncurses TUI: mouse clicks, an in-place theme picker with XDG-spec
persistence, a vim-style `:` command line, and multi-file tabs.

### Added
- **TUI multi-file tabs** — `vv a.vcf b.bed c.parquet` opens each
  file as a tab. `Tab` / `Shift+Tab` cycle. Each tab keeps its own
  scroll position, sort, filter, column-visibility set, search
  anchor, and chunk cache; switching back is instant. Status bar
  shows `tab N/M: basename` when more than one file is open.
  Non-interactive output modes (`--tsv`, `--parquet`, …) still
  process only the first positional — multi-file is TUI-only.
- **TUI command line (`:` key)** — vim/less-style typed-command
  prompt at the bottom of the screen. First verbs:
  - `:<N>` — jump to row N (matches the row-index column;
    out-of-range clamps to the last page). Closes the "scroll
    through millions of rows by hand" pain point.
  - `:q` / `:quit` — quit (vim muscle memory).
  - `:theme NAME` — text-driven theme switch (alt to the `T`
    overlay), with the same name set as `--theme`. Persisted to
    `~/.config/vv/config` via the standard XDG path.
  Errors stay in the input bar so the line can be edited; `Esc`
  cancels. Extensible — future verbs (`:reload`, `:w FILE`, …)
  slot in without new key bindings.
- **TUI theme picker (`T` key)** — overlay listing every built-in
  theme; `[*]` marks the currently-active one, `j`/`k` move the
  cursor, `Enter` applies the choice. The new theme takes effect
  immediately (ncurses color pairs re-initialised in place) and
  the choice is persisted to `~/.config/vv/config`
  (`$XDG_CONFIG_HOME/vv/config` if set) so future runs start with
  the same theme. `Esc` closes the overlay without changing.
- **XDG-spec user config** — `vv` now reads
  `$XDG_CONFIG_HOME/vv/config` (default `~/.config/vv/config`)
  at startup. Format is plain INI-style `key = value`; lines
  starting with `#` are comments. Today only `theme` is read;
  the format is extensible — future preferences (default
  `--threads`, `--decode-threads`, etc.) slot in without breaking
  existing files. Writes are atomic (`.tmp` + rename) and preserve
  any comments / other keys in place. CLI flags always win over
  the config file.
- **TUI mouse clicks** — three additive bindings on top of the
  existing wheel-scroll: click a column header to sort by that
  column (toggle asc/desc on repeat; updates the active column
  for `S` / `y`); click a data row to scroll it to the top;
  double-click a data row to open the detail pane (same as
  `Enter`). Shift+drag continues to work as the universal escape
  hatch for native terminal text selection.

## [1.6.0] - 2026-05-14

A broad feature release on top of 1.5.0. Highlights: bigBed / bigWig
and 2bit support via a vendored libBigWig and a hand-rolled 2bit
reader; full TUI exploration suite (column stats, sort, show/hide,
live filter, copy cell); generic Parquet range queries with column
auto-detection; markdown output and LociSSD validation; coordinate-
convention selector with UCSC and NCBI labels; color themes; LTO +
ccache static builds; and ARM64 release artifacts.

### Changed
- **Smarter inline truncation for `list` / `map` cells.** The
  truncator now walks every top-level comma and picks the largest
  leading-element prefix that fits the column. Previous behavior
  always dropped to `[first, …]` after the first comma, even when
  `[first, second]` would have fit. Most visible on VCF INFO
  expansions, BAM tags, and other multi-element list cells common
  in real data. Falls back to char-truncation only when even one
  element + ellipsis doesn't fit.

### Added
- **TUI copy cell (`y` key)** — copies the top-left visible cell to
  the system clipboard via OSC52. Works through SSH and tmux 3.3+
  with no `xclip` / `pbcopy` helper required (modern terminal
  emulators intercept the escape directly). Status bar flashes
  `copied: <preview>` until the next key; auto-clears.
- **UCSC 2bit (`.2bit`)** — sequence container used for genome
  references (hg38.2bit, mm10.2bit). vv exposes the sequence index
  rather than decoded bases (chromosome-scale strings are too large
  to materialise): `name`, `length`, `n_blocks` (unknown-base runs),
  `mask_blocks` (soft-masked-region runs). Endianness is detected
  from the signature; long-offset 64-bit 2bit is rejected with a
  clear message (use `twoBitToFa` for that variant).
- **`--theme <name>`** — color palette selector. Built-in themes:
  `default`, `dark`, `light`, `solarized-dark`, `solarized-light`
  (`solarized` accepted as a synonym for `solarized-dark`). Affects
  both the non-interactive ASCII table (ANSI escapes) and the
  ncurses TUI. Each theme provides a 256-color palette and a
  16-color fallback for older terminals; the `light` theme also
  suppresses zebra stripes so the table reads cleanly on a bright
  background. Unknown theme names produce a clear error listing the
  available choices.
- **TUI live filter (`&` key)** — long-requested. Opens a
  `&<expression>` input bar; expression grammar matches the
  `--filter` CLI flag. Rows not matching the predicate are hidden
  from the display; status bar shows `filter:<expr>  N/M` (rows
  kept / total). Composes with sort (`s`): one full-file pass
  rebuilds the combined view via the existing `source_row(display)`
  indirection. Empty input or `Esc` (in normal mode) clears.

  Internals: a new `rebuild_display_order()` orchestrates both
  sort and filter into the single `sort_order_` permutation. When
  both are active, rows that pass the filter are collected with
  their sort keys in one pass and sorted at the end.
- **`--decode-threads N`** — separately size Arrow's CPU thread pool
  (Parquet column decode and CSV / TSV parallel parsing) without
  touching htslib's thread count. Defaults to `--threads`; useful
  when cold reads bottleneck on column decompression. Bounded at
  `2 × hardware_concurrency()`.

### Changed
- **Release pipeline**: now builds both **x86_64** and **aarch64**
  (ARM64) static Linux binaries on every tag. The release workflow
  matrices over `ubuntu-latest` and the new GitHub-hosted
  `ubuntu-22.04-arm` runner — native builds on each architecture,
  no QEMU emulation. Tarballs are named
  `vv-<ver>-linux-<arch>.tar.gz`; a merged `SHA256SUMS` covers both.
- **AlmaLinux 8 Docker build**: dropped the `-march=x86-64`
  baseline flag from the build-app CMake invocation so the same
  Dockerfile builds on aarch64. (`-march=x86-64` is the default
  target on x86_64 gcc anyway, so the change is a no-op there.)
- **AlmaLinux 8 Docker build**: rebuild every static dep + vv itself
  with `-flto=auto`. gcc-ar / gcc-ranlib preserve LTO IR in archives
  so the final link can drop unused code across library boundaries
  (not just within each `.a`). Significant text-segment shrink vs
  the previous non-LTO 15 MB static binary.
- **AlmaLinux 8 Docker build**: install `ccache` (via EPEL) and put
  its compiler-symlink dir ahead of gcc-toolset-12 on `PATH`. A
  BuildKit cache mount (`id=vv-ccache`) persists the compile cache
  across docker builds. Requires Docker BuildKit + buildx.

### Added
- **TUI interactive-exploration batch** — three new keybindings turn
  the ncurses viewer into a real exploration tool:
  - **`S`** opens a per-column stats popup (count, nulls, min, max,
    mean, distinct count) for the leftmost visible column. Full-file
    scan with progress in the status line.
  - **`s`** sorts by the leftmost visible column. Repeated `s` on the
    same column toggles ascending / descending; `s` on a different
    column re-sorts ascending; `u` clears the sort. Numeric types
    sort by raw value, others lexicographically; nulls last. Search
    and the detail pane follow the sorted display order via a single
    `source_row(display)` indirection that routes all cache lookups
    through the sort permutation when active.
  - **`c`** opens a column show/hide picker overlay; `Space` toggles
    the column under the cursor, hidden columns drop from the
    layout, and the status bar shows a `hidden:N` indicator.
- **`--tail N`** — show the last N rows instead of the first N.
  Mirrors `-n` but operates on the tail. Reuses the
  `MemoryTableSource` adapter: every chunk is read (through any
  active `--filter`) and the last N rows are sliced off, so the
  result renders identically through every view / export mode
  (table, TUI, TSV, CSV, JSON, Markdown, Parquet).
- **`--coords UCSC|NCBI`** — pick the coordinate convention for
  `-r`. **UCSC** (default; aliases `0-based`, `bed`) means 0-based
  half-open as in BED, bigBed, bigWig, BAM and LociSSD — the
  convention introduced by Jim Kent's UCSC Genome Browser source
  tree in 2000. **NCBI** (aliases `GenBank`, `1-based`, `tabix`)
  means 1-based inclusive as in GenBank (1982), VCF, GFF, and the
  samtools / bcftools / tabix command lines. `vv -r chr1:101-200
  --coords NCBI file` matches the same rows as the UCSC default
  `vv -r chr1:100-200 file`. Conversion happens once in
  `apply_region_modifiers`; downstream sources see normalized
  UCSC. `--regions-file` is always BED (UCSC) per the spec.
- **`--parquet -`** — write Parquet to stdout. Parquet's
  footer-at-end requires seekable writes, so the data is spooled
  to an `mkstemps` temp file under `/tmp`, then streamed to stdout
  and unlinked. Bit-identical to writing to a file path. Enables
  `vv ... --parquet - | tool` pipelines.
- **Generic Parquet range queries** — `-r chr1:1000-2000 file.parquet`
  now works on any Parquet file with chrom/start/end columns, not just
  LociSSD. Chromosome / Start / End column names are auto-detected
  (common variants: `Chromosome`/`Chrom`/`Chr`, `Start`/`POS`/
  `chromStart`, `End`/`Stop`/`chromEnd`). When auto-detection isn't
  clear (or your columns have unusual names) use
  `--region-cols Chr,Start,End`. Row-group pruning uses Parquet
  ByteArray statistics on chrom plus Int statistics on Start/End;
  per-row filtering inside surviving row groups is correct for
  unsorted files too, just less efficient. Dictionary-encoded chrom
  columns are handled transparently.
- **`--validate`** — LociSSD invariants checker. Verifies the
  `lociSSD_manifest` footer parses, the required columns
  (Chromosome / Start / End / MaxEndSoFar) have the right types,
  the manifest's per-chromosome row ranges cover every row
  contiguously, rows are sorted by `(Start, End)` within each
  chromosome, `MaxEndSoFar[i]` matches `max(End[chrom_first..i])`,
  and every row's `Chromosome` label agrees with the manifest's
  window. Prints `PASS` / `FAIL` per check, caps repeat violations
  at 5 lines per category, exits non-zero on any failure.
- **`--md` / `--markdown`** — write a GitHub-flavored markdown table
  to stdout. Useful for pasting query results into issues, READMEs,
  and reports. Cells escape `|` as `\|` and turn embedded newlines
  into `<br>`. Honours `--select`, `--filter`, `-n`, `--no-header`,
  `-r`, `--sample`. Numbers keep their `_` thousands separators
  so the rendered table stays readable.
- **bigBed / bigWig support** via a vendored copy of
  [libBigWig](https://github.com/dpryan79/libBigWig) (0.4.8, MIT-licensed,
  compiled with `-DNOCURL` so only zlib is required). Source lives
  under `vendored/libBigWig/`; CMake compiles it as a private static
  library (`libbigwig.a`) and links it into `vv`. The static AlmaLinux 8
  Docker build picks it up automatically — no new external dependency
  for any distribution channel.
- bigBed: the embedded **autoSql** definition is parsed into typed
  Arrow columns (`signalValue`, `pValue`, `qValue`, …). A small
  ~150-LOC hand-rolled parser handles primitives, `char[N]` fixed-
  width strings, `enum{…}` / `set{…}` (as strings), and arrays
  (`int[N]` / `int[fieldName]` → Arrow `list<int>`).
- bigWig: rendered as four columns (`chrom`, `start`, `end`, `value`).
- Range queries (`-r chr1:78-99`) use libBigWig's overlap APIs
  (`bbGetOverlappingEntries` / `bwGetOverlappingIntervals`); no
  external index needed.
- **User manual** at `docs/USAGE.md`, with worked examples for every
  flag, every output mode, the LociSSD / tabix / BCF range-query
  flows, and a "by-workflow" cookbook section. `docs/build_docs.sh`
  renders it to self-contained HTML and PDF via pandoc (with a
  texlive → chromium-via-HTML fallback).
- **BCF range queries** — `-r chr1:100-200 file.bcf` now works, using
  `bcf_itr_querys` over an existing `.csi` / `.tbi` index. Missing
  index produces a clear hint pointing at `bcftools index`.
- **`--regions-file <BED>`** — read additional windows from a BED file's
  first three columns. Combines with explicit `-r`; both are taken.
- **`--slop N`** — pad every window by N bp on each side (start clamped
  at 0). Equivalent to `bedtools slop` inline.
- **`--stats`** — Parquet metadata dump (row groups, codecs, per-column
  compressed / uncompressed sizes, null counts) without reading any
  data. Falls back to `--schema` for non-Parquet sources.
- **`--unique COL[,COL,...]`** — distinct value counts per column.
  Top-50 values per column with overflow indicator. Honours `--filter`.
- **`--sample N`** — reservoir-sample N rows uniformly instead of
  head-N. Honours `--filter` (filter first, then sample). Combines
  with every view / export mode through a new in-memory
  `MemoryTableSource` adapter that wraps the sampled Arrow Table and
  routes it through the normal rendering pipeline.
- **`--schema`** — print the schema + file metadata footer and exit.
  Cheap, useful for inherited files.
- **`--describe`** — per-column statistics: count, nulls, min, max,
  mean (numeric), distinct count (string, capped at 16). Respects
  `--select` and `--filter`.
- **Column projection by name** — `--select Chromosome,Start,Score`.
  Unknown names produce a clear error. Works across the ASCII table,
  vertical-head, TSV, CSV, JSON, and Parquet output paths. Display
  modes still hide format-derived columns (e.g. LociSSD's
  `MaxEndSoFar`); export modes preserve the user's explicit list.
- **`--filter <expr>`** — value-predicate filter: `<col> <op> <value>`
  joined by `AND` / `OR`. Ops: `== != < <= > >=`. Values: integer,
  float, single- or double-quoted string. Evaluated per row.
- **JSON / NDJSON output** — `--json` writes a JSON array, `--ndjson`
  writes one JSON object per row. Streams chunk-by-chunk.
- **LociSSD range queries**: `-r chr1:78-99 file.lociss` (also
  `--window`). Pruning uses the embedded manifest to locate the
  chromosome's row range, Parquet column statistics on `Start` and
  `MaxEndSoFar` to skip row groups outside the window, and a per-row
  predicate (`Start < end AND End > start`) inside surviving row
  groups. Open-ended forms (`chr1:`, `chr1:78-`, `chr1:-99`) and
  comma-separated multi-windows supported. Plain Parquet without a
  LociSSD manifest is rejected with a hint.
- TUI: **help overlay** (`H` or `F1`) — centred panel listing every
  keybinding. Any key dismisses it.
- TUI: **frozen first column** (`z` toggle) — pin column 0 at the left
  edge while scrolling sideways. Useful for wide tables where the key
  column (`CHROM`, `Name`, ...) should stay visible.
- TUI: **mouse-wheel scrolling** — scroll three rows per wheel notch.
- **LociSSD support** — Parquet files carrying a `lociSSD_manifest` key
  in their footer (see [FORMAT_SPEC.md][lociss-spec]) are detected
  automatically. The derived `MaxEndSoFar` technical column is hidden
  from the ASCII table, the vertical-head view, and the interactive TUI;
  it remains intact in `--tsv`, `--csv`, and `--parquet` output so the
  data round-trips losslessly. New virtual `hidden_for_display()` hook
  on `TabularSource` provides the mechanism.

[lociss-spec]: https://github.com/balwierz/LociSSD/blob/main/FORMAT_SPEC.md

## [1.5.0] - 2026-05-05

### Added
- **BCF (binary VCF)** support via htslib (`.bcf`). Schema mirrors VCF text
  output; the existing TUI INFO-field expansion works unchanged.
- **PAF (minimap2 pairwise alignments)** support (`.paf`, `.paf.gz`). 12
  fixed columns; trailing `tag:type:value` tokens are dropped.
- **Stdin support**: `vv -` reads text formats from stdin (auto-detects
  gzip via magic bytes). Binary formats (Parquet, Arrow IPC, BAM, BCF)
  are rejected with a clear "requires seekable file" error pointing at
  process substitution as the workaround.
- **Parquet output**: `--parquet OUT` converts any supported input
  (BED/TSV/CSV/VCF/BCF/FASTA/FASTQ/BAM/…) into a Parquet file. Streams
  chunk-by-chunk so multi-GB conversions don't need to fit in RAM.
  Compression selectable via `--compression {zstd,snappy,gzip,lz4,none}`,
  default zstd.
- New `FdInputStream` helper for raw-fd InputStream wrapping (avoids
  `arrow::io::StdinStream`'s `std::cin` buffering conflicts).

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
