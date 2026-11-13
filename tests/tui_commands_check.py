#!/usr/bin/env python3
"""The table viewer's view commands and motions.

Checks, on a 300-row file (id r0..r299, name n0..n299, score = id):
  1. `&score > 250`, sort score descending, hide `name` (the picker opens
     on the cursor's column), `:w out.tsv`: the
     file holds what `vv --filter --sort --select --tsv` writes;
  2. `:w` onto an existing file is refused; `:w!` replaces it;
  3. `Y` copies the vv command of that view;
  4. `:region chr1:100-200` on a sorted BED re-reads that window;
  5. `12j y` copies row 12's cell; `3l` moves three columns; `100G` goes to
     row 100; Ctrl-D moves half a page;
  6. `&` then Up recalls the filter entered before;
  7. `Z` on a 30-column file keeps the columns through the cursor at the
     left when the view scrolls right; `Z` again unfreezes them;
  8. after `--coords NCBI`, `:region chr1:95-125` is 1-based (UCSC 94-125);
  9. `50%` goes to row 149 of 300 (vim's (N*rows+99)/100);
 10. `*` on g1 finds the next whole-cell g1 (row 13), not g10 / g11;
 11. `>` moves the cursor's column right; `Y` then writes the new order;
 12. Enter, `J J K`, Enter: the cursor ends on row 1;
 13. `:w` to a .parquet file writes the view's vv.* keys.

Usage: tui_commands_check.py <vv-binary> <scratch-dir>
Exit 0 on success, 1 on failure.
"""
import base64, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402


def keys(s):
    return [s.encode()]


def main():
    vv, scratch = os.path.abspath(sys.argv[1]), sys.argv[2]
    os.chdir(scratch)              # short names: messages fit the screen
    path = "cmds.tsv"
    with open(path, "w") as f:
        f.write("id\tname\tscore\n")
        for i in range(300):
            f.write("r%d\tn%d\t%d\n" % (i, i, i))
    bed = "cmds.bed"
    with open(bed, "w") as f:
        for i in range(50):
            f.write("chr1\t%d\t%d\tp%d\n" % (i * 10, i * 10 + 5, i))
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_commands: " + msg + "\n")
        rc = 1

    def yanked(raw):
        mm = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]+)", raw)
        return base64.b64decode(mm[-1]).decode("utf8", "replace").strip() if mm else None

    view = keys("&score > 250\r") + keys("ll") + keys("ss") + keys("c") + keys("k") + keys(" ") + keys("q")
    out = "cmds_out.tsv"
    if os.path.exists(out):
        os.remove(out)

    # 1. :w writes the view.
    _txt, _raw, hung = run(vv, [path], budget=25.0, keys=view + keys(":w " + out + "\r"))
    want = subprocess.run([vv, "--filter", "score > 250", "--sort", "score:desc", "--select", "id,score",
                           "--tsv", path], capture_output=True, text=True).stdout
    got = open(out).read() if os.path.exists(out) else None
    if hung or got != want:
        fail(":w wrote %r..., expected %r..." % ((got or "")[:60], want[:60]))

    # 2. An existing file is kept unless :w! is used.
    with open(out, "w") as f:
        f.write("keep\n")
    txt, _raw, hung = run(vv, [path], budget=20.0, keys=keys(":w " + out + "\r"))
    if open(out).read() != "keep\n" or "exists; :w!" not in txt:
        fail(":w onto an existing file did not refuse (screen tail: %r)" % txt[-200:])
    _txt, _raw, hung = run(vv, [path], budget=20.0, keys=keys(":w! " + out + "\r"))
    if not open(out).read().startswith("id\tname\tscore\nr0\t"):
        fail(":w! did not replace the file")

    # 3. Y copies the view's vv command.
    _txt, raw, hung = run(vv, [path], budget=25.0, keys=view + [b"Y"])
    cmd = yanked(raw)
    if hung or cmd != "vv --filter 'score > 250' --sort score:desc --select id,score " + path:
        fail("Y copied %r" % cmd)

    # 4. :region re-reads a window of an indexed-or-scanned BED.
    _txt, raw, hung = run(vv, [bed], budget=20.0, keys=keys(":region chr1:100-125\r") + [b"y"])
    if hung or yanked(raw) != "chr1":
        fail(":region did not open the window (copied %r)" % yanked(raw))
    txt, raw, hung = run(vv, [bed], budget=20.0, keys=keys(":region chr1:100-125\r") + keys("ll") + [b"y"])
    if hung or yanked(raw) != "105":
        fail(":region's first row is not chr1:100-105 (copied %r)" % yanked(raw))

    # 5. Counts and half pages.
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys("12j") + [b"y"])
    if hung or yanked(raw) != "r12":
        fail("12j y copied %r, expected r12" % yanked(raw))
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys("2l") + [b"y"])
    if hung or yanked(raw) != "0":
        fail("2l y copied %r, expected 0 (the score column)" % yanked(raw))
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys("100G") + [b"y"])
    if hung or yanked(raw) != "r100":
        fail("100G y copied %r, expected r100" % yanked(raw))
    _txt, raw, hung = run(vv, [path], budget=20.0, rows=24, keys=[b"\x04"] + [b"y"])
    got = yanked(raw)
    if hung or got is None or not got.startswith("r") or not (5 <= int(got[1:]) <= 12):
        fail("Ctrl-D y copied %r, expected about half a page down" % got)

    # 6. Up in the & bar recalls the filter entered before.
    txt, _raw, hung = run(vv, [path], budget=20.0,
                          keys=keys("&score > 290\r") + keys("&") + [b"\x1b[A"] + keys("\r"))
    if hung or "score > 290" not in txt or "9/9" not in txt.replace("_", ""):
        fail("Up in the & bar did not bring back the filter (screen tail: %r)" % txt[-200:])

    # 7. Z freezes the columns through the cursor.
    wide = "cmds_wide.tsv"
    with open(wide, "w") as f:
        f.write("\t".join("k%02d" % c for c in range(30)) + "\n")
        for r in range(20):
            f.write("\t".join("v%02d_%02d" % (c, r) for c in range(30)) + "\n")
    txt, raw, hung = run(vv, [wide], budget=20.0, keys=keys("lZ") + keys("25l") + [b"y"])
    # The screen text holds every frame; row 19 of the last one sits near the end.
    pinned = re.findall(r"v00_19\s+v01_19\s+v(\d\d)_19", txt[-500:])
    if hung or yanked(raw) != "v26_00" or not pinned or int(pinned[-1]) < 10:
        fail("Z did not keep k00 and k01 at the left (copied %r, screen tail %r)" % (yanked(raw), txt[-300:]))
    txt, raw, hung = run(vv, [wide], budget=20.0, keys=keys("lZZ") + keys("25l") + [b"y"])
    if hung or yanked(raw) != "v26_00" or "v00_19" in txt[-500:] or "v26_19" not in txt[-500:]:
        fail("Z twice did not unfreeze (screen tail %r)" % txt[-300:])
    txt, raw, hung = run(vv, [wide], budget=20.0, keys=keys("lZ") + keys("25l") + keys("25h") + [b"y"])
    if hung or yanked(raw) != "v01_00":
        fail("h back onto a frozen column copied %r, expected v01_00" % yanked(raw))

    # 8. :region in the convention --coords gave.
    _txt, raw, hung = run(vv, ["--coords", "NCBI", bed], budget=20.0,
                          keys=keys(":region chr1:95-125\r") + keys("ll") + [b"y"])
    if hung or yanked(raw) != "95":
        fail(":region after --coords NCBI: first row ends at %r, expected 95 (row 90-95)" % yanked(raw))

    # 9. N% goes to that share of the rows.
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys("50%") + [b"y"])
    if hung or yanked(raw) != "r149":
        fail("50%% y copied %r, expected r149" % yanked(raw))

    # 10. * searches the whole cell.
    grp = "cmds_grp.tsv"
    with open(grp, "w") as f:
        f.write("id\tgrp\n")
        for i in range(40):
            f.write("r%d\tg%d\n" % (i, i % 12))
    _txt, raw, hung = run(vv, [grp], budget=20.0, keys=keys("jl*") + keys("h") + [b"y"])
    if hung or yanked(raw) != "r13":
        fail("* on g1 landed on %r, expected r13" % yanked(raw))

    # 11. > moves a column; Y keeps the order.
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys(">h") + [b"y"])
    if hung or yanked(raw) != "n0":
        fail("> then h y copied %r, expected n0 (name now first)" % yanked(raw))
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys(">") + [b"Y"])
    if hung or yanked(raw) != "vv --select name,id,score " + path:
        fail("Y after > copied %r" % yanked(raw))
    _txt, raw, hung = run(vv, ["--select", "id,score", path], budget=20.0, keys=[b"Y"])
    if hung or yanked(raw) != "vv --select id,score " + path:
        fail("Y after --select id,score copied %r" % yanked(raw))

    # 12. J / K step records in the detail pane.
    _txt, raw, hung = run(vv, [path], budget=20.0, keys=keys("\r") + keys("J") + keys("J") + keys("K") + keys("\r") + [b"y"])
    if hung or yanked(raw) != "r1":
        fail("Enter J J K Enter y copied %r, expected r1" % yanked(raw))

    # 13. :w FILE.parquet carries the view's vv.* keys.
    pq = "cmds_out.parquet"
    if os.path.exists(pq):
        os.remove(pq)
    _txt, _raw, hung = run(vv, [path], budget=25.0, keys=view + keys(":w " + pq + "\r"))
    info = subprocess.run([vv, "--info", pq], capture_output=True, text=True).stdout
    for want_kv in ("vv.source: " + path, "vv.filter: score > 250", "vv.sort: score:desc",
                    "vv.select: id,score"):
        if want_kv not in info:
            fail(":w to Parquet: %r missing from --info (%r)" % (want_kv, info[-300:]))

    if rc == 0:
        print("tui_commands: 13 checks passed")
    return rc


if __name__ == "__main__":
    sys.exit(main())
