#!/usr/bin/env python3
"""
Generate small fixture files for the smoke test suite.
Run from the repo root: `python3 tests/data/generate.py`.
"""
from __future__ import annotations
import gzip
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

try:
    import pyarrow as pa
    import pyarrow.parquet as pq
    import pyarrow.ipc as ipc
except ImportError:
    sys.exit("pyarrow is required: pip install pyarrow")

# ── Parquet ──────────────────────────────────────────────────────────────────
schema = pa.schema([
    ("Chr",   pa.string()),
    ("Start", pa.int64()),
    ("End",   pa.int64()),
    ("Score", pa.float32()),
    ("Tags",  pa.list_(pa.string())),
])
def gen_rows():
    rows = []
    # 12 sorted chr1 rows then 8 sorted chr2 rows (tabix requires sorted input).
    for i in range(12):
        rows.append(("chr1", 100 + i * 1000, 200 + i * 1000))
    for i in range(8):
        rows.append(("chr2", 100 + i * 1000, 200 + i * 1000))
    return rows

_rows = gen_rows()
_tag_cycle = [["promoter"], ["enhancer", "open"], [], ["TF"], ["promoter", "TF"]]
table = pa.table({
    "Chr":   [r[0] for r in _rows],
    "Start": [r[1] for r in _rows],
    "End":   [r[2] for r in _rows],
    "Score": [round(0.05 * i, 4) for i in range(20)],
    "Tags":  [_tag_cycle[i % len(_tag_cycle)] for i in range(20)],
}, schema=schema)
pq.write_table(table, HERE / "tiny.parquet", compression="snappy")

# ── Arrow IPC ────────────────────────────────────────────────────────────────
with pa.OSFile(str(HERE / "tiny.arrow"), "wb") as f:
    with ipc.new_file(f, schema) as w:
        # Two batches so we exercise lazy loading.
        w.write_batch(table.slice(0, 10).to_batches()[0])
        w.write_batch(table.slice(10, 10).to_batches()[0])

# ── BED ──────────────────────────────────────────────────────────────────────
# Use round() to avoid float32 noise leaking into the text fixture.
bed = "\n".join(
    f"{chrom}\t{start}\t{end}\tpeak_{i}\t{round(score, 4)}"
    for i, (chrom, start, end, score) in enumerate(
        zip(table["Chr"].to_pylist(),
            table["Start"].to_pylist(),
            table["End"].to_pylist(),
            table["Score"].to_pylist()))
) + "\n"
(HERE / "tiny.bed").write_text(bed)

# ── BED.gz + tabix ───────────────────────────────────────────────────────────
def have(cmd):
    return shutil.which(cmd) is not None

if have("bgzip") and have("tabix"):
    bed_gz = HERE / "tiny.bed.gz"
    if bed_gz.exists():
        bed_gz.unlink()
    subprocess.run(["bgzip", "-k", str(HERE / "tiny.bed")], check=True)
    subprocess.run(["tabix", "-f", "-p", "bed", str(bed_gz)], check=True)
else:
    print("warn: bgzip/tabix not found; skipping tiny.bed.gz", file=sys.stderr)

# ── VCF ──────────────────────────────────────────────────────────────────────
vcf = """##fileformat=VCFv4.2
##INFO=<ID=AF,Number=1,Type=Float,Description="Allele frequency">
#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO
chr1\t100\trs1\tA\tG\t30\tPASS\tAF=0.5
chr1\t500\t.\tC\tT\t40\tPASS\tAF=0.1
chr1\t1500\t.\tG\tA\t50\tPASS\tAF=0.3
chr2\t200\t.\tT\tC\t35\tPASS\tAF=0.2
"""
(HERE / "tiny.vcf").write_text(vcf)
if have("bgzip") and have("tabix"):
    vcf_gz = HERE / "tiny.vcf.gz"
    if vcf_gz.exists():
        vcf_gz.unlink()
    subprocess.run(["bgzip", "-k", str(HERE / "tiny.vcf")], check=True)
    subprocess.run(["tabix", "-f", "-p", "vcf", str(vcf_gz)], check=True)

# ── FASTA ────────────────────────────────────────────────────────────────────
fa = """>seq1 chromosome 1
ACGTACGTACGTACGT
ACGTACGTACGTACGT
>seq2
GGGCCCAAATTT
>seq3 mitochondrial
TTTAAAACCCCGGGGAAAA
"""
(HERE / "tiny.fa").write_text(fa)
with open(HERE / "tiny.fa", "rb") as src, gzip.open(HERE / "tiny.fa.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)

# ── FASTQ ────────────────────────────────────────────────────────────────────
fq = """@read1 lane=1
ACGTACGTACGTACGT
+
IIIIIIIIIIIIIIII
@read2
TTTTAAAA
+
HHHHGGGG
@read3 short
GGG
+
III
"""
(HERE / "tiny.fq").write_text(fq)
with open(HERE / "tiny.fq", "rb") as src, gzip.open(HERE / "tiny.fq.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)

# ── PAF (minimap2 pairwise alignments) ──────────────────────────────────────
paf = (
    "read1\t1000\t100\t900\t+\tchr1\t250000000\t5000\t5800\t750\t800\t60\tNM:i:50\n"
    "read2\t2000\t0\t2000\t-\tchr2\t300000000\t10000\t12000\t1900\t2000\t60\n"
    "read3\t500\t0\t500\t+\tchr1\t250000000\t30000\t30500\t480\t500\t30\tNM:i:20\tms:i:480\n"
)
(HERE / "tiny.paf").write_text(paf)
with open(HERE / "tiny.paf", "rb") as src, gzip.open(HERE / "tiny.paf.gz", "wb") as dst:
    shutil.copyfileobj(src, dst)

# ── BCF (binary VCF, requires bcftools) ──────────────────────────────────────
if have("bcftools"):
    # Make a self-contained VCF (with explicit ##contig lines) so bcftools is
    # happy; tiny.vcf above lacks those.
    bcf_input = HERE / "_bcf_input.vcf"
    bcf_input.write_text(
        "##fileformat=VCFv4.2\n"
        "##contig=<ID=chr1>\n"
        "##contig=<ID=chr2>\n"
        '##INFO=<ID=AF,Number=A,Type=Float,Description="Allele frequency">\n'
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n"
        "chr1\t100\trs1\tA\tG\t30\tPASS\tAF=0.5\n"
        "chr1\t500\t.\tC\tT\t40\tPASS\tAF=0.1\n"
        "chr1\t1500\t.\tG\tA\t50\tPASS\tAF=0.3\n"
        "chr2\t200\t.\tT\tC\t35\tPASS\tAF=0.2\n"
    )
    subprocess.run(["bcftools", "view", "-O", "b",
                    str(bcf_input), "-o", str(HERE / "tiny.bcf")], check=True)
    bcf_input.unlink()
else:
    print("warn: bcftools not found; skipping tiny.bcf", file=sys.stderr)

# ── TSV / CSV ────────────────────────────────────────────────────────────────
tsv = "name\tcount\tratio\nfoo\t100\t0.5\nbar\t250\t0.75\nbaz\t9999\t0.1\n"
(HERE / "tiny.tsv").write_text(tsv)
csv = tsv.replace("\t", ",")
(HERE / "tiny.csv").write_text(csv)

print("done; files in", HERE)
