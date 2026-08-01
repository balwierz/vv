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
    # Also normalise the writer library version in the "Created by:" footer
    # (e.g. "parquet-cpp-arrow version 24.0.0") — fixtures are regenerated in CI
    # with whatever pyarrow is installed, so the exact version drifts and isn't
    # something vv controls or should diff on.
    local _a="/tmp/vv-test.a" _b="/tmp/vv-test.b"
    local _norm='s@(File: |/)/?[^[:space:]]*tests/data/@\1tests/data/@g; s@(parquet-cpp-arrow version )[0-9][0-9.]*@\1X@g'
    sed -E "$_norm" "$2" > "$_a"
    sed -E "$_norm" "$3" > "$_b"
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

assert_exit_code() {
    # $1 = name, $2 = expected exit code, "$@" rest = command.
    # Use for "clean error" paths — a crash would be >=128 (signal), which fails.
    local name="$1" want="$2"; shift 2
    "$@" > /dev/null 2>&1; local rc=$?
    if [ "$rc" = "$want" ]; then
        PASS=$((PASS + 1))
        echo "  ok    $name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL  $name (exit $rc, want $want)"
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
