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
- Region-mode exact row counts (`fix/region-rowcount`) — `total_rows()` /
  `chunk_meta()` for generic-Parquet / LociSSD regions now report the
  post-filter count (computed by running the overlap predicate once per slice
  at open), instead of the pre-filter slice size. Fixes the phantom trailing
  rows in the table view and the trailing blank row in the Qt GUI.

## Output / pipelines

### Done
- JSON / NDJSON output (`--json` array, `--ndjson` per-row).
- Markdown output (`--md` / `--markdown`).
- `--stats` — Parquet metadata footer dump without reading data.
- `--validate` — LociSSD invariants check.
- Parquet to stdout (`--parquet -`, temp-file spool).

## TUI

### Open
- Marks & recall — vim-style `m{a-z}` / `'{a-z}`.

### Done
- Stats popup (`S`) — count/nulls/min/max/mean/distinct for the
  active column.
- Sort by column (`s` toggles asc/desc; `u` clears).
- Column show/hide picker (`c`).
- Live filter (`&`) — same grammar as `--filter`; hides
  non-matching rows; status bar shows kept/total; composes with
  sort via the same `source_row(display)` indirection.
- Copy cell (`y`, OSC52) — copies the top-left visible cell to
  the system clipboard via an OSC52 escape; works over SSH /
  tmux 3.3+ without external clipboard helpers.
- Multi-file tabs — `vv a.vcf b.bed …`, `Tab`/`Shift+Tab` cycle;
  each tab keeps its own sort/filter/scroll/cache.
- Command line (`:` key) — `:N` jumps to row, `:q` quits,
  `:theme NAME` switches theme. Extensible verb list.

## Format gaps

### Open
- Markdown TUI mode — current `vv README.md` emits ANSI to stdout
  (great for `vv README.md | less -R`). A proper scrollable
  ncurses-based TUI integrated into `TableTUI` as a new `TabKind`
  would let prose + GFM tables share a single multi-tab session and
  pick up the existing `/`-search infrastructure. ~200 LOC of glue
  on top of the renderer + TabState already in place.
- Sixel image protocol (xterm / mlterm / WezTerm-no-iterm) for the
  markdown viewer. Needs PNG → RGB decode + palette quantisation;
  ~500 LOC vendored (stb_image.h + sixel encoder).
- Markdown image fetch over HTTP (badges, hosted screenshots).
  Currently any `https://…` URL falls through to the alt-text stub.
- `vv x.bam --pileup -f ref.fa` — reference-aware pileup. The
  current `--pileup` walker matches `samtools mpileup` without `-f`
  (ref always `N`, no `.`/`,` match notation). Plumbing a FASTA in
  via htslib's `faidx_fetch_seq` is mechanical; main cost is
  threading the FASTA path through the Config.
- `.xls` (legacy binary, OLE2 compound document) — needs libxls or a
  hand-rolled OLE2 parser; biology data is overwhelmingly `.xlsx`
  today, so deferred until somebody asks.
- Apache ORC in the AlmaLinux 8 static binary — needs new
  `build-protobuf` and `build-orc` Docker stages plus
  `-DARROW_ORC=ON` in `build-arrow`. Apt / Brew / Conda Arrow already
  ship ORC, so the static-only release is the only platform where
  `vv file.orc` reports "compiled without ORC support".
- AnnData `uns` (unstructured) decoding — nested groups / scalars /
  free-form arrays. v1 skips. A follow-up could surface scalars and
  string entries using the existing hierarchy-table machinery.
- AnnData CSC sparse preview — needs per-column indptr walking
  rather than per-row. v1 only handles CSR; CSC files show a summary
  but no value preview.
- Cooler (`.cool`, `.mcool`) Hi-C contact matrices — HDF5 backbone
  but very different layout (bins / pixels / chroms). Worth a
  follow-up dedicated source class on top of the existing HDF5
  plumbing.
- Galaxy `.dat` / Galaxy archive — niche but visible.

### Done
- AnnData X preview labelled with obs / var identifiers (`feat/anndata-x-labels`)
  — X is (n_obs × n_vars) = cells × genes, so the sparse / dense X preview now
  names its value columns by the var index (gene names) and prepends the obs
  index (cell barcodes) as a row-label column, instead of generic col0/col1
  and bare row numbers. `apply_anndata_x_labels` reads the sibling /var and
  /obs `_index` datasets; no-op for non-AnnData HDF5 (Dataset2D untouched).
- samtools mpileup (`.pileup` / `.mpileup` / `.pile`, plus `.gz`) —
  routed through `DelimKind::Mpileup`. Tab-count on the first row
  infers single- vs multi-sample; columns get named `chrom` / `pos`
  / `ref` / `depth[_i]` / `bases[_i]` / `quals[_i]`. Range queries
  via the existing TabixInputStream path.
- `--decode-pileup` — typed per-allele view (A/C/G/T/N + ins/del +
  fwd/rev + mean_qual) replaces the packed `bases` / `quals`
  columns. Hand-rolled state machine for the pileup bases language
  (matches against ref, mismatches against literal base, indel
  markers, deletion placeholders, mapq-after-`^`).
- `vv x.bam --pileup` — BAM/CRAM-to-mpileup on the fly via htslib's
  `bam_plp_auto`. Byte-identical to `samtools mpileup x.bam` (no
  `-f`). Region queries trim emitted positions to the requested
  span just like samtools.
- OpenDocument Spreadsheet (`.ods`) — hand-rolled reader on minizip +
  expat (already in the tree for xlsxio's deps). Reuses the
  WorkbookSource framework introduced for Excel and the shared
  `csv_buffer_to_table` helper.
- Apache ORC (`.orc`) — opens via Arrow's ORC adapter. One stripe is
  one chunk; OrcSource mirrors IpcSource's lazy-load pattern. CMake
  detects the adapter header at configure time and gates the source
  with `VV_HAVE_ORC`.
- Excel (`.xlsx`, `.xlsm`) — opens via libxlsxio; each sheet becomes
  a TUI tab through the new WorkbookSource abstraction (extends
  MemoryTableSource). Cell text streams through Arrow's CSV reader
  for type inference. `.xls` deferred.
- bigBed / bigWig — vendored libBigWig (`-DNOCURL`, no libcurl
  dep); autoSql parsed into typed Arrow columns; range queries
  via libBigWig overlap APIs.
- 2bit — UCSC sequence container; hand-rolled parser reads only
  the per-sequence index (name / length / n_blocks /
  mask_blocks). DNA bases are not decoded — chromosome-scale
  references would blow up RAM; `twoBitToFa` is the right tool
  for that.
- ENCODE peak / signal text formats — `.narrowPeak`, `.broadPeak`,
  `.gappedPeak`, `.bedGraph` / `.bg`, `.tagAlign` (plus `.gz`)
  routed through DelimKind::BED with variant-aware column naming
  (signalValue / pValue / qValue / peak / value / sequence).
- SQLite (`.sqlite` / `.sqlite3` / `.db`) — read-only; each user
  table becomes a TUI tab. SqliteSource shares the sqlite3 handle
  across sibling sources via shared_ptr. Type affinity drives the
  Arrow schema.

## Performance / build

### Done
- ARM64 static binary in the release workflow — the release CI
  matrices over `ubuntu-latest` (x86_64) and `ubuntu-22.04-arm`
  (aarch64); both arches are published with every tag as
  `vv-<ver>-linux-<arch>.tar.gz`.
- LTO build — every static dep + vv built with `-flto=auto` via
  CMake `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`; gcc-ar / gcc-ranlib
  preserve LTO IR in archives. Significant text-segment shrink
  vs the previous non-LTO 15 MB static binary.
- Parquet decompressor thread-pool tunable — `--decode-threads N`
  separately sizes Arrow's CPU thread pool (Parquet column decode
  / CSV parallel parsing) without touching htslib's thread count.
  Defaults to `--threads`; bounded at `2 × hardware_concurrency()`.
- `ccache` in the AlmaLinux 8 Dockerfile — installed via EPEL,
  symlinks added to PATH so every `gcc` / `g++` invocation gets
  wrapped; BuildKit cache mount (`id=vv-ccache`) persists the
  cache across docker builds.

## Convenience

### Open
- Multi-line nested rendering — wrap long `list`/`map` cells across
  multiple screen rows. Punted as "medium-sized": the current TUI
  bakes row-height=1 into chunk caching, search highlighting, sort
  indirection, and scroll math. Inline expansion (this iteration)
  takes the easy win.
- User-defined themes in `~/.config/vv/config` — would let the user
  override individual palette entries (e.g. `border = 38;5;238`)
  rather than picking from the five built-ins. The XDG config
  plumbing is already in; this is just a palette-override loader.
  Deferred until someone asks.

### Done
- Color themes — `--theme NAME` selects from `default`, `dark`,
  `light`, `solarized-dark`, `solarized-light` (built-in palettes
  covering both the ASCII table and the TUI; 256-color with 16-color
  fallback).
- Smarter inline list/map truncation — `truncate()` walks every
  top-level comma and picks the largest leading-element prefix
  that fits the column, instead of always dropping to
  `[first, …]` after the first comma.
- TUI theme picker (`T`) — overlay; saves to
  `$XDG_CONFIG_HOME/vv/config` (default `~/.config/vv/config`).
  Future runs pick up the saved theme automatically.

### Done
- `--tail N` — last-N rows; reuses the `MemoryTableSource` adapter.
- `--coords UCSC|NCBI` — UCSC (0-based half-open) vs NCBI (1-based
  inclusive); conversion happens once in `apply_region_modifiers`.

## Quality / signal

### Open
- Sanitizer CI job (ASan + UBSan). **[elevated — see Audit findings below;
  this catches the critical memory-safety class automatically.]**
- libFuzzer harness for the binary parsers (BAM, Parquet, BCF,
  FASTA/FASTQ). **[extend to HDF5/NPY/NPZ/2bit/ODS — the parsers the
  audit found unguarded.]**
- Code coverage badge.
- Reproducible builds.

## Audit findings (2026-06-10)

Multi-agent code audit of the reader core, TUI, GUI, and KDE plugins
(96 issues, adversarially verified). Severity: **critical** = crash /
memory-safety / data-corruption on plausible input; **high** = wrong
results or a major perf cliff; **medium** = noticeable; **low** = minor.
Locations are line numbers as of the audit; re-grep before editing.
The systemic prerequisite — ASan/UBSan CI + a libFuzzer harness over the
parsers — is already tracked under **Quality / signal → Open** and should
be done first: it catches this whole bug class automatically.

### Critical (5) — fix before next release

- [x] `main.cpp:3286` — Region coordinates are 0-based when handed to htslib's 1-based region parser (off-by-one start on every tabix/BAM/BCF range query) — fixed on `fix/region-coord-offbyone`
  - Fix: Pick one convention and convert at the boundary. Since cfg.region is canonicalised to 0-based half-open, convert back to 1-based inclusive (start+1, end unchanged) when building the region STRING handed to any…
- [x] `main.cpp:6202` — Buffer overflow in read_string_attr for array-valued string attributes — fixed on `fix/hdf5-attr-overflow`
  - Fix: Query H5Aget_space + H5Sget_simple_extent_npoints and bail out (return "") if npoints != 1, or size the destination buffer to npoints.
- [x] `main.cpp:6789` — Stack/heap buffer overflow reading HDF5 'shape' attribute into a fixed 2-element buffer — fixed on `fix/hdf5-attr-overflow`
  - Fix: Before reading, query the attribute's dataspace size: open the space with H5Aget_space, get H5Sget_simple_extent_npoints, and only read into a 2-slot buffer when the count is exactly 2 (or read into a…
- [x] `main.cpp:7216` — NPY shape parsing accepts negative dimensions -> size_t wrap and huge allocation/OOB — fixed on `fix/npz-shape-validation`
  - Fix: Reject any shape component < 0 (and treat the whole header as invalid). Validate each dimension is in [0, sane_max] before storing.
- [x] `main.cpp:7457` — NPZ/NPY: no bounds check that declared shape fits the array body — out-of-bounds read on malformed input — fixed on `fix/npz-shape-validation`
  - Fix: After parsing the header, compute the required element count with overflow-checked multiplication and verify `data_offset + total_elems*item_size <= bytes->size()`.

### High (24)

**Correctness bugs**
- [~] `gui/kde/thumbrender.cpp:25` — KDE plugins call libvvcore (which uses .ValueOrDie()) with no exception/abort guard — malformed file can crash the worker — `gui/improvements` wraps both cores in function-try-blocks (catches C++ exceptions; .ValueOrDie() aborts remain, but the source readers now validate untrusted input up front)
  - Fix: Wrap the body of vv_render_thumbnail and vv_probe_meta in try/catch(...) returning an empty QImage / unset VvMeta on any exception.
- [x] `main.cpp:2897` — Region queries on Parquet return empty/wrong results when Start/End are not Int32/Int64 — fixed on `fix/parquet-region-int-types` (cell_int handles all integer widths + dictionary-of-int)
  - Fix: Handle the remaining integer Arrow types (Int8/16, UInt8/16/32/64) and dictionary-encoded indices, or use arrow's scalar visitor;
- [x] `main.cpp:3448` — Malformed delimited row past the first block silently truncates output with exit code 0 — fixed on `fix/delimited-silent-truncation` (sticky read_status + non-zero exit; also closes the latent ensure() hang)
  - Fix: Store a sticky arrow::Status member set by advance() on error (and set all_read_=true to stop the loop), then surface it from read_chunk()/total_rows() and from ensure() callers so the CLI prints the CSV parse…
- [x] `main.cpp:3920` — Pileup engine does not handle is_refskip (wrong output for spliced/RNA-seq reads) — fixed on `fix/pileup-refskip` (refskips render as '>'/'<' by strand, not '*'; the quality column now carries the real base quality at qpos for del/refskip too, matching samtools). Regression fixture tiny.splice.bam + byte-for-byte samtools-equivalence test.
  - Fix: Add `else if (p->is_refskip) { bases += bam_is_rev(p->b) ? '>' : '<'; quals += (char)('!'); }` before the base-call branch, matching samtools mpileup.
- [x] `main.cpp:4224` — BcfSource drops the FORMAT field from FORMAT_SAMPLES (data loss) — fixed on `fix/bcf-format-field`
  - Fix: After the loop, `start` already points at the FORMAT field. Set `f[8] = line.substr(start);` (the entire remainder from FORMAT onward) for the fi==8 case, and drop the misnamed `info_end` re-scan entirely.
- [x] `main.cpp:4346` — Region coordinate-convention mismatch: BCF/BamPileup pass 0-based half-open coords to 1-based-inclusive htslib parsers — fixed on `fix/region-coord-offbyone`
  - Fix: When rebuilding the region string for a 1-based-inclusive htslib parser from canonical 0-based half-open coords, convert: start_1based = start0 + 1, end stays (inclusive end == half-open end).
- [x] `main.cpp:4906` — Unbounded reserve(seq_count) on attacker-controlled 2bit header field aborts the process (OOM/bad_alloc DoS) — fixed on `fix/twobit-bounds` (validate seq_count against file size + cap the reserve)
  - Fix: Validate seq_count against the file size before reserving (each index entry needs at least 1+1+4 bytes), or cap the reserve (e.g.
- [x] `main.cpp:5044` — SQLite columns with NUMERIC/DATE/DATETIME/BOOLEAN/unknown affinity are silently coerced to double, corrupting dates and losing 64-bit integer precision — fixed on `fix/sqlite-type-affinity` (NUMERIC affinity → string, lossless)
  - Fix: Default the unknown/NUMERIC/DATE branch to STRING (or decide per-value via sqlite3_column_type at read time, mapping SQLITE_INTEGER->int64, SQLITE_FLOAT->double, SQLITE_TEXT->string), so dates and…
- [x] `main.cpp:5398` — ensure() infinite-loops (hangs) when a streaming source's advance() returns an error without setting all_read_ — fixed on `fix/streaming-ensure-hang` (IpcSource/FastxSource/SqliteSource set sticky read_status_ + all_read_ on error; surfaced via [[main.cpp:3448]]'s read_status())
  - Fix: Make ensure() break out on any error: have load_next_ipc()/advance() set all_read_=true (or a separate failed_ flag) before returning a non-OK status, and/or have ensure() capture the returned Status and break…
- [x] `main.cpp:5684` — Workbook rows wider than the first row cause the whole sheet to fail (XLSX & ODS) — fixed on `fix/workbook-ragged-rows` (shared assemble_ragged_csv pads every buffered row to the widest; the header's overflow columns get synthetic colN names; both xlsx_sheet_to_table and the ODS parser refactored to buffer rows first; fixtures tiny.ragged.xlsx/.ods + 6 assertions)
  - Fix: Compute the maximum column count across all buffered rows (XLSX: track max col; ODS: track max emitted) and pad every row to that width before invoking Arrow, instead of locking the width to the first row.
- [x] `main.cpp:5928` — ODS table:number-rows-repeated on non-empty rows silently drops data — fixed on `fix/ods-rows-repeated` (ods_start parses number-rows-repeated; row close pushes the assembled non-empty row N times, capped at 16384; empty repeated rows still collapse via the trailing-empty trim; fixture tiny.rowrep.ods + 2 assertions)
  - Fix: Parse table:number-rows-repeated in ods_start (with a sane cap), and at row close emit the assembled row line N times (skip when the row is entirely empty so trailing blank runs still collapse).
- [x] `main.cpp:6472` — Dense AnnData/HDF5 2-D matrix is fully densified into RAM with no row or column cap (OOM) — fixed on `fix/anndata-dense-oom` (read_2d_dataset_table now reads only the first 1000 rows × 200 cols corner hyperslab; the true dimensions are reported in the footer as a "preview: first N of M" note; regression fixture tiny.dense.h5ad + GUI CI assertion)
  - Fix: Pass a sane row_cap for Matrix2D/Dataset2D (mirroring the sparse 1000-row preview, or honoring cfg.head_rows), and cap n_cols for dense matrices the way scan_generic already caps Dataset2D at dims[1]<=32 and…
- [x] `main.cpp:7295` — build_2d_table builds one Arrow column per declared cols — unbounded column count is an OOM/DoS — fixed on `fix/npz-column-cap` (build_2d_table caps rendered columns at kNpzMaxCols=4096 while keeping the full declared width as the C-order row stride; the 2-D and 3-D-slice call sites append a "showing first N of M columns" footer note via a new full_cols_out out-param; mirrors the dense-HDF5 [[main.cpp:6472]] col cap; fixture tiny.wide.npz (5000 cols) + footer/overflow assertions)
  - Fix: Clamp the number of rendered columns to a sane maximum (e.g. a few thousand) and surface a 'array too wide to display, showing first N columns' footer, mirroring how other wide sources are handled.
- [x] `main.cpp:7499` — Integer overflow in element/byte-count multiplications for NPY arrays — fixed on `fix/npz-shape-validation` (product is overflow-checked and bounded to the buffer)
  - Fix: Use checked multiplication (e.g. __builtin_mul_overflow or compare against (SIZE_MAX/item_size)) when computing every element-count and byte-offset;
- [x] `main.cpp:9852` — Inf values in a float column poison heatmap normalization (NaN -> lround UB, blank plot) — fixed on `feat/heatmap` (non-finite cells treated as gaps: excluded from min/max and from lround; all-non-finite matrix errors gracefully)
  - Fix: Treat non-finite values like missing: in the scan use `if (!ok || !std::isfinite(d)) d = std::nan("");` (only update lo/hi for finite d), and in the pixel loop test `if (!std::isfinite(d))` instead of just…
- [x] `main.cpp:13038` — TUI sort / filter / stats / search silently operate on only the loaded prefix of a streaming source — fixed on `fix/tui-streaming-prefix` (shared drain_to_eof() helper, mirroring the `G` handler, called at the head of rebuild_display_order / compute_stats_for / find_next; the `G` handler now reuses it). Verified with a 3-block CSV: at op time only 2 of 3 chunks were loaded; the drain pulls in the third so the op covers the whole file.
  - Fix: Before these full-file passes, drain streaming sources to EOF (the same `while (src_->total_rows() < 0) src_->ensure(src_->num_chunks());` loop used by the `G` handler, with a 'Loading…' status), then re-read…
- [x] `main.cpp:14579` — --heatmap emits raw terminal escape sequences with no isatty guard (corrupts pipes/files) — fixed on `feat/heatmap` (a new `ascii` mode is auto-selected when stdout is not a TTY and the backend is `auto`; emits a plain intensity grid)
  - Fix: When stdout is not a TTY and image_mode is empty/auto, either error out ("--heatmap requires a terminal; pick --image-mode explicitly") or downgrade to a plain ASCII intensity grid.

**Performance**
- [x] `gui/arrowtablemodel.cpp:78` — chunkForRow() is O(num_chunks) per cell — quadratic-ish scan on every cell access/repaint — fixed on `gui/improvements` (cumulative first-row offset table + std::upper_bound, O(log chunks))
  - Fix: Maintain a sorted vector of cumulative chunk first_row offsets and binary-search it (std::upper_bound) to map row->chunk in O(log chunks). Rebuild/extend it lazily as chunks are discovered.
- [~] `gui/arrowtablemodel.cpp:196` — Sorting/filtering loads the entire file column(s) into RAM and drains streaming sources — responsiveness fixed on `gui/threaded-filter-sort` (compute moved to a worker thread with progress + cancel; UI no longer freezes); the full-column/whole-source drain itself remains (inherent to a global sort/filter — a separate memory optimization)
  - Fix: For sort, read only the sort column once and keep it as a ChunkedArray without forcing total materialization where avoidable;
- [x] `gui/arrowtablemodel.cpp:284` — findNext() is a full O(rows*cols) linear scan that decodes every cell on the GUI thread — fixed on `gui/threaded-find` (scan moved to a worker thread with progress + cancel; string columns vectorized with Arrow match_substring as a candidate prefilter, Qt regex remains the source of truth; navigation is now O(log matches) over a precomputed, sorted match list)
  - Fix: Search column-by-column over already-loaded chunks using Arrow compute (e.g. match_substring/MatchSubstringRegex) to vectorize, or scan in a background thread with a cancellable progress indicator.
- [x] `main.cpp:1463` — Preamble strippers read one byte at a time, allocating an Arrow Buffer per byte — fixed on `perf/preamble-strippers` (all four strippers now read through the buffered LineReader and hand the first data line + look-ahead back via put_back/PrependInputStream instead of Seeking; the seekable/non-seekable paths are unified, read_stream_line is gone). ~110× faster on a 4.7 MB VCF header (2.67s → 0.024s); format goldens unchanged.
  - Fix: Use the existing buffered LineReader for the non-seekable case, or read in chunks into a local buffer. For the seekable case, capture Tell() at the start of each line and Seek() back to the first data line as…
- [x] `main.cpp:11697` — Sorted TUI search re-decodes a whole Parquet row group on nearly every row — fixed on `perf/tui-sorted-search` (the sorted find scan now routes each row through the shared LRU chunk cache via ensure_cols / cache_ instead of a single last_chunk slot, so a sort that bounces between a handful of row groups decodes each at most once per eviction rather than once per row; factored a shared cached_row_matches helper reused by the highlight check, and throttled the now-cheap progress repaint to every 8192 rows)
  - Fix: Reuse the TableTUI chunk cache (ensure_cols / cache_) inside the sorted find path instead of a single last_chunk slot, and/or add a small decoded-row-group LRU inside ParquetSource::read_chunk keyed by…
- [x] `main.cpp:13336` — Every visible cell is formatted at least twice per redraw — fixed on `perf/tui-cell-cache` (the width-fitting pass now formats each visible integer cell through the same cell_at path the renderer uses, memoizes it in a per-frame frame_cells_ map keyed by frame_key(source_row, virt_col), and draw_data_row reuses it instead of re-formatting — so integer columns, the common Start/End genomic case, format once per redraw instead of twice; the memo is cleared each draw() and bounded by the visible cell count. Also fixed a latent bug where the fit pass treated display rows as source rows under an active sort (now uses source_row()), and the integer width now matches what's painted because it runs through format_cell too. Verified byte-identical TUI frames vs main across static/sort/scroll/col-scroll/goto-bottom/filter on parquet/vcf/bed/lociss.) Cross-frame caching was considered but deferred: it requires invalidating on max_col_w_/theme/sort/filter/column-hide/VCF-expansion changes, and the per-frame single-format already removes the redundant work in the scroll hot path.
  - Fix: Memoize formatted cell strings per (chunk,row,virt_col) in the cache (invalidated on max_col_w_ / format changes), or compute integer column widths from the fitting pass and reuse those same strings when…
- [x] `main.cpp:13865` — Pressing G (or scrolling deep) on a huge streaming file loads the entire file into RAM with no eviction — all seven forward-only streaming sources now keep a bounded trailing window of decoded batches (DelimitedSource on `perf/streaming-batch-eviction`; BamSource, BamPileupSource, BcfSource, BigSource, FastxSource, SqliteSource on `perf/streaming-eviction-rest` via the shared `stream_retain` helper). Default window 64 batches, env VV_STREAM_BATCH_CAP; per-batch first_row/num_rows metadata is retained forever so total_rows()/chunk_meta()/num_chunks() stay exact; read_chunk() of an evicted batch returns CapacityError → the TUI renders those rows blank. Sequential single-pass consumers — export/--tsv/--csv/--json/--md, --unique, table view, and the TUI forward-browse + G — stay correct AND become bounded. Full-pass consumers that drain-then-reread (find_next search, compute_col_stats, filter_rows, rebuild_display_order sort/filter) call set_retain_all(true) first, so they keep their current behaviour (correct; same RAM as before — no regression). Forced-eviction (VV_STREAM_BATCH_CAP=1) export/unique-completeness regression tests on TSV and FASTQ. The forward-only nature means search/sort/back-scroll over a fully-browsed huge stream is limited to retained data — the accepted tradeoff of the RAM-ring approach (no temp files / no re-open).
  - Optional follow-up: IpcSource/OrcSource are random-access files (re-read on miss via ReadRecordBatch(i)/load_stripe(i)) yet still retain every decoded batch — they could bound with zero correctness cost (transparent re-read instead of blank) as a separate, strictly-better variant.
  - Fix: Give streaming sources a bounded ring of retained batches (evict batches outside a window, mirroring the chunk cache) so deep scrolling and `G` don't grow without bound;
- [x] `main.cpp` — `--describe` / `print_describe` reported stats over only the first ~10 rows (Config::head_rows defaults to 10, the table-preview size, and describe was called with the raw cfg — unlike --json/--md/--parquet which reset it to 0 when -n wasn't given) — fixed on `fix/describe-full-scan` (rows_left now defaults to INT64_MAX unless cfg.head_rows_set, so --describe summarises the whole table and `--describe -n N` still caps the scan; describe reads sequentially so it stays bounded on streaming sources). Regression tests describe_scans_all_rows (tiny.parquet → Count 20) and describe_honors_explicit_head_rows (-n 5 → Count 5).

### Medium (39)

**Bugs**
- [ ] `gui/arrowtablemodel.cpp:117` — sourceTotal()/viewRows() truncate to 32-bit int via rowCount() clamp, but vertical-header and footer report from the clamp boundary
- [ ] `main.cpp:865` — display_width() counts codepoints, not terminal columns — wide/combining/zero-width chars misalign the table
- [x] `main.cpp:912` — truncate() byte-based fallback splits a multibyte UTF-8 codepoint, emitting invalid UTF-8 — fixed on `fix/truncate-utf8` (new utf8_prefix_bytes() helper cuts the fallback on a codepoint boundary, keeping max_w-1 whole codepoints + ellipsis; consistent with display_width's codepoint count. The collection branch already cut at ASCII comma boundaries. Tests: iconv round-trip + ééééé… prefix). Note: [[main.cpp:865]] (display_width counts codepoints, not terminal columns — wide/combining chars) is still open; truncate now matches display_width's codepoint semantics, so they stay consistent.
- [ ] `main.cpp:2628` — Generic Parquet region pruning uses Arrow field index as a Parquet leaf index
- [~] `main.cpp:3070` — advance() error is swallowed at every call site, so I/O/parse errors never reach the user — partly addressed by [[main.cpp:3448]] (a `read_status()` accessor now surfaces it on the delimited + table output paths); `--describe`/`--unique` and the TUI status bar still don't check it
- [ ] `main.cpp:3306` — Header detection misclassifies real headers named like numbers (nan, inf, 1e5, hex)
- [ ] `main.cpp:3963` — Pileup treats mid-stream read errors as clean EOF
- [ ] `main.cpp:4185` — BCF region-mode silently swallows read errors (truncated/corrupt file -> partial output)
- [x] `main.cpp:4949` — 2bit N-block table skip computes n_block_count*8 in 32-bit arithmetic, overflowing and seeking to the wrong offset — fixed on `fix/twobit-bounds` (compute the skip in 64-bit)
- [x] `main.cpp:6778` — AnnData DataFrame builds tables from columns of unequal length without validation — fixed on `fix/anndata-validation` (read_anndata_dataframe normalises every column to the longest length — pads short ones with trailing nulls via MakeArrayOfNull+Concatenate, slices over-long ones — and passes the explicit row count to Table::Make, so a malformed file can't produce an invalid table / OOB paging)
- [x] `main.cpp:6818` — Sparse preview trusts the 'shape' attribute for n_rows without validating against indptr length — fixed on `fix/anndata-validation` (read_sparse_preview clamps n_rows to the real indptr extent (h5_len_1d), clamps the nnz read window to the actual indices/data extents, and bounds-checks the per-row scatter offsets so a non-monotonic / lying indptr can't read out of range; shape values are floored at 0). Shared hostile fixture tiny.badsparse.h5ad (shape claims 100 rows, indptr has 2; unequal obs columns) + no-crash/clamp/normalise tests.
- [ ] `main.cpp:7410` — NPZ entry names assumed unique; duplicate .npy members silently shadowed and a wrong array can be displayed
- [ ] `main.cpp:8090` — wrap_runs never hard-cuts over-long words — they overflow the wrap width (contradicting its own doc comment)
- [ ] `main.cpp:8396` — role is a single value, not a stack — nested spans that both set a role lose the outer role on inner close
- [ ] `main.cpp:8758` — Unbalanced inline HTML style tags leak style/role into the rest of the document
- [x] `main.cpp:8803` — Link/image hrefs and raw source bytes are emitted to the terminal without escape-sanitisation (terminal injection) — fixed on `fix/md-terminal-injection` (new sanitize_terminal() drops C0 control bytes + DEL; applied to every non-verbatim run at the emit_line_ansi chokepoint (body text, alt text, " (url)" stubs, code) and inline to the URL embedded in both OSC 8 escape sites (markdown + inline-HTML <a>). The image-protocol payload run is now flagged verbatim so the sanitiser leaves its base64 escape intact. Tests plant ESC/CSI/OSC-title/BEL in body + a link URL and confirm none survive while link text still renders.) Note: generic table-cell values (any format) emitted via print_table are a separate, broader surface not covered here.
- [x] `main.cpp:9831` — DECIMAL/TIMESTAMP/DATE/TIME/DURATION columns counted as numeric but rendered as missing — fixed on `fix/numeric-temporal-decimal` (new array_value_as_double() switches on type_id() and handles every is_numeric_type type — all signed/unsigned int widths, float/double, date32/64, time32/64, timestamp, duration, decimal128/256 with scale; temporal types yield their epoch/elapsed count so they sort chronologically and scale in heatmaps). cell_as_double() and the four duplicated inline cast-chains (compute_col_stats, print_describe, the TUI stats popup, and the TUI numeric sort) now all route through it, so heatmap/--describe/stats/sort agree. Fixture tiny.temporal.parquet + tests (decimal min/max 0.01/99.99 scaled, date extracts, heatmap accepts them). Known nicety: temporal min/max display as raw epoch counts, not formatted dates.
- [ ] `main.cpp:11665` — Forward search is bounded by already-loaded chunks even for indexed Parquet vs streaming inconsistency
- [x] `main.cpp:13536` — No SIGINT/SIGTERM handler: Ctrl-C in the TUI leaves the terminal broken (no endwin()) — fixed on `fix/tui-signal-restore` (run() installs SIGINT/SIGTERM/SIGHUP handlers for the lifetime of the TUI that endwin() then re-raise with the default disposition, so the terminal is restored and the exit status still reflects the signal (130 for SIGINT); previous handlers are restored on normal exit. Pty regression test tests/tui_sigint_check.py asserts the alt-screen-exit sequence is emitted and the process dies via SIGINT). Hardened on `fix/tui-signal-async-safe`: the original handler called endwin() (allocates) directly, which aborted (SIGABRT) when the signal landed mid-malloc during draw() — flaky on Ubuntu 22.04 CI. The signals are now blocked except around the blocking getch(), so endwin() only runs from a safe context (parked in read(), never mid-allocation).

**Performance**
- [ ] `gui/arrowtablemodel.cpp:132` — data() decodes each cell twice when search is active (DisplayRole + BackgroundRole)
- [ ] `gui/kde/vvthumbnail.cpp:15` — Thumbnailer has no time/size budget — opening a huge or slow file blocks the preview worker
- [ ] `main.cpp:1393` — TabixInputStream allocates and frees a kstring buffer for every record line
- [ ] `main.cpp:2743` — Region-mode read_chunk re-decodes the same row group once per overlapping window
- [ ] `main.cpp:5180` — SqliteSource runs an unconditional SELECT COUNT(*) at open, forcing a full table scan even for `-n 10` previews/thumbnails
- [ ] `main.cpp:6578` — read_1d_dataset_table reads entire 1-D dataset into RAM (no cap)
- [ ] `main.cpp:9779` — Sixel emitter is O(palette x width x height): full 240-colour rescan per pixel column
- [x] `main.cpp:10680` — Per-row dynamic_pointer_cast chain in numeric stats loops (RTTI cost on every value) — addressed on `fix/numeric-temporal-decimal`: the shared array_value_as_double() branches on type_id() with a single static_cast instead of a per-cell dynamic_pointer_cast chain, removing the RTTI cost from the stats / describe / sort / heatmap value loops that now call it.
- [ ] `main.cpp:11075` — value_counts uses std::map (red-black tree) keyed by string for every cell
- [ ] `main.cpp:11686` — Searching a sorted view re-decodes a full chunk per row in the worst case

**Usability**
- [x] `main.cpp:490` — --heatmap and --image-mode are undocumented in print_usage (undiscoverable flags) — fixed on `feat/heatmap` (new "Visualization" section in --help)
- [ ] `main.cpp:635` — Missing argument to a known flag reports "Unknown option" and dumps full usage
- [x] `main.cpp:712` — --heatmap and --image-mode are implemented but undocumented in every reference (help, man, README, all 3 completions) — fixed on `feat/heatmap` (help + man + README + bash/fish/zsh completions all updated)
- [ ] `main.cpp:3117` — CSV/TSV type inference silently corrupts leading-zero IDs and scientific notation
- [ ] `main.cpp:9874` — --image-mode value is never validated; typos silently fall back to Auto
- [ ] `main.cpp:12054` — A single column wider than the terminal renders a completely blank table
- [ ] `main.cpp:14459` — No NO_COLOR environment-variable support
- [x] `man/vv.1:1` — Man page version/date stale: shows 1.4.0 / May 2026 while binary is 1.9.0 — bumped to 1.9.1 / June 2026 in the release-prep commit
- [ ] `man/vv.1:119` — NumPy .npz format missing from man page and from all three shell completions
- [ ] `tests/run_tests.sh:43` — Several formats have zero smoke-test coverage (.npz, .paf range, .cram, .sam, .gff/.gtf, .loom/generic h5 already partial)

### Low (28)

**Bugs**
- [ ] `gui/kde/thumbrender.cpp:58` — Thumbnail elision uses non-bold QFontMetrics for bold header text
- [ ] `main.cpp:1150` — emit_cell dims a genuine trailing U+2026 in cell data as if it were a truncation marker
- [ ] `main.cpp:1669` — Region integer parser silently accepts trailing garbage (e.g. 'chr1:-5-10')
- [ ] `main.cpp:3107` — block_size of 16 MiB causes a hard parse failure when a single delimited line exceeds it
- [ ] `main.cpp:3937` — Pileup insertion rendering can read past the read's SEQ on inconsistent CIGAR
- [ ] `main.cpp:5159` — SQLite identifier quoting does not escape embedded double-quotes, breaking on (or mis-parsing) tables/columns containing a " character
- [ ] `main.cpp:5371` — Empty Arrow IPC file seeds a zero-row batch that num_chunks() (=num_record_batches_=0) makes unreachable
- [ ] `main.cpp:7176` — NPY descr quote-stripping mishandles malformed/odd-length quoting
- [ ] `main.cpp:7658` — decode_pileup: '*' deletion placeholder pollutes mean_qual
- [ ] `main.cpp:9137` — HTML <a> with empty/missing href still emits a stray OSC 8 close on the matching close tag

**Performance**
- [ ] `gui/main.cpp:243` — Detail dock rebuilds all rows and allocates fresh QTableWidgetItems on every selection change
- [ ] `main.cpp:898` — truncate() is O(n^2) on wide collection cells, re-run per visible cell per redraw
- [x] `main.cpp:4338` — BcfSource builds and immediately destroys a throwaway region iterator per window — fixed on `fix/region-coord-offbyone`
- [ ] `main.cpp:5277` — read_first() fast-path cap is dead for SqliteSource and FastxSource because open() eagerly reads a full 4096-row batch
- [ ] `main.cpp:5441` — ORC read_chunk decodes every column of a stripe even when only a few columns are requested
- [ ] `main.cpp:6016` — csv_append_quoted re-scans and re-emits a repeated ODS cell up to 16384 times
- [ ] `main.cpp:7356` — unz_file_info.uncompressed_size used directly for reserve() — attacker-controlled allocation hint
- [x] `main.cpp:9838` — render_heatmap can buffer up to ~128 MB of doubles with no reserve — fixed on `feat/heatmap` (source caps lowered to 2048×2048 ≈ 32 MiB ceiling; vals reserved; truncation noted on stderr)
- [ ] `main.cpp:11789` — row_matches_search lowercases the query once per visible row, per redraw
- [ ] `main.cpp:11965` — Per-redraw integer width refit re-stringifies every visible cell on every keypress

**Usability**
- [ ] `main.cpp:718` — `--color always` (space-separated) misparsed as a filename → "file not found"
- [ ] `main.cpp:1657` — No chrom-name normalization between query and file ('chr1' vs '1') silently returns zero rows
- [ ] `main.cpp:1698` — `-r chrom:N` (single coordinate) means a 1-bp window, diverging from the samtools convention users expect
- [~] `main.cpp:9496` — .fods (Flat ODS) is dispatched in code but documented/completed nowhere — now in the README formats table (`docs/readme-npz-gui`); man page + shell completions still pending
- [ ] `main.cpp:9803` — Auto mode never uses inline-image protocols on iTerm2/WezTerm (falls to half-block)
- [ ] `main.cpp:12334` — Status bar row range overshoots loaded data on streaming sources
- [ ] `main.cpp:14683` — Auto-TUI failure falls through silently with no diagnostic unless -i was given
- [ ] `man/vv.1:375` — No documented exit-status / EXIT STATUS section
