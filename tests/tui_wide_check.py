#!/usr/bin/env python3
"""`/` on a matrix tab finds genes and row labels; the detail pane of a wide
tab reads the columns it shows.

A matrix tab's cells are numbers: `/` matches its column names (genes) and
moves the cursor to that column, and searches the row-label column only when
no name matches. The detail pane (Enter) of a tab past 256 columns lists the
columns from its scroll offset down and reads only those.

Checks:
  1. `/gene249` on tiny.dense.h5ad X puts the cursor on column gene249
     (`y` copies row 0's value there, 249), and `n` stays on it (one match);
  2. `/cell2` (no gene matches) moves to row 2, keeping the label column
     (`y` copies "cell2");
  3. Enter on a 600-column TSV shows "columns 1–N of 600"; after paging down,
     every listed column shows its own value (cK: v0_K) and a column past
     100 is listed.

Usage: tui_wide_check.py <vv-binary> <scratch-dir> <tiny.dense.h5ad>
Exit 0 on success, 1 on failure.
"""
import base64, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def yanked(raw):
    mm = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]+)", raw)
    return base64.b64decode(mm[-1]).decode("utf8", "replace").strip() if mm else None


def main():
    vv, scratch, h5ad = sys.argv[1], sys.argv[2], sys.argv[3]
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_wide: " + msg + "\n")
        rc = 1

    def keys(s):
        return [bytes([c]) for c in s.encode()]

    # 1. A gene name moves the cursor to its column; n keeps the one match.
    _txt, raw, hung = run(vv, [h5ad, "--tab", "X"], keys("/gene249") + [b"\r", b"y"])
    if hung:
        fail("hung on /gene249")
    elif yanked(raw) != "249":
        fail("y after /gene249 copied %r, expected row 0's gene249 value '249'" % yanked(raw))
    _txt, raw, hung = run(vv, [h5ad, "--tab", "X"], keys("/gene249") + [b"\r", b"n", b"y"])
    if hung:
        fail("hung on /gene249 n")
    elif yanked(raw) != "249":
        fail("y after /gene249 n copied %r, expected '249'" % yanked(raw))

    # 2. No gene matches: the row labels are searched.
    _txt, raw, hung = run(vv, [h5ad, "--tab", "X"], keys("/cell2") + [b"\r", b"y"])
    if hung:
        fail("hung on /cell2")
    elif yanked(raw) != "cell2":
        fail("y after /cell2 copied %r, expected the row label 'cell2'" % yanked(raw))

    # 3. The detail pane of a 600-column tab.
    path = os.path.join(scratch, "wide600.tsv")
    with open(path, "w") as f:
        f.write("\t".join("c%d" % k for k in range(600)) + "\n")
        for r in range(3):
            f.write("\t".join("v%d_%d" % (r, k) for k in range(600)) + "\n")
    txt, _raw, hung = run(vv, [path], [b"\r"])
    if hung:
        fail("hung opening the detail pane")
    elif not re.search(r"columns 1–\d+ of 600", txt):
        fail("the wide detail pane has no 'columns 1–N of 600' title")
    txt, _raw, hung = run(vv, [path], [b"\r"] + [b"\x1b[6~"] * 8)
    if hung:
        fail("hung paging the detail pane")
    else:
        pairs = re.findall(r"\bc(\d+)\s*: v0_(\d+)\b", txt)
        if not pairs:
            fail("the paged detail pane lists no 'cK: v0_K' lines")
        elif any(a != b for a, b in pairs):
            fail("the detail pane shows a column with another column's value: %r" % pairs[:5])
        elif max(int(a) for a, _ in pairs) <= 100:
            fail("paging did not reach past column 100 (saw up to c%s)" % max(int(a) for a, _ in pairs))
    return rc


if __name__ == "__main__":
    sys.exit(main())
