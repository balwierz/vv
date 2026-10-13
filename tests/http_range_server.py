#!/usr/bin/env python3
"""A local HTTP server for vv's remote-URL tests.

Serves DIR on 127.0.0.1 and prints the port it bound. With --ranges it
honours `Range: bytes=a-b` (206 + Content-Range) and advertises
Accept-Ranges, as object stores and most web servers do; without it every
GET returns the whole file, as a server without range support does. The
query string is ignored, as a pre-signed URL's signature would be.

Usage: http_range_server.py DIR [--ranges]
"""
import http.server
import os
import re
import socketserver
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    ranges = False

    def log_message(self, *args):
        pass

    def translate_path(self, path):
        return super().translate_path(path.split("?", 1)[0].split("#", 1)[0])

    def send_head(self):
        if not self.ranges or "Range" not in self.headers:
            return super().send_head()
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            self.send_error(404, "File not found")
            return None
        size = os.path.getsize(path)
        m = re.match(r"bytes=(\d*)-(\d*)$", self.headers["Range"].strip())
        if not m or (m.group(1) == "" and m.group(2) == ""):
            self.send_error(416)
            return None
        if m.group(1) == "":
            start, end = max(0, size - int(m.group(2))), size - 1
        else:
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        if start >= size:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % size)
            self.end_headers()
            return None
        f = open(path, "rb")
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self._left = end - start + 1
        return f

    def end_headers(self):
        if self.ranges:
            self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    def copyfile(self, source, outputfile):
        left = getattr(self, "_left", None)
        if left is None:
            return super().copyfile(source, outputfile)
        self._left = None
        while left > 0:
            buf = source.read(min(left, 1 << 16))
            if not buf:
                break
            try:
                outputfile.write(buf)
            except (BrokenPipeError, ConnectionResetError):
                break
            left -= len(buf)


def main():
    os.chdir(sys.argv[1])
    Handler.ranges = "--ranges" in sys.argv[2:]
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    socketserver.ThreadingTCPServer.daemon_threads = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler) as srv:
        print(srv.server_address[1], flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    main()
