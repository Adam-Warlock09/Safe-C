/*
 * main_parser.c - command-line driver for the parser.
 * Exit codes: 0 = parsed OK, 1 = errors, 2 = bad command line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

static void usage(void) {
    fprintf(stderr,
            "usage: safec-parser [--ast] [--trace-lexer] <file.safec>\n"
            "  --ast          print the parsed AST after a successful parse\n"
            "  --trace-lexer  keep the lexer's per-token trace prints (debug)\n");
}

int main(int argc, char **argv) {
    const char *file = NULL;
    int dump_ast = 0, trace = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ast") == 0)            dump_ast = 1;
        else if (strcmp(argv[i], "--trace-lexer") == 0) trace = 1;
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "safec-parser: unknown option '%s'\n", argv[i]);
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

    // mute the lexer trace unless -- trace-lexer
    if (!trace) safec_lexer_trace = 0;

    ParseResult r = safec_parse_file(file);

    if (r.error_count > 0) {
        for (int i = 0; i < r.error_count; i++)
            fprintf(stderr, "%s:%d: error: %s\n",
                    file, r.errors[i].line, r.errors[i].message);
        fprintf(stderr, "%s: %d error%s found\n",
                file, r.error_count, r.error_count == 1 ? "" : "s");
        ast_free(r.ast);
        return 1;
    }

    printf("OK: parsed %d top-level item(s)\n", ast_child_count(r.ast));
    if (dump_ast) {
        puts("---- AST ----");
        ast_dump(r.ast, stdout);
    }
    ast_free(r.ast);
    return 0;
}
