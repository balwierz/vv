#!/usr/bin/env python3
"""Search in a sorted view decodes each chunk once and lands on the right row.

A sort can interleave row groups: here consecutive sorted rows cycle through
every row group of the file, five rows at a time. The sorted search looked
rows up through the 4-entry chunk cache, so each visit to a row group
re-decoded it — 40 row groups × 40 visits. It now scans a chunk whole the
first time one of its rows comes up and records the matching rows.

Check: 20,000 rows in 40 row groups, key = (i % 40) * 100000 + i // 40, one
row named "needle" (i = 12345, sorted position 12,809, 1-based). With
--sort key, `/needle` Enter scrolls the view to a window holding row 12,809
and draws the needle row; `n` wraps back to it (the only match).

Usage: tui_sorted_search_check.py <vv-binary> <tmpdir>
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
        print("tui_sorted_search: pyarrow missing, skipped")
        return 0
    n, g = 20000, 40
    path = os.path.join(tmp, "interleave.parquet")
    names = ["r%d" % i for i in range(n)]
    names[12345] = "needle"
    pq.write_table(pa.table({"key": [(i % g) * 100000 + i // g for i in range(n)],
                             "name": names}),
                   path, row_group_size=n // g)
    # Sorted position of i = 12345: residue 12345 % 40 = 25 → 25 * 500 rows
    # before it, then 12345 // 40 = 308 within its residue.
    want = 25 * (n // g) + 12345 // g + 1
    rc = 0
    for keys, label in (([b"/needle\r"], "search"), ([b"/needle\r", b"n"], "n")):
        txt, _raw, hung = run(vv, ["--sort", "key", path], keys, budget=30)
        wins = re.findall(r"Row ([\d_]+)-([\d_]+)/20_000", txt)
        lo, hi = (int(wins[-1][0].replace("_", "")), int(wins[-1][1].replace("_", ""))) if wins else (0, -1)
        drawn = re.search(r"12_345\s+2_500_308\s+needle", txt[txt.find("/needle"):])
        if hung or not (lo <= want <= hi) or not drawn:
            sys.stderr.write("tui_sorted_search: %s: window %r, needle drawn: %s%s\n"
                             % (label, wins[-1:] if wins else None, bool(drawn),
                                " (hung)" if hung else ""))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
