#!/usr/bin/env python3
"""The Python side of the vv benchmark: one subcommand per task, written the
way a user would do it with pyarrow / pandas / anndata / pyBigWig, so the
timing includes interpreter start and imports, as in an interactive session.

Usage: comparators.py TASK ARGS...   (see TASKS below)
"""
import sys


def parquet_head(path, n):
    import pyarrow.parquet as pq
    batch = next(pq.ParquetFile(path).iter_batches(batch_size=int(n)))
    print(batch.to_pandas().to_string())


def parquet_tsv(path):
    import pyarrow.csv as pc
    import pyarrow.parquet as pq
    pc.write_csv(pq.read_table(path), sys.stdout.buffer,
                 pc.WriteOptions(delimiter="\t", quoting_style="none"))


def parquet_describe(path):
    import pandas as pd
    print(pd.read_parquet(path).describe(include="all").to_string())


def parquet_sort(path, col, n):
    import pandas as pd
    print(pd.read_parquet(path).sort_values(col).head(int(n)).to_string())


def parquet_count_eq(path, col, value):
    import pyarrow.compute as pc
    import pyarrow.parquet as pq
    t = pq.read_table(path, columns=[col])
    print(pc.sum(pc.equal(t[col], value)).as_py())


def csv_head(path, n):
    import pandas as pd
    print(pd.read_csv(path, nrows=int(n)).to_string())


def csv_count(path):
    import pyarrow.csv as pc
    print(pc.read_csv(path).num_rows)


def h5ad_obs_head(path, n):
    import anndata as ad
    print(ad.read_h5ad(path, backed="r").obs.head(int(n)).to_string())


def h5ad_obs_tsv(path):
    import anndata as ad
    ad.read_h5ad(path, backed="r").obs.to_csv(sys.stdout, sep="\t")


def h5ad_obs_describe(path):
    import anndata as ad
    print(ad.read_h5ad(path, backed="r").obs.describe(include="all").to_string())


def h5ad_x_head(path, n):
    import anndata as ad
    x = ad.read_h5ad(path, backed="r").X[: int(n)]
    print(x.toarray() if hasattr(x, "toarray") else x)


def bigwig_region_count(path, chrom, start, end):
    import pyBigWig
    print(len(pyBigWig.open(path).intervals(chrom, int(start), int(end)) or []))


TASKS = {f.__name__: f for f in (
    parquet_head, parquet_tsv, parquet_describe, parquet_sort, parquet_count_eq,
    csv_head, csv_count, h5ad_obs_head, h5ad_obs_tsv, h5ad_obs_describe, h5ad_x_head,
    bigwig_region_count)}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in TASKS:
        sys.exit("usage: comparators.py TASK ARGS...\ntasks: " + " ".join(TASKS))
    try:
        TASKS[sys.argv[1]](*sys.argv[2:])
    except BrokenPipeError:
        pass
