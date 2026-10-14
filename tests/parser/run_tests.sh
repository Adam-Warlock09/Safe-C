#!/usr/bin/env bash
#
# SafeC parser test suite (common C-like grammar).
#
# Two groups:
#
# 1. Valid programs (tests/parser/NN_*.safec):
#    must parse with exit code 0; the full AST dump must match the
#    golden file in expected/.
#
# 2. Invalid programs (tests/parser/eNN_*.safec):
#    must be rejected with exit code 1; stderr must contain the
#    expected message fragments listed below.
#
# (SafeC extension grammar - own/mut/raw/'?'/new/free/defer/trusted/
#  null/&mut - is another team member's part, so it is not tested here.)
#
# Run with:  make test-parser        (from the repository root)
#            bash tests/parser/run_tests.sh
#
# To regenerate golden files after an INTENTIONAL dump change:
#   bash tests/parser/run_tests.sh --update-expected

set -u
cd "$(dirname "$0")/../.."
PARSER=./safec-parser
DIR=tests/parser
UPDATE=0
[ "${1:-}" = "--update-expected" ] && UPDATE=1

pass=0
fail=0
failed_names=()

if [ ! -x "$PARSER" ]; then
    echo "error: $PARSER not built; run 'make' first" >&2
    exit 2
fi

# ---- valid programs -------------------------------------------------
for src in "$DIR"/[0-9][0-9]_*.safec; do
    name=$(basename "$src" .safec)
    golden="$DIR/expected/$name.txt"

    out=$("$PARSER" --ast "$src" 2>&1)
    status=$?

    if [ $status -ne 0 ]; then
        echo "FAIL $name: expected success, got exit $status"
        echo "$out" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names+=("$name"); continue
    fi
    if echo "$out" | grep -q "error:"; then
        echo "FAIL $name: parser printed an error but exited 0"
        echo "$out" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names+=("$name"); continue
    fi

    if [ "$UPDATE" = 1 ]; then
        printf '%s\n' "$out" > "$golden"
        echo "updated golden: $golden"
        pass=$((pass + 1)); continue
    fi

    if [ ! -f "$golden" ]; then
        echo "FAIL $name: missing golden file $golden (run with --update-expected)"
        fail=$((fail + 1)); failed_names+=("$name"); continue
    fi

    if printf '%s\n' "$out" | diff -u "$golden" - > /tmp/safec_test_diff_$$; then
        pass=$((pass + 1))
    else
        echo "FAIL $name: AST dump differs from $golden"
        head -15 /tmp/safec_test_diff_$$ | sed 's/^/    /'
        fail=$((fail + 1)); failed_names+=("$name")
    fi
    rm -f /tmp/safec_test_diff_$$
done

# ---- invalid programs ------------------------------------------------
# Each entry: filename  followed by the message fragments stderr must contain.
expect_error() {
    src="$DIR/$1"; shift
    name=$(basename "$src" .safec)
    err=$("$PARSER" "$src" 2>&1 >/dev/null)
    status=$?

    if [ $status -ne 1 ]; then
        echo "FAIL $name: expected exit 1 with errors, got exit $status"
        [ -n "$err" ] && echo "$err" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names+=("$name"); return
    fi
    for frag in "$@"; do
        if ! echo "$err" | grep -qF "$frag"; then
            echo "FAIL $name: stderr missing expected fragment: $frag"
            echo "$err" | sed 's/^/    /'
            fail=$((fail + 1)); failed_names+=("$name"); return
        fi
    done
    pass=$((pass + 1))
}

expect_error e01_missing_semicolon.safec ":3:" "expected ';' at the end of the declaration but got 'int'"
expect_error e02_missing_brace.safec     "missing '}' to close the block opened at line 1"
expect_error e03_incomplete_expr.safec    ":2:" ":3:" "expected an expression but got ';'"
expect_error e04_malformed_decl.safec     "expected an identifier but got '='" "but got identifier 'y'"
expect_error e05_unknown_type.safec       "unknown type name 'Foo'"
expect_error e06_unbalanced_parens.safec  "expected ')' but got ';'" "expected ')' after the if condition but got '{'"
expect_error e07_else_without_if.safec     "'else' without a matching 'if'"
expect_error e08_struct_no_semicolon.safec "expected ';' after the struct definition but got 'int'"
expect_error e09_duplicate_struct.safec   "duplicate struct definition: 'struct A' is already defined"
expect_error e10_bad_params.safec          "expected ')' to close the parameter list but got identifier 'b'"
expect_error e11_stray_case.safec          "'case' labels are only allowed inside a 'switch' body"
expect_error e12_unclosed_call.safec       "expected ')' to close the argument list but got ';'"

# ---------------------------------------------------------------------
echo
echo "parser tests: $pass passed, $fail failed (out of $((pass + fail)))"
if [ $fail -gt 0 ]; then
    printf 'failed: %s\n' "${failed_names[*]}"
    exit 1
fi
echo "ALL PARSER TESTS PASSED"
exit 0
