#!/usr/bin/env bash
#
# tests/demo/run_demo.sh - runs the SafeC demos and shows what happens.
#
#   bash tests/demo/run_demo.sh            all demos
#   bash tests/demo/run_demo.sh 03 09      only demos 03 and 09
#
# Each NN_name.safec says in a "// demo:" line what it shows:
#   compile-error   the compiler rejects it: its errors are shown
#   run             it is built and run: its output is shown
#   trap            it is built and run, and a runtime check stops it
# If NN_name.c exists, the C file is part of the demo: linked into the SafeC
# program (SafeC has main), or the SafeC file is built with -c and linked into
# the C program (C has main).
# Exits with 0 if every demo behaved as its "// demo:" line says.

set -u
cd "$(dirname "$0")" || exit 2
SAFEC=../../safec
if ! make -s -C ../.. safec > /dev/null; then echo "building safec failed"; exit 2; fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if [ -t 1 ]; then BOLD=$'\033[1m'; GREEN=$'\033[32m'; RED=$'\033[31m'; DIM=$'\033[2m'; RESET=$'\033[0m'
else BOLD=""; GREEN=""; RED=""; DIM=""; RESET=""; fi

# run a program so its output appears line by line even if stdout is a pipe
# (otherwise output still buffered in printf is lost when a check stops it)
run() { if command -v stdbuf > /dev/null; then stdbuf -oL "$@"; else "$@"; fi; }
show() { echo "${DIM}\$ $*${RESET}"; }

files=()
if [ $# -eq 0 ]; then files=([0-9][0-9]_*.safec)
else for n in "$@"; do files+=("$n"_*.safec); done; fi

bad=0
for f in "${files[@]}"; do
    [ -f "$f" ] || { echo "no demo $f"; bad=$((bad + 1)); continue; }
    name=${f%.safec}
    kind=$(sed -nE 's#^// demo: *([a-z-]+).*#\1#p' "$f" | head -1)
    echo
    echo "${BOLD}=== $(head -1 "$f" | sed 's#^// ##')${RESET}"
    sed -n '2,/demo:/p' "$f" | grep -v 'demo:' | sed 's#^// \{0,1\}#    #'

    if [ "$kind" = compile-error ]; then
        show safec "$f"
        "$SAFEC" "$f" -o "$TMP/prog" 2>&1 | grep -v '^OK'
        if [ "${PIPESTATUS[0]}" -ne 0 ]; then echo "${GREEN}-> rejected at compile time${RESET}"
        else echo "${RED}-> compiled, but should have been rejected${RESET}"; bad=$((bad + 1)); fi
        continue
    fi

    # build: alone, with the C file linked in, or as a library for the C program
    if [ ! -f "$name.c" ]; then
        show safec "$f" -o "$name"
        "$SAFEC" "$f" -o "$TMP/prog" > "$TMP/out" 2>&1
    elif grep -q 'int main(' "$f"; then
        show safec "$f" "$name.c" -lm -o "$name"
        "$SAFEC" "$f" "$name.c" -lm -o "$TMP/prog" > "$TMP/out" 2>&1
    else
        show safec -c "$f" -o "$name.o"
        show clang "$name.c" "$name.o" -o "$name"
        "$SAFEC" -c "$f" -o "$TMP/lib.o" > "$TMP/out" 2>&1 && clang "$name.c" "$TMP/lib.o" -o "$TMP/prog" >> "$TMP/out" 2>&1
    fi
    if [ $? -ne 0 ]; then cat "$TMP/out"; echo "${RED}-> build failed${RESET}"; bad=$((bad + 1)); continue; fi

    show "./$name"
    run "$TMP/prog" 2>/dev/null
    status=$?
    if [ "$kind" = trap ]; then
        if [ $status -eq 132 ] || [ $status -eq 133 ]; then echo "${GREEN}-> stopped by a runtime check (exit $status)${RESET}"
        else echo "${RED}-> should have been stopped by a runtime check, exited with $status${RESET}"; bad=$((bad + 1)); fi
    else
        if [ $status -eq 0 ]; then echo "${GREEN}-> ran correctly${RESET}"
        else echo "${RED}-> exited with $status${RESET}"; bad=$((bad + 1)); fi
    fi
done

echo
if [ $bad -eq 0 ]; then echo "${BOLD}All ${#files[@]} demos behaved as described.${RESET}"
else echo "${BOLD}${RED}$bad demo(s) didn't behave as described.${RESET}"; fi
[ $bad -eq 0 ]
