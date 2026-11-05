#!/usr/bin/env bash

# ============================================================
# Safe-C Parser Test Suite
# Valid tests   : parse successfully + AST matches expected
# Invalid tests : parser must detect a syntax error
# ============================================================

set -u
shopt -s nullglob

cd "$(dirname "$0")/../.."

PARSER="./safec-parser"
DIR="tests/parser"
VALID="$DIR/valid"
INVALID="$DIR/invalid"
EXPECTED="$DIR/expected/valid"

UPDATE=0
[[ "${1:-}" == "--update-expected" ]] && UPDATE=1

# Colours
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RESET='\033[0m'

PASS=0
FAIL=0
UPDATED=0
VALID_PASS=0
INVALID_PASS=0
FAILED_TESTS=()

echo
echo "============================================================"
echo "                 SAFEC PARSER TEST SUITE"
echo "============================================================"

if [[ ! -x "$PARSER" ]]; then
    echo -e "${RED}ERROR:${RESET} Parser executable not found."
    echo "Run make first."
    exit 2
fi

TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT

# ------------------------------------------------------------
# VALID TESTS
# ------------------------------------------------------------

echo
echo -e "${CYAN}--------------- VALID TESTS ----------------${RESET}"

valid_tests=("$VALID"/*.safec)

if [[ ${#valid_tests[@]} -eq 0 ]]; then
    echo -e "${YELLOW}No valid test files found.${RESET}"
fi

for src in "${valid_tests[@]}"; do

    name=$(basename "$src" .safec)
    golden="$EXPECTED/$name.txt"
    actual="$TMPDIR_TEST/$name.actual"

    "$PARSER" --ast "$src" > "$actual" 2>&1
    status=$?

    if [[ $status -ne 0 ]] || grep -q "error:" "$actual"; then
        echo -e "${RED}[FAIL]${RESET} $name - Expected valid program"
        echo "       Parser rejected the input."
        sed 's/^/       /' "$actual"
        FAIL=$((FAIL + 1))
        FAILED_TESTS+=("$name")
        continue
    fi

    if [[ $UPDATE -eq 1 ]]; then
        mkdir -p "$EXPECTED"
        cp "$actual" "$golden"
        echo -e "${YELLOW}[UPDATED]${RESET} $name - Expected AST regenerated"
        UPDATED=$((UPDATED + 1))
        continue
    fi

    if [[ ! -f "$golden" ]]; then
        echo -e "${RED}[FAIL]${RESET} $name - Expected AST file missing"
        echo "       Missing: $golden"
        FAIL=$((FAIL + 1))
        FAILED_TESTS+=("$name")
        continue
    fi

    if diff -u "$golden" "$actual" > "$TMPDIR_TEST/diff"; then
        echo -e "${GREEN}[PASS]${RESET} $name - Valid syntax, AST matches expected"
        PASS=$((PASS + 1))
        VALID_PASS=$((VALID_PASS + 1))
    else
        echo -e "${RED}[FAIL]${RESET} $name - AST does not match expected"
        echo "       Differences (expected vs actual):"
        head -20 "$TMPDIR_TEST/diff" | sed 's/^/       /'
        FAIL=$((FAIL + 1))
        FAILED_TESTS+=("$name")
    fi

done

# ------------------------------------------------------------
# INVALID TESTS
# ------------------------------------------------------------

echo
echo -e "${CYAN}--------------- INVALID TESTS ---------------${RESET}"

invalid_tests=("$INVALID"/*.safec)

if [[ ${#invalid_tests[@]} -eq 0 ]]; then
    echo -e "${YELLOW}No invalid test files found.${RESET}"
fi

for src in "${invalid_tests[@]}"; do

    name=$(basename "$src" .safec)
    actual="$TMPDIR_TEST/$name.actual"

    "$PARSER" "$src" > "$actual" 2>&1
    status=$?

    if [[ $status -ne 0 ]] && grep -qi "syntax error" "$actual"; then
        echo -e "${GREEN}[PASS]${RESET} $name - Syntax error detected correctly"
        PASS=$((PASS + 1))
        INVALID_PASS=$((INVALID_PASS + 1))
    else
        echo -e "${RED}[FAIL]${RESET} $name - Expected syntax error detection"
        echo "       Exit code: $status"
        sed 's/^/       /' "$actual"
        FAIL=$((FAIL + 1))
        FAILED_TESTS+=("$name")
    fi

done

# ------------------------------------------------------------
# SUMMARY
# ------------------------------------------------------------

TOTAL=$((PASS + FAIL))

echo
echo "============================================================"
echo "                     TEST SUMMARY"
echo "============================================================"

if [[ $UPDATE -eq 1 ]]; then
    echo -e "${YELLOW}Expected AST files updated: $UPDATED${RESET}"
    echo "Run the script again without --update-expected to compare."
else
    echo -e "Valid tests:   ${GREEN}$VALID_PASS passed${RESET}"
    echo -e "Invalid tests: ${GREEN}$INVALID_PASS passed${RESET}"
    echo
    echo -e "Total passed:  ${GREEN}$PASS${RESET}"
    echo -e "Total failed:  ${RED}$FAIL${RESET}"
    echo "Total tests:   $TOTAL"

    if [[ $FAIL -gt 0 ]]; then
        echo
        echo -e "${RED}Failed tests:${RESET}"
        for test in "${FAILED_TESTS[@]}"; do
            echo "  - $test"
        done
        echo
        echo -e "${RED}SOME PARSER TESTS FAILED${RESET}"
        exit 1
    fi

    echo
    echo -e "${GREEN}ALL PARSER TESTS PASSED!${RESET}"
fi

echo "============================================================"
echo

exit 0
