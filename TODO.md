# TODO — open work

Open items only, grouped. What has shipped is in CHANGELOG.md and the git
history; the "Deliberately not doing" list at the end records what was ruled
out and why, so it is not proposed again.

## Range / region

- LociSSD `lociSSD_interval_index` consumption (spec §6.5) — helps
  when the window is much smaller than a row group.

## Format gaps

Deferred until a file needs them (owner's decision, 2026-10-07).

- Delta Lake tables (`_delta_log/`): vv names one and points at DuckDB's
  `delta_scan`; reading one means replaying the log's add / remove actions
  and Parquet checkpoints to find the live files.
- NumPy arrays past 65,536 columns are a preview (kNpyMaxCols in
  `src/formats/npz.cpp`): the schema holds one Arrow field per column, and
  the table view builds every column of the rows it prints (1,455,328
  columns: 320 MB of schema, 7.9 s and 3.9 GB for `-t -n 3`). Lifting it
  needs exports and the table view that do not hold the whole schema.
- Sixel image protocol (xterm / mlterm / WezTerm-no-iterm) for the
  markdown viewer. Needs PNG → RGB decode + palette quantisation;
  ~500 LOC vendored (stb_image.h + sixel encoder).
- Markdown image fetch over HTTP (badges, hosted screenshots).
  Currently any `https://…` URL falls through to the alt-text stub.
- `.xls` (legacy binary, OLE2 compound document) — needs libxls or a
  hand-rolled OLE2 parser; biology data is overwhelmingly `.xlsx`
  today, so deferred until somebody asks.
- Apache ORC in the AlmaLinux 8 static binary — needs new
  `build-protobuf` and `build-orc` Docker stages plus
  `-DARROW_ORC=ON` in `build-arrow`. Apt / Brew / Conda Arrow already
  ship ORC, so the static-only release is the only platform where
  `vv file.orc` reports "compiled without ORC support".
- SAS (`.sas7bdat`, `.xpt`), Stata (`.dta`), SPSS (`.sav`, `.por`) via
  ReadStat (MIT).
- GFA assembly graphs (`.gfa`): segments / links / paths as tabs.
- PDB / mmCIF structures (`.pdb`, `.cif`): atom records as a table.
- mzTab (proteomics / metabolomics): the MTD / PRT / PSM / SML sections
  as tabs.
- Galaxy `.dat` / Galaxy archive — niche but visible.

## Performance / build

- **Bioconda Arrow pin** — revisit `packaging/bioconda/conda_build_config.yaml`
  (libarrow 22 only) when Bioconda's global pinning drops libarrow 19 / 20,
  the versions without `libarrow-compute`.
- **Plain gzip text decodes on one thread.** A bgzip (BGZF) input in a full
  scan is decoded on the parse threads (BgzfInputStream, htslib bgzf_mt), but
  a plain `.gz` (gzip, as GENCODE ships) can only be decoded serially:
  GENCODE CSV.gz `--count` 0.82 s vs 0.31 s for the same file uncompressed.
  Nothing to do short of a parallel inflate with an index (pigz-style
  checkpoints) built on first read.
- **Arrow IPC / Feather random access.** File sources now expose a discovered
  frontier without decoding in `chunk_meta()` and use a 64 MiB decoded-batch
  cache. Feather v2 uses this path; only v1 is eager. First discovery of batch
  row counts still decodes a prefix. A portable public metadata-only batch
  index and projected reads remain follow-on work.
- **Parquet directories:** raw `.parquet` children now supply row-group counts
  from footers and preserve projection. Other dataset formats and compressed
  children retain the streaming fallback.
- **Parquet row-group read-ahead** — measured 2026-10-28 and not done: on
  the NFS share (/genomics, 374 MB / 64 row groups / 67 M rows, page cache
  evicted before each run, output through a pipe) a cold `--tsv` export
  takes 0.86 s, a warm one 0.84 s — Arrow's pre-buffering and the threaded
  formatter already hide the I/O. A prototype that asked the kernel for
  the next row group's byte ranges (GetReadRanges + posix_fadvise WILLNEED)
  gave 0.93 s (`--tsv`), 0.58 s vs 0.68 s (`--select POS`) and no change
  (`-n 20M`): no consistent gain. Revisit only for slower storage (S3 /
  HTTP), where a one-group lookahead would need Arrow's own async reads.
- **HDF5 viewport reads:** metadata-backed projected blocks and a bounded
  dataset-handle/raw-chunk cache are implemented. Frames, 1-D arrays, dense
  and CSR expose all rows. CSC remains row-capped and matrices column-capped;
  full exports stay separate. Measure physical-chunk-aware cache tuning on
  compressed workloads before changing the fixed logical block size.
- **TUI long passes:** signal polling, visible read errors and partial/capped
  scope are implemented. Cancellation is between bounded reads, not inside
  a blocking HDF5 call.
- **Remaining reader stages:** an ODS sheet's text is still buffered whole
  for Arrow's type inference (row streaming would change the inferred
  types), and an XLSX sheet likewise. A deflated `.npz` member seeks back by
  inflating again from its start (6.5 s to the last slice of a 1 GB
  member); inflate checkpoints would bound that. Out-of-core R object
  parsing is a separate design.
- **Export quirks kept for byte-identical output:** a string cell equal to
  `∅` exports as an empty field, and an unquoted TSV / CSV field ends at its
  first NUL byte (`finish_csv_field`, `src/core/format_fast.cpp`).

## Viewer (TUI) and vvg

- Multi-line nested rendering — wrap long `list`/`map` cells across
  multiple screen rows. Punted as "medium-sized": the current TUI
  bakes row-height=1 into chunk caching, search highlighting, sort
  indirection, and scroll math. Inline expansion (this iteration)
  takes the easy win.

## Windows

- **MSIX.** An MSIX installs only when signed by a certificate the machine
  trusts. The no-cost routes: the Microsoft Store (registration is free for
  individual developers; the Store signs the package), SignPath Foundation's
  free signing for open-source projects (an application, and their
  certificate then signs the MSI too), or a self-signed certificate users
  import into *Trusted People* themselves (admin rights; poor experience).
  The MSI covers installation meanwhile.
- The ncurses viewers on Windows (MSYS2 ships ncurses with a Windows console
  driver); `VV_WITH_TUI=OFF` there for now.
- File associations in the MSI (open `.parquet`, `.h5ad`, … in vvg from
  Explorer), a winget manifest, Windows on ARM.

## Quality / signal

- **HDF5 / Cooler fuzzing stops in libhdf5.** `tests/fuzz/fuzz_hdf5.cpp`
  (h5ad, loom, h5, hdf5, cool, mcool, scool) builds with the other
  harnesses but is not in CI's fuzz smoke run: against libhdf5 2.2.0 (Arch),
  a mutated seed makes `H5Aread` of a variable-length string attribute
  (`read_string_attr`, src/formats/hdf5.cpp) loop in the global-heap code,
  so a run stops at its first timeout within seconds, and `H5Lexists`
  (cooler.cpp `exists`) can fail an assertion in `H5G__stab_lookup`. 96
  restarted runs (37,105 inputs, `-timeout=4`) found nothing in vv's own
  code. Both are library faults: an upstream report, or a CI run that
  restarts past timeouts. DuckDB has no harness: the file is parsed by
  libduckdb, and the CI fuzz build has no libduckdb.
- **ORC stripe data crashes liborc.** Malformed column data in a stripe
  makes apache-orc 2.3.1 write through a null pointer:
  `StringDirectColumnReader::next` sizes its blob from the LENGTH stream, and
  `DataBuffer::reserve` uses the default pool's `malloc` result unchecked, so
  `vv --tsv bad.orc` exits 139. Arrow's adapter passes no `orc::MemoryPool`.
  vv checks the footer, the stripe footers and decimal precision before
  liborc / Arrow parse them (`orc_check`, src/formats/arrow.cpp); checking
  the data streams would mean decoding ORC's integer RLE. An upstream report
  (ORC: check the allocation, or bound the length total by the stream size);
  until then ORC stays out of the fuzz harness (`fuzz_arrow` lists how to
  fuzz it locally).
- Code coverage badge.
- Reproducible builds.

## Audit (2026-06-10) — the one item left

- [~] `gui/arrowtablemodel.cpp:196` — Sorting/filtering loads the entire file column(s) into RAM and drains streaming sources — responsiveness fixed on `gui/threaded-filter-sort` (compute moved to a worker thread with progress + cancel; UI no longer freezes); the full-column/whole-source drain itself remains (inherent to a global sort/filter — a separate memory optimization) — deferred 2026-10-01 (needs a design for out-of-core sort/filter)
  - Fix: For sort, read only the sort column once and keep it as a ChunkedArray without forcing total materialization where avoidable;

## Deliberately not doing — do not re-propose without new information

- **Progressive SQLite row counts** — measured 2026-11-20 and not done: the
  `SELECT COUNT(*)` that `total_rows()` runs on first use takes 0.20 s for a
  20,000,000-row, 840 MB table (warm cache; `--count` 0.20 s and 21 MB,
  `-n 5` 0.18 s). Nothing for a count in progress to hide.
- **`--group-by` / `--agg`** — where vv stops being a viewer and becomes a
  query engine. `duckdb -c "select … from 'f.parquet'"` is one command away,
  and `vv in.bam --tags CB --parquet - | duckdb` reaches the only real
  differentiator without owning an aggregation mini-language forever (users
  will immediately want median/percentile/stddev/first/last). Also needs an
  unbounded hash map.
- **`--join` / `--intersect` / `--annotate` / `--per-region`** — an interval and
  relational join engine inside a single-file program. `bedtools` is installed
  wherever these files live. It dies in the long tail: `--nearest`
  tie-breaking, strandedness, upstream/downstream sign, `-d` vs `-D`, and a
  multi-overlap policy every user re-litigates.
- **`--tview` / an IGV-style locus panel** — the most tempting item on the list
  and still a different product: read stacking, CIGAR-aware base rendering, a
  reference track, zoom/pan, a second layout engine. `samtools tview` and IGV
  exist. The adjacent scope was already declined (2bit bases are deliberately
  not decoded).
- **A Python binding over libvvcore** — XL for a marginal audience; pyarrow,
  pysam and anndata already cover most of what vv reads. The blocker is not
  pybind11, it is two Arrow runtimes in one process: the release configuration
  is static and `--whole-archive`'d and cannot produce a PIC `.so` — the same
  reason the KF6 plugins are skipped in that configuration.
- **vv writing index or cache sidecars** (`--index` building `.tbi`/`.bai`, a
  `.vvi` row index, a `~/.cache/vv` metadata cache) — turns a read-only viewer
  into a file producer, duplicates tabix/samtools, and the failure users
  actually hit (plain gzip instead of bgzip) is not fixable by indexing without
  rewriting a multi-GB file.
- **A background prefetch thread in the TUI** — no `TabularSource` subclass is
  thread-safe: streaming sources mutate `batches_`/`rows_so_far_` under
  `mutable` with no lock, and htslib, HDF5 and sqlite handles are
  single-threaded per handle. A permanent class of heisenbugs, not a speedup.
- **`--sorting-columns`** and the Parquet writer-tuning flag family —
  `parquet/properties.h` warns that a sorting hint can be inconsistent with the
  data, and a lying hint makes downstream engines prune *correct* rows: a
  silent wrong-results bug in someone else's pipeline, caused by a viewer.
- **h5ad / HDF5 integrity validation** — investigated 2026-07-26 and rejected
  on evidence. Truncation is already caught by `h5ls`, `h5stat`, `anndata` and
  vv itself; corruption *inside* a dataset is caught by **none** of them,
  including a full `h5dump` and a complete `anndata.read_h5ad` (verified by
  corrupting bytes inside `X/data` — the array sum went to `nan` and every tool
  exited 0). HDF5 stores raw data with no checksum unless the writer enabled
  Fletcher32, which anndata does not. Nothing vv could add would change that.
  A narrow *semantic* check (obs/var lengths vs X dims, indptr monotonicity,
  category codes in range) is the only real gap, and vv already computes
  several of those and silently clamps them — worth doing only if someone asks,
  and it must not be advertised as an integrity check.
