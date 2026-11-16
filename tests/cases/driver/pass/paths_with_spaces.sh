# File names are passed to clang as they are, so spaces in paths work.
set -u
fail() { echo "$*"; exit 1; }

mkdir "my dir"
cat > "my dir/my prog.safec" <<'EOF'
int main() { return 7; }
EOF

"$SAFEC" "my dir/my prog.safec" -o "my dir/my app" > /dev/null || fail "building a path with spaces failed"
"./my dir/my app"
[ $? -eq 7 ] || fail "the program built from 'my dir' returned the wrong exit code"

"$SAFEC" -c "my dir/my prog.safec" > /dev/null || fail "-c on a path with spaces failed"
[ -f "my dir/my prog.o" ] || fail "-c should write 'my dir/my prog.o'"
exit 0
