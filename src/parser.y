/* Safe-C grammar for bison.
   Token numbers are synced with src/tokens.h (EOF_TOKEN = 0 is
   bison's $end).  To re-sync after editing tokens.h, regenerate the
   %token block - see README.md.
   One language change from the whitepaper: struct types are always
   written with the 'struct' keyword (`struct List x`), which makes
   this grammar conflict-free for LALR(1).
   Actions are empty for now; this file only checks syntax. */

%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%define parse.error verbose

%token BREAK 1
%token CASE 2
%token CONTINUE 3
%token DEFAULT 4
%token ELSE 5
%token FOR 6
%token IF 7
%token RETURN 8
%token STRUCT 9
%token SWITCH 10
%token WHILE 11
%token VOID 12
%token BOOL 13
%token CHAR 14
%token INT 15
%token FLOAT 16
%token OWN_KEYWORD 17
%token MUT_KEYWORD 18
%token RAW_KEYWORD 19
%token TRUSTED_KEYWORD 20
%token NEW_KEYWORD 21
%token FREE_KEYWORD 22
%token DEFER_KEYWORD 23
%token NULL_KEYWORD 24
%token BOOL_LITERAL 25
%token IDENTIFIER 26
%token INT_LITERAL 27
%token CHAR_LITERAL 28
%token FLOAT_LITERAL 29
%token STRING_LITERAL 30
%token ELLIPSIS 31
%token RIGHT_SHIFT_ASSIGN 32
%token LEFT_SHIFT_ASSIGN 33
%token RIGHT_SHIFT 34
%token LEFT_SHIFT 35
%token ARROW 36
%token INCREMENT 37
%token DECREMENT 38
%token LOGICAL_AND 39
%token LOGICAL_OR 40
%token LESS_EQUAL 41
%token GREATER_EQUAL 42
%token EQUAL 43
%token NOT_EQUAL 44
%token ADD_ASSIGN 45
%token SUB_ASSIGN 46
%token MUL_ASSIGN 47
%token DIV_ASSIGN 48
%token MOD_ASSIGN 49
%token AND_ASSIGN 50
%token XOR_ASSIGN 51
%token OR_ASSIGN 52
%token ASSIGN 53
%token BITWISE_AND 54
%token LOGICAL_NOT 55
%token MINUS 56
%token PLUS 57
%token MULTIPLY 58
%token DIVIDE 59
%token MODULO 60
%token LESS_THAN 61
%token GREATER_THAN 62
%token BITWISE_XOR 63
%token BITWISE_OR 64
%token BITWISE_NOT 65
%token NULLABLE_PTR 66
%token SEMICOLON 67
%token LEFT_BRACE 68
%token RIGHT_BRACE 69
%token COMMA 70
%token COLON 71
%token LEFT_PAREN 72
%token RIGHT_PAREN 73
%token LEFT_BRACKET 74
%token RIGHT_BRACKET 75
%token DOT 76

/* dangling-else fix: ELSE binds to the nearest if */
%token THEN
%precedence THEN
%precedence ELSE

/* operator precedence, loosest first */
%left LOGICAL_OR
%left LOGICAL_AND
%left BITWISE_OR
%left BITWISE_XOR
%left BITWISE_AND
%left EQUAL NOT_EQUAL
%left LESS_THAN GREATER_THAN LESS_EQUAL GREATER_EQUAL
%left LEFT_SHIFT RIGHT_SHIFT
%left PLUS MINUS
%left MULTIPLY DIVIDE MODULO
%precedence UNARY

%%

program
    : %empty
    | program external
    ;

external
    : struct_def
    | func_def
    | decl
    ;

struct_def
    : STRUCT IDENTIFIER LEFT_BRACE fields RIGHT_BRACE SEMICOLON
    ;

fields
    : %empty
    | fields field
    ;

field
    : qual_base field_declarators SEMICOLON
    ;

field_declarators
    : field_declarator
    | field_declarators COMMA field_declarator
    ;

field_declarator
    : ptrs IDENTIFIER arrays
    ;

decl
    : qual_base declarators SEMICOLON
    ;

declarators
    : declarator
    | declarators COMMA declarator
    ;

declarator
    : ptrs IDENTIFIER arrays
    | ptrs IDENTIFIER arrays ASSIGN assignment
    ;

/* stars belong to the declarator: in `int a, *b;` only b is a pointer */
ptrs
    : %empty
    | ptrs MULTIPLY
    | ptrs MULTIPLY NULLABLE_PTR
    ;

arrays
    : %empty
    | arrays LEFT_BRACKET RIGHT_BRACKET
    | arrays LEFT_BRACKET assignment RIGHT_BRACKET
    ;

qual_base
    : type_base
    | OWN_KEYWORD type_base
    | MUT_KEYWORD type_base
    | RAW_KEYWORD type_base
    ;

/* the one language change: no bare identifier here, always `struct X` */
type_base
    : VOID
    | BOOL
    | CHAR
    | INT
    | FLOAT
    | STRUCT IDENTIFIER
    ;

func_def
    : qual_base ptrs IDENTIFIER LEFT_PAREN params RIGHT_PAREN block
    ;

params
    : %empty
    | VOID
    | param_list
    ;

param_list
    : param
    | param_list COMMA param
    ;

param
    : qual_base ptrs IDENTIFIER arrays
    ;

block
    : LEFT_BRACE items RIGHT_BRACE
    ;

items
    : %empty
    | items item
    ;

item
    : decl
    | stmt
    ;

stmt
    : block
    | if_stmt
    | while_stmt
    | for_stmt
    | switch_stmt
    | RETURN SEMICOLON
    | RETURN assignment SEMICOLON
    | BREAK SEMICOLON
    | CONTINUE SEMICOLON
    | DEFER_KEYWORD stmt
    | TRUSTED_KEYWORD block
    | assignment SEMICOLON
    | SEMICOLON
    ;

if_stmt
    : IF LEFT_PAREN assignment RIGHT_PAREN stmt %prec THEN
    | IF LEFT_PAREN assignment RIGHT_PAREN stmt ELSE stmt
    ;

while_stmt
    : WHILE LEFT_PAREN assignment RIGHT_PAREN stmt
    ;

for_stmt
    : FOR LEFT_PAREN for_init opt_assign SEMICOLON opt_assign RIGHT_PAREN stmt
    ;

/* a decl already ends with its own SEMICOLON */
for_init
    : decl
    | opt_assign SEMICOLON
    ;

opt_assign
    : %empty
    | assignment
    ;

switch_stmt
    : SWITCH LEFT_PAREN assignment RIGHT_PAREN LEFT_BRACE sections RIGHT_BRACE
    ;

sections
    : %empty
    | sections section
    ;

section
    : CASE assignment COLON items
    | DEFAULT COLON items
    ;

assignment
    : logical_or
    | logical_or assign_op assignment
    ;

assign_op
    : ASSIGN
    | ADD_ASSIGN
    | SUB_ASSIGN
    | MUL_ASSIGN
    | DIV_ASSIGN
    | MOD_ASSIGN
    | AND_ASSIGN
    | OR_ASSIGN
    | XOR_ASSIGN
    | LEFT_SHIFT_ASSIGN
    | RIGHT_SHIFT_ASSIGN
    ;

logical_or
    : logical_and
    | logical_or LOGICAL_OR logical_and
    ;

logical_and
    : bitwise_or
    | logical_and LOGICAL_AND bitwise_or
    ;

bitwise_or
    : bitwise_xor
    | bitwise_or BITWISE_OR bitwise_xor
    ;

bitwise_xor
    : bitwise_and
    | bitwise_xor BITWISE_XOR bitwise_and
    ;

bitwise_and
    : equality
    | bitwise_and BITWISE_AND equality
    ;

equality
    : relational
    | equality EQUAL relational
    | equality NOT_EQUAL relational
    ;

relational
    : shift
    | relational LESS_THAN shift
    | relational GREATER_THAN shift
    | relational LESS_EQUAL shift
    | relational GREATER_EQUAL shift
    ;

shift
    : additive
    | shift LEFT_SHIFT additive
    | shift RIGHT_SHIFT additive
    ;

additive
    : multiplicative
    | additive PLUS multiplicative
    | additive MINUS multiplicative
    ;

multiplicative
    : unary
    | multiplicative MULTIPLY unary
    | multiplicative DIVIDE unary
    | multiplicative MODULO unary
    ;

unary
    : postfix
    | MINUS unary %prec UNARY
    | PLUS unary %prec UNARY
    | LOGICAL_NOT unary %prec UNARY
    | BITWISE_NOT unary %prec UNARY
    | MULTIPLY unary %prec UNARY
    | BITWISE_AND unary %prec UNARY
    | BITWISE_AND MUT_KEYWORD unary %prec UNARY
    | INCREMENT unary %prec UNARY
    | DECREMENT unary %prec UNARY
    ;

postfix
    : primary
    | postfix LEFT_PAREN RIGHT_PAREN
    | postfix LEFT_PAREN args RIGHT_PAREN
    | postfix LEFT_BRACKET assignment RIGHT_BRACKET
    | postfix DOT IDENTIFIER
    | postfix ARROW IDENTIFIER
    | postfix INCREMENT
    | postfix DECREMENT
    ;

args
    : assignment
    | args COMMA assignment
    ;

primary
    : INT_LITERAL
    | FLOAT_LITERAL
    | CHAR_LITERAL
    | STRING_LITERAL
    | BOOL_LITERAL
    | NULL_KEYWORD
    | IDENTIFIER
    | LEFT_PAREN assignment RIGHT_PAREN
    | NEW_KEYWORD type_base
    | FREE_KEYWORD LEFT_PAREN assignment RIGHT_PAREN
    | compound_literal
    ;

compound_literal
    : LEFT_PAREN qual_base ptrs RIGHT_PAREN LEFT_BRACE inits RIGHT_BRACE
    ;

inits
    : init
    | inits COMMA init
    ;

init
    : DOT IDENTIFIER ASSIGN assignment
    | assignment
    ;

%%

void yyerror(const char *s) {
    fprintf(stderr, "%s\n", s);
}
