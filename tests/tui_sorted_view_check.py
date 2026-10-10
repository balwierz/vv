#!/usr/bin/env python3
"""A sorted view draws every visible row, whatever chunks they come from.

The viewer caches four chunks. A sort can put rows of many chunks on one
screen: here consecutive sorted rows sit two row groups apart, so a
20-row screen draws from 20 of the file's 200 row groups. The chunks were
loaded by display row instead of source row, and rows whose chunk was not
cached were drawn blank (one of 20 showed).

Check: 20,000 rows in 200 row groups of 100, key = (i % 200) * 1000 + i // 200,
name = "r<i>". Sorted position p holds i = (p % 100) * 200 + p // 100. With
--sort key, after opening, after G and after PgDn, every row in the status
bar's "Row a-b/20_000" range has its name drawn.

Usage: tui_sorted_view_check.py <vv-binary> <tmpdir>
Exit 0 on success, 1 on failure.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def main():
    vv, tmp = sys.argv[1], sys.argv[2]
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
    except ImportError:
        print("tui_sorted_view: pyarrow missing, skipped")
        return 0
    n, g = 20000, 200
    path = os.path.join(tmp, "sortview.parquet")
    pq.write_table(pa.table({"key": [(i % g) * 1000 + i // g for i in range(n)],
                             "name": ["r%d" % i for i in range(n)]}),
                   path, row_group_size=n // g)
    rc = 0
    for keys, label in (([], "open"), ([b"G"], "G"), ([b"\x1b[6~"], "PgDn")):
        txt, _raw, hung = run(vv, ["--sort", "key", path], keys, cols=100, rows=24, budget=20)
        wins = re.findall(r"Row ([\d_]+)-([\d_]+)/20_000", txt)
        if hung or not wins:
            sys.stderr.write("tui_sorted_view: %s: no status bar%s\n" % (label, " (hung)" if hung else ""))
            rc = 1
            continue
        lo, hi = (int(wins[-1][0].replace("_", "")), int(wins[-1][1].replace("_", "")))
        drawn = set(re.findall(r"\br\d+\b", txt))
        want = ["r%d" % ((p % 100) * 200 + p // 100) for p in range(lo - 1, hi)]
        missing = [w for w in want if w not in drawn]
        if len(want) < 10 or missing:
            sys.stderr.write("tui_sorted_view: %s: rows %d-%d, %d of %d not drawn: %s\n"
                             % (label, lo, hi, len(missing), len(want), " ".join(missing[:8])))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
