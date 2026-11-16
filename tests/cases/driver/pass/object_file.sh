# -c compiles a SafeC file without main into an object file that a C
# program links against (whitepaper §4.6: incremental adoption).
set -u
fail() { echo "$*"; exit 1; }

cat > lib.safec <<'EOF'
int add(int a, int b) { return a + b; }
EOF
cat > main.c <<'EOF'
int add(int, int);
int main(void) { return add(40, 2) == 42 ? 0 : 1; }
EOF

"$SAFEC" -c lib.safec || fail "safec -c lib.safec failed"
[ -f lib.o ] || fail "-c without -o should write lib.o"
nm lib.o | grep -q ' T add$' || fail "lib.o doesn't define a global 'add': $(nm lib.o)"

"$SAFEC" -c lib.safec -o renamed.o || fail "safec -c -o renamed.o failed"
[ -f renamed.o ] || fail "-c -o renamed.o didn't write renamed.o"

clang main.c lib.o -o app || fail "a C program couldn't link lib.o"
./app || fail "the C program got a wrong result from add (exit $?)"

ls ./*.ll > /dev/null 2>&1 && fail "-c left LLVM IR behind: $(ls ./*.ll)"
exit 0
