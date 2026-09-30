#!/usr/bin/env python3
"""A capped Loom or Cell Ranger matrix exports in full, cells × genes.

Builds, with h5py, a Loom file (250 genes × 1200 cells, int32 /matrix and a
float32 layer; a repeated gene name gets its Accession appended) and a Cell
Ranger v3 file (300 features × 1500 barcodes, CSC by barcode). Both are
larger than the 1000 × 200 preview. `--tab matrix --tsv` (and the layer) must
write every cell and gene, labelled like the preview, with values equal to
the stored matrix transposed.

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

    for b in bad:
        sys.stderr.write("h5_matrix_export: %s\n" % b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
