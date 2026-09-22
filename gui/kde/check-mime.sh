#!/usr/bin/env bash
# Validate the shared-mime-info package (vv-formats.xml) and the MIME lists of
# the .desktop entry, the Dolphin thumbnailer and the KFileMetaData extractor.
#
#   gui/kde/check-mime.sh <path/to/vvkdetest>
#
# Builds a private MIME database from vv-formats.xml (update-mime-database into
# a temporary XDG_DATA_HOME, on top of the system database) and checks:
#   1. real files resolve to the expected type by name + content, including the
#      glob collisions with text/vcard (*.vcf) and application/x-amipro (*.sam);
#   2. every type the .desktop entry, thumbnailer or extractor lists is known;
#   3. the thumbnailer and extractor list only types the .desktop entry lists,
#      the extractor's .json and .cpp lists agree, and neither plugin handles
#      CRAM (decoding may fetch the reference over the network) or FASTA (a
#      first record can be a whole chromosome).
set -euo pipefail

vvkdetest=$1
here=$(cd "$(dirname "$0")" && pwd)
data=$(cd "$here/../../tests/data" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/share/mime/packages"
cp "$here/vv-formats.xml" "$tmp/share/mime/packages/"
update-mime-database "$tmp/share/mime" >/dev/null 2>&1
export XDG_DATA_HOME="$tmp/share"
export XDG_DATA_DIRS="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export QT_QPA_PLATFORM=offscreen

fail=0
f=$tmp/files
mkdir -p "$f"
printf 'BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Jane Doe\r\nEND:VCARD\r\n' > "$f/contact.vcf"
printf '@HD\tVN:1.6\n@SQ\tSN:chr1\tLN:100\nr1\t0\tchr1\t1\t60\t4M\t*\t0\t0\tACGT\tIIII\n' > "$f/aln.sam"
printf 'r1\t0\tchr1\t1\t60\t4M\t*\t0\t0\tACGT\tIIII\n' > "$f/noheader.sam"
printf '##gff-version 3\nchr1\tsrc\tgene\t1\t100\t.\t+\t.\tID=g1\n' > "$f/genes.gff3"
printf '##gff-version 3\nchr1\tsrc\tgene\t1\t100\t.\t+\t.\tID=g1\n' | gzip > "$f/genes.gff3.gz"
printf '%%%%MatrixMarket matrix coordinate integer general\n2 2 1\n1 1 5\n' > "$f/matrix.mtx"
gzip -c "$f/matrix.mtx" > "$f/matrix.mtx.gz"
printf 'chr1\t0\t10\t1.5\n' > "$f/signal.bg"
cp "$data/tiny.vcf" "$f/variants-no-extension"

expect=(
  "$f/contact.vcf"              text/vcard
  "$data/tiny.vcf"              text/x-vcf
  "$f/variants-no-extension"    text/x-vcf
  "$data/tiny.vcf.gz"           application/x-compressed-vcf
  "$data/tiny.bcf"              application/x-bcf
  "$data/tiny.bam"              application/x-bam
  "$data/tiny.cram"             application/x-cram
  "$f/aln.sam"                  text/x-sam
  "$f/noheader.sam"             text/x-sam
  "$data/tiny.paf"              text/x-paf
  "$data/tiny.paf.gz"           application/x-compressed-paf
  "$data/tiny.mpileup"          text/x-mpileup
  "$data/tiny.mpileup.gz"       application/x-compressed-mpileup
  "$data/tiny.bed"              text/x-bed
  "$data/tiny.bed.gz"           application/x-compressed-bed
  "$data/tiny.narrowPeak"       text/x-bed
  "$data/tiny.broadPeak"        text/x-bed
  "$data/tiny.bedGraph"         text/x-bedgraph
  "$f/signal.bg"                text/x-bedgraph
  "$f/genes.gff3"               text/x-gff
  "$f/genes.gff3.gz"            application/x-compressed-gff
  "$data/tiny.gtf"              text/x-gtf
  "$data/tiny.bw"               application/x-bigwig
  "$data/tiny.bb"               application/x-bigbed
  "$data/tiny.fa"               text/x-fasta
  "$data/tiny.fa.gz"            application/x-compressed-fasta
  "$data/tiny.fq"               text/x-fastq
  "$data/tiny.fq.gz"            application/x-compressed-fastq
  "$data/tiny.2bit"             application/x-2bit
  "$f/matrix.mtx"               text/x-matrix-market
  "$f/matrix.mtx.gz"            application/x-compressed-matrix-market
  "$data/tiny.parquet"          application/vnd.apache.parquet
)
files=()
for (( i = 0; i < ${#expect[@]}; i += 2 )); do files+=("${expect[i]}"); done
mapfile -t got < <("$vvkdetest" --mime "${files[@]}" | cut -f2)
for (( i = 0; i < ${#expect[@]}; i += 2 )); do
  want=${expect[i+1]} have=${got[i/2]:-}
  if [[ $have != "$want" ]]; then
    echo "FAIL: $(basename "${expect[i]}") resolves to '$have', expected '$want'"
    fail=1
  fi
done

# The MIME lists of the three consumers.
desktop=$(sed -n 's/^MimeType=//p' "$here/org.vv.Viewer.desktop" | tr ';' '\n' | sed '/^$/d' | sort)
thumb=$(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["MimeTypes"]))' \
          "$here/vvthumbnail.json" | sort)
extr_json=$(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["MimeTypes"]))' \
          "$here/vvextractor.json" | sort)
extr_cpp=$(grep -o 'QStringLiteral("[^"]*/[^"]*")' "$here/vvextractor.cpp" \
          | sed 's/QStringLiteral("\(.*\)")/\1/' | sort)

# shellcheck disable=SC2086
if ! "$vvkdetest" --mime-types $desktop; then fail=1; fi
extra=$(comm -23 <(printf '%s\n' "$thumb" "$extr_json" | sort -u) <(printf '%s\n' "$desktop"))
if [[ -n $extra ]]; then echo "FAIL: plugin types missing from the .desktop entry:"; echo "$extra"; fail=1; fi
if [[ $extr_json != "$extr_cpp" ]]; then
  echo "FAIL: vvextractor.json and vvextractor.cpp list different types:"
  diff <(echo "$extr_json") <(echo "$extr_cpp") || true
  fail=1
fi
for t in application/x-cram text/x-fasta application/x-compressed-fasta; do
  if grep -qx "$t" <<<"$thumb"$'\n'"$extr_json"; then
    echo "FAIL: $t must not be handled by the thumbnailer / extractor"; fail=1
  fi
  if ! grep -qx "$t" <<<"$desktop"; then echo "FAIL: $t missing from the .desktop entry"; fail=1; fi
done

if (( fail )); then exit 1; fi
echo "MIME check passed: $(( ${#expect[@]} / 2 )) files, $(wc -l <<<"$desktop") .desktop types, $(wc -l <<<"$thumb") thumbnailer types, $(wc -l <<<"$extr_json") extractor types"
