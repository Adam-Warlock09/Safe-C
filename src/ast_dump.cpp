// ast_dump.cpp - prints the tree, one node per line.
#include "ast_dump.h"

#include <ostream>
#include <string>

#include "new_ast.h"

namespace safec {
namespace {

void indent(std::ostream &os, int depth) {
    for (int i = 0; i < depth; i++) os << "  ";
}

void head(std::ostream &os, int depth, const char *kind, const SourceLoc &loc) {
    indent(os, depth);
    os << '(' << kind << " @" << loc.line;
}

void close(std::ostream &os) { os << ")\n"; }

// ---- literals ---------------------------------------------------------

void putCharLit(std::ostream &os, char c) {
    os << "'";
    switch (c) {
    case '\n': os << "\\n"; break;
    case '\t': os << "\\t"; break;
    case '\r': os << "\\r"; break;
    case '\0': os << "\\0"; break;
    case '\\': os << "\\\\"; break;
    case '\'': os << "\\'"; break;
    default:   os << c; break;
    }
    os << "'";
}

void putStringLit(std::ostream &os, const std::string &s) {
    os << "\"";
    for (char c : s) {
        switch (c) {
        case '\n': os << "\\n"; break;
        case '\t': os << "\\t"; break;
        case '\r': os << "\\r"; break;
        case '"':  os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        default:   os << c; break;
        }
    }
    os << "\"";
}

// ---- types ------------------------------------------------------------

// e.g. "own struct Node*?[4]"
void typeSpecText(const TypeSpec *t, std::ostream &os) {
    switch (t->qualifier) {
    case PtrQualifier::Own: os << "own "; break;
    case PtrQualifier::Mut: os << "mut "; break;
    case PtrQualifier::Raw: os << "raw "; break;
    case PtrQualifier::None: break;
    }
    switch (t->base) {
    case BaseTypeKind::Void:  os << "void";  break;
    case BaseTypeKind::Bool:  os << "bool";  break;
    case BaseTypeKind::Char:  os << "char";  break;
    case BaseTypeKind::Int:   os << "int";   break;
    case BaseTypeKind::Float: os << "float"; break;
    case BaseTypeKind::Struct: os << "struct " << t->structName; break;
    }
    for (const PointerLevel &p : t->pointers) {
        os << '*';
        if (p.nullable) os << '?';
    }
    for (size_t i = 0; i < t->arrayDims.size(); i++) os << "[]";
}

// dims are printed by the caller, as extra children
void dumpTypeSpec(const TypeSpec *t, std::ostream &os, int depth) {
    head(os, depth, "type", t->loc);
    os << ' ';
    typeSpecText(t, os);
    close(os);
}

// ---- forward decls ----------------------------------------------------

void dumpExpr(const Expr *e, std::ostream &os, int depth);
void dumpStmt(const Stmt *s, std::ostream &os, int depth);

// ---- declarations -----------------------------------------------------

void dumpVarDecl(const VarDecl *v, std::ostream &os, int depth) {
    head(os, depth, isa<ParamDecl>(v) ? "param" : "var_decl", v->loc);
    os << ' ' << v->name;
    if (v->isGlobal && !isa<ParamDecl>(v)) os << " global";
    close(os);
    dumpTypeSpec(v->typeSpec.get(), os, depth + 1);
    for (const ExprPtr &dim : v->typeSpec->arrayDims)
        if (dim) dumpExpr(dim.get(), os, depth + 2);
    if (v->init) dumpExpr(v->init.get(), os, depth + 1);
}

void dumpStructDecl(const StructDecl *sd, std::ostream &os, int depth) {
    head(os, depth, "struct_def", sd->loc);
    os << ' ' << sd->name;
    close(os);
    for (const auto &f : sd->fields) {
        head(os, depth + 1, "field", f->loc);
        os << ' ' << f->name;
        close(os);
        dumpTypeSpec(f->typeSpec.get(), os, depth + 2);
        for (const ExprPtr &dim : f->typeSpec->arrayDims)
            if (dim) dumpExpr(dim.get(), os, depth + 3);
    }
}

void dumpFunctionDecl(const FunctionDecl *fn, std::ostream &os, int depth) {
    head(os, depth, "func_def", fn->loc);
    os << ' ' << fn->name;
    close(os);
    dumpTypeSpec(fn->returnSpec.get(), os, depth + 1);
    for (const ExprPtr &dim : fn->returnSpec->arrayDims)
        if (dim) dumpExpr(dim.get(), os, depth + 2);
    for (const auto &p : fn->params) dumpVarDecl(p.get(), os, depth + 1);
    dumpStmt(fn->body.get(), os, depth + 1);
}

void dumpDecl(const Decl *d, std::ostream &os, int depth) {
    if (isa<VarDecl>(d)) dumpVarDecl(cast<VarDecl>(d), os, depth);
    else if (isa<StructDecl>(d)) dumpStructDecl(cast<StructDecl>(d), os, depth);
    else if (isa<FunctionDecl>(d)) dumpFunctionDecl(cast<FunctionDecl>(d), os, depth);
}

// ---- statements -------------------------------------------------------

void dumpBody(const std::vector<StmtPtr> &body, std::ostream &os, int depth) {
    for (const StmtPtr &s : body) dumpStmt(s.get(), os, depth);
}

void dumpStmt(const Stmt *s, std::ostream &os, int depth) {
    if (!s) {
        indent(os, depth);
        os << "(absent)\n";
        return;
    }
    if (isa<CompoundStmt>(s)) {
        const CompoundStmt *b = cast<CompoundStmt>(s);
        head(os, depth, "block", b->loc);
        close(os);
        dumpBody(b->body, os, depth + 1);
        return;
    }
    if (isa<DeclStmt>(s)) {
        const DeclStmt *ds = cast<DeclStmt>(s);
        head(os, depth, "decl", ds->loc);
        close(os);
        for (const auto &v : ds->decls) dumpVarDecl(v.get(), os, depth + 1);
        return;
    }
    if (isa<ExprStmt>(s)) {
        const ExprStmt *es = cast<ExprStmt>(s);
        head(os, depth, "expr", es->loc);
        close(os);
        dumpExpr(es->expr.get(), os, depth + 1);
        return;
    }
    if (isa<NullStmt>(s)) {
        head(os, depth, "empty", s->loc);
        close(os);
        return;
    }
    if (isa<IfStmt>(s)) {
        const IfStmt *n = cast<IfStmt>(s);
        head(os, depth, "if", n->loc);
        close(os);
        dumpExpr(n->cond.get(), os, depth + 1);
        dumpStmt(n->thenStmt.get(), os, depth + 1);
        if (n->elseStmt) dumpStmt(n->elseStmt.get(), os, depth + 1);
        return;
    }
    if (isa<WhileStmt>(s)) {
        const WhileStmt *n = cast<WhileStmt>(s);
        head(os, depth, "while", n->loc);
        close(os);
        dumpExpr(n->cond.get(), os, depth + 1);
        dumpStmt(n->body.get(), os, depth + 1);
        return;
    }
    if (isa<ForStmt>(s)) {
        const ForStmt *n = cast<ForStmt>(s);
        head(os, depth, "for", n->loc);
        close(os);
        dumpStmt(n->init.get(), os, depth + 1);      // (absent) if null
        if (n->cond) dumpExpr(n->cond.get(), os, depth + 1);
        else { indent(os, depth + 1); os << "(absent)\n"; }
        if (n->step) dumpExpr(n->step.get(), os, depth + 1);
        else { indent(os, depth + 1); os << "(absent)\n"; }
        dumpStmt(n->body.get(), os, depth + 1);
        return;
    }
    if (isa<SwitchStmt>(s)) {
        const SwitchStmt *n = cast<SwitchStmt>(s);
        head(os, depth, "switch", n->loc);
        close(os);
        dumpExpr(n->cond.get(), os, depth + 1);
        for (const auto &c : n->cases) {
            if (isa<CaseStmt>(c.get())) {
                const CaseStmt *cs = cast<CaseStmt>(c.get());
                head(os, depth + 1, "case", cs->loc);
                close(os);
                dumpExpr(cs->value.get(), os, depth + 2);
                dumpBody(cs->body, os, depth + 2);
            } else {
                const DefaultStmt *ds = cast<DefaultStmt>(c.get());
                head(os, depth + 1, "default", ds->loc);
                close(os);
                dumpBody(ds->body, os, depth + 2);
            }
        }
        return;
    }
    if (isa<ReturnStmt>(s)) {
        const ReturnStmt *n = cast<ReturnStmt>(s);
        head(os, depth, "return", n->loc);
        close(os);
        if (n->value) dumpExpr(n->value.get(), os, depth + 1);
        return;
    }
    if (isa<BreakStmt>(s)) {
        head(os, depth, "break", s->loc);
        close(os);
        return;
    }
    if (isa<ContinueStmt>(s)) {
        head(os, depth, "continue", s->loc);
        close(os);
        return;
    }
    if (isa<DeferStmt>(s)) {
        const DeferStmt *n = cast<DeferStmt>(s);
        head(os, depth, "defer", n->loc);
        close(os);
        dumpStmt(n->body.get(), os, depth + 1);
        return;
    }
    if (isa<TrustedStmt>(s)) {
        const TrustedStmt *n = cast<TrustedStmt>(s);
        head(os, depth, "trusted", n->loc);
        close(os);
        dumpStmt(n->body.get(), os, depth + 1);
        return;
    }
}

// ---- expressions ------------------------------------------------------

void dumpExpr(const Expr *e, std::ostream &os, int depth) {
    if (isa<IntLiteral>(e)) {
        const IntLiteral *n = cast<IntLiteral>(e);
        head(os, depth, "int_lit", n->loc);
        os << ' ' << n->value;
        close(os);
        return;
    }
    if (isa<FloatLiteral>(e)) {
        const FloatLiteral *n = cast<FloatLiteral>(e);
        head(os, depth, "float_lit", n->loc);
        os << ' ' << n->value;
        close(os);
        return;
    }
    if (isa<CharLiteral>(e)) {
        const CharLiteral *n = cast<CharLiteral>(e);
        head(os, depth, "char_lit", n->loc);
        os << ' ';
        putCharLit(os, n->value);
        close(os);
        return;
    }
    if (isa<StringLiteral>(e)) {
        const StringLiteral *n = cast<StringLiteral>(e);
        head(os, depth, "string_lit", n->loc);
        os << ' ';
        putStringLit(os, n->value);
        close(os);
        return;
    }
    if (isa<BoolLiteral>(e)) {
        const BoolLiteral *n = cast<BoolLiteral>(e);
        head(os, depth, "bool_lit", n->loc);
        os << (n->value ? " true" : " false");
        close(os);
        return;
    }
    if (isa<NullLiteral>(e)) {
        head(os, depth, "null", e->loc);
        close(os);
        return;
    }
    if (isa<NameExpr>(e)) {
        const NameExpr *n = cast<NameExpr>(e);
        head(os, depth, "name", n->loc);
        os << ' ' << n->name;
        close(os);
        return;
    }
    if (isa<UnaryExpr>(e)) {
        const UnaryExpr *n = cast<UnaryExpr>(e);
        bool post = n->op == UnaryOp::PostInc || n->op == UnaryOp::PostDec;
        head(os, depth, post ? "postfix_op" : "unary_op", n->loc);
        os << ' ' << unaryOpSpelling(n->op);
        close(os);
        dumpExpr(n->operand.get(), os, depth + 1);
        return;
    }
    if (isa<BinaryExpr>(e)) {
        const BinaryExpr *n = cast<BinaryExpr>(e);
        head(os, depth, "bin_op", n->loc);
        os << ' ' << binaryOpSpelling(n->op);
        close(os);
        dumpExpr(n->lhs.get(), os, depth + 1);
        dumpExpr(n->rhs.get(), os, depth + 1);
        return;
    }
    if (isa<AssignExpr>(e)) {
        const AssignExpr *n = cast<AssignExpr>(e);
        head(os, depth, "assign", n->loc);
        os << ' ' << assignOpSpelling(n->op);
        close(os);
        dumpExpr(n->target.get(), os, depth + 1);
        dumpExpr(n->value.get(), os, depth + 1);
        return;
    }
    if (isa<CallExpr>(e)) {
        const CallExpr *n = cast<CallExpr>(e);
        head(os, depth, "call", n->loc);
        close(os);
        dumpExpr(n->callee.get(), os, depth + 1);
        for (const ExprPtr &a : n->args) dumpExpr(a.get(), os, depth + 1);
        return;
    }
    if (isa<IndexExpr>(e)) {
        const IndexExpr *n = cast<IndexExpr>(e);
        head(os, depth, "index", n->loc);
        close(os);
        dumpExpr(n->base.get(), os, depth + 1);
        dumpExpr(n->index.get(), os, depth + 1);
        return;
    }
    if (isa<MemberExpr>(e)) {
        const MemberExpr *n = cast<MemberExpr>(e);
        head(os, depth, n->isArrow ? "arrow_access" : "field_access", n->loc);
        os << ' ' << n->member;
        close(os);
        dumpExpr(n->base.get(), os, depth + 1);
        return;
    }
    if (isa<NewExpr>(e)) {
        const NewExpr *n = cast<NewExpr>(e);
        head(os, depth, "new", n->loc);
        os << ' ';
        typeSpecText(n->allocated.get(), os);
        close(os);
        return;
    }
    if (isa<FreeExpr>(e)) {
        const FreeExpr *n = cast<FreeExpr>(e);
        head(os, depth, "free", n->loc);
        close(os);
        dumpExpr(n->operand.get(), os, depth + 1);
        return;
    }
    if (isa<CompoundLiteralExpr>(e)) {
        const CompoundLiteralExpr *n = cast<CompoundLiteralExpr>(e);
        head(os, depth, "compound_lit", n->loc);
        os << ' ';
        typeSpecText(n->typeSpec.get(), os);
        close(os);
        for (const Initializer &i : n->inits) {
            if (i.isDesignated()) {
                head(os, depth + 1, "field_init", i.loc);
                os << ' ' << i.field;
                close(os);
                dumpExpr(i.value.get(), os, depth + 2);
            } else {
                dumpExpr(i.value.get(), os, depth + 1);
            }
        }
        return;
    }
}

} // namespace

void dumpProgram(const Program &prog, std::ostream &os) {
    os << "(program)\n";
    for (const DeclPtr &d : prog.decls) dumpDecl(d.get(), os, 1);
}

} // namespace safec
