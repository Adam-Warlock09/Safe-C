/*
 * driver.cpp - runs the compiler up to a given stage and prints the result.
 *
 *   safec [options] <file.safec> [inputs...]
 *
 * Stages (default: build an executable):
 *   --till-lexer    print the token stream
 *   --till-parser   print the AST
 *   --till-sema     run semantic + ownership checks, print diagnostics
 *   --emit-llvm, -S emit LLVM IR and stop (alias: --till-codegen)
 *                   written to -o, or <file>.ll next to the source;
 *                   with -O1..-O3 it is the IR after LLVM's optimizer
 *   -c              compile to an object file, don't link
 *                   written to -o, or <file>.o next to the source
 *
 * Build options:
 *   -o <file>       output file (executable default: a.out)
 *   -O0 .. -O3, -g  passed to clang
 *   -l<lib>, -L<dir> passed to the linker (also "-l lib", "-L dir")
 *   inputs          .c, .o, .a and .so files, compiled/linked with the program
 *
 * For -c and executables the IR goes to a temporary file that is removed
 * afterwards. clang is run directly (no shell), so paths may contain spaces.
 *
 * Exit codes: 0 ok, 1 errors, 2 bad usage.
 */
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

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
extern int   safec_lexer_errors;
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
    case INT_LITERAL:   yylval->num  = std::strtoull(yytext, nullptr, (yytext[1] == 'x' || yytext[1] == 'X') ? 16 : 10); break;
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

struct Options {
    Stage till = LINK;
    bool compile_only = false;               // -c
    const char *file = nullptr;              // the SafeC source
    std::string output;                      // -o, empty if not given
    std::vector<std::string> clang_flags;    // -O<n>, -g
    std::vector<std::string> inputs;         // .c .o .a .so files
    std::vector<std::string> link_flags;     // -l<lib>, -L<dir>
};

static int usage() {
    std::fprintf(stderr,
                 "usage: safec [options] <file.safec> [inputs...]\n"
                 "Stages (default: build an executable):\n"
                 "  --till-lexer     print token stream\n"
                 "  --till-parser    print the AST\n"
                 "  --till-sema      run semantic + ownership checks\n"
                 "  --emit-llvm, -S  emit LLVM IR and stop (--till-codegen)\n"
                 "  -c               compile to an object file, don't link\n"
                 "Build options:\n"
                 "  -o <file>        output file (executable default: a.out)\n"
                 "  -O0 .. -O3, -g   optimization level, debug info\n"
                 "  -l<lib>, -L<dir> libraries to link and where to find them\n"
                 "  inputs           .c, .o, .a and .so files to build and link with\n");
    return 2;
}

static int usage_error(const std::string &msg) {
    std::fprintf(stderr, "safec: %s\n", msg.c_str());
    return usage();
}

static bool ends_with(const std::string &s, const char *suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Returns -1 when the options are fine, otherwise the exit code to stop with
static int parse_args(int argc, char **argv, Options &opt) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--till-lexer")  opt.till = LEXER;
        else if (arg == "--till-parser") opt.till = PARSER;
        else if (arg == "--till-sema")   opt.till = SEMA;
        else if (arg == "--till-codegen" || arg == "--emit-llvm" || arg == "-S") opt.till = CODEGEN;
        else if (arg == "-c") opt.compile_only = true;
        else if (arg == "-o") {
            if (i + 1 >= argc) return usage_error("-o needs a file name");
            opt.output = argv[++i];
        }
        else if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3" || arg == "-g") {
            opt.clang_flags.push_back(arg);
        }
        else if (arg == "-l" || arg == "-L") {           // separate form: -l m, -L dir
            if (i + 1 >= argc) return usage_error(arg + " needs an argument");
            opt.link_flags.push_back(arg + argv[++i]);
        }
        else if (arg.size() > 2 && (arg.compare(0, 2, "-l") == 0 || arg.compare(0, 2, "-L") == 0)) {
            opt.link_flags.push_back(arg);
        }
        else if (ends_with(arg, ".c") || ends_with(arg, ".o") || ends_with(arg, ".a") || ends_with(arg, ".so")) {
            opt.inputs.push_back(arg);
        }
        else if (arg[0] == '-') {
            return usage_error("unknown option '" + arg + "'");
        }
        else {                                            // .safec, .sc, or any other name
            if (opt.file) return usage_error("only one SafeC source file can be compiled at a time");
            opt.file = argv[i];
        }
    }
    if (!opt.file) return usage_error("no SafeC source file given");
    if (opt.compile_only && opt.till != LINK)
        return usage_error("-c can't be combined with a --till/--emit-llvm stage");
    if (opt.compile_only && !opt.inputs.empty())
        return usage_error("-c compiles only the SafeC file; build the other inputs separately");
    return -1;
}

// The source path without its .safec/.sc extension, for default output names
static std::string strip_extension(const std::string &path) {
    if (ends_with(path, ".safec")) return path.substr(0, path.size() - 6);
    if (ends_with(path, ".sc"))    return path.substr(0, path.size() - 3);
    return path;
}

// A file in $TMPDIR (or /tmp) that is deleted when this goes out of scope
struct TempFile {
    std::string path;
    ~TempFile() { if (!path.empty()) std::remove(path.c_str()); }
};

static bool make_temp_file(TempFile &tmp, const char *suffix) {
    const char *dir = std::getenv("TMPDIR");
    std::string name = std::string(dir && *dir ? dir : "/tmp") + "/safec-XXXXXX" + suffix;
    std::vector<char> buf(name.begin(), name.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), static_cast<int>(std::strlen(suffix)));
    if (fd < 0) {
        std::fprintf(stderr, "safec: cannot create a temporary file in %s: %s\n",
                     dir && *dir ? dir : "/tmp", std::strerror(errno));
        return false;
    }
    close(fd);
    tmp.path = buf.data();
    return true;
}

// Runs a program with these arguments, without a shell (so file names are
// passed through exactly). Returns its exit status, or -1 if it couldn't run
// or was killed by a signal; the reason has been printed then
static int run_program(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "safec: cannot start %s: %s\n", argv[0], std::strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv.data());
        std::fprintf(stderr, "safec: cannot run %s: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            std::fprintf(stderr, "safec: waiting for %s failed: %s\n", argv[0], std::strerror(errno));
            return -1;
        }
    }
    if (WIFSIGNALED(status)) {
        std::fprintf(stderr, "safec: %s was killed by signal %d\n", argv[0], WTERMSIG(status));
        return -1;
    }
    return WEXITSTATUS(status);
}

// The optimization level asked for ("-O1".."-O3"; the last one wins, as in
// clang), or "" for none or -O0
static std::string optimization_level(const Options &opt) {
    std::string level;
    for (const std::string &f : opt.clang_flags)
        if (f.compare(0, 2, "-O") == 0) level = f;
    return level == "-O0" ? "" : level;
}

// Runs clang. Returns false (after reporting why) if it failed; clang prints
// its own errors, and 127 means it couldn't be started at all
static bool run_clang(const std::vector<std::string> &cmd, const std::string &what) {
    int status = run_program(cmd);
    if (status == 0) return true;
    if (status > 0 && status != 127)
        std::fprintf(stderr, "safec: clang failed (exit %d) while %s\n", status, what.c_str());
    return false;
}

// Writes the program's LLVM IR to path. Returns false (after reporting why) on failure
static bool write_ir(const safec::Program &prog, const char *file, const std::string &path) {
    {
        safec::DiagnosticsEngine cg_diag(file);   // so sema's warnings aren't printed twice
        if (!safec::CodeGen::check_reserved_names(prog, cg_diag)) {
            cg_diag.print(std::cerr);
            return false;
        }
    }

    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "safec: cannot open %s for writing LLVM IR\n", path.c_str());
        return false;
    }

    // codegen throws only on programs sema should have rejected: a compiler bug
    try {
        safec::CodeGen cg(out);
        cg.generate(prog);
    } catch (const std::exception &e) {
        out.close();
        std::remove(path.c_str());          // don't leave half-written IR behind
        std::fprintf(stderr, "%s: internal compiler error in codegen: %s\n", file, e.what());
        return false;
    }

    out.close();
    if (!out) {
        std::fprintf(stderr, "safec: writing %s failed\n", path.c_str());
        std::remove(path.c_str());
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    Options opt;
    int bad_usage = parse_args(argc, argv, opt);
    if (bad_usage >= 0) return bad_usage;
    const char *file = opt.file;

    yyin = std::fopen(file, "r");
    if (!yyin) {
        std::fprintf(stderr, "safec: cannot open file '%s'\n", file);
        return 1;
    }

    // ---- 1. Lexer: the lexer prints each token as it scans it
    if (opt.till == LEXER) {
        safec_lexer_trace = 1;
        while (yylex() != 0) {}
        return safec_lexer_errors > 0 ? 1 : 0;
    }
    safec_lexer_trace = 0;

    // ---- 2. Parser
    safec::DiagnosticsEngine diag(file);
    safec::Program prog;
    safec::SafeCParser parser(prog, diag);
    parser.parse();
    if (diag.hasErrors() || safec_lexer_errors > 0) {
        diag.print(std::cerr);
        return 1;
    }
    if (opt.till == PARSER) {
        diag.print(std::cerr);
        safec::dumpProgram(prog, std::cout);
        return 0;
    }

    // ---- 3. Sema (includes the ownership / borrow checker)
    safec::Sema sema(diag);
    sema.run(prog);
    diag.print(std::cerr);
    if (diag.hasErrors()) return 1;
    if (opt.till == SEMA) {
        std::printf("OK: semantic checks passed\n");
        return 0;
    }

    // ---- 4. Codegen: LLVM IR, kept only when it is what was asked for
    if (opt.till == CODEGEN) {
        std::string ir_path = opt.output.empty() ? strip_extension(file) + ".ll" : opt.output;
        std::string level = optimization_level(opt);
        if (level.empty()) {                    // the IR exactly as codegen made it
            if (!write_ir(prog, file, ir_path)) return 1;
            std::printf("OK: Successfully emitted %s\n", ir_path.c_str());
            return 0;
        }
        // -O1..-O3: codegen's IR goes to a temporary file and LLVM's optimizer
        // (run by clang, as for a binary) writes the optimized IR
        TempFile unoptimized;
        if (!make_temp_file(unoptimized, ".ll")) return 1;
        if (!write_ir(prog, file, unoptimized.path)) return 1;
        if (!run_clang({"clang", "-S", "-emit-llvm", level, unoptimized.path, "-o", ir_path},
                       "optimizing the IR into " + ir_path))
            return 1;
        std::printf("OK: Successfully emitted %s (optimized with %s)\n", ir_path.c_str(), level.c_str());
        return 0;
    }

    TempFile ir;
    if (!make_temp_file(ir, ".ll")) return 1;
    if (!write_ir(prog, file, ir.path)) return 1;

    // ---- 5. clang: object file (-c) or linked executable
    std::string target = !opt.output.empty() ? opt.output
                       : opt.compile_only    ? strip_extension(file) + ".o"
                       :                       std::string("a.out");
    std::vector<std::string> cmd = {"clang"};
    if (opt.compile_only) cmd.push_back("-c");
    cmd.push_back(ir.path);
    cmd.insert(cmd.end(), opt.clang_flags.begin(), opt.clang_flags.end());
    cmd.insert(cmd.end(), opt.inputs.begin(), opt.inputs.end());
    if (!opt.compile_only)   // after the inputs that use them; meaningless without linking
        cmd.insert(cmd.end(), opt.link_flags.begin(), opt.link_flags.end());
    cmd.push_back("-o");
    cmd.push_back(target);

    if (!run_clang(cmd, "building " + target)) return 1;

    std::printf("OK: Successfully compiled %s -> %s\n", file, target.c_str());
    return 0;
}
