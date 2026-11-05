
#!/bin/bash

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TEST_DIR="$ROOT/tests/lexer"
LEXER="$ROOT/src/lexer"

PASSED=0
FAILED=0

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT

echo "========================================"
echo "           LEXER TEST SUITE"
echo "========================================"

if [ ! -x "$LEXER" ]; then
    echo "[ERROR] Lexer executable not found"
    exit 1
fi

# VALID TESTS
find "$TEST_DIR/input" -mindepth 2 -type f -name '*.safec' \
    ! -path "$TEST_DIR/input/10-invalid/*" | sort |
while IFS= read -r input; do
    relative="${input#"$TEST_DIR/input/"}"
    expected="$TEST_DIR/expected/${relative%.safec}.txt"
    actual="$TEMP_DIR/$(echo "$relative" | tr '/' '_').txt"

    if [ ! -f "$expected" ]; then
        echo "[MISSING EXPECTED] $relative"
        echo "$relative" >> "$TEMP_DIR/failed"
    elif "$LEXER" < "$input" > "$actual" 2>&1 &&
         diff -u "$expected" "$actual" > /dev/null; then
        echo "[PASS] $relative"
        echo "pass" >> "$TEMP_DIR/passed"
    else
        echo "[FAIL] $relative"
        echo "$relative" >> "$TEMP_DIR/failed"
    fi
done

# INVALID TESTS
find "$TEST_DIR/input/10-invalid" -type f -name '*.safec' | sort |
while IFS= read -r input; do
    relative="${input#"$TEST_DIR/input/"}"
    actual="$TEMP_DIR/invalid_$(basename "$input").txt"

    "$LEXER" < "$input" > "$actual" 2>&1 || true

    if grep -q "^ERROR at line" "$actual"; then
        echo "[PASS - ERROR DETECTED] $relative"
        echo "pass" >> "$TEMP_DIR/passed"
    else
        echo "[FAIL - NO ERROR DETECTED] $relative"
        echo "$relative" >> "$TEMP_DIR/failed"
    fi
done

PASSED=$(wc -l < "$TEMP_DIR/passed" 2>/dev/null || echo 0)
FAILED=$(wc -l < "$TEMP_DIR/failed" 2>/dev/null || echo 0)

echo "========================================"
echo "Passed: $PASSED"
echo "Failed: $FAILED"
echo "Total:  $((PASSED + FAILED))"
echo "========================================"

[ "$FAILED" -eq 0 ]
