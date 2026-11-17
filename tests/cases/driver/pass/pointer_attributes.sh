# What the type system proves about safe pointers reaches LLVM as parameter
# attributes (codegen_abi.cpp, pointer_attrs): borrows are nonnull and
# dereferenceable, write borrows and owners are noalias, nullable pointers
# are dereferenceable_or_null, raw pointers get nothing. Read borrows must
# NOT be readonly or noalias: trusted code may write through them.
set -u
fail() { echo "$*"; exit 1; }

cat > prog.safec <<'EOF'
struct Pair { int a; int b; };
int read_borrow(struct Pair* p) { return p->a; }
void write_borrow(mut struct Pair* p) { p->a = 1; }
void owner(own struct Pair* p) { free(p); }
int nullable_borrow(struct Pair*? p) { return 0; }
void nullable_owner(own struct Pair*? p) { if (p != null) { free(p); } }
int raw_pointer(raw struct Pair* p) { return 0; }
own struct Pair* make() { return new struct Pair; }
own struct Pair*? maybe() { return null; }
int written_in_trusted(int* p) { trusted { *p = 2; } return 0; }
extern void c_fill(mut int* out);
int main() { return 0; }
EOF
"$SAFEC" -S prog.safec -o prog.ll > /dev/null || fail "safec -S failed"

expect() {   # $1 = function name, $2 = text its define/declare line must contain
    local line
    line=$(grep -E "^(define|declare) .*@$1\(" prog.ll)
    [ -n "$line" ] || fail "no define/declare for $1"
    case "$line" in *"$2"*) ;; *) fail "$1 should have '$2': $line" ;; esac
}
reject() {   # $1 = function name, $2 = text its line must not contain
    local line
    line=$(grep -E "^(define|declare) .*@$1\(" prog.ll)
    case "$line" in *"$2"*) fail "$1 must not have '$2': $line" ;; esac
}

expect read_borrow      "ptr nonnull dereferenceable(8) %p.arg"
reject read_borrow      "noalias"
reject read_borrow      "readonly"
expect write_borrow     "ptr nonnull dereferenceable(8) noalias %p.arg"
expect owner            "ptr nonnull noalias %p.arg"
reject owner            "dereferenceable"          # the callee may free it
expect nullable_borrow  "ptr dereferenceable_or_null(8) %p.arg"
expect nullable_owner   "ptr noalias %p.arg"
reject nullable_owner   "nonnull"
expect raw_pointer      "(ptr %p.arg)"
expect make             "define nonnull ptr @make"
expect maybe            "define ptr @maybe"
reject written_in_trusted "readonly"
expect c_fill           "declare void @c_fill(ptr nonnull dereferenceable(4) noalias)"
expect printf           "declare i32 @printf(ptr nonnull dereferenceable(1), ...)"
exit 0
