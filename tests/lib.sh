#!/usr/bin/env bash
# Shared helpers for tests/run_tests.sh.

PASS=0
FAIL=0

assert_eq_file() {
    # $1 = name, $2 = actual_file, $3 = golden_file
    # Normalise absolute fixture paths so goldens committed from one
    # checkout (e.g. /home/piotr/Sources/ParquetViewer/tests/data) match
    # output captured from another (e.g. /home/runner/work/vv/vv/tests/data
    # on GitHub-hosted CI). Strips everything up to and including
    # "/tests/data/" so the line reads "File: tests/data/tiny.parquet".
    local _a="/tmp/vv-test.a" _b="/tmp/vv-test.b"
    sed -E 's@(File: |/)/?[^[:space:]]*tests/data/@\1tests/data/@g' "$2" > "$_a"
    sed -E 's@(File: |/)/?[^[:space:]]*tests/data/@\1tests/data/@g' "$3" > "$_b"
    if diff -u "$_b" "$_a" > /tmp/vv-test.diff 2>&1; then
        PASS=$((PASS + 1))
        echo "  ok    $1"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL  $1"
        echo "     diff:"
        sed 's/^/       /' /tmp/vv-test.diff | head -40
    fi
}

assert_contains() {
    # $1 = name, $2 = haystack, $3 = needle
    if printf '%s' "$2" | grep -qF -- "$3"; then
        PASS=$((PASS + 1))
        echo "  ok    $1"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL  $1 (missing '$3')"
        echo "       got: $2"
    fi
}

refute_contains() {
    # $1 = name, $2 = haystack, $3 = needle (must be ABSENT)
    if printf '%s' "$2" | grep -qF -- "$3"; then
        FAIL=$((FAIL + 1))
        echo "  FAIL  $1 (unexpected '$3')"
        echo "       got: $2"
    else
        PASS=$((PASS + 1))
        echo "  ok    $1"
    fi
}

assert_exit_zero() {
    # $1 = name, "$@" rest = command
    local name="$1"; shift
    if "$@" > /dev/null 2>&1; then
        PASS=$((PASS + 1))
        echo "  ok    $name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL  $name (exit $?)"
    fi
}

assert_eq_file_inline() {
    # $1 = name, $2 = actual, $3 = expected (string equality)
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1)); echo "  ok    $1"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL  $1 (got '$2', want '$3')"
    fi
}

summarize() {
    echo
    echo "passed: $PASS  failed: $FAIL"
    [ $FAIL -eq 0 ]
}
