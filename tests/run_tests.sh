#!/usr/bin/env bash
#
# tests/run_tests.sh - the one test runner for the SafeC compiler.
#
#   bash tests/run_tests.sh [--lexer | --parser | --sema | --codegen | --safec] [--only]
#
#   --lexer, --parser, ...  run every stage up to and including that one
#                           (no stage flag means --safec, which runs everything)
#   --only                  run just the chosen stage
#
# Each stage keeps its tests in tests/cases/<stage>/:
#   pass/NAME.safec       the compiler must accept this at that stage
#   pass/NAME.expected    optional: the stage's output must match it exactly
#   fail/NAME.safec       the compiler must reject this at that stage
#
# What each stage checks:
#   lexer    safec --till-lexer.  pass: exit 0 and the token list matches.
#            fail: nonzero exit.
#   parser   safec --till-parser. pass: exit 0 and the AST matches.
#            fail: nonzero exit with a "syntax error".
#   sema     safec --till-sema, run from both tests/cases/sema (type
#            checking) and tests/cases/borrowck (ownership and borrows).
#            pass: exit 0. fail: nonzero exit. Both: every diagnostic matches
#            a marker comment in the test file, and every marker is hit:
#              // error: <text>    an error on this line containing <text>
#              // warning: <text>  the same for a warning
#              // error            any error on this line
#            Case is ignored, and anything after " -- " is just a comment.
# In every stage a compiler crash counts as a failure, even for a fail test.
#
# tests/known_bugs.txt lists tests that fail today because of a known bug.
# They show up as KNOWN and don't fail the run. A listed test that passes
# shows up as FIXED and does fail the run, so the list gets cleaned up as
# bugs are fixed.
#
# Exit status: 0 = nothing unexpected, 1 = unexpected results, 2 = usage or build error.
# Written for bash 3.2 too (the macOS default), so no associative arrays.

set -u

cd "$(dirname "$0")/.." || exit 2          # run from the repo root
COMPILER="./safec"
CASES="tests/cases"
KNOWN="tests/known_bugs.txt"
STAGES="lexer parser sema borrowck codegen safec"

# the flag a stage belongs to: --sema covers both sema and borrowck
flag_of() {
    case "$1" in
        borrowck) echo sema ;;
        *)        echo "$1" ;;
    esac
}

usage() {
    sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'
}

# ---- options

target="safec"
only=0
for arg in "$@"; do
    case "$arg" in
        --lexer|--parser|--sema|--codegen|--safec) target="${arg#--}" ;;
        --only)    only=1 ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "run_tests.sh: unknown option '$arg'"; usage; exit 2 ;;
    esac
done

# ---- colours, only when printing to a terminal

if [ -t 1 ]; then
    GREEN=$'\033[32m'; RED=$'\033[31m'; YELLOW=$'\033[33m'; BOLD=$'\033[1m'; RESET=$'\033[0m'
else
    GREEN=""; RED=""; YELLOW=""; BOLD=""; RESET=""
fi

# ---- build first, so we never test a stale binary

BUILD_LOG=$(mktemp)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP" "$BUILD_LOG"' EXIT

if ! make -s safec > "$BUILD_LOG" 2>&1; then
    echo "${RED}build failed:${RESET}"
    cat "$BUILD_LOG"
    exit 2
fi

# ---- known bugs: "<stage>/<pass|fail>/<name>  <bug number>  <note>"

known_bug() {   # prints the bug number for a test id, or nothing
    [ -f "$KNOWN" ] || return 0
    awk -v id="$1" '$1 == id { print $2; exit }' "$KNOWN"
}

# ---- one test per stage
#
# Each case function runs one test. It returns 0 if the test behaved as
# expected and 1 if not, and when it fails it leaves the reason in $TMP/why.

# Runs the compiler up to a stage: stdout goes to $TMP/out, stderr to
# $TMP/err, and the exit code to $RC. A test fails outright if the compiler
# crashed (killed by a signal, exit code 128 and up), even a "fail" test:
# crashing is not the same as rejecting a program.
run_compiler() {   # $1 = stage flag, $2 = file
    "$COMPILER" "$1" "$2" > "$TMP/out" 2> "$TMP/err"
    RC=$?
    if [ $RC -ge 128 ]; then
        { echo "compiler crashed (exit $RC)"; tail -5 "$TMP/err"; } > "$TMP/why"
        return 1
    fi
    return 0
}

check_expected() {   # $1 = .safec path; compares stdout with NAME.expected if there is one
    local expected="${1%.safec}.expected"
    [ -f "$expected" ] || return 0
    if ! diff -u "$expected" "$TMP/out" > "$TMP/diff"; then
        { echo "output differs from $expected:"; head -20 "$TMP/diff"; } > "$TMP/why"
        return 1
    fi
    return 0
}

expect_accept() {   # $1 = file
    if [ $RC -ne 0 ]; then
        { echo "rejected (exit $RC), should have been accepted:"; cat "$TMP/out" "$TMP/err" | grep -i error | head -5; } > "$TMP/why"
        return 1
    fi
    check_expected "$1"
}

expect_reject() {   # $1 = text the error output must contain (optional)
    if [ $RC -eq 0 ]; then
        { echo "accepted (exit 0), should have been rejected. Output:"; cat "$TMP/out" "$TMP/err" | grep -i error | head -5; } > "$TMP/why"
        return 1
    fi
    if [ -n "${1:-}" ] && ! cat "$TMP/out" "$TMP/err" | grep -qi "$1"; then
        { echo "rejected, but not with a '$1'. Output:"; cat "$TMP/out" "$TMP/err" | head -5; } > "$TMP/why"
        return 1
    fi
    return 0
}

case_lexer() {   # $1 = pass|fail, $2 = file
    run_compiler --till-lexer "$2" || return 1
    if [ "$1" = pass ]; then expect_accept "$2"; else expect_reject; fi
}

case_parser() {   # $1 = pass|fail, $2 = file; expected output is the AST
    run_compiler --till-parser "$2" || return 1
    if [ "$1" = pass ]; then expect_accept "$2"; else expect_reject "syntax error"; fi
}

# Checks diagnostics against the markers in the test file:
#   // error: <text>      an error on this line whose message contains <text>
#   // warning: <text>    the same for a warning
#   // error              any error on this line (for bugs whose message
#                         doesn't exist yet)
# Matching ignores case, and anything after " -- " is a comment. Every
# marker needs a matching diagnostic, and every error or warning the
# compiler reports needs a marker on its line. Notes are ignored.
check_markers() {   # $1 = file
    awk '
        match($0, /\/\/[ \t]*(error|warning)(:|[ \t]*$)/) {
            rest = substr($0, RSTART)
            kind = (rest ~ /^\/\/[ \t]*error/) ? "error" : "warning"
            sub(/^\/\/[ \t]*(error|warning):?[ \t]*/, "", rest)
            i = index(rest, " -- "); if (i) rest = substr(rest, 1, i - 1)
            sub(/[ \t]+$/, "", rest)
            print kind "\t" NR "\t" rest
        }' "$1" > "$TMP/want"

    sed -nE 's/^[^:]*:([0-9]+)(:[0-9]+)?: (error|warning): (.*)$/\3	\1	\4/p' "$TMP/err" > "$TMP/got"

    awk -F'\t' '
        FILENAME == ARGV[1] { nw++; wk[nw] = $1; wl[nw] = $2; wt[nw] = tolower($3); next }
        { ng++; gk[ng] = $1; gl[ng] = $2; gm[ng] = $3 }
        END {
            bad = 0
            for (i = 1; i <= nw; i++) {
                ok = 0
                for (j = 1; j <= ng; j++)
                    if (gk[j] == wk[i] && gl[j] == wl[i] && (wt[i] == "" || index(tolower(gm[j]), wt[i]) > 0)) ok = 1
                if (!ok) { printf "missing %s on line %s: %s\n", wk[i], wl[i], (wt[i] == "" ? "(any)" : wt[i]); bad = 1 }
            }
            for (j = 1; j <= ng; j++) {
                ok = 0
                for (i = 1; i <= nw; i++) if (gk[j] == wk[i] && gl[j] == wl[i]) ok = 1
                if (!ok) { printf "unexpected %s on line %s: %s\n", gk[j], gl[j], gm[j]; bad = 1 }
            }
            exit bad
        }' "$TMP/want" "$TMP/got" > "$TMP/why"
}

case_sema() {   # $1 = pass|fail, $2 = file
    run_compiler --till-sema "$2" || return 1
    if [ "$1" = pass ]; then
        expect_accept "$2" || return 1
    else
        expect_reject || return 1
    fi
    check_markers "$2"
}

case_borrowck() { case_sema "$@"; }   # same command; the borrow checker runs at the end of sema

# ---- running a stage

TOTAL_PASS=0; TOTAL_KNOWN=0; TOTAL_BAD=0

run_stage() {   # $1 = stage name
    local stage="$1" dir="$CASES/$1"
    local pass=0 known=0 bad=0

    echo
    echo "${BOLD}== $stage ==${RESET}"

    if [ ! -d "$dir" ]; then
        echo "  (no tests yet: $dir does not exist)"
        return
    fi

    local kind file name id bug
    for kind in pass fail; do
        [ -d "$dir/$kind" ] || continue
        for file in "$dir/$kind"/*.safec; do
            [ -f "$file" ] || continue
            name=$(basename "$file" .safec)
            id="$stage/$kind/$name"
            bug=$(known_bug "$id")
            : > "$TMP/why"

            if "case_$stage" "$kind" "$file"; then
                if [ -n "$bug" ]; then
                    echo "  ${RED}FIXED${RESET}  $kind/$name  (listed as bug $bug, but it passes now: remove it from $KNOWN)"
                    bad=$((bad + 1))
                else
                    echo "  ${GREEN}PASS${RESET}   $kind/$name"
                    pass=$((pass + 1))
                fi
            elif [ -n "$bug" ]; then
                echo "  ${YELLOW}KNOWN${RESET}  $kind/$name  (bug $bug)"
                known=$((known + 1))
            else
                echo "  ${RED}FAIL${RESET}   $kind/$name"
                sed 's/^/           /' "$TMP/why"
                bad=$((bad + 1))
            fi
        done
    done

    echo "  $stage: ${GREEN}$pass passed${RESET}, ${YELLOW}$known known bugs${RESET}, ${RED}$bad unexpected${RESET}"
    TOTAL_PASS=$((TOTAL_PASS + pass)); TOTAL_KNOWN=$((TOTAL_KNOWN + known)); TOTAL_BAD=$((TOTAL_BAD + bad))
}

# ---- which stages to run

reached=0
for stage in $STAGES; do
    flag=$(flag_of "$stage")
    if [ "$flag" = "$target" ]; then
        reached=1
    elif [ $reached -eq 1 ]; then
        break                           # past the chosen stage
    fi
    if [ "$only" -eq 0 ] || [ "$flag" = "$target" ]; then
        run_stage "$stage"
    fi
done

echo
echo "${BOLD}Total:${RESET} ${GREEN}$TOTAL_PASS passed${RESET}, ${YELLOW}$TOTAL_KNOWN known bugs${RESET}, ${RED}$TOTAL_BAD unexpected${RESET}"
[ "$TOTAL_BAD" -eq 0 ]
