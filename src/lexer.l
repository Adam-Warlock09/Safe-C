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
"break"         { printf("TOKEN: BREAK, NUMBER: %d\n", BREAK); return BREAK; }
"case"          { printf("TOKEN: CASE, NUMBER: %d\n", CASE); return CASE; }
"continue"      { printf("TOKEN: CONTINUE, NUMBER: %d\n", CONTINUE); return CONTINUE; }
"default"       { printf("TOKEN: DEFAULT, NUMBER: %d\n", DEFAULT); return DEFAULT; }
"else"          { printf("TOKEN: ELSE, NUMBER: %d\n", ELSE); return ELSE; }
"for"           { printf("TOKEN: FOR, NUMBER: %d\n", FOR); return FOR; }
"if"            { printf("TOKEN: IF, NUMBER: %d\n", IF); return IF; }
"return"        { printf("TOKEN: RETURN, NUMBER: %d\n", RETURN); return RETURN; }
"struct"        { printf("TOKEN: STRUCT, NUMBER: %d\n", STRUCT); return STRUCT; }
"switch"        { printf("TOKEN: SWITCH, NUMBER: %d\n", SWITCH); return SWITCH; }
"while"         { printf("TOKEN: WHILE, NUMBER: %d\n", WHILE); return WHILE; }

"void"          { printf("TOKEN: VOID, NUMBER: %d\n", VOID); return VOID; }
"bool"          { printf("TOKEN: BOOL, NUMBER: %d\n", BOOL); return BOOL; }
"char"          { printf("TOKEN: CHAR, NUMBER: %d\n", CHAR); return CHAR; }
"int"           { printf("TOKEN: INT, NUMBER: %d\n", INT); return INT; }
"float"         { printf("TOKEN: FLOAT, NUMBER: %d\n", FLOAT); return FLOAT; }

"own"           { printf("TOKEN: OWN_KEYWORD, NUMBER: %d\n", OWN_KEYWORD); return OWN_KEYWORD; }
"mut"           { printf("TOKEN: MUT_KEYWORD, NUMBER: %d\n", MUT_KEYWORD); return MUT_KEYWORD; }
"raw"           { printf("TOKEN: RAW_KEYWORD, NUMBER: %d\n", RAW_KEYWORD); return RAW_KEYWORD; }
"trusted"       { printf("TOKEN: TRUSTED_KEYWORD, NUMBER: %d\n", TRUSTED_KEYWORD); return TRUSTED_KEYWORD; }
"new"           { printf("TOKEN: NEW_KEYWORD, NUMBER: %d\n", NEW_KEYWORD); return NEW_KEYWORD; }
"free"          { printf("TOKEN: FREE_KEYWORD, NUMBER: %d\n", FREE_KEYWORD); return FREE_KEYWORD; }
"defer"         { printf("TOKEN: DEFER_KEYWORD, NUMBER: %d\n", DEFER_KEYWORD); return DEFER_KEYWORD; }
"null"          { printf("TOKEN: NULL_KEYWORD, NUMBER: %d\n", NULL_KEYWORD); return NULL_KEYWORD; }

"true"          { printf("TOKEN: BOOL_LITERAL, NUMBER: %d\n", BOOL_LITERAL); return BOOL_LITERAL; }
"false"         { printf("TOKEN: BOOL_LITERAL, NUMBER: %d\n", BOOL_LITERAL); return BOOL_LITERAL; }

{L}({L}|{D})*   { printf("TOKEN: IDENTIFIER, NUMBER: %d\n", IDENTIFIER); return IDENTIFIER; }

0[xX]{H}+       { printf("TOKEN: INT_LITERAL, NUMBER: %d\n", INT_LITERAL); return INT_LITERAL; }
{D}+            { printf("TOKEN: INT_LITERAL, NUMBER: %d\n", INT_LITERAL); return INT_LITERAL; }

'(\\.|[^\\'\n])'                { printf("TOKEN: CHAR_LITERAL, NUMBER: %d\n", CHAR_LITERAL); return CHAR_LITERAL; }
'(\\.|[^\\'\n])*'?              { printf("ERROR at line %d: invalid character literal: %s\n", yylineno, yytext); }

{D}+{E}{FS}?                    { printf("TOKEN: FLOAT_LITERAL, NUMBER: %d\n", FLOAT_LITERAL); return FLOAT_LITERAL; }
{D}*"."{D}+({E})?{FS}?          { printf("TOKEN: FLOAT_LITERAL, NUMBER: %d\n", FLOAT_LITERAL); return FLOAT_LITERAL; }
{D}+"."{D}*({E})?{FS}?          { printf("TOKEN: FLOAT_LITERAL, NUMBER: %d\n", FLOAT_LITERAL); return FLOAT_LITERAL; }
{D}({L}|{D})*                   { printf("ERROR at line %d: malformed number: %s\n", yylineno, yytext); }

\"(\\.|[^\\\"\n])*\"            { printf("TOKEN: STRING_LITERAL, NUMBER: %d\n", STRING_LITERAL); return STRING_LITERAL; }
\"(\\.|[^\\\"\n])*              { printf("ERROR at line %d: unterminated string: %s\n", yylineno, yytext); }

"..."       { printf("TOKEN: ELLIPSIS, NUMBER: %d\n", ELLIPSIS); return ELLIPSIS; }
">>="       { printf("TOKEN: RIGHT_SHIFT_ASSIGN, NUMBER: %d\n", RIGHT_SHIFT_ASSIGN); return RIGHT_SHIFT_ASSIGN; }
"<<="       { printf("TOKEN: LEFT_SHIFT_ASSIGN, NUMBER: %d\n", LEFT_SHIFT_ASSIGN); return LEFT_SHIFT_ASSIGN; }
">>"        { printf("TOKEN: RIGHT_SHIFT, NUMBER: %d\n", RIGHT_SHIFT); return RIGHT_SHIFT; }
"<<"        { printf("TOKEN: LEFT_SHIFT, NUMBER: %d\n", LEFT_SHIFT); return LEFT_SHIFT; }
"->"        { printf("TOKEN: ARROW, NUMBER: %d\n", ARROW); return ARROW; }
"++"        { printf("TOKEN: INCREMENT, NUMBER: %d\n", INCREMENT); return INCREMENT; }
"--"        { printf("TOKEN: DECREMENT, NUMBER: %d\n", DECREMENT); return DECREMENT; }
"&&"        { printf("TOKEN: LOGICAL_AND, NUMBER: %d\n", LOGICAL_AND); return LOGICAL_AND; }
"||"        { printf("TOKEN: LOGICAL_OR, NUMBER: %d\n", LOGICAL_OR); return LOGICAL_OR; }
"<="        { printf("TOKEN: LESS_EQUAL, NUMBER: %d\n", LESS_EQUAL); return LESS_EQUAL; }
">="        { printf("TOKEN: GREATER_EQUAL, NUMBER: %d\n", GREATER_EQUAL); return GREATER_EQUAL; }
"=="        { printf("TOKEN: EQUAL, NUMBER: %d\n", EQUAL); return EQUAL; }
"!="        { printf("TOKEN: NOT_EQUAL, NUMBER: %d\n", NOT_EQUAL); return NOT_EQUAL; }
"+="        { printf("TOKEN: ADD_ASSIGN, NUMBER: %d\n", ADD_ASSIGN); return ADD_ASSIGN; }
"-="        { printf("TOKEN: SUB_ASSIGN, NUMBER: %d\n", SUB_ASSIGN); return SUB_ASSIGN; }
"*="        { printf("TOKEN: MUL_ASSIGN, NUMBER: %d\n", MUL_ASSIGN); return MUL_ASSIGN; }
"/="        { printf("TOKEN: DIV_ASSIGN, NUMBER: %d\n", DIV_ASSIGN); return DIV_ASSIGN; }
"%="        { printf("TOKEN: MOD_ASSIGN, NUMBER: %d\n", MOD_ASSIGN); return MOD_ASSIGN; }
"&="        { printf("TOKEN: AND_ASSIGN, NUMBER: %d\n", AND_ASSIGN); return AND_ASSIGN; }
"^="        { printf("TOKEN: XOR_ASSIGN, NUMBER: %d\n", XOR_ASSIGN); return XOR_ASSIGN; }
"|="        { printf("TOKEN: OR_ASSIGN, NUMBER: %d\n", OR_ASSIGN); return OR_ASSIGN; }

"="         { printf("TOKEN: ASSIGN, NUMBER: %d\n", ASSIGN); return ASSIGN; }
"&"         { printf("TOKEN: BITWISE_AND, NUMBER: %d\n", BITWISE_AND); return BITWISE_AND; }
"!"         { printf("TOKEN: LOGICAL_NOT, NUMBER: %d\n", LOGICAL_NOT); return LOGICAL_NOT; }
"-"         { printf("TOKEN: MINUS, NUMBER: %d\n", MINUS); return MINUS; }
"+"         { printf("TOKEN: PLUS, NUMBER: %d\n", PLUS); return PLUS; }
"*"         { printf("TOKEN: MULTIPLY, NUMBER: %d\n", MULTIPLY); return MULTIPLY; }
"/"         { printf("TOKEN: DIVIDE, NUMBER: %d\n", DIVIDE); return DIVIDE; }
"%"         { printf("TOKEN: MODULO, NUMBER: %d\n", MODULO); return MODULO; }
"<"         { printf("TOKEN: LESS_THAN, NUMBER: %d\n", LESS_THAN); return LESS_THAN; }
">"         { printf("TOKEN: GREATER_THAN, NUMBER: %d\n", GREATER_THAN); return GREATER_THAN; }
"^"         { printf("TOKEN: BITWISE_XOR, NUMBER: %d\n", BITWISE_XOR); return BITWISE_XOR; }
"|"         { printf("TOKEN: BITWISE_OR, NUMBER: %d\n", BITWISE_OR); return BITWISE_OR; }
"~"         { printf("TOKEN: BITWISE_NOT, NUMBER: %d\n", BITWISE_NOT); return BITWISE_NOT; }
"?"         { printf("TOKEN: NULLABLE_PTR, NUMBER: %d\n", NULLABLE_PTR); return NULLABLE_PTR; }

";"         { printf("TOKEN: SEMICOLON, NUMBER: %d\n", SEMICOLON); return SEMICOLON; }
"{"         { printf("TOKEN: LEFT_BRACE, NUMBER: %d\n", LEFT_BRACE); return LEFT_BRACE; }
"}"         { printf("TOKEN: RIGHT_BRACE, NUMBER: %d\n", RIGHT_BRACE); return RIGHT_BRACE; }
","         { printf("TOKEN: COMMA, NUMBER: %d\n", COMMA); return COMMA; }
":"         { printf("TOKEN: COLON, NUMBER: %d\n", COLON); return COLON; }
"("         { printf("TOKEN: LEFT_PAREN, NUMBER: %d\n", LEFT_PAREN); return LEFT_PAREN; }
")"         { printf("TOKEN: RIGHT_PAREN, NUMBER: %d\n", RIGHT_PAREN); return RIGHT_PAREN; }
"["         { printf("TOKEN: LEFT_BRACKET, NUMBER: %d\n", LEFT_BRACKET); return LEFT_BRACKET; }
"]"         { printf("TOKEN: RIGHT_BRACKET, NUMBER: %d\n", RIGHT_BRACKET); return RIGHT_BRACKET; }
"."         { printf("TOKEN: DOT, NUMBER: %d\n", DOT); return DOT; }

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