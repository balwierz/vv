#!/usr/bin/env bash
# Time vv on large inputs: wall-clock seconds and peak RSS per case, one line
# each. Every input is optional (pass "" to skip one); nothing is written
# except to the scratch directory, which is removed at exit.
#
#   scripts/bench_large.sh [-v VV] PARQUET H5AD_DENSE H5AD_CSR TEXT_GZ
#
# PARQUET      a multi-million-row Parquet file; its first string column and
#              first floating-point column are used for --filter / --sort
# H5AD_DENSE   an AnnData file whose X is a dense matrix
# H5AD_CSR     an AnnData file whose X is CSR
# TEXT_GZ      a gzip-compressed CSV / TSV
set -uo pipefail

VV="$(dirname "$0")/../build/vv"
if [ "${1:-}" = "-v" ]; then VV="$2"; shift 2; fi
PARQUET="${1:-}" DENSE="${2:-}" CSR="${3:-}" TEXT="${4:-}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

run() {
    local label="$1"; shift
    /usr/bin/time -f "%e %M" -o "$TMP/time" "$@" >/dev/null 2>"$TMP/err"
    local rc=$? t m
    read -r t m < <(tail -1 "$TMP/time")
    printf '%-44s %8s s %9s MB' "$label" "$t" "$((m / 1024))"
    [ "$rc" -eq 0 ] || printf '  (exit %d: %s)' "$rc" "$(tail -c 120 "$TMP/err" | tr '\n' ' ')"
    printf '\n'
}

if [ -n "$PARQUET" ]; then
    # First string and first floating-point column, from the schema footer.
    read -r SCOL FCOL < <("$VV" --schema "$PARQUET" 2>/dev/null | awk '
        $2 ~ /^(string|large_string|utf8)/ && !s { s = $1 }
        $2 ~ /^(double|float)/ && !f { f = $1 }
        END { print s, f }')
    echo "## $(basename "$PARQUET")  (string column: $SCOL, float column: $FCOL)"
    run "parquet -n 5 -t"                  "$VV" -n 5 -t "$PARQUET"
    run "parquet --tsv"                    "$VV" --tsv "$PARQUET"
    run "parquet --csv"                    "$VV" --csv "$PARQUET"
    run "parquet --json"                   "$VV" --json "$PARQUET"
    run "parquet --describe"               "$VV" --describe "$PARQUET"
    run "parquet --tsv --select $SCOL"     "$VV" --tsv --select "$SCOL" "$PARQUET"
    run "parquet --tsv --filter '$SCOL ~ ^A'" "$VV" --tsv --filter "$SCOL ~ \"^A\"" "$PARQUET"
    run "parquet --count --filter '$FCOL > 1'" "$VV" --count --filter "$FCOL > 1" "$PARQUET"
    run "parquet --sort $FCOL -n 5"        "$VV" --sort "$FCOL" -n 5 -t "$PARQUET"
    run "parquet --tail 5"                 "$VV" --tail 5 -t "$PARQUET"
    run "parquet --sample 5"               "$VV" --sample 5 -t "$PARQUET"
    run "parquet --parquet"                "$VV" --parquet "$TMP/out.parquet" "$PARQUET"
fi
if [ -n "$DENSE" ]; then
    echo "## $(basename "$DENSE")  (dense X)"
    run "dense X --tsv"                    "$VV" --tsv --tab X "$DENSE"
    run "dense X --describe"               "$VV" --describe --tab X "$DENSE"
fi
if [ -n "$CSR" ]; then
    echo "## $(basename "$CSR")  (CSR X)"
    run "csr X --tsv"                      "$VV" --tsv --tab X "$CSR"
    run "csr obs --tsv"                    "$VV" --tsv --tab obs "$CSR"
    run "csr obs --describe"               "$VV" --describe --tab obs "$CSR"
fi
if [ -n "$TEXT" ]; then
    echo "## $(basename "$TEXT")"
    run "text.gz -n 5 -t"                  "$VV" -n 5 -t "$TEXT"
    run "text.gz --count"                  "$VV" --count "$TEXT"
    run "text.gz --tsv"                    "$VV" --tsv "$TEXT"
    run "text.gz --describe"               "$VV" --describe "$TEXT"
fi
