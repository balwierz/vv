#!/usr/bin/env python3
"""Forward-only streaming sources keep only a bounded window of decoded
batches. A full-file pass (sort / filter / search / column stats) that runs
after batches were released can only see part of the file — and used to
present that partial answer as if it were complete.

This drives the TUI over a multi-batch input with VV_STREAM_BATCH_CAP=1, jumps
to the end (G) so the early batches are released, then triggers a sort (s).
The status bar must carry the [PARTIAL] marker.

Also checks the negative: with the default retention window nothing is
released, so the marker must NOT appear.

The data file must span several batches. A FASTQ is the cheap way to get
there (FastxSource batches every 4096 records); a delimited file would need
to exceed Arrow's 16 MiB CSV block size.

Usage: tui_partial_pass_check.py <vv-binary> <multi-batch-file>
Exit 0 on success, 1 on failure.
"""
import os, select, time, fcntl, termios, struct, sys, re


def run(vv, data, keys, env_extra, cols=100, rows=24, budget=6.0):
    m, s = os.openpty()
    fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2); os.close(m)
        env = dict(os.environ, TERM="xterm-256color", **env_extra)
        os.execvpe(vv, [vv, "-i", data], env)
    os.close(s)
    out = b""
    pending = list(keys) + [b"q"]
    deadline = time.time() + budget
    next_send = time.time() + 0.6
    while time.time() < deadline:
        r, _, _ = select.select([m], [], [], 0.2)
        if r:
            try:
                out += os.read(m, 65536)
            except OSError:
                break
        if pending and time.time() >= next_send and len(out) > 50:
            os.write(m, pending.pop(0))
            next_send = time.time() + 0.5
        if not pending and time.time() > deadline - 4.0:
            deadline = min(deadline, time.time() + 1.0)
    for fd in (m,):
        try:
            os.close(fd)
        except OSError:
            pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    return re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", out).replace(b"\x1b(B", b"")


def main():
    vv, data = sys.argv[1], sys.argv[2]
    rc = 0

    # G (drain to end, releasing every batch but the last) then s (sort:
    # a full-file pass that re-reads from row 0).
    forced = run(vv, data, [b"G", b"s"], {"VV_STREAM_BATCH_CAP": "1"})
    if b"[PARTIAL]" not in forced:
        sys.stderr.write("expected [PARTIAL] marker after a full-file pass "
                         "over an evicted stream\n")
        rc = 1

    # Default window: the fixture fits, nothing is released, no marker.
    normal = run(vv, data, [b"G", b"s"], {})
    if b"[PARTIAL]" in normal:
        sys.stderr.write("[PARTIAL] marker appeared with the default "
                         "retention window (false alarm)\n")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
