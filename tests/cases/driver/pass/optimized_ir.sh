# --emit-llvm/-S with -O1..-O3 writes the IR after LLVM's optimizer; without
# -O (or with -O0) it is codegen's IR unchanged. Runtime checks the optimizer
# can prove never fail are removed; the others must stay.
set -u
fail() { echo "$*"; exit 1; }

cat > prog.safec <<'EOF'
// every index and sum here is known: all checks can be proven safe
int sum_known() {
    int a[8];
    for (int i = 0; i < 8; i++) { a[i] = i; }
    int s = 0;
    for (int i = 0; i < 8; i++) { s += a[i]; }
    return s;
}
// i comes from the caller: the bounds check must stay
int get(int i) {
    int a[4];
    a[0] = 1; a[1] = 2; a[2] = 3; a[3] = 4;
    return a[i];
}
int main() { return sum_known() + get(1) - 30; }
EOF
mkdir tmp
export TMPDIR="$PWD/tmp"     # the unoptimized IR goes to a temporary file

"$SAFEC" -S prog.safec -o plain.ll > /dev/null || fail "-S failed"
"$SAFEC" -S -O0 prog.safec -o o0.ll > /dev/null || fail "-S -O0 failed"
cmp -s plain.ll o0.ll || fail "-O0 should give codegen's IR unchanged"
grep -q 'alloca' plain.ll || fail "unoptimized IR should still have its stack slots"

"$SAFEC" -S -O2 prog.safec -o opt.ll > out.txt || fail "-S -O2 failed: $(cat out.txt)"
grep -q 'optimized with -O2' out.txt || fail "no 'optimized with -O2' message: $(cat out.txt)"
[ "$(wc -l < opt.ll)" -lt "$(wc -l < plain.ll)" ] || fail "-O2 IR isn't smaller than the unoptimized IR"
sed -n '/^define.*@sum_known/,/^}/p' opt.ll | grep -q 'ret i32 28' \
    || fail "sum_known should fold to 'ret i32 28' at -O2: $(sed -n '/^define.*@sum_known/,/^}/p' opt.ll)"
sed -n '/^define.*@get/,/^}/p' opt.ll | grep -q 'llvm.trap' \
    || fail "get's bounds check must survive -O2: $(sed -n '/^define.*@get/,/^}/p' opt.ll)"
clang -c opt.ll -o opt.o 2> clang.err || fail "the optimized IR doesn't compile: $(cat clang.err)"

"$SAFEC" -S -O2 -O0 prog.safec -o last.ll > /dev/null || fail "-O2 -O0 failed"
cmp -s plain.ll last.ll || fail "the last -O should win, as in clang: -O2 -O0 is unoptimized"
"$SAFEC" -S -O3 prog.safec > /dev/null || fail "-S -O3 failed"
[ -f prog.ll ] || fail "-S -O3 without -o should write prog.ll"

[ -z "$(ls tmp)" ] || fail "temporary files were not removed: $(ls tmp)"
exit 0
