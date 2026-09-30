#!/usr/bin/env python3
"""--image-mode sixel draws the same picture as --image-mode kitty.

Decodes vv's sixel output (palette, `#` colour select, `!n` repeats, `$` / `-`
carriage return and new band) into pixels and compares every pixel with the
kitty output of the same heatmap (RGBA, base64 chunks), scaled to sixel's
0-100 colour range. Both are rendered at the same pixel size, so a sixel
encoder that drops, shifts or miscolours a pixel fails.

Usage: sixel_check.py <vv-binary> <data-file>
Exit 0 on success, 1 on failure.
"""
import base64, re, subprocess, sys


def decode_sixel(b):
    s = b.decode("latin1"); s = s[s.index("\x1bPq")+3 : s.index("\x1b\\")]
    pal = {}; img = {}; x = y = 0; cur = 0; i = 0
    while i < len(s):
        ch = s[i]
        if ch == "#":
            m = re.match(r"#(\d+)(;2;(\d+);(\d+);(\d+))?", s[i:])
            cur = int(m.group(1))
            if m.group(2): pal[cur] = tuple(int(m.group(k)) for k in (3,4,5))
            i += len(m.group(0)); continue
        if ch == "!":
            m = re.match(r"!(\d+)(.)", s[i:]); n = int(m.group(1)); c = m.group(2); i += len(m.group(0))
        else:
            n = 1; c = ch; i += 1
        if c == "$": x = 0; continue
        if c == "-": x = 0; y += 6; continue
        v = ord(c) - 0x3F
        for _ in range(n):
            for k in range(6):
                if v >> k & 1: img[(x, y+k)] = pal[cur]
            x += 1
    return img


def decode_kitty(b):
    s = b.decode("latin1")
    w = int(re.search(r"s=(\d+)", s).group(1)); h = int(re.search(r"v=(\d+)", s).group(1))
    data = base64.b64decode("".join(re.findall(r";([A-Za-z0-9+/=]*)\x1b\\", s)))
    img = {}
    for yy in range(h):
        for xx in range(w):
            o = (yy*w+xx)*4; r,g,bb = data[o:o+3]
            img[(xx,yy)] = tuple((c*100+127)//255 for c in (r,g,bb))
    return img, w, h


def main():
    vv, data = sys.argv[1], sys.argv[2]
    run = lambda mode: subprocess.run([vv, "--heatmap", "--image-mode", mode, data],
                                      capture_output=True).stdout
    six = decode_sixel(run("sixel"))
    kit, w, h = decode_kitty(run("kitty"))
    bad = [p for p in kit if six.get(p) != kit[p]]
    if len(kit) != w * h or bad or len(six) != len(kit):
        sys.stderr.write("sixel_check: %d of %d pixels differ (sixel has %d), e.g. %r\n"
                         % (len(bad), w * h, len(six), bad[:3]))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
