#!/usr/bin/env bash
#
# tests/run_tests.sh - the one test runner for the SafeC compiler.
#
#   bash tests/run_tests.sh [--lexer | --parser | --sema | --codegen | --safec | --interop | --driver] [--only] [--verbose]
#
#   --lexer, --parser, ...  run every stage up to and including that one
#                           (no stage flag means --driver, which runs everything)
#   --only                  run just the chosen stage
#   --verbose, -v           list every test, not just the ones that fail
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
#   codegen  safec --emit-llvm, then LLVM's verifier (opt -passes=verify) on
#            the IR, with and without debug info (-g). pass: exit 0 and the IR verifies. fail: programs that
#            pass sema but codegen must reject; nonzero exit, and the
#            diagnostics match the markers, as in sema.
#   safec    the same programs, built into an executable with safec and run.
#            The exit code must match "// expect-exit: N" in the file (0 if
#            there's no such line). The programs return the number of the
#            first check that fails, so exit code 3 means check 3 failed.
#            "// expect-trap": the program must be stopped by a runtime check
#            (bounds, overflow, division by zero, ...) instead.
#            If there is a NAME.stdout, what the program prints must match it.
#   interop  C and SafeC together: pass/NAME.safec is built with safec -c and
#            linked with pass/NAME.c, which has main. The program's exit code
#            must match "// expect-exit: N" in the .c file (0 if none), and
#            its output NAME.stdout if there is one.
#   driver   the command line: tests/cases/driver/pass/NAME.sh, run with bash
#            in an empty temp folder with $SAFEC set to the compiler. Passes
#            if the script exits 0; what it printed is shown when it fails.
# In every stage a compiler crash counts as a failure, even for a fail test.
#
# tests/known_bugs.txt lists tests that fail today because of a known bug.
# They are counted as known bugs (listed with --verbose) and don't fail the run. A listed test that passes
# shows up as FIXED and does fail the run, so the list gets cleaned up as
# bugs are fixed.
#
# Exit status: 0 = no failures, 1 = something failed, 2 = usage or build error.
# Written for bash 3.2 too (the macOS default), so no associative arrays.

set -u

cd "$(dirname "$0")/.." || exit 2          # run from the repo root
ROOT=$(pwd)
COMPILER="./safec"
CASES="tests/cases"
KNOWN="tests/known_bugs.txt"
STAGES="lexer parser sema borrowck codegen safec interop driver"
RUN_TIMEOUT=10   # seconds a compiled test program may run

# the folder a stage's tests live in: safec runs the codegen programs
dir_of() {
    case "$1" in
        safec) echo codegen ;;
        *)     echo "$1" ;;
    esac
}

# the flag a stage belongs to: --sema covers both sema and borrowck
flag_of() {
    case "$1" in
        borrowck) echo sema ;;
        *)        echo "$1" ;;
    esac
}

usage() {
    sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//'
}

# ---- options

target="driver"
only=0
verbose=0
for arg in "$@"; do
    case "$arg" in
        --lexer|--parser|--sema|--codegen|--safec|--interop|--driver) target="${arg#--}" ;;
        --only)    only=1 ;;
        -v|--verbose) verbose=1 ;;
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
        match($0, /\/\/[ \t]*(error|warning)[ \t]*(:|--|$)/) {
            rest = substr($0, RSTART)
            kind = (rest ~ /^\/\/[ \t]*error/) ? "error" : "warning"
            sub(/^\/\/[ \t]*(error|warning)[ \t]*/, "", rest)
            if (rest ~ /^--/) rest = ""         # "// error -- comment": no text to match
            sub(/^:[ \t]*/, "", rest)
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

# The compiler writes NAME.ll next to the source, so codegen tests
# compile a copy in the temp folder to keep tests/cases clean.
case_codegen() {   # $1 = pass|fail, $2 = file
    cp "$2" "$TMP/prog.safec"
    run_compiler --emit-llvm "$TMP/prog.safec" || return 1
    if [ "$1" = fail ]; then
        expect_reject || return 1
        check_markers "$2"
        return
    fi
    expect_accept "$2" || return 1
    if ! command -v opt > /dev/null; then
        echo "'opt' not found: install LLVM to run the codegen tests" > "$TMP/why"
        return 1
    fi
    if ! opt -passes=verify -disable-output "$TMP/prog.ll" > "$TMP/verify" 2>&1; then
        { echo "the generated LLVM IR is invalid:"; head -5 "$TMP/verify"; } > "$TMP/why"
        return 1
    fi
    # the same program with debug info (-g): the verifier also checks its rules
    if ! "$COMPILER" -S -g "$TMP/prog.safec" -o "$TMP/prog_g.ll" > "$TMP/out" 2> "$TMP/err"; then
        { echo "safec -S -g failed:"; head -5 "$TMP/err"; } > "$TMP/why"
        return 1
    fi
    if ! opt -passes=verify -disable-output "$TMP/prog_g.ll" > "$TMP/verify" 2>&1; then
        { echo "the LLVM IR with debug info (-g) is invalid:"; head -5 "$TMP/verify"; } > "$TMP/why"
        return 1
    fi
    return 0
}

case_safec() {   # $1 = pass|fail, $2 = file
    [ "$1" = fail ] && { case_codegen fail "$2"; return; }   # rejected before anything is built
    cp "$2" "$TMP/prog.safec"
    rm -f "$TMP/prog"
    "$COMPILER" "$TMP/prog.safec" -o "$TMP/prog" > "$TMP/out" 2> "$TMP/err"
    RC=$?
    if [ $RC -ne 0 ] || [ ! -x "$TMP/prog" ]; then
        { echo "could not build the program (exit $RC):"; cat "$TMP/out" "$TMP/err" | grep -iE 'error|warning' | head -5; } > "$TMP/why"
        return 1
    fi
    run_built_program "$2" "${2%.safec}.stdout"
}

# Runs $TMP/prog and checks its exit code against "// expect-exit: N" in the
# file (0 if there's no such line). "// expect-trap" means the program must
# be stopped by a runtime check (llvm.trap: SIGILL on x86-64, SIGTRAP on arm64).
run_built_program() {   # $1 = the test file with the expect-exit/expect-trap marker, $2 = NAME.stdout
    local want trap=0
    want=$(sed -nE 's#^[[:space:]]*//[[:space:]]*expect-exit:[[:space:]]*([0-9]+).*#\1#p' "$1" | head -1)
    want=${want:-0}
    grep -qE '^[[:space:]]*//[[:space:]]*expect-trap' "$1" && trap=1

    if command -v timeout > /dev/null; then
        timeout "$RUN_TIMEOUT" "$TMP/prog" > "$TMP/out" 2> "$TMP/err"
    else
        "$TMP/prog" > "$TMP/out" 2> "$TMP/err"   # macOS has no timeout command
    fi
    local got=$?

    if [ $got -eq 124 ]; then
        echo "the program didn't finish within ${RUN_TIMEOUT}s (infinite loop?)" > "$TMP/why"
        return 1
    fi
    if [ $trap -eq 1 ]; then
        [ $got -eq 132 ] || [ $got -eq 133 ] && return 0     # SIGILL, SIGTRAP
        echo "the program should have been stopped by a runtime check, but exited with $got" > "$TMP/why"
        return 1
    fi
    if [ $got -ge 128 ]; then
        echo "the program crashed (exit $got, signal $((got - 128)))" > "$TMP/why"
        return 1
    fi
    if [ "$got" != "$want" ]; then
        if [ "$want" = 0 ]; then
            echo "the program returned $got: check $got failed" > "$TMP/why"
        else
            echo "the program exited with $got, expected $want" > "$TMP/why"
        fi
        return 1
    fi
    check_stdout "$2"
}

# What the program printed must match NAME.stdout, if there is one. (A
# separate name from NAME.expected, which the codegen stage compares with the
# compiler's own output.) Not checked for expect-trap programs: output still
# buffered in printf is lost when the trap stops the program.
check_stdout() {   # $1 = NAME.stdout
    [ -f "$1" ] || return 0
    if ! diff -u "$1" "$TMP/out" > "$TMP/diff"; then
        { echo "the program printed something else than $1:"; head -20 "$TMP/diff"; } > "$TMP/why"
        return 1
    fi
    return 0
}

# A SafeC file compiled with safec -c, linked with the C file of the same
# name (which has main) and run: checks SafeC follows the C calling convention.
case_interop() {   # $1 = pass, $2 = .safec file
    local c_file="${2%.safec}.c"
    if [ ! -f "$c_file" ]; then
        echo "missing $c_file: an interop test is a .safec and a .c file" > "$TMP/why"
        return 1
    fi
    cp "$2" "$TMP/lib.safec"
    rm -f "$TMP/lib.o" "$TMP/prog"
    "$COMPILER" -c "$TMP/lib.safec" -o "$TMP/lib.o" > "$TMP/out" 2> "$TMP/err"
    RC=$?
    if [ $RC -ge 128 ]; then
        { echo "compiler crashed (exit $RC)"; tail -5 "$TMP/err"; } > "$TMP/why"
        return 1
    fi
    if [ $RC -ne 0 ]; then
        { echo "safec -c failed (exit $RC):"; grep -iE 'error|warning' "$TMP/out" "$TMP/err" | head -5; } > "$TMP/why"
        return 1
    fi
    if ! clang "$c_file" "$TMP/lib.o" -o "$TMP/prog" > "$TMP/out" 2> "$TMP/err"; then
        { echo "could not link the C program with the SafeC object:"; head -5 "$TMP/err"; } > "$TMP/why"
        return 1
    fi
    run_built_program "$c_file" "${2%.safec}.stdout"
}

# Driver tests are shell scripts that run the compiler with different command
# lines. Each runs in its own empty folder, so it can create files freely.
case_driver() {   # $1 = pass, $2 = script
    local work="$TMP/driver"
    rm -rf "$work" && mkdir -p "$work"
    local script
    script="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
    if ! (cd "$work" && SAFEC="$ROOT/safec" bash "$script") > "$TMP/out" 2>&1; then
        { echo "the script failed. Its output:"; tail -15 "$TMP/out"; } > "$TMP/why"
        return 1
    fi
    return 0
}

# ---- running a stage

TOTAL_PASS=0; TOTAL_KNOWN=0; TOTAL_BAD=0

run_stage() {   # $1 = stage name
    local stage="$1" dir="$CASES/$(dir_of "$1")"
    local pass=0 known=0 bad=0

    [ "$verbose" -eq 1 ] && { echo; echo "${BOLD}== $stage ==${RESET}"; }

    if [ ! -d "$dir" ]; then
        printf '%-10s no tests yet\n' "$stage"
        return
    fi

    local kind file name id bug
    local ext=safec
    [ "$stage" = driver ] && ext=sh     # driver tests are scripts
    for kind in pass fail; do
        [ -d "$dir/$kind" ] || continue
        for file in "$dir/$kind"/*."$ext"; do
            [ -f "$file" ] || continue
            name=$(basename "$file" ."$ext")
            id="$stage/$kind/$name"
            bug=$(known_bug "$id")
            : > "$TMP/why"

            if "case_$stage" "$kind" "$file"; then
                if [ -n "$bug" ]; then
                    echo "  ${RED}FIXED${RESET}  $stage/$kind/$name  (listed as bug $bug, but it passes now: remove it from $KNOWN)"
                    bad=$((bad + 1))
                else
                    [ "$verbose" -eq 1 ] && echo "  ${GREEN}PASS${RESET}   $kind/$name"
                    pass=$((pass + 1))
                fi
            elif [ -n "$bug" ]; then
                [ "$verbose" -eq 1 ] && echo "  ${YELLOW}KNOWN${RESET}  $kind/$name  (bug $bug)"
                known=$((known + 1))
            else
                echo "  ${RED}FAIL${RESET}   $stage/$kind/$name"
                sed 's/^/           /' "$TMP/why"
                bad=$((bad + 1))
            fi
        done
    done

    printf '%-10s %s\n' "$stage" "$(counts $pass $known $bad)"
    TOTAL_PASS=$((TOTAL_PASS + pass)); TOTAL_KNOWN=$((TOTAL_KNOWN + known)); TOTAL_BAD=$((TOTAL_BAD + bad))
}

# "12 passed", plus known bugs and failures only when there are some
counts() {   # $1 = passed, $2 = known, $3 = failed
    local out="${GREEN}$1 passed${RESET}"
    [ "$2" -gt 0 ] && out="$out, ${YELLOW}$2 known bugs${RESET}"
    [ "$3" -gt 0 ] && out="$out, ${RED}$3 failed${RESET}"
    echo "$out"
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
echo "${BOLD}Total:${RESET} $(counts $TOTAL_PASS $TOTAL_KNOWN $TOTAL_BAD)"
[ "$TOTAL_BAD" -eq 0 ]
