#!/usr/bin/env python3
"""Check C/F-order leading-axis slices through both NPY and NPZ readers."""
import base64
import pathlib
import re
import subprocess
import sys
import zipfile


def main():
    vv, fixture, scratch = sys.argv[1:4]
    tui = "--tui" in sys.argv[4:]
    expected = {
        "cube": "0\t1\t2\n10\t11\t12\n",
        "tensor": "0\t1\t2\t10\t11\t12\n100\t101\t102\t110\t111\t112\n",
    }
    with zipfile.ZipFile(fixture) as archive:
        for name in archive.namelist():
            member = pathlib.Path(name).stem
            bare = pathlib.Path(scratch) / name
            bare.write_bytes(archive.read(name))
            try:
                for args in ([str(bare)], ["--tab", member, fixture]):
                    if member.startswith("empty_"):
                        result = subprocess.run(
                            [vv, "--count", *args], capture_output=True,
                            text=True, timeout=15, check=True)
                        rows = "2\n" if member == "empty_cols_f" else "0\n"
                        assert result.stdout == rows, (member, result.stdout)
                    else:
                        result = subprocess.run(
                            [vv, "--tsv", "--no-header", *args],
                            capture_output=True, text=True, timeout=15, check=True)
                        assert result.stdout == expected[member.rsplit("_", 1)[0]], (
                            member, args, result.stdout)
                if tui and not member.startswith("empty_"):
                    from tui_cursor_check import run

                    # Yank the last cell in row 1 after changing to slice 1,
                    # then jump back to slice 0. Use each container type.
                    width = 3 if member.startswith("cube_") else 6
                    cell = [b"j"] + [b"l"] * (width - 1) + [b"y"]
                    keys = [b"]", *cell, b":slice 0\n", *cell]
                    args = [str(bare)] if width == 3 else [
                        "--tab", member, fixture]
                    text, raw, hung = run(vv, args, keys, budget=12)
                    copied = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]+)", raw)
                    values = [int(base64.b64decode(value)) for value in copied]
                    want = [112, 12] if width == 3 else [1112, 112]
                    assert not hung and values == want, (member, values, text[-1000:])
            finally:
                bare.unlink()
    print("NumPy C/F 3D/4D slices and empty axes passed")


if __name__ == "__main__":
    main()
