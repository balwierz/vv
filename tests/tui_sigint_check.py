#!/usr/bin/env python3
"""Regression check for TUI signal handling (main.cpp:13536).

Launches `vv <file>` under a pseudo-terminal, waits until the TUI has entered
the alternate screen, sends Ctrl-C (which the tty turns into SIGINT), and
verifies that:

  1. vv restored the terminal before dying — the alternate-screen-exit
     sequence (rmcup) appears in the output, i.e. endwin() ran; and
  2. the process actually terminated via SIGINT (the handler re-raised it with
     the default disposition rather than swallowing it).

Without the signal handler the process would be killed before endwin(),
leaving the shell in raw/no-echo/alt-screen mode (no rmcup emitted).

Usage: tui_sigint_check.py <vv-binary> <file>
Exit 0 on success, 1 on failure (with a diagnostic on stderr).
"""
import os
import pty
import select
import signal
import struct
import sys
import termios
import time

SMCUP = b"\x1b[?1049h"   # enter alternate screen (xterm)
RMCUP = b"\x1b[?1049l"   # leave alternate screen — emitted by endwin()


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: tui_sigint_check.py <vv-binary> <file>", file=sys.stderr)
        return 2
    binary, path = sys.argv[1], sys.argv[2]

    pid, fd = pty.fork()
    if pid == 0:                       # child: the TUI
        os.environ["TERM"] = "xterm-256color"
        try:
            os.execvp(binary, [binary, path])
        except Exception:
            os._exit(127)

    # 40x120 window so the table view has room to draw.
    import fcntl
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

    buf = bytearray()
    # Phase 1: read until the TUI enters the alternate screen (or time out).
    deadline = time.time() + 5.0
    while SMCUP not in buf and time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d

    if SMCUP not in buf:
        print("TUI never entered the alternate screen (cannot test)",
              file=sys.stderr)
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        return 1

    # Phase 2: Ctrl-C -> SIGINT, then drain output until the process exits.
    os.write(fd, b"\x03")
    deadline = time.time() + 3.0
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d

    try:
        os.close(fd)
    except OSError:
        pass
    _, status = os.waitpid(pid, 0)

    restored = RMCUP in buf
    by_sigint = os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGINT
    if restored and by_sigint:
        return 0
    print(f"FAIL: terminal_restored={restored} killed_by_sigint={by_sigint} "
          f"(status={status:#x})", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
