#!/usr/bin/env python3
"""--heatmap's auto backend follows the terminal.

Runs `vv --heatmap` on a pty with the variables each terminal sets and checks
which escape the output carries: iTerm2's OSC 1337 File= for WezTerm
(TERM_PROGRAM=WezTerm) and iTerm2 (TERM_PROGRAM=iTerm.app, or LC_TERMINAL=iTerm2
over ssh), kitty's APC G for kitty (KITTY_WINDOW_ID), half-block text
otherwise — and half-blocks inside tmux, which drops image escapes.

Usage: image_auto_check.py <vv-binary> <data-file>
Exit 0 on success, 1 on failure.
"""
import os, pty, select, sys

CASES = [
    ({"TERM_PROGRAM": "WezTerm"}, "iterm"),
    ({"TERM_PROGRAM": "iTerm.app"}, "iterm"),
    ({"LC_TERMINAL": "iTerm2"}, "iterm"),
    ({"KITTY_WINDOW_ID": "1"}, "kitty"),
    ({}, "halfblock"),
    ({"TERM_PROGRAM": "iTerm.app", "TMUX": "/tmp/tmux-1/default,1,0"}, "halfblock"),
]
STRIP = ("TERM_PROGRAM", "LC_TERMINAL", "KITTY_WINDOW_ID", "TMUX", "STY")


def backend(vv, data, env):
    pid, fd = pty.fork()
    if pid == 0:
        e = {k: v for k, v in os.environ.items() if k not in STRIP}
        e.update(env)
        e["TERM"] = "xterm-256color"
        os.execve(vv, [vv, "--heatmap", data], e)
    out = b""
    while True:
        try:
            r, _, _ = select.select([fd], [], [], 10)
            if not r:
                break
            d = os.read(fd, 1 << 16)
            if not d:
                break
            out += d
        except OSError:
            break
    os.waitpid(pid, 0)
    if b"\x1b]1337;File=" in out:
        return "iterm"
    if b"\x1b_G" in out:
        return "kitty"
    if "▀".encode() in out:
        return "halfblock"
    return "none"


def main():
    vv, data = sys.argv[1], sys.argv[2]
    rc = 0
    for env, want in CASES:
        got = backend(vv, data, env)
        if got != want:
            sys.stderr.write("image_auto: %r -> %s, want %s\n" % (env, got, want))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
