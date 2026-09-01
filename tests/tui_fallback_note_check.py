#!/usr/bin/env python3
"""When stdout/stdin are a TTY, vv auto-launches the ncurses viewer. If that
viewer cannot initialise (an unusable TERM / missing terminfo) and the user did
not ask for it with -i, vv falls back to non-interactive output. It used to do
so silently, so the user saw a static table with no hint the interactive viewer
was even attempted.

This runs vv on a pty (so auto-launch is chosen) with a bogus TERM so newterm()
fails, and stderr on a separate pipe. The fallback note must appear on stderr,
and the data must still render on stdout.

Usage: tui_fallback_note_check.py <vv-binary> <data-file> <expect-substring>
Exit 0 on success, 1 on failure.
"""
import os, sys


def main():
    vv, data, expect = sys.argv[1], sys.argv[2], sys.argv[3]
    mo, ms = os.openpty()     # pty master/slave for stdin+stdout
    er, ew = os.pipe()        # separate pipe for stderr
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(ms, 0); os.dup2(ms, 1); os.dup2(ew, 2)
        os.close(mo); os.close(er)
        os.environ["TERM"] = "vv-nonexistent-terminal-xyz"
        os.execvp(vv, [vv, data])
        os._exit(127)
    os.close(ms); os.close(ew)

    out = b""
    try:
        while True:
            d = os.read(mo, 65536)
            if not d:
                break
            out += d
    except OSError:
        pass
    err = b""
    while True:
        d = os.read(er, 65536)
        if not d:
            break
        err += d
    os.waitpid(pid, 0)

    rc = 0
    if b"interactive viewer unavailable" not in err:
        sys.stderr.write("expected the fallback note on stderr; got: "
                         + err.decode(errors="replace") + "\n")
        rc = 1
    if expect.encode() not in out:
        sys.stderr.write("expected the data (%r) on stdout after the fallback\n"
                         % expect)
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
