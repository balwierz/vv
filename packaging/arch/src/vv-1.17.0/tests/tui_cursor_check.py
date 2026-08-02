#!/usr/bin/env python3
"""The TUI has a cell cursor: j/k/h/l move a highlighted cell and the per-cell
actions (S, s, y, Enter, , / .) operate on THAT cell, instead of every one of
them reading the top-left corner.

The load-bearing observation is that moving the cursor no longer drags the
viewport: on the pre-cursor build `lll` scrolled the window (`Col 4-5/5`),
now it just moves the cursor inside it (`Col 1-5/5`). That check is the one
that actually distinguishes the two builds — verified by running this harness
against both. The others are regression guards for the new state (they pass
on the old build too, because "scroll the view and act on its left edge"
happens to give the same answer as "act on the cursor" whenever the cursor
sits at the left edge, which is all the old build could do).

Screen-scraping note: ncurses repaints differentially, so the status bar
normally appears in the byte stream only once, at start-up. This sends
SIGWINCH after the keystrokes to force a full repaint, then reads the final
frame.

Usage: tui_cursor_check.py <vv-binary> <data-file> [second-data-file]
Exit 0 on success, 1 on failure.
"""
import os, select, time, fcntl, termios, struct, sys, re, base64, signal


def run(vv, args, keys, cols=100, rows=24, budget=8.0):
    """Drive the TUI under a pty; return (screen_text, raw_bytes, hung)."""
    m, s = os.openpty()
    fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2); os.close(m)
        os.execvpe(vv, [vv, "-i"] + args,
                   dict(os.environ, TERM="xterm-256color"))
    os.close(s)
    out = b""
    pending = list(keys)
    deadline = time.time() + budget
    nxt = time.time() + 0.5
    phase = "keys"
    while time.time() < deadline:
        r, _, _ = select.select([m], [], [], 0.15)
        if r:
            try:
                out += os.read(m, 65536)
            except OSError:
                break
        if len(out) <= 50:
            continue
        if pending and time.time() >= nxt:
            os.write(m, pending.pop(0))
            nxt = time.time() + 0.3
        elif not pending and phase == "keys" and time.time() >= nxt:
            # Force a full repaint so the status bar is re-emitted whole.
            fcntl.ioctl(m, termios.TIOCSWINSZ,
                        struct.pack("HHHH", rows, cols - 1, 0, 0))
            os.kill(pid, signal.SIGWINCH)
            phase = "resized"; nxt = time.time() + 0.7
        elif phase == "resized" and time.time() >= nxt:
            # Two quits: any key dismisses an open overlay first.
            os.write(m, b"q"); os.write(m, b"q")
            phase = "quitting"; nxt = time.time() + 0.5
        elif phase == "quitting" and time.time() >= nxt:
            break
    # Drain whatever is left.
    for _ in range(6):
        r, _, _ = select.select([m], [], [], 0.2)
        if not r:
            break
        try:
            d = os.read(m, 65536)
        except OSError:
            break
        if not d:
            break
        out += d
    try:
        os.close(m)
    except OSError:
        pass
    hung = False
    for _ in range(20):
        if os.waitpid(pid, os.WNOHANG)[0]:
            break
        time.sleep(0.1)
    else:
        hung = True
        try:
            os.kill(pid, 9); os.waitpid(pid, 0)
        except OSError:
            pass
    plain = re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", out).replace(b"\x1b(B", b"")
    return plain.decode("utf8", "replace"), out, hung


def main():
    vv, data = sys.argv[1], sys.argv[2]
    second = sys.argv[3] if len(sys.argv) > 3 else None
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("tui_cursor: " + msg + "\n")
        rc = 1

    # 1. THE discriminating check. tiny.parquet has 5 columns, all of which fit
    #    in 100 terminal columns. Three `l` presses move the cursor to Score;
    #    the viewport must not move, so the status bar still reads Col 1-5/5.
    #    The pre-cursor build scrolled instead and reads Col 4-5/5.
    txt, _raw, hung = run(vv, [data], [b"l", b"l", b"l"])
    if hung:
        fail("hung with lll")
    else:
        cols_seen = re.findall(r"Col (\d+)-(\d+)/(\d+)", txt)
        if not cols_seen:
            fail("no column range on the status bar")
        elif cols_seen[-1][0] != "1":
            fail("moving the cursor scrolled the viewport: Col %s-%s/%s"
                 % cols_seen[-1])

    # 2. The stats popup reports the cursor's column, not column 0.
    txt, _raw, hung = run(vv, [data], [b"l", b"l", b"l", b"S"])
    if hung:
        fail("hung with lllS")
    elif "column stats" not in txt:
        fail("stats popup did not open")
    else:
        # The popup is narrow, so the name may be truncated ("S…"). Match the
        # popup's own Column row rather than the table header behind it.
        mm = re.search(r"Column\s+(\S+)", txt[txt.rfind("column stats"):])
        if not mm or not mm.group(1).startswith("S"):
            fail("stats popup names %r, expected the cursor column Score"
                 % (mm.group(1) if mm else None))

    # 3. Yank copies the cell under the cursor.
    _txt, raw, hung = run(vv, [data], [b"l", b"y"])
    if hung:
        fail("hung with ly")
    else:
        mm = re.search(rb"\x1b\]52;c;([A-Za-z0-9+/=]+)", raw)
        if not mm:
            fail("no OSC52 payload after y")
        else:
            val = base64.b64decode(mm.group(1)).decode("utf8", "replace").strip()
            if not val.isdigit():
                fail("y copied %r; expected a Start value (the cursor column)"
                     % val)

    # 4. The hang case. A terminal too narrow for even one column makes
    #    visible_cols() take its force-emit fallback, so "is the cursor column
    #    on screen yet?" is not monotonic — ensure_cursor_visible() needs its
    #    own bound or it spins forever. Only a timeout catches that.
    _txt, _raw, hung = run(vv, [data], [b"l", b"l", b"j"], cols=6, rows=10)
    if hung:
        fail("hung at 6 columns wide (ensure_cursor_visible spun)")

    # 5. The cursor survives a tab switch (TabState round-trip).
    if second:
        txt, _raw, hung = run(vv, [data, second],
                              [b"l", b"l", b"\t", b"\t", b"S"])
        if hung:
            fail("hung on tab round-trip")
        elif "column stats" not in txt:
            fail("stats popup did not open after tab round-trip")
        else:
            mm = re.search(r"Column\s+(\S+)", txt[txt.rfind("column stats"):])
            if not mm or not mm.group(1).startswith("E"):
                fail("cursor column lost across tabs: popup names %r, want End"
                     % (mm.group(1) if mm else None))

    return rc


if __name__ == "__main__":
    sys.exit(main())
