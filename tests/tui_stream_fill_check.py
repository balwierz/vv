#!/usr/bin/env python3
"""The TUI fills its viewport from a stream whose batches are smaller than the
screen.

A forward-only source (FASTQ here) closes a batch early on a byte budget, so a
batch can hold fewer records than the screen has rows (long reads). The TUI
read one more batch per redraw and loaded columns for only the top and bottom
chunks, leaving the rows in between blank while the status bar claimed them
("Row 1-20/?"). It now reads on until the loaded rows reach the bottom of the
viewport and loads every chunk in between.

Check: 2000 reads, VV_FASTX_BATCH_BYTES=50 (about 5 reads per batch), a
24-row terminal: reads r1 .. r19 are all drawn. On the previous build several
are missing.

Usage: tui_stream_fill_check.py <vv-binary> <tmpdir>
Exit 0 on success, 1 on failure.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def main():
    vv, tmp = sys.argv[1], sys.argv[2]
    fq = os.path.join(tmp, "fill.fq")
    with open(fq, "w") as f:
        for i in range(2000):
            f.write(f"@r{i}\nACGT\n+\nIIII\n")
    os.environ["VV_FASTX_BATCH_BYTES"] = "50"
    txt, _raw, hung = run(vv, [fq], [], budget=10)
    drawn = {int(m) for m in re.findall(r"r(\d+)ACGT", txt)}
    missing = [i for i in range(1, 20) if i not in drawn]
    if hung or missing:
        sys.stderr.write("tui_stream_fill: reads not drawn: %r%s\n"
                         % (missing, " (hung)" if hung else ""))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
