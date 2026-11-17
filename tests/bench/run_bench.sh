#!/usr/bin/env bash
#
# tests/bench/run_bench.sh - how fast SafeC programs run compared with C.
#
#   bash tests/bench/run_bench.sh [NAME...]
#
# Each benchmark is a pair, tests/bench/NAME.safec and NAME.c: the same
# program written in SafeC and as ordinary C (plain pointers, no restrict).
# Both are built at -O2 (safec -O2, clang -O2) and must print the same output.
# They run RUNS times each, alternating (C, SafeC, C, SafeC, ...), and the
# fastest run of each is reported, with SafeC's time as a percentage of C's:
# the fastest run is the one least disturbed by the rest of the machine.
# The whitepaper's target (section 5.1) is within 10% of C.
#
#   RUNS=9            how many times each binary runs
#   SAFEC=./safec     the compiler to measure (to compare two builds of it)
#
# Not part of tests/run_tests.sh: timings depend on the machine and its load.

set -u
cd "$(dirname "$0")/../.." || exit 2
SAFEC=${SAFEC:-./safec}
RUNS=${RUNS:-9}
DIR=tests/bench
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ "$SAFEC" = ./safec ] && ! make -s safec > "$TMP/build" 2>&1; then
    cat "$TMP/build"; exit 2
fi

# wall time of one run of $1, in milliseconds
run_ms() {
    local start end
    start=$(date +%s%N)
    "$1" > /dev/null
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

# fastest of the numbers on stdin
fastest() { sort -n | head -1; }

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
    for f in "$DIR"/*.safec; do names+=("$(basename "$f" .safec)"); done
fi

echo "fastest of $RUNS runs each, built with -O2"
printf '%-12s %10s %10s %8s\n' benchmark "C (ms)" "SafeC (ms)" "SafeC/C"
status=0
for n in "${names[@]}"; do
    if ! "$SAFEC" -O2 "$DIR/$n.safec" -o "$TMP/$n.safec.bin" > "$TMP/out" 2>&1; then
        echo "$n: safec failed:"; cat "$TMP/out"; status=1; continue
    fi
    if ! clang -O2 "$DIR/$n.c" -o "$TMP/$n.c.bin" 2> "$TMP/out"; then
        echo "$n: clang failed:"; cat "$TMP/out"; status=1; continue
    fi
    if [ "$("$TMP/$n.safec.bin")" != "$("$TMP/$n.c.bin")" ]; then
        echo "$n: SafeC and C print different results"; status=1; continue
    fi
    : > "$TMP/c_times"; : > "$TMP/s_times"
    for i in $(seq "$RUNS"); do
        run_ms "$TMP/$n.c.bin" >> "$TMP/c_times"
        run_ms "$TMP/$n.safec.bin" >> "$TMP/s_times"
    done
    c_ms=$(fastest < "$TMP/c_times")
    s_ms=$(fastest < "$TMP/s_times")
    printf '%-12s %10s %10s %7s%%\n' "$n" "$c_ms" "$s_ms" "$(awk -v s="$s_ms" -v c="$c_ms" 'BEGIN { printf "%.0f", (c > 0) ? 100 * s / c : 0 }')"
done
exit $status
