#!/usr/bin/env python3
"""A single column wider than the terminal must still render (clamped), not a
blank table. Drives the TUI under a pty sized narrower than the column and
asserts the frame carries the column's header/data.

Usage: tui_wide_column_check.py <vv-binary> <data-file>
Exit 0 on success (non-blank frame), 1 on failure.
"""
import os, select, time, fcntl, termios, struct, sys, re


def run(vv, data, cols=20, rows=24):
    m, s = os.openpty()
    fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2); os.close(m)
        os.execvpe(vv, [vv, "-i", data], dict(os.environ, TERM="xterm-256color"))
    os.close(s)
    out = b""
    sent = False
    deadline = time.time() + 4.0
    while time.time() < deadline:
        r, _, _ = select.select([m], [], [], 0.2)
        if r:
            try:
                out += os.read(m, 65536)
            except OSError:
                break
        if not sent and len(out) > 50:
            os.write(m, b"q")  # quit the TUI
            sent = True
    try:
        os.close(m)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    return out


def main():
    vv, data = sys.argv[1], sys.argv[2]
    raw = run(vv, data)
    # Strip ANSI/terminfo escapes, then look for any of the column's content.
    txt = re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", raw).replace(b"\x1b(B", b"")
    ok = (b"this_is" in txt) or (b"vA" in txt) or (b"vB" in txt)
    if not ok:
        sys.stderr.write("wide-column frame was blank (no column content)\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
