# SafeC Grammar

This is the grammar accepted by the SafeC compiler. The lexical rules come from
[`src/lexer.l`](../src/lexer.l), and the syntax from the bison grammar in
[`src/parser.y`](../src/parser.y), which is LALR(1) with no conflicts. If this
document and the source disagree, the source is authoritative.

The grammar describes only which programs parse. Types, ownership, borrowing and
null safety are checked afterwards by semantic analysis and the borrow checker,
as described in the [whitepaper](WHITEPAPER.pdf). For example, `int f(void x)`
parses but is rejected because a parameter can't have type `void`.

## Notation

| Notation | Meaning |
| --- | --- |
| `"text"` | A terminal: a keyword, operator or punctuation token |
| `name` | A nonterminal |
| `[ x ]` | Optional |
| `{ x }` | Zero or more repetitions |
| `a \| b` | Alternatives |
| `(* ... *)` | A comment |

## Lexical grammar

### Whitespace and comments

Spaces, tabs, newlines, vertical tabs, form feeds and carriage returns separate
tokens and are otherwise ignored. Comments are `// ...` to the end of the line
and `/* ... */`, which do not nest. An unterminated `/*` comment is an error.

There is no preprocessor: `#include`, `#define` and other directives are not
recognized.

### Keywords

```
bool     break    case     char     continue default  defer    else
extern   false    float    for      free     if       int      mut
new      null     own      raw      return   struct   switch   true
trusted  void     while
```

`own`, `mut`, `raw`, `trusted`, `new`, `free`, `defer`, `null` and `extern` are
reserved in SafeC and can't be used as identifiers.

### Identifiers

```
identifier     = letter { letter | digit } ;
letter         = "a".."z" | "A".."Z" | "_" ;
digit          = "0".."9" ;
```

### Literals

```
integer_literal   = digit { digit }
                  | ( "0x" | "0X" ) hex_digit { hex_digit } ;
hex_digit         = digit | "a".."f" | "A".."F" ;

float_literal     = digit { digit } exponent [ "f" | "F" ]
                  | { digit } "." digit { digit } [ exponent ] [ "f" | "F" ]
                  | digit { digit } "." { digit } [ exponent ] [ "f" | "F" ] ;
exponent          = ( "e" | "E" ) [ "+" | "-" ] digit { digit } ;

character_literal = "'" ( escape | any character except "\", "'" or newline ) "'" ;
string_literal    = '"' { escape | any character except "\", '"' or newline } '"' ;
escape            = "\" any character ;

boolean_literal   = "true" | "false" ;
```

A number immediately followed by letters (such as `12abc` or `1.5x`), a number
with two decimal points, and an exponent with no digits are lexical errors.

### Operators and punctuation

```
+   -   *   /   %   ++  --
==  !=  <   >   <=  >=
&&  ||  !
&   |   ^   ~   <<  >>
=   +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
.   ->  ,   ;   :   ?   ...
(   )   [   ]   {   }
```

`?` appears only as the nullable-pointer suffix (`T*?`); SafeC has no
conditional operator.

## Syntax

### Programs and top-level declarations

```
program               = { external_declaration } ;

external_declaration  = struct_definition
                      | [ "extern" ] function_definition
                      | [ "extern" ] function_declaration
                      | [ "extern" ] declaration ;
                        (* an extern variable can't have an initializer *)
```

### Types

```
type_base             = "void" | "bool" | "char" | "int" | "float"
                      | "struct" identifier ;

qualified_base        = [ pointer_qualifier ] type_base ;

pointer_qualifier     = "own" | "mut" | "raw" ;

pointer_level         = "*" [ "?" ] ;      (* "?" makes the pointer nullable *)

array_suffix          = "[" [ assignment_expr ] "]" ;
```

A pointer type is a `qualified_base` followed by one or more `pointer_level`s,
such as `own struct Node*?` or `mut int*`. Struct types are always written with
the `struct` keyword.

### Structs

```
struct_definition     = "struct" identifier "{" { field_declaration } "}" ";" ;

field_declaration     = qualified_base field_declarator
                          { "," field_declarator } ";" ;

field_declarator      = { pointer_level } identifier { array_suffix } ;
```

### Variables

```
declaration           = qualified_base declarator { "," declarator } ";" ;

declarator            = { pointer_level } identifier { array_suffix }
                          [ "=" assignment_expr ] ;
```

### Functions

```
function_definition   = function_header compound_statement ;

function_declaration  = function_header ";" ;
                        (* defined elsewhere, for example in C *)

function_header       = qualified_base { pointer_level } identifier
                          "(" [ parameters ] ")" ;

parameters            = parameter { "," parameter } [ "," "..." ] ;
                        (* "..." only in declarations of C functions *)

parameter             = qualified_base { pointer_level } [ identifier ]
                          { array_suffix } ;
                        (* the name may be left out only in a declaration;
                           "(void)" is an empty parameter list *)
```

### Statements

```
compound_statement    = "{" { block_item } "}" ;

block_item            = declaration | statement ;

statement             = compound_statement
                      | expression ";"
                      | ";"
                      | "if" "(" expression ")" statement [ "else" statement ]
                      | "while" "(" expression ")" statement
                      | "for" "(" for_init [ expression ] ";" [ expression ] ")"
                          statement
                      | "switch" "(" expression ")" "{" { switch_section } "}"
                      | "return" [ expression ] ";"
                      | "break" ";"
                      | "continue" ";"
                      | "defer" statement
                      | "trusted" compound_statement ;

for_init              = declaration | [ expression ] ";" ;

switch_section        = "case" expression ":" { block_item }
                      | "default" ":" { block_item } ;
```

An `else` belongs to the nearest unmatched `if`.

### Expressions

Operators are listed from lowest to highest precedence. All binary operators
are left-associative except assignment, which is right-associative. SafeC has
no comma operator; operands are evaluated left to right.

```
expression            = assignment_expr ;

assignment_expr       = logical_or_expr [ assignment_op assignment_expr ] ;

assignment_op         = "=" | "+=" | "-=" | "*=" | "/=" | "%="
                      | "&=" | "|=" | "^=" | "<<=" | ">>=" ;

logical_or_expr       = logical_and_expr { "||" logical_and_expr } ;

logical_and_expr      = bitwise_or_expr { "&&" bitwise_or_expr } ;

bitwise_or_expr       = bitwise_xor_expr { "|" bitwise_xor_expr } ;

bitwise_xor_expr      = bitwise_and_expr { "^" bitwise_and_expr } ;

bitwise_and_expr      = equality_expr { "&" equality_expr } ;

equality_expr         = relational_expr { ( "==" | "!=" ) relational_expr } ;

relational_expr       = shift_expr { ( "<" | ">" | "<=" | ">=" ) shift_expr } ;

shift_expr            = additive_expr { ( "<<" | ">>" ) additive_expr } ;

additive_expr         = multiplicative_expr
                          { ( "+" | "-" ) multiplicative_expr } ;

multiplicative_expr   = unary_expr { ( "*" | "/" | "%" ) unary_expr } ;

unary_expr            = postfix_expr
                      | unary_operator unary_expr ;

unary_operator        = "-" | "+" | "!" | "~"
                      | "*"                (* dereference *)
                      | "&"                (* read borrow *)
                      | "&" "mut"          (* write borrow *)
                      | "++" | "--" ;

postfix_expr          = primary_expr { postfix_operator } ;

postfix_operator      = "(" [ argument_list ] ")"
                      | "[" expression "]"
                      | "." identifier
                      | "->" identifier
                      | "++" | "--" ;

argument_list         = assignment_expr { "," assignment_expr } ;

primary_expr          = integer_literal
                      | float_literal
                      | character_literal
                      | string_literal
                      | boolean_literal
                      | "null"
                      | identifier
                      | "(" expression ")"
                      | "new" type_base
                      | "free" "(" expression ")"
                      | compound_literal ;

compound_literal      = "(" qualified_base { pointer_level } ")"
                          "{" initializer { "," initializer } "}" ;

initializer           = "." identifier "=" assignment_expr
                      | assignment_expr ;
```

## Example

```c
struct Counter {
    int count;
    int limit;
};

own struct Counter* make(int limit) {
    own struct Counter* c = new struct Counter;
    c->limit = limit;
    return c;
}

bool step(mut struct Counter* c) {
    if (c->count >= c->limit) return false;
    c->count += 1;
    return true;
}

int main(void) {
    own struct Counter* c = make(3);
    defer free(c);
    while (step(c)) {
        printf("%d\n", c->count);
    }
    return 0;
}
```
