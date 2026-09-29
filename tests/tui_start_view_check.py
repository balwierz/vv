#!/usr/bin/env python3
"""The TUI opens with --filter / --select / --sort applied as its own live
state (the `&` filter, the column layout, the `s` sort), so they can be
changed or cleared in the viewer.

On a terminal, --filter and --select were ignored, and --sort was baked into a
materialised table the viewer could not undo (no indicator; `u` did nothing).

Checks, on tiny.parquet (20 rows; Score 0 .. 0.95 by row):
  1. --filter 'Score > 0.5' opens at "Row 1-9/9"; Esc clears it to 1-20/20;
  2. --select Score,Chr opens with 2 columns, Score before Chr;
  3. --sort Score:desc shows the "sort:Score ↓" indicator, row 19 first;
  4. u clears that sort (no indicator);
  5. a bad --filter is reported, not ignored.
Discriminating: on the previous build checks 1-4 fail (Row 1-20/20,
Col 1-5/5, no sort indicator).

Usage: tui_start_view_check.py <vv-binary> <tiny.parquet>
Exit 0 on success, 1 on failure.
"""
import os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def main():
    vv, data = sys.argv[1], sys.argv[2]
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_start_view: " + msg + "\n")
        rc = 1

    def status(txt):
        m = re.findall(r"Row \d+-\d+/\d+  Col \d+-\d+/\d+[^\[]*", txt)
        return m[-1] if m else ""

    # 1. --filter, then Esc.
    txt, _raw, hung = run(vv, ["--filter", "Score > 0.5", data], [])
    if hung or not status(txt).startswith("Row 1-9/9"):
        fail("--filter did not open filtered: %r" % status(txt))
    txt, _raw, hung = run(vv, ["--filter", "Score > 0.5", data], [b"\x1b"])
    if hung or not status(txt).startswith("Row 1-20/20"):
        fail("Esc did not clear the --filter: %r" % status(txt))

    # 2. --select: two columns, in the given order.
    txt, _raw, hung = run(vv, ["--select", "Score,Chr", data], [])
    st = status(txt)
    if hung or "Col 1-2/2" not in st:
        fail("--select did not open with its 2 columns: %r" % st)
    else:
        frame = txt[txt.rfind("Score", 0, txt.rfind("Row 1-")) - 200:]
        if not re.search(r"Score\s+Chr\s", frame):
            fail("--select column order is not Score, Chr")

    # 3. --sort: indicator and order.
    txt, _raw, hung = run(vv, ["--sort", "Score:desc", data], [])
    st = status(txt)
    if hung or "sort:Score" not in st:
        fail("--sort did not open as the live sort: %r" % st)
    else:
        # The first data row after the last header repaint. Match on the row
        # index + chrom, not the frame: without a UTF-8 locale (CI) the
        # frame is ASCII.
        heads = list(re.finditer(r"Score\s+Tags", txt))
        tail = txt[heads[-1].end():] if heads else ""
        first = re.search(r"\b(\d+)\s+chr[12]\b", tail)
        if not first or first.group(1) != "19":
            fail("--sort Score:desc does not start at row 19 (got %r)"
                 % (first.group(1) if first else None))

    # 4. u clears it.
    txt, _raw, hung = run(vv, ["--sort", "Score:desc", data], [b"u"])
    if hung or "sort:" in status(txt):
        fail("u did not clear the --sort: %r" % status(txt))

    # 5. A bad filter is an error, not a silently unfiltered view. -i takes
    # the viewer path without a terminal; vv must exit before starting it
    # (driving this through the pty would write to a closed terminal, which
    # macOS reports as EIO).
    p = subprocess.run([vv, "-i", "--filter", "Nope > 1", data],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       timeout=30)
    if p.returncode != 1 or "unknown column 'Nope'" not in p.stderr:
        fail("a bad --filter was not reported (exit %d: %r)" % (p.returncode, p.stderr[-200:]))

    return rc


if __name__ == "__main__":
    sys.exit(main())
