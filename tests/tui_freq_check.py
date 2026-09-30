#!/usr/bin/env python3
"""TUI value-count sheet: F counts the values of the cursor's column over the
rows the live filter keeps; Enter narrows the filter to the value under the
sheet's cursor.

Checks, on tiny.parquet (20 rows; Chr chr1 x12 / chr2 x8; Score 0 .. 0.95 by
row, float32; Tags a list column):
  1. F on Chr lists chr1 12 (60.0%) and chr2 8 (40.0%);
  2. Enter filters to Chr == "chr1": "Row 1-12/12";
  3. under --filter 'Score > 0.5' (rows 11-19: chr1 x1, chr2 x8) the counts
     cover the filtered rows only, and Enter ANDs: 8 rows;
  4. under an OR filter ('Score < 0.1 OR Score > 0.9': chr1 x2, chr2 x1) the
     value is ANDed into both branches: j + Enter gives chr2's 1 row;
  5. on the float32 Score column, j + Enter filters to Score == 0.05 and
     finds its one row;
  6. on the list column Tags, Enter says why it cannot filter and leaves the
     view unfiltered.
Discriminating: on the previous build F does nothing (no sheet, no filter).

Usage: tui_freq_check.py <vv-binary> <tiny.parquet>
Exit 0 on success, 1 on failure.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def main():
    vv, data = sys.argv[1], sys.argv[2]
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_freq: " + msg + "\n")
        rc = 1

    def status(txt):
        m = re.findall(r"Row \d+-\d+/\d+  Col \d+-\d+/\d+[^\[]*", txt)
        return m[-1] if m else ""

    # 1. The sheet.
    txt, _raw, hung = run(vv, [data], [b"F"])
    if hung or "value counts: Chr" not in txt:
        fail("F opened no value-count sheet for Chr")
    elif not (re.search(r"12\s+60\.0%\s+chr1", txt) and re.search(r"8\s+40\.0%\s+chr2", txt)):
        fail("F did not count chr1 12 / chr2 8")

    # 2. Enter filters to the value.
    txt, _raw, hung = run(vv, [data], [b"F", b"\r"])
    if hung or not status(txt).startswith("Row 1-12/12"):
        fail("Enter did not filter to chr1: %r" % status(txt))

    # 3. Counts over the filtered rows; Enter ANDs with the filter.
    txt, _raw, hung = run(vv, ["--filter", "Score > 0.5", data], [b"F"])
    if hung or not (re.search(r"8\s+88\.9%\s+chr2", txt) and re.search(r"1\s+11\.1%\s+chr1", txt)):
        fail("F did not count the filtered rows (chr2 8, chr1 1)")
    txt, _raw, hung = run(vv, ["--filter", "Score > 0.5", data], [b"F", b"\r"])
    if hung or not status(txt).startswith("Row 1-8/8"):
        fail("Enter under a filter did not AND: %r" % status(txt))

    # 4. An OR filter: the value goes into every branch.
    txt, _raw, hung = run(vv, ["--filter", "Score < 0.1 OR Score > 0.9", data],
                          [b"F", b"j", b"\r"])
    if hung or not status(txt).startswith("Row 1-1/1"):
        fail("Enter under an OR filter: %r" % status(txt))

    # 5. float32: Score == 0.05 finds its row. Score is the 4th column.
    txt, _raw, hung = run(vv, [data], [b"l", b"l", b"l", b"F", b"j", b"\r"])
    st = status(txt)
    if hung or not st.startswith("Row 1-1/1") or "Score == 0.05" not in st:
        fail("Enter on float32 Score did not filter to 0.05: %r" % st)

    # 6. A list column cannot be filtered with ==; say so, keep the view.
    txt, _raw, hung = run(vv, [data], [b"l", b"l", b"l", b"l", b"F", b"\r"])
    if hung or "cannot be filtered" not in txt:
        fail("Enter on the list column Tags gave no reason")
    txt, _raw, hung = run(vv, [data], [b"l", b"l", b"l", b"l", b"F", b"\r", b"\x1b"])
    if hung or not status(txt).startswith("Row 1-20/20"):
        fail("the list column changed the view: %r" % status(txt))
    return rc


if __name__ == "__main__":
    sys.exit(main())
