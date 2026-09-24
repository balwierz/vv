#!/usr/bin/env python3
"""`/` search, `n` / `N` and `:N` move the cell cursor to their target.

They used to move only the viewport (top_row_). The next draw ran
ensure_cursor_visible(), which scrolled the view back to the unchanged cursor,
so a match or a row past the first screen was never shown: the keys appeared
to do nothing.

Checks, on a 300-row file whose only "needle" is in column 2 of row 150:
  1. `/needle` Enter shows row 150 and puts the cursor on the matching cell —
     `y` then copies "needle", not a column-1 value;
  2. `:250` Enter shows row 250;
  3. `n` after the match wraps (the file has one match) and stays on row 150.
Discriminating: on the previous build checks 1-3 fail (row 150 / 250 never
appear; `y` copies row 0's first cell).

Usage: tui_search_goto_check.py <vv-binary> <scratch-dir>
Exit 0 on success, 1 on failure.
"""
import base64, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def main():
    vv, scratch = sys.argv[1], sys.argv[2]
    path = os.path.join(scratch, "search_goto.tsv")
    with open(path, "w") as f:
        f.write("id\tname\tscore\n")
        for i in range(300):
            f.write("r%d\t%s\t%d\n" % (i, "needle" if i == 150 else "hay%d" % i, i))
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_search_goto: " + msg + "\n")
        rc = 1

    def rows_on_screen(txt):
        return set(int(m) for m in re.findall(r"\br(\d+)\b", txt))

    # 1. Search, then yank the cursor's cell.
    txt, raw, hung = run(vv, [path], [b"/", b"n", b"e", b"e", b"d", b"l", b"e",
                                      b"\r", b"y"])
    if hung:
        fail("hung on /needle")
    else:
        if 150 not in rows_on_screen(txt):
            fail("row 150 (the match) is not on screen after /needle")
        mm = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]+)", raw)
        got = base64.b64decode(mm[-1]).decode("utf8", "replace").strip() if mm else None
        if got != "needle":
            fail("y after the search copied %r, expected the matching cell 'needle'" % got)

    # 2. :N jumps to that row.
    txt, _raw, hung = run(vv, [path], [b":", b"2", b"5", b"0", b"\r"])
    if hung:
        fail("hung on :250")
    elif 250 not in rows_on_screen(txt):
        fail("row 250 is not on screen after :250")

    # 3. n with a single match wraps back onto it.
    txt, raw, hung = run(vv, [path], [b"/", b"n", b"e", b"e", b"d", b"l", b"e",
                                      b"\r", b"n", b"y"])
    if hung:
        fail("hung on /needle n")
    else:
        if 150 not in rows_on_screen(txt):
            fail("row 150 is not on screen after n")
        mm = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]+)", raw)
        got = base64.b64decode(mm[-1]).decode("utf8", "replace").strip() if mm else None
        if got != "needle":
            fail("y after n copied %r, expected 'needle'" % got)

    return rc


if __name__ == "__main__":
    sys.exit(main())
