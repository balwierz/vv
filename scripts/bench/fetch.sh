#!/usr/bin/env bash
# Download the public inputs of the vv benchmark into DIR and derive the
# files made from them. Resumable and idempotent: a file whose SHA-256
# matches is kept, a partial one is continued (curl -C -).
#
#   scripts/bench/fetch.sh DIR
#
# About 1.8 GB. Needs curl, bgzip, samtools, and vv (for the Parquet and
# CSV copies of the GENCODE annotation; VV=... picks the binary).
set -euo pipefail

DIR=${1:?usage: fetch.sh DIR}
VV=${VV:-$(cd "$(dirname "$0")/../.." && pwd)/build/vv}
mkdir -p "$DIR"
cd "$DIR"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

get() {   # file url sha256
    local f="$1" url="$2" want="$3"
    if [ -f "$f" ] && [ "$(sha256 "$f")" = "$want" ]; then echo "ok    $f"; return; fi
    echo "fetch $f"
    curl -fL --retry 3 -C - -o "$f" "$url"
    local got; got=$(sha256 "$f")
    if [ "$got" != "$want" ]; then
        echo "checksum mismatch for $f: got $got, want $want" >&2
        exit 1
    fi
}

G1K=https://ftp.1000genomes.ebi.ac.uk/vol1/ftp
get 1kg.chr22.vcf.gz \
    $G1K/release/20130502/ALL.chr22.phase3_shapeit2_mvncall_integrated_v5b.20130502.genotypes.vcf.gz \
    a90c16c4ff2b3196476d506ae13cb3047fae8670163c7c932c4b0239aef3daf5
get 1kg.chr22.vcf.gz.tbi \
    $G1K/release/20130502/ALL.chr22.phase3_shapeit2_mvncall_integrated_v5b.20130502.genotypes.vcf.gz.tbi \
    27de6b77af65d300bb968e8e372439deb949389e4395eb0dd251f9ba7d73bbed
get NA12878.chr20.bam \
    $G1K/phase3/data/NA12878/alignment/NA12878.chrom20.ILLUMINA.bwa.CEU.low_coverage.20121211.bam \
    dfc164c34dd94e1c46ea94cad915489171ae8913da200ca4e5cae03a554f1996
get NA12878.chr20.bam.bai \
    $G1K/phase3/data/NA12878/alignment/NA12878.chrom20.ILLUMINA.bwa.CEU.low_coverage.20121211.bam.bai \
    525860a53fa5e8264258006cff2f1f5f275c4014726192989d1dae26dd4810cf
get gencode.v47.basic.gff3.gz \
    https://ftp.ebi.ac.uk/pub/databases/gencode/Gencode_human/release_47/gencode.v47.basic.annotation.gff3.gz \
    3eb3297a734beb62920b8b3f634de66ceca40ff9c8c20be03f7d1c05c558aa8d
get hg38.chr22.fa.gz https://hgdownload.soe.ucsc.edu/goldenPath/hg38/chromosomes/chr22.fa.gz 05f9d97d6fbfd08a44ca45b50837ca2ae9c471f35ba79dffec04d2cb5eaaf695
get ENCFF247SXC.bigWig https://www.encodeproject.org/files/ENCFF247SXC/@@download/ENCFF247SXC.bigWig \
    1a8a170579b913e6871f2f63741a66d6ba30f912c7937001f139baa3460d2f91
get breast_atlas.h5ad https://datasets.cellxgene.cziscience.com/a9e2497a-0600-4bbd-8095-0e1271b10b1d.h5ad \
    5a91ae49f317b3d693efb7837335338fed76a81684c890506f879735597dc9c2

# Derived: an uncompressed, faidx-indexed chr22 for -r / samtools faidx; the
# GENCODE annotation as Parquet and as CSV.gz (attributes as columns).
if [ ! -s hg38.chr22.fa.fai ]; then
    echo "make  hg38.chr22.fa + .fai"
    gzip -dc hg38.chr22.fa.gz > hg38.chr22.fa
    samtools faidx hg38.chr22.fa
fi
if [ ! -s gencode.v47.basic.parquet ]; then
    echo "make  gencode.v47.basic.parquet"
    "$VV" --expand attributes --parquet gencode.v47.basic.parquet gencode.v47.basic.gff3.gz
fi
if [ ! -s gencode.v47.basic.csv.gz ]; then
    echo "make  gencode.v47.basic.csv.gz"
    "$VV" --csv gencode.v47.basic.parquet | gzip -6 > gencode.v47.basic.csv.gz
fi
echo "done: $DIR"
