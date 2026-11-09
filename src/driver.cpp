/*
 * driver.cpp - runs the compiler up to a given stage and prints the result.
 *
 *   driver [options] <file.safec> [c_files...]
 *
 *   --till-lexer    print the token stream
 *   --till-parser   print the AST
 *   --till-sema     run semantic + ownership checks, print diagnostics
 *   --till-codegen  emit LLVM IR (.ll) and stop (alias: --emit-llvm)
 *   
 *   -o <file>       output binary name (default: a.out)
 *   -O2             enable optimizations during clang linkage
 *
 * Exit codes: 0 ok, 1 errors, 2 bad usage.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>

#include "parser.tab.hpp"
#include "ast_dump.h"
#include "new_ast.h"
#include "sema.h"
#include "tokens.h"
#include "codegen.h"

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

enum Stage { LEXER, PARSER, SEMA, CODEGEN, LINK };

static int usage() {
    std::fprintf(stderr,
                 "usage: safec [options] <file.safec> [c_files...]\n"
                 "Stages:\n"
                 "  --till-lexer    print token stream\n"
                 "  --till-parser   print the AST\n"
                 "  --till-sema     run semantic + ownership checks\n"
                 "  --emit-llvm     emit LLVM IR and stop (--till-codegen)\n"
                 "Build Options:\n"
                 "  -o <file>       output binary name (default: a.out)\n"
                 "  -O1, -O2, -O3   optimization levels\n");
    return 2;
}

int main(int argc, char **argv) {
    Stage till = LINK; // Default to full build pipeline
    const char *file = nullptr;
    std::string output_bin = "a.out";
    std::string clang_opts = "";
    std::string c_files = "";

    // Parse CLI arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--till-lexer")  till = LEXER;
        else if (arg == "--till-parser") till = PARSER;
        else if (arg == "--till-sema")   till = SEMA;
        else if (arg == "--till-codegen" || arg == "--emit-llvm") till = CODEGEN;
        else if (arg == "-o") {
            if (i + 1 < argc) output_bin = argv[++i];
            else return usage();
        }
        else if (arg == "-O1" || arg == "-O2" || arg == "-O3" || arg == "-g") {
            clang_opts += arg + " ";
        }
        else if (arg.length() >= 6 && (arg.substr(arg.length() - 6) == ".safec" || arg.substr(arg.length() - 3) == ".sc")) {
            file = argv[i];
        }
        else if (arg.length() >= 2 && (arg.substr(arg.length() - 2) == ".c" || arg.substr(arg.length() - 2) == ".o")) {
            c_files += arg + " ";
        }
        else if (arg[0] == '-') {
            return usage();
        }
        else {
            file = argv[i]; // Fallback if extension isn't exactly .safec/.sc
        }
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

    // ---- 4. Codegen (Emit LLVM IR)
    std::string out_file = std::string(file) + ".ll";
    std::ofstream out(out_file);
    if (!out) {
        std::cerr << "driver: cannot open " << out_file << " for writing LLVM IR\n";
        return 1;
    }

    safec::CodeGen cg(out);
    cg.generate(prog);
    out.close();

    if (till == CODEGEN) {
        std::printf("OK: Successfully emitted %s\n", out_file.c_str());
        return 0;
    }

    // ---- 5. Link (Invoke Clang)
    std::string clang_cmd = "clang " + out_file + " " + clang_opts + c_files + " -Wno-override-module -o " + output_bin;
    int link_status = std::system(clang_cmd.c_str());
    
    if (link_status != 0) {
        std::fprintf(stderr, "Linker Error: clang backend failed to compile the generated LLVM IR.\n");
        return link_status;
    }

    std::printf("OK: Successfully compiled %s -> %s\n", file, output_bin.c_str());
    return 0;
}
