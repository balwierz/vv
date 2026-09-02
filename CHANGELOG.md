# Changelog

All notable changes to `vv` are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.21.0] - 2026-09-02

### Added
- **`--sort COL[:asc|:desc]`** orders rows by one column before any output mode.
  Numeric columns sort numerically, others by their rendered text; nulls sort
  last and equal keys keep input order (stable). Materialises in memory.
- **Zstandard input.** `.zst` / `.zstd`-compressed files are decompressed on the
  fly for the delimited-text (VCF, GFF/GTF, BED and peak family, TSV/CSV,
  mpileup, PAF), JSON, and plain-text readers, and for stdin. Detected by content
  (magic bytes) for text and stdin, by suffix otherwise. FASTA/FASTQ and tabix
  range queries remain gzip-only (htslib bgzf).
- **JSON and NDJSON input** (`.json`, `.ndjson`, `.jsonl`, plus `.gz` / `.zst`).
  A top-level array of objects or newline-delimited objects parses into a table;
  records need not share a schema (fields are unioned, gaps filled with nulls),
  and nested objects / arrays become struct / list columns.
- **`--tags LIST`** (BAM/CRAM/SAM) adds one typed column per named aux tag
  (`--tags NM,AS,RG`). The column type follows the tag's SAM type code, so
  `--filter 'NM <= 2'` compares numbers; a read without the tag is null.
- **Directory / partitioned-dataset input.** `vv DIR/` concatenates the data
  files under a directory (recursively, one format: Parquet / Arrow / ORC /
  CSV / TSV / JSON), skipping `_SUCCESS` / `.crc` / hidden files. Hive-style
  `key=value/` path components become columns (an all-integer key is `int64`, so
  it filters numerically). Streaming.
- **`--contigs`** (BAM/CRAM/SAM, VCF/BCF) lists the reference sequences named in
  the header as a `name` / `length` table — reading no records — and names the
  genome assembly by the length of `chr1` (GRCh38, GRCh37, T2T-CHM13, mm39,
  mm10, and others). Composes with `--tsv` / `--json` / `--sort` / `--filter`.
- **`--distinct`** drops duplicate rows (SQL `SELECT DISTINCT`) over the shown
  columns: `--select chrom --distinct` lists the distinct chromosomes,
  `--distinct --count` counts distinct rows. Honours `--filter`.
- **`--box <style>`** picks the non-interactive table frame: `unicode` (default)
  or `ascii` (`+ - |`, with `...` truncation). Auto-selects `ascii` when the
  locale is not UTF-8, so `LC_ALL=C vv file` no longer renders the box-drawing
  characters as mojibake.
- **`--gt-stats`** (VCF/BCF) appends per-variant genotype summary columns over
  the samples — `n_called`, `n_het`, `n_hom_ref`, `n_hom_alt`, `n_missing`,
  `AC`, `AN`, `AF`, `call_rate` — a fixed set regardless of sample count.
  Diploid, haploid, and multi-allelic calls are classified; the columns are
  numeric, so `--filter 'AF > 0.05'` and `--sort AC:desc` work. Streaming.

## [1.20.0] - 2026-09-01

### Added
- **Shell completion now completes a file's columns and tabs.** In bash, zsh and
  fish, `--select` / `--cols` / `--filter` / `--expand` / `--region-cols`
  complete the real column names of the file on the command line, and `--tab`
  completes its component tabs (AnnData obs/var/X, spreadsheet sheets, NumPy
  arrays, …). Flag, value and file-extension completion are unchanged.
- **The Qt viewer (`vvg`) has a proper application icon.** It also declares its
  desktop-file identity, so on Wayland the taskbar/dock shows the icon instead
  of a generic placeholder.
- **The Qt viewer (`vvg`) can copy from the grid by right-click.** Right-click a
  cell for a menu with *Copy* (the selected cells, or the cell under the cursor)
  and *Copy Row* (every column of the row, as one tab-separated line); *Copy Row*
  is also `Ctrl+Shift+C`. A copy now takes the raw value — a big integer is
  copied as `11200`, not the grouped `11_200` shown in the grid — so it pastes
  cleanly into a spreadsheet or code.

### Fixed
- **bigWig / bigBed reads no longer stop at 32,768 intervals per chromosome.** A
  chromosome with more intervals than that was silently truncated with exit 0;
  every interval is now read.
- **Filtering a dictionary-encoded (categorical) column works.** A `--filter` on
  such a column previously matched nothing — or every row for `!=` / `not in` —
  because the value was never decoded; it now compares the decoded value, and
  `--describe` reads these columns too.
- **Headerless CSV/TSV keep leading-zero IDs.** A file with no header whose first
  column looks like `007` no longer has the zeros dropped (read as the integer
  7); the header case already preserved them.
- **Markdown rendering fixes.** Over-long words are hard-cut to the wrap width
  instead of overflowing; an unclosed inline HTML tag (`<b>` with no `</b>`) no
  longer leaks its style into the rest of the document; text after inline code
  inside a link keeps the link colour; and a hrefless `<a>` no longer emits a
  stray hyperlink escape.
- Also: NumPy `.npz` archives with duplicate member names now resolve to the
  same array `np.load` would; merged (column-spanned) ODS cells keep the later
  columns aligned; `--decode-pileup` mean quality ignores deletions and
  reference skips; malformed bigWig/bigBed data blocks are bounded against an
  out-of-bounds read; and the auto-launched TUI notes on stderr when it falls
  back to non-interactive output.

### Performance
- `--unique` on a high-cardinality column, narrow rendering of very wide list
  cells, and multi-window region queries on Parquet are all faster, with
  identical output.

## [1.19.0] - 2026-08-26

### Added
- **`-t` / `--table` is a short name for `--no-interactive`** (force the printed
  table instead of the TUI). `--no-interactive` still works.
- **The Qt viewer (`vvg`) sizes columns to their content at open.** Each column
  is fitted to the 95th percentile of its values, so identifiers that were a
  little wider than the old fixed default now show in full while the occasional
  long value still elides; a prose column is capped (50 characters) so it can't
  push the rest off-screen. Columns you resize by hand are left alone. The width
  heuristic (`plan_column_widths`) lives in the shared core, shared with the TUI.
- **The interactive terminal viewer (`vv -i`) does the same.** A string column
  is sized once to the 95th percentile of a sample of its cells (default cap
  raised to 50 characters, still overridable with `-w`), so identifiers show in
  full instead of being cut at the old 32-character default, and the width is
  stable while you scroll rather than jumping to the widest value on screen.
  Integer columns keep tracking the digits currently visible, and `,`/`.`
  resizes now stick.

### Changed
- **`--schema` and `--no-interactive` on a multi-tab file now cover every tab.**
  An AnnData `.h5ad`, an Excel workbook, a SQLite database, an HDF5 file or an
  `.npz` archive used to show only its first tab (for AnnData, the near-empty
  key/value *summary*) and hide the rest behind `+N more tab(s)`. `--schema` now
  prints each tab's schema and lists them all; `--no-interactive` previews each
  tab (a bounded number of rows). A matrix-like tab such as an AnnData `X` is
  shown with only its first columns and rows, never a full-width dump, so a file
  with tens of thousands of genes stays readable. Selecting one tab with `--tab`,
  and single-tab files, are unchanged.

## [1.18.7] - 2026-08-24

### Fixed
- **Crashes reachable from an untrusted file.** A round of hardening closed
  several ways a crafted or merely unusual file could crash `vv`, read past a
  buffer, or return a wrong answer:
  - An HDF5/AnnData enum column whose base integer type is wider than 8 bytes
    wrote past a fixed stack slot when decoded (`H5Tget_member_value`); the
    buffer is now sized from the datatype.
  - A bigWig or bigBed whose chromosome B-tree names a chromosome ID outside
    the count the file declares corrupted the heap at open; the ID is now
    bounds-checked (also fixed upstream in the vendored libBigWig).
  - `--pileup` read the base at a CIGAR-derived offset with no length bound, so
    a record with `SEQ=*` and a full CIGAR — legal SAM — over-read the sequence
    array (garbage bases, then a fault); the read is now bounded and a missing
    base renders `N`.
  - A partly-decodable Arrow IPC or ORC file crashed when the table view paged
    past a batch that failed to decode; `chunk_meta()` is now bounded, and a
    truncated ORC reports the error instead of silently dropping rows.
  - `--validate` aborted (rather than reporting) on a LociSSD file with a null
    or non-integer coordinate; it now reports it as a validation failure.
  - A LociSSD v4 file declaring its `Start`/`End` column as a non-integer type
    read past the decoded buffer; such a file is now rejected at open.

### Internal
- The test suite no longer creates a golden file from the binary under test
  when one is missing (a missing golden now fails), skipped test blocks are
  announced and counted, and the Linux CI job installs `samtools` / `tmux` so
  the pileup cross-checks and TUI checks actually run there.

## [1.18.6] - 2026-08-20

### Fixed
- **AnnData files written before anndata 0.8 showed integer codes instead of
  categories.** That encoding stores a categorical column as a plain integer
  array whose `categories` attribute is an HDF5 object reference into a
  `__categories` group beside the columns — not the modern
  `{codes, categories}` sub-group vv already understood. On a Perturb-seq
  dataset that meant `gene` read `1_157`, `gene_id` `1_542` and `strand` `0`
  instead of `NAF1`, `ENSG00000145414` and `+`; the digit-grouping formatter
  made the codes look like measurements. Both encodings now share one decoder,
  so the two files verified against each other are byte-identical across all
  310,385 obs rows and 8,563 var rows.
- **`__categories` was counted as a column.** The reserved-child skip tested
  the name `__categories__`, which anndata has never written, so `--tab
  summary` reported one column more than the file has (13 vs 12 for the same
  data in the newer encoding).
- **The obs/var preview cap applied to every mode, not just the preview.**
  `kDataFrameRowCap` (1000) exists so opening a 10 GB `.h5ad` in the TUI does
  not read 310k rows up front, but only `--tsv`/`--csv` escaped it. So
  `vv f.h5ad --tab obs --count` answered **1000** for a 310,385-row obs,
  `--unique` reported "of 1000", and — worst — `--tab var --parquet out.parquet`
  wrote **1000 of 8,563 rows**, silently truncating a format conversion.
  Every mode that produces a complete answer now reads all rows; the table and
  TUI previews stay capped, and still say so in the footer.

## [1.18.5] - 2026-08-14

### Added
- **The config file learned the terminal preferences.**
  `~/.config/vv/config` (there since 1.10, written by the TUI theme picker)
  recognised only `theme` and `scrolloff`; now also `background`
  (dark/light — what `VV_BACKGROUND` sets, permanently; it also skips the
  OSC 11 terminal query, which web consoles answer too slowly to be
  useful), `max_col_width` (the `-w` default) and `threads` (the `-@`
  default). The command line wins over the file, `VV_BACKGROUND` wins over
  `background`, and unknown keys are ignored so a config written for a
  newer vv doesn't break an older one.

### Fixed
- **Naming `--theme` on the command line silently discarded the rest of the
  config file.** The loader only ran when no theme was given, so
  `scrolloff` (and now every new key) vanished the moment a theme was
  named. The file is now always read; each key still loses to its own CLI
  flag.

## [1.18.4] - 2026-08-13

### Fixed
- **The TUI quit "by itself" in jupyter-lab web consoles**, leaving
  `11;rgb:ffff/ffff/ffff` on the shell prompt. vv queries the terminal
  background (OSC 11) before ncurses starts and waits ~80 ms for the
  answer; an xterm.js terminal proxied through a kubernetes API server
  replies slower than that, so the reply landed in the input queue after
  the TUI was up. ncurses handed its leading ESC to the Esc-quits binding —
  a clean, instant, message-less exit that looks exactly like a crash —
  and the unread tail drained to the shell (minus the `ESC]` ncurses had
  already consumed, which is why the stray text was missing its prefix).
  The key read now recognises a terminal-response introducer after a bare
  ESC (the OSC/DCS/APC/SOS/PM string sequences, and CSI replies) and
  swallows the sequence through its terminator; a real Esc, double-Esc and
  Alt+key are pushed back and behave exactly as before. Regression-tested
  by delivering that exact reply, byte for byte, into a live tmux TUI.
  On builds without the fix, `VV_BACKGROUND=dark` (or `light`) skips the
  query entirely and avoids the problem.

## [1.18.3] - 2026-08-11

Distribution release: the program is unchanged.

### Added
- **A Debian testing/sid flavor of the vv-gui `.deb`** —
  `vv-gui_<ver>-1+debianforky_<deb-arch>.deb`, built inside a
  `debian:testing` container (the suffix follows testing's codename via
  `/etc/os-release`, so it renames itself when forky releases). This
  corrects a false claim shipped in 1.18.2: the `+debian13` package does
  **not** install on forky/sid — the Qt6/KF6 names match, but the bundled
  Arrow's transitive dependencies do not. A stable-built deb pins
  `libthrift-0.19.0t64`, `libabsl20240722`, `libicu76` and `libxml2`, and
  testing has since moved to `libthrift-0.23.0`, `libabsl20260107`,
  `libicu78` and `libxml2-16` — package names that churn on every soname
  bump, which is why testing gets its own build, refreshed against current
  names at every release, rather than a promise that stable's happens to
  fit. CI builds and pristine-container install-tests both Debian flavors
  on every push.

## [1.18.2] - 2026-08-11

Distribution release: the program is unchanged.

### Added
- **A Debian 13 flavor of the vv-gui `.deb`.** The Ubuntu 24.04 package
  declares noble's shared-library names (`libqt6gui6t64`, …) in its Depends,
  which Debian does not have — Ubuntu-only by construction, and labelled so.
  Releases now also attach `vv-gui_<ver>-1+debian13_<deb-arch>.deb` for amd64
  and arm64, produced by the same distro-agnostic `build-gui-deb.sh` running
  inside a `debian:13` container (GitHub has no Debian runners; same pattern
  as the Fedora RPMs). Debian 13 packages KF6, so this flavor is the first
  `.deb` to carry the Dolphin thumbnailer / KFileMetaData plugins — and the
  only build anywhere that exercises the script's plugin-packaging path. The
  Arrow apt-repo install dance moved from the composite action into
  `scripts/setup-linux-build-deps.sh` (the action now calls it) so container
  and runner builds share one copy; along the way the dep list swapped
  `libncursesw5-dev` for `libncurses-dev`, because the transitional name was
  removed in Debian 13 — the exact trap the 1.17.0 changelog records
  INSTALL.md falling into. CI builds and pristine-container install-tests
  the Debian flavor on every push; the package also installs on current
  forky/sid, where the library package names still match.

## [1.18.1] - 2026-08-10

Distribution release: the program is unchanged; every artifact channel grew.

### Added
- **The Qt6 GUI ships as a Debian package.** Until now `vvg` existed only as
  an Arch split package or a source build; releases now attach
  `vv-gui_<ver>-1+ubuntu24.04_<arch>.deb` for amd64 and arm64. It cannot ride
  in the static CLI `.deb` — a GUI has to link the distro's shared Qt6 — so
  the package targets the release named in its version suffix. The two
  libraries Ubuntu 24.04's archive does not carry (Apache Arrow/Parquet,
  libxlsxio) are bundled privately under `/usr/lib/vv-gui` and found via
  `DT_RPATH`; every other dependency is computed from the ldd closure at
  build time, so `apt install ./vv-gui_….deb` works without any third-party
  repository. CI builds the same package on every push and install-tests it
  in a pristine `ubuntu:24.04` container — the release job is not a tag-only
  code path — and the KF6 Dolphin/KFileMetaData plugins are picked up
  automatically once the builder distro packages KF6 (Ubuntu 25.10+). The
  Arrow apt-repo install dance moved from `ci.yml` into a composite action
  (`.github/actions/setup-linux-deps`) so CI and the release job share one
  copy.
- **Fedora RPMs, CLI + GUI.** `packaging/rpm/vv.spec` had rotted at 1.9.0 —
  never built anywhere, missing half its BuildRequires (expat, minizip, zlib,
  git, pkgconf), missing the shell completions, and silent on xlsxio, which
  no distro packages. Rewritten: xlsxio is built inside `%build` as a PIC
  static archive (PIC is load-bearing — libvvcore links into the KF6 plugin
  `.so` files), and releases now attach `vv-<ver>-1.fc<NN>.<arch>.rpm` plus
  `vv-gui-<ver>-1.fc<NN>.<arch>.rpm` for x86_64 and aarch64, built by
  `packaging/rpm/build-rpm.sh` in a `fedora:latest` container. Fedora's repos
  carry Arrow, Qt6, KF6, htslib and HDF5, so unlike the Ubuntu `.deb` the
  RPMs ship no private shared libraries — xlsxio simply joins mimalloc,
  libBigWig and md4c among the statics every vv build links — and `vv-gui`
  is the first prebuilt artifact with the Dolphin thumbnailer /
  KFileMetaData plugins. CI builds and pristine-container install-tests the
  same packages on every push, and the spec's `Version:` is overridden from
  `CMakeLists.txt` at build time (and joined the release checklist) so it
  cannot rot unnoticed again.
- **`vvg` ships in the macOS tarball.** `vv-<ver>-macos-arm64.tar.gz` now
  carries the Qt6 GUI next to the CLI. Like `vv` it links the Homebrew
  libraries it was built against, so it additionally needs `brew install qt`.
  The macOS CI leg builds with `-DVV_BUILD_GUI=ON` and runs the same offscreen
  GUI checks as Linux (thumbnail/metadata cores, selftest, window path, tabix
  region re-open, background filter and find) — the GUI cannot silently rot
  on the one platform that ships it inside the main tarball.

## [1.18.0] - 2026-08-02

### Fixed
- **AnnData `obsm` / `varm` tabs were labelled with gene names.** A UMAP
  embedding rendered as `gene0 | gene1`, which reads as expression data rather
  than coordinates. The labelling helper was written for `X` — its own comment
  said so — but was called for every dense 2-D tab, and the shapes differ:
  `X` and `layers/*` are (n_obs × n_var), but `obsm/*` is (n_obs × d) and
  `varm/*` is (n_var × d), where `d` is an embedding width unrelated to the
  number of genes. `obsm[X_umap]` now shows `X_umap1` / `X_umap2`, `varm[PCs]`
  shows `PCs1…PCs3` with **gene** row labels (its rows were wrong too — they
  carried cell barcodes), and `X` / `layers/*` keep gene columns as before.
  The test fixture gained a `varm` and a `layers` entry; it had only `obsm`, so
  two of the three shapes had no test at all.
- **TUI columns other than integers never resized to their contents.** Integer
  columns were fitted to the visible rows, but everything else kept the guess
  made at setup — string 12, float 8, list 14 — regardless of the data. A
  `name` column holding two-character values sat at 12 and a `val` column of
  `0.5` at 8; on a three-column file that is 40 screen columns where 32 will
  do, and the waste scales with the number of columns. Widths now follow the
  visible rows for every type, floored by the source's `min_col_width()` and
  the type name (so `int64` does not render as `i…`) and still capped by `-w`.
  Table (`--no-interactive`) output is unchanged.

### Added
- **macOS is a tested, published target.** The "Build (macOS)" CI job had been
  green for 15+ consecutive runs, but it only ran `cmake --build` plus
  `vv --version && vv --help` — no fixtures, no tests, so not one reader was
  ever exercised there. The suite now runs on macOS in CI (691 of the Linux
  709; the rest gate themselves on tools absent from the runner), and each
  release publishes `vv-<ver>-macos-arm64.tar.gz`. Unlike the Linux tarball it
  is **not** static: it links the Homebrew libraries it was built against.

### Fixed
- **The documented macOS build command and the Homebrew formula both failed.**
  `find_package(Curses REQUIRED)` set no version floor, and Homebrew's ncurses
  is keg-only — so anything that did not name its prefix explicitly resolved to
  Apple's bundled ncurses 5.7, which lacks `set_escdelay()` (≥ 5.9) and
  `BUTTON5_PRESSED` (mouse version ≥ 2). CI worked only because it happened to
  list every prefix. CMake now stops at configure with the fix in the message,
  and `INSTALL.md` lists all seven prefixes.
- **`packaging/homebrew/vv.rb` could not have installed anything.** It was
  pinned to v1.4.0 with a placeholder all-zeros `sha256`, declared
  `pkg-config` (a 404 in current Homebrew — it is `pkgconf` now), and omitted
  `xlsxio`, `expat`, `minizip` and `hdf5`, every one a hard `FATAL_ERROR`
  dependency. Rewritten, and its URL is now covered by the version-coherence
  check so it cannot silently go stale again.
- **`setenv()` was called between `fork()` and `exec()`** in the pager path
  while Arrow's thread pool was live. It takes a libc lock and is not
  async-signal-safe, so it can deadlock on a lock another thread held at fork
  time — a hazard Apple's libc hits far more readily than glibc, presenting as
  an intermittent hang. Moved before the fork.
- **`STATIC_RUNTIME` on macOS** now errors at configure instead of failing at
  link (AppleClang has neither `-static-libstdc++` nor `-static-libgcc`), the
  unconditional `-L../static-libs/lib` no longer makes ld64 warn on every
  link, `libminizip-ng.dylib` was missing from the minizip search names, and
  the four freedesktop assets (`.desktop`, MIME XML, AppStream metainfo,
  hicolor SVG) are no longer installed on macOS, where they mean nothing.

## [1.17.0] - 2026-08-01

### Added
- **Plain-text viewing.** `vv notes.txt`, `vv server.log`, `vv README` — and
  any file no other format claims whose content sniffs as text. Previously all
  of these were the same error at exit 1 ("unrecognised file extension"), which
  made a `.txt`, a shell script, an extension-less `README` and 4 KiB of
  `/dev/urandom` indistinguishable to the user; the error's own advice
  (`cat foo.txt | vv -`) rendered prose as a one-column table with line 1
  promoted to a column header.
  - In the TUI it reads like `less -SN`: a line-number gutter, long lines
    **chopped** at the screen edge with `h`/`l` scrolling sideways and `0`
    returning home, no column header, no 32-character truncation. `/` search,
    `&` filter, `y`, `Enter`, themes and multi-file tabs all work, because a
    text file is modelled internally as one `utf8` column named `line`. The tab
    bar hides itself when only one file is open. `s`/`S`/`c`/`z` say why they
    do not apply instead of doing nothing.
  - In a pipe it is written back **verbatim**: `vv f.log > copy` is byte-for-byte
    identical, CRLF and a missing final newline included. On screen the line is
    sanitised instead — SGR colour is honoured, so a coloured log looks like
    one, but every other escape (cursor moves, OSC window-title sets) is
    dropped whole rather than executed or shown as literal `]0;…` garbage.
  - `-n N` (`0` = all), `--tail N`, `--count`, `--filter 'line contains "ERROR"'`
    and `--list-columns` work naturally; `--tail` streams through a bounded ring
    rather than slurping, so `vv --tail 100 /var/log/syslog` stays cheap. The
    column-shaped flags exit 1 with a message.
  - gzip is detected by magic, not suffix, so `syslog.1.gz` works as well as
    `notes.txt.gz`.
- **`--text`** forces text mode whatever the extension says — the escape hatch
  for reading a `.md` source or a `.csv` raw. The content is still sniffed.
- **`--expand COL`: packed `key=value` columns become real columns.** VCF
  `INFO` and GFF/GTF `attributes` carry the actual payload of those formats as
  one opaque string, so `vv variants.vcf --tsv` emitted `AF=0.5` and there was
  no `AF` column for `--filter`, `--select`, `--unique`, `--parquet` or the Qt
  GUI to see. GFF/GTF `attributes` was expanded nowhere at all — not even in
  the TUI.

  ```sh
  vv variants.vcf --expand INFO --filter 'AF > 0.05' --select CHROM,POS,AF
  vv gencode.gtf  --expand attributes --select feature,gene_name,gene_type
  ```

  Implemented as an `ExpandedSource` decorator, so every format and every
  consumer inherits it — the wrap happens once in `open_source()` rather than
  at each of its ~25 success paths. The expanded columns are appended and the
  raw column is kept, so existing column indices don't move.

  For VCF the keys and types come from the `##INFO=<...>` declarations, with
  no data scan. `Number=A/R/G/.` keys (`AD`, `PL`, …) hold one value per allele
  or genotype and deliberately stay text — typing them numeric would make
  every value null. GFF/GTF declares nothing, so keys come from the first
  chunk in first-seen order, capped at 256, with a repeated key (gencode
  repeats `tag`) yielding one column: consequently a `-n` preview and a full
  scan can legitimately disagree on the column set for the same GTF, which is
  documented rather than papered over.

  A column that does not look like a key=value list is refused, instead of
  manufacturing one column per distinct value — `parse_kv_list` treats a bare
  token as a flag, so `--expand Name` on a BED would otherwise have produced
  twenty junk columns.

  The three parsing helpers moved above the `VV_CORE_LIB` guard, where they
  belonged: they had been trapped inside the ncurses frontend, which is why
  the TUI could show INFO while nothing else could. The TUI's display-only
  expansion now stands down when the source is already expanded, so keys are
  not shown twice.


### Changed
- **Binary files are refused rather than dumped.** `vv /bin/ls` and
  `cat foo.bin | vv -` both exit 1 with an explanation and an empty stdout.
  Piped binary used to reach Arrow's CSV reader, which echoed the raw bytes
  back inside a parse error — control characters and all, straight at the
  terminal. Deliberately unlike `less`, which offers to show it anyway; a hex
  view is out of scope. UTF-16/32 gets its own message naming `iconv`, since a
  NUL-heavy Windows export is not what "binary file" means to whoever made it.
- **Install documentation now matches reality.** `README.md` and `INSTALL.md`
  advertised Bioconda, Homebrew and AUR channels that vv has never been
  published to, and the from-source dependency lists could not configure:
  Debian's line named the removed `libncursesw5-dev` and omitted five hard
  dependencies, Fedora's named `arrow-devel` / `parquet-devel` which exist in
  no Fedora or EPEL repository, and neither mentioned that `libxlsxio` is not
  packaged by any distribution. Arrow and Parquet need Apache's own apt
  repository on every current Debian stable and Ubuntu LTS. The Arch section
  now says `htslib` and `xlsxio` are AUR-only — `makepkg -s` resolves with
  `pacman`, which never looks at the AUR — and the Docker section says that
  `docker-sources/` is gitignored, so a fresh clone must run
  `scripts/fetch-docker-sources.sh` first.
- **Flat OpenDocument (`.fods`) is no longer advertised.** It is a single XML
  document, not a zipped `.ods`, so the reader could never open it — the
  registry, `--help` and the README claimed it anyway and the failure was an
  unhelpful "Cannot open as ODS (zip)". It now names the conversion command.
- **Markdown now rejects the flags it used to ignore.** A markdown file returns
  early in `main()`, before the tabular pipeline, so `--count`, `--schema`,
  `--describe`, `--stats`, `--list-tabs`, `--heatmap`, `--unique`, `--sample`,
  `--tail`, `--tab`, `--expand`, `--parquet`, `--arrow`, `--json` and
  `--pileup` never applied — but they were **silently ignored**:
  `vv --count foo.md` rendered the document and exited 0. Each is now a clean
  error, matching the exit-code work in 1.16.0. `--tsv` / `--csv` / `--select` /
  `--filter` deliberately keep working, since a markdown file can embed GFM
  tables and those flags genuinely drive them. A `--vertical` typed by the user
  is rejected; the one `vh` implies from `argv[0]` is not.

### Fixed
- **`--arrow` wrote unreadable files for most NumPy dtypes and every VCF `Flag`
  key.** `arrow_type_for_id()` mapped only `INT64`/`DOUBLE`/`BINARY` and fell
  through to `utf8()` for everything else, so the declared schema said `string`
  while the chunk carried the real array. Arrow does not check that on the write
  path: `--parquet` failed loudly, but `--arrow` exited **0** and produced an IPC
  file that neither vv nor pyarrow could read back (`buffer_index out of range`).
  This hit 7 of the 9 NumPy dtypes (`int8`/`int16`/`int32`/`uint8`/`uint16`/
  `uint32`/`uint64`/`float32`/`bool` — only `int64` and `float64` worked) and,
  through `--expand`, every VCF `Flag` INFO key: `DB`, `SOMATIC`, `IMPRECISE`,
  `VALIDATED` are `Flag` in essentially every real callset. The TUI and
  `--schema` also reported those columns as `string`.
- **`--text` was a silent no-op on markdown files.** `vv --text notes.md` — the
  literal example in the man page, `docs/USAGE.md` and the README — rendered the
  document instead of showing its source, because `main()` intercepts markdown
  before `open_source()` ever sees the flag.
- **`--text` was a silent no-op on stdin.** `cat server.log | vv --text -` still
  went to the CSV reader, which promotes line 1 to a column header and drops it
  from the data. It now matches `vv server.log` byte for byte, gzip included.
- **`--expand` did not refuse a non-`key=value` column on VCF.** The shape gate
  lived only in the key-discovery branch, and a VCF takes the declared-keys
  branch — so `--expand REF` or `--expand FILTER` appended every declared INFO
  key as an all-null column at exit 0, contrary to the man page's promise that
  such a column is refused. The `##INFO` declarations now apply only to the
  column they describe.
- **`--md` escaped the document flag gate.** On a text file it produced exactly
  the one-column `line` table the plain-text feature exists to remove; on a
  markdown file it was ignored, so `vv README.md --md > tables.md` wrote the
  rendered prose instead of the embedded tables. It is now rejected for text and
  emits the tables for markdown, like `--tsv`.
- **`vv a.md b.md` no longer shows only the first file.** On a terminal the
  multi-positional guard let markdown through to its early return, which reads
  `cfg.path` alone — so the second file was dropped silently, exit 0.
- **`vv x.md --tsv` no longer writes the rendered document into a scripted
  stream.** It emitted the prose and a caption ahead of the embedded table's
  TSV, so `--tsv > out.tsv` produced an unparseable file.


## [1.16.0] - 2026-07-27

### Added
- **`--formats`: one authoritative list of what vv reads.** The same
  information was restated in seven places — `--help`, README, `docs/USAGE.md`,
  the man page, three shell completions and four KDE manifests — and they had
  drifted. `vv --formats` prints a table with capability columns (gz variants,
  region queries, component tabs, streaming vs random access); `--formats
  --json` is the machine-readable form, and `tests/run_tests.sh` now diffs the
  completions against it, so the next divergence fails CI instead of shipping.
  A second check asserts every extension the registry claims actually has a
  dispatch branch.
- **`--schema --json` and `--count --json`.** Both flags were parsed and then
  silently ignored: `--schema` printed the human table and `--count` a bare
  number. The only structured shape was `--describe --json`, which scans every
  row — so automation reached for the most expensive mode just to learn the
  column names. `rows` is **null** when the source has not been fully scanned;
  draining a stream to produce a number would defeat the point of a cheap
  metadata mode, and `--count` is there when the number is what you want.
  Hidden columns (LociSSD's derived `MaxEndSoFar`) are reported and flagged.
- **`--list-columns` and `--list-tabs`.** One name per line, for completions
  and pipelines. `--list-tabs` is the enumerator the `--tab` error already
  printed, promoted from an error path to a success path.
- **`.npy` files open.** README has documented `.npy` since the NumPy viewer
  landed, but no dispatch branch ever existed — `vv x.npy` answered
  "unrecognised file extension". Building the registry surfaced it. A bare
  `.npy` is now wrapped as a one-entry archive and read through the same
  (fuzz-hardened) parser as an in-archive member.
- **A cell cursor in the TUI.** There wasn't one. `top_row_`/`left_col_` were
  the whole story, so every per-cell action read the top-left corner: the
  stats popup (`S`), sort (`s`), yank (`y`), the detail pane (`Enter`) and the
  width keys (`,`/`.`) all operated on whatever happened to be in that corner,
  and the help text had to say "the leftmost visible column". `h j k l` /
  arrows now move a highlighted cell and the viewport follows it, with a
  vim-style `scrolloff` (3 rows by default, settable in
  `~/.config/vv/config`). A mouse click puts the cursor on the clicked cell
  instead of scrolling that row to the top — the old behaviour shipped as a
  comment describing itself as a workaround. The cursor is per-tab, so it
  survives `Tab` switching.
- **`--filter` gained regex, substring, set and null operators.** The grammar
  was six ordering comparisons over one literal, so `--describe` could report
  a column's null count but no mode in vv could *select* those rows, and
  `FILTER != PASS` was expressible while `Chr in (chr1, chrX)` was not. Added:

  | Operator | Meaning |
  |---|---|
  | `~` `!~` | ECMAScript regex, unanchored |
  | `contains` `startswith` `endswith` | substring tests |
  | `in (a, b, c)`, `not in (…)` | set membership; numeric columns compare numerically |
  | `is null`, `is not null` | the column's actual nulls |

  One parser edit reaches all three frontends — the CLI, the TUI `&` bar and
  the Qt filter box. The word operators are operators only in *operator
  position*, so a column genuinely named `in`, `is` or `contains` stays
  filterable. A malformed regex is a parse error rather than a silently-empty
  result: in a CLI a silent degrade is worse than a hard failure. Compiled
  patterns are cached (evaluation runs per row), and `is null` is verified
  against `--describe`'s own null count as an in-tree oracle.
- **`--select` gained a pattern language.** Projection was one exact name per
  comma-separated token; on a 380-column AnnData `obs` table or a bigBed with
  autoSql extras that is a typing exercise, and there was no way to say
  "everything except". Each term can now be:

  | Term | Meaning |
  |---|---|
  | `Chromosome` | an exact column name — **always wins** over pattern interpretation |
  | `chr*`, `?_pct` | a glob (`fnmatch(3)`; `*` and `?`) |
  | `2-4`, `5-` | a 1-based inclusive index range |
  | `@numeric` | a type class: `@numeric`, `@string`, `@list`, `@bool`, `@temporal` |
  | `!TERM` | exclusion — remove everything `TERM` matches |

  Output follows the order given, so `--select End,Start` also reorders;
  duplicates collapse to their first occurrence. All six consumers inherit it,
  including `--parquet` and `--arrow`, so it is a converter feature and not
  just a viewer one. Two rules keep it from surprising anyone: an exact column
  name always wins (so `log2-ratio`, `2-4` and `!flag` stay addressable), and
  on a file whose own headers are range-shaped — binned matrices, Hi-C bins —
  a bare `N-M` is *not* reinterpreted positionally. A pattern that matches
  nothing is an error, reported separately from an unknown name: a
  silently-empty `!pct_*` typo would otherwise quietly drop columns from a
  conversion.
- **`-r` / `--region` on BAM and CRAM.** A region query used to be a silent
  no-op on alignment files: `vv reads.bam -r chr1:1000-2000` ignored the flag,
  printed the whole file and exited 0 — even when the requested contig wasn't
  in the file at all. `BamSource` now loads the index and walks htslib's
  multi-region iterator, so several comma-separated windows are covered in one
  pass, and `chr1`/`1` aliasing applies as it already did elsewhere. Verified
  byte-for-byte against `samtools view` at the window boundaries (a read
  starting on the last base is in; one ending before the first is out) and
  under `--coords ncbi`. Without an index vv now fails cleanly instead of
  quietly falling back to a full scan, and a plain `.sam` — which has no index
  — is rejected with a pointer to `samtools view -b`.
- **CRAM reference resolution via `-f` / `--fasta`.** CRAM stores bases as
  differences from a reference, so decoding needs one; htslib otherwise falls
  back to `$REF_PATH` / `$REF_CACHE`, which may be unset or a network fetch.
  `-f` now applies to a CRAM input as well as to `--pileup`. (It still errors
  on a plain BAM without `--pileup`, where it would mean nothing.) New fixture
  `tiny.cram`, encoded against the committed `tiny.pileup.fa`.
- **A warning wherever `-r` cannot be honoured.** Formats with no region index
  (Arrow IPC, ORC, FASTA, SQLite, spreadsheets, ...) print a note to stderr and
  show the whole file, instead of returning an unfiltered result that looks
  like a region query. Backed by a new `TabularSource::region_applied()`, which
  reports whether the source actually restricted its scan.
- **UCSC↔Ensembl chromosome-name aliasing for `-r`.** A `-r chr1:…` query against
  a file that names the contig `1` (or vice versa) used to silently return zero
  rows. vv now retries with the alias when the queried name isn't present but its
  counterpart is, noting the swap on stderr. Limited to **human/mouse** standard
  chromosomes (autosomes 1–22, X, Y, and the mitochondrion **`chrM`↔`MT`** — never
  `M`); scaffolds/patches/alt-contigs are never remapped. Applies to tabix
  (`.vcf.gz`/`.bed.gz`/`.gff.gz`), BCF, BAM `--pileup`, and LociSSD v4. (Generic
  Parquet and bigWig region paths are a follow-up.)

### Changed
- **Failures that used to exit 0 now exit 1.** This is a deliberate
  compatibility break: several requests vv could not honour were reported on
  stderr (or not at all) while the process still exited successfully, so a
  pipeline had no way to tell a typo or a truncated read from a clean run.
  Specifically:
  - An unknown `--select` column or an unparseable `--filter` expression now
    fails in **every** output mode. Previously only `--json`/`--ndjson`
    surfaced it; `--tsv`, `--csv`, `--delimiter`, `--md`, the ASCII table, the
    vertical head, `--parquet` and `--arrow` printed the message and exited 0.
    The message also suggests the intended column now — `unknown column(s) in
    --select: Scoree (did you mean 'Score'?)`.
  - A mid-file read error under `--json`/`--ndjson`/`--md`/`--parquet`/
    `--arrow` is reported instead of silently yielding a truncated result.
    (`--tsv`/`--csv` and the table view already did this.)
  - Passing more than one input file to a non-interactive mode is an error.
    Extra positionals become tabs in the interactive viewer; every other mode
    read only the first file and dropped the rest without a word, so
    `vv --count a.bed b.bed` answered about `a.bed` alone.
  - `--validate` is a LociSSD **v3** (Parquet) check and now says so. It used
    to run its Parquet reader against any path, so `vv x.bed --validate`
    reported "Not a valid Parquet file" — and exited 0 — while a v4
    "colblock" `.lociss` (the writer's current default, and not Parquet at
    all) got the same misleading message.
- **LociSSD v4 region queries faster.** A `-r` query decoded each candidate
  block's coordinate columns twice — once in the open-time row-count pass, then
  again when the rows were read for display/export. In region mode vv now
  memoises decoded column arrays per block, so each is decoded once. ~50% less
  time on a wide `-r … --tsv` over a 1 M-row file; output byte-identical. The
  cache is region-mode only, so a sequential full scan is unaffected.
- **Tabix range queries reuse the line buffer.** `TabixInputStream` allocated and
  freed an htslib `kstring` per record; it now keeps one buffer for the whole
  scan (htslib reallocs only when a line outgrows it). Output unchanged.

### Fixed
- **`--help` listed neither `.arrow` nor `.feather`** — two first-class
  formats, dispatched since they were added and documented everywhere else.
  Also adds the missing `.fods`, `.ffn` and `.frn`.
- **The completions were missing `.npz` and `.fods`** in all three shells;
  they had been copy-pasted once and frozen at a pre-`.npz` revision.
- **The KDE desktop entry claimed `.xls`.** vv has never supported the legacy
  binary Excel format — the man page says so explicitly — so Dolphin was
  offering `vvg` for files it cannot open. Claim removed.
- **NumPy shape sub-product overflow (found by fuzzing).** A `.npy` header can
  declare an empty array — any dimension zero — while its other dimensions are
  astronomically large. One zero makes the total element count zero, so the
  declared-size check passes for free and every later overflow guard in that
  loop becomes vacuous. But the readers still multiply *sub-ranges* of the
  shape to compute strides and slice sizes: the 3-D+ path collapses the
  trailing dimensions into one, and `(1, 1, 392361265078550784, 29, 0)` made
  that product overflow `int64` — undefined behaviour, reachable just by
  opening a crafted `.npz`, not only through the fuzz harness. The validator
  now also bounds the product of the non-zero dimensions; every sub-product
  divides it, so bounding it bounds them all. Legitimate empty arrays
  (`(0, 3)`) and ordinary 3-D arrays are unaffected. New fixture
  `tiny.shapeovf.npz`; clean over 1.24 M fuzz iterations seeded with the repro.
- **A `--select` that resolves to no columns is an error.** Every output path
  used to write an empty, zero-column result and exit 0 — `--parquet` even
  announced `[20 rows → out.parquet]` over a 0×0 file. Reachable before this
  release via `--select ','`; the pattern syntax makes it easy to hit by
  accident (`--select 'Chr,!C*'`), so it now fails and writes nothing.
- **BAM/CRAM read errors are no longer swallowed.** `BamSource` had no
  `read_status()` override, so the `ret < -1` check in its read loop was
  discarded by `ensure()` — a truncated or corrupt file produced a partial
  result with exit 0. The status is now sticky and surfaces to the CLI.
- **NumPy `.npy` zero-row Fortran-order array (found by fuzzing).** The
  per-column gather in `build_2d_table` sized its scratch buffer to
  `rows * item_size`, so an array declaring zero rows left the buffer empty and
  its `data()` null — and the Fortran-order branch then called
  `memcpy(nullptr, ..., 0)`. A zero length does not make a null argument legal
  (both parameters are declared non-null), so this is undefined behaviour, and
  UBSan traps it. Reachable from a crafted `.npz`, and from a legitimate one:
  `np.savez(f, a=np.asfortranarray(np.zeros((0, 3))))` is a valid archive.
  Guarded at `rows > 0`; new fixture `tiny.zerorow.npz` covers both memory
  orders so the ASan/UBSan CI job catches a regression without depending on the
  fuzzer reaching the same input again. Clean over 1.48 M fuzz iterations.
- **Partial full-file results are marked in the TUI.** Forward-only streaming
  sources keep only a bounded window of decoded batches, so a search, sort,
  filter or column-stats pass started after the window had already released
  batches could only see part of the file — and presented that answer as if it
  were complete. `evicted_any()` was added for exactly this case and had no
  callers anywhere. The status bar now carries `[PARTIAL]` and the column-stats
  popup gains a `Scope: PARTIAL (batches released)` row once such a pass runs.
- **`--parquet -` / `--arrow -` honour `$TMPDIR`.** Both spool through a temp
  file (the formats need seekable writes) and hardcoded `/tmp`, which fails on
  containers whose `/tmp` is tiny or read-only.
- **NumPy `.npy` parser hardening (found by fuzzing).** A new libFuzzer harness
  over the `.npy` parse-and-build path surfaced two undefined-behaviour bugs
  reachable via a crafted `.npz`: a dtype with a zero declared item size (e.g.
  `|b0`) skipped the shape-fits check and read out of bounds, and the numeric
  slab reader loaded values via an unaligned typed pointer (harmless on x86 but
  faults on aarch64). Both fixed — item size is pinned to the Arrow type's real
  element size for the bounds check, and elements are read with `memcpy`. Clean
  over 1.4 M fuzz iterations under ASan+UBSan; legitimate `.npz` reads unchanged.
- **LociSSD v4 decoder hardening (found by fuzzing).** A new libFuzzer harness
  over `decode_colblock` surfaced two undefined-behaviour bugs reachable via a
  crafted file: an empty block (`n_rows = 0`) passed a null pointer to `memcpy`
  through Arrow's builder, and the DELTA/LENGTH coordinate cumsum could overflow
  `int64` on adversarial deltas. Both are fixed (skip the zero-length append;
  accumulate in unsigned). Legitimate output is unchanged; the decoder now runs
  clean for tens of millions of fuzz iterations under ASan+UBSan.
- **A single column wider than the terminal no longer blanks the TUI.** When the
  first (or only) column was wider than the screen, the column-fitting loop broke
  immediately and drew nothing — an empty browser for a perfectly valid file. It
  now force-renders that column clamped to the available width.
- **bigWig/bigBed misaligned read (aarch64 correctness).** libBigWig read each
  interval's `chrom/start/end` via a `(uint32_t*)` cast on a pointer that advances
  by a variable-length string every iteration — an unaligned load, which is
  undefined behaviour (harmless on x86 but can fault on aarch64, which vv ships a
  static binary for). Now read via `memcpy`. Surfaced by the new ASan/UBSan CI
  gate, which rebuilds vv with AddressSanitizer + UndefinedBehaviorSanitizer and
  reruns the smoke suite — catching the out-of-bounds / misaligned class the
  binary-format parsers are prone to (verified to catch a reintroduced LociSSD v4
  DICT overflow).
- **AnnData ≥ 0.13 string columns.** anndata 0.13 changed string `obs`/`var`
  columns and the DataFrame `_index` from a plain `string-array` dataset to a
  `nullable-string-array` group (`values` + boolean `mask`). vv skipped the group
  form, so obs/var string columns, the index labels, and the X-preview cell/gene
  labels came up empty on files written by current anndata. vv now decodes it
  (applying the `mask` as nulls) while still reading the legacy dataset form.
  Covered by a version-independent hand-written fixture (`tiny.nullstr.h5ad`).

## [1.15.0] - 2026-07-02

### Fixed
- **TUI zebra stripe readable on light terminals.** The default theme's
  alternating-row background is a near-black grey that only reads as a subtle
  stripe on a dark terminal; on a light-background terminal (e.g. JupyterLab's
  web terminal) it became a hard black band that swallowed the default-foreground
  text. vv now detects the terminal background (OSC 11 query, then `COLORFGBG`,
  overridable with `VV_BACKGROUND=light|dark`) and, on a light terminal, uses a
  subtle light-grey stripe instead. Dark terminals are unchanged; detection
  failure falls back to the previous behaviour.

### Added
- **`--arrow` / `--feather` output** — write an Arrow IPC file (Feather v2), the
  zero-copy interchange format for pandas / polars / R `arrow`. Same column
  projection / `--filter` / `-` (stdout) handling as `--parquet`; streams
  chunk-by-chunk. `--compression` accepts `zstd` (default), `lz4`, or `none`
  (Arrow IPC body compression). Round-trips through vv and is read by pyarrow.
- **`--count`** — print the row count and exit. Instant on formats that carry a
  count in metadata (Parquet, LociSSD); reflects a `-r` region and, with
  `--filter`, counts only matching rows. A `wc -l` for any supported format.
- **`--describe --json` / `--describe --ndjson`** — machine-readable per-column
  statistics (the same count/nulls/min/max/mean/distinct as the text table) as a
  JSON array or one object per line, for reproducible QC in pipelines. Integers
  are exact (not `%.6g`-rounded), floats round-trippable, strings JSON-escaped;
  numeric columns omit `distinct`, all-null columns report `null` min/max/mean.
- **Reference-aware pileup — `vv x.bam --pileup -f ref.fa`.** With a reference
  FASTA (needs a `.fai`), the `--pileup` output fills the `ref` column from the
  reference and renders read bases matching it as `.` (forward) / `,` (reverse),
  byte-for-byte like `samtools mpileup -f` (vv applies no BAQ, i.e. `-B`); deleted
  bases are filled from the reference too. Without `-f` the previous behaviour is
  unchanged (ref `N`, literal bases). The current contig is fetched once and
  cached. Verified byte-identical against `samtools mpileup -B -f` (matches,
  mismatches, deletions, ref-column case). `-f` without `--pileup` is a clean
  usage error.

### Changed
- **LociSSD v4 reads faster.** The colblock `read_chunk` decoded the `Start`
  column twice whenever `End` was also present (once for the LENGTH/mask input,
  once as an output column) — the universal case for these interval files. Start
  is now decoded once and the array reused. ~40% less time on both a full
  `--tsv` scan and a `-r` region query over a 1 M-row file; output byte-identical.

### Fixed
- **LociSSD v4 reader hardening (memory safety).** The colblock decoders trusted
  on-disk offsets from the (untrusted) file: the DICT and ARENA string codecs did
  `assign(blob + o0, o1 - o0)` with no `o0 ≤ o1 ≤ blob_len` check (a non-monotone
  offset underflowed the length to ~4 GiB → out-of-bounds read), `n_dict + 1` was
  computed in 32-bit (wrap at `UINT32_MAX`), a `-r` region query on a file lacking
  Start/End indexed columns out of range, and `zstd_inflate` had no output ceiling
  (a tiny chunk could inflate to gigabytes). All now validate and reject with a
  clean error (non-zero exit), never an OOB read or OOM. Crafted-input fixtures
  (`tiny.v4.baddict/badarena/nocoord/zbomb.lociss`) guard each path.

## [1.14.0] - 2026-06-25

### Added
- **LociSSD v4.1 single-file layout.** The colblock reader now also reads v4.1
  files, where the `LSI1` index is stored **inline** in one self-contained file
  (no sidecar `.idx`), located via a 24-byte `LSIX` trailer at EOF — the writer's
  new default. Legacy v4 sidecar files still read; detection is by the trailer
  magic. Verified byte-for-byte against the Python reference reader on a real
  `write_colblock` file.
- **LociSSD v4 ("colblock") reader.** vv now reads the v4 custom binary columnar
  container (data magic `LSB1` + a sidecar `.idx` zone-map index; per-block
  zstd-compressed column chunks with the DELTA / LENGTH / DICT / FRONTCODE /
  ARENA / RAW / BOOL codecs), alongside the existing v3 Parquet `.lociss` —
  dispatched by magic. Site-level reading (TUI + non-interactive + `--tsv`/etc.),
  the assembly/species/element-count banner, and **`-r` region queries** pruned
  via the index zone-map. The optional genotype matrix is not read (a site-level
  reader is spec-conformant). Validated byte-for-byte against the Python
  reference reader.
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
  in their footer (see the LociSSD format specification) are detected
  automatically. The derived `MaxEndSoFar` technical column is hidden
  from the ASCII table, the vertical-head view, and the interactive TUI;
  it remains intact in `--tsv`, `--csv`, and `--parquet` output so the
  data round-trips losslessly. New virtual `hidden_for_display()` hook
  on `TabularSource` provides the mechanism.


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
