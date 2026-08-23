#!/usr/bin/env python3
"""Regression check for chunk_meta() bounds on a partly-decodable IPC file.

An Arrow IPC file whose footer declares N record batches but whose batch 1 is
corrupt opens cleanly (schema + batch 0 are intact), yet num_chunks() reports
the declared count. IpcSource::chunk_meta(i) indexed batches_ / batch_first_row_
directly, so paging to a chunk past the one that failed to load read out of
bounds and dereferenced a garbage pointer.

The TUI reaches chunk_meta() for the last chunk when the user jumps to the
bottom. This launches `vv -i <corrupt.arrow>` under a pty, sends 'G' (go to
bottom), then 'q', and verifies the process did NOT die from a signal.

Usage: tui_corrupt_chunk_check.py <vv-binary> <file>
Exit 0 on success, 1 if the process was killed by a signal.
"""
import os
import pty
import select
import signal
import struct
import sys
import termios
import time
import fcntl


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: tui_corrupt_chunk_check.py <vv-binary> <file>", file=sys.stderr)
        return 2
    binary, path = sys.argv[1], sys.argv[2]

    pid, fd = pty.fork()
    if pid == 0:                       # child: the TUI
        os.environ["TERM"] = "xterm-256color"
        try:
            os.execvp(binary, [binary, "-i", path])
        except Exception:
            os._exit(127)

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

    # Drain briefly so the TUI paints its first frame, then jump to the bottom
    # (which needs chunk_meta(num_chunks()-1)) and quit.
    def drain(seconds):
        # Non-blocking: never wait on os.read (a quiet TUI produces no bytes,
        # and a blocking read would stall past the window and delay the keys).
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if not r:
                continue
            try:
                if not os.read(fd, 65536):
                    return
            except OSError:
                return

    time.sleep(0.8)
    try:
        os.write(fd, b"G")
        drain(0.8)
        os.write(fd, b"q")
    except OSError:
        pass
    drain(0.5)

    _, status = os.waitpid(pid, 0)
    try:
        os.close(fd)
    except OSError:
        pass

    if os.WIFSIGNALED(status):
        sig = os.WTERMSIG(status)
        print(f"FAIL: vv died from signal {sig} ({signal.Signals(sig).name}) "
              f"paging a partly-decodable IPC file", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
