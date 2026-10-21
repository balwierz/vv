#!/usr/bin/env python3
"""Unused Parquet columns, invisible workbook sheets and IPC prefix decoding."""
import pathlib
import subprocess
import sys
import zipfile

import pyarrow as pa
import pyarrow.ipc as ipc
import pyarrow.parquet as pq


def main():
    binary, fixtures, scratch = sys.argv[1:4]
    root = pathlib.Path(scratch)
    source = pathlib.Path(fixtures)
    dataset = root / "projected-parquet"
    dataset.mkdir(exist_ok=True)
    part = dataset / "part.parquet"
    pq.write_table(pa.table({"id": list(range(10)), "unused": [b"x" * 100000] * 10}),
                   part, compression="zstd", use_dictionary=False)
    # Preserve the footer and the id column; corrupt only an unused page.
    column = pq.ParquetFile(part).metadata.row_group(0).column(1)
    data = bytearray(part.read_bytes())
    start = column.data_page_offset
    data[start:start + min(32, column.total_compressed_size)] = b"\xff" * min(32, column.total_compressed_size)
    part.write_bytes(data)
    result = subprocess.run([binary, "--tsv", "--no-header", "--select", "id", str(dataset)],
                            text=True, capture_output=True, timeout=20)
    assert result.returncode == 0 and result.stdout == "".join("%d\n" % i for i in range(10)), result
    result = subprocess.run([binary, "--tsv", str(dataset)], capture_output=True, timeout=20)
    assert result.returncode != 0, "unprojected corrupt payload was not reported"

    # A sheet that cannot be parsed must not be opened (or dropped from the tab
    # list) just to enumerate siblings. The selected bad sheet still errors.
    workbook = root / "lazy-sheets.xlsx"
    with zipfile.ZipFile(source / "tiny.xlsx") as original:
        sheets = [name for name in original.namelist() if name.startswith("xl/worksheets/sheet") and name.endswith(".xml")]
        assert len(sheets) >= 2
        with zipfile.ZipFile(workbook, "w") as output:
            for name in original.namelist():
                output.writestr(name, b"<worksheet/>" if name == sheets[1] else original.read(name))
    original_tabs = subprocess.run([binary, "--list-tabs", str(source / "tiny.xlsx")],
                                   text=True, capture_output=True, check=True, timeout=20).stdout
    tabs = subprocess.run([binary, "--list-tabs", str(workbook)],
                          text=True, capture_output=True, check=True, timeout=20)
    assert tabs.stdout == original_tabs and not tabs.stderr, tabs
    bad = subprocess.run([binary, "--tab", original_tabs.splitlines()[1], "--tsv", str(workbook)],
                         capture_output=True, timeout=20)
    assert bad.returncode != 0 and not bad.stdout, bad

    if "--tui" in sys.argv[4:]:
        from tui_cursor_check import run

        arrow = root / "lazy-prefix.arrow"
        schema = pa.schema([("id", pa.int64())])
        with ipc.new_file(arrow, schema) as writer:
            writer.write_batch(pa.record_batch([pa.array(range(1024))], schema=schema))
            for i in range(20):
                writer.write_batch(pa.record_batch([pa.array([1024 + i])], schema=schema))
        data = bytearray(arrow.read_bytes())
        markers = []
        offset = 0
        while True:
            offset = data.find(b"\xff\xff\xff\xff", offset)
            if offset < 0:
                break
            markers.append(offset)
            offset += 4
        broken = markers[6] + 8
        data[broken:broken + 16] = bytes(x ^ 255 for x in data[broken:broken + 16])
        arrow.write_bytes(data)
        text, _, hung = run(binary, [str(arrow)], [], budget=12)
        assert not hung and "Row 1-" in text and "[READ ERROR:" not in text, text[-1000:]
        text, _, hung = run(binary, [str(arrow)], [b"G"], budget=12)
        assert not hung and "[READ ERROR:" in text, text[-1000:]
    print("lazy-reader projection, sheet activation and IPC frontier checks passed")


if __name__ == "__main__":
    main()
