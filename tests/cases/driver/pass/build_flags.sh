# -O0..-O3 and -g reach clang; -l/-L reach the linker (both spellings).
set -u
fail() { echo "$*"; exit 1; }

cat > prog.safec <<'EOF'
int main() { int s = 0; for (int i = 0; i < 10; i++) { s += i; } return s - 45; }
EOF

for flag in -O0 -O1 -O2 -O3 -g; do
    "$SAFEC" "$flag" prog.safec -o prog > /dev/null || fail "build with $flag failed"
    ./prog || fail "program built with $flag returned $?"
done

"$SAFEC" prog.safec -lm -o prog > /dev/null || fail "-lm was rejected"
"$SAFEC" prog.safec -l m -L . -o prog > /dev/null || fail "'-l m -L .' was rejected"

# A library that doesn't exist must make the link fail: proof -l is passed on
"$SAFEC" prog.safec -lsafec_no_such_library -o prog > out.txt 2>&1
rc=$?
[ $rc -eq 1 ] || fail "linking a missing library should exit 1, got $rc"
grep -q 'safec: clang failed' out.txt || fail "no 'clang failed' message: $(cat out.txt)"
exit 0
