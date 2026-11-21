#!/usr/bin/env bash
#
# tests/fuzz/run_fuzz.sh - crash fuzzing of the compiler.
#
#   bash tests/fuzz/run_fuzz.sh                 20000 damaged programs, seed 1
#   bash tests/fuzz/run_fuzz.sh 100000 7        more, and different ones (seed 7)
#   FUZZ_MODE=semantic bash tests/fuzz/run_fuzz.sh   only the syntax-keeping kind
#
# Builds a separate copy of safec with AddressSanitizer and UBSan in
# tests/fuzz/build/ (your normal ./safec isn't touched), then runs fuzz.py with
# it over every test in tests/cases. Problems are saved in tests/fuzz/out/.
# Exits 0 if nothing crashed, hung or produced invalid IR.

set -u
cd "$(dirname "$0")" || exit 2
N=${1:-20000}
SEED=${2:-1}

# the sanitizer build: the same sources and Makefile, compiled with the checks.
# cp -p keeps the timestamps, so make only rebuilds what changed since last time
mkdir -p build
cp -pR ../../src ../../Makefile build/
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
echo "building safec with AddressSanitizer + UBSan in tests/fuzz/build/ ..."
if ! make -s -C build safec CC="cc $SAN" CXX="c++ $SAN" > build/make.log 2>&1; then
    cat build/make.log
    echo "the sanitizer build failed (the compiler must support -fsanitize=address,undefined)"
    exit 2
fi

rm -rf out
echo "fuzzing: $N damaged programs from tests/cases, seed $SEED"
python3 fuzz.py build/safec ../cases out "$N" "$SEED"
