#!/usr/bin/env python3
"""`vv --info --json` on Parquet files agrees with pyarrow's reading of the
same footer.

Writes three files with pyarrow:
  dict.parquet      a low-cardinality string column, dictionary + zstd, three
                    row groups, with nulls; an int column without dictionary
  fallback.parquet  unique long strings with a 1 KiB dictionary page limit, so
                    every row group falls back from dictionary to PLAIN
  mixed.parquet     per-column codecs (snappy / gzip / none), a struct column
                    (two Parquet leaves), DECIMAL, INT96 timestamps, a page
                    index, declared sorting columns

and checks, per column, the physical / logical type, codec, compressed and
uncompressed sizes, null count and dictionary use against
pyarrow.parquet.ParquetFile(f).metadata; and per file the row-group count,
codecs and page index coverage.

Usage: parquet_info_check.py <vv-binary> <scratch-dir>
Exit 0 on success, 1 on failure.
"""
import decimal, json, os, subprocess, sys

import pyarrow as pa
import pyarrow.parquet as pq

errors = []


def fail(msg):
    errors.append(msg)
    sys.stderr.write("parquet_info: " + msg + "\n")


def size_text(n):
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KiB" % (n / 1024.0)
    if n < 1024 ** 3:
        return "%.2f MiB" % (n / (1024.0 * 1024))
    return "%.2f GiB" % (n / (1024.0 ** 3))


CODEC = {"SNAPPY": "snappy", "GZIP": "gzip", "ZSTD": "zstd", "UNCOMPRESSED": "none",
         "LZ4": "lz4", "BROTLI": "brotli"}


def info(vv, path):
    out = subprocess.run([vv, "--info", "--json", path], capture_output=True, text=True)
    if out.returncode != 0:
        fail("%s: vv --info exit %d: %s" % (path, out.returncode, out.stderr.strip()))
        return None
    return json.loads(out.stdout)


def leaf_rows(vv_cols, leaf_paths):
    """The vv rows of each Parquet leaf, keyed by its dot path."""
    rows = {}
    for name, col in vv_cols.items():
        children = {k: v for k, v in col.items() if k != "value"}
        if "Physical type" in children:            # one leaf
            leaf = [p for p in leaf_paths if p == name or p.startswith(name + ".")]
            rows[leaf[0] if leaf else name] = children
        else:                                       # several leaves
            for k, v in children.items():
                if isinstance(v, dict):
                    rows[k] = v
    return rows


def check_file(vv, path, expect_encoding):
    d = info(vv, path)
    if d is None:
        return
    md = pq.ParquetFile(path).metadata
    name = os.path.basename(path)
    if d["File"].get("Rows") != "{:_}".format(md.num_rows):
        fail("%s: Rows %r, expected %d" % (name, d["File"].get("Rows"), md.num_rows))
    if not d["Parquet"]["Row groups"].startswith(str(md.num_row_groups)):
        fail("%s: Row groups %r, expected %d" % (name, d["Parquet"]["Row groups"], md.num_row_groups))
    leaf_paths = [md.schema.column(i).path for i in range(md.num_columns)]
    rows = leaf_rows(d["Columns"], leaf_paths)
    for c in range(md.num_columns):
        p = leaf_paths[c]
        r = rows.get(p)
        if r is None:
            fail("%s: no rows for leaf %s (have %s)" % (name, p, sorted(rows)))
            continue
        chunks = [md.row_group(g).column(c) for g in range(md.num_row_groups)]
        desc = md.schema.column(c)
        phys = desc.physical_type
        if phys == "FIXED_LEN_BYTE_ARRAY":
            phys = "FIXED_LEN_BYTE_ARRAY(%d)" % desc.length
        if r.get("Physical type") != phys:
            fail("%s %s: Physical type %r, expected %r" % (name, p, r.get("Physical type"), phys))
        lt = desc.logical_type.type
        if lt != "NONE" and r.get("Logical type") != str(desc.logical_type):
            fail("%s %s: Logical type %r, expected %r" % (name, p, r.get("Logical type"), str(desc.logical_type)))
        codecs = ", ".join(sorted({CODEC.get(ch.compression, ch.compression.lower()) for ch in chunks}))
        if r.get("Codec") != codecs:
            fail("%s %s: Codec %r, expected %r" % (name, p, r.get("Codec"), codecs))
        comp = sum(ch.total_compressed_size for ch in chunks)
        raw = sum(ch.total_uncompressed_size for ch in chunks)
        if r.get("Compressed") != size_text(comp):
            fail("%s %s: Compressed %r, expected %r" % (name, p, r.get("Compressed"), size_text(comp)))
        if not r.get("Uncompressed", "").startswith(size_text(raw)):
            fail("%s %s: Uncompressed %r, expected %r" % (name, p, r.get("Uncompressed"), size_text(raw)))
        if all(ch.statistics is not None and ch.statistics.has_null_count for ch in chunks):
            nulls = "{:_}".format(sum(ch.statistics.null_count for ch in chunks))
            if r.get("Nulls") != nulls:
                fail("%s %s: Nulls %r, expected %r" % (name, p, r.get("Nulls"), nulls))
        want = expect_encoding.get(p)
        if want and want not in r.get("Encoding", ""):
            fail("%s %s: Encoding %r does not say %r" % (name, p, r.get("Encoding"), want))
        dict_rgs = sum(ch.has_dictionary_page for ch in chunks)
        if dict_rgs == 0 and "dictionary" in r.get("Encoding", ""):
            fail("%s %s: Encoding %r but no row group has a dictionary page" % (name, p, r.get("Encoding")))
    return d


def main():
    vv, scratch = sys.argv[1], sys.argv[2]

    # 1. Dictionary + zstd, three row groups, nulls.
    n = 3000
    t = pa.table({
        "chrom": pa.array([None if i % 97 == 0 else "chr%d" % (i % 5) for i in range(n)]),
        "pos": pa.array(range(n), pa.int64()),
    })
    p1 = os.path.join(scratch, "dict.parquet")
    pq.write_table(t, p1, compression="zstd", use_dictionary=["chrom"], row_group_size=1000)
    d = check_file(vv, p1, {"chrom": "dictionary in all 3 row groups", "pos": "PLAIN"})
    if d:
        s = d["Columns"]["chrom"]["value"]
        if s != "string · dictionary · zstd · " + d["Columns"]["chrom"]["Compressed"]:
            fail("dict.parquet chrom: summary %r" % s)
        if d["Parquet"].get("Codecs") != "zstd":
            fail("dict.parquet: Codecs %r" % d["Parquet"].get("Codecs"))

    # 2. Dictionary falling back to PLAIN in every row group.
    t = pa.table({"id": pa.array(["read_%08d_%s" % (i, "x" * 40) for i in range(4000)])})
    p2 = os.path.join(scratch, "fallback.parquet")
    pq.write_table(t, p2, compression="snappy", use_dictionary=True,
                   dictionary_pagesize_limit=1024, row_group_size=2000)
    d = check_file(vv, p2, {"id": "dictionary in both row groups, falls back to PLAIN in 2 of them"})
    if d and "dictionary + PLAIN" not in d["Columns"]["id"]["value"]:
        fail("fallback.parquet id: summary %r" % d["Columns"]["id"]["value"])

    # 3. Per-column codecs, a struct, DECIMAL, INT96, page index, sorting.
    m = 500
    t = pa.table({
        "a": pa.array(range(m), pa.int32()),
        "s": pa.array([{"x": i, "y": "v%d" % (i % 3)} for i in range(m)]),
        "dec": pa.array([decimal.Decimal("%d.%02d" % (i, i % 100)) for i in range(m)], pa.decimal128(10, 2)),
        "ts": pa.array([1_600_000_000_000_000 + i for i in range(m)], pa.timestamp("us")),
    })
    p3 = os.path.join(scratch, "mixed.parquet")
    pq.write_table(t, p3, compression={"a": "snappy", "s.x": "gzip", "s.y": "gzip",
                                       "dec": "none", "ts": "zstd"},
                   use_dictionary=False, write_page_index=True,
                   use_deprecated_int96_timestamps=True,
                   sorting_columns=[pq.SortingColumn(0)], row_group_size=250)
    d = check_file(vv, p3, {"a": "PLAIN"})
    if d:
        pqs = d["Parquet"]
        if "gzip" not in pqs.get("Codecs", "") or "snappy" not in pqs.get("Codecs", ""):
            fail("mixed.parquet: Codecs %r" % pqs.get("Codecs"))
        # INT96 has no sort order, so the writer gives it no column index.
        md = pq.ParquetFile(p3).metadata
        chunks = [md.row_group(g).column(c) for g in range(md.num_row_groups)
                  for c in range(md.num_columns)]
        k = sum(ch.has_column_index for ch in chunks)
        want = "every column chunk" if k == len(chunks) else "%d of %d column chunks" % (k, len(chunks))
        if pqs.get("Column index") != want:
            fail("mixed.parquet: Column index %r, expected %r" % (pqs.get("Column index"), want))
        k = sum(ch.has_offset_index for ch in chunks)
        want = "every column chunk" if k == len(chunks) else "%d of %d column chunks" % (k, len(chunks))
        if pqs.get("Offset index") != want:
            fail("mixed.parquet: Offset index %r, expected %r" % (pqs.get("Offset index"), want))
        if pqs.get("Sorted by") != "a ascending":
            fail("mixed.parquet: Sorted by %r" % pqs.get("Sorted by"))
        s = d["Columns"].get("s", {})
        if "2 Parquet columns" not in s.get("value", ""):
            fail("mixed.parquet s: summary %r" % s.get("value"))
        if d["Columns"]["ts"].get("Physical type") != "INT96":
            fail("mixed.parquet ts: Physical type %r" % d["Columns"]["ts"].get("Physical type"))
        if d["Columns"]["dec"].get("Max") != "499.99":
            fail("mixed.parquet dec: Max %r" % d["Columns"]["dec"].get("Max"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
