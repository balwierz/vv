# tiny.md

Hand-crafted fixture for the vv markdown viewer's smoke tests. Exercises one
of each block type: paragraph with **bold** / *italic* / ~~strike~~ / `inline
code` / a [link](https://example.com), a fenced code block, an unordered
list, an ordered list, a block quote, an HR, and **two GFM tables** whose
columns vv exposes through the regular table renderer for type inference.

```sh
# fenced code block, language = sh
vv tiny.md
```

## Lists

- alpha
- beta
- gamma

1. first
2. second
3. third

> A block quote. vv prefixes wrapped lines with a left-bar glyph.

---

## Benchmark table

| label | rows  | runtime_ms |
|-------|-------|------------|
| small | 1000  | 12.5       |
| med   | 10000 | 121.7      |
| large | 100000| 1240.3     |

## Reference table

| key   | value          |
|-------|----------------|
| one   | first entry    |
| two   | second entry   |
| three | third entry    |

![placeholder image](missing.png)
