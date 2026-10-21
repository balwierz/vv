# Benchmarks

How long vv takes, and how much memory it uses, for everyday tasks on public
files of realistic size — next to the tool or library one would otherwise
use for the same task. Everything here can be re-run (see the end).

## Method

- **Inputs** (downloaded by `scripts/bench/fetch.sh`, checksums pinned):
  1000 Genomes phase 3 chr22 VCF (1.1 M variants × 2,504 samples, 206 MB
  bgzip, with `.tbi`); NA12878 chr20 low-coverage BAM (3.2 M reads, 312 MB,
  with `.bai`); GENCODE v47 basic annotation GFF3 (41 MB gzip; 2.2 M
  records), also as Parquet (attributes as columns) and as CSV.gz, both
  written by vv; hg38 chr22 FASTA with `.fai`; ENCODE ENCFF247SXC bigWig
  (fold change, 336 MB); the CELLxGENE breast cancer atlas AnnData
  (100,064 cells, 848 MB `.h5ad`).
- **Alternatives**: the command-line tool for the format where there is one
  (`samtools`, `bcftools`, UCSC `bigWigToBedGraph`, `zcat` + `awk`), else
  the usual Python library (pyarrow, pandas, anndata, pyBigWig), timed as a
  script run from the shell so interpreter start and imports count, as in an
  interactive session (`scripts/bench/comparators.py`).
- **Measure**: GNU `time`, wall seconds and peak RSS; best of 3 runs, output
  to `/dev/null`, warm page cache. A preview (`-n 20 -t`) is compared with
  printing the first 20 records the other way.

## Results

vv 1.31.1 plus the changes on main to b7c8ee5, Linux 7.2.11, AMD Ryzen 9
9950X3D (16 cores), 60 GB RAM; best of 3 runs, warm cache, 2026-10-21.

| Task | vv | | Alternative | | |
|---|---:|---:|---|---:|---:|
| VCF: first 20 records | 0.29 s | 224 MB | bcftools view \| head | 0.00 s | 8 MB |
| VCF: -r 1 Mb, count | 0.14 s | 44 MB | bcftools view -H -r \| wc -l | 0.89 s | 8 MB |
| VCF: sites to TSV (CHROM POS REF ALT) | 1.14 s | 65 MB | bcftools query | 1.98 s | 8 MB |
| BAM: first 20 reads | 0.01 s | 46 MB | samtools view \| head | 0.00 s | 4 MB |
| BAM: -r 1 Mb, count | 0.01 s | 52 MB | samtools view -c REGION | 0.01 s | 4 MB |
| BAM: count MAPQ >= 30 (full scan) | 0.73 s | 882 MB | samtools view -c -q 30 | 0.87 s | 4 MB |
| BAM: export to TSV/SAM | 0.73 s | 962 MB | samtools view | 1.35 s | 4 MB |
| GFF3: first 20 records | 0.03 s | 109 MB | zcat \| head | 0.00 s | 4 MB |
| GFF3: count genes | 0.61 s | 300 MB | zcat \| awk | 0.68 s | 4 MB |
| Parquet: first 20 rows | 0.01 s | 66 MB | pyarrow iter_batches | 0.20 s | 158 MB |
| Parquet: --describe | 0.56 s | 321 MB | pandas describe() | 5.15 s | 4763 MB |
| Parquet: export to TSV | 0.33 s | 505 MB | pyarrow write_csv | 0.75 s | 2946 MB |
| Parquet: sort by start, first 5 | 1.51 s | 5082 MB | pandas sort_values | 2.05 s | 5096 MB |
| Parquet: count exons | 0.04 s | 35 MB | pyarrow compute | 0.21 s | 260 MB |
| CSV.gz: first 20 rows | 0.05 s | 162 MB | pandas read_csv(nrows=20) | 0.19 s | 121 MB |
| CSV.gz: count rows | 0.79 s | 448 MB | pyarrow read_csv | 0.90 s | 4920 MB |
| FASTA: -r 10 kb | 0.00 s | 23 MB | samtools faidx | 0.00 s | 4 MB |
| bigWig: -r 1 Mb, count | 0.01 s | 26 MB | pyBigWig intervals | 0.07 s | 51 MB |
| bigWig: -r 1 Mb, TSV | 0.01 s | 29 MB | bigWigToBedGraph | 0.01 s | 8 MB |
| h5ad: obs, first 20 rows | 0.01 s | 32 MB | anndata backed obs.head | 0.67 s | 453 MB |
| h5ad: obs to TSV | 0.10 s | 76 MB | anndata obs.to_csv | 1.21 s | 457 MB |
| h5ad: obs --describe | 0.15 s | 49 MB | pandas describe() | 0.71 s | 454 MB |
| h5ad: X, first 5 rows | 0.00 s | 30 MB | anndata backed X[:5] | 0.67 s | 453 MB |

## Reading the table

- **AnnData, Parquet**: vv answers in tens of milliseconds what takes the
  Python stack 0.2–1.2 s (most of it imports and opening the file), with a
  fraction of the memory; `--describe` of the 2.2 M-row Parquet is 9× faster
  than pandas and uses 7 % of its memory.
- **BAM, FASTA, bigWig, region queries**: vv reads these through htslib and
  libBigWig, as samtools, bcftools and the UCSC tools do, so times are
  alike; a full BAM export is faster (threaded formatting), at a higher
  peak memory.
- **Text formats**: vv parses delimited text with Apache Arrow's CSV reader:
  types come from the first 16 MiB, the rest is parsed in 2 MiB blocks on
  the `-@` threads for a full scan, and a bgzip file is decoded on them
  too. A VCF read for its site columns has its lines cut after INFO before
  they are parsed, so the 2,504 sample fields of each chr22 record are never
  tokenised: sites to TSV takes 1.1 s against `bcftools query`'s 2.0 s (it
  took 36 s in vv 1.30). A preview of that VCF (`-n 20`, every column shown)
  still parses the first block's sample fields, 0.29 s. A plain-gzip file
  (the GENCODE CSV.gz) decodes on one thread, which bounds its full scans.

## Re-running it

```sh
scripts/bench/fetch.sh ~/vv-bench          # ~1.8 GB, resumable, checksums pinned
pip install -r scripts/bench/requirements.txt
scripts/bench/run.sh ~/vv-bench            # Markdown table on stdout, ~/vv-bench/results.tsv
```

`run.sh -v PATH` picks the vv binary and `-n N` the number of runs. A tool
that is not installed gets a `-` row instead of a time.
