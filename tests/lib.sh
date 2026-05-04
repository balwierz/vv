#!/usr/bin/env bash
# Shared helpers for tests/run_tests.sh.

PASS=0
FAIL=0

assert_eq_file() {
    # $1 = name, $2 = actual_file, $3 = golden_file
    if diff -u "$3" "$2" > /tmp/vv-test.diff 2>&1; then
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

summarize() {
    echo
    echo "passed: $PASS  failed: $FAIL"
    [ $FAIL -eq 0 ]
}
