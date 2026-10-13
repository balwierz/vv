#!/usr/bin/env python3
"""TUI column stats (S): a numeric column shows its distribution.

On tiny.parquet (20 rows; Score 0 .. 0.95 in steps of 0.05, float32; Chr a
string column):
  1. S on Score shows a "Hist" row of block glyphs (U+2581..U+2588) — 20
     values over 32 equal-width bins, so both full bins and gaps — and the
     range 0 .. 0.95 under it;
  2. S on Chr (a string column) shows no "Hist" row.
Discriminating: on the previous build the stats popup has no Hist row.

Usage: tui_stats_hist_check.py <vv-binary> <tiny.parquet>
Exit 0 on success, 1 on failure.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402

BLOCKS = "".join(chr(c) for c in range(0x2581, 0x2589))


def main():
    vv, data = sys.argv[1], sys.argv[2]
    rc = 0

    txt, _raw, hung = run(vv, [data], [b"l", b"l", b"l", b"S"])
    line = next((l for l in txt.splitlines() if "Hist" in l), "")
    if hung or "column stats" not in txt:
        sys.stderr.write("tui_stats_hist: S opened no stats popup on Score\n")
        rc = 1
    elif not line or not any(ch in line for ch in BLOCKS):
        sys.stderr.write("tui_stats_hist: no Hist row of block glyphs on Score\n")
        rc = 1
    elif "0.95" not in txt[txt.find("Hist"):]:   # the range row (ncurses skips its spaces)
        sys.stderr.write("tui_stats_hist: the range 0 .. 0.95 is not under the Hist row\n")
        rc = 1

    txt, _raw, hung = run(vv, [data], [b"S"])
    if hung or "column stats" not in txt:
        sys.stderr.write("tui_stats_hist: S opened no stats popup on Chr\n")
        rc = 1
    elif "Hist" in txt:
        sys.stderr.write("tui_stats_hist: a Hist row on the string column Chr\n")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
