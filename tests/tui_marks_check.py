#!/usr/bin/env python3
"""Marks: `m{a-z}` marks the cursor's row and column, `'{a-z}` jumps back to
it, `''` returns to where the last jump came from; a mark stays on its row
through a sort, and a row the filter hides is reported, not jumped to.

Checks, on a 300-row file (id r0..r299, score = id):
  1. `:120` `ma` `:10` `'a` `y` copies "r120" (the marked row and column);
  2. ... then `''` `y` copies "r10";
  3. `:120` `ma`, sort by score descending (`l l s s`), `'a` `y` copies
     "r120": the mark followed its row;
  4. `ma` on row 120, filter `score < 50`, `'a`: the status line says the
     row is hidden by the filter.

Usage: tui_marks_check.py <vv-binary> <scratch-dir>
Exit 0 on success, 1 on failure.
"""
import base64, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def keys(s):
    # One write per command: the harness waits 0.3 s between writes, and a
    # slow runner (macOS CI) ran out of its budget key by key.
    return [s.encode()]


def main():
    vv, scratch = sys.argv[1], sys.argv[2]
    path = os.path.join(scratch, "marks.tsv")
    with open(path, "w") as f:
        f.write("id\tname\tscore\n")
        for i in range(300):
            f.write("r%d\tn%d\t%d\n" % (i, i, i))
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_marks: " + msg + "\n")
        rc = 1

    def yanked(raw):
        mm = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]+)", raw)
        return base64.b64decode(mm[-1]).decode("utf8", "replace").strip() if mm else None

    # 1. Mark, move away, jump back.
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys(":120\r") + keys("ma") + keys(":10\r") + keys("'a") + [b"y"])
    if hung or yanked(raw) != "r120":
        fail("'a after ma on row 120 copied %r, expected r120" % yanked(raw))

    # 2. '' returns to where that jump came from.
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys(":120\r") + keys("ma") + keys(":10\r") + keys("'a")
                                      + keys("''") + [b"y"])
    if hung or yanked(raw) != "r10":
        fail("'' after the jump copied %r, expected r10" % yanked(raw))

    # 3. The mark follows its row through a sort (descending by score).
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys(":120\r") + keys("ma") + keys("ll") + keys("ss")
                                      + keys("'a") + [b"y"])
    if hung or yanked(raw) != "r120":
        fail("'a after a descending sort copied %r, expected r120" % yanked(raw))

    # 4. A row the filter hides is reported.
    txt, _raw, hung = run(vv, [path], budget=20.0, keys=keys(":120\r") + keys("ma") + keys("&score < 50\r") + keys("'a"))
    if hung or "hidden by the filter" not in txt:
        fail("'a to a filtered-out row did not report it (screen tail: %r)" % txt[-200:])

    if rc == 0:
        print("tui_marks: 4 checks passed")
    return rc


if __name__ == "__main__":
    sys.exit(main())
