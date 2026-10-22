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

/* parser sets this to 0 to mute the trace; 1 = print like before */
int safec_lexer_trace = 1;

/* 1-based column of the most recently returned token's start. */
int safec_lexer_token_column = 1;

/* Column where the next matched lexeme begins. */
static int safec_lexer_current_column = 1;

/* Run before every lexer rule, including whitespace and comment rules.
   Record the lexeme's starting column, then advance across its characters. */
#define YY_USER_ACTION do { \
    safec_lexer_token_column = safec_lexer_current_column; \
    for (int i = 0; i < yyleng; ++i) { \
        if (yytext[i] == '\n') safec_lexer_current_column = 1; \
        else safec_lexer_current_column++; \
    } \
} while (0);
%}

%%
"break"         { if (safec_lexer_trace) printf("TOKEN: BREAK, NUMBER: %d\n", BREAK); return BREAK; }
"case"          { if (safec_lexer_trace) printf("TOKEN: CASE, NUMBER: %d\n", CASE); return CASE; }
"continue"      { if (safec_lexer_trace) printf("TOKEN: CONTINUE, NUMBER: %d\n", CONTINUE); return CONTINUE; }
"default"       { if (safec_lexer_trace) printf("TOKEN: DEFAULT, NUMBER: %d\n", DEFAULT); return DEFAULT; }
"else"          { if (safec_lexer_trace) printf("TOKEN: ELSE, NUMBER: %d\n", ELSE); return ELSE; }
"for"           { if (safec_lexer_trace) printf("TOKEN: FOR, NUMBER: %d\n", FOR); return FOR; }
"if"            { if (safec_lexer_trace) printf("TOKEN: IF, NUMBER: %d\n", IF); return IF; }
"return"        { if (safec_lexer_trace) printf("TOKEN: RETURN, NUMBER: %d\n", RETURN); return RETURN; }
"struct"        { if (safec_lexer_trace) printf("TOKEN: STRUCT, NUMBER: %d\n", STRUCT); return STRUCT; }
"switch"        { if (safec_lexer_trace) printf("TOKEN: SWITCH, NUMBER: %d\n", SWITCH); return SWITCH; }
"while"         { if (safec_lexer_trace) printf("TOKEN: WHILE, NUMBER: %d\n", WHILE); return WHILE; }

"void"          { if (safec_lexer_trace) printf("TOKEN: VOID, NUMBER: %d\n", VOID); return VOID; }
"bool"          { if (safec_lexer_trace) printf("TOKEN: BOOL, NUMBER: %d\n", BOOL); return BOOL; }
"char"          { if (safec_lexer_trace) printf("TOKEN: CHAR, NUMBER: %d\n", CHAR); return CHAR; }
"int"           { if (safec_lexer_trace) printf("TOKEN: INT, NUMBER: %d\n", INT); return INT; }
"float"         { if (safec_lexer_trace) printf("TOKEN: FLOAT, NUMBER: %d\n", FLOAT); return FLOAT; }

"own"           { if (safec_lexer_trace) printf("TOKEN: OWN_KEYWORD, NUMBER: %d\n", OWN_KEYWORD); return OWN_KEYWORD; }
"mut"           { if (safec_lexer_trace) printf("TOKEN: MUT_KEYWORD, NUMBER: %d\n", MUT_KEYWORD); return MUT_KEYWORD; }
"raw"           { if (safec_lexer_trace) printf("TOKEN: RAW_KEYWORD, NUMBER: %d\n", RAW_KEYWORD); return RAW_KEYWORD; }
"trusted"       { if (safec_lexer_trace) printf("TOKEN: TRUSTED_KEYWORD, NUMBER: %d\n", TRUSTED_KEYWORD); return TRUSTED_KEYWORD; }
"new"           { if (safec_lexer_trace) printf("TOKEN: NEW_KEYWORD, NUMBER: %d\n", NEW_KEYWORD); return NEW_KEYWORD; }
"free"          { if (safec_lexer_trace) printf("TOKEN: FREE_KEYWORD, NUMBER: %d\n", FREE_KEYWORD); return FREE_KEYWORD; }
"defer"         { if (safec_lexer_trace) printf("TOKEN: DEFER_KEYWORD, NUMBER: %d\n", DEFER_KEYWORD); return DEFER_KEYWORD; }
"null"          { if (safec_lexer_trace) printf("TOKEN: NULL_KEYWORD, NUMBER: %d\n", NULL_KEYWORD); return NULL_KEYWORD; }

"true"          { if (safec_lexer_trace) printf("TOKEN: BOOL_LITERAL, NUMBER: %d\n", BOOL_LITERAL); return BOOL_LITERAL; }
"false"         { if (safec_lexer_trace) printf("TOKEN: BOOL_LITERAL, NUMBER: %d\n", BOOL_LITERAL); return BOOL_LITERAL; }

{L}({L}|{D})*   { if (safec_lexer_trace) printf("TOKEN: IDENTIFIER, NUMBER: %d\n", IDENTIFIER); return IDENTIFIER; }

0[xX]{H}+       { if (safec_lexer_trace) printf("TOKEN: INT_LITERAL, NUMBER: %d\n", INT_LITERAL); return INT_LITERAL; }
{D}+            { if (safec_lexer_trace) printf("TOKEN: INT_LITERAL, NUMBER: %d\n", INT_LITERAL); return INT_LITERAL; }

'(\\.|[^\\'\n])'                { if (safec_lexer_trace) printf("TOKEN: CHAR_LITERAL, NUMBER: %d\n", CHAR_LITERAL); return CHAR_LITERAL; }
'(\\.|[^\\'\n])*'?              { printf("ERROR at line %d: invalid character literal: %s\n", yylineno, yytext); }

{D}+{E}{FS}?                    { if (safec_lexer_trace) printf("TOKEN: FLOAT_LITERAL, NUMBER: %d\n", FLOAT_LITERAL); return FLOAT_LITERAL; }
{D}*"."{D}+({E})?{FS}?          { if (safec_lexer_trace) printf("TOKEN: FLOAT_LITERAL, NUMBER: %d\n", FLOAT_LITERAL); return FLOAT_LITERAL; }
{D}+"."{D}*({E})?{FS}?          { if (safec_lexer_trace) printf("TOKEN: FLOAT_LITERAL, NUMBER: %d\n", FLOAT_LITERAL); return FLOAT_LITERAL; }
{D}({L}|{D})*                   { printf("ERROR at line %d: malformed number: %s\n", yylineno, yytext); }

\"(\\.|[^\\\"\n])*\"            { if (safec_lexer_trace) printf("TOKEN: STRING_LITERAL, NUMBER: %d\n", STRING_LITERAL); return STRING_LITERAL; }
\"(\\.|[^\\\"\n])*              { printf("ERROR at line %d: unterminated string: %s\n", yylineno, yytext); }

"..."       { if (safec_lexer_trace) printf("TOKEN: ELLIPSIS, NUMBER: %d\n", ELLIPSIS); return ELLIPSIS; }
">>="       { if (safec_lexer_trace) printf("TOKEN: RIGHT_SHIFT_ASSIGN, NUMBER: %d\n", RIGHT_SHIFT_ASSIGN); return RIGHT_SHIFT_ASSIGN; }
"<<="       { if (safec_lexer_trace) printf("TOKEN: LEFT_SHIFT_ASSIGN, NUMBER: %d\n", LEFT_SHIFT_ASSIGN); return LEFT_SHIFT_ASSIGN; }
">>"        { if (safec_lexer_trace) printf("TOKEN: RIGHT_SHIFT, NUMBER: %d\n", RIGHT_SHIFT); return RIGHT_SHIFT; }
"<<"        { if (safec_lexer_trace) printf("TOKEN: LEFT_SHIFT, NUMBER: %d\n", LEFT_SHIFT); return LEFT_SHIFT; }
"->"        { if (safec_lexer_trace) printf("TOKEN: ARROW, NUMBER: %d\n", ARROW); return ARROW; }
"++"        { if (safec_lexer_trace) printf("TOKEN: INCREMENT, NUMBER: %d\n", INCREMENT); return INCREMENT; }
"--"        { if (safec_lexer_trace) printf("TOKEN: DECREMENT, NUMBER: %d\n", DECREMENT); return DECREMENT; }
"&&"        { if (safec_lexer_trace) printf("TOKEN: LOGICAL_AND, NUMBER: %d\n", LOGICAL_AND); return LOGICAL_AND; }
"||"        { if (safec_lexer_trace) printf("TOKEN: LOGICAL_OR, NUMBER: %d\n", LOGICAL_OR); return LOGICAL_OR; }
"<="        { if (safec_lexer_trace) printf("TOKEN: LESS_EQUAL, NUMBER: %d\n", LESS_EQUAL); return LESS_EQUAL; }
">="        { if (safec_lexer_trace) printf("TOKEN: GREATER_EQUAL, NUMBER: %d\n", GREATER_EQUAL); return GREATER_EQUAL; }
"=="        { if (safec_lexer_trace) printf("TOKEN: EQUAL, NUMBER: %d\n", EQUAL); return EQUAL; }
"!="        { if (safec_lexer_trace) printf("TOKEN: NOT_EQUAL, NUMBER: %d\n", NOT_EQUAL); return NOT_EQUAL; }
"+="        { if (safec_lexer_trace) printf("TOKEN: ADD_ASSIGN, NUMBER: %d\n", ADD_ASSIGN); return ADD_ASSIGN; }
"-="        { if (safec_lexer_trace) printf("TOKEN: SUB_ASSIGN, NUMBER: %d\n", SUB_ASSIGN); return SUB_ASSIGN; }
"*="        { if (safec_lexer_trace) printf("TOKEN: MUL_ASSIGN, NUMBER: %d\n", MUL_ASSIGN); return MUL_ASSIGN; }
"/="        { if (safec_lexer_trace) printf("TOKEN: DIV_ASSIGN, NUMBER: %d\n", DIV_ASSIGN); return DIV_ASSIGN; }
"%="        { if (safec_lexer_trace) printf("TOKEN: MOD_ASSIGN, NUMBER: %d\n", MOD_ASSIGN); return MOD_ASSIGN; }
"&="        { if (safec_lexer_trace) printf("TOKEN: AND_ASSIGN, NUMBER: %d\n", AND_ASSIGN); return AND_ASSIGN; }
"^="        { if (safec_lexer_trace) printf("TOKEN: XOR_ASSIGN, NUMBER: %d\n", XOR_ASSIGN); return XOR_ASSIGN; }
"|="        { if (safec_lexer_trace) printf("TOKEN: OR_ASSIGN, NUMBER: %d\n", OR_ASSIGN); return OR_ASSIGN; }

"="         { if (safec_lexer_trace) printf("TOKEN: ASSIGN, NUMBER: %d\n", ASSIGN); return ASSIGN; }
"&"         { if (safec_lexer_trace) printf("TOKEN: BITWISE_AND, NUMBER: %d\n", BITWISE_AND); return BITWISE_AND; }
"!"         { if (safec_lexer_trace) printf("TOKEN: LOGICAL_NOT, NUMBER: %d\n", LOGICAL_NOT); return LOGICAL_NOT; }
"-"         { if (safec_lexer_trace) printf("TOKEN: MINUS, NUMBER: %d\n", MINUS); return MINUS; }
"+"         { if (safec_lexer_trace) printf("TOKEN: PLUS, NUMBER: %d\n", PLUS); return PLUS; }
"*"         { if (safec_lexer_trace) printf("TOKEN: MULTIPLY, NUMBER: %d\n", MULTIPLY); return MULTIPLY; }
"/"         { if (safec_lexer_trace) printf("TOKEN: DIVIDE, NUMBER: %d\n", DIVIDE); return DIVIDE; }
"%"         { if (safec_lexer_trace) printf("TOKEN: MODULO, NUMBER: %d\n", MODULO); return MODULO; }
"<"         { if (safec_lexer_trace) printf("TOKEN: LESS_THAN, NUMBER: %d\n", LESS_THAN); return LESS_THAN; }
">"         { if (safec_lexer_trace) printf("TOKEN: GREATER_THAN, NUMBER: %d\n", GREATER_THAN); return GREATER_THAN; }
"^"         { if (safec_lexer_trace) printf("TOKEN: BITWISE_XOR, NUMBER: %d\n", BITWISE_XOR); return BITWISE_XOR; }
"|"         { if (safec_lexer_trace) printf("TOKEN: BITWISE_OR, NUMBER: %d\n", BITWISE_OR); return BITWISE_OR; }
"~"         { if (safec_lexer_trace) printf("TOKEN: BITWISE_NOT, NUMBER: %d\n", BITWISE_NOT); return BITWISE_NOT; }
"?"         { if (safec_lexer_trace) printf("TOKEN: NULLABLE_PTR, NUMBER: %d\n", NULLABLE_PTR); return NULLABLE_PTR; }

";"         { if (safec_lexer_trace) printf("TOKEN: SEMICOLON, NUMBER: %d\n", SEMICOLON); return SEMICOLON; }
"{"         { if (safec_lexer_trace) printf("TOKEN: LEFT_BRACE, NUMBER: %d\n", LEFT_BRACE); return LEFT_BRACE; }
"}"         { if (safec_lexer_trace) printf("TOKEN: RIGHT_BRACE, NUMBER: %d\n", RIGHT_BRACE); return RIGHT_BRACE; }
","         { if (safec_lexer_trace) printf("TOKEN: COMMA, NUMBER: %d\n", COMMA); return COMMA; }
":"         { if (safec_lexer_trace) printf("TOKEN: COLON, NUMBER: %d\n", COLON); return COLON; }
"("         { if (safec_lexer_trace) printf("TOKEN: LEFT_PAREN, NUMBER: %d\n", LEFT_PAREN); return LEFT_PAREN; }
")"         { if (safec_lexer_trace) printf("TOKEN: RIGHT_PAREN, NUMBER: %d\n", RIGHT_PAREN); return RIGHT_PAREN; }
"["         { if (safec_lexer_trace) printf("TOKEN: LEFT_BRACKET, NUMBER: %d\n", LEFT_BRACKET); return LEFT_BRACKET; }
"]"         { if (safec_lexer_trace) printf("TOKEN: RIGHT_BRACKET, NUMBER: %d\n", RIGHT_BRACKET); return RIGHT_BRACKET; }
"."         { if (safec_lexer_trace) printf("TOKEN: DOT, NUMBER: %d\n", DOT); return DOT; }

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

#ifndef SAFEC_NO_LEXER_MAIN
/* test driver; the parser build skips this (-DSAFEC_NO_LEXER_MAIN) */
int main(void) {
    int token;
    while ((token = yylex()) != 0) {
        if (safec_lexer_trace)
            printf("TOKEN: %d, COLUMN: %d\n", token, safec_lexer_token_column);
    }
    return 0;
}
#endif