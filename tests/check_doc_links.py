#!/usr/bin/env python3
"""Check relative links and #anchors in the Markdown docs.

  check_doc_links.py <repo-root> [file.md ...]

Every `[text](target)` / `![alt](target)` whose target is not a URL must name
an existing file, and a `#fragment` must match a heading of the target file
(GitHub's slug rules: lower-case, punctuation dropped, spaces to hyphens,
-1 / -2 suffixes for repeats). Links inside code are ignored. Prints one line
per broken link; exit 1 if there is any.
"""
import os, re, sys

root = sys.argv[1]
files = sys.argv[2:] or ["README.md", "INSTALL.md", "CONTRIBUTING.md",
                         "docs/USAGE.md", "docs/formats.md", "docs/vvg.md"]


def strip_code(text):
    text = re.sub(r"^(```|~~~).*?^\1[^\n]*$", "", text, flags=re.M | re.S)
    return re.sub(r"`[^`\n]*`", "", text)


def slug(heading):
    h = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)     # [text](url) → text
    h = h.strip().lower()
    h = re.sub(r"[^\w\- ]", "", h)                            # drop punctuation
    return h.replace(" ", "-")


def anchors(path, cache={}):
    if path not in cache:
        seen, out = {}, set()
        text = open(path, encoding="utf-8").read()
        text = re.sub(r"^(```|~~~).*?^\1[^\n]*$", "", text, flags=re.M | re.S)
        for m in re.finditer(r"^#{1,6}\s+(.*?)\s*#*\s*$", text, flags=re.M):
            s = slug(m.group(1))
            n = seen.get(s, 0)
            out.add(s if n == 0 else "%s-%d" % (s, n))
            seen[s] = n + 1
        for m in re.finditer(r'<a\s+(?:name|id)="([^"]+)"', text):
            out.add(m.group(1))
        cache[path] = out
    return cache[path]


bad = 0
for rel in files:
    path = os.path.join(root, rel)
    if not os.path.exists(path):
        continue
    text = strip_code(open(path, encoding="utf-8").read())
    targets = re.findall(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)", text)
    targets += re.findall(r'<img[^>]+src="([^"]+)"', text)
    for t in targets:
        if re.match(r"[a-z]+:", t):                           # http:, mailto:, …
            continue
        file_part, _, frag = t.partition("#")
        dest = os.path.normpath(os.path.join(os.path.dirname(path), file_part)) if file_part else path
        if not os.path.exists(dest):
            print("%s: missing file %s" % (rel, t)); bad += 1
            continue
        if frag and dest.endswith(".md") and frag not in anchors(dest):
            print("%s: no heading for #%s in %s" % (rel, frag, os.path.relpath(dest, root)))
            bad += 1
sys.exit(1 if bad else 0)
