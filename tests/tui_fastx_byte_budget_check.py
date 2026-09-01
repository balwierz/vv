#!/usr/bin/env python3
"""FastxSource batches by record count (4096). A file of a few very long
records stays under that cap, so without a byte budget it packs the whole file
into a single batch — the trailing-window eviction never fires and a genome
FASTA loads entirely into RAM.

This drives the TUI over a file of a FEW large records (fewer than the 4096
record cap) with VV_STREAM_BATCH_CAP=1. The ONLY variable between the two runs
is VV_FASTX_BATCH_BYTES:

  * small byte budget  -> each large record exceeds it, so the file spans
    several batches; jumping to the end (G) evicts all but one, and a full-file
    pass (s) must then flag [PARTIAL].
  * default byte budget -> the same few records fit in one batch (record count
    is well under 4096), nothing is evicted, so [PARTIAL] must NOT appear.

[PARTIAL] appearing only with the small budget proves the byte budget — not the
record cap — split the batches. A binary without the byte budget produces one
batch in both runs and fails the first check.

Usage: tui_fastx_byte_budget_check.py <vv-binary> <few-large-records-file>
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
    try:
        os.close(m)
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

    # Small byte budget: the few large records span several batches; G evicts
    # all but the last, then s (a full-file pass) must flag the partial view.
    forced = run(vv, data, [b"G", b"s"],
                 {"VV_STREAM_BATCH_CAP": "1", "VV_FASTX_BATCH_BYTES": "131072"})
    if b"[PARTIAL]" not in forced:
        sys.stderr.write("expected [PARTIAL]: a small byte budget should split "
                         "the few large records across batches\n")
        rc = 1

    # Default byte budget, same window cap: record count is under 4096, so the
    # file is one batch and nothing is evicted — no marker.
    normal = run(vv, data, [b"G", b"s"], {"VV_STREAM_BATCH_CAP": "1"})
    if b"[PARTIAL]" in normal:
        sys.stderr.write("[PARTIAL] appeared with the default byte budget: the "
                         "few records should fit one batch (false alarm)\n")
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
