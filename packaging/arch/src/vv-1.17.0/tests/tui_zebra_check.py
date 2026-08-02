#!/usr/bin/env python3
"""Verify the TUI zebra stripe adapts to the terminal background.

On a dark terminal the alternating-row background is a near-black grey
(256-colour 235); on a light terminal that would swallow the default-
foreground text, so it must become a subtle light grey (254). We force the
detection via VV_BACKGROUND and check which `48;5;<n>` background the TUI emits.

Usage: tui_zebra_check.py <vv-binary> <data-file>
Exit 0 on success, 1 on failure.
"""
import os, pty, select, sys, time


def capture(vv, data, bg):
    env = dict(os.environ, TERM="xterm-256color", VV_BACKGROUND=bg)
    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe(vv, [vv, data], env)
    out = b""
    sent = False
    deadline = time.time() + 3.0
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                out += os.read(fd, 65536)
            except OSError:
                break
        if not sent and len(out) > 100:
            os.write(fd, b"q")  # quit the TUI
            sent = True
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    return out


def main():
    vv, data = sys.argv[1], sys.argv[2]
    light = capture(vv, data, "light")
    dark = capture(vv, data, "dark")
    ok = (b"48;5;254" in light and b"48;5;235" not in light and
          b"48;5;235" in dark and b"48;5;254" not in dark)
    if not ok:
        sys.stderr.write(
            "zebra check failed: "
            f"light has254={b'48;5;254' in light} has235={b'48;5;235' in light}; "
            f"dark has235={b'48;5;235' in dark} has254={b'48;5;254' in dark}\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
