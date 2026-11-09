#!/usr/bin/env python3
"""Write raw RGBA pixels as an indexed PNG whose palette carries alpha (tRNS).

    magick in.png -depth 8 RGBA:- | palette_png.py WIDTH HEIGHT out.png

ImageMagick's PNG8 writer keeps one fully transparent colour only, which leaves
an anti-aliased rounded corner jagged; a truecolour + alpha PNG of a window
screenshot is about three times the size of the indexed one. The input must
already hold at most 256 distinct RGBA values.
"""
import struct
import sys
import zlib


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def main() -> None:
    w, h, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    raw = sys.stdin.buffer.read()
    if len(raw) != w * h * 4:
        sys.exit(f"palette_png: expected {w * h * 4} bytes of RGBA, got {len(raw)}")
    px = [raw[i:i + 4] for i in range(0, len(raw), 4)]
    # Translucent entries first, so tRNS covers only those.
    colours = sorted(set(px), key=lambda c: (c[3] == 255, c))
    if len(colours) > 256:
        sys.exit(f"palette_png: {len(colours)} colours, at most 256 fit a palette")
    index = {c: i for i, c in enumerate(colours)}
    rows = bytearray()
    for y in range(h):
        rows.append(0)                                   # filter type: none
        rows.extend(index[c] for c in px[y * w:(y + 1) * w])
    plte = b"".join(c[:3] for c in colours)
    trns = bytes(c[3] for c in colours if c[3] != 255)
    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)))
        f.write(chunk(b"PLTE", plte))
        if trns:
            f.write(chunk(b"tRNS", trns))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
        f.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
