#!/usr/bin/env bash
# Smoke tests for vv. Diffs `vv` output against checked-in golden files.
# Override the binary with VV=/path/to/vv.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VV=${VV:-$ROOT/build/vv}
DATA=$HERE/data
GOLDEN=$HERE/golden

source "$HERE/lib.sh"

if [ ! -x "$VV" ]; then
    echo "vv binary not found at $VV — set VV=/path/to/vv" >&2
    exit 1
fi
if [ ! -f "$DATA/tiny.parquet" ]; then
    echo "fixtures missing — run: python3 tests/data/generate.py" >&2
    exit 1
fi

echo "Using $VV"
echo "Version: $($VV --version)"
echo

mkdir -p "$GOLDEN"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# Helper: run vv, capture stdout, compare against golden.
run_case() {
    local name="$1"; shift
    local out="$TMP/$name.out"
    "$VV" "$@" > "$out" 2>&1
    if [ -f "$GOLDEN/$name.expected" ]; then
        assert_eq_file "$name" "$out" "$GOLDEN/$name.expected"
    else
        cp "$out" "$GOLDEN/$name.expected"
        echo "  init  $name (created golden)"
    fi
}

echo "── TSV export ─────────────────────────────────────────────"
run_case parquet_tsv  --tsv --no-header "$DATA/tiny.parquet"
run_case bed_tsv      --tsv --no-header "$DATA/tiny.bed"
run_case bed_gz_tsv   --tsv --no-header "$DATA/tiny.bed.gz"
run_case vcf_tsv      --tsv --no-header "$DATA/tiny.vcf"
run_case vcf_gz_tsv   --tsv --no-header "$DATA/tiny.vcf.gz"
run_case fasta_tsv    --tsv --no-header "$DATA/tiny.fa"
run_case fasta_gz_tsv --tsv --no-header "$DATA/tiny.fa.gz"
run_case fastq_tsv    --tsv --no-header "$DATA/tiny.fq"
run_case fastq_gz_tsv --tsv --no-header "$DATA/tiny.fq.gz"
run_case tsv_tsv      --tsv --no-header "$DATA/tiny.tsv"
run_case csv_tsv      --tsv --no-header "$DATA/tiny.csv"
run_case arrow_tsv    --tsv --no-header "$DATA/tiny.arrow"
# Empty Arrow IPC (schema, zero record batches): the table view must render the
# column header + "0 rows" like an empty Parquet, not draw nothing (the seeded
# zero-row batch was unreachable when num_chunks() reported 0).
if [ -f "$DATA/tiny.empty.arrow" ]; then
    EMPTY_IPC=$("$VV" --no-interactive --color=never "$DATA/tiny.empty.arrow" 2>&1)
    assert_contains "empty_ipc_renders_header" "$EMPTY_IPC" "Chr"
    assert_contains "empty_ipc_zero_rows"      "$EMPTY_IPC" "0 rows"
fi
run_case paf_tsv      --tsv --no-header "$DATA/tiny.paf"
run_case paf_gz_tsv   --tsv --no-header "$DATA/tiny.paf.gz"

# Streaming delimited error handling: a malformed row BEYOND the first 16 MiB
# CSV block must surface as a non-zero exit, not be swallowed into a silently
# truncated result with status 0. Build the fixture in $TMP (too big to commit:
# the first block must be full of good rows so the bad one lands in a later
# block).
BIGTSV="$TMP/truncate.tsv"
{ printf 'a\tb\tc\n'; yes "$(printf 'row\t123\t4.5')" | head -n 2000000; \
  printf 'BADROW_ONLY_ONE_COL\n'; } > "$BIGTSV"
"$VV" --tsv --no-header "$BIGTSV" >/dev/null 2>/dev/null
TRUNC_RC=$?
if [ "$TRUNC_RC" -ne 0 ]; then
    PASS=$((PASS+1)); echo "  ok    delimited_midstream_error_exits_nonzero"
else
    FAIL=$((FAIL+1)); echo "  FAIL  delimited_midstream_error_exits_nonzero (silent truncation, exit 0)"
fi
rm -f "$BIGTSV"

# Streaming retention window: a forward-only stream keeps only a bounded window
# of decoded batches (so deep scroll / G on a huge file doesn't grow RAM
# without bound). Forcing the window to a single batch (VV_STREAM_BATCH_CAP=1)
# over a >16 MiB input (several CSV blocks → several batches) must STILL export
# every row: eviction frees old batches, but a sequential read consumes each
# one once, as it's produced, so nothing is dropped. (2M rows ≈ 24 MiB.)
WINTSV="$TMP/window.tsv"
{ printf 'a\tb\tc\n'; yes "$(printf 'row\t123\t4.5')" | head -n 2000000; } > "$WINTSV"
WIN_OUT=$(VV_STREAM_BATCH_CAP=1 "$VV" --tsv --no-header "$WINTSV" | wc -l | tr -d ' ')
assert_eq_file_inline "stream_window_export_complete_under_eviction" "$WIN_OUT" "2000000"
# --unique drives ensure() sequentially too: the distinct count must be exact
# (one repeated row → 2,000,000 occurrences of a single value) under eviction.
WIN_UNIQ=$(VV_STREAM_BATCH_CAP=1 "$VV" --unique a "$WINTSV" 2>&1)
assert_contains "stream_window_unique_count_exact" "$WIN_UNIQ" "2_000_000"
rm -f "$WINTSV"

# Streaming FASTX error handling: a malformed record BEYOND the first batch
# (BATCH_SIZE=4096) must surface as a non-zero exit and must not spin forever
# in ensure(). Build a FASTQ with >4096 good records then a truncated one
# (quality shorter than the sequence, at EOF) in $TMP.
BADFQ="$TMP/bigbad.fq"
awk 'BEGIN{for(i=1;i<=4200;i++)printf "@r%d\nACGT\n+\nIIII\n",i;
          printf "@bad\nACGTACGT\n+\nII\n"}' > "$BADFQ"
timeout 30 "$VV" --tsv --no-header "$BADFQ" >/dev/null 2>/dev/null
FQ_RC=$?
if [ "$FQ_RC" -eq 124 ]; then
    FAIL=$((FAIL+1)); echo "  FAIL  fastx_midstream_error_no_hang (timed out / hung)"
elif [ "$FQ_RC" -ne 0 ]; then
    PASS=$((PASS+1)); echo "  ok    fastx_midstream_error_no_hang"
else
    FAIL=$((FAIL+1)); echo "  FAIL  fastx_midstream_error_no_hang (silent truncation, exit 0)"
fi
rm -f "$BADFQ"

# Streaming retention window on a second source family (FastxSource): a FASTQ
# with >BATCH_SIZE (4096) records spans multiple batches. With the window
# forced to 1 batch, a sequential export must still emit every record — the
# same bounded-window helper as DelimitedSource, exercised here on FASTQ.
WINFQ="$TMP/window.fq"
awk 'BEGIN{for(i=1;i<=10000;i++)printf "@r%d\nACGTACGT\n+\nIIIIIIII\n",i}' > "$WINFQ"
WINFQ_OUT=$(VV_STREAM_BATCH_CAP=1 "$VV" --tsv --no-header "$WINFQ" | wc -l | tr -d ' ')
assert_eq_file_inline "stream_window_fastq_export_complete_under_eviction" \
    "$WINFQ_OUT" "10000"
rm -f "$WINFQ"

if [ -f "$DATA/tiny.bcf" ]; then
    run_case bcf_tsv  --tsv --no-header "$DATA/tiny.bcf"
fi
run_case lociss_tsv             --tsv --no-header "$DATA/tiny.lociss"
run_case lociss_table_hides     --no-interactive --no-index --color=never "$DATA/tiny.lociss"
run_case lociss_region_basic    --tsv --no-header -r chr1:600-1100 "$DATA/tiny.lociss"
run_case lociss_region_multi    --tsv --no-header -r 'chr1:600-1100,chr2:0-1000' "$DATA/tiny.lociss"
run_case lociss_region_open     --tsv --no-header --window chr2: "$DATA/tiny.lociss"
EMPTY=$("$VV" --tsv --no-header -r chr3:0-100 "$DATA/tiny.lociss" 2>&1)
if [ -z "$EMPTY" ]; then
    PASS=$((PASS + 1)); echo "  ok    lociss_region_empty"
else
    FAIL=$((FAIL + 1)); echo "  FAIL  lociss_region_empty (expected empty output, got: $EMPTY)"
fi
# LociSSD top banner: genome assembly (+ species, derived when the manifest omits
# it) and the total element count, shown above the table. Plain Parquet has none.
LOC_BANNER=$("$VV" --no-interactive --color=never "$DATA/tiny.lociss" 2>&1 | head -1)
assert_contains "lociss_banner_assembly" "$LOC_BANNER" "hg38"
assert_contains "lociss_banner_species"  "$LOC_BANNER" "Homo sapiens"
assert_contains "lociss_banner_count"    "$LOC_BANNER" "5 elements"
refute_contains "parquet_no_banner" \
    "$("$VV" --no-interactive --color=never "$DATA/tiny.parquet" 2>&1)" "LociSSD"
# The banner is a table-view header only — it must NOT leak into --tsv export.
refute_contains "lociss_banner_not_in_tsv" \
    "$("$VV" --tsv "$DATA/tiny.lociss" 2>&1)" "elements"
# Generic Parquet range queries: auto-detected chrom/start/end columns,
# plus the --region-cols override path.
PQ_REG=$("$VV" --tsv --no-header -r chr1:1000-2500 "$DATA/tiny.parquet" | wc -l)
assert_eq_file_inline "parquet_region_autodetect_two_rows" "$PQ_REG" "2"
PQ_REG_OV=$("$VV" --tsv --no-header -r chr1:1000-2500 --region-cols Chr,Start,End "$DATA/tiny.parquet" | wc -l)
assert_eq_file_inline "parquet_region_cols_override_two_rows" "$PQ_REG_OV" "2"
PQ_REG_BAD=$("$VV" -r chr1:0-100 --region-cols NoSuch,Start,End "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "parquet_region_cols_unknown_errors" "$PQ_REG_BAD" "not found"
# Region predicate must handle compact integer coordinate types, not just
# Int32/Int64. tiny.uint32.parquet stores Start/End as UInt32; chr1:1000-2500
# overlaps two rows. Pre-fix the predicate read 0 for UInt32 and returned none.
if [ -f "$DATA/tiny.uint32.parquet" ]; then
    PQ_U32=$("$VV" --tsv --no-header -r chr1:1000-2500 "$DATA/tiny.uint32.parquet" | wc -l)
    assert_eq_file_inline "parquet_region_uint32_coords_two_rows" "$PQ_U32" "2"
fi
# Region stats-pruning must index Parquet column statistics by *leaf* column,
# not Arrow field. tiny.nested.parquet puts a 2-leaf struct before Start/End,
# shifting their leaf index; with the bug, pruning read the struct's stats as
# "Start" (set to 100000) and pruned the only — matching — row group, so the
# query returned 0 rows. chr1:150-160 overlaps the row (Start 100, End 200).
if [ -f "$DATA/tiny.nested.parquet" ]; then
    PQ_NEST=$("$VV" --tsv --no-header -r chr1:150-160 "$DATA/tiny.nested.parquet" | wc -l)
    assert_eq_file_inline "parquet_region_leaf_index_nested_schema" "$PQ_NEST" "1"
fi
# Region row count must be exact (post-filter), not the pre-filter slice size:
# the table-mode "[N rows]" must match the streamed (--tsv) row count. Generic
# Parquet is the worst case — a slice spans a whole row group — where pre-fix
# this reported the row-group size with phantom trailing rows.
for f in tiny.parquet tiny.lociss; do
    REG_TSV=$("$VV" --tsv --no-header -r chr1:0-3000 "$DATA/$f" 2>/dev/null | wc -l)
    REG_TBL=$("$VV" -r chr1:0-3000 --no-interactive --color=never "$DATA/$f" 2>/dev/null \
        | grep -oE '\[[0-9]+ rows' | grep -oE '[0-9]+')
    assert_eq_file_inline "region_count_exact_${f#tiny.}" "$REG_TBL" "$REG_TSV"
done

# --slop on tabix BED.
SLOP_OUT=$("$VV" --tsv --no-header -r chr1:1500-1500 --slop 4000 "$DATA/tiny.bed.gz" | wc -l)
if [ "$SLOP_OUT" -gt 0 ]; then
    PASS=$((PASS+1)); echo "  ok    slop_tabix_bed"
else
    FAIL=$((FAIL+1)); echo "  FAIL  slop_tabix_bed (expected non-empty output)"
fi
# --regions-file (TSV-formatted BED).
printf 'chr1\t100\t900\nchr2\t1400\t1900\n' > "$TMP/multi.bed"
RF_OUT=$("$VV" --tsv --no-header --regions-file "$TMP/multi.bed" "$DATA/tiny.lociss" | wc -l)
assert_eq_file_inline "regions_file_collected_three_rows" "$RF_OUT" "3"
# BCF range queries (skip if bcftools wasn't available during fixture build).
if [ -f "$DATA/tiny.bcf.csi" ]; then
    BCF_REGION=$("$VV" --tsv --no-header -r chr1:200-1600 "$DATA/tiny.bcf" | wc -l)
    assert_eq_file_inline "bcf_region_returns_two_rows" "$BCF_REGION" "2"
fi

# Coordinate-convention regression: -r windows are UCSC 0-based half-open for
# EVERY format. The htslib-backed paths (tabix BED/VCF/GFF, BAM pileup, BCF)
# used to forward the 0-based string straight to htslib's 1-based-inclusive
# region parsers, shifting every query one base and disagreeing with the
# Parquet interval path. These cases sit exactly on a feature boundary, where
# the old off-by-one flipped the result.
#
# tiny.bed.gz / tiny.parquet share peak_0 = chr1 [100, 200) (0-based half-open).
# The half-open window [200, 250) starts where the peak ends, so it must match
# NOTHING — and tabix BED must agree with Parquet. (An empty tabix region also
# exits non-zero with "Empty CSV file"; we assert on stdout row count only.)
BED_BOUNDARY=$("$VV" --tsv --no-header -r chr1:200-250 "$DATA/tiny.bed.gz" 2>/dev/null | grep -c . || true)
PQ_BOUNDARY=$("$VV"  --tsv --no-header -r chr1:200-250 "$DATA/tiny.parquet" 2>/dev/null | grep -c . || true)
assert_eq_file_inline "region_tabix_bed_boundary_excludes_peak" "$BED_BOUNDARY" "0"
assert_eq_file_inline "region_tabix_parquet_agree_at_boundary"  "$BED_BOUNDARY" "$PQ_BOUNDARY"
# Interior window [150, 160) lies strictly inside peak_0 → exactly one row.
BED_INTERIOR=$("$VV" --tsv --no-header -r chr1:150-160 "$DATA/tiny.bed.gz" 2>/dev/null | grep -c . || true)
assert_eq_file_inline "region_tabix_bed_interior_matches_peak" "$BED_INTERIOR" "1"
# BCF: variant at POS 500 is 0-based base 499, so the half-open window
# [500, 1600) must EXCLUDE it and return only POS 1500 (one row). The old
# off-by-one (htslib read "500-1600" as 1-based inclusive) returned two.
if [ -f "$DATA/tiny.bcf.csi" ]; then
    BCF_BOUNDARY=$("$VV" --tsv --no-header -r chr1:500-1600 "$DATA/tiny.bcf" | wc -l)
    assert_eq_file_inline "bcf_region_boundary_excludes_start_variant" "$BCF_BOUNDARY" "1"
fi

# BCF with genotype samples: the FORMAT_SAMPLES column must keep the FORMAT
# spec (e.g. GT:AD:DP), not collapse to the per-sample values alone.
if [ -f "$DATA/tiny.samples.bcf" ]; then
    SMP_OUT=$("$VV" --tsv --no-header "$DATA/tiny.samples.bcf" 2>&1)
    assert_contains "bcf_format_spec_preserved" "$SMP_OUT" "GT:AD:DP"
    assert_contains "bcf_sample_values_present" "$SMP_OUT" "0/1:5,6:11"
fi

# Empty tabix region: a window over a known chromosome that overlaps no records
# must return an empty result with exit 0 — matching the Parquet/BCF/BAM paths —
# rather than aborting with "Empty CSV file" (the tabix stream is empty, which
# Arrow's CSV reader rejects). The column layout is still recovered from the
# file so headers/schema render normally.
assert_exit_zero "tabix_empty_region_bed_exit0" \
    "$VV" --tsv --no-header -r chr1:9000000-9000001 "$DATA/tiny.bed.gz"
EMPTY_BED=$("$VV" --tsv --no-header -r chr1:9000000-9000001 "$DATA/tiny.bed.gz" 2>/dev/null | grep -c . || true)
assert_eq_file_inline "tabix_empty_region_bed_no_rows" "$EMPTY_BED" "0"
EMPTY_SCH=$("$VV" --schema -r chr1:9000000-9000001 "$DATA/tiny.bed.gz" 2>&1)
assert_contains "tabix_empty_region_schema_recovered" "$EMPTY_SCH" "BED5"
if [ -f "$DATA/tiny.vcf.gz.tbi" ]; then
    assert_exit_zero "tabix_empty_region_vcf_exit0" \
        "$VV" --tsv --no-header -r chr1:9000000-9000001 "$DATA/tiny.vcf.gz"
fi

# bigBed / bigWig — autoSql expansion + range queries.
if [ -f "$DATA/tiny.bb" ]; then
    BB_TSV=$("$VV" --tsv --no-header "$DATA/tiny.bb" | wc -l)
    assert_eq_file_inline "bigbed_tsv_returns_five_rows" "$BB_TSV" "5"
    BB_SCHEMA=$("$VV" --schema "$DATA/tiny.bb")
    assert_contains "bigbed_autosql_expanded_signalValue" "$BB_SCHEMA" "signalValue"
    assert_contains "bigbed_autosql_expanded_pValue"      "$BB_SCHEMA" "pValue"
    BB_REGION=$("$VV" --tsv --no-header -r chr1:300-1100 "$DATA/tiny.bb" | wc -l)
    assert_eq_file_inline "bigbed_region_returns_two_rows" "$BB_REGION" "2"
    BB_FILTER=$("$VV" --tsv --no-header --filter 'signalValue > 10' "$DATA/tiny.bb" | wc -l)
    assert_eq_file_inline "bigbed_filter_by_signalValue" "$BB_FILTER" "2"
fi
if [ -f "$DATA/tiny.bw" ]; then
    BW_TSV=$("$VV" --tsv --no-header "$DATA/tiny.bw" | wc -l)
    assert_eq_file_inline "bigwig_tsv_returns_five_rows" "$BW_TSV" "5"
    BW_REGION=$("$VV" --tsv --no-header -r chr1:300-1100 "$DATA/tiny.bw" | wc -l)
    assert_eq_file_inline "bigwig_region_returns_two_rows" "$BW_REGION" "2"
fi

# Smart list/map truncation: when a column is too narrow for the full
# value, the old code dropped to "[first, …]" after the first comma.
# The new code walks every top-level comma and picks the largest prefix
# that fits. tiny.parquet's Tags column has a row with two 2-letter and
# one 8-letter element; at width 14 the whole "[promoter, TF]" fits.
NARROW=$("$VV" -w 14 --no-interactive --no-index --color=never -n 6 "$DATA/tiny.parquet")
assert_contains "trunc_lists_fit_multiple_elements" "$NARROW" "[promoter, TF]"
# UTF-8-safe truncation: truncate() must cut on a codepoint boundary, not
# mid-byte. "é" is 2 bytes; at a narrow width the byte-based fallback used to
# split the trailing 'é' and emit invalid UTF-8. The truncated cell (and the
# whole table, which is full of multibyte box-drawing) must round-trip iconv.
UTF8TSV="$TMP/utf8trunc.tsv"
printf 'v\nééééééééé\n' > "$UTF8TSV"
UTF8_OUT=$("$VV" --no-interactive --color=never -w 6 "$UTF8TSV" 2>&1)
if command -v iconv >/dev/null 2>&1; then
    if printf '%s' "$UTF8_OUT" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1; then
        PASS=$((PASS+1)); echo "  ok    truncate_utf8_codepoint_boundary"
    else
        FAIL=$((FAIL+1)); echo "  FAIL  truncate_utf8_codepoint_boundary (invalid UTF-8)"
    fi
fi
# The kept prefix is whole codepoints + ellipsis (5×'é' then '…' at width 6).
assert_contains "truncate_utf8_keeps_whole_codepoints" "$UTF8_OUT" "ééééé…"
rm -f "$UTF8TSV"
# display_width must count terminal columns (wide CJK / Hangul = 2 cols), not
# codepoints, so a table mixing wide and narrow cells stays aligned. Render one
# and confirm every box-drawing line has the same display width — computed
# independently via python's unicodedata.east_asian_width. (pysam not needed.)
if command -v python3 >/dev/null 2>&1; then
    CJKTSV="$TMP/cjk.tsv"
    printf 'name\tval\n日本語\t1\nAB\t2\n한국어\t3\n' > "$CJKTSV"
    CJK_OUT=$("$VV" --no-interactive --color=never --no-index "$CJKTSV" 2>&1)
    if printf '%s' "$CJK_OUT" | python3 -c '
import sys, unicodedata
def dw(s):
    w = 0
    for ch in s:
        if unicodedata.combining(ch): continue
        w += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return w
box = set("╭─┬╮│├┼┤╰┴╯")
widths = set(dw(l) for l in sys.stdin.read().splitlines()
             if any(c in box for c in l))
sys.exit(0 if len(widths) == 1 else 1)
'; then
        PASS=$((PASS+1)); echo "  ok    table_aligned_with_wide_chars"
    else
        FAIL=$((FAIL+1)); echo "  FAIL  table_aligned_with_wide_chars (misaligned)"
    fi
    rm -f "$CJKTSV"
fi
# 2bit — UCSC sequence-index reader.
if [ -f "$DATA/tiny.2bit" ]; then
    TBT_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.2bit" | wc -l)
    assert_eq_file_inline "twobit_returns_three_sequences" "$TBT_ROWS" "3"
    TBT_LEN=$("$VV" --tsv --no-header --select length "$DATA/tiny.2bit" | paste -sd, -)
    assert_eq_file_inline "twobit_lengths_match"             "$TBT_LEN" "20,27,12"
    TBT_NB=$("$VV" --tsv --no-header --select n_blocks "$DATA/tiny.2bit" | paste -sd, -)
    assert_eq_file_inline "twobit_n_block_counts"            "$TBT_NB"  "0,1,0"
    TBT_FOOTER=$("$VV" --schema "$DATA/tiny.2bit" 2>&1)
    assert_contains      "twobit_footer_shows_format"        "$TBT_FOOTER" "Format: 2bit"
fi
# Malformed 2bit: a header declaring 0xFFFFFFFF sequences over a 16-byte file
# must be rejected before the index reserve (which would request ~170 GB and
# abort on systems without memory overcommit), not after a generic truncation.
if [ -f "$DATA/tiny.malformed.2bit" ]; then
    TBT_BAD=$("$VV" --schema "$DATA/tiny.malformed.2bit" 2>&1 || true)
    assert_contains "twobit_malformed_seqcount_rejected" "$TBT_BAD" "exceeds the size"
fi
# --tsv must keep MaxEndSoFar; table must not show it.
TSV_OUT=$("$VV" --tsv --no-header "$DATA/tiny.lociss")
assert_contains "lociss_tsv_keeps_maxendsofar"  "$TSV_OUT"  "1800"

# --validate on a well-formed LociSSD passes.
"$VV" --validate "$DATA/tiny.lociss" > "$TMP/validate.ok" 2>&1
if [ $? -eq 0 ] && grep -q "0 failed" "$TMP/validate.ok"; then
    PASS=$((PASS + 1)); echo "  ok    validate_passes_on_tiny_lociss"
else
    FAIL=$((FAIL + 1)); echo "  FAIL  validate_passes_on_tiny_lociss"
fi
# --validate on a non-LociSSD Parquet fails with a clear message.
"$VV" --validate "$DATA/tiny.parquet" > "$TMP/validate.bad" 2>&1
if [ $? -ne 0 ] && grep -q "not a LociSSD file" "$TMP/validate.bad"; then
    PASS=$((PASS + 1)); echo "  ok    validate_rejects_non_lociss_parquet"
else
    FAIL=$((FAIL + 1)); echo "  FAIL  validate_rejects_non_lociss_parquet"
fi
TABLE_OUT=$("$VV" --no-interactive --no-index --color=never "$DATA/tiny.lociss" 2>&1)
if printf '%s' "$TABLE_OUT" | grep -q "^| MaxEndSoFar "; then
    FAIL=$((FAIL + 1))
    echo "  FAIL  lociss_table_omits_maxendsofar (column header leaked into view)"
else
    PASS=$((PASS + 1))
    echo "  ok    lociss_table_omits_maxendsofar"
fi

echo
echo "── Tabix range queries ───────────────────────────────────"
run_case bed_region   --tsv --no-header -r 'chr1:5000-15000' "$DATA/tiny.bed.gz"
run_case bed_region_multi --tsv --no-header -r 'chr1:1000-3000,chr2:6000-9000' "$DATA/tiny.bed.gz"
run_case vcf_region   --tsv --no-header -r 'chr1:200-2000' "$DATA/tiny.vcf.gz"

echo
echo "── ASCII table mode ──────────────────────────────────────"
run_case parquet_table --no-interactive --no-index --color=never -n 5 "$DATA/tiny.parquet"
run_case bed_table     --no-interactive --no-index --color=never "$DATA/tiny.bed"

echo
echo "── Vertical-head mode ───────────────────────────────────"
COLUMNS=150 "$VV" --vertical --color=never -n 3 "$DATA/tiny.parquet" \
    > "$TMP/parquet_vertical.out" 2>&1
if [ -f "$GOLDEN/parquet_vertical.expected" ]; then
    assert_eq_file "parquet_vertical" "$TMP/parquet_vertical.out" "$GOLDEN/parquet_vertical.expected"
else
    cp "$TMP/parquet_vertical.out" "$GOLDEN/parquet_vertical.expected"
    echo "  init  parquet_vertical (created golden)"
fi

echo
echo "── Parquet output ──────────────────────────────────────"
"$VV" --parquet "$TMP/out.parquet" "$DATA/tiny.tsv" > /dev/null
"$VV" --tsv --no-header "$TMP/out.parquet" > "$TMP/out.parquet.tsv"
assert_eq_file "parquet_write_roundtrip" "$TMP/out.parquet.tsv" "$GOLDEN/tsv_tsv.expected"
"$VV" --parquet "$TMP/out_snappy.parquet" --compression snappy "$DATA/tiny.tsv" > /dev/null
assert_exit_zero "parquet_write_snappy" test -s "$TMP/out_snappy.parquet"
BAD=$("$VV" --parquet "$TMP/x.parquet" --compression rar "$DATA/tiny.tsv" 2>&1 || true)
assert_contains "parquet_bad_codec" "$BAD" "Unknown --compression"

# --parquet - : write Parquet to stdout via a temp spool.
"$VV" --parquet - "$DATA/tiny.tsv" > "$TMP/parquet_stdout.parquet" 2>/dev/null
"$VV" --tsv --no-header "$TMP/parquet_stdout.parquet" > "$TMP/parquet_stdout.tsv"
assert_eq_file "parquet_stdout_roundtrip" "$TMP/parquet_stdout.tsv" "$GOLDEN/tsv_tsv.expected"
"$VV" --parquet "$TMP/parquet_disk.parquet" "$DATA/tiny.tsv" > /dev/null
cmp -s "$TMP/parquet_stdout.parquet" "$TMP/parquet_disk.parquet"
if [ $? -eq 0 ]; then
    PASS=$((PASS+1)); echo "  ok    parquet_stdout_bit_identical_to_disk"
else
    FAIL=$((FAIL+1)); echo "  FAIL  parquet_stdout_bit_identical_to_disk"
fi

echo
echo '── Stdin (-) ──────────────────────────────────────────'
"$VV" --tsv --no-header - < "$DATA/tiny.tsv" > "$TMP/stdin_tsv.out"
assert_eq_file "stdin_tsv_matches_file" "$TMP/stdin_tsv.out" "$GOLDEN/tsv_tsv.expected"
gzip -c "$DATA/tiny.tsv" | "$VV" --tsv --no-header - > "$TMP/stdin_gz.out"
assert_eq_file "stdin_tsv_gz_matches_file" "$TMP/stdin_gz.out" "$GOLDEN/tsv_tsv.expected"
PARQUET_REJECT=$("$VV" - < "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "stdin_rejects_parquet" "$PARQUET_REJECT" "seekable"

echo
echo "── --schema / --describe / --select / --filter / --json ─"
SCHEMA_OUT=$("$VV" --schema "$DATA/tiny.lociss")
assert_contains "schema_has_chromosome" "$SCHEMA_OUT" "Chromosome"
assert_contains "schema_has_maxendsofar" "$SCHEMA_OUT" "MaxEndSoFar"
# Header detection: a header row of word-like float tokens (nan / inf / hex)
# must be kept as column NAMES, not mistaken for headerless numeric data.
# strtod() accepts those, so the real header used to be dropped.
WORDHDR="$TMP/wordhdr.csv"
printf 'nan,inf\n1,2\n3,4\n' > "$WORDHDR"
WORDHDR_SCHEMA=$("$VV" --schema "$WORDHDR" 2>&1)
assert_contains "header_nan_kept_as_column_name" "$WORDHDR_SCHEMA" "nan"
assert_contains "header_inf_kept_as_column_name" "$WORDHDR_SCHEMA" "inf"
# A genuinely headerless numeric CSV is still detected (auto-named f0/f1), so
# the first data row isn't consumed as a header.
NUMDATA="$TMP/numdata.csv"
printf '1,2\n3,4\n5,6\n' > "$NUMDATA"
NUMDATA_SCHEMA=$("$VV" --schema "$NUMDATA" 2>&1)
assert_contains "headerless_numeric_autogen_f0" "$NUMDATA_SCHEMA" "f0"
rm -f "$WORDHDR" "$NUMDATA"
# Type inference must not drop leading zeros: a column of values like 007 is a
# code/ID, not the integer 7. Such columns are read as utf8 (string); ordinary
# numeric columns and scientific notation are left numeric.
IDZ="$TMP/idzeros.csv"
printf 'id,sample,score\n007,100,0.5\n012,250,0.75\n003,9999,0.1\n' > "$IDZ"
IDZ_ID=$("$VV" --schema "$IDZ" 2>&1 | awk '/^id[[:space:]]/{print $2; exit}')
assert_eq_file_inline "leading_zero_id_is_string"  "$IDZ_ID" "string"
IDZ_SAMPLE=$("$VV" --schema "$IDZ" 2>&1 | awk '/^sample[[:space:]]/{print $2; exit}')
assert_eq_file_inline "normal_int_col_unchanged"   "$IDZ_SAMPLE" "int64"
IDZ_TSV=$("$VV" --tsv --no-header "$IDZ" 2>&1)
assert_contains "leading_zero_value_preserved"     "$IDZ_TSV" "007"
rm -f "$IDZ"
# Scientific notation must stay numeric (forcing it to string would corrupt
# legitimate numeric data — explicitly out of scope).
SCI="$TMP/sci.csv"
printf 'p\n1e5\n2e5\n' > "$SCI"
SCI_TYPE=$("$VV" --schema "$SCI" 2>&1 | awk '/^p[[:space:]]/{print $2; exit}')
assert_eq_file_inline "scientific_notation_stays_numeric" "$SCI_TYPE" "double"
rm -f "$SCI"
# csv_buffer_to_table path (markdown / xlsx / ods / html tables): same fix.
IDZMD="$TMP/idzeros.md"
printf '| id | n |\n|----|---|\n| 007 | 1 |\n| 012 | 2 |\n' > "$IDZMD"
IDZMD_OUT=$("$VV" --color=never "$IDZMD" 2>&1)
assert_contains "leading_zero_preserved_markdown"  "$IDZMD_OUT" "007"
rm -f "$IDZMD"
DESCRIBE_OUT=$("$VV" --describe "$DATA/tiny.lociss")
assert_contains "describe_has_columns_header" "$DESCRIBE_OUT" "Column"
assert_contains "describe_has_distinct_for_string" "$DESCRIBE_OUT" "Chromosome"
# --describe must summarise the WHOLE table, not just the first head_rows (10)
# preview rows. tiny.parquet has 20 rows → the per-column Count must read 20.
DESC_COUNT=$("$VV" --describe "$DATA/tiny.parquet" 2>&1 \
             | awk '/^Chr[[:space:]]/{print $3; exit}')
assert_eq_file_inline "describe_scans_all_rows" "$DESC_COUNT" "20"
# An explicit -n still caps the describe scan.
DESC_N=$("$VV" --describe -n 5 "$DATA/tiny.parquet" 2>&1 \
         | awk '/^Chr[[:space:]]/{print $3; exit}')
assert_eq_file_inline "describe_honors_explicit_head_rows" "$DESC_N" "5"
# Temporal/decimal columns are numeric: the value extractor must read date /
# timestamp / decimal (previously skipped → blank stats, blank heatmap).
if [ -f "$DATA/tiny.temporal.parquet" ]; then
    TEMP_DESC=$("$VV" --describe "$DATA/tiny.temporal.parquet" 2>&1)
    # decimal128(10,2) min/max with scale honoured.
    assert_contains "describe_decimal_min_scaled" "$TEMP_DESC" "0.01"
    assert_contains "describe_decimal_max_scaled" "$TEMP_DESC" "99.99"
    # date32(2024-01-01) = 19723 days since the epoch — proves the date column
    # now yields a numeric value instead of a blank min/max.
    assert_contains "describe_date_extracts" "$TEMP_DESC" "19723"
    # --heatmap must treat date/timestamp/decimal as plottable numeric columns.
    TEMP_HM=$("$VV" --heatmap --image-mode ascii "$DATA/tiny.temporal.parquet" 2>&1)
    refute_contains "heatmap_accepts_temporal_cols" "$TEMP_HM" "no numeric columns"
fi
SELECT_OUT=$("$VV" --tsv --no-header --select Chromosome,Score "$DATA/tiny.lociss" \
             | head -1)
assert_eq_file_inline "select_two_cols" "$SELECT_OUT" "chr1	0.5"
FILTER_OUT=$("$VV" --tsv --no-header --filter 'Score > 0.4' "$DATA/tiny.lociss" \
             | wc -l)
assert_eq_file_inline "filter_keeps_3_rows" "$FILTER_OUT" "3"
JSON_OUT=$("$VV" --ndjson --select Chromosome,Start "$DATA/tiny.lociss" \
           | head -1)
assert_eq_file_inline "ndjson_one_object_per_line" "$JSON_OUT" '{"Chromosome": "chr1", "Start": 100}'
BAD_COL=$("$VV" --select Bogus "$DATA/tiny.lociss" 2>&1 || true)
assert_contains "select_unknown_column_errors" "$BAD_COL" "unknown"
BAD_FILTER=$("$VV" --tsv --filter 'BogusCol > 0' "$DATA/tiny.lociss" 2>&1 || true)
assert_contains "filter_unknown_column_errors" "$BAD_FILTER" "unknown"

# --stats: per-column rollup on a Parquet file.
STATS_OUT=$("$VV" --stats "$DATA/tiny.lociss")
assert_contains "stats_has_row_groups" "$STATS_OUT" "Row groups:"
assert_contains "stats_has_codec_column" "$STATS_OUT" "zstd"

# --unique: distinct value counts.
UNIQ_OUT=$("$VV" --unique Chromosome "$DATA/tiny.lociss")
assert_contains "unique_lists_chr1" "$UNIQ_OUT" "chr1"
assert_contains "unique_counts_2_distinct" "$UNIQ_OUT" "2 distinct"
BAD_UNIQ=$("$VV" --unique BogusCol "$DATA/tiny.lociss" 2>&1 || true)
assert_contains "unique_unknown_column_errors" "$BAD_UNIQ" "unknown"

# --sample: pulls a subset and preserves the hidden-column convention.
SAMPLE_OUT=$("$VV" --tsv --no-header --sample 2 "$DATA/tiny.lociss" | wc -l)
assert_eq_file_inline "sample_returns_two_rows" "$SAMPLE_OUT" "2"
SAMPLE_BIG=$("$VV" --tsv --no-header --sample 100 "$DATA/tiny.lociss" | wc -l)
assert_eq_file_inline "sample_more_than_total_returns_all" "$SAMPLE_BIG" "5"


# --md: GitHub-flavored markdown table.
MD_OUT=$("$VV" --md -n 2 "$DATA/tiny.lociss")
assert_contains "md_has_header_pipes"     "$MD_OUT" "| Chromosome |"
assert_contains "md_has_separator_row"    "$MD_OUT" "| --- |"
assert_contains "md_has_data_row_chr1"    "$MD_OUT" "| chr1 |"

# --tail: last-N rows. tiny.parquet has 20 rows.
TAIL_OUT=$("$VV" --tail 3 --tsv --no-header "$DATA/tiny.parquet" | wc -l)
assert_eq_file_inline "tail_returns_three_rows" "$TAIL_OUT" "3"
TAIL_LAST=$("$VV" --tail 1 --tsv --no-header "$DATA/tiny.parquet")
assert_contains "tail_picks_last_row" "$TAIL_LAST" "7100"  # last row Start
TAIL_BIG=$("$VV" --tail 100 --tsv --no-header "$DATA/tiny.parquet" | wc -l)
assert_eq_file_inline "tail_larger_than_total_returns_all" "$TAIL_BIG" "20"

# --coords: NCBI (1-based inclusive) input converts to UCSC (0-based
# half-open). "chr1:101-200" NCBI == "chr1:100-200" UCSC (default).
COORDS_UCSC=$("$VV" -r 'chr1:100-200' --tsv --no-header "$DATA/tiny.parquet")
COORDS_NCBI=$("$VV" -r 'chr1:101-200' --coords NCBI --tsv --no-header "$DATA/tiny.parquet")
if [ "$COORDS_UCSC" = "$COORDS_NCBI" ] && [ -n "$COORDS_UCSC" ]; then
    PASS=$((PASS+1)); echo "  ok    coords_ncbi_matches_ucsc_window"
else
    FAIL=$((FAIL+1)); echo "  FAIL  coords_ncbi_matches_ucsc_window"
fi
# Legacy aliases still work.
COORDS_TBX=$("$VV" -r 'chr1:101-200' --coords tabix --tsv --no-header "$DATA/tiny.parquet")
COORDS_GENBANK=$("$VV" -r 'chr1:101-200' --coords GenBank --tsv --no-header "$DATA/tiny.parquet")
if [ "$COORDS_TBX" = "$COORDS_UCSC" ] && [ "$COORDS_GENBANK" = "$COORDS_UCSC" ]; then
    PASS=$((PASS+1)); echo "  ok    coords_aliases_tabix_and_genbank"
else
    FAIL=$((FAIL+1)); echo "  FAIL  coords_aliases_tabix_and_genbank"
fi
COORDS_BAD=$("$VV" -r chr1:1-2 --coords nonsense "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "coords_unknown_value_errors" "$COORDS_BAD" "UCSC"
# A malformed region (trailing garbage after the coordinate) is rejected with a
# clear error, not silently dropped into a whole-file query.
REGION_BAD=$("$VV" -r chr1:5x --no-interactive "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "region_trailing_garbage_rejected" "$REGION_BAD" "Invalid region"
REGION_BAD2=$("$VV" -r chr1:-5-10 --no-interactive "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "region_double_dash_rejected" "$REGION_BAD2" "Invalid region"
# A known flag missing its argument gives a targeted error, not "Unknown option".
MISSING_ARG=$("$VV" --tab 2>&1 || true)
assert_contains "missing_arg_targeted_error"   "$MISSING_ARG" "requires an argument"
refute_contains "missing_arg_not_unknown_option" "$MISSING_ARG" "Unknown option"
# A genuinely unknown flag still reports "Unknown option".
UNKNOWN_OPT=$("$VV" --bogus-flag "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "unknown_option_still_reported" "$UNKNOWN_OPT" "Unknown option"

# --color accepts the space-separated form ("--color always"), not just
# "--color=always"; the space form used to be misread as a filename.
CLR_SPACE=$("$VV" --color always --no-interactive --no-index "$DATA/tiny.parquet" 2>&1)
assert_contains "color_space_separated_renders" "$CLR_SPACE" "$(printf '\033[')"
refute_contains "color_space_not_file_error"    "$CLR_SPACE" "Cannot open"
# NO_COLOR (https://no-color.org): disables colour when the mode is auto…
NOCOLOR_OUT=$(NO_COLOR=1 "$VV" --no-interactive --no-index "$DATA/tiny.parquet" 2>&1)
refute_contains "no_color_env_disables_color"   "$NOCOLOR_OUT" "$(printf '\033[')"
# …but an explicit --color=always still wins over NO_COLOR.
NOCOLOR_FORCE=$(NO_COLOR=1 "$VV" --color=always --no-interactive --no-index "$DATA/tiny.parquet" 2>&1)
assert_contains "no_color_overridden_by_always"  "$NOCOLOR_FORCE" "$(printf '\033[')"
# --image-mode validates its value at parse time (a typo is an error, exit 2).
IMG_BAD=$("$VV" --image-mode bogus --no-interactive "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "image_mode_typo_rejected" "$IMG_BAD" "unknown mode"

echo
# --theme: every built-in name parses cleanly; unknown names get a
# clear error and exit 2. Color output is exercised via --color=always
# so the ANSI escape strings show up in the captured output.
for theme in default dark light solarized-dark solarized-light solarized; do
    OUT=$("$VV" --theme "$theme" --color=always -n 1 --no-interactive "$DATA/tiny.parquet" 2>&1)
    RC=$?
    if [ $RC -eq 0 ] && printf '%s' "$OUT" | grep -q $'\033\['; then
        PASS=$((PASS+1)); echo "  ok    theme_${theme}_renders_ansi"
    else
        FAIL=$((FAIL+1)); echo "  FAIL  theme_${theme}_renders_ansi"
    fi
done
BAD_THEME=$("$VV" --theme bogus "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "theme_unknown_lists_choices" "$BAD_THEME" "default, dark, light"

# Config-file persistence: a theme set in ~/.config/vv/config (XDG) is
# picked up when --theme isn't passed; explicit --theme overrides.
TMP_XDG=$(mktemp -d)
mkdir -p "$TMP_XDG/vv"
printf 'theme = solarized-dark\n' > "$TMP_XDG/vv/config"
# solarized-dark uses 256-color "38;5;240" for borders, the default
# theme uses "90". Greping for the solarized escape proves the
# config was honoured (no --theme flag).
CFG_OUT=$(XDG_CONFIG_HOME="$TMP_XDG" "$VV" --schema --color=always "$DATA/tiny.parquet")
if printf '%s' "$CFG_OUT" | grep -q $'\033\[38;5;240m'; then
    PASS=$((PASS+1)); echo "  ok    theme_config_file_honoured"
else
    FAIL=$((FAIL+1)); echo "  FAIL  theme_config_file_honoured"
fi
# Explicit --theme wins over the config-file value.
OVR_OUT=$(XDG_CONFIG_HOME="$TMP_XDG" "$VV" --theme default --schema --color=always "$DATA/tiny.parquet")
if printf '%s' "$OVR_OUT" | grep -q $'\033\[90m' && \
   ! printf '%s' "$OVR_OUT" | grep -q $'\033\[38;5;240m'; then
    PASS=$((PASS+1)); echo "  ok    theme_cli_overrides_config"
else
    FAIL=$((FAIL+1)); echo "  FAIL  theme_cli_overrides_config"
fi
rm -rf "$TMP_XDG"

echo
# Multi-file CLI: extra positionals become TUI tabs. In non-interactive
# mode only the first file is processed; verify nothing crashes.
MULTI=$("$VV" --no-interactive --no-index --color=never -n 1 "$DATA/tiny.parquet" "$DATA/tiny.bed" 2>&1)
assert_contains "multifile_cli_accepts_extra_paths" "$MULTI" "Chr"
# An unopenable second positional must still error out cleanly.
MULTI_BAD=$("$VV" -i "$DATA/tiny.parquet" /no/such/file 2>&1 || true)
assert_contains "multifile_bad_second_path_errors" "$MULTI_BAD" "not found"

# ENCODE peak / signal family — extension dispatch + typed-column naming.
if [ -f "$DATA/tiny.narrowPeak" ]; then
    NP_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.narrowPeak" | wc -l)
    assert_eq_file_inline "narrowPeak_returns_three_rows" "$NP_ROWS" "3"
    NP_SCH=$("$VV" --schema "$DATA/tiny.narrowPeak" 2>&1)
    assert_contains "narrowPeak_has_signalValue"    "$NP_SCH" "signalValue"
    assert_contains "narrowPeak_has_pValue"         "$NP_SCH" "pValue"
    assert_contains "narrowPeak_has_qValue"         "$NP_SCH" "qValue"
    assert_contains "narrowPeak_has_peak"           "$NP_SCH" "peak"
    assert_contains "narrowPeak_footer"             "$NP_SCH" "narrowPeak (BED6+4)"
fi
if [ -f "$DATA/tiny.broadPeak" ]; then
    BP_SCH=$("$VV" --schema "$DATA/tiny.broadPeak" 2>&1)
    assert_contains "broadPeak_has_signalValue"     "$BP_SCH" "signalValue"
    assert_contains "broadPeak_footer"              "$BP_SCH" "broadPeak (BED6+3)"
    if printf '%s' "$BP_SCH" | grep -q "^peak"; then
        FAIL=$((FAIL+1)); echo "  FAIL  broadPeak_no_peak_column"
    else
        PASS=$((PASS+1)); echo "  ok    broadPeak_no_peak_column"
    fi
fi
if [ -f "$DATA/tiny.bedGraph" ]; then
    BG_SCH=$("$VV" --schema "$DATA/tiny.bedGraph" 2>&1)
    assert_contains "bedGraph_has_value_column"     "$BG_SCH" "value"
    assert_contains "bedGraph_footer"               "$BG_SCH" "Format: bedGraph"
fi

# SQLite: each table becomes a tab; --tsv on the file dumps the first table.
if [ -f "$DATA/tiny.sqlite" ]; then
    SQL_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.sqlite" | wc -l)
    assert_eq_file_inline "sqlite_first_table_rows" "$SQL_ROWS" "3"
    SQL_SCH=$("$VV" --schema "$DATA/tiny.sqlite" 2>&1)
    assert_contains "sqlite_footer_format"          "$SQL_SCH" "Format: SQLite"
    assert_contains "sqlite_footer_first_table"     "$SQL_SCH" "Table: peaks"
    assert_contains "sqlite_footer_sibling_count"   "$SQL_SCH" "+1 more"
    # Column types follow SQLite affinity (TEXT → string, INTEGER → int64,
    # REAL → double, declared NOT NULL preserved).
    assert_contains "sqlite_type_text"              "$SQL_SCH" "string"
    assert_contains "sqlite_type_int"               "$SQL_SCH" "int64"
    assert_contains "sqlite_type_real"              "$SQL_SCH" "double"
    # --filter works on declared-typed columns (SQLite REAL → Arrow double).
    SQL_FLT=$("$VV" --tsv --no-header --filter 'score > 5.0' "$DATA/tiny.sqlite" | wc -l)
    assert_eq_file_inline "sqlite_filter_by_real"   "$SQL_FLT" "2"
fi

# SQLite NUMERIC-affinity columns (DATE / DATETIME / NUMERIC / BOOLEAN) must be
# preserved verbatim, not coerced through double. Pre-fix, dates rendered as
# their leading year (2026) and the two distinct 2^53-exceeding integers both
# collapsed to 9.0072e+15.
if [ -f "$DATA/tiny.types.sqlite" ]; then
    TYPES_OUT=$("$VV" --tsv --no-header "$DATA/tiny.types.sqlite" 2>&1)
    assert_contains "sqlite_date_preserved"      "$TYPES_OUT" "2026-06-10"
    assert_contains "sqlite_datetime_preserved"  "$TYPES_OUT" "2026-06-10 12:34:56"
    assert_contains "sqlite_bigint_exact"        "$TYPES_OUT" "9007199254740993"
    assert_contains "sqlite_bigint_distinct"     "$TYPES_OUT" "9007199254740995"
fi

# SQLite identifier quoting: a table named a"b (embedded double quote) must be
# read, not produce the malformed/injectable SQL `"a"b"`. Exercises the PRAGMA,
# the SELECT * and the lazy COUNT(*) — all of which quote the table name.
if [ -f "$DATA/tiny.quoteid.sqlite" ]; then
    QID_OUT=$("$VV" --no-interactive --color=never "$DATA/tiny.quoteid.sqlite" 2>&1)
    assert_contains "sqlite_quoted_ident_reads"   "$QID_OUT" "hello"
    assert_contains "sqlite_quoted_ident_table"   "$QID_OUT" 'Table: a"b'
    assert_contains "sqlite_quoted_ident_count"   "$QID_OUT" "Rows: 2"
fi

# Apache ORC: columnar; one stripe → one chunk. The fixture is small so it
# lands in a single stripe even at stripe_size=2. The AlmaLinux 8 static
# build currently has ARROW_ORC=OFF, so probe for ORC support at runtime
# and skip these tests if vv was built without it.
if [ -f "$DATA/tiny.orc" ] && \
   ! "$VV" --schema "$DATA/tiny.orc" 2>&1 | grep -q "without Apache ORC"; then
    ORC_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.orc" | wc -l)
    assert_eq_file_inline "orc_rows"                "$ORC_ROWS" "3"
    ORC_SCH=$("$VV" --schema "$DATA/tiny.orc" 2>&1)
    assert_contains "orc_footer_format"             "$ORC_SCH" "Format: ORC"
    assert_contains "orc_footer_stripes"            "$ORC_SCH" "Stripes:"
    assert_contains "orc_footer_codec_zstd"         "$ORC_SCH" "Codec: zstd"
    assert_contains "orc_type_int"                  "$ORC_SCH" "int64"
    assert_contains "orc_type_real"                 "$ORC_SCH" "double"
    # --filter on the typed double column works as expected.
    ORC_FLT=$("$VV" --tsv --no-header --filter 'score > 5.0' "$DATA/tiny.orc" | wc -l)
    assert_eq_file_inline "orc_filter_by_real"      "$ORC_FLT" "2"
fi

# Excel (.xlsx): two sheets; first ("peaks") dumps via --tsv, schema shows
# sibling count, type inference catches the int / double / string mix from
# the cell contents.
if [ -f "$DATA/tiny.xlsx" ]; then
    XL_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.xlsx" | wc -l)
    assert_eq_file_inline "xlsx_first_sheet_rows"   "$XL_ROWS" "3"
    XL_SCH=$("$VV" --schema "$DATA/tiny.xlsx" 2>&1)
    assert_contains "xlsx_footer_format"            "$XL_SCH" "Format: Excel"
    assert_contains "xlsx_footer_first_sheet"       "$XL_SCH" "Sheet: peaks"
    assert_contains "xlsx_footer_sibling_count"     "$XL_SCH" "+1 more sheet"
    # Type inference from Arrow CSV reader on the cell-text stream.
    assert_contains "xlsx_type_text"                "$XL_SCH" "string"
    assert_contains "xlsx_type_int"                 "$XL_SCH" "int64"
    assert_contains "xlsx_type_real"                "$XL_SCH" "double"
    # --filter against the inferred double column.
    XL_FLT=$("$VV" --tsv --no-header --filter 'score > 5.0' "$DATA/tiny.xlsx" | wc -l)
    assert_eq_file_inline "xlsx_filter_by_real"     "$XL_FLT" "2"
fi

# OpenDocument Spreadsheet (.ods): two sheets; first ("peaks") dumps via
# --tsv, schema shows sibling count, types inferred from cell contents.
if [ -f "$DATA/tiny.ods" ]; then
    OD_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.ods" | wc -l)
    assert_eq_file_inline "ods_first_sheet_rows"    "$OD_ROWS" "3"
    OD_SCH=$("$VV" --schema "$DATA/tiny.ods" 2>&1)
    assert_contains "ods_footer_format"             "$OD_SCH" "Format: ODS"
    assert_contains "ods_footer_first_sheet"        "$OD_SCH" "Sheet: peaks"
    assert_contains "ods_footer_sibling_count"      "$OD_SCH" "+1 more sheet"
    assert_contains "ods_type_text"                 "$OD_SCH" "string"
    assert_contains "ods_type_int"                  "$OD_SCH" "int64"
    assert_contains "ods_type_real"                 "$OD_SCH" "double"
    OD_FLT=$("$VV" --tsv --no-header --filter 'score > 5.0' "$DATA/tiny.ods" | wc -l)
    assert_eq_file_inline "ods_filter_by_real"      "$OD_FLT" "2"
fi

# Ragged workbooks: a data row wider than the 3-column header. The sheet must
# parse in full (a wider row used to make Arrow reject the whole sheet) with the
# overflow column named "col4" and the extra value preserved.
if [ -f "$DATA/tiny.ragged.xlsx" ]; then
    RG_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.ragged.xlsx" | wc -l)
    assert_eq_file_inline "xlsx_ragged_rows"        "$RG_ROWS" "3"
    RG_OUT=$("$VV" --schema "$DATA/tiny.ragged.xlsx" 2>&1)
    assert_contains "xlsx_ragged_overflow_col"      "$RG_OUT" "col4"
    RG_TSV=$("$VV" --tsv "$DATA/tiny.ragged.xlsx" 2>&1)
    assert_contains "xlsx_ragged_wide_value"        "$RG_TSV" "EXTRA"
fi
if [ -f "$DATA/tiny.ragged.ods" ]; then
    RGO_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.ragged.ods" | wc -l)
    assert_eq_file_inline "ods_ragged_rows"         "$RGO_ROWS" "3"
    RGO_OUT=$("$VV" --schema "$DATA/tiny.ragged.ods" 2>&1)
    assert_contains "ods_ragged_overflow_col"       "$RGO_OUT" "col4"
    RGO_TSV=$("$VV" --tsv "$DATA/tiny.ragged.ods" 2>&1)
    assert_contains "ods_ragged_wide_value"         "$RGO_TSV" "EXTRA"
fi

# ODS table:number-rows-repeated on a non-empty row: the "dup" row (repeat=3)
# must expand to 3 rows, so the sheet has 5 data rows (a + dup×3 + z) instead of
# the 3 it dropped to before the fix.
if [ -f "$DATA/tiny.rowrep.ods" ]; then
    RR_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.rowrep.ods" | wc -l)
    assert_eq_file_inline "ods_rowrep_expands"      "$RR_ROWS" "5"
    RR_DUP=$("$VV" --tsv --no-header "$DATA/tiny.rowrep.ods" | grep -c '^dup')
    assert_eq_file_inline "ods_rowrep_dup_count"    "$RR_DUP" "3"
fi

# AnnData (.h5ad) — first tab is the summary; siblings are obs / var /
# X-preview / obsm[X_umap]. Footer reports sibling count.
if [ -f "$DATA/tiny.h5ad" ]; then
    H5AD_SCH=$("$VV" --schema "$DATA/tiny.h5ad" 2>&1)
    assert_contains "h5ad_footer_format"     "$H5AD_SCH" "Format: AnnData"
    assert_contains "h5ad_footer_siblings"   "$H5AD_SCH" "+4 more tab(s)"
    H5AD_OUT=$("$VV" --color=never --no-interactive "$DATA/tiny.h5ad" 2>&1)
    # Summary table has rows for format / X / obs / var / obsm.
    assert_contains "h5ad_summary_root"      "$H5AD_OUT" "AnnData"
    assert_contains "h5ad_summary_X"         "$H5AD_OUT" "csr_matrix"
    assert_contains "h5ad_summary_obs"       "$H5AD_OUT" "5 rows, 3 columns"
    assert_contains "h5ad_summary_var"       "$H5AD_OUT" "4 rows, 3 columns"
    assert_contains "h5ad_summary_obsm"      "$H5AD_OUT" "obsm"
fi

# Dense-X AnnData (.h5ad) — a 3 × 250 dense X. The summary reports the true
# shape; the X tab (a sibling, only materialised by the GUI / TUI) is capped to
# the 200-column dense preview so a wide dense matrix can't OOM the reader (the
# cap itself is asserted by the GUI CI job via VVG_TABDIMS).
if [ -f "$DATA/tiny.dense.h5ad" ]; then
    TIMES=$(printf '\xc3\x97')
    DENSE_OUT=$("$VV" --color=never --no-interactive "$DATA/tiny.dense.h5ad" 2>&1)
    assert_contains "h5ad_dense_summary_shape" "$DENSE_OUT" "3 $TIMES 250"
    assert_exit_zero "h5ad_dense_no_crash" \
        "$VV" --no-interactive --color=never "$DATA/tiny.dense.h5ad"
fi

# --tab: view a named AnnData component (obs/var/X) from the CLI — the data
# tabs are otherwise only reachable in the interactive TUI.
if [ -f "$DATA/tiny.h5ad" ]; then
    TAB_OBS=$("$VV" --tab obs --no-interactive --color=never "$DATA/tiny.h5ad" 2>&1)
    assert_contains "h5ad_tab_obs_col"     "$TAB_OBS" "cluster"
    assert_contains "h5ad_tab_obs_data"    "$TAB_OBS" "cell0"
    TAB_VAR=$("$VV" --tab var --schema "$DATA/tiny.h5ad" 2>&1)
    assert_contains "h5ad_tab_var"         "$TAB_VAR" "gene_name"
    # 'X' matches the 'X (preview)' tab via the word-boundary prefix rule.
    TAB_X=$("$VV" --tab X --schema "$DATA/tiny.h5ad" 2>&1)
    assert_contains "h5ad_tab_x_prefix"    "$TAB_X" "X (preview)"
    # X is (obs x var) = cells x genes: the preview names its value columns by
    # the var index (gene names) and prepends the obs index (cell barcodes) as
    # a row-label column, instead of generic col0/col1 and bare row numbers.
    X_VIEW=$("$VV" --no-interactive --color=never --tab X "$DATA/tiny.h5ad" 2>&1)
    assert_contains "anndata_x_var_column_names" "$X_VIEW" "gene0"
    assert_contains "anndata_x_obs_row_labels"   "$X_VIEW" "cell0"
    TAB_BAD=$("$VV" --tab nope "$DATA/tiny.h5ad" 2>&1)
    assert_contains "h5ad_tab_unknown"     "$TAB_BAD" "no tab named"
    assert_contains "h5ad_tab_unknown_avail" "$TAB_BAD" "available:"
    # An AnnData with no populated uns (anndata writes an empty uns group) must
    # NOT get a uns tab.
    refute_contains "anndata_empty_uns_no_tab" \
        "$("$VV" --tab uns "$DATA/tiny.h5ad" 2>&1)" "AnnData (uns)"
fi

# uns (unstructured) decoding: tiny.uns.h5ad has scalars (string/int/float), a
# string array, and a nested dict. The uns tab must surface each as a key/value
# row (nested dicts flattened with dotted keys), and the summary must count it.
if [ -f "$DATA/tiny.uns.h5ad" ]; then
    UNS=$("$VV" --no-interactive --color=never --tab uns "$DATA/tiny.uns.h5ad" 2>&1)
    assert_contains "anndata_uns_string_scalar"  "$UNS" "demo dataset"
    assert_contains "anndata_uns_int_scalar"     "$UNS" "n_pcs"
    assert_contains "anndata_uns_float_scalar"   "$UNS" "0.05"
    assert_contains "anndata_uns_string_array"   "$UNS" "#FF0000, #00FF00, #0000FF"
    assert_contains "anndata_uns_nested_dotkey"  "$UNS" "pca.variance_ratio"
    assert_contains "anndata_uns_nested_values"  "$UNS" "0.5, 0.3, 0.2"
    assert_contains "anndata_uns_footer"         "$UNS" "AnnData (uns)"
    UNS_SUM=$("$VV" --no-interactive --color=never "$DATA/tiny.uns.h5ad" 2>&1)
    assert_contains "anndata_uns_in_summary"     "$UNS_SUM" "6 entries"
fi

# CSC-sparse X: the preview must densify identically to CSR by walking each
# column's indptr range (here the indices are row indices). tiny.csc.h5ad's X is
# the fixed 3×4 matrix [[1,0,0,2],[0,3,0,0],[0,0,4,5]]; the summary must now
# offer a preview tab (no "not implemented" note) and the X-labels still apply.
if [ -f "$DATA/tiny.csc.h5ad" ]; then
    CSC_SUM=$("$VV" --no-interactive --color=never "$DATA/tiny.csc.h5ad" 2>&1)
    refute_contains "anndata_csc_no_unimpl_note" "$CSC_SUM" "not implemented"
    assert_contains "anndata_csc_summary_enc"    "$CSC_SUM" "csc_matrix"
    CSC_X=$("$VV" --no-interactive --color=never --tab X "$DATA/tiny.csc.h5ad" 2>&1)
    assert_contains "anndata_csc_x_gene_labels"  "$CSC_X" "gene0"
    assert_contains "anndata_csc_x_cell_labels"  "$CSC_X" "cell2"
    CSC_TSV=$("$VV" --tsv --no-header --tab X "$DATA/tiny.csc.h5ad" 2>/dev/null)
    assert_eq_file_inline "anndata_csc_exit0" "$?" "0"
    assert_contains "anndata_csc_row0_densified" \
        "$(printf '%s\n' "$CSC_TSV" | sed -n 1p)" "$(printf 'cell0\t1\t0\t0\t2')"
    assert_contains "anndata_csc_row2_densified" \
        "$(printf '%s\n' "$CSC_TSV" | sed -n 3p)" "$(printf 'cell2\t0\t0\t4\t5')"
fi

# Hostile AnnData: the CSR `X` group's shape attribute claims 100 rows but
# indptr holds only 2, and the obs DataFrame has unequal-length columns. vv
# must clamp to the real dataset extents (no OOB read) and normalise the
# columns (no invalid table), rendering a bounded preview instead of crashing.
if [ -f "$DATA/tiny.badsparse.h5ad" ]; then
    BADX=$("$VV" --tsv --no-header --tab X "$DATA/tiny.badsparse.h5ad" 2>/dev/null)
    assert_eq_file_inline "anndata_bad_sparse_exit0" "$?" "0"
    # Clamped to the 2 rows indptr actually describes, not the claimed 100.
    assert_eq_file_inline "anndata_bad_sparse_clamped_rows" \
        "$(printf '%s\n' "$BADX" | grep -c .)" "2"
    BADOBS=$("$VV" --tsv --no-header --tab obs "$DATA/tiny.badsparse.h5ad" 2>/dev/null)
    assert_eq_file_inline "anndata_bad_obs_exit0" "$?" "0"
    # 3 rows: the short column is null-padded to match the index column.
    assert_eq_file_inline "anndata_bad_obs_normalised_rows" \
        "$(printf '%s\n' "$BADOBS" | grep -c .)" "3"
fi

# AnnData obs/var: the TUI / table view shows a bounded 1000-row preview (so a
# multi-million-row component can't stall the reader), but a delimited export
# (--tsv/--csv) dumps the FULL component; -n still bounds the export.
if [ -f "$DATA/tiny.bigobs.h5ad" ]; then
    # Table view (non-export): capped preview, with the "first 1000 of 1500" note.
    BIG_OBS=$("$VV" --tab obs --no-interactive --color=never "$DATA/tiny.bigobs.h5ad" 2>&1)
    assert_contains "h5ad_obs_preview_note" "$BIG_OBS" "first 1000 of 1500 rows"
    # Delimited export: all 1500 rows (the preview cap no longer truncates the dump).
    BIG_ROWS=$("$VV" --tab obs --tsv --no-header "$DATA/tiny.bigobs.h5ad" | wc -l)
    assert_eq_file_inline "h5ad_obs_export_full_rows" "$(echo $BIG_ROWS)" "1500"
    # -n still limits the export.
    BIG_N=$("$VV" --tab obs --tsv --no-header -n 100 "$DATA/tiny.bigobs.h5ad" | wc -l)
    assert_eq_file_inline "h5ad_obs_export_head_limit" "$(echo $BIG_N)" "100"
    # Categorical obs columns decode to their string labels, not integer codes.
    # The dictionary cap (VV_CATEGORY_DICT_CAP, default 1,000,000 — raised from
    # 65536, which wrongly coded real high-cardinality columns like CRISPR
    # perturbation guides/targets) gates this; forcing it below the category
    # count falls back to "(codes)". (grp is a 3-category categorical.)
    CAT_HDR=$("$VV" --tab obs --tsv "$DATA/tiny.bigobs.h5ad" 2>/dev/null | head -1)
    CAT_ROW=$("$VV" --tab obs --tsv "$DATA/tiny.bigobs.h5ad" 2>/dev/null | sed -n '2p')
    refute_contains "h5ad_categorical_decoded"     "$CAT_HDR" "(codes)"
    assert_contains "h5ad_categorical_label_value" "$CAT_ROW" "$(printf '\tA\t')"
    CAT_CAP=$(VV_CATEGORY_DICT_CAP=2 "$VV" --tab obs --tsv "$DATA/tiny.bigobs.h5ad" 2>/dev/null | head -1)
    assert_contains "h5ad_categorical_cap_codes"   "$CAT_CAP" "grp (codes)"
fi

# Boolean obs/var columns are stored as HDF5 enums; they must render their
# member names, not the old "?" fallback. tiny.h5ad var.mt = [F,F,T,F].
if [ -f "$DATA/tiny.h5ad" ]; then
    VAR_MT=$("$VV" --tab var --tsv "$DATA/tiny.h5ad" 2>&1)
    assert_contains "h5ad_bool_col_rendered"  "$VAR_MT" "TRUE"
    refute_contains "h5ad_bool_col_not_qmark" "$VAR_MT" "?"
fi

# Generic HDF5 (.h5) — first tab is the hierarchy table; siblings are
# each 1D/2D dataset.
if [ -f "$DATA/tiny.h5" ]; then
    H5_SCH=$("$VV" --schema "$DATA/tiny.h5" 2>&1)
    assert_contains "h5_footer_format"       "$H5_SCH" "Format: HDF5 (hierarchy)"
    assert_contains "h5_footer_siblings"     "$H5_SCH" "+3 more tab(s)"
    H5_OUT=$("$VV" --color=never --no-interactive "$DATA/tiny.h5" 2>&1)
    assert_contains "h5_hierarchy_root"      "$H5_OUT" "/counts"
    TIMES=$(printf '\xc3\x97')   # × — assert_contains uses grep -F; bytes must be literal
    assert_contains "h5_hierarchy_matrix"    "$H5_OUT" "10 $TIMES 2"
    assert_contains "h5_hierarchy_dtype"     "$H5_OUT" "float64"
fi

# Malformed HDF5: a hostile AnnData whose /X "shape" attribute carries 16
# int64s and whose /obs "_index" is a 6-element string array. H5Aread fills
# one element per dataspace point, so the reader used to smash a fixed
# int64[2] / single-string buffer and segfault. Opening it must now exit 0
# (reads only the first two dims; treats the array string attr as absent).
if [ -f "$DATA/tiny.malformed.h5ad" ]; then
    assert_exit_zero "h5_malformed_shape_attr_no_crash" \
        "$VV" --schema "$DATA/tiny.malformed.h5ad"
    assert_exit_zero "h5_malformed_table_no_crash" \
        "$VV" --no-interactive --color=never "$DATA/tiny.malformed.h5ad"
fi

# Generic HDF5 1-D dataset preview cap: a 1500-element 1-D dataset must render
# the first 1000 rows only (so a multi-million-element array can't OOM/stall the
# reader), with the real length reported in the footer.
if [ -f "$DATA/tiny.big1d.h5" ]; then
    BIG1D=$("$VV" --no-interactive --color=never --tab /big "$DATA/tiny.big1d.h5" 2>&1)
    assert_contains "hdf5_1d_preview_cap_note" "$BIG1D" "first 1000 of 1500 rows"
    BIG1D_ROWS=$("$VV" --tsv --no-header --tab /big "$DATA/tiny.big1d.h5" 2>/dev/null | wc -l)
    assert_eq_file_inline "hdf5_1d_preview_cap_rows" "$(echo $BIG1D_ROWS)" "1000"
fi

# NumPy .npz — valid archive (summary lists each array) plus a malformed one.
if [ -f "$DATA/tiny.npz" ]; then
    NPZ_OUT=$("$VV" --no-interactive --color=never "$DATA/tiny.npz" 2>&1)
    assert_contains "npz_summary_vec"    "$NPZ_OUT" "vec"
    assert_contains "npz_summary_mat"    "$NPZ_OUT" "mat"
    assert_contains "npz_summary_cube"   "$NPZ_OUT" "cube"
fi
# Wide .npz: a 2-D array with 5000 columns. vv builds one Arrow column per
# declared column, so it must render only the first kNpzMaxCols (4096) and flag
# the truncation in the footer — otherwise a genuinely-wide (or hostile) array
# would allocate unboundedly.
if [ -f "$DATA/tiny.wide.npz" ]; then
    # The array is a sibling tab (the summary tab opens first); --tab selects it.
    NPZ_WIDE=$("$VV" --no-interactive --color=never --tab wide \
        "$DATA/tiny.wide.npz" 2>&1)
    assert_contains "npz_wide_col_cap_footer" "$NPZ_WIDE" \
        "showing first 4096 of 5000 columns"
    refute_contains "npz_wide_no_overflow_col" "$NPZ_WIDE" "c4096"
fi
# Malformed .npz: the member's .npy header declares shape (1000000000,) <f8
# (8 GB) but stores only 16 bytes. The reader derived element counts / byte
# offsets straight from the shape, driving an out-of-bounds read. vv must now
# reject it at open with a clear error instead of crashing or reading OOB.
if [ -f "$DATA/tiny.malformed.npz" ]; then
    NPZ_BAD=$("$VV" --schema "$DATA/tiny.malformed.npz" 2>&1 || true)
    assert_contains "npz_malformed_shape_rejected" "$NPZ_BAD" "shape"
    # Must fail cleanly (exit non-zero), never crash (signal → 128+).
    "$VV" --schema "$DATA/tiny.malformed.npz" >/dev/null 2>&1
    NPZ_RC=$?
    if [ "$NPZ_RC" -eq 0 ]; then
        FAIL=$((FAIL+1)); echo "  FAIL  npz_malformed_rejected_cleanly (accepted bad file)"
    elif [ "$NPZ_RC" -ge 128 ]; then
        FAIL=$((FAIL+1)); echo "  FAIL  npz_malformed_rejected_cleanly (crashed, signal $((NPZ_RC-128)))"
    else
        PASS=$((PASS+1)); echo "  ok    npz_malformed_rejected_cleanly"
    fi
fi

# NPZ allocation hint: the zip central-directory uncompressed_size is
# attacker-controllable. tiny.badsize.npz claims ~2 GiB for a 5-element array.
# vv must handle it gracefully — never a crash / OOM from reserving the bogus
# size. Whether the zip layer tolerates the size mismatch and reads the array
# (exit 0) or rejects it as malformed (clean non-zero) varies by minizip
# version, so accept either; the point is that it must not die by signal.
if [ -f "$DATA/tiny.badsize.npz" ]; then
    "$VV" --no-interactive --color=never "$DATA/tiny.badsize.npz" >/dev/null 2>&1
    BADSIZE_RC=$?
    if [ "$BADSIZE_RC" -lt 128 ]; then
        PASS=$((PASS+1)); echo "  ok    npz_badsize_no_crash"
    else
        FAIL=$((FAIL+1)); echo "  FAIL  npz_badsize_no_crash (signal $((BADSIZE_RC-128)))"
    fi
fi

# samtools mpileup: 6-col single-sample + 9-col two-sample fixtures; tabix
# range query on the bgzipped variant.
if [ -f "$DATA/tiny.mpileup" ]; then
    MP_ROWS=$("$VV" --tsv --no-header "$DATA/tiny.mpileup" | wc -l)
    assert_eq_file_inline "mpileup_rows"           "$MP_ROWS" "4"
    MP_SCH=$("$VV" --schema "$DATA/tiny.mpileup" 2>&1)
    assert_contains "mpileup_footer_format"        "$MP_SCH" "Format: mpileup"
    assert_contains "mpileup_footer_samples_1"     "$MP_SCH" "Samples: 1"
    assert_contains "mpileup_col_chrom"            "$MP_SCH" "chrom"
    assert_contains "mpileup_col_depth"            "$MP_SCH" "depth"
    # --filter on the typed depth column.
    MP_FLT=$("$VV" --tsv --no-header --filter 'depth >= 12' "$DATA/tiny.mpileup" | wc -l)
    assert_eq_file_inline "mpileup_filter_by_depth" "$MP_FLT" "3"
fi
if [ -f "$DATA/tiny.multi.mpileup" ]; then
    MM_SCH=$("$VV" --schema "$DATA/tiny.multi.mpileup" 2>&1)
    assert_contains "mpileup_multi_footer_samples" "$MM_SCH" "Samples: 2"
    assert_contains "mpileup_multi_col_depth_1"    "$MM_SCH" "depth_1"
    assert_contains "mpileup_multi_col_depth_2"    "$MM_SCH" "depth_2"
fi
if [ -f "$DATA/tiny.mpileup.gz" ] && [ -f "$DATA/tiny.mpileup.gz.tbi" ]; then
    # -r is UCSC 0-based half-open for every format, so 1-based pileup pos 100
    # is selected by the window [99, 100). (samtools would say chr1:100-100.)
    MP_REG=$("$VV" --tsv --no-header -r chr1:99-100 "$DATA/tiny.mpileup.gz" | wc -l)
    assert_eq_file_inline "mpileup_tabix_one_pos"  "$MP_REG" "1"
fi

# --decode-pileup: typed per-allele counts on the same single-sample file.
if [ -f "$DATA/tiny.mpileup" ]; then
    DEC_SCH=$("$VV" --schema --decode-pileup "$DATA/tiny.mpileup" 2>&1)
    assert_contains "decode_footer_format"          "$DEC_SCH" "mpileup (decoded)"
    assert_contains "decode_col_A"                  "$DEC_SCH" "A "
    assert_contains "decode_col_mean_qual"          "$DEC_SCH" "mean_qual"
    assert_contains "decode_type_mean_qual_double"  "$DEC_SCH" "double"
    # First row (chr1:100, ref=A): bases ".+2AC.,,..C,c-1G,..", depth 12.
    # Decoder sees 10 ref-matches (.+,) + 2 mismatch-Cs (C,c) + ins=1 + del=1.
    # fwd events: the 7 forward-strand chars (. and uppercase mismatches).
    DEC=$("$VV" --tsv --no-header --decode-pileup --filter 'pos == 100' \
        --select "A,C,ins,del,fwd,rev" "$DATA/tiny.mpileup")
    assert_eq_file_inline "decode_row_chr1_100"     "$DEC" "10	2	1	1	7	5"
    # --filter on the inferred int columns from the decoded schema.
    DEC_C=$("$VV" --tsv --no-header --decode-pileup --filter 'C >= 2' \
        --select 'chrom,pos' "$DATA/tiny.mpileup" | wc -l)
    assert_eq_file_inline "decode_filter_C_2plus"   "$DEC_C" "2"
fi
# Multi-sample: --decode-pileup produces per-sample suffixed columns.
if [ -f "$DATA/tiny.multi.mpileup" ]; then
    DM_SCH=$("$VV" --schema --decode-pileup "$DATA/tiny.multi.mpileup" 2>&1)
    assert_contains "decode_multi_col_A_1"          "$DM_SCH" "A_1"
    assert_contains "decode_multi_col_A_2"          "$DM_SCH" "A_2"
    assert_contains "decode_multi_col_mean_qual_2"  "$DM_SCH" "mean_qual_2"
fi

# --pileup on a BAM file: htslib's bam_plp engine emits the same per-base
# pileup that `samtools mpileup` does (without -f / -B). The fixture has
# three reads spanning chr1:100-119 (20 positions); pos 105 carries a
# uniform G mismatch on all reads.
if [ -f "$DATA/tiny.bam" ]; then
    PL_ROWS=$("$VV" --tsv --no-header --pileup "$DATA/tiny.bam" | wc -l)
    assert_eq_file_inline "bam_pileup_rows"        "$PL_ROWS" "20"
    PL_SCH=$("$VV" --schema --pileup "$DATA/tiny.bam" 2>&1)
    assert_contains "bam_pileup_footer_format"     "$PL_SCH" "mpileup (from BAM)"
    assert_contains "bam_pileup_col_bases"         "$PL_SCH" "bases"
    # Range query: only emit positions within the requested span. vv's -r is
    # UCSC 0-based half-open for every format, so 1-based pileup pos 105 is the
    # window [104, 105) (samtools' own -r is 1-based: chr1:105-105).
    PL_REG=$("$VV" --tsv --no-header --pileup -r chr1:104-105 "$DATA/tiny.bam" | wc -l)
    assert_eq_file_inline "bam_pileup_region_one"  "$PL_REG" "1"
    # Compare a single row byte-for-byte against samtools mpileup if samtools is
    # in PATH (skip otherwise — CI doesn't always ship it). The pileup *content*
    # must match; only the -r convention differs (vv 0-based [104,105) selects
    # the same base as samtools' 1-based chr1:105-105).
    if command -v samtools >/dev/null 2>&1; then
        VV_OUT=$("$VV" --tsv --no-header --pileup -r chr1:104-105 "$DATA/tiny.bam")
        SAM_OUT=$(samtools mpileup -r chr1:105-105 "$DATA/tiny.bam" 2>/dev/null)
        assert_eq_file_inline "bam_pileup_matches_samtools" "$VV_OUT" "$SAM_OUT"
    fi
    # --decode-pileup composes: pos 105 has 3 reads, all G — so G=3,
    # other allele counts zero; fwd=2 (uppercase G), rev=1 (lowercase g).
    DEC_BAM=$("$VV" --tsv --no-header --pileup --decode-pileup \
        --filter 'pos == 105' --select 'A,C,G,T,N,fwd,rev' "$DATA/tiny.bam")
    assert_eq_file_inline "bam_pileup_decoded_chr1_105" "$DEC_BAM" "0	0	3	0	0	2	1"
fi

# Reference skips (CIGAR N, e.g. RNA-seq introns) and deletions: the whole
# pileup must match samtools mpileup byte-for-byte — refskips render as '>'/'<'
# (strand), deletions as '*', and the quality column always carries the real
# base quality (never '*'). tiny.splice.bam has both, on both strands.
if [ -f "$DATA/tiny.splice.bam" ]; then
    # vv-only checks (no samtools needed — CI has pysam to build the fixture but
    # not samtools): the refskip row (pos 6) shows '>'/'<' with the real base
    # quality, not '*'.
    SP_SKIP=$("$VV" --tsv --no-header --pileup "$DATA/tiny.splice.bam" 2>/dev/null \
        | awk -F'\t' '$2==6')
    assert_contains "bam_pileup_refskip_bases" "$SP_SKIP" "><"
    assert_contains "bam_pileup_refskip_quals" "$SP_SKIP" "II"
    # Whole-pileup byte-for-byte against samtools when it's available (local).
    if command -v samtools >/dev/null 2>&1; then
        SP_VV=$("$VV" --tsv --no-header --pileup "$DATA/tiny.splice.bam" 2>/dev/null)
        SP_SAM=$(samtools mpileup "$DATA/tiny.splice.bam" 2>/dev/null)
        assert_eq_file_inline "bam_pileup_splice_matches_samtools" "$SP_VV" "$SP_SAM"
    fi
fi

# Markdown viewer — prose + GFM tables routed through the existing
# table renderer. The fixture tiny.md has two tables (one numeric,
# one string) and one of each block type.
if [ -f "$DATA/tiny.md" ]; then
    MD_OUT=$("$VV" --color=never "$DATA/tiny.md" 2>&1)
    assert_contains "md_heading_lvl1"        "$MD_OUT" "# tiny.md"
    assert_contains "md_heading_lvl2"        "$MD_OUT" "## Lists"
    # Each unordered-list item should carry the `•` bullet glyph — caught
    # a regression where tight-list rendering lost bullets entirely when
    # md4c skipped the MD_BLOCK_P inside MD_BLOCK_LI.
    BULLET=$(printf '\xe2\x80\xa2')
    assert_contains "md_list_bullet"         "$MD_OUT" "$BULLET alpha"
    assert_contains "md_list_bullet_beta"    "$MD_OUT" "$BULLET beta"
    assert_contains "md_list_ordered"        "$MD_OUT" "1. first"
    assert_contains "md_table_caption_1"     "$MD_OUT" "Benchmark table"
    assert_contains "md_table_caption_2"     "$MD_OUT" "Reference table"
    assert_contains "md_table_footer"        "$MD_OUT" "Format: markdown table"
    # First table has typed columns — int64 for rows, double for runtime_ms.
    assert_contains "md_table_col_int64"     "$MD_OUT" "int64"
    assert_contains "md_table_col_double"    "$MD_OUT" "double"
    # Image stub for the missing.png reference (kitty/iTerm protocol off
    # because $TERM_PROGRAM isn't set inside the test harness).
    assert_contains "md_image_stub"          "$MD_OUT" "[placeholder image]"
fi

# Terminal-injection defence: a hostile markdown file must not be able to emit
# raw control bytes (ESC/CSI/OSC/BEL) to the terminal — not from body text, and
# not via a link URL embedded in an OSC 8 escape. Render with colour forced on
# (exercises the escape-emitting paths) and confirm none of the planted control
# sequences survive, while the visible link text still renders.
MDINJ="$TMP/inject.md"
printf 'Heading\n\nNormal \x1b]0;PWNED\x07 text and a [click](http://e\x1b[31mvil.example).\n' \
    > "$MDINJ"
MD_INJ_OUT=$("$VV" --color=always "$MDINJ" 2>&1)
if printf '%s' "$MD_INJ_OUT" | grep -qaF $'\x1b]0;'; then
    FAIL=$((FAIL+1)); echo "  FAIL  md_no_osc_title_injection (raw OSC survived)"
else PASS=$((PASS+1)); echo "  ok    md_no_osc_title_injection"; fi
if printf '%s' "$MD_INJ_OUT" | grep -qaF $'\x07'; then
    FAIL=$((FAIL+1)); echo "  FAIL  md_no_bel_injection (raw BEL survived)"
else PASS=$((PASS+1)); echo "  ok    md_no_bel_injection"; fi
if printf '%s' "$MD_INJ_OUT" | grep -qaF $'\x1b[31mvil'; then
    FAIL=$((FAIL+1)); echo "  FAIL  md_no_url_csi_injection (raw CSI from URL survived)"
else PASS=$((PASS+1)); echo "  ok    md_no_url_csi_injection"; fi
assert_contains "md_injection_link_text_preserved" "$MD_INJ_OUT" "click"
rm -f "$MDINJ"

echo "── Threading parity ──────────────────────────────────────"
"$VV" --tsv --no-header -@ 1 "$DATA/tiny.parquet" > "$TMP/t1.out"
"$VV" --tsv --no-header -@ 4 "$DATA/tiny.parquet" > "$TMP/t4.out"
assert_eq_file "threads_1_vs_4_parquet" "$TMP/t1.out" "$TMP/t4.out"
# Decode-threads override produces identical output (parallelism is an
# implementation detail, results must match the serial run byte-for-byte).
"$VV" --tsv --no-header --decode-threads 8 "$DATA/tiny.parquet" > "$TMP/d8.out"
assert_eq_file "decode_threads_8_matches_t1" "$TMP/d8.out" "$TMP/t1.out"
"$VV" --tsv --no-header -@ 2 --decode-threads 16 "$DATA/tiny.parquet" > "$TMP/d16.out"
assert_eq_file "decode_threads_16_matches_t1" "$TMP/d16.out" "$TMP/t1.out"

echo
echo "── Heatmap (--heatmap) ───────────────────────────────────"
# Deterministic ASCII grid (stdout only — the status line goes to stderr).
"$VV" --heatmap --image-mode ascii "$DATA/tiny.parquet" 2>/dev/null > "$TMP/heatmap_ascii.out"
assert_eq_file "heatmap_ascii" "$TMP/heatmap_ascii.out" "$GOLDEN/heatmap_ascii.expected"
# Not a TTY (piped) + auto mode must fall back to ASCII — no escape sequences.
HM_PIPED=$("$VV" --heatmap "$DATA/tiny.parquet" 2>/dev/null | tr -dc '\033' | wc -c)
assert_eq_file_inline "heatmap_no_escapes_when_piped" "$HM_PIPED" "0"
# Unknown backend is rejected.
if "$VV" --heatmap --image-mode bogus "$DATA/tiny.parquet" >/dev/null 2>&1; then
    FAIL=$((FAIL + 1)); echo "  FAIL  heatmap_bad_mode_rejected (exit 0)"
else
    PASS=$((PASS + 1)); echo "  ok    heatmap_bad_mode_rejected"
fi
# Help documents the flags.
HM_HELP=$("$VV" --help 2>&1 || true)
assert_contains "help_has_heatmap"    "$HM_HELP" "--heatmap"
assert_contains "help_has_image_mode" "$HM_HELP" "--image-mode"

echo
echo "── TUI signal handling ───────────────────────────────────"
# Ctrl-C (SIGINT) in the interactive TUI must restore the terminal (endwin)
# before the process dies — otherwise the shell is left in raw/alt-screen mode.
# Driven under a pty by tui_sigint_check.py (needs python3; soft-skip otherwise).
if command -v python3 >/dev/null 2>&1; then
    if python3 "$HERE/tui_sigint_check.py" "$VV" "$DATA/tiny.parquet"; then
        PASS=$((PASS+1)); echo "  ok    tui_sigint_restores_terminal"
    else
        FAIL=$((FAIL+1)); echo "  FAIL  tui_sigint_restores_terminal"
    fi
else
    echo "  skip  tui_sigint_restores_terminal (python3 not found)"
fi

echo
echo "── Help / version ────────────────────────────────────────"
HELP_OUT=$("$VV" --help 2>&1 || true)
assert_contains "help_has_tagline" "$HELP_OUT" "vv -- universal genomic file viewer"
assert_contains "help_has_threads" "$HELP_OUT" "--threads"
assert_contains "help_has_region"  "$HELP_OUT" "--region"
VERSION_OUT=$("$VV" --version 2>&1 || true)
assert_contains "version_starts_with_vv" "$VERSION_OUT" "vv "

summarize
