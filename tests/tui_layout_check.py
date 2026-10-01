#!/usr/bin/env python3
"""The viewer opens --matrix long and --samples long in their long layouts.

  1. `--matrix long --tab X tiny.h5ad`: columns obs / var / value (labels
     categorical), 7 rows — the stored entries of the CSR X.
  2. `--samples long tiny.samples.bcf`: a `sample` column and 4 rows (2 records
     x 2 samples), with the typed DP column.
  3. Default (`tiny.samples.bcf`): 2 rows, sample columns shown as structs.

Usage: tui_layout_check.py <vv-binary> <data-dir>
Exit 0 on success, 1 on failure.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def status(txt):
    m = re.findall(r"Row [\d_]+-[\d_]+/[\d_?]+", txt)
    return m[-1] if m else ""


def main():
    vv, data = sys.argv[1], sys.argv[2]
    rc = 0
    cases = [
        (["--matrix", "long", "--tab", "X", os.path.join(data, "tiny.h5ad")],
         "Row 1-7/7", [r"\bobs\b", r"value", r"category\[stri", r"cell0\s+gene3\s+86"]),
        (["--samples", "long", os.path.join(data, "tiny.samples.bcf")],
         "Row 1-4/4", [r"\bsample\b", r"\bGT\b", r"S2\s+0/1\s+\[4, 4\]\s+8"]),
        ([os.path.join(data, "tiny.samples.bcf")],
         "Row 1-2/2", [r"\{GT: 0/1, AD: \[5, 6\], DP: 11\}"]),
    ]
    for args, want_status, patterns in cases:
        txt, _raw, hung = run(vv, args, [], cols=160, budget=8)
        st = status(txt)
        missing = [p for p in patterns if not re.search(p, txt)]
        if hung or st != want_status or missing:
            sys.stderr.write("tui_layout: %s: status %r (want %r), missing %r%s\n"
                             % (" ".join(args[:-1]) or "default", st, want_status, missing,
                                " (hung)" if hung else ""))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
