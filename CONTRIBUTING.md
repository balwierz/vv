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

- [ ] CI is green: build + smoke tests on Linux (22.04, 24.04) and macOS,
      plus the ASan/UBSan and fuzzer gates (Linux only).
- [ ] CHANGELOG.md updated under "Unreleased".
- [ ] Help text and man page (`man/vv.1`) updated if a CLI flag changed.
- [ ] Bumped `kVersion` only if releasing.
- [ ] No new compiler warnings.

## Releasing (maintainers)

1. Verify `tests/run_tests.sh` passes locally, and under an ASan+UBSan build.
2. Bump the version in **every** place that hard-codes it. There is no single
   source of truth, and each of these has been missed by at least one past
   release:

   | File | What |
   |---|---|
   | `main.cpp` | `kVersion` — feeds `--version` *and* the Parquet writer's `created_by` |
   | `CMakeLists.txt` | `project(... VERSION ...)` |
   | `CITATION.cff` | `version:` **and** `date-released:` (GitHub's "Cite this repository" box) |
   | `man/vv.1` | the `.TH` line — both the version and the month |
   | `docs/USAGE.md` | the `date:` field in the YAML front matter |
   | `README.md`, `INSTALL.md` | the hard-coded version in every install command |
   | `packaging/arch/PKGBUILD` | `pkgver`, then run `updpkgsums` (from `pacman-contrib`) |

   `packaging/debian/build-deb.sh` derives its version from `vv --version`, and
   `release.yml` derives every artifact name from the tag, so neither needs an
   edit — but both are wrong if `kVersion` is.

3. Move "Unreleased" CHANGELOG entries to a new versioned section with the
   release date. `release.yml` extracts that section verbatim as the GitHub
   Release body, so it is the release notes — write it for readers.
4. Commit: `release: X.Y.Z`.
5. Tag: `git tag -a vX.Y.Z -m "vX.Y.Z"; git push --tags`.
6. The `release.yml` workflow builds the static binary for x86_64 and aarch64,
   attaches the tarballs, `.deb`s and `SHA256SUMS` to the GitHub Release, and
   publishes the page.
7. Check `vv --version` in a downloaded artifact matches the tag.

`packaging/bioconda/meta.yaml` and `packaging/homebrew/vv.rb` are **unpublished
drafts** — vv is not on Bioconda and there is no Homebrew tap, so there is
nothing to update there. Do not treat their stale version fields as a release
step; if you publish those channels, add the steps back.

## Code of conduct

Participation is governed by the
[Contributor Covenant](CODE_OF_CONDUCT.md).
