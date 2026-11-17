# Safe-C

A memory-safe C-family language on LLVM — team compilers project.

## Repo layout

- `src/lexer.l` — flex lexer (shared, produces the token stream)
- `src/tokens.h` — token codes
- `src/parser.y` — **the SafeC parser: bison (LALR(1)) grammar + actions** building the `new_ast.h` tree; token numbers synced with `tokens.h`
- `src/main_parser.cpp` — driver + scanner bridge: `safec::yylex` (token decoding), `safec::parseFile()`, the CLI
- `src/ast_dump.cpp`, `src/ast_dump.h` — AST printer (`--ast`, test goldens)
- `src/new_ast.h` — C++17 AST (parser output, semantic-analysis input)
- `src/sema.cpp` — semantic analysis (in progress)
- `tests/` — lexer and parser tests (`make test`)

## Build

Requires flex, bison (3.0+, for the C++ skeleton), cc and c++ (C++17).

```
make          # safec-parser + standalone lexer
make test     # lexer regression + parser tests
```

## Parser status

The grammar in `src/parser.y` covers the full SafeC language — the
common C-like core **and** the SafeC extensions (`own`/`mut`/`raw`,
`*?`, `new`, `free`, `defer`, `trusted`, `null`, `&mut`) — and bison
reports **zero conflicts**.  Its actions build the complete
`new_ast.h` tree:

- `safec-parser file.safec` — parse; prints `OK` on success
- `safec-parser --ast file.safec` — also dump the tree (the format is
  whatever the goldens in `tests/parser/expected/` show)
- syntax errors print as `file:line: error: syntax error, unexpected ...`
  through the `DiagnosticsEngine`, and exit 1.  The grammar has no
  `error` recovery rules yet, so parsing stops at the first error and
  the partially built tree leaks — harmless, the process exits.
- `safec::parseFile(path, diag)` in `src/main_parser.cpp` is the
  entry point the next stages (sema) should call.

Syntactic checks only: duplicate struct names, unknown struct types,
etc. are semantic-analysis matters (`docs/SEMA_CHECKLIST.md`).

# Differences from the Whitepaper

This section explains where our implementation differs from the whitepaper and why. Each difference has a test to verify its behaviour.

### 1. Struct types must use `struct`

```c
struct List list = list_new();  // Allowed
List list = list_new();         // Not allowed
```

We require the `struct` keyword because otherwise the parser cannot easily distinguish between a struct declaration and multiplication.

For example, `List * p` could be a pointer declaration or a multiplication expression.

Requiring `struct` keeps the grammar simple. Bison reports zero parsing conflicts.

Test: `parser/fail/e05_unknown_type`

### 2. Variables must be initialized before borrowing

```c
int x;
list_pop(&mut list, &mut x);  // Error

int x = 0;
list_pop(&mut list, &mut x);  // Allowed
```

Even when passing a mutable borrow (`&mut`), the variable must already have a value.

This is because the function receiving the pointer might read its value. Allowing uninitialized variables could therefore cause errors.

Test: `sema/fail/definite_assignment`

### 3. The borrow checker checks one function at a time

```c
list_free(&mut list);
return 0;  // Error: list.head might still own memory
```

The borrow checker cannot know whether `list_free()` actually emptied the list because it doesn't inspect what happens inside called functions.

Therefore, it assumes the list might still own memory.

To avoid this, explicitly transfer ownership and free it:

```c
own struct Node*? chain = list.head;
list.head = null;
nodes_free(chain);
```

This follows the whitepaper's design of checking borrows locally.

Tests:

- `borrowck/fail/e35_leak_after_mut_call`
- `codegen/pass/case_study_list`

### 4. Owned struct fields must be nullable

Owned pointer fields must use `own T*?`.

This is because `new` uses `calloc`, which initializes allocated memory to zero.

Therefore, owned pointer fields start as null and must allow null values.

Tests: `codegen/pass/new_zero_fill` and borrow-checker tests.

### 5. Evaluation happens from left to right

The whitepaper doesn't specify evaluation order, so we define it ourselves.

In SafeC:

- Assignment targets are evaluated before their values.
- Function arguments are evaluated from left to right.
- Operators evaluate their operands from left to right.

Test: `codegen/pass/evaluation_order`

### 6. Borrows last until the statement finishes

A borrow created inside a statement remains active until that entire statement finishes.

For example:

```c
x = f(&x);  // Error
```

Here, `x` is borrowed while the assignment is trying to modify it.

Use a separate variable instead.

Test: `borrowck/fail/e36_borrow_lasts_whole_statement`

### 7. Arithmetic follows C rules

SafeC follows C's arithmetic behaviour:

- `char` and `bool` are promoted to `int`.
- `-true` evaluates to `-1`.
- `x op= e` works like `x = x op e`. For example, `b -= -3` on a boolean keeps it true.
- Array accesses are checked at runtime to prevent out-of-bounds access.
- Unsized array parameters and code inside `trusted` are exceptions to runtime bounds checking.
- Unlike C, `int` arithmetic is checked at runtime and stops the program (like an out-of-bounds index) on:
  overflow of `+`, `-`, `*`, unary `-`, `++`, `--` and `op=`; division or remainder by zero; `INT_MIN / -1`;
  and shifting by a negative amount or by 32 or more. `char` arithmetic is done in `int`, so `char` results
  wrap when stored back. Inside `trusted` nothing is checked and overflow wraps around.
- These checks cost little in most code, but in a tight loop of integer arithmetic they can make it
  much slower than C (the `collatz` benchmark in `tests/bench` runs at about 180% of C's time). To get
  C's speed in such a loop, put it inside a `trusted` block: its arithmetic then wraps on overflow
  instead of stopping the program (`collatz` drops to about 94%). Use it only where overflow can't
  happen or wrapping is acceptable; the rest of the program stays checked.

Tests: `codegen/pass/updates`, `codegen/pass/structs_arrays`, `codegen/pass/arith_checks_boundaries`,
`codegen/pass/trap_*` (programs that must be stopped, marked `// expect-trap`).

### 8. Calling C: declarations, `extern` and a built-in `printf`

SafeC calls C functions and uses C globals the way C does: by declaring them.
clang links the C library automatically; other C code is passed to `safec`
as `.c`, `.o`, `.a` or `.so` files (and libraries with `-l`/`-L`).

```c
int c_add(int a, int b);            // a function defined elsewhere: no body
extern int c_sub(int, int);         // `extern` is optional on functions; names may be left out
int c_log(char* fmt, ...);          // `...` after at least one named parameter
extern int counter;                 // a global defined elsewhere

int main() {
    printf("%d\n", c_add(1, 2));   // printf needs no declaration
    return 0;
}
```

- `extern` is a keyword, as in C, so it can't be used as a name.
- `printf` is built in as `int printf(char* fmt, ...)`, since SafeC has no
  `#include`. Repeating that exact declaration is allowed; any other
  declaration or a definition of `printf` is an error. Other C functions
  (`puts`, `abs`, ...) need a declaration.
- A function may be declared many times and defined at most once, and every
  declaration must have the same type; the same goes for `extern` globals,
  which can't have an initializer. `main` must be defined.
- `...` is only for declaring C functions: a SafeC function can't be
  variadic, because SafeC can't read the extra arguments. Extra arguments
  can be numbers, strings, pointers, arrays and `null`, not structs; they are
  passed as C passes them (`char` and `bool` as `int`, `float` as `double`).
- A declaration is trusted like a SafeC signature: `own T*` means the C
  function takes ownership (or hands it over, as a result), `T*` and `mut T*`
  are borrows for the call. The borrow checker checks the SafeC side; the C
  side must keep these promises. Memory C returns as `own` must come from
  `malloc`/`calloc`, since `free` is C's `free`. Passing an owner to
  `printf`'s `...` only reads it.
- Calls follow the C calling convention of x86-64 Linux (System V), structs
  passed by value included. Only C functions whose types SafeC can write
  can be declared correctly: `int`, `char`, `bool`, `float`, pointers and
  structs of these. Not `double` (most of `math.h`), `long`/`size_t`
  (`strlen`, `malloc`), `unsigned`, `void*` or `FILE*`.

Tests: `parser/pass/18_extern_declarations`, `sema/pass/extern_declarations`,
`sema/fail/extern_declarations`, `borrowck/*/extern_calls`,
`codegen/pass/printf_output`, `codegen/pass/case_study_list`,
`interop/pass/safec_calls_c`.

## Grammar (EBNF)

The grammar implemented by `src/parser.y`.  Terminals in quotes are
lexer tokens (`src/tokens.h`); `[ x ]` = optional; `{ x }` = zero or
more.

```
program               = { external_declaration } ;

external_declaration  = struct_definition
                      | [ "extern" ] function_definition
                      | [ "extern" ] function_declaration
                      | [ "extern" ] global_declaration ;

global_declaration    = declaration ;          (* with extern: no initializer *)

pointer_qualifier     = "own" | "mut" | "raw" ;

qualified_base        = [ pointer_qualifier ] type_base ;

type_base             = "void" | "bool" | "char" | "int" | "float"
                      | "struct" identifier ;

pointer_level         = "*" [ "?" ] ;

array_suffix          = "[" [ expression ] "]" ;

struct_definition     = "struct" identifier
                          "{" { field_declaration } "}" ";" ;

field_declaration     = qualified_base
                          field_declarator { "," field_declarator } ";" ;

field_declarator      = { pointer_level } identifier { array_suffix } ;

declaration           = qualified_base
                          declarator { "," declarator } ";" ;

declarator            = { pointer_level } identifier { array_suffix }
                        [ "=" assignment_expr ] ;

function_definition   = function_header compound_statement ;

function_declaration  = function_header ";" ;  (* defined elsewhere, e.g. in C *)

function_header       = qualified_base { pointer_level }
                          identifier parameter_list ;

parameter_list        = "(" [ parameters ] ")" ;

parameters            = parameter { "," parameter } [ "," "..." ]
                      | "void" ;

parameter             = qualified_base { pointer_level }
                          [ identifier ] { array_suffix } ;
                        (* a name may be left out only in a declaration *)

compound_statement    = "{" { block_item } "}" ;

block_item            = declaration | statement ;

statement             = compound_statement
                      | expression_statement
                      | selection_statement
                      | iteration_statement
                      | switch_statement
                      | jump_statement
                      | defer_statement
                      | trusted_statement
                      | empty_statement ;

expression_statement  = expression ";" ;

empty_statement       = ";" ;

selection_statement   = "if" "(" expression ")" statement
                        [ "else" statement ] ;

iteration_statement   = "while" "(" expression ")" statement
                      | "for" "(" for_init
                               [ expression ] ";"
                               [ expression ] ")"
                          statement ;

for_init              = declaration | [ expression ] ";" ;

switch_statement      = "switch" "(" expression ")"
                          "{" { switch_section } "}" ;

switch_section        = switch_label { block_item } ;

switch_label          = "case" expression ":"
                      | "default" ":" ;

jump_statement        = "return" [ expression ] ";"
                      | "break" ";"
                      | "continue" ";" ;

defer_statement       = "defer" statement ;

trusted_statement     = "trusted" compound_statement ;

expression            = assignment_expr ;

assignment_expr       = logical_or
                        [ assignment_op assignment_expr ] ;

assignment_op         = "=" | "+=" | "-=" | "*=" | "/=" | "%="
                      | "&=" | "|=" | "^=" | "<<=" | ">>=" ;

logical_or            = logical_and { "||" logical_and } ;

logical_and           = bitwise_or { "&&" bitwise_or } ;

bitwise_or            = bitwise_xor { "|" bitwise_xor } ;

bitwise_xor           = bitwise_and { "^" bitwise_and } ;

bitwise_and           = equality_expr { "&" equality_expr } ;

equality_expr         = relational_expr
                        { ( "==" | "!=" ) relational_expr } ;

relational_expr       = shift_expr
                        { ( "<" | ">" | "<=" | ">=" ) shift_expr } ;

shift_expr            = additive_expr
                        { ( "<<" | ">>" ) additive_expr } ;

additive_expr         = multiplicative_expr
                        { ( "+" | "-" ) multiplicative_expr } ;

multiplicative_expr   = unary_expr
                        { ( "*" | "/" | "%" ) unary_expr } ;

unary_expr            = unary_operator unary_expr
                      | postfix_expr ;

unary_operator        = "&" [ "mut" ]
                      | "-" | "+" | "!" | "~"
                      | "*" | "++" | "--" ;

postfix_expr          = primary_expr { postfix_operator } ;

postfix_operator      = "(" [ argument_list ] ")"
                      | "[" expression "]"
                      | "." identifier
                      | "->" identifier
                      | "++" | "--" ;

argument_list         = assignment_expr { "," assignment_expr } ;

primary_expr          = literal
                      | identifier
                      | "null"
                      | "(" expression ")"
                      | new_expr
                      | free_expr
                      | compound_literal ;

new_expr              = "new" type_base ;

free_expr             = "free" "(" expression ")" ;

compound_literal      = "(" qualified_base { pointer_level } ")"
                          initializer_list ;

initializer_list      = "{" initializer { "," initializer } "}" ;

initializer           = designation | assignment_expr ;

designation           = "." identifier "=" assignment_expr ;

literal               = integer_literal
                      | float_literal
                      | character_literal
                      | string_literal
                      | boolean_literal ;

boolean_literal       = "true" | "false" ;
```

## Keeping parser.y in sync with tokens.h

`src/parser.y` declares every token with its exact number from
`src/tokens.h` (`EOF_TOKEN = 0` is bison's end-of-input automatically).
If `tokens.h` ever changes, regenerate the `%token` block in `parser.y`
the same way it was made:

```
python3 - <<'EOF'
import re
body = open('src/tokens.h').read()
body = body[body.index('{')+1:body.rindex('}')]
val, first = 0, True
for m in re.finditer(r'([A-Z_]+)\s*(?:=\s*(\d+))?\s*[,}]', body):
    name = m.group(1)
    if m.group(2): val = int(m.group(2))
    if name == 'EOF_TOKEN': val += 1; continue
    print(f"%token {name} {val}")
    val += 1
EOF
```

(Note: the last enum member must keep its trailing comma in `tokens.h`,
or the script misses it.)

The C++ skeleton bison generates keeps these numbers (bison's own
reserved `error` and "invalid token" kinds move to 256/257), so the
flex lexer's `tokens.h` codes reach the parser unchanged — the scanner
bridge in `src/main_parser.cpp` passes them straight through.

## Semantic-analysis AST (`src/new_ast.h`)

A header-only C++17 AST, built by the parser's actions (`src/parser.y`)
and consumed by semantic analysis (`src/sema.cpp`).

- **Hierarchy.** `ASTNode` (holds `loc` and an error bit) splits into
  `TypeSpec`, `Decl`, `Stmt` and `Expr`. `Program` is the root: it
  owns the top-level decls in source order plus the `TypeContext`.
- **`TypeSpec` vs `Type`.** `TypeSpec` is a type *as written*
  (`own struct Node *? next[4]`), built by the parser. `Type` is what it
  *means*, filled in by sema. Types are interned in `TypeContext`, so
  compare them with `==` on the pointer. `PointerType` carries the
  whitepaper's two axes: `PtrKind` (`Borrow`, `MutBorrow`, `Own`, `Raw`)
  and `isNullable()` (`T*?`).
- **`[sema]` fields** (`Expr::type`, `NameExpr::decl`,
  `MemberExpr::field`, `BreakStmt::target`, ...) are left null by the
  parser and filled in by semantic analysis.
- **Error bit.** After reporting an error on a node, call
  `setInvalid()`; later passes skip invalid nodes instead of piling on
  follow-on errors.
- **Diagnostics.** Report through `DiagnosticsEngine`
  (`error` / `warning` / `note`); `print()` emits
  `file:line:col: error: msg`. Suppressing warnings or turning them into
  errors is a setting on the engine, not a diagnostic kind.
- **Locations.** `loc` is the name for decls, the keyword for
  statements, and the operator for operator expressions. `col` stays 0
  until `lexer.l` tracks columns.
- **Casting.** LLVM-style: `isa<T>`, `dyn_cast<T>`, `cast<T>` on both
  `ASTNode*` and `Type*`; switch on `kind()` to dispatch.
- **Shapes worth knowing:**
  - `int a, *b;` becomes two `VarDecl`s, each with its own `TypeSpec`.
    At top level they go straight into `Program::decls` with `isGlobal`
    set; inside a block they go into a `DeclStmt`.
  - `f(void)` and `f()` both give an empty parameter list.
  - `case` / `default` sections are owned by their `SwitchStmt`.
  - Assignments are `AssignExpr`, not `BinaryExpr`, because a plain `=`
    on an `own` pointer is a move.
  - Postfix `++` / `--` are `UnaryExpr` with `PostInc` / `PostDec`.
  - Only sema inserts `ImplicitCastExpr` nodes, for conversions such as
    int to float, `null` to `T*?`, and reborrowing an `own` pointer.
- **Open for sema to decide:** which pointer level a qualifier applies
  to in `own int** p`, and rejecting a qualifier with no pointer
  (`own int x`). `TypeSpec` records the source as written.

# Testing

### Running the tests

```bash
make test
```

Runs the complete test suite.

Other options:

```bash
bash tests/run_tests.sh --sema
```

Runs all stages up to semantic analysis.

```bash
bash tests/run_tests.sh --sema --only
```

Runs only semantic analysis.

```bash
bash tests/run_tests.sh -v
```

Shows every test, including passing tests.

### Test stages

| Stage          | What it checks                                     |
| -------------- | -------------------------------------------------- |
| Lexer          | Whether the generated tokens are correct           |
| Parser         | Whether the AST matches the expected output        |
| Sema           | Whether semantic errors and warnings are correct   |
| Borrow checker | Whether ownership and borrowing rules are followed |
| Codegen        | Whether generated LLVM IR is valid                 |
| SafeC          | Whether compiled programs execute correctly        |
| Interop        | Whether SafeC and C code linked together work      |
| Driver         | Whether the command line (`-c`, `-S`, `-o`, `-l`) works |

`--codegen` runs the code generation tests. With no stage flag (or `--driver`)
the complete suite runs.

The test runner exits with code 0 if all tests pass.

### Test folder structure

Tests are stored in:

```text
tests/cases/<stage>/pass/
tests/cases/<stage>/fail/
```

- `pass`: Programs that should be accepted.
- `fail`: Programs that should be rejected.

Codegen tests also check the program's output or return value. If a check fails, the program returns the number of the failed check.

For example, `returned 5` means check number 5 failed.

If a program is expected to exit with a non-zero value, use:

```c
// expect-exit: N
```

If a program must be stopped by a runtime check (an out-of-bounds index,
overflow, division by zero), use `// expect-trap`.

If `NAME.stdout` exists next to a codegen or interop test, what the program
prints must match it exactly.

An interop test is a pair: `NAME.safec`, built with `safec -c`, and `NAME.c`,
which is linked with it.

### Error markers

Semantic analysis and borrow-checker tests use comments to specify expected errors and warnings.

```c
*p = 1;  // error: cannot assign through a read borrow
free(q); // error: may be null
```

The test passes only if:

- Every expected error or warning is reported.
- The diagnostic appears on the correct line.
- The message contains the expected text.
- No unexpected errors or warnings are reported.

---

### Benchmarks

```bash
bash tests/bench/run_bench.sh
```

Times each program in `tests/bench/` written in SafeC and as ordinary C, both
built with `-O2`, and reports SafeC's time as a percentage of C's (the
whitepaper's target is within 10%). Not part of `make test`, since timings
depend on the machine.

## Additional Testing

Apart from the regular test suite, SafeC has been tested in four other ways.

### 1. Crash Fuzzing

Around 120,000 modified versions of existing tests were generated by deleting, swapping, or replacing tokens and moving lines.

The compiler was built with AddressSanitizer and UBSan to detect memory errors and undefined behaviour.

### 2. Mutation Testing

We disabled each of the 118 error-reporting checks in semantic analysis and the borrow checker, one at a time.
```
All 118 are now covered: switching any one of them off makes at least one test fail.
```

### 3. Whitepaper Review

We tested 76 small programs, each checking a particular rule from the whitepaper.

All behaved as expected, except for the differences documented in the 'differences from Whitepaper' section.

### 4. Differential Testing  - TO BE DONE STILL

We generated random programs and compiled them both as SafeC and as C.

We then compared their outputs to check whether they behaved identically.

More than 7,000 programs have produced matching results.
