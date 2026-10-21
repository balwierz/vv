#!/usr/bin/env python3
"""On a terminal, a Markdown file opens in the TUI: its rendered prose as a
text tab and each GFM table as a further tab (it used to be piped to less).

Checks, on tests/data/tiny.md (prose, two tables):
  1. the prose tab shows the "Lists" heading and the status says tab 1/3;
  2. Tab moves to the first table: its "label" column header is shown;
  3. `/gamma` Enter in the prose tab finds a word further down (the list).

Usage: tui_markdown_check.py <vv-binary> <tiny.md>
Exit 0 on success, 1 on failure.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def main():
    vv, md = sys.argv[1], sys.argv[2]
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_markdown: " + msg + "\n")
        rc = 1

    txt, _, hung = run(vv, [md], [])
    if hung or "Lists" not in txt or "tab 1/3" not in txt:
        fail("the prose tab (Lists heading, tab 1/3) is not shown: %r" % txt[-300:])

    txt, _, hung = run(vv, [md], [b"\t"])
    if hung or "label" not in txt or "tab 2/3" not in txt:
        fail("Tab did not show the first table (label column, tab 2/3): %r" % txt[-300:])

    txt, _, hung = run(vv, [md], [b"/", b"g", b"a", b"m", b"m", b"a", b"\r"])
    if hung or "gamma" not in txt:
        fail("/gamma did not show the list item: %r" % txt[-300:])

    if rc == 0:
        print("tui_markdown: 3 checks passed")
    return rc


if __name__ == "__main__":
    sys.exit(main())
