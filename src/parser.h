/*
 *     ParseResult r = safec_parse_file("program.safec");
 *     if (r.error_count > 0) { print r.errors; }
 *     else                   { r.ast is the tree; ast_free it when done }
 */
#ifndef SAFEC_PARSER_H
#define SAFEC_PARSER_H

#include "ast.h"

#define SAFEC_MAX_ERRORS 25
#define SAFEC_MSG_MAX 256

typedef struct {
    char message[SAFEC_MSG_MAX];
    int  line;                 
} SafecError;

typedef struct {
    AstNode *ast;              /* AST_PROGRAM, or NULL if input was empty/failed */
    int error_count;
    SafecError errors[SAFEC_MAX_ERRORS];
} ParseResult;

/* 
   Parse one SafeC source file.  Never returns NULL errors on a file
   that fails to open: error_count is 1 and errors[0] describes it.
 */
ParseResult safec_parse_file(const char *path);

/* 
Defined in the Flex-generated lexer (src/lexer.l).  1 = print the per-token trace 
(standalone lexer behaviour), 0 = quiet.  The parser driver sets this to 0 before parsing.
*/
extern int safec_lexer_trace;

#endif
