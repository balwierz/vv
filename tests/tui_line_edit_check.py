#!/usr/bin/env python3
"""Line editing and UTF-8 in the TUI's input bars (& filter, : command).

The bars accepted only ASCII 32-126 and Backspace removed the last byte: no
cursor movement, and a UTF-8 character could neither be typed nor deleted
whole. They now edit at a cursor (Left / Right, Home / End, Ctrl-A / E,
Backspace / Delete, Ctrl-U / K / W) in whole UTF-8 characters.

Checks, on tiny.parquet (Score 0 .. 0.95 by row) and a 3-row TSV whose `name`
column holds café / naïve / plain:
  1. "Scre > 0.5", Left x8, "o"  ->  Score > 0.5: Row 1-9/9;
  2. "jk", Ctrl-U, "Score > 0.9 z", Ctrl-W, Backspace  ->  Row 1-1/1;
  3. name == "café" typed as UTF-8 bytes: Row 1-1/1;
  4. "caféX", Backspace x2 (X, then the whole é), "é" again: still 1 row —
     a byte-wise Backspace would leave a stray 0xC3 and match nothing;
  5. Home / Delete: "xScore > 0.5", Home, Delete  ->  Row 1-9/9.
Discriminating: on the previous build all five fail (the editing keys and
the UTF-8 bytes were ignored).

Usage: tui_line_edit_check.py <vv-binary> <tiny.parquet> <tmpdir>
Exit 0 on success, 1 on failure.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402

# xterm-256color terminfo: kcub1=\EOD, khome=\EOH, kdch1=\E[3~ (keypad mode).
LEFT, HOME, DEL = b"\x1bOD", b"\x1bOH", b"\x1b[3~"
BS, CTRL_U, CTRL_W, ENTER = b"\x7f", b"\x15", b"\x17", b"\r"


def keys(*parts):
    """Split plain text into one key per character (a UTF-8 character is one
    write of its bytes); pass special keys through as they are."""
    out = []
    for p in parts:
        if isinstance(p, str):
            out.extend(c.encode("utf-8") for c in p)
        else:
            out.append(p)
    return out


def main():
    vv, data, tmp = sys.argv[1], sys.argv[2], sys.argv[3]
    tsv = os.path.join(tmp, "utf8.tsv")
    with open(tsv, "w", encoding="utf-8") as f:
        f.write("name\tn\ncafé\t1\nnaïve\t2\nplain\t3\n")
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_line_edit: " + msg + "\n")
        rc = 1

    def status(txt):
        m = re.findall(r"Row \d+-\d+/\d+", txt)
        return m[-1] if m else ""

    cases = [
        ("insert after Left", data,
         keys("&", "Scre > 0.5", *[LEFT] * 8, "o", ENTER), "Row 1-9/9"),
        ("Ctrl-U / Ctrl-W", data,
         keys("&", "jk", CTRL_U, "Score > 0.9 z", CTRL_W, BS, ENTER), "Row 1-1/1"),
        ("UTF-8 literal", tsv,
         keys("&", 'name == "café"', ENTER), "Row 1-1/1"),
        ("Backspace removes a whole character", tsv,
         keys("&", 'name == "caféX', BS, BS, 'é"', ENTER), "Row 1-1/1"),
        ("Home / Delete", data,
         keys("&", "xScore > 0.5", HOME, DEL, ENTER), "Row 1-9/9"),
    ]
    for label, path, ks, want in cases:
        # Keys go 0.3 s apart; the budget leaves room for a slow runner.
        txt, _raw, hung = run(vv, [path], ks, budget=40)
        if hung:
            fail("%s: timed out (last status %r)" % (label, status(txt)))
        elif status(txt) != want:
            fail("%s: got %r, want %r" % (label, status(txt), want))
    return rc


if __name__ == "__main__":
    sys.exit(main())
