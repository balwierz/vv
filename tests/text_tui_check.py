#!/usr/bin/env python3
"""A text file opens in vv's own TUI as a document, not as a one-column table.

The load-bearing checks are the two that a naive "just let TextSource fall
through to the table renderer" implementation fails:

  * a long line is CHOPPED at the screen edge, not ellipsised at 32 columns
    (cell_at truncates every string cell at max_col_w_, default 32, before the
    renderer ever sees it — a text viewer that did that would be useless);
  * there is no `line` / `string` column header eating three of the rows.

Both were verified to fail against a build with the text branch removed from
draw_data_row. The rest are regression guards for the new state.

Screen-scraping note: ncurses repaints differentially, so a frame normally
reaches the byte stream only once. This sends SIGWINCH after the keystrokes to
force a full repaint, then reads the final frame. (Same trick as
tui_cursor_check.py — without it the regex only ever sees the start-up frame.)

Usage: text_tui_check.py <vv-binary> <tmpdir>
Exit 0 on success, 1 on failure.
"""
import os, select, time, fcntl, termios, struct, sys, re, signal


def run(vv, args, keys, cols=60, rows=14, budget=14.0):
    """Drive the TUI under a pty; return (whole, last_frame, raw, hung)."""
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
            fcntl.ioctl(m, termios.TIOCSWINSZ,
                        struct.pack("HHHH", rows, cols - 1, 0, 0))
            os.kill(pid, signal.SIGWINCH)
            phase = "resized"; nxt = time.time() + 1.5
        elif phase == "resized" and time.time() >= nxt:
            os.write(m, b"q"); os.write(m, b"q")
            phase = "quitting"; nxt = time.time() + 0.5
        elif phase == "quitting" and time.time() >= nxt:
            break
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
    strip = lambda b: re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", b) \
                        .replace(b"\x1b(B", b"").decode("utf8", "replace")
    # Two views of the session. `whole` is every frame it drew, which is the
    # right thing to assert on whenever a key only ADDS something to the
    # screen (a marker scrolled into view, a status message, a tab bar) or
    # when nothing may ever appear (an escape payload) — a hit anywhere is a
    # hit, and a miss anywhere is a miss.
    #
    # `last` keeps only what follows the final full-screen erase, i.e. the
    # repaint the SIGWINCH above forced. It is needed for exactly one kind of
    # question: did a key UNDO a visible change? `0` scrolling back to column
    # 0 passes against `whole` either way, because the scrolled frame is still
    # in the buffer. It is also the fragile view — under a loaded CI runner
    # the forced repaint can land after we stop reading — so it is used only
    # where `whole` cannot answer.
    return strip(out), strip(out.rsplit(b"\x1b[2J", 1)[-1]), out, hung


def main():
    vv, tmp = sys.argv[1], sys.argv[2]
    rc = 0

    def fail(msg):
        nonlocal rc
        sys.stderr.write("text_tui: " + msg + "\n")
        rc = 1

    # A line far wider than the 60-column terminal, with a marker at column 90
    # that only becomes visible after scrolling right.
    wide = os.path.join(tmp, "wide.txt")
    with open(wide, "w") as f:
        f.write("A" * 88 + "MARKER_AT_90" + "B" * 40 + "\n")
        for i in range(2, 12):
            f.write("line %d\n" % i)
    small = os.path.join(tmp, "small.txt")
    with open(small, "w") as f:
        f.write("alpha\nbeta\n")

    # 1. No column header. `line` / `string` / the box rule would eat three
    #    rows and tell the user nothing.
    txt, _last, _, hung = run(vv, [wide], [])
    if hung:
        fail("hung opening a text file")
    else:
        if re.search(r"\bline\b\s+\bstring\b", txt):
            fail("a `line` column header is being drawn over a text file")
        if "string" in txt:
            fail("a `string` type row is being drawn over a text file")

        # 2. THE discriminating check: the long line is chopped at the screen
        #    edge, not ellipsised at max_col_w_ (32). A truncating renderer
        #    emits the … and stops around 32 columns.
        if "…" in txt:
            fail("the long line was ellipsised; a text line must be chopped")
        runs = re.findall(r"A{20,}", txt)
        if not runs:
            fail("no long run of A found on screen at all")
        elif max(len(r) for r in runs) < 45:
            fail("longest visible A-run is %d columns; the line was truncated "
                 "before reaching the screen edge" % max(len(r) for r in runs))

        # 3. Line numbers are 1-based, like less -N / grep -n, and match the
        #    status bar's own "Line 1-..." range. NB the scrape has had its
        #    cursor-positioning stripped, so rows run together — anchor on the
        #    distinctive "1 AAAA…" rather than on a line start.
        if not re.search(r"1 A{40,}", txt):
            fail("no 1-based line number in the gutter")
        if not re.search(r"Line 1-", txt):
            fail("status bar does not show a Line range")

    # 4. `l` scrolls sideways: a marker past the right edge becomes visible.
    txt, _last, _, hung = run(vv, [wide], [b"l", b"l", b"l"])
    if hung:
        fail("hung after l")
    elif "MARKER_AT_90" not in txt:
        fail("l did not scroll the line sideways (marker at column 90 never "
             "appeared)")

    # ...and `0` goes back home.
    _txt, last, _, hung = run(vv, [wide], [b"l", b"l", b"l", b"0"])
    if hung:
        fail("hung after l0")
    elif "MARKER_AT_90" in last:
        fail("0 did not return the line to column 0")

    # 5. Two text files open as tabs; one file shows no tab bar.
    txt, _last, _, hung = run(vv, [wide, small], [])
    if hung:
        fail("hung with two text files")
    else:
        if "wide.txt" not in txt or "small.txt" not in txt:
            fail("both tab labels should appear on the tab bar; got neither")
        if "[Tab] next" not in txt:
            fail("no tab bar with two files open")
    txt, _last, _, hung = run(vv, [small], [])
    if hung:
        fail("hung with one text file")
    elif "[Tab] next" in txt:
        fail("a tab bar was drawn for a single file")

    # 6. The column-oriented keys say why rather than doing nothing.
    txt, _last, _, hung = run(vv, [small], [b"s"])
    if hung:
        fail("hung after s")
    elif "not available for a text file" not in txt:
        fail("s should report why sorting does not apply to a text file")

    # 7. An OSC title escape in the file must not reach the terminal, and its
    #    tail must not show up as literal garbage either. Mirrors
    #    md_no_osc_title_injection.
    eviltxt = os.path.join(tmp, "evil.log")
    with open(eviltxt, "w") as f:
        f.write("BEFORE\x1b]0;PWNED\x07\x1b[31mred\x1b[0m AFTER\n")
    txt, _last, raw, hung = run(vv, [eviltxt], [])
    if hung:
        fail("hung on the escape fixture")
    else:
        if b"PWNED" in raw:
            fail("an OSC title payload reached the terminal")
        if "]0;" in txt or "[31m" in txt:
            fail("an escape sequence tail was painted as literal text")
        if "BEFORE" not in txt or "AFTER" not in txt:
            fail("the printable text around the escapes was lost")

    return rc


if __name__ == "__main__":
    sys.exit(main())
