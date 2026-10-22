#!/usr/bin/env bash
#
# SafeC parser test suite.
#
# Valid programs (NN_*.safec): exit 0 and the --ast dump must match
# the golden file in expected/.
# Invalid programs (eNN_*.safec): exit 1 and a "syntax error" on
# stderr.  Only the first error is reported (no recovery rules yet),
# so that's all we check.
#
# Run with: bash tests/parser/run_tests.sh
#
# To regenerate goldens after an INTENTIONAL dump change:
#   bash tests/parser/run_tests.sh --update-expected
# (read them before committing - a golden only proves the dump is
# stable, not that it is right)

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

# ---- valid programs ---------------------------------------------------
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
        mkdir -p "$DIR/expected"
        printf '%s\n' "$out" > "$golden"
        echo "updated golden: $golden"
        pass=$((pass + 1)); continue
    fi

    if [ ! -f "$golden" ]; then
        echo "FAIL $name: missing golden file $golden (run with --update-expected)"
        fail=$((fail + 1)); failed_names+=("$name"); continue
    fi

    diffout=$(mktemp)
    if printf '%s\n' "$out" | diff -u "$golden" - > "$diffout"; then
        pass=$((pass + 1))
    else
        echo "FAIL $name: AST dump differs from $golden"
        head -15 "$diffout" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names+=("$name")
    fi
    rm -f "$diffout"
done

# ---- invalid programs -------------------------------------------------
for src in "$DIR"/e[0-9][0-9]_*.safec; do
    name=$(basename "$src" .safec)

    err=$("$PARSER" "$src" 2>&1 >/dev/null)
    status=$?

    if [ $status -ne 1 ]; then
        echo "FAIL $name: expected exit 1 with a syntax error, got exit $status"
        [ -n "$err" ] && echo "$err" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names+=("$name"); continue
    fi
    if ! echo "$err" | grep -q "syntax error"; then
        echo "FAIL $name: stderr missing 'syntax error'"
        echo "$err" | sed 's/^/    /'
        fail=$((fail + 1)); failed_names+=("$name"); continue
    fi
    pass=$((pass + 1))
done

# -----------------------------------------------------------------------
echo
echo "parser tests: $pass passed, $fail failed (out of $((pass + fail)))"
if [ $fail -gt 0 ]; then
    printf 'failed: %s\n' "${failed_names[*]}"
    exit 1
fi
echo "ALL PARSER TESTS PASSED"
exit 0
