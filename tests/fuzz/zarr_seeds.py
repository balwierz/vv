#!/usr/bin/env python3
"""Turn the Zarr fixtures (tests/data/*.zarr.zip) into fuzz_zarr seeds.

  zarr_seeds.py OUTDIR

Each seed is a whole store as [u16 key length][key][u32 value length][value]
records, the input format of tests/fuzz/fuzz_zarr.cpp.
"""
import pathlib, struct, sys, zipfile

out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
data = pathlib.Path(__file__).resolve().parent.parent / "data"
for z in sorted(data.glob("*.zarr.zip")):
    rec = b""
    with zipfile.ZipFile(z) as f:
        for name in f.namelist():
            if name.endswith("/"):
                continue
            k, v = name.encode(), f.read(name)
            rec += struct.pack("<H", len(k)) + k + struct.pack("<I", len(v)) + v
    (out / (z.name + ".seed")).write_bytes(rec)
