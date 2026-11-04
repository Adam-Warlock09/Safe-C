/*
 * driver.cpp - runs the compiler up to a given stage and prints the result.
 *
 *   driver [--till-lexer | --till-parser | --till-sema | --till-codegen] <file.safec>
 *
 *   --till-lexer    print the token stream
 *   --till-parser   print the AST
 *   --till-sema     run semantic + ownership checks, print diagnostics
 *   --till-codegen  everything (the default); codegen is a placeholder
 *
 * Exit codes: 0 ok, 1 errors, 2 bad usage.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "parser.tab.hpp"
#include "ast_dump.h"
#include "new_ast.h"
#include "sema.h"
#include "tokens.h"

// the flex lexer, compiled as C
extern "C" {
int   yylex(void);
extern char *yytext;
extern int   yylineno;
extern FILE *yyin;
extern int   safec_lexer_trace;
}

// ---- glue the bison parser needs: a token source and an error hook

namespace safec {

// decode one char of a char/string literal, handling escapes
static char decodeChar(const char *&p) {
    if (*p == '\\' && p[1] != '\0') {
        ++p;
        switch (char c = *p++) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        default:  return c;        // \\ ' " and unknown escapes: as-is
        }
    }
    return *p++;
}

// run flex, and turn the token text into the value bison expects
int yylex(SafeCParser::semantic_type *yylval, SafeCParser::location_type *yylloc) {
    int tok = ::yylex();
    yylloc->begin.line = yylloc->end.line = yylineno;

    switch (tok) {
    case IDENTIFIER:    yylval->str  = new std::string(yytext); break;
    case INT_LITERAL:   yylval->num  = std::strtoull(yytext, nullptr, 0); break;
    case FLOAT_LITERAL: yylval->fnum = std::strtod(yytext, nullptr); break;
    case BOOL_LITERAL:  yylval->bval = (yytext[0] == 't'); break;
    case CHAR_LITERAL: {
        const char *p = yytext + 1;               // skip the opening quote
        yylval->ch = decodeChar(p);
        break;
    }
    case STRING_LITERAL: {
        std::string out;
        for (const char *p = yytext + 1; *p != '\0' && *p != '"';) out += decodeChar(p);
        yylval->str = new std::string(std::move(out));
        break;
    }
    default: break;                               // keywords and punctuation
    }
    return tok;
}

void SafeCParser::error(const location_type &loc, const std::string &msg) {
    diag.error(SourceLoc(loc.begin.line, 0), msg);
}

} // namespace safec

// ---- driver

enum Stage { LEXER, PARSER, SEMA, CODEGEN };

static int usage() {
    std::fprintf(stderr,
                 "usage: driver [--till-lexer | --till-parser | --till-sema | --till-codegen] <file.safec>\n");
    return 2;
}

int main(int argc, char **argv) {
    Stage till = CODEGEN;
    const char *file = nullptr;

    for (int i = 1; i < argc; i++) {
        if      (std::strcmp(argv[i], "--till-lexer") == 0)   till = LEXER;
        else if (std::strcmp(argv[i], "--till-parser") == 0)  till = PARSER;
        else if (std::strcmp(argv[i], "--till-sema") == 0)    till = SEMA;
        else if (std::strcmp(argv[i], "--till-codegen") == 0) till = CODEGEN;
        else if (argv[i][0] == '-' || file)                   return usage();
        else                                                  file = argv[i];
    }
    if (!file) return usage();

    yyin = std::fopen(file, "r");
    if (!yyin) {
        std::fprintf(stderr, "driver: cannot open file '%s'\n", file);
        return 1;
    }

    // ---- 1. Lexer: the lexer prints each token as it scans it
    if (till == LEXER) {
        safec_lexer_trace = 1;
        while (yylex() != 0) {}
        return 0;
    }
    safec_lexer_trace = 0;

    // ---- 2. Parser
    safec::DiagnosticsEngine diag(file);
    safec::Program prog;
    safec::SafeCParser parser(prog, diag);
    parser.parse();
    if (diag.hasErrors()) {
        diag.print(std::cerr);
        return 1;
    }
    if (till == PARSER) {
        diag.print(std::cerr);
        safec::dumpProgram(prog, std::cout);
        return 0;
    }

    // ---- 3. Sema (includes the ownership / borrow checker)
    safec::Sema sema(diag);
    sema.run(prog);
    diag.print(std::cerr);
    if (diag.hasErrors()) return 1;
    if (till == SEMA) {
        std::printf("OK: semantic checks passed\n");
        return 0;
    }

    // ---- 4. Codegen (placeholder)
    std::printf("codegen: not implemented yet\n");
    return 0;
}
