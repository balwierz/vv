#!/usr/bin/env python3
"""Empty mapped views and whole-stream TUI operations, including known counts.

SQLite reports COUNT(*) before its forward-only SELECT reaches EOF. The fixture
spans multiple batches, so a loaded-prefix filter/search/sort/stats pass cannot
produce the expected answer. Also exercise zero-match views through filtering,
navigation, sorting, clearing, and tab snapshot restoration.

Usage: tui_stream_pass_check.py <vv-binary> <scratch-dir>
"""
import os
import re
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def final_frame(raw):
    # The shared driver resizes before quitting, forcing a complete repaint.
    # Inspect only that frame: earlier unfiltered rows must not mask a failure.
    frame = raw.rsplit(b"\x1b[2J", 1)[-1]
    frame = re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", frame)
    return frame.replace(b"\x1b(B", b"").decode("utf8", "replace")


def main():
    vv, scratch = sys.argv[1:3]
    path = os.path.join(scratch, "tui_stream_pass.sqlite")
    with sqlite3.connect(path) as db:
        db.executescript("DROP TABLE IF EXISTS a; DROP TABLE IF EXISTS b;"
                         "CREATE TABLE a(id INTEGER, name TEXT);"
                         "CREATE TABLE b(id INTEGER, name TEXT);")
        db.executemany("INSERT INTO a VALUES (?, ?)",
                       ((i, "needle-8500" if i == 8500 else "item-%d" % i)
                        for i in range(9000)))
        db.executemany("INSERT INTO b VALUES (?, ?)",
                       ((i, "other-tab-%d" % i) for i in range(3)))
    failed = []
    checks = 0

    def check(label, args, keys, predicate, data=path, whole=False):
        nonlocal checks
        checks += 1
        txt, raw, hung = run(vv, ["-w", "30"] + args + [data], keys, cols=180, budget=20)
        frame = txt if whole else final_frame(raw)
        if hung or not predicate(frame):
            failed.append(label)
            sys.stderr.write("tui_stream_pass: %s%s: %r\n" %
                             (label, " (hung)" if hung else "", frame[-1200:]))

    def empty(frame):
        return ("Row 0-0/0" in frame and "filter:id < 0" in frame
                and not re.search(r"(?:item-|needle-|other-tab-)\d+", frame))

    check("empty startup filter", ["--filter", "id < 0"], [], empty)
    check("empty live filter", [], [b"&id < 0\r"], empty)
    check("empty navigation and search", ["--filter", "id < 0"],
          [b"jG", b"/needle\r", b"nN", b"\r", b"\x1b", b"y"], empty)
    check("empty sort and undo", ["--filter", "id < 0"], [b"s", b"u"], empty)
    check("empty tab snapshot", ["--filter", "id < 0"], [b"\t", b"\t"], empty)
    check("new tab has identity view", ["--filter", "id < 0"], [b"\t"],
          lambda f: "Row 1-3/3" in f and "other-tab-0" in f and "filter:" not in f)
    check("clear empty filter", ["--filter", "id < 0"], [b"&\x15\r"],
          lambda f: re.search(r"Row 1-[\d_]+/9_000", f)
          and "item-0" in f and "filter:" not in f)
    check("clear empty filter keeps sort", ["--filter", "id < 0",
                                           "--sort", "id:desc"], [b"\x1b"],
          lambda f: "sort:id" in f and "item-8999" in f and "filter:" not in f)

    check("filter reaches later batches", ["--filter", "id < 5000"], [],
          lambda f: "/5_000" in f and "5_000/9_000" in f and "[PARTIAL]" not in f)
    check("deep jump extends known-count stream", [], [b":8500\r"],
          lambda f: re.search(r"8_500\s+8_500\s+needle", f))
    check("scrolling extends known-count stream", [], [b":4080\r", b"j" * 20],
          lambda f: re.search(r"4_100\s+4_100\s+item", f))
    check("end extends known-count stream", [], [b"G"],
          lambda f: re.search(r"8_999\s+8_999\s+item", f) and "/9_000" in f)
    check("forward search reaches later batches", [], [b"/needle-8500\r"],
          lambda f: "needle-8500" in f and "(not found)" not in f)
    check("backward search wraps over later batches", [], [b"?needle-8500\r"],
          lambda f: "needle-8500" in f and "(wrapped)" in f)
    check("search repeat wraps", [], [b"/needle-8500\r", b"n"],
          lambda f: "needle-8500" in f and "(wrapped)" in f)
    check("sort covers every batch", ["--sort", "id:desc"], [],
          lambda f: "item-8999" in f and "/9_000" in f)
    check("stats cover every batch", [], [b"S"],
          lambda f: re.search(r"Count\s+9_000", f) and re.search(r"Max\s*8999", f),
          whole=True)
    check("value counts cover every batch", [], [b"F"],
          lambda f: "9_000 rows" in f and "9_000 values" in f)

    fastq = os.path.join(scratch, "tui_stream_error.fq")
    with open(fastq, "w") as out:
        for i in range(9000):
            out.write("@read-%d\nACGT\n+\nIIII\n" % i)
        out.write("@broken\nACGT\n+\nI\n")
    check("early search does not drain the source", [], [b"/^read-0$\r"],
          lambda f: "read-0" in f and "/?" in f and "[READ ERROR:" not in f,
          data=fastq)
    check("late read failure keeps useful rows", [], [b"G", b"g"],
          lambda f: "[READ ERROR:" in f and "read-0" in f, data=fastq)

    old_cap = os.environ.get("VV_STREAM_BATCH_CAP")
    os.environ["VV_STREAM_BATCH_CAP"] = "1"
    try:
        # Searching for the first row briefly pins retention, but must not leave
        # the stream pinned while a subsequent G advances through later batches.
        check("search restores bounded retention", [], [b"/^read-0$\r", b"G", b"s"],
              lambda f: "[PARTIAL]" in f, data=fastq)
    finally:
        if old_cap is None:
            os.environ.pop("VV_STREAM_BATCH_CAP", None)
        else:
            os.environ["VV_STREAM_BATCH_CAP"] = old_cap

    try:
        import numpy as np
    except ImportError:
        print("tui_stream_pass: capped-preview check skipped (numpy unavailable)")
    else:
        capped = os.path.join(scratch, "tui_stream_capped.npy")
        np.save(capped, np.zeros((1, 4097), dtype=np.int8))
        check("capped scope is visible", [], [],
              lambda f: "[CAPPED cols 4_096/4_097]" in f, data=capped)

    if failed:
        sys.stderr.write("tui_stream_pass: %d checks failed\n" % len(failed))
        return 1
    print("tui_stream_pass: %d checks passed" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
