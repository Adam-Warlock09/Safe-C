/*
 * Ones marked RESERVED are for the SafeC extensions
 *   - empty slots (no init, no else) get AST_ABSENT, not NULL
 *   - lines are 1-based, straight from the lexer
 */
#ifndef SAFEC_LEGACY_AST_H
#define SAFEC_LEGACY_AST_H

#include <stdio.h>

typedef enum {
    // Whole program / top level 
    AST_PROGRAM,        /* children: struct defs, function defs, global var decls */
    AST_STRUCT_DEF,      /* text = struct name; children: AST_STRUCT_FIELD* */
    AST_STRUCT_FIELD,    /* text = field name; [0] = AST_TYPE, [1] = init (absent in v1) */
    AST_VAR_DECL,        /* text = variable name; [0] = AST_TYPE, [1] = init expr or absent */
    AST_FUNCTION_DEF,    /* text = function name; [0] = AST_TYPE (return), [1] = AST_PARAM_LIST, [2] = AST_COMPOUND */

    // Declarations helpers 
    AST_PARAM_LIST,      /* children: AST_PARAM* (possibly none) */
    AST_PARAM,           /* text = parameter name; [0] = AST_TYPE */
    AST_TYPE,           /* text = base type name; see TF_* flags and ptr fields */

    // Statements 
    AST_COMPOUND,        /* children: statements in order */
    AST_IF,              /* [0] = condition, [1] = then-branch, [2] = else-branch (or absent) */
    AST_WHILE,           /* [0] = condition, [1] = body */
    AST_FOR,             /* [0] = init (block or absent), [1] = condition (or absent),
                            [2] = step (or absent), [3] = body */
    AST_SWITCH,          /* [0] = controlling expression, [1..] = AST_CASE / AST_DEFAULT */
    AST_CASE,            /* [0] = case label expression, [1..] = statements of this section */
    AST_DEFAULT,         /* children: statements of this section */
    AST_BREAK,           /* no children */
    AST_CONTINUE,        /* no children */
    AST_RETURN,          /* [0] = return value (or absent) */
    AST_EMPTY_STMT,      /* a lone ';' */
    AST_DEFER,           /* RESERVED (extensions): [0] = deferred statement */
    AST_TRUSTED,         /* RESERVED (extensions): [0] = the statement inside trusted */

    // Expressions - atoms
    AST_INT_LITERAL,     /* text = literal spelling, e.g. "10", "0x1F" */
    AST_FLOAT_LITERAL,   /* text = literal spelling */
    AST_CHAR_LITERAL,    /* text = literal spelling including quotes */
    AST_STRING_LITERAL,  /* text = literal spelling including quotes */
    AST_BOOL_LITERAL,    /* text = "true" or "false" */
    AST_NULL_LITERAL,    /* RESERVED (extensions): the null literal */
    AST_IDENTIFIER,      /* text = identifier */

    // Expressions - operators
    AST_BINARY_OP,       /* text = operator symbol; [0] = lhs, [1] = rhs */
    AST_UNARY_OP,        /* text = op symbol; [0] = operand (&mut is RESERVED) */
    AST_POSTFIX_OP,      /* text = "++" or "--"; [0] = operand */
    AST_ASSIGNMENT,      /* text = operator symbol (=, +=, ...); [0] = target, [1] = value */

    // Expressions - postfix forms
    AST_CALL,            /* [0] = callee expression, [1..] = argument expressions */
    AST_FIELD_ACCESS,    /* text = field name; [0] = base expression  (a.b) */
    AST_ARROW_ACCESS,    /* text = field name; [0] = base expression  (a->b) */
    AST_INDEX,           /* [0] = base expression, [1] = index expression (a[i]) */

    // SafeC memory ops - RESERVED (extensions)
    AST_NEW,             /* [0] = AST_TYPE of the allocated value (`new T`) */
    AST_FREE,            /* [0] = the expression consumed by free */
    AST_COMPOUND_LIT,    /* [0] = AST_TYPE, [1..] = initializers
                            (AST_DESIGNATED_INIT for `.name = expr`,
                             or plain expression nodes for positional). */
    AST_DESIGNATED_INIT, /* text = field name; [0] = value expression */

    // Placeholder for empty grammar slots (see conventions above)
    AST_ABSENT
} AstKind;

// type flags; own/mut/raw are RESERVED (extensions)
enum {
    TF_NONE        = 0,
    TF_OWN         = 1 << 0,   /* RESERVED (extensions): own T* */
    TF_MUT         = 1 << 1,   /* RESERVED (extensions): mut T* */
    TF_RAW         = 1 << 2,   /* RESERVED (extensions): raw T* */
    TF_STRUCT_KW   = 1 << 3,   /* base written with the `struct` keyword */
    TF_PRIM        = 1 << 4    /* base is a primitive keyword (int, ...) */
};

typedef struct AstNode AstNode;
struct AstNode {
    AstKind kind;
    int line;            /* 1-based source line */
    char *text;          /* kind-dependent payload, never NULL ("" if unused) */
    int flags;           /* AST_TYPE: TF_* qualifier/kind bits */
    int ptr_depth;       /* AST_TYPE: number of '*' levels (0 = plain value) */
    int nullable_mask;   /* RESERVED (extensions): bit i = i-th '*' is
                            nullable; always 0 from this parser */
    AstNode **children;
    int child_count;
};

// Create / attach / release 
AstNode *ast_new(AstKind kind, int line);
AstNode *ast_new_text(AstKind kind, int line, const char *text);
void     ast_add(AstNode *parent, AstNode *child);
AstNode *ast_clone_type(const AstNode *type);   
void     ast_free(AstNode *node);

// Utilities
const char *ast_kind_name(AstKind kind);        
int        ast_child_count(const AstNode *n);

void ast_dump(const AstNode *node, FILE *out);

#endif 
