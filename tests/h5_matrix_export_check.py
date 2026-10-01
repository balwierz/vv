#!/usr/bin/env python3
"""A capped Loom, Cell Ranger or AnnData CSC matrix exports in full.

Builds, with h5py, a Loom file (250 genes × 1200 cells, int32 /matrix and a
float32 layer; a repeated gene name gets its Accession appended) and a Cell
Ranger v3 file (300 features × 1500 barcodes, CSC by barcode). Both are
larger than the 1000 × 200 preview. `--tab matrix --tsv` (and the layer) must
write every cell and gene, labelled like the preview, with values equal to
the stored matrix transposed. An AnnData file whose X is a CSC matrix
(1200 cells × 300 genes, written with h5py in AnnData's on-disk layout) must
export as cells × genes equal to the dense matrix (transposed to CSR in
memory by vv).

`--matrix long` on the same tabs must give, row for row, the non-zero cells
of the wide export as (row label, column label, value).

Usage: h5_matrix_export_check.py <vv-binary> <tmpdir>
Exit 0 on success, 1 on failure.
"""
import csv, io, os, subprocess, sys

import h5py
import numpy as np
import scipy.sparse as sp


def export(vv, tab, path):
    r = subprocess.run([vv, "--tab", tab, "--tsv", path], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("h5_matrix_export: %s %s: exit %d: %s" % (path, tab, r.returncode, r.stderr))
    rows = list(csv.reader(io.StringIO(r.stdout), delimiter="\t"))
    return rows[0], [row[0] for row in rows[1:]], np.array([[float(v) for v in row[1:]] for row in rows[1:]])


def long_matches_wide(vv, tab, path):
    head, labels, vals = export(vv, tab, path)
    r = subprocess.run([vv, "--matrix", "long", "--tab", tab, "--tsv", path], capture_output=True, text=True)
    if r.returncode != 0:
        return "exit %d: %s" % (r.returncode, r.stderr)
    rows = list(csv.reader(io.StringIO(r.stdout), delimiter="\t"))[1:]
    got = [(a, b, float(c)) for a, b, c in rows]
    want = [(labels[i], head[j + 1], vals[i, j]) for i in range(vals.shape[0])
            for j in range(vals.shape[1]) if vals[i, j] != 0]
    return "" if got == want else "%d long rows, %d non-zero wide cells" % (len(got), len(want))


def main():
    vv, tmp = sys.argv[1], sys.argv[2]
    rng = np.random.default_rng(1)
    bad = []

    G, C = 250, 1200
    loom = os.path.join(tmp, "big.loom")
    m = rng.poisson(0.5, (G, C)).astype(np.int32)
    layer = rng.random((G, C)).astype(np.float32)
    with h5py.File(loom, "w") as f:
        f["matrix"] = m
        f.create_group("row_attrs")["Gene"] = np.array([("g%d" % (i % 240)).encode() for i in range(G)])
        f["row_attrs"]["Accession"] = np.array([("ENSG%d" % i).encode() for i in range(G)])
        f.create_group("col_attrs")["CellID"] = np.array([("c%d" % i).encode() for i in range(C)])
        f.create_group("layers")["spliced"] = layer
    head, labels, vals = export(vv, "matrix", loom)
    if head[:2] != ["CellID", "g0 (ENSG0)"] or head[11] != "g10" or len(head) != G + 1:
        bad.append("loom header %r ... (%d columns)" % (head[:3], len(head)))
    if labels[:2] != ["c0", "c1"] or len(labels) != C or not np.array_equal(vals, m.T):
        bad.append("loom matrix values / cell labels")
    _, _, lv = export(vv, "layers[spliced]", loom)
    if not np.allclose(lv, layer.T.astype(np.float64)):
        bad.append("loom layer values")

    nf, nb = 300, 1500
    tenx = os.path.join(tmp, "big10x.h5")
    X = sp.random(nf, nb, density=0.05, format="csc", random_state=2, dtype=np.float64)
    X.data = np.ceil(X.data * 10)
    with h5py.File(tenx, "w") as f:
        g = f.create_group("matrix")
        g["barcodes"] = np.array([("BC%d" % i).encode() for i in range(nb)])
        g["data"] = X.data.astype(np.int32)
        g["indices"] = X.indices.astype(np.int64)
        g["indptr"] = X.indptr.astype(np.int64)
        g["shape"] = np.array([nf, nb], dtype=np.int32)
        fe = g.create_group("features")
        fe["id"] = np.array([("ID%d" % i).encode() for i in range(nf)])
        fe["name"] = np.array([("G%d" % (i % 290)).encode() for i in range(nf)])
        fe["feature_type"] = np.array([b"Gene Expression"] * nf)
        fe["genome"] = np.array([b"GRCh38"] * nf)
    head, labels, vals = export(vv, "matrix", tenx)
    if head[:2] != ["barcode", "G0 (ID0)"] or head[11] != "G10" or len(set(head)) != nf + 1:
        bad.append("10x header %r ... (%d unique of %d)" % (head[:3], len(set(head)), len(head)))
    if labels[:2] != ["BC0", "BC1"] or len(labels) != nb or not np.array_equal(vals, X.toarray().T):
        bad.append("10x matrix values / barcodes")

    nc, ng = 1200, 300
    h5ad = os.path.join(tmp, "csc.h5ad")
    Y = sp.random(nc, ng, density=0.05, format="csc", random_state=4, dtype=np.float64)
    def index_frame(grp, names):
        grp.attrs["encoding-type"] = "dataframe"
        grp.attrs["encoding-version"] = "0.2.0"
        grp.attrs["_index"] = "_index"
        grp.attrs["column-order"] = np.array([], dtype="S")
        grp.create_dataset("_index", data=np.array(names, dtype="S"))
    with h5py.File(h5ad, "w") as f:
        f.attrs["encoding-type"] = "anndata"
        f.attrs["encoding-version"] = "0.1.0"
        x = f.create_group("X")
        x.attrs["encoding-type"] = "csc_matrix"
        x.attrs["encoding-version"] = "0.1.0"
        x.attrs["shape"] = np.array([nc, ng])
        x["data"] = Y.data; x["indices"] = Y.indices.astype(np.int32); x["indptr"] = Y.indptr.astype(np.int64)
        index_frame(f.create_group("obs"), ["c%d" % i for i in range(nc)])
        index_frame(f.create_group("var"), ["g%d" % i for i in range(ng)])
    head, labels, vals = export(vv, "X", h5ad)
    if head[:3] != ["obs", "g0", "g1"] or len(head) != ng + 1 or labels[:2] != ["c0", "c1"] \
            or len(labels) != nc or not np.array_equal(vals, Y.toarray()):
        bad.append("anndata CSC X: header %r, %d rows" % (head[:3], len(labels)))

    for tab, path in (("matrix", loom), ("layers[spliced]", loom), ("matrix", tenx), ("X", h5ad)):
        why = long_matches_wide(vv, tab, path)
        if why:
            bad.append("--matrix long %s %s: %s" % (os.path.basename(path), tab, why))

    for b in bad:
        sys.stderr.write("h5_matrix_export: %s\n" % b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
