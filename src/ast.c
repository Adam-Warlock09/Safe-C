// ast.c - makes, frees and prints AST nodes (kinds documented in ast.h).
#include <stdlib.h>
#include <string.h>
#include "ast.h"

// one short name per kind, used by ast_dump 
static const char *const KIND_NAMES[] = {
    "program", "struct_def", "field", "var_decl", "func_def",
    "params", "param", "type",
    "block", "if", "while", "for", "switch", "case", "default",
    "break", "continue", "return", "empty", "defer", "trusted",
    "int_lit", "float_lit", "char_lit", "string_lit", "bool_lit", "null_lit",
    "ident", "bin_op", "unary_op", "postfix_op", "assign",
    "call", "field_access", "arrow_access", "index",
    "new", "free", "compound_lit", "field_init",
    "absent"
};

const char *ast_kind_name(AstKind kind) {
    if (kind >= 0 && (size_t)kind < sizeof KIND_NAMES / sizeof KIND_NAMES[0])
        return KIND_NAMES[kind];
    return "?";
}

int ast_child_count(const AstNode *n) {
    return n ? n->child_count : 0;
}

// creation
static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) {
        fprintf(stderr, "safec: out of memory\n");
        exit(1);
    }
    return p;
}

AstNode *ast_new(AstKind kind, int line) {
    return ast_new_text(kind, line, "");
}

AstNode *ast_new_text(AstKind kind, int line, const char *text) {
    AstNode *n = calloc(1, sizeof *n);
    if (!n) {
        fprintf(stderr, "safec: out of memory\n");
        exit(1);
    }
    n->kind = kind;
    n->line = line;
    n->text = xstrdup(text);
    return n;
}

void ast_add(AstNode *parent, AstNode *child) {
    AstNode **grown = realloc(parent->children,
                              (size_t)(parent->child_count + 1) * sizeof *grown);
    if (!grown) {
        fprintf(stderr, "safec: out of memory\n");
        exit(1);
    }
    parent->children = grown;
    parent->children[parent->child_count++] = child;
}

AstNode *ast_clone_type(const AstNode *type) {
    AstNode *copy = ast_new_text(type->kind, type->line, type->text);
    copy->flags = type->flags;
    copy->ptr_depth = type->ptr_depth;
    copy->nullable_mask = type->nullable_mask;
    // dims are not copied - each declarator adds its own
    return copy;
}

void ast_free(AstNode *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++)
        ast_free(node->children[i]);
    free(node->children);
    free(node->text);
    free(node);
}

// printing

static void dump_type_text(const AstNode *n, FILE *out) {
    if (n->flags & TF_OWN) fputs("own ", out);
    else if (n->flags & TF_MUT) fputs("mut ", out);
    else if (n->flags & TF_RAW) fputs("raw ", out);
    if (n->flags & TF_STRUCT_KW) fprintf(out, "struct %s", n->text);
    else fputs(n->text, out);
    for (int i = 0; i < n->ptr_depth; i++) {
        fputc('*', out);
        if (n->nullable_mask & (1 << i)) fputc('?', out);
    }
    for (int i = 0; i < n->child_count; i++)   /* array dimensions */
        fputs("[]", out);
}

static void dump_indent(int depth, FILE *out) {
    for (int i = 0; i < depth; i++) fputs("  ", out);
}

static void dump_node(const AstNode *n, int depth, FILE *out) {
    if (!n) return;
    dump_indent(depth, out);
    fputc('(', out);
    fputs(ast_kind_name(n->kind), out);
    fprintf(out, " @%d", n->line);

    if (n->kind == AST_TYPE) {
        fputc(' ', out);
        dump_type_text(n, out);
    } else if (n->text[0]) {
        fprintf(out, " %s", n->text);
    }
    fputc(')', out);
    fputc('\n', out);

    // Indent child nodes; array types store their dimensions as children
    for (int i = 0; i < n->child_count; i++)
        dump_node(n->children[i], depth + 1, out);
}

void ast_dump(const AstNode *node, FILE *out) {
    dump_node(node, 0, out);
}
