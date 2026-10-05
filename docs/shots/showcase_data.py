#!/usr/bin/env python3
"""Synthetic but realistic files for the documentation screenshots.

  showcase_data.py <outdir>

Deterministic (fixed seed): the same files every run, so regenerated
screenshots only change when vv's output does. Gene names, chromosomes and
coordinates follow GRCh38; the values are random. A format whose Python module
or command-line tool is missing is skipped with a note.
"""
import gzip, json, os, random, shutil, sqlite3, subprocess, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
R = random.Random(42)

# (gene, chrom, start, end, strand) — GRCh38 coordinates
GENES = [
    ("BRCA1", "chr17", 43044295, 43125483, "-"), ("TP53", "chr17", 7661779, 7687538, "-"),
    ("EGFR", "chr7", 55019017, 55211628, "+"), ("KRAS", "chr12", 25205246, 25250929, "-"),
    ("MYC", "chr8", 127735434, 127742951, "+"), ("PTEN", "chr10", 87863113, 87971930, "+"),
    ("CD19", "chr16", 28931965, 28939342, "+"), ("CD3E", "chr11", 118304730, 118316175, "+"),
    ("MS4A1", "chr11", 60455752, 60470760, "+"), ("LYZ", "chr12", 69348341, 69354234, "+"),
    ("NKG7", "chr19", 51371606, 51372715, "-"), ("GNLY", "chr2", 85685408, 85698854, "+"),
    ("CST3", "chr20", 23626706, 23638473, "-"), ("FCGR3A", "chr1", 161541759, 161550737, "-"),
    ("PPBP", "chr4", 73986439, 73988190, "-"), ("IL7R", "chr5", 35852695, 35879603, "+"),
    ("GAPDH", "chr12", 6534512, 6538374, "+"), ("ACTB", "chr7", 5526409, 5563902, "-"),
]
CELL_TYPES = ["CD4 T", "CD8 T", "B", "NK", "CD14 Mono", "FCGR3A Mono", "DC", "Platelet"]


def note(msg):
    print("showcase: " + msg, file=sys.stderr)


def tool(name):
    return shutil.which(name) is not None


# ── ATAC peaks: Parquet, narrowPeak, BED, bigWig ────────────────────────────
peaks = []
for i in range(20000):
    g = R.choice(GENES)
    s = R.randint(g[2] - 50000, g[3] + 50000)
    w = R.randint(150, 1200)
    sig = round(R.lognormvariate(2.0, 0.8), 3)
    peaks.append((g[1], s, s + w, "peak_%05d" % i, min(1000, int(sig * 40)), ".",
                  sig, round(R.uniform(2, 60), 2), round(R.uniform(1, 55), 2), w // 2, g[0]))
peaks.sort(key=lambda p: (p[0], p[1]))
with open(os.path.join(out, "atac_peaks.narrowPeak"), "w") as f:
    for p in peaks:
        f.write("\t".join(str(x) for x in p[:10]) + "\n")
with open(os.path.join(out, "atac_peaks.bed"), "w") as f:
    for p in peaks:
        f.write("%s\t%d\t%d\t%s\t%d\t%s\n" % p[:6])
try:
    import pyarrow as pa, pyarrow.parquet as pq
    cols = ["chrom", "start", "end", "name", "score", "strand", "signal", "pvalue", "qvalue",
            "summit", "nearest_gene"]
    pq.write_table(pa.table({c: [p[k] for p in peaks] for k, c in enumerate(cols)}),
                   os.path.join(out, "atac_peaks.parquet"), row_group_size=5000)
except ImportError:
    note("pyarrow missing: no Parquet")

sizes = {}
for g in GENES:
    sizes[g[1]] = max(sizes.get(g[1], 0), g[3] + 200000)
with open(os.path.join(out, "chrom.sizes"), "w") as f:
    for c, n in sorted(sizes.items()):
        f.write("%s\t%d\n" % (c, n))
if tool("bedGraphToBigWig"):
    bg = os.path.join(out, "coverage.bedGraph")
    with open(bg, "w") as f:
        for c in sorted(sizes):
            pos = min(g[2] for g in GENES if g[1] == c) - 20000
            for _ in range(400):
                f.write("%s\t%d\t%d\t%.2f\n" % (c, pos, pos + 50, R.lognormvariate(1.5, 0.7)))
                pos += 50
    subprocess.run(["bedGraphToBigWig", bg, os.path.join(out, "chrom.sizes"),
                    os.path.join(out, "coverage.bw")], check=True)
else:
    note("bedGraphToBigWig missing: no bigWig")

# ── Genes: GFF3 ──────────────────────────────────────────────────────────────
with open(os.path.join(out, "genes.gff3"), "w") as f:
    f.write("##gff-version 3\n")
    for g, c, s, e, st in sorted(GENES, key=lambda x: (x[1], x[2])):
        gid = "ENSG%011d" % R.randint(1, 10**9)
        f.write("%s\tENSEMBL\tgene\t%d\t%d\t.\t%s\t.\tID=gene:%s;Name=%s;biotype=protein_coding\n"
                % (c, s, e, st, gid, g))
        tid = "ENST%011d" % R.randint(1, 10**9)
        f.write("%s\tENSEMBL\tmRNA\t%d\t%d\t.\t%s\t.\tID=transcript:%s;Parent=gene:%s;Name=%s-201\n"
                % (c, s, e, st, tid, gid, g))
        pos = s
        for k in range(R.randint(3, 6)):
            es = pos + R.randint(0, 2000)
            ee = min(e, es + R.randint(100, 400))
            f.write("%s\tENSEMBL\texon\t%d\t%d\t.\t%s\t.\tParent=transcript:%s;exon_number=%d\n"
                    % (c, es, ee, st, tid, k + 1))
            pos = ee + R.randint(1000, 8000)
            if pos >= e:
                break

# ── Variants: multi-sample VCF (bgzipped + tabix) ───────────────────────────
samples = ["NA12878", "NA12891", "NA12892", "HG00096", "HG00097", "HG00099"]
vcf = os.path.join(out, "cohort.vcf")
with open(vcf, "w") as f:
    f.write("##fileformat=VCFv4.2\n##source=showcase\n")
    for c, n in sorted(sizes.items()):
        f.write("##contig=<ID=%s,length=%d>\n" % (c, n))
    f.write('##INFO=<ID=AC,Number=A,Type=Integer,Description="Allele count">\n'
            '##INFO=<ID=AF,Number=A,Type=Float,Description="Allele frequency">\n'
            '##INFO=<ID=DP,Number=1,Type=Integer,Description="Total depth">\n'
            '##INFO=<ID=GENE,Number=1,Type=String,Description="Gene">\n'
            '##FORMAT=<ID=GT,Number=1,Type=String,Description="Genotype">\n'
            '##FORMAT=<ID=AD,Number=R,Type=Integer,Description="Allelic depths">\n'
            '##FORMAT=<ID=DP,Number=1,Type=Integer,Description="Read depth">\n'
            '##FORMAT=<ID=GQ,Number=1,Type=Integer,Description="Genotype quality">\n')
    f.write("#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t" + "\t".join(samples) + "\n")
    rows = []
    for _ in range(600):
        g = R.choice(GENES)
        pos = R.randint(g[2], g[3])
        ref = R.choice("ACGT")
        alt = R.choice([b for b in "ACGT" if b != ref])
        gts, ac = [], 0
        for _ in samples:
            gt = R.choices(["0/0", "0/1", "1/1", "./."], [60, 28, 10, 2])[0]
            ac += gt.count("1")
            dp = R.randint(12, 60)
            alt_n = 0 if gt == "0/0" else dp if gt == "1/1" else dp // 2
            if gt == "./.":
                gts.append("./.:.:.:.")
                continue
            gts.append("%s:%d,%d:%d:%d" % (gt, dp - alt_n, alt_n, dp, R.randint(20, 99)))
        rid = "rs%d" % R.randint(1000, 99999999) if R.random() < 0.7 else "."
        rows.append((g[1], pos, rid, ref, alt, "%.1f" % R.uniform(30, 900),
                     R.choices(["PASS", "LowQual"], [92, 8])[0],
                     "AC=%d;AF=%.3f;DP=%d;GENE=%s" % (ac, ac / (2 * len(samples)), R.randint(80, 400), g[0]),
                     "GT:AD:DP:GQ", gts))
    for r in sorted(rows, key=lambda r: (r[0], r[1])):
        f.write("\t".join(str(x) for x in r[:9]) + "\t" + "\t".join(r[9]) + "\n")
try:
    import pysam
    pysam.tabix_compress(vcf, vcf + ".gz", force=True)
    pysam.tabix_index(vcf + ".gz", preset="vcf", force=True)
except ImportError:
    note("pysam missing: cohort.vcf stays uncompressed")

# ── Reads: BAM (sorted, indexed), FASTQ, FASTA, PAF ─────────────────────────
ref_len = 30000
ref_seq = "".join(R.choice("ACGT") for _ in range(ref_len))
with open(os.path.join(out, "regions.fa"), "w") as f:
    f.write(">chr17:43044295-43074295 BRCA1 region (synthetic sequence)\n")
    for i in range(0, ref_len, 60):
        f.write(ref_seq[i:i + 60] + "\n")
    for g, c, st, en, _ in GENES[1:7]:
        seq = "".join(R.choice("ACGT") for _ in range(R.randint(800, 3000)))
        f.write(">%s:%d-%d %s promoter (synthetic sequence)\n" % (c, st - len(seq), st, g))
        for i in range(0, len(seq), 60):
            f.write(seq[i:i + 60] + "\n")
with gzip.open(os.path.join(out, "reads.fq.gz"), "wt") as f:
    for i in range(4000):
        p = R.randint(0, ref_len - 151)
        seq = ref_seq[p:p + 150]
        q = "".join(chr(33 + min(41, max(2, int(R.gauss(36, 4))))) for _ in seq)
        f.write("@A00123:8:H7VJMDSXX:1:1101:%d:%d 1:N:0:ATCACG\n%s\n+\n%s\n"
                % (R.randint(1000, 32000), R.randint(1000, 37000), seq, q))
try:
    import pysam
    hdr = {"HD": {"VN": "1.6", "SO": "coordinate"},
           "SQ": [{"SN": "chr17", "LN": 83257441}],
           "RG": [{"ID": "NA12878", "SM": "NA12878", "PL": "ILLUMINA"}]}
    unsorted = os.path.join(out, "reads.unsorted.bam")
    with pysam.AlignmentFile(unsorted, "wb", header=hdr) as bam:
        for i in range(3000):
            a = pysam.AlignedSegment()
            p = R.randint(0, ref_len - 151)
            a.query_name = "A00123:8:H7VJMDSXX:1:%d:%d:%d" % (R.randint(1101, 2678),
                                                          R.randint(1000, 32000), R.randint(1000, 37000))
            seq = list(ref_seq[p:p + 150])
            nm = 0
            for _ in range(R.choice([0, 0, 1, 2])):
                k = R.randrange(150)
                seq[k] = R.choice([b for b in "ACGT" if b != seq[k]])
                nm += 1
            a.query_sequence = "".join(seq)
            a.flag = R.choice([99, 147, 83, 163, 0, 16])
            a.reference_id = 0
            a.reference_start = 43044295 + p
            a.mapping_quality = R.choice([60, 60, 60, 60, 27, 0])
            a.cigar = [(0, 150)]
            a.query_qualities = pysam.qualitystring_to_array("".join(
                chr(33 + min(41, max(2, int(R.gauss(36, 4))))) for _ in range(150)))
            a.set_tags([("NM", nm), ("AS", 150 - 4 * nm), ("RG", "NA12878")])
            bam.write(a)
    pysam.sort("-o", os.path.join(out, "reads.bam"), unsorted)
    pysam.index(os.path.join(out, "reads.bam"))
    os.remove(unsorted)
except ImportError:
    note("pysam missing: no BAM")
with open(os.path.join(out, "contigs.paf"), "w") as f:
    for i in range(300):
        qlen = R.randint(2000, 20000)
        g = R.choice(GENES)
        ts = R.randint(g[2], g[3])
        aln = int(qlen * R.uniform(0.85, 1.0))
        f.write("contig_%04d\t%d\t0\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t60\tNM:i:%d\ttp:A:P\n"
                % (i, qlen, aln, g[4], g[1], sizes[g[1]], ts, ts + aln, aln - R.randint(0, 80), aln,
                   R.randint(0, 80)))

# ── Single cell: AnnData, MatrixMarket, Loom-like HDF5 ──────────────────────
try:
    import numpy as np, pandas as pd, anndata as ad, scipy.sparse as sp, scipy.io
    rng = np.random.default_rng(7)
    n_cells = 2700
    gene_names = [g[0] for g in GENES] + ["GENE%04d" % i for i in range(482)]
    ct = rng.choice(CELL_TYPES, size=n_cells, p=[.3, .15, .13, .1, .17, .05, .05, .05])
    X = sp.random(n_cells, len(gene_names), density=0.08, format="csr", random_state=7,
                  data_rvs=lambda n: rng.poisson(3, n) + 1).astype(np.float32)
    obs = pd.DataFrame({
        "cell_type": pd.Categorical(ct, categories=CELL_TYPES),
        "sample": pd.Categorical(rng.choice(["donor1", "donor2", "donor3"], n_cells)),
        "n_genes": np.asarray((X > 0).sum(1)).ravel(),
        "total_counts": np.asarray(X.sum(1)).ravel(),
        "pct_mito": np.round(rng.uniform(0.5, 8, n_cells), 2),
    }, index=["AAACATACAACCAC-%d" % i for i in range(n_cells)])
    var = pd.DataFrame({
        "gene_ids": ["ENSG%011d" % rng.integers(1, 10**9) for _ in gene_names],
        "highly_variable": rng.random(len(gene_names)) < 0.2,
        "means": np.round(np.asarray(X.mean(0)).ravel(), 4),
    }, index=gene_names)
    a = ad.AnnData(X=X, obs=obs, var=var)
    a.layers["counts"] = X.copy()
    a.obsm["X_umap"] = rng.normal(size=(n_cells, 2)).astype(np.float32)
    a.obsm["X_pca"] = rng.normal(size=(n_cells, 50)).astype(np.float32)
    a.uns["leiden"] = {"params": {"resolution": 1.0}}
    a.write_h5ad(os.path.join(out, "pbmc.h5ad"))
    shutil.rmtree(os.path.join(out, "pbmc.zarr"), ignore_errors=True)
    a.write_zarr(os.path.join(out, "pbmc.zarr"))       # the same AnnData as a Zarr store
    scipy.io.mmwrite(os.path.join(out, "matrix.mtx"), X[:500].T.tocoo())
    np.savez(os.path.join(out, "embedding.npz"), umap=a.obsm["X_umap"][:1000],
             cluster=np.array([CELL_TYPES.index(x) for x in ct[:1000]], dtype=np.int32))
except ImportError as e:
    note("single-cell modules missing (%s): no AnnData / MatrixMarket / NumPy" % e)

# ── Tables: TSV, SQLite, Excel ───────────────────────────────────────────────
meta = []
for i, s in enumerate(samples + ["HG00100", "HG00101"]):
    meta.append((s, R.choice(["CEU", "GBR", "YRI"]), R.choice(["F", "M"]), R.randint(18, 70),
                 R.choice(["NovaSeq 6000", "HiSeq X"]), round(R.uniform(28, 45), 1),
                 "2024-%02d-%02d" % (R.randint(1, 12), R.randint(1, 28))))
hdr = ["sample", "population", "sex", "age", "instrument", "mean_coverage", "sequenced"]
with open(os.path.join(out, "samples.tsv"), "w") as f:
    f.write("\t".join(hdr) + "\n")
    for m in meta:
        f.write("\t".join(str(x) for x in m) + "\n")
db = sqlite3.connect(os.path.join(out, "lab.sqlite"))
db.execute("DROP TABLE IF EXISTS samples")
db.execute("CREATE TABLE samples (sample TEXT, population TEXT, sex TEXT, age INTEGER, "
           "instrument TEXT, mean_coverage REAL, sequenced TEXT)")
db.executemany("INSERT INTO samples VALUES (?,?,?,?,?,?,?)", meta)
db.execute("DROP TABLE IF EXISTS runs")
db.execute("CREATE TABLE runs (run TEXT, sample TEXT, lane INTEGER, reads INTEGER, q30 REAL)")
db.executemany("INSERT INTO runs VALUES (?,?,?,?,?)",
               [("RUN%03d" % i, R.choice(meta)[0], R.randint(1, 4), R.randint(10**8, 9 * 10**8),
                 round(R.uniform(88, 96), 1)) for i in range(40)])
db.commit()
db.close()
try:
    import duckdb
    path = os.path.join(out, "lab.duckdb")
    if os.path.exists(path):
        os.remove(path)
    dd = duckdb.connect(path)
    dd.execute("CREATE TABLE runs (run VARCHAR, sample VARCHAR, sequenced DATE, reads BIGINT, "
               "q30 DECIMAL(4,1), lanes INTEGER[], flowcell STRUCT(id VARCHAR, kit VARCHAR))")
    dd.executemany("INSERT INTO runs VALUES (?, ?, ?, ?, ?, ?, ?)",
                   [("RUN%03d" % i, R.choice(meta)[0], "2024-%02d-%02d" % (R.randint(1, 12), R.randint(1, 28)),
                     R.randint(10**8, 9 * 10**8), round(R.uniform(88, 96), 1),
                     sorted(R.sample([1, 2, 3, 4], R.randint(1, 4))),
                     {"id": "H%04dDSX%d" % (R.randint(1000, 9999), R.randint(1, 7)),
                      "kit": R.choice(["S4 v1.5", "S2 v1.5"])}) for i in range(40)])
    dd.execute("CREATE TABLE samples (sample VARCHAR, population VARCHAR, sex VARCHAR, age INTEGER, "
               "instrument VARCHAR, mean_coverage DOUBLE, sequenced DATE)")
    dd.executemany("INSERT INTO samples VALUES (?, ?, ?, ?, ?, ?, ?)", meta)
    dd.execute("CREATE VIEW low_q30 AS SELECT run, sample, q30 FROM runs WHERE q30 < 90")
    dd.close()
except ImportError:
    note("duckdb (Python) not found; skipping lab.duckdb")
try:
    import openpyxl
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "samples"
    ws.append(hdr)
    for m in meta:
        ws.append(list(m))
    ws2 = wb.create_sheet("qc")
    ws2.append(["sample", "reads_M", "dup_pct", "q30_pct"])
    for m in meta:
        ws2.append([m[0], round(R.uniform(300, 900), 1), round(R.uniform(5, 20), 1),
                    round(R.uniform(88, 96), 1)])
    wb.save(os.path.join(out, "samples.xlsx"))
except ImportError:
    note("openpyxl missing: no Excel")

# ── JSON, NDJSON, Markdown, a log ───────────────────────────────────────────
run = {
    "run_id": "RUN042", "pipeline": {"name": "nf-core/rnaseq", "version": "3.14.0",
                                     "profile": ["docker", "slurm"]},
    "started": "2024-06-14T09:12:33Z", "status": "COMPLETED",
    "params": {"genome": "GRCh38", "aligner": "star_salmon", "trimmer": "trimgalore",
               "skip_qc": False, "max_cpus": 64, "max_memory": "256.GB"},
    "samples": [{"id": m[0], "population": m[1],
                 "fastq": ["s3://lab-raw/%s_R1.fastq.gz" % m[0], "s3://lab-raw/%s_R2.fastq.gz" % m[0]],
                 "metrics": {"reads": R.randint(2 * 10**7, 9 * 10**7),
                             "mapped_pct": round(R.uniform(88, 97), 2),
                             "duplicates_pct": round(R.uniform(5, 25), 2)}} for m in meta],
    "outputs": {"counts": "results/star_salmon/salmon.merged.gene_counts.tsv",
                "multiqc": "results/multiqc/multiqc_report.html"},
}
with open(os.path.join(out, "run.json"), "w") as f:
    json.dump(run, f, indent=2)
with open(os.path.join(out, "events.ndjson"), "w") as f:
    for i in range(500):
        f.write(json.dumps({"ts": "2024-06-14T%02d:%02d:%02dZ" % (9 + i // 3600, i // 60 % 60, i % 60),
                            "level": R.choices(["INFO", "WARN", "ERROR"], [85, 12, 3])[0],
                            "task": R.choice(["FASTQC", "TRIMGALORE", "STAR_ALIGN", "SALMON_QUANT"]),
                            "sample": R.choice(meta)[0], "cpus": R.choice([4, 8, 16]),
                            "duration_s": round(R.uniform(30, 3600), 1)}) + "\n")
with open(os.path.join(out, "README.md"), "w") as f:
    f.write("# RNA-seq run RUN042\n\nProcessed with **nf-core/rnaseq 3.14.0** on GRCh38.\n\n"
            "## Samples\n\n| sample | population | reads (M) | mapped % |\n|---|---|---:|---:|\n")
    for m in meta[:6]:
        f.write("| %s | %s | %.1f | %.1f |\n" % (m[0], m[1], R.uniform(20, 90), R.uniform(88, 97)))
    f.write("\n## Notes\n\n- Two samples were re-sequenced after low Q30.\n"
            "- Counts are in `results/star_salmon/`.\n")
with open(os.path.join(out, "pipeline.log"), "w") as f:
    for i in range(400):
        lvl = {3: "WARN", 8: "ERROR", 11: "WARN"}.get(i) or R.choices(["INFO", "WARN", "ERROR"], [88, 10, 2])[0]
        # Levels coloured the way loguru / nextflow write them to a terminal.
        col = {"INFO": "\x1b[32m", "WARN": "\x1b[33m", "ERROR": "\x1b[1;31m"}[lvl]
        f.write("\x1b[2m2024-06-14 09:%02d:%02d\x1b[0m %s%-5s\x1b[0m [%s] %s\n" % (i // 60 % 60, i % 60, col, lvl,
                R.choice(["STAR_ALIGN", "SALMON_QUANT", "FASTQC"]),
                R.choice(["submitted job", "completed in %ds" % R.randint(30, 900),
                          "retrying (exit 137)", "cached result reused"])))

# ── A Hive-partitioned dataset directory ─────────────────────────────────────
try:
    import pyarrow as pa, pyarrow.parquet as pq
    for assay in ("ATAC-seq", "RNA-seq", "WGS"):
        d = os.path.join(out, "measurements", "assay=%s" % assay)
        os.makedirs(d, exist_ok=True)
        pq.write_table(pa.table({
            "sample": [R.choice(meta)[0] for _ in range(200)],
            "metric": [R.choice(["mapped_pct", "dup_pct", "q30_pct"]) for _ in range(200)],
            "value": [round(R.uniform(5, 98), 2) for _ in range(200)]}), os.path.join(d, "part-0.parquet"))
except ImportError:
    pass


# ── R: a DESeq2 results table (.rds) ────────────────────────────────────────
if tool("Rscript"):
    rows = []
    for g, *_ in GENES:
        lfc = round(R.gauss(0, 2), 3)
        rows.append((g, round(R.uniform(20, 9000), 1), lfc, round(R.uniform(0.1, 0.6), 3),
                     10 ** -R.uniform(0.2, 12)))
    tsv = os.path.join(out, "deseq2.tsv")
    with open(tsv, "w") as f:
        f.write("gene\tbaseMean\tlog2FoldChange\tlfcSE\tpvalue\n")
        for r in rows:
            f.write("%s\t%s\t%s\t%s\t%.3g\n" % r)
    subprocess.run(["Rscript", "-e",
        "d <- read.delim('%s', stringsAsFactors = FALSE); d$padj <- p.adjust(d$pvalue, 'BH');"
        "d$direction <- factor(ifelse(d$log2FoldChange > 0, 'up', 'down'));"
        "rownames(d) <- d$gene; d$gene <- NULL; saveRDS(d, '%s')" % (tsv, os.path.join(out, "deseq2.rds"))],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.remove(tsv)
else:
    note("Rscript not found; skipping deseq2.rds")

# ── Hi-C: a two-resolution .mcool (chr17 + chr7, first 2 Mb) ───────────────
try:
    import h5py, numpy as np
    def cooler(g, binsize):
        chroms = [("chr17", 2_000_000), ("chr7", 2_000_000)]
        bc, bs, be = [], [], []
        for ci, (_, ln) in enumerate(chroms):
            for st in range(0, ln, binsize):
                bc.append(ci); bs.append(st); be.append(min(st + binsize, ln))
        n = len(bc)
        rng = np.random.default_rng(binsize)
        b1, b2, cnt = [], [], []
        for i in range(n):
            for j in range(i, n):
                d = j - i
                if bc[i] == bc[j] and d < 40:
                    lam = 400.0 / (1 + d) ** 1.1
                elif bc[i] != bc[j] and rng.random() < 0.01:
                    lam = 1.5
                else:
                    continue
                c = int(rng.poisson(lam))
                if c:
                    b1.append(i); b2.append(j); cnt.append(c)
        g.attrs.update({"format": "HDF5::Cooler", "format-version": 3, "bin-type": "fixed",
                        "bin-size": binsize, "storage-mode": "symmetric-upper", "genome-assembly": "hg38",
                        "nbins": n, "nchroms": 2, "nnz": len(cnt), "sum": int(sum(cnt)),
                        "generated-by": "cooler-0.10.2", "creation-date": "2026-09-30T12:00:00"})
        g.create_dataset("chroms/name", data=np.array([c[0].encode() for c in chroms], dtype="S5"))
        g.create_dataset("chroms/length", data=np.array([c[1] for c in chroms], dtype=np.int32))
        g.create_dataset("bins/chrom", data=np.array(bc, dtype=np.int32),
                         dtype=h5py.enum_dtype({"chr17": 0, "chr7": 1}, basetype="i4"))
        g.create_dataset("bins/start", data=np.array(bs, dtype=np.int32))
        g.create_dataset("bins/end", data=np.array(be, dtype=np.int32))
        w = rng.uniform(0.002, 0.02, n); w[rng.random(n) < 0.03] = np.nan
        g.create_dataset("bins/weight", data=w)
        g.create_dataset("pixels/bin1_id", data=np.array(b1, dtype=np.int64))
        g.create_dataset("pixels/bin2_id", data=np.array(b2, dtype=np.int64))
        g.create_dataset("pixels/count", data=np.array(cnt, dtype=np.int32))
        co = [0, bc.count(0), n]
        g.create_dataset("indexes/chrom_offset", data=np.array(co, dtype=np.int64))
        off = np.searchsorted(np.array(b1), np.arange(n + 1))
        g.create_dataset("indexes/bin1_offset", data=off.astype(np.int64))
    with h5py.File(os.path.join(out, "hic.mcool"), "w") as f:
        f.attrs["format"] = "HDF5::MCOOL"; f.attrs["format-version"] = 2
        for res in (10_000, 50_000):
            cooler(f.create_group("resolutions/%d" % res), res)
except ImportError:
    note("h5py / numpy not found; skipping hic.mcool")

# ── GenBank: a cloning vector (.gb) ─────────────────────────────────────────
try:
    from Bio.Seq import Seq
    from Bio.SeqRecord import SeqRecord
    from Bio.SeqFeature import SeqFeature, SimpleLocation
    from Bio import SeqIO
    seq = Seq("".join(R.choice("ACGT") for _ in range(2686)))
    rec = SeqRecord(seq, id="L09137.2", name="SYNPUC19CV",
                    description="pUC19c cloning vector (beta-galactosidase mRNA on complementary strand)")
    rec.annotations = {"molecule_type": "DNA", "topology": "circular", "data_file_division": "SYN",
                       "date": "18-DEC-2018", "accessions": ["L09137"], "sequence_version": 2,
                       "organism": "synthetic construct", "taxonomy": ["other sequences", "artificial sequences", "vectors"]}
    def feat(s, e, strand, typ, **q):
        return SeqFeature(SimpleLocation(s, e, strand=strand), type=typ, qualifiers={k: [v] for k, v in q.items()})
    rec.features = [
        feat(0, 2686, 1, "source", organism="synthetic construct", mol_type="other DNA"),
        feat(145, 469, -1, "gene", gene="lacZ"),
        feat(145, 469, -1, "CDS", gene="lacZ", codon_start="1", product="beta-galactosidase alpha peptide",
             protein_id="AAA03418.1", translation="MTMITPSLHACRSTLEDPRVPSSNSLAVVLQRRDWENPGVTQLNRLAAHPPFASWRNSEEARTDRPSQQLRSLNGEWRLMRYFLLTHLCGISHRIWCTLSTICSDAA"),
        feat(396, 453, -1, "misc_feature", note="multiple cloning site"),
        feat(1625, 2486, -1, "gene", gene="bla"),
        feat(1625, 2486, -1, "CDS", gene="bla", codon_start="1", product="beta-lactamase", protein_id="AAA03419.1",
             note="ampicillin resistance", translation="MSIQHFRVALIPFFAAFCLPVFAHPETLVKVKDAEDQLGARVGYIELDLNSGKILESFRPEERFPMMSTFKVLLCGAVLSRIDAGQEQLGRRIHYSQNDLVEYSPVTEKHLTDGMTVRELCSAAITMSDNTAANLLLTTIGGPKELTAFLHNMGDHVTRLDRWEPELNEAIPNDERDTTMPVAMATTLRKLLTGELLTLASRQQLIDWMEADKVAGPLLRSALPAGWFIADKSGAGERGSRGIIAALGPDGKPSRIVVIYTTGSQATMDERNRQIAEIGASLIKHW"),
        feat(867, 1456, 1, "rep_origin", note="pMB1 origin of replication"),
    ]
    SeqIO.write([rec], os.path.join(out, "pUC19.gb"), "genbank")
except ImportError:
    note("Biopython not found; skipping pUC19.gb")

# ── Nanopore: a POD5 run ─────────────────────────────────────────────────────
try:
    import datetime, uuid
    import numpy as np
    import pod5
    run = pod5.RunInfo(
        acquisition_id="a3f9c2d1e8b7", acquisition_start_time=datetime.datetime(2026, 9, 30, 9, 15),
        adc_max=2047, adc_min=-2048, context_tags={"sequencing_kit": "sqk-lsk114", "basecall_config_filename": "dna_r10.4.1_e8.2_400bps_hac.cfg"},
        experiment_name="PBMC_ONT_run1", flow_cell_id="PAW12345", flow_cell_product_code="FLO-PRO114M",
        protocol_name="sequencing/sequencing_PRO114_DNA_e8_2_400K:FLO-PRO114M:SQK-LSK114",
        protocol_run_id="7d1c0f2e-run", protocol_start_time=datetime.datetime(2026, 9, 30, 9, 10),
        sample_id="PBMC_donor1", sample_rate=5000, sequencing_kit="sqk-lsk114", sequencer_position="1A",
        sequencer_position_type="PromethION", software="MinKNOW 25.09", system_name="PC24A1",
        system_type="PromethION 24", tracking_id={"run_id": "7d1c0f2e"})
    rng = np.random.default_rng(5)
    reads = []
    for k in range(400):
        n = int(rng.integers(4000, 40000))
        sig = (700 + np.cumsum(rng.normal(0, 6, n))).astype(np.int16)
        reads.append(pod5.Read(
            read_id=uuid.UUID(int=int(rng.integers(0, 2**62)) << 64 | int(rng.integers(0, 2**62))),
            end_reason=pod5.EndReason.from_reason_with_default_forced(
                pod5.EndReasonEnum.SIGNAL_POSITIVE if rng.random() < 0.9 else pod5.EndReasonEnum.MUX_CHANGE),
            calibration=pod5.Calibration(offset=-243.0, scale=0.1462), pore=pod5.Pore(channel=int(rng.integers(1, 3000)), well=1, pore_type="not_set"),
            read_number=1000 + k, start_sample=int(k * 51234), median_before=float(rng.uniform(190, 230)),
            run_info=run, signal=sig))
    with pod5.Writer(os.path.join(out, "run.pod5")) as w:
        w.add_reads(reads)
except ImportError:
    note("pod5 not found; skipping run.pod5")

# ── BLAST tabular hits and a wiggle track ───────────────────────────────────
with open(os.path.join(out, "hits.m8"), "w") as f:
    for q in range(12):
        for h in range(R.randint(1, 3)):
            g = R.choice(GENES)
            pid = round(R.uniform(78, 100), 3); ln = R.randint(80, 600)
            f.write("contig_%03d\tNM_%06d.%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.2e\t%.1f\n" % (
                q, R.randint(1000, 999999), R.randint(1, 4), pid, ln, int(ln * (100 - pid) / 100), R.randint(0, 3),
                1, ln, R.randint(1, 3000), R.randint(1, 3000), 10 ** -R.uniform(20, 180), ln * pid / 50))
with open(os.path.join(out, "signal.wig"), "w") as f:
    f.write('track type=wiggle_0 name="H3K27ac" description="fold change over input"\n')
    f.write("fixedStep chrom=chr17 start=43044001 step=200 span=200\n")
    for k in range(60):
        f.write("%.2f\n" % max(0.0, 2 + 6 * __import__("math").sin(k / 6) + R.gauss(0, 0.7)))

print(out)
