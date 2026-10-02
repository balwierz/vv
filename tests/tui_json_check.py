#!/usr/bin/env python3
"""The JSON tree viewer: navigation, folding, search, copy, table switch.

Drives `vv -i` on a pty (tests/tui_cursor_check.py's run()) and reads the
status bar ("<jq path>  <type> ...") of the last frame:

  1. open: the root is an object with 3 keys; the file validates.
  2. j j → .items, l → into it (.items[0]), j → .items[0].id; h closes
     .items[0], h again goes to .items.
  3. 1 folds to depth 1: a collapsed preview "{…} 2 keys" appears.
  4. /carol⏎ finds a value inside a collapsed subtree: .items[2].name.
  5. y copies the value under the cursor (OSC 52, decoded string), p its path.
  6. NDJSON: a virtual array of lines; t opens the table view (id / name
     columns, data on the first frame), t returns to the tree.
  7. A 5,000-element array with VV_JSON_CHECKPOINT=16 (many index pages):
     G → .[4999].i (the last row), g, j, J (next sibling) → .[1].
  8. A truncated file: the error row and "invalid at 1:..." in the status.
  9. JSON on stdin with the pty as the controlling terminal: the tree reads
     keys from /dev/tty (j → .a). Linux only (see the note in main()).
 10. A 6-column terminal draws and quits without hanging.

Usage: tui_json_check.py <vv-binary> <tmpdir>
Exit 0 on success, 1 on failure.
"""
import base64, fcntl, json, os, re, select, signal, struct, sys, termios, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui_cursor_check import run  # noqa: E402

STATUS = re.compile(r" (\.[^\s]*)  (object|array|string|number|boolean|null|document|error)")


def status(txt):
    m = STATUS.findall(txt)
    return m[-1] if m else None


def osc52(raw):
    got = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]*)\x07", raw)
    return [base64.b64decode(g).decode("utf-8", "replace") for g in got]


def run_stdin(vv, data, keys, cols=80, rows=12, budget=6.0):
    """vv - with `data` on stdin and the pty as stdout and controlling tty."""
    m, s = os.openpty()
    fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    r_fd, w_fd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.setsid()
        fcntl.ioctl(s, termios.TIOCSCTTY, 0)
        os.dup2(r_fd, 0); os.dup2(s, 1); os.dup2(s, 2)
        os.close(m); os.close(w_fd)
        os.execvpe(vv, [vv, "-"], dict(os.environ, TERM="xterm-256color"))
    os.close(s); os.close(r_fd)
    os.write(w_fd, data); os.close(w_fd)
    out = b""
    pending = list(keys)
    deadline = time.time() + budget
    nxt = time.time() + 0.8
    phase = "keys"
    while time.time() < deadline:
        rd, _, _ = select.select([m], [], [], 0.15)
        if rd:
            try:
                d = os.read(m, 65536)
            except OSError:          # vv has exited (closed the pty)
                break
            if not d:
                break
            out += d
        if time.time() < nxt:
            continue
        try:
            if pending:
                os.write(m, pending.pop(0)); nxt = time.time() + 0.4
            elif phase == "keys":
                # Force a full repaint so the status bar is re-emitted whole.
                fcntl.ioctl(m, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols - 1, 0, 0))
                os.kill(pid, signal.SIGWINCH)
                phase = "resized"; nxt = time.time() + 0.7
            else:
                os.write(m, b"q"); nxt = deadline
        except OSError:              # vv has exited
            break
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    try:
        os.close(m)
    except OSError:
        pass
    return re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", out).decode("utf-8", "replace")


def main():
    vv, tmp = sys.argv[1], sys.argv[2]
    bad = []

    def check(name, got, want):
        if got != want:
            bad.append("%s: got %r, want %r" % (name, got, want))

    doc = os.path.join(tmp, "tree.json")
    with open(doc, "w") as f:
        json.dump({"title": "t", "items": [{"id": 1, "name": "alice"}, {"id": 2, "name": "bob"},
                                           {"id": 3, "name": "carol"}], "n": 3}, f)

    txt, raw, hung = run(vv, [doc], [])
    check("open", status(txt), (".", "object"))
    if "valid" not in txt:
        bad.append("open: no validation state in the status bar")
    # Strings are drawn in the string colour, not in an uninitialised pair
    # (setaf 0 + setab 0: black on black) — the wide library reports 65536
    # pairs, more than COLOR_PAIR() can address.
    m = re.search(rb'((?:\x1b\[[0-9;]*m)+)"alice"', raw)
    if not m:
        bad.append("string colour: no SGR before a string value")
    elif b"[30m" in m.group(1) or b"[40m" in m.group(1):
        bad.append("string colour: drawn black on black (%r)" % m.group(1))
    # A small file opens fully expanded: l on an open node moves into it,
    # h on an open node closes it, h again goes to the parent.
    txt, raw, hung = run(vv, [doc], [b"j", b"j", b"l", b"j"])
    check("navigate", status(txt), (".items[0].id", "number"))
    txt, raw, hung = run(vv, [doc], [b"j", b"j", b"l", b"h", b"h"])
    check("h to parent", status(txt), (".items", "array"))
    txt, raw, hung = run(vv, [doc], [b"1"])
    # "…" or, outside a UTF-8 locale, "..."; ncurses may send the spaces
    # between the parts as cursor moves, which the scrape drops.
    if not re.search(r"[\[{](…|\.\.\.)[\]}]\s*\d+\s*(keys|items)", txt):
        bad.append("fold: no collapsed preview after 1; screen tail: %r" % txt[-700:])
    txt, raw, hung = run(vv, [doc], [b"1", b"/carol\r"])
    check("search", status(txt), (".items[2].name", "string"))
    txt, raw, hung = run(vv, [doc], [b"/carol\r", b"y", b"p"])
    check("copy", osc52(raw)[-2:], ["carol", ".items[2].name"])
    # T picks a theme and saves it to the config file.
    cfg = os.path.join(tmp, "themecfg")
    os.makedirs(cfg, exist_ok=True)
    old_cfg = os.environ.get("XDG_CONFIG_HOME")
    os.environ["XDG_CONFIG_HOME"] = cfg
    txt, raw, hung = run(vv, [doc], [b"T", b"j", b"\r"])
    if old_cfg is None:
        del os.environ["XDG_CONFIG_HOME"]
    else:
        os.environ["XDG_CONFIG_HOME"] = old_cfg
    try:
        saved = open(os.path.join(cfg, "vv", "config")).read()
    except OSError:
        saved = ""
    if "theme = dark" not in saved or "theme: dark" not in txt:
        bad.append("theme picker: T j Enter did not select and save 'dark' (%r)" % saved)

    # ":" goes to a path or a line; a bad path says why.
    txt, raw, hung = run(vv, [doc], [b"1", b":.items[2].name\r"])
    check("goto path", status(txt), (".items[2].name", "string"))
    txt, raw, hung = run(vv, [doc], [b':.items[7]\r'])
    if "has 3 items; no [7]" not in txt:
        bad.append("goto path: no message for a missing index")

    nd = os.path.join(tmp, "tree.ndjson")
    with open(nd, "w") as f:
        for i, n in enumerate(["alice", "bob", "carol"]):
            f.write(json.dumps({"id": i, "name": n}) + "\n")
    txt, raw, hung = run(vv, [nd], [])
    check("ndjson", status(txt), (".", "document"))
    txt, raw, hung = run(vv, [nd], [b"t"])
    if not re.search(r"id\s+name", txt) or "alice" not in txt:
        bad.append("table view: no id / name columns with data after t")
    txt, raw, hung = run(vv, [nd], [b"t", b"t", b"j"])
    check("back to tree", status(txt), (".[0]", "object"))

    big = os.path.join(tmp, "tree_big.json")
    with open(big, "w") as f:
        json.dump([{"i": i} for i in range(5000)], f)
    os.environ["VV_JSON_CHECKPOINT"] = "16"
    txt, raw, hung = run(vv, [big], [b"G"])
    check("G across pages", status(txt), (".[4999].i", "number"))
    txt, raw, hung = run(vv, [big], [b"G", b"g", b"j", b"J"])
    check("g, J", status(txt), (".[1]", "object"))
    del os.environ["VV_JSON_CHECKPOINT"]

    tr = os.path.join(tmp, "tree_trunc.json")
    with open(tr, "w") as f:
        f.write('[{"a":1},{"a":')
    txt, raw, hung = run(vv, [tr], [])
    if "unexpected end of input" not in txt or "invalid at 1:" not in txt or hung:
        bad.append("truncated: no error row / status (hung=%s)" % hung)
    # "!" goes to the first error.
    bt = os.path.join(tmp, "tree_badtok.json")
    with open(bt, "w") as f:
        f.write('{"a": [1, 2, {"b": tru}], "c": 3}')
    txt, raw, hung = run(vv, [bt], [b"!"])
    check("goto error", status(txt), (".a[2].b", "boolean"))

    # macOS: not checked (reported only) — on the CI runner the child had
    # closed the pty before the first key; not reproducible on Linux.
    out = run_stdin(vv, b'{"a":1,"b":[true]}', [b"j"])
    if status(out) != (".a", "number"):
        sys.stderr.write("tui_json: stdin output tail: %r\n" % out[-400:])
        if sys.platform != "darwin":
            check("stdin", status(out), (".a", "number"))

    txt, raw, hung = run(vv, [doc], [b"j"], cols=6, rows=8, budget=5)
    if hung:
        bad.append("narrow terminal: hung")

    for b in bad:
        sys.stderr.write("tui_json: %s\n" % b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
