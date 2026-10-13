#!/usr/bin/env bash
# Time vv against the usual alternative for each task, on the public inputs
# scripts/bench/fetch.sh downloads. Each command runs REPS times with its
# output discarded; the table reports the fastest wall time and the largest
# peak RSS. A tool that is not installed gets "-".
#
#   scripts/bench/run.sh [-v VV] [-n REPS] DIR
#
# Writes DIR/results.tsv and prints a Markdown table. Needs GNU time
# (/usr/bin/time, or gtime on macOS), python3 with scripts/bench/
# requirements.txt, and samtools / bcftools / tabix / UCSC bigWigToBedGraph
# for their rows.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
VV="$HERE/../../build/vv"
REPS=3
while getopts "v:n:h" o; do
    case $o in
        v) VV=$OPTARG ;;
        n) REPS=$OPTARG ;;
        *) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    esac
done
shift $((OPTIND - 1))
DIR=${1:?usage: run.sh [-v VV] [-n REPS] DIR}
cd "$DIR" || exit 1
TIME=$(command -v gtime || echo /usr/bin/time)
PY="python3 $HERE/comparators.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
OUT="$DIR/results.tsv"

# measure CMD: "seconds MB" — the fastest of REPS runs, the largest peak RSS;
# "- -" when its first word is not an installed command, "ERR ERR" (and its
# stderr on ours) when it fails.
measure() {
    local cmd="$1" first best="" mem=0 t m
    first=${cmd%% *}
    if ! command -v "$first" >/dev/null 2>&1 && [ ! -x "$first" ]; then echo "- -"; return; fi
    for _ in $(seq "$REPS"); do
        if ! "$TIME" -f "%e %M" -o "$TMP/t" bash -c "$cmd" >/dev/null 2>"$TMP/err"; then
            { echo "failed: $cmd"; tail -3 "$TMP/err"; } >&2
            echo "ERR ERR"; return
        fi
        read -r t m < <(tail -1 "$TMP/t")
        if [ -z "$best" ] || awk -v a="$t" -v b="$best" 'BEGIN{exit !(a < b)}'; then best=$t; fi
        [ "$m" -gt "$mem" ] && mem=$m
    done
    echo "$best $((mem / 1024))"
}

ROWS=()
row() {   # task, vv command, alternative's name, its command
    local task="$1" vcmd="$2" aname="$3" acmd="$4" v a
    v=$(measure "$vcmd")
    if [ -n "$acmd" ]; then a=$(measure "$acmd"); else a="- -"; fi
    printf '%s\t%s\t%s\t%s\n' "$task" "$v" "$aname" "$a" | tr ' ' '\t' >> "$OUT.tmp"
    ROWS+=("$task"$'\x1f'"${v% *}"$'\x1f'"${v#* }"$'\x1f'"$aname"$'\x1f'"${a% *}"$'\x1f'"${a#* }")
    printf '  %-46s vv %7s s %6s MB | %-28s %7s s %6s MB\n' "$task" ${v} "$aname" ${a} >&2
}

: > "$OUT.tmp"
VCF=1kg.chr22.vcf.gz BAM=NA12878.chr20.bam GFF=gencode.v47.basic.gff3.gz
PQ=gencode.v47.basic.parquet CSV=gencode.v47.basic.csv.gz FA=hg38.chr22.fa
BW=ENCFF247SXC.bigWig H5=breast_atlas.h5ad
echo "## VCF: 1000 Genomes phase 3 chr22 (1.1 M variants x 2,504 samples)" >&2
row "VCF: first 20 records" "$VV -n 20 -t $VCF" "bcftools view | head" "bcftools view -H $VCF | head -20"
row "VCF: -r 1 Mb, count" "$VV --count -r 22:20000000-21000000 $VCF" "bcftools view -H -r | wc -l" \
    "bcftools view -H -r 22:20000001-21000000 $VCF | wc -l"
row "VCF: sites to TSV (CHROM POS REF ALT)" "$VV --select CHROM,POS,REF,ALT --tsv $VCF" \
    "bcftools query" "bcftools query -f '%CHROM\t%POS\t%REF\t%ALT\n' $VCF"
echo "## BAM: NA12878 chr20, 1000 Genomes low coverage (3.2 M reads)" >&2
row "BAM: first 20 reads" "$VV -n 20 -t $BAM" "samtools view | head" "samtools view $BAM | head -20"
row "BAM: -r 1 Mb, count" "$VV --count -r 20:30000000-31000000 $BAM" "samtools view -c REGION" \
    "samtools view -c $BAM 20:30000001-31000000"
row "BAM: count MAPQ >= 30 (full scan)" "$VV --count --filter 'MAPQ >= 30' $BAM" "samtools view -c -q 30" \
    "samtools view -c -q 30 $BAM"
row "BAM: export to TSV/SAM" "$VV --tsv $BAM" "samtools view" "samtools view $BAM"
echo "## GFF3: GENCODE v47 basic (gzip)" >&2
row "GFF3: first 20 records" "$VV -n 20 -t $GFF" "zcat | head" "gzip -dc $GFF | head -20"
row "GFF3: count genes" "$VV --count --filter 'feature == \"gene\"' $GFF" "zcat | awk" \
    "gzip -dc $GFF | awk -F'\t' '\$3 == \"gene\"' | wc -l"
echo "## Parquet: GENCODE v47 basic, attributes as columns" >&2
row "Parquet: first 20 rows" "$VV -n 20 -t $PQ" "pyarrow iter_batches" "$PY parquet_head $PQ 20"
row "Parquet: --describe" "$VV --describe $PQ" "pandas describe()" "$PY parquet_describe $PQ"
row "Parquet: export to TSV" "$VV --tsv $PQ" "pyarrow write_csv" "$PY parquet_tsv $PQ"
row "Parquet: sort by start, first 5" "$VV --sort start -n 5 -t $PQ" "pandas sort_values" "$PY parquet_sort $PQ start 5"
row "Parquet: count exons" "$VV --count --filter 'feature == \"exon\"' $PQ" "pyarrow compute" \
    "$PY parquet_count_eq $PQ feature exon"
echo "## CSV.gz: the same table" >&2
row "CSV.gz: first 20 rows" "$VV -n 20 -t $CSV" "pandas read_csv(nrows=20)" "$PY csv_head $CSV 20"
row "CSV.gz: count rows" "$VV --count $CSV" "pyarrow read_csv" "$PY csv_count $CSV"
echo "## FASTA: hg38 chr22 with .fai" >&2
row "FASTA: -r 10 kb" "$VV --tsv -r chr22:20000000-20010000 $FA" "samtools faidx" \
    "samtools faidx $FA chr22:20000001-20010000"
echo "## bigWig: ENCODE ENCFF247SXC (fold change, 336 MB)" >&2
row "bigWig: -r 1 Mb, count" "$VV --count -r chr1:1000000-2000000 $BW" "pyBigWig intervals" \
    "$PY bigwig_region_count $BW chr1 1000000 2000000"
row "bigWig: -r 1 Mb, TSV" "$VV --tsv -r chr1:1000000-2000000 $BW" "bigWigToBedGraph" \
    "bigWigToBedGraph -chrom=chr1 -start=1000000 -end=2000000 $BW /dev/stdout"
echo "## AnnData: CELLxGENE breast cancer atlas (100,064 cells)" >&2
row "h5ad: obs, first 20 rows" "$VV --tab obs -n 20 -t $H5" "anndata backed obs.head" "$PY h5ad_obs_head $H5 20"
row "h5ad: obs to TSV" "$VV --tab obs --tsv $H5" "anndata obs.to_csv" "$PY h5ad_obs_tsv $H5"
row "h5ad: obs --describe" "$VV --tab obs --describe $H5" "pandas describe()" "$PY h5ad_obs_describe $H5"
row "h5ad: X, first 5 rows" "$VV --tab X -n 5 -t $H5" "anndata backed X[:5]" "$PY h5ad_x_head $H5 5"

mv "$OUT.tmp" "$OUT"
{
    echo
    echo "vv $("$VV" --version | awk '{print $2}'), $(uname -sr), $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //'), $(awk '/MemTotal/ {printf "%.0f GB RAM", $2/1048576}' /proc/meminfo 2>/dev/null); best of $REPS runs, warm cache, $(date +%F)"
    echo
    echo "| Task | vv | | Alternative | | |"
    echo "|---|---:|---:|---|---:|---:|"
    for r in "${ROWS[@]}"; do
        IFS=$'\x1f' read -r task vt vm an at am <<<"$r"
        unit() { case $1 in -|ERR) echo "$1" ;; *) echo "$1 $2" ;; esac; }
        printf '| %s | %s | %s | %s | %s | %s |\n' "$task" "$(unit "$vt" s)" "$(unit "$vm" MB)" \
            "${an//|/\\|}" "$(unit "$at" s)" "$(unit "$am" MB)"
    done
}
