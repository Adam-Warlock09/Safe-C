D       [0-9]
L       [a-zA-Z\_]
H       [a-fA-F0-9]
E       [Ee][+-]?{D}+
FS      (f|F)

%option yylineno
%x COMMENT

%{
#include <stdio.h>
#include "tokens.h"
%}

%%
"break"         { return BREAK; }
"case"          { return CASE; }
"continue"      { return CONTINUE; }
"default"       { return DEFAULT; }
"else"          { return ELSE; }
"for"           { return FOR; }
"if"            { return IF; }
"return"        { return RETURN; }
"struct"        { return STRUCT; }
"switch"        { return SWITCH; }
"while"         { return WHILE; }

"void"          { return VOID; }
"bool"          { return BOOL; }
"char"          { return CHAR; }
"int"           { return INT; }
"float"         { return FLOAT; }

"own"           { return OWN_KEYWORD; }
"mut"           { return MUT_KEYWORD; }
"raw"           { return RAW_KEYWORD; }
"trusted"       { return TRUSTED_KEYWORD; }
"new"           { return NEW_KEYWORD; }
"free"          { return FREE_KEYWORD; }
"defer"         { return DEFER_KEYWORD; }
"null"          { return NULL_KEYWORD; }

"true"          { return BOOL_LITERAL; }
"false"         { return BOOL_LITERAL; }

{L}({L}|{D})*   { return IDENTIFIER; }

0[xX]{H}+       { return INT_LITERAL; }
{D}+            { return INT_LITERAL; }

'(\\.|[^\\'\n])'                { return CHAR_LITERAL; }
'(\\.|[^\\'\n])*'?              { printf("ERROR at line %d: invalid character literal: %s\n", yylineno, yytext); }

{D}+{E}{FS}?                    { return FLOAT_LITERAL; }
{D}*"."{D}+({E})?{FS}?          { return FLOAT_LITERAL; }
{D}+"."{D}*({E})?{FS}?          { return FLOAT_LITERAL; }
{D}({L}|{D})*                   { printf("ERROR at line %d: malformed number: %s\n", yylineno, yytext); }

\"(\\.|[^\\\"\n])*\"            { return STRING_LITERAL; }
\"(\\.|[^\\\"\n])*              { printf("ERROR at line %d: unterminated string: %s\n", yylineno, yytext); }

"..."       { return ELLIPSIS; }
">>="       { return RIGHT_SHIFT_ASSIGN; }
"<<="       { return LEFT_SHIFT_ASSIGN; }
">>"        { return RIGHT_SHIFT; }
"<<"        { return LEFT_SHIFT; }
"->"        { return ARROW; }
"++"        { return INCREMENT; }
"--"        { return DECREMENT; }
"&&"        { return LOGICAL_AND; }
"||"        { return LOGICAL_OR; }
"<="        { return LESS_EQUAL; }
">="        { return GREATER_EQUAL; }
"=="        { return EQUAL; }
"!="        { return NOT_EQUAL; }
"+="        { return ADD_ASSIGN; }
"-="        { return SUB_ASSIGN; }
"*="        { return MUL_ASSIGN; }
"/="        { return DIV_ASSIGN; }
"%="        { return MOD_ASSIGN; }
"&="        { return AND_ASSIGN; }
"^="        { return XOR_ASSIGN; }
"|="        { return OR_ASSIGN; }

"="         { return ASSIGN; }
"&"         { return BITWISE_AND; }
"!"         { return LOGICAL_NOT; }
"-"         { return MINUS; }
"+"         { return PLUS; }
"*"         { return MULTIPLY; }
"/"         { return DIVIDE; }
"%"         { return MODULO; }
"<"         { return LESS_THAN; }
">"         { return GREATER_THAN; }
"^"         { return BITWISE_XOR; }
"|"         { return BITWISE_OR; }
"~"         { return BITWISE_NOT; }
"?"         { return NULLABLE_PTR; }

";"         { return SEMICOLON; }
"{"         { return LEFT_BRACE; }
"}"         { return RIGHT_BRACE; }
","         { return COMMA; }
":"         { return COLON; }
"("         { return LEFT_PAREN; }
")"         { return RIGHT_PAREN; }
"["         { return LEFT_BRACKET; }
"]"         { return RIGHT_BRACKET; }
"."         { return DOT; }

"/*"        { BEGIN(COMMENT); }
"//".*      { }

<COMMENT>"*/"      { BEGIN(INITIAL); }
<COMMENT>[^*\n]+  { }
<COMMENT>"*"      { }
<COMMENT>\n        { }
<COMMENT><<EOF>> {
    printf("ERROR at line %d: unterminated comment\n", yylineno);
    return 0;
}

[ \t\v\n\f\r]+    { }

.           { printf("ERROR at line %d: unknown character: %s\n", yylineno, yytext); }
%%

int yywrap(void) {
    return 1;
}

int main(void) {
    int token;
    while ((token = yylex()) != 0) {
        printf("TOKEN: %d\n", token);
    }
    return 0;
}