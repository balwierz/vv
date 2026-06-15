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

# tiny.uint32.parquet: Start/End stored as UInt32 (a common compact encoding
# for genomic positions). The region predicate used to read only Int32/Int64
# and returned 0 for any other width, silently emptying the result.
pq.write_table(pa.table({
    "Chr":   pa.array(["chr1", "chr1", "chr1", "chr2"], pa.string()),
    "Start": pa.array([100, 1100, 2100, 500], pa.uint32()),
    "End":   pa.array([200, 1200, 2200, 600], pa.uint32()),
    "Score": pa.array([0.0, 0.1, 0.2, 0.3], pa.float64()),
}), HERE / "tiny.uint32.parquet")

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

# tiny.malformed.2bit: a header declaring 0xFFFFFFFF sequences over a 16-byte
# (header-only) file. The reader used to reserve() ~170 GB for the index up
# front (abort/OOM on systems without memory overcommit); it must now reject
# the impossible count cleanly.
(HERE / "tiny.malformed.2bit").write_bytes(
    struct.pack("<IIII", 0x1A412743, 0, 0xFFFFFFFF, 0))

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

# tiny.types.sqlite: exercises NUMERIC-affinity columns (DATE / NUMERIC /
# DATETIME / BOOLEAN). These used to be mapped to Arrow double and read via
# sqlite3_column_double, which corrupted text dates ('2026-06-10' → 2026.0)
# and rounded 64-bit integers beyond a double's 2^53 exact range.
types_path = HERE / "tiny.types.sqlite"
if types_path.exists():
    types_path.unlink()
con = sqlite3.connect(types_path)
cur = con.cursor()
cur.execute("""CREATE TABLE events(
    d DATE, ts DATETIME, big NUMERIC, flag BOOLEAN, label TEXT)""")
cur.executemany("INSERT INTO events VALUES(?,?,?,?,?)", [
    # 9007199254740993 == 2**53 + 1, NOT exactly representable as a double.
    ("2026-06-10", "2026-06-10 12:34:56", 9007199254740993, 1, "alpha"),
    ("2025-01-02", "2025-01-02 00:00:00", 9007199254740995, 0, "beta"),
])
con.commit()
con.close()

# ── BAM (for --pileup tests; needs pysam) ────────────────────────────────────
# Generate a sorted + indexed BAM with three reads on chr1:100 covering
# positions 100–119, so the pileup engine emits 20 rows when invoked.
try:
    import pysam                                              # type: ignore
except ImportError:
    print("warn: pysam not found; skipping tiny.bam", file=sys.stderr)
else:
    bam_path = HERE / "tiny.bam"
    if bam_path.exists():
        bam_path.unlink()
    bai_path = HERE / "tiny.bam.bai"
    if bai_path.exists():
        bai_path.unlink()
    bam_header = {
        "HD": {"VN": "1.6", "SO": "coordinate"},
        "SQ": [
            {"SN": "chr1", "LN": 1000},
            {"SN": "chr2", "LN": 1000},
        ],
    }
    # 3 reads spanning chr1:100-119 with deliberate mismatches at pos 105
    # so the decoded pileup shows non-zero off-ref counts.
    reads = [
        # forward strand, 20bp, all match-T except pos 105 (offset 5) = G
        ("r1", 99,  "TTTTTGTTTTTTTTTTTTTT", 0,    True),
        # reverse strand, 20bp, all match-T except pos 105 = G
        ("r2", 99,  "TTTTTGTTTTTTTTTTTTTT", 16,   True),
        # forward strand starting at pos 102, only 17bp long (covers 102-118)
        ("r3", 101, "TTTGTTTTTTTTTTTTT",     0,    True),
    ]
    with pysam.AlignmentFile(str(bam_path), "wb", header=bam_header) as bf:
        for (qn, pos0, seq, flag, _) in reads:
            r = pysam.AlignedSegment(header=bf.header)
            r.query_name = qn
            r.flag = flag
            r.reference_id = 0
            r.reference_start = pos0
            r.mapping_quality = 60
            r.cigarstring = f"{len(seq)}M"
            r.query_sequence = seq
            r.query_qualities = pysam.qualitystring_to_array("I" * len(seq))
            r.next_reference_id = -1
            r.next_reference_start = -1
            r.template_length = 0
            bf.write(r)
    pysam.index(str(bam_path))

# ── samtools mpileup (single-sample + two-sample fixtures) ──────────────────
# Real samtools mpileup output. Six columns for single-sample; the
# two-sample variant has 3 + 3*2 = 9 columns.
# Hand-crafted to be self-consistent: depth == count of base events in the
# bases column == length of the quals column (after the decoder consumes
# `^X` start markers, `$` end markers, and `+N<seq>` / `-N<seq>` indels).
# Row 1 carries a single mismatch (C) on each strand, plus one insertion
# and one deletion attached to forward-strand reads, so the decoded view
# has non-zero values across the allele / indel columns.
mpileup_single = (
    "chr1\t100\tA\t12\t.+2AC.,,..C,c-1G,..\tIHGFGHIGFGFG\n"
    "chr1\t101\tG\t13\t..,GG.gg..,.,\tIHHHGGGHGFFFG\n"
    "chr1\t102\tT\t8\t..,..,,.\tHHHGGGFG\n"
    "chr2\t500\tC\t20\t..,,...,..,.,..,..,.\tHGFFGGGFGGFFGGFFGGFG\n"
)
(HERE / "tiny.mpileup").write_text(mpileup_single)

mpileup_multi = (
    "chr1\t100\tA\t12\t..,,..C,c,..\tIHGFGHIGFGFG\t5\t..,.,\tHGFGF\n"
    "chr1\t101\tG\t13\t..,GG.gg..,.,\tIHHHGGGHGFFFG\t4\t...G\tHGFG\n"
    "chr2\t500\tC\t20\t..,,...,..,.,..,..,.\tHGFFGGGFGGFFGGFFGGFG\t7\t..,..,.\tGFGFGFG\n"
)
(HERE / "tiny.multi.mpileup").write_text(mpileup_multi)

# Bgzip + tabix index the single-sample file so range queries are exercised.
# Mpileup uses single-position records, so tabix gets `-s 1 -b 2 -e 2`.
if have("bgzip") and have("tabix"):
    mp_gz = HERE / "tiny.mpileup.gz"
    if mp_gz.exists():
        mp_gz.unlink()
    with open(HERE / "tiny.mpileup", "rb") as fin, open(mp_gz, "wb") as fout:
        subprocess.run(["bgzip", "-c"], stdin=fin, stdout=fout, check=True)
    subprocess.run(["tabix", "-f", "-s", "1", "-b", "2", "-e", "2", str(mp_gz)],
                   check=True)

# ── OpenDocument Spreadsheet (.ods, two sheets, requires odfpy) ──────────────
try:
    from odf.opendocument import OpenDocumentSpreadsheet
    from odf.table import Table, TableRow, TableCell
    from odf.text import P
except ImportError:
    print("warn: odfpy not found; skipping tiny.ods", file=sys.stderr)
else:
    ods_path = HERE / "tiny.ods"
    if ods_path.exists():
        ods_path.unlink()
    ods = OpenDocumentSpreadsheet()

    def _add_sheet(name, rows):
        t = Table(name=name)
        for row in rows:
            tr = TableRow()
            for cell in row:
                # Distinguish strings from numerics so the typed value
                # attributes get set (lets Arrow CSV infer correctly).
                if isinstance(cell, str):
                    tc = TableCell(valuetype="string")
                else:
                    tc = TableCell(valuetype="float", value=str(cell))
                tc.addElement(P(text=str(cell)))
                tr.addElement(tc)
            t.addElement(tr)
        ods.spreadsheet.addElement(t)

    _add_sheet("peaks", [
        ["chrom", "start", "end", "score", "name"],
        ["chr1",  100,     200,   5.2,     "p1"],
        ["chr1",  500,     800,   8.1,     "p2"],
        ["chr2",  1000,    1300,  3.4,     "p3"],
    ])
    _add_sheet("samples", [
        ["sample_id", "name", "depth"],
        [1, "sampleA", 12.5],
        [2, "sampleB", 8.7],
        [3, "sampleC", 15.1],
    ])
    ods.save(str(ods_path))

# ── Ragged ODS: a row wider than the header (.ods) ───────────────────────────
# A 3-column header followed by a 4-column data row. The reader used to lock the
# width to the header and Arrow's CSV reader then rejected the whole sheet
# ("Expected 3 columns, got 4"). The fix pads every row to the widest, naming
# the header's overflow column "col4".
try:
    from odf.opendocument import OpenDocumentSpreadsheet
    from odf.table import Table, TableRow, TableCell
    from odf.text import P
except ImportError:
    print("warn: odfpy not found; skipping tiny.ragged.ods", file=sys.stderr)
else:
    rg_path = HERE / "tiny.ragged.ods"
    if rg_path.exists():
        rg_path.unlink()
    doc = OpenDocumentSpreadsheet()
    t = Table(name="ragged")
    for row in [["chrom", "start", "end"],
                ["chr1", 100, 200],
                ["chr2", 300, 400, "EXTRA"],   # wider than the header
                ["chr3", 500]]:                # shorter than the header
        tr = TableRow()
        for cell in row:
            tc = (TableCell(valuetype="string") if isinstance(cell, str)
                  else TableCell(valuetype="float", value=str(cell)))
            tc.addElement(P(text=str(cell)))
            tr.addElement(tc)
        t.addElement(tr)
    doc.spreadsheet.addElement(t)
    doc.save(str(rg_path))

    # tiny.rowrep.ods: a *non-empty* row carrying table:number-rows-repeated.
    # The reader used to ignore the attribute and emit the row once, silently
    # dropping the duplicates. The "dup" row (repeat=3) must yield 3 rows, so
    # the sheet has 5 data rows total (a + dup×3 + z) under a 2-column header.
    rr_path = HERE / "tiny.rowrep.ods"
    if rr_path.exists():
        rr_path.unlink()
    rdoc = OpenDocumentSpreadsheet()
    rt = Table(name="rep")

    def _rr_cell(c):
        tc = (TableCell(valuetype="string") if isinstance(c, str)
              else TableCell(valuetype="float", value=str(c)))
        tc.addElement(P(text=str(c)))
        return tc

    def _rr_row(cells, repeat=None):
        tr = TableRow(numberrowsrepeated=str(repeat)) if repeat else TableRow()
        for c in cells:
            tr.addElement(_rr_cell(c))
        rt.addElement(tr)

    _rr_row(["k", "v"])                # header
    _rr_row(["a", 1])
    _rr_row(["dup", 9], repeat=3)      # non-empty row repeated 3×
    _rr_row(["z", 2])
    rdoc.spreadsheet.addElement(rt)
    rdoc.save(str(rr_path))

# ── HDF5 / AnnData fixtures ──────────────────────────────────────────────────
# tiny.h5ad: small AnnData with a CSR-sparse X, two obs columns
# (one categorical), one var column, one obsm embedding. Exercises
# the AnnData layout parser (summary + obs + var + X-preview + obsm tabs).
# tiny.h5: plain HDF5 with nested groups + a 1-D and a 2-D dataset to
# exercise the generic hierarchy walker + dataset readers.
try:
    import h5py                                              # type: ignore
    import numpy as np                                       # type: ignore
except ImportError:
    print("warn: h5py / numpy not found; skipping tiny.h5 / tiny.h5ad",
          file=sys.stderr)
else:
    # Plain HDF5 file with a group hierarchy and two datasets.
    h5_path = HERE / "tiny.h5"
    if h5_path.exists():
        h5_path.unlink()
    with h5py.File(h5_path, "w") as f:
        grp = f.create_group("counts")
        grp.create_dataset("rows", data=np.arange(10, dtype=np.int64))
        grp.create_dataset("matrix",
                            data=np.arange(20, dtype=np.float64).reshape(10, 2))
        meta = f.create_group("meta")
        meta.attrs["created_by"] = "vv tests"
        meta.create_dataset("labels",
                              data=np.array(["a", "b", "c"], dtype="S2"))

try:
    import anndata as ad                                     # type: ignore
    import numpy as np                                       # type: ignore
    from scipy import sparse                                 # type: ignore
except ImportError:
    print("warn: anndata / scipy not found; skipping tiny.h5ad",
          file=sys.stderr)
else:
    h5ad_path = HERE / "tiny.h5ad"
    if h5ad_path.exists():
        h5ad_path.unlink()
    # Tiny dataset: 5 "cells" × 4 "genes" with a CSR-sparse X.
    rng = np.random.default_rng(42)
    rows, cols, vals = [], [], []
    for r in range(5):
        for c in range(4):
            if rng.random() < 0.5:
                rows.append(r); cols.append(c)
                vals.append(float(rng.integers(1, 100)))
    X = sparse.csr_matrix((vals, (rows, cols)), shape=(5, 4),
                            dtype=np.float64)
    obs = ad.AnnData.__module__  # marker for older anndata versions
    import pandas as pd                                       # type: ignore
    obs_df = pd.DataFrame({
        "cluster":  pd.Categorical(["A", "B", "A", "B", "A"]),
        "n_counts": np.array([12, 7, 19, 4, 25], dtype=np.float64),
    }, index=[f"cell{i}" for i in range(5)])
    var_df = pd.DataFrame({
        "gene_name": ["G1", "G2", "G3", "G4"],
        "mt":        [False, False, True, False],
    }, index=[f"gene{i}" for i in range(4)])
    adata = ad.AnnData(X=X, obs=obs_df, var=var_df,
                          obsm={"X_umap": np.array(
                              [[0.1, 0.2], [-0.3, 0.4],
                               [0.5, -0.1], [-0.2, -0.5],
                               [0.0, 0.0]], dtype=np.float64)})
    adata.write_h5ad(h5ad_path)

    # tiny.dense.h5ad: a *dense* X that is wider than the 200-column dense
    # preview cap (3 cells × 250 genes). scan_anndata emits a Matrix2D tab for
    # a dense X with no column gate, so without the cap this densifies the whole
    # matrix into RAM (the OOM/DoS guarded by read_2d_dataset_table). Here it
    # must preview as 3 × 200 with a "first 200 of 250 cols" footer.
    dense_path = HERE / "tiny.dense.h5ad"
    if dense_path.exists():
        dense_path.unlink()
    n_obs, n_var = 3, 250
    Xd = np.arange(n_obs * n_var, dtype=np.float64).reshape(n_obs, n_var)
    dobs = pd.DataFrame({"cluster": pd.Categorical(["A", "B", "A"])},
                        index=[f"cell{i}" for i in range(n_obs)])
    dvar = pd.DataFrame(index=[f"gene{i}" for i in range(n_var)])
    ad.AnnData(X=Xd, obs=dobs, var=dvar).write_h5ad(dense_path)

    # tiny.bigobs.h5ad: 1500 obs rows — exceeds the 1000-row component preview
    # cap, so the obs / X tabs must render "preview: first 1000 of 1500 rows"
    # instead of reading every row (the fix for stalling on huge AnnData
    # components over a slow mount).
    big_path = HERE / "tiny.bigobs.h5ad"
    if big_path.exists():
        big_path.unlink()
    nbig = 1500
    Xb = sparse.random(nbig, 4, density=0.3, format="csr", dtype=np.float64,
                       random_state=0)
    bobs = pd.DataFrame({
        "grp": pd.Categorical((["A", "B", "C"] * ((nbig // 3) + 1))[:nbig]),
        "val": np.arange(nbig, dtype=np.float64),
    }, index=[f"cell{i}" for i in range(nbig)])
    bvar = pd.DataFrame(index=[f"gene{i}" for i in range(4)])
    ad.AnnData(X=Xb, obs=bobs, var=bvar).write_h5ad(big_path)

# tiny.malformed.h5ad: a hostile AnnData that previously crashed the reader.
# Derived from the valid tiny.h5ad (so it stays AnnData-detectable and reaches
# the sparse-X path) by poking two attributes that HDF5's H5Aread fills one
# element per dataspace point:
#   - /X "shape": 16 int64 values instead of 2 → smashed the fixed int64[2]
#   - /obs "_index": a 6-element string array → smashed a single-string buffer
# vv must now open it without crashing (it reads only the first two dims and
# treats the array-valued string attr as absent).
try:
    import h5py                                              # type: ignore
    import numpy as np                                       # type: ignore
except ImportError:
    pass
else:
    base = HERE / "tiny.h5ad"
    if base.exists():
        import shutil
        mal = HERE / "tiny.malformed.h5ad"
        if mal.exists():
            mal.unlink()
        shutil.copy(base, mal)
        with h5py.File(mal, "r+") as f:
            if "X" in f:
                if "shape" in f["X"].attrs:
                    del f["X"].attrs["shape"]
                f["X"].attrs.create("shape", np.arange(16, dtype=np.int64))
            if "obs" in f:
                if "_index" in f["obs"].attrs:
                    del f["obs"].attrs["_index"]
                f["obs"].attrs.create(
                    "_index", np.array([b"i0", b"i1", b"i2",
                                        b"i3", b"i4", b"i5"], dtype="S2"))

# ── NumPy .npz fixtures ──────────────────────────────────────────────────────
# tiny.npz: a valid archive with a 1-D, 2-D and 3-D array — exercises the
# NPZ summary tab and the per-array densification paths.
# tiny.malformed.npz: a hostile archive whose single member's .npy header
# declares shape (1000000000,) <f8 (8 GB) but stores only 16 bytes. The reader
# derived element counts / byte offsets straight from the shape, so this drove
# an out-of-bounds read; vv must now reject it at open with a clear error.
try:
    import numpy as np                                       # type: ignore
    import zipfile, struct                                   # type: ignore
except ImportError:
    print("warn: numpy not found; skipping tiny.npz / tiny.malformed.npz",
          file=sys.stderr)
else:
    np.savez(HERE / "tiny.npz",
             vec=np.arange(6, dtype=np.int64),
             mat=np.arange(12, dtype=np.float64).reshape(3, 4),
             cube=np.arange(24, dtype=np.float32).reshape(2, 3, 4))

    def _make_npy(descr, shape, data):
        tup = "(" + ", ".join(str(s) for s in shape) + \
              ("," if len(shape) == 1 else "") + ")"
        hdr = "{'descr': '%s', 'fortran_order': False, 'shape': %s, }" % (descr, tup)
        pad = (64 - (10 + len(hdr) + 1) % 64) % 64
        hdr = hdr + " " * pad + "\n"
        return (b"\x93NUMPY\x01\x00" + struct.pack("<H", len(hdr))
                + hdr.encode() + data)

    mal_npz = HERE / "tiny.malformed.npz"
    if mal_npz.exists():
        mal_npz.unlink()
    with zipfile.ZipFile(mal_npz, "w", zipfile.ZIP_STORED) as z:
        # header claims 1e9 float64 elements; body is only 16 bytes
        z.writestr("a.npy", _make_npy("<f8", (1000000000,), b"\x00" * 16))

# ── Apache ORC (columnar; pyarrow.orc is built into pyarrow when Arrow was
# compiled with -DARROW_ORC=ON, which is the wheel default since pyarrow
# 12.0). Soft-skip otherwise.
try:
    import pyarrow.orc as paorc                            # type: ignore
except ImportError:
    print("warn: pyarrow.orc not available; skipping tiny.orc", file=sys.stderr)
else:
    orc_path = HERE / "tiny.orc"
    if orc_path.exists():
        orc_path.unlink()
    orc_tbl = pa.table({
        "chrom": pa.array(["chr1", "chr1", "chr2"], type=pa.string()),
        "start": pa.array([100, 500, 1000], type=pa.int64()),
        "end":   pa.array([200, 800, 1300], type=pa.int64()),
        "score": pa.array([5.2, 8.1, 3.4],  type=pa.float64()),
        "name":  pa.array(["p1", "p2", "p3"], type=pa.string()),
    })
    # 2 rows per stripe → 2 stripes so num_chunks > 1 in vv's output.
    paorc.write_table(orc_tbl, orc_path, compression="zstd", stripe_size=2)

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

    # Ragged workbook: a row wider than the header (.xlsx). See the matching
    # tiny.ragged.ods note — the wider row used to make Arrow reject the sheet.
    rg_xlsx = HERE / "tiny.ragged.xlsx"
    if rg_xlsx.exists():
        rg_xlsx.unlink()
    rwb = openpyxl.Workbook()
    rs = rwb.active
    assert rs is not None
    rs.title = "ragged"
    rs.append(["chrom", "start", "end"])
    rs.append(["chr1", 100, 200])
    rs.append(["chr2", 300, 400, "EXTRA"])   # wider than the header
    rs.append(["chr3", 500])                  # shorter than the header
    rwb.save(rg_xlsx)

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

    # tiny.samples.bcf: a BCF WITH genotype samples, so the reader emits a
    # FORMAT_SAMPLES column. Exercises that the FORMAT spec (GT:AD:DP) is kept,
    # not dropped in favour of the sample columns alone.
    smp_input = HERE / "_bcf_samples.vcf"
    smp_input.write_text(
        "##fileformat=VCFv4.2\n"
        "##contig=<ID=chr1>\n"
        '##INFO=<ID=AF,Number=A,Type=Float,Description="Allele frequency">\n'
        '##FORMAT=<ID=GT,Number=1,Type=String,Description="Genotype">\n'
        '##FORMAT=<ID=AD,Number=R,Type=Integer,Description="Allelic depths">\n'
        '##FORMAT=<ID=DP,Number=1,Type=Integer,Description="Read depth">\n'
        "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tS1\tS2\n"
        "chr1\t100\trs1\tA\tG\t30\tPASS\tAF=0.5\tGT:AD:DP\t0/1:5,6:11\t1/1:0,9:9\n"
        "chr1\t500\t.\tC\tT\t40\tPASS\tAF=0.1\tGT:AD:DP\t0/0:10,0:10\t0/1:4,4:8\n"
    )
    subprocess.run(["bcftools", "view", "-O", "b",
                    str(smp_input), "-o", str(HERE / "tiny.samples.bcf")],
                   check=True)
    subprocess.run(["bcftools", "index", "-f",
                    str(HERE / "tiny.samples.bcf")], check=True)
    smp_input.unlink()
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
