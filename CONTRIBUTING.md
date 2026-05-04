# Contributing to vv

Thanks for your interest in `vv`! Bug reports, feature requests, and pull
requests are all welcome.

## Reporting bugs

Please include:
- `vv --version` output
- The OS / distribution and how you installed `vv` (Bioconda, Homebrew,
  built from source, …)
- A minimal file or command that reproduces the issue (a few KB attached
  to the issue is best)
- The expected vs. observed behaviour

For potential security issues, follow [SECURITY.md](SECURITY.md) instead.

## Building locally

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Dependencies: CMake ≥ 3.16, GCC ≥ 10 / Clang ≥ 12, Apache Arrow + Parquet,
htslib, ncurses. See [INSTALL.md](INSTALL.md) for distro-specific install
hints.

## Running tests

```sh
tests/run_tests.sh
```

The harness diffs `vv` output against checked-in golden files. If you
intentionally changed user-facing output, regenerate the affected goldens
and include them in your PR.

To regenerate the test fixture data (rare):

```sh
python3 tests/data/generate.py
```

## Code style

- Single-file `main.cpp` is intentional. New formats fit alongside the
  existing source classes.
- Keep changes localized; don't refactor unrelated sections in a feature PR.
- `.clang-format` defines indentation and column limits; please run it on
  any new code.
- Prefer the existing helpers (`cell_to_string`, `truncate`,
  `digits_with_sep`, `display_width`) over reinventing.
- New features that touch user-visible output usually need a new test
  fixture under `tests/data/` and a golden file under `tests/golden/`.

## Pull request checklist

- [ ] CI is green (Linux + macOS build + smoke tests).
- [ ] CHANGELOG.md updated under "Unreleased".
- [ ] Help text and man page (`man/vv.1`) updated if a CLI flag changed.
- [ ] Bumped `kVersion` only if releasing.
- [ ] No new compiler warnings.

## Releasing (maintainers)

1. Verify `tests/run_tests.sh` passes locally.
2. Bump `kVersion` in `main.cpp` and the version field in `CITATION.cff`.
3. Move "Unreleased" CHANGELOG entries to a new versioned section with
   today's date.
4. Commit: `chore: release vX.Y.Z`.
5. Tag: `git tag -a vX.Y.Z -m "vX.Y.Z"; git push --tags`.
6. The `release.yml` workflow builds the static binary, attaches it
   plus checksums to the GitHub Release, and publishes the page.
7. Update the Bioconda recipe (`packaging/bioconda/meta.yaml`) and open
   a PR against `bioconda/bioconda-recipes`.
8. Bump the Homebrew formula in `balwierz/homebrew-tap`.

## Code of conduct

Participation is governed by the
[Contributor Covenant](CODE_OF_CONDUCT.md).
