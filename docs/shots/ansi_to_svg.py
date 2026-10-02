#!/usr/bin/env python3
"""Render a captured terminal screen (ANSI escapes) as an SVG.

  ansi_to_svg.py <in.ansi> <out.svg> <title> <cols> [max-lines]

The input is what `tmux capture-pane -e -p` or a colour-forced command
prints. Uses rich's SVG export (window chrome, monospace grid).
"""
import re, sys
from rich.console import Console
from rich.terminal_theme import TerminalTheme
from rich.text import Text

src, dst, title, cols = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
max_lines = int(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] else 0
data = open(src, encoding="utf-8", errors="replace").read()
# Keep SGR (colour) sequences only; drop OSC / cursor / keypad controls.
data = re.sub(r"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", "", data)
data = re.sub(r"\x1b\[[0-9;?]*[A-Za-ln-z]", "", data)
data = re.sub(r"\x1b[=>]", "", data)
lines = data.rstrip("\n").split("\n")
while lines and not lines[-1].strip():
    lines.pop()
if max_lines and len(lines) > max_lines:          # long output: mark the cut
    lines = lines[:max_lines] + ["\x1b[2m…\x1b[0m"]

# One Dark-like palette; background matches GitHub's dark code blocks.
THEME = TerminalTheme(
    (0x1e, 0x22, 0x27), (0xd7, 0xda, 0xe0),
    [(0x1e, 0x22, 0x27), (0xe0, 0x6c, 0x75), (0x98, 0xc3, 0x79), (0xe5, 0xc0, 0x7b),
     (0x61, 0xaf, 0xef), (0xc6, 0x78, 0xdd), (0x56, 0xb6, 0xc2), (0xab, 0xb2, 0xbf)],
    [(0x5c, 0x63, 0x70), (0xe0, 0x6c, 0x75), (0x98, 0xc3, 0x79), (0xe5, 0xc0, 0x7b),
     (0x61, 0xaf, 0xef), (0xc6, 0x78, 0xdd), (0x56, 0xb6, 0xc2), (0xff, 0xff, 0xff)],
)

con = Console(record=True, width=cols, force_terminal=True, color_system="truecolor",
              file=open("/dev/null", "w"))
for ln in lines:
    t = Text.from_ansi(ln)
    t.no_wrap = True
    t.overflow = "crop"
    con.print(t)
svg = con.export_svg(title=title, theme=THEME)
# rich derives element ids from a hash of the content: fine, deterministic.
open(dst, "w", encoding="utf-8").write(svg)
