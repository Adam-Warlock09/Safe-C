# Where LLVM IR goes: --emit-llvm/-S write it next to the source or to -o.
# Full builds and -c put it in a temporary file and delete it. The IR
# names its target, so clang compiles it without an override warning.
set -u
fail() { echo "$*"; exit 1; }

cat > prog.safec <<'EOF'
int main() { return 0; }
EOF
mkdir tmp
export TMPDIR="$PWD/tmp"     # see that the temporary IR is cleaned up

"$SAFEC" --emit-llvm prog.safec > /dev/null || fail "--emit-llvm failed"
[ -f prog.ll ] || fail "--emit-llvm should write prog.ll"
grep -q '^target triple = ' prog.ll || fail "the IR has no target triple"
grep -q '^target datalayout = ' prog.ll || fail "the IR has no target datalayout"

clang -c prog.ll -o prog.o 2> clang.err || fail "clang couldn't compile the IR: $(cat clang.err)"
[ -s clang.err ] && fail "clang warned about the IR: $(cat clang.err)"

"$SAFEC" -S prog.safec -o out.ll > /dev/null || fail "-S -o out.ll failed"
[ -f out.ll ] || fail "-S -o out.ll didn't write out.ll"

rm -f ./*.ll
"$SAFEC" prog.safec -o prog > /dev/null || fail "full build failed"
"$SAFEC" -c prog.safec > /dev/null || fail "-c failed"
ls ./*.ll > /dev/null 2>&1 && fail "a build left IR next to the source: $(ls ./*.ll)"
[ -z "$(ls tmp)" ] || fail "temporary files were not removed: $(ls tmp)"
exit 0
