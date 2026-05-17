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
# Split into 4 row groups so generic-Parquet region-query tests actually
# exercise row-group pruning (otherwise everything fits in row group 0).
pq.write_table(table, HERE / "tiny.parquet", compression="snappy", row_group_size=5)

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
    # `bgzip -k` (keep input) only exists in htslib >= 1.14. Ubuntu 22.04
    # ships htslib 1.13. Stream form (`bgzip -c < src > out`) is portable.
    with open(HERE / "tiny.bed", "rb") as fin, open(bed_gz, "wb") as fout:
        subprocess.run(["bgzip", "-c"], stdin=fin, stdout=fout, check=True)
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
    with open(HERE / "tiny.vcf", "rb") as fin, open(vcf_gz, "wb") as fout:
        subprocess.run(["bgzip", "-c"], stdin=fin, stdout=fout, check=True)
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

# ── 2bit (UCSC sequence container, hand-rolled) ─────────────────────────────
# Minimal 32-bit little-endian 2bit file with three short sequences.
# Format reference: https://genome.ucsc.edu/FAQ/FAQformat.html#format7
#
# Bases pack into 2 bits: T=0, C=1, A=2, G=3 (MSB-first within each byte).
import struct
def _encode_2bit(seq: str) -> tuple[bytes, list[tuple[int,int]]]:
    # Returns (packed_dna, n_blocks). N-blocks are contiguous runs of Ns;
    # the packed bytes for those bases use any value (we use T=0 fill).
    code = {"T": 0, "C": 1, "A": 2, "G": 3, "t": 0, "c": 1, "a": 2, "g": 3}
    n_blocks = []
    nrun_start = None
    cleaned = []
    for i, b in enumerate(seq):
        if b in ("N", "n"):
            cleaned.append("T")
            if nrun_start is None: nrun_start = i
        else:
            cleaned.append(b)
            if nrun_start is not None:
                n_blocks.append((nrun_start, i - nrun_start))
                nrun_start = None
    if nrun_start is not None:
        n_blocks.append((nrun_start, len(seq) - nrun_start))
    cleaned_s = "".join(cleaned)
    out = bytearray()
    for i in range(0, len(cleaned_s), 4):
        v = 0
        for j in range(4):
            v <<= 2
            if i + j < len(cleaned_s):
                v |= code[cleaned_s[i + j]]
        out.append(v)
    return bytes(out), n_blocks

_seqs = [
    ("chr1", "ACGTACGTACGTACGTACGT"),                # 20 bp, no Ns
    ("chr2", "ACGTNNNACGTACGTACGTACGTACGT"),         # 27 bp, one N-run
    ("chrM", "GGGCCCAAATTT"),                        # 12 bp
]
# Compute layout: header (16) + index entries (1+name+4 each) +
# per-seq seqRecords. We need the offsets before writing — compute
# in two passes.
index_bytes = sum(1 + len(n) + 4 for n, _ in _seqs)
header_size = 16
cursor = header_size + index_bytes
seq_records = []
offsets = []
for _name, _seq in _seqs:
    offsets.append(cursor)
    dna, nb = _encode_2bit(_seq)
    rec = struct.pack("<I", len(_seq))                    # dnaSize
    rec += struct.pack("<I", len(nb))                     # nBlockCount
    rec += b"".join(struct.pack("<I", s) for s, _ in nb)  # nBlockStarts
    rec += b"".join(struct.pack("<I", l) for _, l in nb)  # nBlockSizes
    rec += struct.pack("<I", 0)                           # maskBlockCount
    rec += struct.pack("<I", 0)                           # reserved
    rec += dna
    seq_records.append(rec)
    cursor += len(rec)

out = bytearray()
out += struct.pack("<IIII", 0x1A412743, 0, len(_seqs), 0)
for (name, _), off in zip(_seqs, offsets):
    nb = name.encode()
    out.append(len(nb))
    out += nb
    out += struct.pack("<I", off)
for rec in seq_records:
    out += rec
(HERE / "tiny.2bit").write_bytes(bytes(out))

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

# ── LociSSD (Parquet + lociSSD_manifest in file-level KV metadata) ──────────
# Reference: /home/piotr/Sources/LociSSD/FORMAT_SPEC.md
# Only the manifest's presence matters for vv detection; we hand-craft a
# minimal valid manifest here instead of pulling in the full lociSSD package.
import json
loc = pa.table({
    "Chromosome":  pa.array(["chr1","chr1","chr1","chr2","chr2"], pa.string()),
    "Start":       pa.array([100, 500, 1000, 200, 1500], pa.int32()),
    "End":         pa.array([200, 800, 1200, 400, 1800], pa.int32()),
    "Name":        pa.array(["peak_0","peak_1","peak_2","peak_3","peak_4"], pa.string()),
    "Score":       pa.array([0.5, 0.7, 0.2, 0.9, 0.1], pa.float64()),
    "MaxEndSoFar": pa.array([200, 800, 1200, 400, 1800], pa.int32()),
})
manifest = json.dumps({
    "format_version": 2,
    "writer_version": "vv tests/data/generate.py",
    "created_utc":    "2026-05-07T00:00:00+00:00",
    "row_count":      5,
    "chromosomes": [
        {"name": "chr1", "rows": 3, "row_offset": 0,
         "min_start": 100,  "max_start": 1000, "max_end": 1200},
        {"name": "chr2", "rows": 2, "row_offset": 3,
         "min_start": 200,  "max_start": 1500, "max_end": 1800},
    ],
    "sort_keys": ["Chromosome", "Start", "End"],
    "default_compression": ["zstd", 3],
    "coord_dtype": "int32",
})
loc = loc.replace_schema_metadata({"lociSSD_manifest": manifest})
# Force 2 row groups (rows 0..1 and 2..4) so vv's row-group statistics
# pruning is actually exercised by region-query tests.
pq.write_table(loc, HERE / "tiny.lociss", compression="zstd", row_group_size=2)

# ── SQLite (two tables: peaks + samples) ────────────────────────────────────
import sqlite3
sqlite_path = HERE / "tiny.sqlite"
if sqlite_path.exists():
    sqlite_path.unlink()
con = sqlite3.connect(sqlite_path)
cur = con.cursor()
cur.execute("""CREATE TABLE peaks(
    chrom TEXT NOT NULL, start INTEGER, end INTEGER,
    score REAL, name TEXT)""")
cur.executemany("INSERT INTO peaks VALUES(?,?,?,?,?)", [
    ("chr1", 100, 200, 5.2, "p1"),
    ("chr1", 500, 800, 8.1, "p2"),
    ("chr2", 1000, 1300, 3.4, "p3"),
])
cur.execute("""CREATE TABLE samples(
    sample_id INTEGER PRIMARY KEY, name TEXT, depth REAL)""")
cur.executemany("INSERT INTO samples VALUES(?,?,?)", [
    (1, "sampleA", 12.5),
    (2, "sampleB", 8.7),
    (3, "sampleC", 15.1),
])
con.commit()
con.close()

# ── Excel (.xlsx, two sheets, requires openpyxl) ─────────────────────────────
# Soft-skip if openpyxl isn't installed (CI installs it in the venv alongside
# pyarrow; local dev can `pip install openpyxl` to regenerate).
try:
    import openpyxl                                        # type: ignore
except ImportError:
    print("warn: openpyxl not found; skipping tiny.xlsx", file=sys.stderr)
else:
    xlsx_path = HERE / "tiny.xlsx"
    if xlsx_path.exists():
        xlsx_path.unlink()
    wb = openpyxl.Workbook()
    # First sheet — replace the default name to assert sheet-naming works.
    s1 = wb.active
    assert s1 is not None
    s1.title = "peaks"
    s1.append(["chrom", "start", "end", "score", "name"])
    s1.append(["chr1", 100, 200, 5.2, "p1"])
    s1.append(["chr1", 500, 800, 8.1, "p2"])
    s1.append(["chr2", 1000, 1300, 3.4, "p3"])
    # Second sheet — exercises sibling-tab expansion in main().
    s2 = wb.create_sheet("samples")
    s2.append(["sample_id", "name", "depth"])
    s2.append([1, "sampleA", 12.5])
    s2.append([2, "sampleB", 8.7])
    s2.append([3, "sampleC", 15.1])
    wb.save(xlsx_path)

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
    subprocess.run(["bcftools", "index", "-f",
                    str(HERE / "tiny.bcf")], check=True)
    bcf_input.unlink()
else:
    print("warn: bcftools not found; skipping tiny.bcf", file=sys.stderr)

# ── bigBed / bigWig ──────────────────────────────────────────────────────────
# Need UCSC's bedToBigBed / bedGraphToBigWig. They're in the
# `ucsc-bedtobigbed` and `ucsc-bedgraphtobigwig` Bioconda packages, on
# Arch under `kentutils`, in /opt/ucsc-kent-genome-tools on this dev box.
def find_kent_tool(name):
    if shutil.which(name): return name
    cand = "/opt/ucsc-kent-genome-tools/" + name
    if (HERE.parent.parent.parent / cand[1:]).exists() or shutil.which(cand): return cand
    import os
    if os.path.exists(cand): return cand
    return None

bb_tool = find_kent_tool("bedToBigBed")
bw_tool = find_kent_tool("bedGraphToBigWig")
if bb_tool and bw_tool:
    sizes = HERE / "_sizes.txt"
    sizes.write_text("chr1\t50000\nchr2\t50000\n")
    # bigBed with BED6+3 autoSql so the autoSql parser is actually exercised.
    bb_bed = HERE / "_peaks.bed"
    bb_bed.write_text(
        "chr1\t100\t200\tpeak_0\t500\t+\t12.5\t30.2\t25.1\n"
        "chr1\t500\t800\tpeak_1\t800\t-\t8.0\t15.0\t12.0\n"
        "chr1\t1000\t1200\tpeak_2\t300\t+\t5.5\t8.1\t7.0\n"
        "chr2\t200\t400\tpeak_3\t950\t.\t20.1\t40.5\t35.2\n"
        "chr2\t1500\t1800\tpeak_4\t150\t+\t2.0\t3.0\t2.5\n"
    )
    bb_as = HERE / "_peaks.as"
    bb_as.write_text(
        'table bigBed6Plus3\n'
        '"BED6 + 3 narrow-peak columns"\n'
        '(\n'
        '    string chrom;        "Reference sequence"\n'
        '    uint   chromStart;   "Start"\n'
        '    uint   chromEnd;     "End"\n'
        '    string name;         "Item name"\n'
        '    uint   score;        "0..1000"\n'
        '    char[1] strand;      "+/-/."\n'
        '    float  signalValue;  "Enrichment"\n'
        '    float  pValue;       "-log10 p"\n'
        '    float  qValue;       "-log10 q"\n'
        ')\n')
    subprocess.run([bb_tool, "-type=bed6+3", "-as=" + str(bb_as),
                    str(bb_bed), str(sizes), str(HERE / "tiny.bb")], check=True)
    # bigWig from a bedGraph.
    bg = HERE / "_signal.bedGraph"
    bg.write_text(
        "chr1\t100\t200\t0.5\n"
        "chr1\t500\t800\t0.7\n"
        "chr1\t1000\t1200\t0.2\n"
        "chr2\t200\t400\t0.9\n"
        "chr2\t1500\t1800\t0.1\n"
    )
    subprocess.run([bw_tool, str(bg), str(sizes), str(HERE / "tiny.bw")], check=True)
    sizes.unlink(); bb_bed.unlink(); bb_as.unlink(); bg.unlink()
else:
    print("warn: bedToBigBed / bedGraphToBigWig not found; "
          "skipping tiny.bb and tiny.bw", file=sys.stderr)

# ── TSV / CSV ────────────────────────────────────────────────────────────────
tsv = "name\tcount\tratio\nfoo\t100\t0.5\nbar\t250\t0.75\nbaz\t9999\t0.1\n"
(HERE / "tiny.tsv").write_text(tsv)
csv = tsv.replace("\t", ",")
(HERE / "tiny.csv").write_text(csv)

print("done; files in", HERE)
