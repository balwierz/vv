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
run_case paf_tsv      --tsv --no-header "$DATA/tiny.paf"
run_case paf_gz_tsv   --tsv --no-header "$DATA/tiny.paf.gz"
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
# Generic Parquet range queries: auto-detected chrom/start/end columns,
# plus the --region-cols override path.
PQ_REG=$("$VV" --tsv --no-header -r chr1:1000-2500 "$DATA/tiny.parquet" | wc -l)
assert_eq_file_inline "parquet_region_autodetect_two_rows" "$PQ_REG" "2"
PQ_REG_OV=$("$VV" --tsv --no-header -r chr1:1000-2500 --region-cols Chr,Start,End "$DATA/tiny.parquet" | wc -l)
assert_eq_file_inline "parquet_region_cols_override_two_rows" "$PQ_REG_OV" "2"
PQ_REG_BAD=$("$VV" -r chr1:0-100 --region-cols NoSuch,Start,End "$DATA/tiny.parquet" 2>&1 || true)
assert_contains "parquet_region_cols_unknown_errors" "$PQ_REG_BAD" "not found"

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
DESCRIBE_OUT=$("$VV" --describe "$DATA/tiny.lociss")
assert_contains "describe_has_columns_header" "$DESCRIBE_OUT" "Column"
assert_contains "describe_has_distinct_for_string" "$DESCRIBE_OUT" "Chromosome"
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
echo "── Help / version ────────────────────────────────────────"
HELP_OUT=$("$VV" --help 2>&1 || true)
assert_contains "help_has_tagline" "$HELP_OUT" "vv -- universal genomic file viewer"
assert_contains "help_has_threads" "$HELP_OUT" "--threads"
assert_contains "help_has_region"  "$HELP_OUT" "--region"
VERSION_OUT=$("$VV" --version 2>&1 || true)
assert_contains "version_starts_with_vv" "$VERSION_OUT" "vv "

summarize
