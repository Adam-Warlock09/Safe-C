
#!/bin/bash

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TEST_DIR="$ROOT/tests/lexer"
LEXER="$ROOT/lexer"

PASSED=0
FAILED=0

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT

echo "================================"
echo "       LEXER TEST SUITE"
echo "================================"

for input in "$TEST_DIR"/input/*/*.safec; do
    [ -f "$input" ] || continue

    relative="${input#"$TEST_DIR/input/"}"
    expected="$TEST_DIR/expected/${relative%.safec}.txt"
    actual="$TEMP_DIR/${relative%.safec}.txt"

    mkdir -p "$(dirname "$actual")"

    if [ ! -f "$expected" ]; then
        echo "[MISSING EXPECTED] $relative"
        FAILED=$((FAILED + 1))
        continue
    fi

    if ! "$LEXER" < "$input" > "$actual" 2>&1; then
        echo "[ERROR] $relative"
        FAILED=$((FAILED + 1))
        continue
    fi

    if diff -u "$expected" "$actual" > /dev/null; then
        echo "[PASS] $relative"
        PASSED=$((PASSED + 1))
    else
        echo "[FAIL] $relative"
        FAILED=$((FAILED + 1))
    fi
done

echo "================================"
echo "Passed: $PASSED"
echo "Failed: $FAILED"
echo "================================"

[ "$FAILED" -eq 0 ]
