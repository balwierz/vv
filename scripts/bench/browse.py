#!/usr/bin/env python3
"""Read-only TUI timings; JSONL contains timings/counters, never cell contents."""
import argparse
import codecs
import errno
import fcntl
import json
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

import pyte


def counters(pid):
    """Linux process I/O counters; absent on other platforms."""
    try:
        with open("/proc/%d/io" % pid) as stream:
            return {k: int(v) for k, v in (line.split(":") for line in stream)}
    except FileNotFoundError:
        return {}


def measure(binary, path, timeout, tab=None):
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 300, 0, 0))
        os.environ["TERM"] = "xterm-256color"
        os.environ["VV_BACKGROUND"] = "dark"
        os.execvp(binary, [binary, "-i"] + (["--tab", tab] if tab else []) + [path])
    actions = [
        ("open", b""), ("next_tab", b"\t"), ("scroll", b"j" * 40),
        ("last_row", b"G"), ("first_row", b"g"),
        ("previous_tab", b"\x1b[Z"), ("revisit_tab", b"\t"),
    ]
    results = []
    active_tab, tab_count, previous_bottom = 1, 1, 0
    previous_range = [0, 0, None]
    screen = pyte.Screen(300, 40)
    terminal = pyte.Stream(screen)
    decoder = codecs.getincrementaldecoder("utf8")("replace")
    alive = True
    try:
        for name, keys in actions:
            before = counters(pid)
            start = time.monotonic()
            if name in ("next_tab", "revisit_tab"):
                active_tab = active_tab % tab_count + 1
            elif name == "previous_tab":
                active_tab = (active_tab - 2) % tab_count + 1
            if keys:
                os.write(fd, keys)
            received = 0
            unchanged = (
                (name == "last_row" and previous_bottom == previous_range[2])
                or (name == "first_row" and previous_range[0] <= 1)
                or (name == "scroll" and previous_bottom == previous_range[2])
                or (name in ("next_tab", "previous_tab", "revisit_tab") and tab_count == 1)
            )
            frame_seconds = 0.0 if unchanged else None
            row_range = previous_range if unchanged else None
            shown_tab = 0
            while time.monotonic() - start < timeout:
                ready, _, _ = select.select([fd], [], [], 0.1)
                if not ready:
                    if frame_seconds is not None:
                        break
                    continue
                try:
                    data = os.read(fd, 65536)
                except OSError as exc:
                    if exc.errno != errno.EIO:
                        raise
                    data = b""
                if not data:
                    raise RuntimeError("viewer exited during " + name)
                received += len(data)
                terminal.feed(decoder.decode(data))
                frame = re.search(r"Row ([\d_]+)-([\d_]+)/([\d_?]+)(.*?)q:quit", screen.display[-1])
                if frame:
                    first, bottom = (int(v.replace("_", "")) for v in frame.group(1, 2))
                    total = int(frame[3].replace("_", "")) if frame[3] != "?" else None
                    tab_match = re.search(r"tab (\d+)/(\d+)", screen.display[-1])
                    shown_tab = int(tab_match[1]) if tab_match else 1
                    if name == "open":
                        active_tab = shown_tab
                    if tab_match:
                        tab_count = int(tab_match[2])
                    ready = shown_tab == active_tab
                    if name == "last_row":
                        ready &= total is not None and bottom == total
                    elif name == "first_row":
                        ready &= first <= 1
                    elif name == "scroll":
                        ready &= bottom > previous_bottom or bottom == total
                    if ready:
                        frame_seconds = time.monotonic() - start
                        row_range = [first, bottom, total]
            else:
                row = re.search(r"Row [\d_]+-[\d_]+/[\d_?]+", screen.display[-1])
                raise TimeoutError("no completed table frame: %s (expected tab %d; row %s; marker %s; shown tab %d)"
                                   % (name, active_tab, row[0] if row else "absent",
                                      "q:quit" in screen.display[-1], shown_tab))
            previous_bottom = row_range[1]
            previous_range = row_range
            after = counters(pid)
            results.append({
                "action": name, "frame_seconds": round(frame_seconds, 4),
                "terminal_bytes": received, "row_range": row_range,
                "unchanged_frame": received == 0,
                "io_delta": {k: after[k] - before.get(k, 0) for k in after},
            })
        os.write(fd, b"q")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            done, status, usage = os.wait4(pid, os.WNOHANG)
            if done:
                alive = False
                return {"file": os.path.basename(path), "measurements": results,
                        "exit_code": os.waitstatus_to_exitcode(status),
                        "peak_rss": usage.ru_maxrss,
                        "peak_rss_unit": "bytes" if sys.platform == "darwin" else "KiB",
                        "user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime,
                        "binary": binary}
            time.sleep(0.05)
        raise TimeoutError("viewer did not quit")
    finally:
        if alive:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+")
    parser.add_argument("--binary", default="build/vv")
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--tab", help="initial tab (for example X)")
    args = parser.parse_args()
    for path in args.files:
        print(json.dumps(measure(args.binary, path, args.timeout, args.tab)), flush=True)


if __name__ == "__main__":
    main()
