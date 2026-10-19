# Safe-C

A memory-safe C-family language on LLVM — team compilers project.

## Repo layout

- `src/lexer.l` — flex lexer (shared, produces the token stream)
- `src/tokens.h` — token codes
- `src/parser.y` — **the SafeC grammar for bison** (token numbers synced with `tokens.h`)
- `src/parser.c` — current handwritten parser (common grammar only, builds the AST)
- `src/ast.c`, `src/ast.h`, `src/main_parser.c` — AST + parser driver
- `tests/` — lexer and parser tests (`make test`)
- `docs/safec_grammar.ebnf` — this grammar in EBNF form

## Build

```
make          # safec-parser + standalone lexer
make test     # lexer regression + parser tests
```

## One language change (vs the whitepaper)

Struct types are always written with the `struct` keyword:

```
struct List list = list_new();     // yes
List list = list_new();            // no — syntax error
```

Reason: bare `List` makes `List * p;` (declaration) and `a * b;`
(multiplication) indistinguishable to any generated parser, since
struct names are ordinary identifiers. Requiring `struct` makes the
grammar LALR(1)-clean — bison reports **zero conflicts**.

## Grammar (EBNF)

Same as `docs/safec_grammar.ebnf`. Terminals in quotes are lexer
tokens (`src/tokens.h`); `[ x ]` = optional; `{ x }` = zero or more.

```
program               = { external_declaration } ;

external_declaration  = struct_definition
                      | function_definition
                      | global_declaration ;

global_declaration    = declaration ;

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

function_definition   = qualified_base { pointer_level }
                          identifier parameter_list
                          compound_statement ;

parameter_list        = "(" [ parameters ] ")" ;

parameters            = parameter { "," parameter } | "void" ;

parameter             = qualified_base { pointer_level }
                          identifier { array_suffix } ;

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

## Semantic-analysis AST (`src/new_ast.h`)

A header-only C++17 AST for semantic analysis.

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
