# -g: DWARF debug info, so gdb and lldb show SafeC source lines, functions,
# variables (shadowed ones too), globals, structs and arrays (whitepaper
# §5.1). Checks the DWARF is valid, and with gdb (skipped if not installed)
# that a debugging session shows the right things.
set -u
fail() { echo "$*"; exit 1; }

cat > prog.safec <<'EOF'
int counter = 41;
struct Pair { int a; char tag; };

int work(int x, mut struct Pair* out) {
    int depth = 1;
    bool flag = true;
    float ratio = 2.5;
    int nums[3];
    nums[0] = 7; nums[1] = 8; nums[2] = 9;
    {
        int depth = 2;
        counter = counter + depth;
    }
    out->a = x + depth + nums[1];
    return out->a;
}

int main() {
    struct Pair p = (struct Pair){ 0, 'z' };
    return work(1, &mut p) - 10;
}
EOF

"$SAFEC" -S prog.safec -o plain.ll > /dev/null || fail "safec -S failed"
grep -q 'DICompileUnit' plain.ll && fail "debug info emitted without -g"

"$SAFEC" -g prog.safec -o prog > /dev/null || fail "safec -g failed"
./prog || fail "the -g build returned $?"
command -v llvm-dwarfdump > /dev/null || fail "llvm-dwarfdump not found: install LLVM"
llvm-dwarfdump --verify prog > dwarf.txt 2>&1 || fail "invalid DWARF: $(grep -i error dwarf.txt | head -3)"
llvm-dwarfdump --debug-info prog > info.txt
for name in '"work"' '"main"' '"depth"' '"counter"' '"Pair"' '"nums"' '"prog.safec"'; do
    grep -q "DW_AT_name.*$name" info.txt || fail "no $name in the DWARF"
done

if ! command -v gdb > /dev/null; then
    echo "gdb not found: skipped the gdb session"
    exit 0
fi
gdb -batch -nx -ex 'set debuginfod enabled off' \
    -ex 'break work' -ex run -ex bt -ex 'print x' -ex 'print *out' \
    -ex 'break prog.safec:12' -ex continue -ex 'print depth' \
    -ex 'break prog.safec:14' -ex continue -ex 'print depth' -ex 'print counter' \
    -ex 'print nums' -ex 'print flag' -ex 'print ratio' -ex 'next' -ex 'print *out' \
    -ex kill ./prog > gdb.txt 2>&1
expect() { grep -q -- "$1" gdb.txt || fail "gdb should show '$1'. Session: $(cat gdb.txt)"; }
expect 'Breakpoint 1, work (x=1, out=0x'        # stops after the parameters are stored
expect 'at prog.safec:5'                        # at the function's first statement
expect 'in main () at prog.safec:20'            # backtrace: the caller and its line
expect '= {a = 0, tag = 122 '"'"'z'"'"'}'      # a struct through a pointer
expect '\$1 = 1'                                # print x
expect '\$3 = 2'                                # the inner depth, inside its block
expect '\$4 = 1'                                # the outer depth again after the block
expect '\$5 = 43'                               # the global
expect '\$6 = {7, 8, 9}'                        # an array
expect '\$7 = true'
expect '\$8 = 2.5'
expect '{a = 10, tag = 122'                     # after stepping over the store
exit 0
