/*
 * parser.c - the parser for SafeC.
 *
 * SafeC-only stuff (own/mut/raw, '?', new, free, defer, trusted, null,
 * &mut) - when we see one of those tokens we say
 * so with a clear "SafeC extension" error instead of parsing it.
 *
 * Grammar in short:
 *   program      := (struct_def | function_def | declaration)*
 *   struct_def   := 'struct' IDENT '{' field* '}' ';'
 *   function_def := type IDENT '(' params? ')' compound
 *   declaration  := type declarator (',' declarator)* ';'
 *   declarator   := '*'* IDENT ('[' expr? ']')* ('=' assignment)?
 *   type         := 'void'|'bool'|'char'|'int'|'float'|IDENT|'struct' IDENT
 *   statement    := compound|if|while|for|switch|return|break|continue
 *                 |';'|declaration|expr';'
 *   expression   := assignment (no comma operator)
 *   binary, loosest first: || && | ^ & == != < > <= >= << >> + - * / %
 *   unary        := ('-'|'+'|'!'|'~'|'*'|'&'|'++'|'--') unary | postfix
 *   postfix      := primary ('(' args ')' | '[' e ']' | '.' ID
 *                 | '->' ID | '++' | '--')*
 *   primary      := literal | IDENT | '(' expr ')'
 *                 | '(' type ')' '{' inits '}'    <- compound literal
 *
 * In SafeC, '?' is used for nullable types, not for the ?: operator.
 * If there's an error, skip to the next ';' or '}' and keep parsing
 * so we can catch more errors in one run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "tokens.h"
#include "parser.h"
#include "ast.h"

// from the flex lexer
extern int    yylex(void);
extern char  *yytext;
extern int    yylineno;
extern FILE  *yyin;

// token buffer over yylex()

#define PEEK_BUF 8

typedef struct {
    int   kind;   /* token code; EOF_TOKEN at the end */
    char *text;   /* copy of yytext */
    int   line;   /* line number from yylineno */
} Token;

typedef struct Parser {
    Token  buf[PEEK_BUF];
    int    count;
    long   adv_total;   /* tokens eaten so far (recovery checks this) */
    int    in_error;    /* set once an error happens; unwinds */
    int    give_up;     /* too many errors, stop */
    ParseResult *res;
    char **types;       /* struct names seen so far */
    int    type_count, type_cap;
} Parser;

static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) { fprintf(stderr, "safec: out of memory\n"); exit(1); }
    return p;
}

static void fill(Parser *p) {
    if (p->count >= PEEK_BUF) return;
    if (p->count > 0 && p->buf[p->count - 1].kind == EOF_TOKEN) return; /* sticky EOF */
    int kind = yylex();
    Token t;
    t.kind = kind;
    t.line = yylineno;
    t.text = xstrdup(kind == EOF_TOKEN ? "<end of file>" : yytext);
    p->buf[p->count++] = t;
}

static Token *peek(Parser *p, int k) {
    while (p->count <= k) fill(p);
    return &p->buf[k];
}

static Token *cur(Parser *p) { return peek(p, 0); }

// eat one token; EOF is never eaten, so loops still end
static void advance(Parser *p) {
    if (p->count == 0) fill(p);
    if (p->buf[0].kind == EOF_TOKEN) return;
    free(p->buf[0].text);
    for (int i = 1; i < p->count; i++) p->buf[i - 1] = p->buf[i];
    p->count--;
    p->adv_total++;
}

// eat the token, return its text 
static char *advance_take(Parser *p) {
    Token *t = cur(p);
    char *s = xstrdup(t->text);
    advance(p);
    return s;
}

// errors

static void perr(Parser *p, const char *fmt, ...) {
    ParseResult *res = p->res;
    if (p->give_up) return;
    if (res->error_count >= SAFEC_MAX_ERRORS - 1) {
        res->errors[res->error_count].line = p->count > 0 ? p->buf[0].line : 1;
        snprintf(res->errors[res->error_count].message, SAFEC_MSG_MAX,
                 "too many errors; stopping");
        res->error_count++;
        p->give_up = 1;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res->errors[res->error_count].message, SAFEC_MSG_MAX, fmt, ap);
    va_end(ap);
    res->errors[res->error_count].line = p->count > 0 ? p->buf[0].line : 1;
    res->error_count++;
    p->in_error = 1;
}

static int bad(Parser *p) { return p->in_error; }

// nice token names for error messages
static const char *tokdesc(const Token *t) {
    static char buf[160];
    switch (t->kind) {
    case EOF_TOKEN:      return "the end of the file";
    case IDENTIFIER:     snprintf(buf, sizeof buf, "identifier '%s'", t->text); return buf;
    case INT_LITERAL:    snprintf(buf, sizeof buf, "integer literal '%s'", t->text); return buf;
    case FLOAT_LITERAL:  snprintf(buf, sizeof buf, "float literal '%s'", t->text); return buf;
    case CHAR_LITERAL:   snprintf(buf, sizeof buf, "character literal %s", t->text); return buf;
    case STRING_LITERAL: snprintf(buf, sizeof buf, "string literal %.40s%s", t->text,
                                  strlen(t->text) > 40 ? "..." : ""); return buf;
    case BOOL_LITERAL:   snprintf(buf, sizeof buf, "boolean literal '%s'", t->text); return buf;
    default:             snprintf(buf, sizeof buf, "'%s'", t->text); return buf;
    }
}

static int expect_tok(Parser *p, int kind, const char *what) {
    Token *t = cur(p);
    if (t->kind == kind) { advance(p); return 1; }
    perr(p, "expected %s but got %s", what, tokdesc(t));
    return 0;
}

static char *expect_text(Parser *p, int kind, const char *what) {
    Token *t = cur(p);
    if (t->kind == kind) return advance_take(p);
    perr(p, "expected %s but got %s", what, tokdesc(t));
    return NULL;
}

// struct type names 

/* 
struct names are types here (`List list = ...`), so remember them;
that's how `List * p;` (decl) differs from `a * b;` (multiply) 
 */
static void register_type(Parser *p, const char *name) {
    if (p->type_count == p->type_cap) {
        p->type_cap = p->type_cap ? p->type_cap * 2 : 8;
        char **grown = realloc(p->types, (size_t)p->type_cap * sizeof *grown);
        if (!grown) { fprintf(stderr, "safec: out of memory\n"); exit(1); }
        p->types = grown;
    }
    p->types[p->type_count++] = xstrdup(name);
}

static int is_type_name(Parser *p, const char *name) {
    for (int i = 0; i < p->type_count; i++)
        if (strcmp(p->types[i], name) == 0) return 1;
    return 0;
}

// helpers 

static int is_prim_kind(int k) {
    switch (k) {
    case VOID: case BOOL: case CHAR: case INT: case FLOAT: return 1;
    default: return 0;
    }
}

static int is_assign_op(int k) {
    switch (k) {
    case ASSIGN: case ADD_ASSIGN: case SUB_ASSIGN: case MUL_ASSIGN:
    case DIV_ASSIGN: case MOD_ASSIGN: case AND_ASSIGN: case XOR_ASSIGN:
    case OR_ASSIGN: case LEFT_SHIFT_ASSIGN: case RIGHT_SHIFT_ASSIGN:
        return 1;
    default: return 0;
    }
}

// SafeC extension guard

/* 
these tokens are safe-C extensions; we error out clearly instead of parsing them
this is also where safe-C rules comes in
 */
static int safec_extension_tok(int k) {
    switch (k) {
    case OWN_KEYWORD: case MUT_KEYWORD: case RAW_KEYWORD:
    case NEW_KEYWORD: case FREE_KEYWORD: case DEFER_KEYWORD:
    case TRUSTED_KEYWORD: case NULL_KEYWORD:
        return 1;
    default:
        return 0;
    }
}

// if this token is an extension, say and return 1 
static int perr_if_safec_extension(Parser *p) {
    Token *t = cur(p);
    if (t->kind == NULLABLE_PTR) {
        perr(p, "'?' (the SafeC nullable pointer suffix) is a SafeC "
                "extension, not part of the common C-like grammar "
                "(common pointers are written T*)");
        return 1;
    }
    if (safec_extension_tok(t->kind)) {
        perr(p, "'%s' is a SafeC extension, outside the common C-like "
                "grammar implemented by this parser (the SafeC "
                "extensions module owns this rule)", t->text);
        return 1;
    }
    return 0;
}

// does a declaration start here
static int decl_start(Parser *p) {
    int k = cur(p)->kind;
    switch (k) {
    case VOID: case BOOL: case CHAR: case INT: case FLOAT: case STRUCT:
        return 1;
    case IDENTIFIER:
        /* `List * p` = decl, `a * b` = multiply; registry decides */
        return is_type_name(p, cur(p)->text)
            && (peek(p, 1)->kind == IDENTIFIER || peek(p, 1)->kind == MULTIPLY);
    default:
        return 0;
    }
}

// type base at lookahead k
static int type_base_ahead(Parser *p, int k) {
    Token *t = peek(p, k);
    switch (t->kind) {
    case VOID: case BOOL: case CHAR: case INT: case FLOAT: case STRUCT:
        return 1;
    case IDENTIFIER:
        return is_type_name(p, t->text);
    default:
        return 0;
    }
}

// forward decls

static AstNode *parse_expression(Parser *p);
static AstNode *parse_assignment(Parser *p);
static AstNode *parse_binary(Parser *p, int level);
static AstNode *parse_unary(Parser *p);
static AstNode *parse_postfix(Parser *p);
static AstNode *parse_primary(Parser *p);
static void     parse_init_items(Parser *p, AstNode *lit);
static AstNode *parse_compound(Parser *p);
static void     parse_statement_into(Parser *p, AstNode *sink, int allow_decl);
static AstNode *parse_single(Parser *p);
static void     parse_declaration_into(Parser *p, AstNode *sink, int in_struct);
static int      parse_array_suffixes(Parser *p, AstNode *type);

// types

static AstNode *parse_type_base(Parser *p) {
    Token *t = cur(p);
    int line = t->line;
    switch (t->kind) {
    case VOID: case BOOL: case CHAR: case INT: case FLOAT: {
        char *txt = advance_take(p);
        AstNode *type = ast_new_text(AST_TYPE, line, txt);
        type->flags |= TF_PRIM;
        free(txt);
        return type;
    }
    case STRUCT: {
        advance(p);
        char *name = expect_text(p, IDENTIFIER, "a struct name");
        if (!name) return NULL;
        AstNode *type = ast_new_text(AST_TYPE, line, name);
        type->flags |= TF_STRUCT_KW;
        free(name);
        return type;
    }
    case IDENTIFIER: {
        if (!is_type_name(p, t->text)) {
            perr(p, "unknown type name '%s' (struct types must be defined "
                    "with 'struct %s { ... };' before they are used)",
                 t->text, t->text);
            return NULL;
        }
        char *txt = advance_take(p);
        AstNode *type = ast_new_text(AST_TYPE, line, txt);
        free(txt);
        return type;
    }
    default:
        if (perr_if_safec_extension(p)) return NULL;   /* own/mut/raw/... */
        perr(p, "expected a type name but got %s", tokdesc(t));
        return NULL;
    }
}

// just the base; stars come later, per declarator
static AstNode *parse_type_full(Parser *p) {
    return parse_type_base(p);
}

/* stars for one declarator; '?' is an extension, so no */
static void parse_pointer_suffixes(Parser *p, AstNode *type) {
    while (!bad(p) && cur(p)->kind == MULTIPLY) {
        advance(p);
        if (cur(p)->kind == NULLABLE_PTR) {
            perr(p, "'?' (the SafeC nullable pointer suffix) is a SafeC "
                    "extension, not part of the common C-like grammar "
                    "(common pointers are written T*)");
            return;
        }
        type->ptr_depth++;
    }
}

// array dims: '[' expr? ']'
static int parse_array_suffixes(Parser *p, AstNode *type) {
    while (!bad(p) && cur(p)->kind == LEFT_BRACKET) {
        advance(p);
        AstNode *dim;
        if (cur(p)->kind == RIGHT_BRACKET) {
            dim = ast_new(AST_ABSENT, cur(p)->line);   /* `int a[]` */
        } else {
            dim = parse_expression(p);
            if (bad(p) || !dim) return 0;
        }
        if (!expect_tok(p, RIGHT_BRACKET, "']'")) { ast_free(dim); return 0; }
        ast_add(type, dim);
    }
    return 1;
}

// expressions 


typedef struct { int ops[4]; int n; } Level;
static const Level LEVELS[] = {
    {{LOGICAL_OR}, 1},
    {{LOGICAL_AND}, 1},
    {{BITWISE_OR}, 1},
    {{BITWISE_XOR}, 1},
    {{BITWISE_AND}, 1},
    {{EQUAL, NOT_EQUAL}, 2},
    {{LESS_THAN, GREATER_THAN, LESS_EQUAL, GREATER_EQUAL}, 4},
    {{LEFT_SHIFT, RIGHT_SHIFT}, 2},
    {{PLUS, MINUS}, 2},
    {{MULTIPLY, DIVIDE, MODULO}, 3},
};
#define N_LEVELS ((int)(sizeof LEVELS / sizeof LEVELS[0]))

static AstNode *parse_expression(Parser *p) {
    return parse_assignment(p);
}

static AstNode *parse_assignment(Parser *p) {
    AstNode *left = parse_binary(p, 0);
    if (bad(p) || !left) return NULL;
    if (is_assign_op(cur(p)->kind)) {
        int line = cur(p)->line;
        char *op = advance_take(p);
        AstNode *right = parse_assignment(p);   /* right-associative */
        if (bad(p) || !right) { free(op); return NULL; }
        AstNode *n = ast_new_text(AST_ASSIGNMENT, line, op);
        free(op);
        ast_add(n, left);
        ast_add(n, right);
        return n;
    }
    return left;
}

static AstNode *parse_binary(Parser *p, int level) {
    if (level >= N_LEVELS) return parse_unary(p);
    AstNode *left = parse_binary(p, level + 1);
    if (bad(p) || !left) return NULL;
    const Level *lv = &LEVELS[level];
    while (!bad(p)) {
        int matched = 0;
        for (int i = 0; i < lv->n; i++)
            if (cur(p)->kind == lv->ops[i]) matched = 1;
        if (!matched) break;
        int line = cur(p)->line;
        char *op = advance_take(p);
        AstNode *right = parse_binary(p, level + 1);
        if (bad(p) || !right) { free(op); return NULL; }
        AstNode *b = ast_new_text(AST_BINARY_OP, line, op);
        free(op);
        ast_add(b, left);
        ast_add(b, right);
        left = b;
    }
    return left;
}

static AstNode *parse_unary(Parser *p) {
    Token *t = cur(p);
    int line = t->line;
    switch (t->kind) {
    case MINUS: case PLUS: case LOGICAL_NOT: case BITWISE_NOT:
    case INCREMENT: case DECREMENT: case MULTIPLY: {
        char *op = advance_take(p);
        AstNode *operand = parse_unary(p);
        if (bad(p) || !operand) { free(op); return NULL; }
        AstNode *n = ast_new_text(AST_UNARY_OP, line, op);
        free(op);
        ast_add(n, operand);
        return n;
    }
    case BITWISE_AND: {
        if (peek(p, 1)->kind == MUT_KEYWORD) {
            /* &mut is an extension */
            perr(p, "'&mut' (mutable address-of) is a SafeC extension, "
                    "outside the common C-like grammar (the common "
                    "grammar has plain address-of '&')");
            return NULL;
        }
        char *op = advance_take(p);                  /* &x */
        AstNode *operand = parse_unary(p);
        if (bad(p) || !operand) { free(op); return NULL; }
        AstNode *n = ast_new_text(AST_UNARY_OP, line, op);
        free(op);
        ast_add(n, operand);
        return n;
    }
    default:
        return parse_postfix(p);
    }
}

static AstNode *parse_postfix(Parser *p) {
    AstNode *e = parse_primary(p);
    if (bad(p) || !e) return NULL;
    while (!bad(p)) {
        Token *t = cur(p);
        int line = t->line;
        switch (t->kind) {
        case LEFT_PAREN: {                       /* function call */
            advance(p);
            AstNode *call = ast_new(AST_CALL, line);
            ast_add(call, e);
            if (cur(p)->kind != RIGHT_PAREN) {
                for (;;) {
                    AstNode *arg = parse_assignment(p);
                    if (bad(p) || !arg) { ast_free(call); return NULL; }
                    ast_add(call, arg);
                    if (cur(p)->kind == COMMA) { advance(p); continue; }
                    break;
                }
            }
            if (!expect_tok(p, RIGHT_PAREN, "')' to close the argument list")) {
                ast_free(call);
                return NULL;
            }
            e = call;
            break;
        }
        case LEFT_BRACKET: {                     /* indexing */
            advance(p);
            AstNode *idx = parse_expression(p);
            if (bad(p) || !idx) return NULL;
            if (!expect_tok(p, RIGHT_BRACKET, "']'")) return NULL;
            AstNode *n = ast_new(AST_INDEX, line);
            ast_add(n, e);
            ast_add(n, idx);
            e = n;
            break;
        }
        case DOT: case ARROW: {                 /* field access */
            int is_arrow = (t->kind == ARROW);
            advance(p);
            char *fname = expect_text(p, IDENTIFIER, "a field name");
            if (!fname) return NULL;
            AstNode *n = ast_new_text(is_arrow ? AST_ARROW_ACCESS
                                               : AST_FIELD_ACCESS, line, fname);
            free(fname);
            ast_add(n, e);
            e = n;
            break;
        }
        case INCREMENT: case DECREMENT: {       /* postfix ++ / -- */
            char *op = advance_take(p);
            AstNode *n = ast_new_text(AST_POSTFIX_OP, line, op);
            free(op);
            ast_add(n, e);
            e = n;
            break;
        }
        default:
            return e;
        }
    }
    return e;
}

static AstNode *parse_primary(Parser *p) {
    Token *t = cur(p);
    int line = t->line;
    switch (t->kind) {
    case INT_LITERAL: case FLOAT_LITERAL: case CHAR_LITERAL:
    case STRING_LITERAL: case BOOL_LITERAL: {
        AstKind k = (t->kind == INT_LITERAL)    ? AST_INT_LITERAL
                  : (t->kind == FLOAT_LITERAL)  ? AST_FLOAT_LITERAL
                  : (t->kind == CHAR_LITERAL)  ? AST_CHAR_LITERAL
                  : (t->kind == STRING_LITERAL) ? AST_STRING_LITERAL
                                               : AST_BOOL_LITERAL;
        char *txt = advance_take(p);
        AstNode *n = ast_new_text(k, line, txt);
        free(txt);
        return n;
    }
    case IDENTIFIER: {
        char *txt = advance_take(p);
        AstNode *n = ast_new_text(AST_IDENTIFIER, line, txt);
        free(txt);
        return n;
    }
    case LEFT_PAREN: {
        /* (Type){...} compound literal or plain parens? peek 3 ahead */
        if (type_base_ahead(p, 1)
            && peek(p, 2)->kind == RIGHT_PAREN
            && peek(p, 3)->kind == LEFT_BRACE) {
            advance(p);                          /* '(' */
            AstNode *type = parse_type_full(p);
            if (bad(p) || !type) return NULL;
            if (!expect_tok(p, RIGHT_PAREN, "')'")) return NULL;
            AstNode *lit = ast_new(AST_COMPOUND_LIT, line);
            ast_add(lit, type);
            parse_init_items(p, lit);
            if (bad(p)) { ast_free(lit); return NULL; }
            return lit;
        }
        advance(p);                              /* '(' */
        AstNode *e = parse_expression(p);
        if (bad(p) || !e) return NULL;
        if (!expect_tok(p, RIGHT_PAREN, "')'")) return NULL;
        return e;
    }
    default:
        if (perr_if_safec_extension(p)) return NULL;  /* null/new/free/... */
        perr(p, "expected an expression but got %s", tokdesc(t));
        return NULL;
    }
}

// { .name = expr, ... } - items get appended to lit
static void parse_init_items(Parser *p, AstNode *lit) {
    if (!expect_tok(p, LEFT_BRACE, "'{'")) return;
    if (cur(p)->kind == RIGHT_BRACE) {
        perr(p, "an initializer list cannot be empty");
        return;
    }
    for (;;) {
        if (bad(p)) return;
        int line = cur(p)->line;
        if (cur(p)->kind == DOT) {
            advance(p);
            char *fname = expect_text(p, IDENTIFIER, "a field name");
            if (!fname) return;
            if (!expect_tok(p, ASSIGN, "'=' in the designated initializer")) {
                free(fname);
                return;
            }
            AstNode *val = parse_assignment(p);
            if (bad(p) || !val) { free(fname); return; }
            AstNode *d = ast_new_text(AST_DESIGNATED_INIT, line, fname);
            free(fname);
            ast_add(d, val);
            ast_add(lit, d);
        } else {
            AstNode *val = parse_assignment(p);
            if (bad(p) || !val) return;
            ast_add(lit, val);
        }
        if (cur(p)->kind == COMMA) { advance(p); continue; }
        break;
    }
    expect_tok(p, RIGHT_BRACE, "'}' to close the initializer list");
}

// statements

static void sync_stmt(Parser *p);

static AstNode *parse_compound(Parser *p) {
    int open_line = cur(p)->line;
    advance(p);                                  /* '{' */
    AstNode *block = ast_new(AST_COMPOUND, open_line);
    for (;;) {
        if (p->give_up) break;
        int k = cur(p)->kind;
        if (k == RIGHT_BRACE) { advance(p); return block; }
        if (k == EOF_TOKEN) {
            perr(p, "missing '}' to close the block opened at line %d", open_line);
            return block;
        }
        p->in_error = 0;                          /* fresh attempt */
        long before = p->adv_total;
        parse_statement_into(p, block, 1);
        if (p->in_error) {
            sync_stmt(p);
            p->in_error = 0;
            if (p->adv_total == before) advance(p);   /* always make progress */
        }
    }
    return block;
}

// one statement, e.g. an if's body; bare declarations aren't allowed
static AstNode *parse_single(Parser *p) {
    AstNode *tmp = ast_new(AST_COMPOUND, cur(p)->line);
    parse_statement_into(p, tmp, 0);
    if (p->in_error || tmp->child_count == 0) {
        ast_free(tmp);
        return NULL;
    }
    AstNode *first = tmp->children[0];
    for (int i = 1; i < tmp->child_count; i++)  
        ast_free(tmp->children[i]);
    free(tmp->children);
    free(tmp->text);
    free(tmp);
    return first;
}

// case/default sections inside a switch body 
static int parse_switch_sections(Parser *p, AstNode *sw, int open_line) {
    for (;;) {
        if (p->give_up) return 0;
        int k = cur(p)->kind;
        if (k == RIGHT_BRACE) { advance(p); return 1; }
        if (k == EOF_TOKEN) {
            perr(p, "missing '}' to close the switch opened at line %d", open_line);
            return 0;
        }
        p->in_error = 0;
        long before = p->adv_total;

        if (k == CASE || k == DEFAULT) {
            int line = cur(p)->line;
            AstNode *sec = ast_new(k == CASE ? AST_CASE : AST_DEFAULT, line);
            advance(p);
            if (k == CASE) {
                AstNode *label = parse_expression(p);
                if (p->in_error || !label) { ast_free(sec); goto recover; }
                ast_add(sec, label);
            }
            if (!expect_tok(p, COLON, "':' after the label")) { ast_free(sec); goto recover; }
            for (;;) {
                int kk = cur(p)->kind;
                if (kk == CASE || kk == DEFAULT || kk == RIGHT_BRACE
                    || kk == EOF_TOKEN || p->give_up) break;
                p->in_error = 0;
                long b2 = p->adv_total;
                parse_statement_into(p, sec, 1);
                if (p->in_error) {
                    sync_stmt(p);
                    p->in_error = 0;
                    if (p->adv_total == b2) advance(p);
                }
            }
            ast_add(sw, sec);
            continue;
        }

        perr(p, "expected 'case' or 'default' inside the switch body but got %s",
             tokdesc(cur(p)));
    recover:
        sync_stmt(p);
        p->in_error = 0;
        if (p->adv_total == before) advance(p);
    }
}

static void parse_statement_into(Parser *p, AstNode *sink, int allow_decl) {
    Token *t = cur(p);
    int line = t->line;

    switch (t->kind) {
    case LEFT_BRACE: {
        AstNode *b = parse_compound(p);
        if (!p->in_error && b) ast_add(sink, b);
        return;
    }
    case IF: {
        advance(p);
        if (!expect_tok(p, LEFT_PAREN, "'(' after 'if'")) return;
        AstNode *cond = parse_expression(p);
        if (p->in_error || !cond) return;
        if (!expect_tok(p, RIGHT_PAREN, "')' after the if condition")) return;
        AstNode *thenb = parse_single(p);
        if (p->in_error || !thenb) return;
        AstNode *elseb = NULL;
        if (cur(p)->kind == ELSE) {
            advance(p);
            elseb = parse_single(p);
            if (p->in_error || !elseb) return;
        }
        AstNode *n = ast_new(AST_IF, line);
        ast_add(n, cond);
        ast_add(n, thenb);
        ast_add(n, elseb ? elseb : ast_new(AST_ABSENT, line));
        ast_add(sink, n);
        return;
    }
    case WHILE: {
        advance(p);
        if (!expect_tok(p, LEFT_PAREN, "'(' after 'while'")) return;
        AstNode *cond = parse_expression(p);
        if (p->in_error || !cond) return;
        if (!expect_tok(p, RIGHT_PAREN, "')' after the while condition")) return;
        AstNode *body = parse_single(p);
        if (p->in_error || !body) return;
        AstNode *n = ast_new(AST_WHILE, line);
        ast_add(n, cond);
        ast_add(n, body);
        ast_add(sink, n);
        return;
    }
    case FOR: {
        advance(p);
        if (!expect_tok(p, LEFT_PAREN, "'(' after 'for'")) return;
        AstNode *init;
        if (cur(p)->kind == SEMICOLON) {
            advance(p);
            init = ast_new(AST_ABSENT, line);
        } else if (decl_start(p)) {
            init = ast_new(AST_COMPOUND, cur(p)->line);
            parse_declaration_into(p, init, 0);   /* consumes the ';' */
            if (p->in_error) return;
        } else {
            int l = cur(p)->line;
            AstNode *e = parse_expression(p);
            if (p->in_error || !e) return;
            if (!expect_tok(p, SEMICOLON, "';' in the for header")) return;
            init = ast_new(AST_COMPOUND, l);
            ast_add(init, e);
        }
        AstNode *cond;
        if (cur(p)->kind == SEMICOLON) {
            advance(p);
            cond = ast_new(AST_ABSENT, line);
        } else {
            cond = parse_expression(p);
            if (p->in_error || !cond) return;
            if (!expect_tok(p, SEMICOLON, "';' in the for header")) return;
        }
        AstNode *step;
        if (cur(p)->kind == RIGHT_PAREN) {
            step = ast_new(AST_ABSENT, line);
        } else {
            step = parse_expression(p);
            if (p->in_error || !step) return;
        }
        if (!expect_tok(p, RIGHT_PAREN, "')' after the for header")) return;
        AstNode *body = parse_single(p);
        if (p->in_error || !body) return;
        AstNode *n = ast_new(AST_FOR, line);
        ast_add(n, init);
        ast_add(n, cond);
        ast_add(n, step);
        ast_add(n, body);
        ast_add(sink, n);
        return;
    }
    case SWITCH: {
        advance(p);
        if (!expect_tok(p, LEFT_PAREN, "'(' after 'switch'")) return;
        AstNode *expr = parse_expression(p);
        if (p->in_error || !expr) return;
        if (!expect_tok(p, RIGHT_PAREN, "')' after the switch expression")) return;
        if (!expect_tok(p, LEFT_BRACE, "'{' to open the switch body")) return;
        AstNode *sw = ast_new(AST_SWITCH, line);
        ast_add(sw, expr);
        int ok = parse_switch_sections(p, sw, line);
        if (p->in_error || !ok) { ast_free(sw); return; }
        ast_add(sink, sw);
        return;
    }
    case BREAK:
        advance(p);
        if (!expect_tok(p, SEMICOLON, "';' after 'break'")) return;
        ast_add(sink, ast_new(AST_BREAK, line));
        return;
    case CONTINUE:
        advance(p);
        if (!expect_tok(p, SEMICOLON, "';' after 'continue'")) return;
        ast_add(sink, ast_new(AST_CONTINUE, line));
        return;
    case RETURN: {
        advance(p);
        AstNode *n = ast_new(AST_RETURN, line);
        if (cur(p)->kind == SEMICOLON) {
            ast_add(n, ast_new(AST_ABSENT, line));
            advance(p);
        } else {
            AstNode *v = parse_expression(p);
            if (p->in_error || !v) return;
            if (!expect_tok(p, SEMICOLON, "';' after the return value")) return;
            ast_add(n, v);
        }
        ast_add(sink, n);
        return;
    }
    case SEMICOLON:
        advance(p);
        ast_add(sink, ast_new(AST_EMPTY_STMT, line));
        return;
    case CASE: case DEFAULT:
        perr(p, "'%s' labels are only allowed inside a 'switch' body", t->text);
        return;
    case ELSE:
        perr(p, "'else' without a matching 'if'");
        return;
    case STRUCT:
        if (peek(p, 1)->kind == IDENTIFIER && peek(p, 2)->kind == LEFT_BRACE) {
            perr(p, "struct definitions are only allowed at the top level of a file");
            return;
        }
        if (!allow_decl) {
            perr(p, "a declaration cannot be the body of this statement "
                    "(wrap it in a block '{ ... }')");
            return;
        }
        parse_declaration_into(p, sink, 0);
        return;
    default:
        break;
    }

    if (decl_start(p)) {
        if (!allow_decl) {
            perr(p, "a declaration cannot be the body of this statement "
                    "(wrap it in a block '{ ... }')");
            return;
        }
        parse_declaration_into(p, sink, 0);
        return;
    }
    if (t->kind == IDENTIFIER && peek(p, 1)->kind == IDENTIFIER) {
        /* ident then ident: unknown type, or a missing ';' */
        perr(p, "unknown type name '%s' (struct types must be defined before "
                "use; otherwise a ';' is probably missing before '%s')",
             t->text, peek(p, 1)->text);
        return;
    }
    AstNode *e = parse_expression(p);
    if (p->in_error || !e) return;
    if (!expect_tok(p, SEMICOLON, "';' after the expression")) return;
    ast_add(sink, e);
}

// declarations 

// declarator list; takes over `type` and frees it
static void parse_declarator_list(Parser *p, AstNode *sink,
                                  AstNode *type, int in_struct) {
    for (;;) {
        AstNode *dtype = ast_clone_type(type);
        parse_pointer_suffixes(p, dtype);        /* stars per declarator: int a, *b; */
        if (bad(p)) { ast_free(dtype); ast_free(type); return; }
        Token *t = cur(p);
        if (t->kind != IDENTIFIER) {
            perr(p, "expected an identifier but got %s", tokdesc(t));
            ast_free(dtype);
            ast_free(type);
            return;
        }
        int line = t->line;
        char *name = advance_take(p);
        AstNode *decl = ast_new_text(in_struct ? AST_STRUCT_FIELD : AST_VAR_DECL,
                                    line, name);
        free(name);

        if (!parse_array_suffixes(p, dtype)) { ast_free(decl); ast_free(dtype); ast_free(type); return; }
        ast_add(decl, dtype);

        if (in_struct) {
            if (cur(p)->kind == ASSIGN) {
                perr(p, "initializers are not allowed inside a struct definition");
                ast_free(decl);
                ast_free(type);
                return;
            }
            ast_add(decl, ast_new(AST_ABSENT, line));
        } else if (cur(p)->kind == ASSIGN) {
            advance(p);
            AstNode *init = parse_assignment(p);
            if (p->in_error || !init) { ast_free(decl); ast_free(type); return; }
            ast_add(decl, init);
        } else {
            ast_add(decl, ast_new(AST_ABSENT, line));
        }
        ast_add(sink, decl);

        if (cur(p)->kind == COMMA) { advance(p); continue; }
        break;
    }
    if (!expect_tok(p, SEMICOLON, "';' at the end of the declaration")) {
        ast_free(type);
        return;
    }
    ast_free(type);
}

static void parse_declaration_into(Parser *p, AstNode *sink, int in_struct) {
    AstNode *type = parse_type_full(p);
    if (p->in_error || !type) return;
    parse_declarator_list(p, sink, type, in_struct);
}

// params after '('; eats the ')' too 
static AstNode *parse_param_list(Parser *p) {
    AstNode *list = ast_new(AST_PARAM_LIST, cur(p)->line);
    if (cur(p)->kind == RIGHT_PAREN) { advance(p); return list; }
    if (cur(p)->kind == VOID && peek(p, 1)->kind == RIGHT_PAREN) {
        advance(p);
        advance(p);
        return list;
    }
    for (;;) {
        AstNode *type = parse_type_full(p);
        if (p->in_error || !type) { ast_free(list); return NULL; }
        parse_pointer_suffixes(p, type);          /* stars, e.g. Point* p */
        if (bad(p)) { ast_free(type); ast_free(list); return NULL; }
        Token *t = cur(p);
        if (t->kind != IDENTIFIER) {
            perr(p, "expected a parameter name but got %s", tokdesc(t));
            ast_free(type);
            ast_free(list);
            return NULL;
        }
        int line = t->line;
        char *name = advance_take(p);
        AstNode *param = ast_new_text(AST_PARAM, line, name);
        free(name);
        ast_add(param, type);                    /* param owns the type */
        if (!parse_array_suffixes(p, type)) { ast_free(param); ast_free(list); return NULL; }
        ast_add(list, param);
        if (cur(p)->kind == COMMA) { advance(p); continue; }
        break;
    }
    if (!expect_tok(p, RIGHT_PAREN, "')' to close the parameter list")) {
        ast_free(list);
        return NULL;
    }
    return list;
}

/* struct IDENT { fields }; register the name first so
 * `struct Node { Node* next; }` works 
 */
static void parse_struct_def(Parser *p, AstNode *sink) {
    int line = cur(p)->line;
    advance(p);                                  /* 'struct' */
    char *name = expect_text(p, IDENTIFIER, "a struct name");
    if (!name) return;
    if (is_type_name(p, name)) {
        perr(p, "duplicate struct definition: 'struct %s' is already defined", name);
        /* keep going; errors below still line up */
    } else {
        register_type(p, name);
    }
    AstNode *sdef = ast_new_text(AST_STRUCT_DEF, line, name);
    free(name);
    if (!expect_tok(p, LEFT_BRACE, "'{' to open the struct body")) { ast_free(sdef); return; }

    for (;;) {
        if (p->give_up) break;
        int k = cur(p)->kind;
        if (k == RIGHT_BRACE) break;
        if (k == EOF_TOKEN) {
            perr(p, "missing '}' to close the struct opened at line %d", line);
            break;
        }
        p->in_error = 0;
        long before = p->adv_total;
        parse_declaration_into(p, sdef, 1);
        if (p->in_error) {
            sync_stmt(p);
            p->in_error = 0;
            if (p->adv_total == before) advance(p);
        }
    }
    if (p->give_up) { ast_free(sdef); return; }
    if (!expect_tok(p, RIGHT_BRACE, "'}' to close the struct body")) { ast_free(sdef); return; }
    if (!expect_tok(p, SEMICOLON, "';' after the struct definition")) { ast_free(sdef); return; }
    ast_add(sink, sdef);
}

// top level

/* function ahead?  scan the stars without eating them - they're the
 * return type for `int* f() {...}` but the declarator's for `int* p, q;` 
*/
static int is_function_ahead(Parser *p) {
    int k = 0;
    while (peek(p, k)->kind == MULTIPLY) k++;     /* '?' is an extension token */
    return peek(p, k)->kind == IDENTIFIER
        && peek(p, k + 1)->kind == LEFT_PAREN;
}

static void parse_top_item(Parser *p, AstNode *prog) {
    Token *t = cur(p);

    if (t->kind == STRUCT
        && peek(p, 1)->kind == IDENTIFIER
        && peek(p, 2)->kind == LEFT_BRACE) {
        parse_struct_def(p, prog);
        return;
    }
    if (decl_start(p)) {
        AstNode *type = parse_type_full(p);
        if (p->in_error || !type) return;
        if (is_function_ahead(p)) {
            parse_pointer_suffixes(p, type);      /* stars -> return type */
            if (bad(p)) { ast_free(type); return; }
            int line = cur(p)->line;
            char *name = advance_take(p);
            advance(p);                          
            AstNode *fn = ast_new_text(AST_FUNCTION_DEF, line, name);
            free(name);
            ast_add(fn, type);                   
            AstNode *params = parse_param_list(p);
            if (p->in_error || !params) { ast_free(fn); return; }
            ast_add(fn, params);
            if (cur(p)->kind != LEFT_BRACE) {
                perr(p, "expected '{' to start the function body "
                        "(SafeC v1 has function definitions only, "
                        "not separate declarations)");
                ast_free(fn);
                return;
            }
            AstNode *body = parse_compound(p);
            if (p->in_error) { ast_free(fn); return; }
            ast_add(fn, body);
            ast_add(prog, fn);
        } else {
            parse_declarator_list(p, prog, type, 0);
        }
        return;
    }
    if (t->kind == IDENTIFIER) {
        perr(p, "unknown type name '%s' (struct types must be defined with "
                "'struct %s { ... };' before they are used)", t->text, t->text);
        return;
    }
    if (perr_if_safec_extension(p)) return;       /* own/mut/raw/... at top level */
    perr(p, "expected a function definition, a struct definition, or a "
            "global declaration, but got %s", tokdesc(t));
}

// skip to the next ';', '}' or statement start, then try again
static void sync_stmt(Parser *p) {
    for (;;) {
        if (p->give_up) return;
        int k = cur(p)->kind;
        if (k == EOF_TOKEN || k == RIGHT_BRACE) return;
        if (k == SEMICOLON) { advance(p); return; }
        switch (k) {
        case LEFT_BRACE: case IF: case WHILE: case FOR: case SWITCH:
        case RETURN: case BREAK: case CONTINUE: case CASE: case DEFAULT:
            return;
        default:
            if (decl_start(p)) return;
            advance(p);
        }
    }
}

// same, for the top level; also eats '}'
static void sync_top(Parser *p) {
    for (;;) {
        if (p->give_up) return;
        int k = cur(p)->kind;
        if (k == EOF_TOKEN) return;
        if (k == SEMICOLON) { advance(p); return; }
        if (k == RIGHT_BRACE) { advance(p); continue; }
        if (k == STRUCT || is_prim_kind(k)) return;
        if (k == IDENTIFIER && is_type_name(p, cur(p)->text)) return;
        advance(p);
    }
}

static AstNode *parse_program(Parser *p) {
    AstNode *prog = ast_new(AST_PROGRAM, 1);
    for (;;) {
        if (p->give_up) break;
        if (cur(p)->kind == EOF_TOKEN) break;
        p->in_error = 0;                          /* fresh attempt */
        long before = p->adv_total;
        parse_top_item(p, prog);
        if (p->in_error) {
            sync_top(p);
            p->in_error = 0;
            if (p->adv_total == before) advance(p);    /* always make progress */
        }
    }
    return prog;
}

// entry point 

ParseResult safec_parse_file(const char *path) {
    ParseResult res;
    memset(&res, 0, sizeof res);

    FILE *f = fopen(path, "r");
    if (!f) {
        res.error_count = 1;
        res.errors[0].line = 0;
        snprintf(res.errors[0].message, SAFEC_MSG_MAX,
                 "cannot open file '%s'", path);
        return res;
    }
    yyin = f;

    Parser p;
    memset(&p, 0, sizeof p);
    p.res = &res;

    AstNode *prog = parse_program(&p);
    fclose(f);

    for (int i = 0; i < p.type_count; i++) free(p.types[i]);
    free(p.types);

    res.ast = prog;
    return res;
}
