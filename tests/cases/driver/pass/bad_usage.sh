# Bad command lines exit with 2 and say what was wrong.
set -u
fail() { echo "$*"; exit 1; }

cat > a.safec <<'EOF'
int main() { return 0; }
EOF
cp a.safec b.safec
echo 'int helper(void) { return 1; }' > helper.c

expect_usage() {   # $1 = what the message must mention, rest = arguments
    local want="$1"; shift
    "$SAFEC" "$@" > out.txt 2>&1
    local rc=$?
    [ $rc -eq 2 ] || fail "safec $* should exit 2, got $rc: $(cat out.txt)"
    grep -qi -- "$want" out.txt || fail "safec $* should mention '$want': $(cat out.txt)"
}

expect_usage "unknown option"  --frobnicate a.safec
expect_usage "only one SafeC"  a.safec b.safec
expect_usage "no SafeC source" -O2
expect_usage "-o needs"        a.safec -o
expect_usage "-c can't"        -c --emit-llvm a.safec
expect_usage "-c compiles only" -c a.safec helper.c
exit 0
