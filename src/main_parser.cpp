/*
 * main_parser.cpp - driver + scanner bridge for the parser (parser.y).
 * safec::yylex feeds the flex lexer into bison; parseFile() is what
 * the later stages call.  Exit codes: 0 ok, 1 errors, 2 bad usage.
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
#include "tokens.h"

// the flex lexer, compiled as C
extern "C" {
int    yylex(void);
extern char *yytext;
extern int    yylineno;
extern FILE *yyin;
extern int   safec_lexer_trace;
}

namespace safec {
namespace {

// decode one literal char, handling escapes
char decodeChar(const char *&p) {
    if (*p == '\\' && p[1] != '\0') {
        ++p;
        char c = *p++;
        switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        default:  return c;    // \\ ' " and unknown escapes: as-is
        }
    }
    return *p++;
}

} // namespace

// the parser calls this: run flex, decode the token text into a value.
// flex returns tokens.h numbers, which is what bison expects; 0 = eof.
int yylex(SafeCParser::semantic_type *yylval, SafeCParser::location_type *yylloc)
{
    int tok = ::yylex();       // flex's yylex, not this one

    // no columns from the lexer yet
    yylloc->begin.line = yylineno;
    yylloc->end.line   = yylineno;

    switch (tok) {
    case IDENTIFIER:
        yylval->str = new std::string(yytext);
        break;
    case INT_LITERAL:      // base 0 handles 0x hex too
        yylval->num = std::strtoull(yytext, nullptr, 0);
        break;
    case FLOAT_LITERAL:    // strtod stops before the f/F suffix
        yylval->fnum = std::strtod(yytext, nullptr);
        break;
    case CHAR_LITERAL: {
        const char *p = yytext + 1;             // skip the opening quote
        yylval->ch = decodeChar(p);
        break;
    }
    case STRING_LITERAL: {
        std::string out;
        const char *p = yytext + 1;             // skip the opening quote
        while (*p != '\0' && *p != '"')
            out += decodeChar(p);
        yylval->str = new std::string(std::move(out));
        break;
    }
    case BOOL_LITERAL:    // "true" / "false" share one token code
        yylval->bval = (yytext[0] == 't');
        break;
    default:
        break;           // keywords and punctuation carry no value
    }
    return tok;
}

// syntax errors go through the diagnostics engine, same as everything else
void SafeCParser::error(const location_type &loc, const std::string &msg) {
    diag.error(SourceLoc(loc.begin.line, 0), msg);
}

// parse one file into a Program; stops at the first error (no recovery
// rules yet).  nullptr only if the file can't be opened.
std::unique_ptr<Program> parseFile(const char *path, DiagnosticsEngine &diag) {
    FILE *f = std::fopen(path, "r");
    if (!f) {
        diag.error(SourceLoc(0, 0),
                   std::string("cannot open file '") + path + "'");
        return nullptr;
    }
    yyin = f;

    auto prog = std::make_unique<Program>();
    SafeCParser parser(*prog, diag);
    parser.parse();

    std::fclose(f);
    return prog;
}

} // namespace safec

static void usage(void) {
    std::fprintf(stderr,
                 "usage: safec-parser [--ast] [--trace-lexer] <file.safec>\n"
                 "  --ast          print the parsed AST after a successful parse\n"
                 "  --trace-lexer  keep the lexer's per-token trace prints (debug)\n");
}

int main(int argc, char **argv) {
    const char *file = NULL;
    int dump_ast = 0, trace = 0;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--ast") == 0)             dump_ast = 1;
        else if (std::strcmp(argv[i], "--trace-lexer") == 0) trace = 1;
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            std::fprintf(stderr, "safec-parser: unknown option '%s'\n", argv[i]);
            usage();
            return 2;
        } else {
            file = argv[i];
        }
    }
    if (!file) {
        usage();
        return 2;
    }

    // mute the lexer trace unless --trace-lexer
    if (!trace) safec_lexer_trace = 0;

    safec::DiagnosticsEngine diag(file);
    std::unique_ptr<safec::Program> prog = safec::parseFile(file, diag);

    diag.print(std::cerr);
    if (!prog || diag.hasErrors()) return 1;

    std::printf("OK: parsed %zu top-level item(s)\n", prog->decls.size());
    if (dump_ast) {
        std::puts("---- AST ----");
        safec::dumpProgram(*prog, std::cout);
    }
    return 0;
}
