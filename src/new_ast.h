/*
 * new_ast.h - C++ AST for SafeC semantic analysis.
 *
 * Mirrors the rules in src/parser.y.  Layout:
 *
 *   SourceLoc, Diagnostic, DiagnosticsEngine     error reporting
 *   Type + TypeContext                           semantic types (interned)
 *   ASTNode                                      base: loc + error bit
 *     +- TypeSpec                                a type as written in source
 *     +- Decl  -> ValueDecl -> VarDecl -> ParamDecl
 *     |                     -> FunctionDecl
 *     |        -> FieldDecl, StructDecl
 *     +- Stmt  -> CompoundStmt, DeclStmt, ExprStmt, NullStmt, IfStmt,
 *     |           WhileStmt, ForStmt, SwitchStmt, SwitchCase -> CaseStmt /
 *     |           DefaultStmt, ReturnStmt, BreakStmt, ContinueStmt,
 *     |           DeferStmt, TrustedStmt
 *     +- Expr  -> literals, NameExpr, UnaryExpr, BinaryExpr, AssignExpr,
 *                 CallExpr, IndexExpr, MemberExpr, NewExpr, FreeExpr,
 *                 CompoundLiteralExpr, ImplicitCastExpr
 *   Program                                      owns the whole tree
 *
 * Syntax vs. semantics: TypeSpec is what the programmer wrote
 * (`own struct Node *? next`), Type is what it means (an owning,
 * nullable pointer to struct Node).  The parser builds TypeSpecs; sema
 * resolves each to a Type.  Types are interned in a TypeContext, so two
 * types are equal iff their pointers are equal.
 *
 * Fields marked [sema] are left null/default by the parser and filled
 * in by semantic analysis.
 *
 * Each node's `loc` is its most useful location for diagnostics: the
 * name for declarations, the leading keyword for statements, the
 * operator for operator expressions, the first token otherwise.
 *
 * Down-casting is LLVM-style: every class has a static classof(), and
 * isa<T>(n) / dyn_cast<T>(n) / cast<T>(n) work on both ASTNode* and Type*.
 */
#ifndef SAFEC_AST_H
#define SAFEC_AST_H

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace safec {

// ===================================================================
// Casting helpers
// ===================================================================

template <typename To, typename From>
inline bool isa(const From *p) { return p && To::classof(p); }

template <typename To, typename From>
inline To *dyn_cast(From *p) { return isa<To>(p) ? static_cast<To *>(p) : nullptr; }

template <typename To, typename From>
inline const To *dyn_cast(const From *p) {
    return isa<To>(p) ? static_cast<const To *>(p) : nullptr;
}

template <typename To, typename From>
inline To *cast(From *p) { assert(isa<To>(p) && "cast<> to wrong type"); return static_cast<To *>(p); }

template <typename To, typename From>
inline const To *cast(const From *p) {
    assert(isa<To>(p) && "cast<> to wrong type");
    return static_cast<const To *>(p);
}

// ===================================================================
// Source locations and diagnostics
// ===================================================================

struct SourceLoc {
    int line = 0;   // 1-based; 0 = unknown
    int col  = 0;   // 1-based; 0 = unknown (lexer.l does not track columns yet)

    SourceLoc() = default;
    SourceLoc(int l, int c = 0) : line(l), col(c) {}
    bool isValid() const { return line > 0; }
};

enum class DiagKind {
    Error,      // program is rejected
    Warning,    // suspicious but legal
    Note        // attached to the previous Error/Warning ("previous definition is here")
};

struct Diagnostic {
    DiagKind    kind;
    SourceLoc   loc;
    std::string message;
};

inline const char *diagKindName(DiagKind k) {
    switch (k) {
    case DiagKind::Error:   return "error";
    case DiagKind::Warning: return "warning";
    case DiagKind::Note:    return "note";
    }
    return "?";
}

/*
 * Collects diagnostics in report order.  "Ignoring" is a policy, not a
 * kind: with suppressWarnings set, warnings (and the notes that follow
 * them) are dropped instead of stored.
 */
class DiagnosticsEngine {
public:
    explicit DiagnosticsEngine(std::string filename = "") : filename_(std::move(filename)) {}

    void report(DiagKind kind, SourceLoc loc, std::string message) {
        if (kind == DiagKind::Warning && warningsAsErrors_) kind = DiagKind::Error;
        if (kind == DiagKind::Note) {
            if (lastDropped_) return;
        } else {
            lastDropped_ = (kind == DiagKind::Warning && suppressWarnings_);
            if (lastDropped_) return;
        }
        if (kind == DiagKind::Error)   ++errors_;
        if (kind == DiagKind::Warning) ++warnings_;
        diags_.push_back({kind, loc, std::move(message)});
    }

    void error(SourceLoc loc, std::string msg)   { report(DiagKind::Error, loc, std::move(msg)); }
    void warning(SourceLoc loc, std::string msg) { report(DiagKind::Warning, loc, std::move(msg)); }
    void note(SourceLoc loc, std::string msg)    { report(DiagKind::Note, loc, std::move(msg)); }

    bool   hasErrors()    const { return errors_ > 0; }
    size_t errorCount()   const { return errors_; }
    size_t warningCount() const { return warnings_; }
    const std::vector<Diagnostic> &diagnostics() const { return diags_; }

    void setSuppressWarnings(bool on) { suppressWarnings_ = on; }
    void setWarningsAsErrors(bool on) { warningsAsErrors_ = on; }
    const std::string &filename() const { return filename_; }

    // "file:line:col: error: message", same shape as safec-parser's output
    void print(std::ostream &os) const {
        for (const Diagnostic &d : diags_) {
            os << filename_;
            if (d.loc.isValid()) {
                os << ':' << d.loc.line;
                if (d.loc.col > 0) os << ':' << d.loc.col;
            }
            os << ": " << diagKindName(d.kind) << ": " << d.message << '\n';
        }
    }

private:
    std::string filename_;
    std::vector<Diagnostic> diags_;
    size_t errors_ = 0, warnings_ = 0;
    bool suppressWarnings_ = false;
    bool warningsAsErrors_ = false;
    bool lastDropped_ = false;
};

// ===================================================================
// Semantic types
// ===================================================================

class StructDecl;

enum class TypeKind { Builtin, Pointer, Array, Struct, Function };

class Type {
public:
    virtual ~Type() = default;
    Type(const Type &) = delete;
    Type &operator=(const Type &) = delete;

    TypeKind kind() const { return kind_; }

protected:
    explicit Type(TypeKind k) : kind_(k) {}

private:
    const TypeKind kind_;
};

enum class BuiltinKind {
    Void, Bool, Char, Int, Float,
    Null    // type of the `null` literal; converts only to nullable pointers
};

class BuiltinType : public Type {
public:
    BuiltinKind builtinKind() const { return bk_; }
    bool isInteger() const { return bk_ == BuiltinKind::Bool || bk_ == BuiltinKind::Char || bk_ == BuiltinKind::Int; }
    bool isArithmetic() const { return isInteger() || bk_ == BuiltinKind::Float; }

    static bool classof(const Type *t) { return t->kind() == TypeKind::Builtin; }

private:
    friend class TypeContext;
    explicit BuiltinType(BuiltinKind bk) : Type(TypeKind::Builtin), bk_(bk) {}
    BuiltinKind bk_;
};

enum class PtrKind {
    Borrow,     // T*      read borrow
    MutBorrow,  // mut T*  write borrow
    Own,        // own T*  sole owner
    Raw         // raw T*  unchecked, usable only inside `trusted`
};

class PointerType : public Type {
public:
    const Type *pointee() const { return pointee_; }
    PtrKind ptrKind() const { return pk_; }
    bool isNullable() const { return nullable_; } // For T*?

    static bool classof(const Type *t) { return t->kind() == TypeKind::Pointer; }

private:
    friend class TypeContext;
    PointerType(const Type *pointee, PtrKind pk, bool nullable)
        : Type(TypeKind::Pointer), pointee_(pointee), pk_(pk), nullable_(nullable) {}
    const Type *pointee_;
    PtrKind pk_;
    bool nullable_;
};

class ArrayType : public Type {
public:
    const Type *element() const { return elem_; }
    std::optional<uint64_t> size() const { return size_; }   // nullopt for `T x[]`

    static bool classof(const Type *t) { return t->kind() == TypeKind::Array; }

private:
    friend class TypeContext;
    ArrayType(const Type *elem, std::optional<uint64_t> size)
        : Type(TypeKind::Array), elem_(elem), size_(size) {}
    const Type *elem_;
    std::optional<uint64_t> size_;
};

class StructType : public Type {
public:
    StructDecl *decl() const { return decl_; }

    static bool classof(const Type *t) { return t->kind() == TypeKind::Struct; }

private:
    friend class TypeContext;
    explicit StructType(StructDecl *d) : Type(TypeKind::Struct), decl_(d) {}
    StructDecl *decl_;
};

class FunctionType : public Type {
public:
    const Type *returnType() const { return ret_; }
    const std::vector<const Type *> &params() const { return params_; }

    static bool classof(const Type *t) { return t->kind() == TypeKind::Function; }

private:
    friend class TypeContext;
    FunctionType(const Type *ret, std::vector<const Type *> params)
        : Type(TypeKind::Function), ret_(ret), params_(std::move(params)) {}
    const Type *ret_;
    std::vector<const Type *> params_;
};

/*
 * Owns and interns every Type.  Ask it for a type; asking twice with
 * the same arguments returns the same pointer.
 */
class TypeContext {
public:
    TypeContext()
        : void_(BuiltinKind::Void), bool_(BuiltinKind::Bool), char_(BuiltinKind::Char),
          int_(BuiltinKind::Int), float_(BuiltinKind::Float), null_(BuiltinKind::Null) {}
    TypeContext(const TypeContext &) = delete;
    TypeContext &operator=(const TypeContext &) = delete;

    const BuiltinType *voidType()  const { return &void_; }
    const BuiltinType *boolType()  const { return &bool_; }
    const BuiltinType *charType()  const { return &char_; }
    const BuiltinType *intType()   const { return &int_; }
    const BuiltinType *floatType() const { return &float_; }
    const BuiltinType *nullType()  const { return &null_; }

    const BuiltinType *builtin(BuiltinKind k) const {
        switch (k) {
        case BuiltinKind::Void:  return &void_;
        case BuiltinKind::Bool:  return &bool_;
        case BuiltinKind::Char:  return &char_;
        case BuiltinKind::Int:   return &int_;
        case BuiltinKind::Float: return &float_;
        case BuiltinKind::Null:  return &null_;
        }
        return nullptr;
    }

    const PointerType *pointerTo(const Type *pointee, PtrKind pk, bool nullable) {
        auto &slot = pointers_[{pointee, pk, nullable}];
        if (!slot) slot.reset(new PointerType(pointee, pk, nullable));
        return slot.get();
    }

    const ArrayType *arrayOf(const Type *elem, std::optional<uint64_t> size) {
        auto &slot = arrays_[{elem, size.has_value(), size.value_or(0)}];
        if (!slot) slot.reset(new ArrayType(elem, size));
        return slot.get();
    }

    const StructType *structType(StructDecl *decl) {
        auto &slot = structs_[decl];
        if (!slot) slot.reset(new StructType(decl));
        return slot.get();
    }

    const FunctionType *functionType(const Type *ret, std::vector<const Type *> params) {
        std::vector<const Type *> key;
        key.reserve(params.size() + 1);
        key.push_back(ret);
        key.insert(key.end(), params.begin(), params.end());
        auto &slot = functions_[key];
        if (!slot) slot.reset(new FunctionType(ret, std::move(params)));
        return slot.get();
    }

private:
    BuiltinType void_, bool_, char_, int_, float_, null_;
    std::map<std::tuple<const Type *, PtrKind, bool>, std::unique_ptr<PointerType>> pointers_;
    std::map<std::tuple<const Type *, bool, uint64_t>, std::unique_ptr<ArrayType>> arrays_;
    std::map<StructDecl *, std::unique_ptr<StructType>> structs_;
    std::map<std::vector<const Type *>, std::unique_ptr<FunctionType>> functions_;
};

// Defined after StructDecl (needs its name)
inline std::string typeToString(const Type *t);

// ===================================================================
// AST base
// ===================================================================

enum class NodeKind {
    // TypeSpec
    TypeSpec,

    // Decl
    VarDecl, FirstDecl = VarDecl, FirstValueDecl = VarDecl,
    ParamDecl,
    FunctionDecl, LastValueDecl = FunctionDecl,
    FieldDecl,
    StructDecl, LastDecl = StructDecl,

    // Stmt
    CompoundStmt, FirstStmt = CompoundStmt,
    DeclStmt,
    ExprStmt,
    NullStmt,
    IfStmt,
    WhileStmt,
    ForStmt,
    SwitchStmt,
    CaseStmt, FirstSwitchCase = CaseStmt,
    DefaultStmt, LastSwitchCase = DefaultStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    DeferStmt,
    TrustedStmt, LastStmt = TrustedStmt,

    // Expr
    IntLiteral, FirstExpr = IntLiteral,
    FloatLiteral,
    CharLiteral,
    StringLiteral,
    BoolLiteral,
    NullLiteral,
    NameExpr,
    UnaryExpr,
    BinaryExpr,
    AssignExpr,
    CallExpr,
    IndexExpr,
    MemberExpr,
    NewExpr,
    FreeExpr,
    CompoundLiteralExpr,
    ImplicitCastExpr, LastExpr = ImplicitCastExpr
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
    ASTNode(const ASTNode &) = delete;
    ASTNode &operator=(const ASTNode &) = delete;

    NodeKind kind() const { return kind_; }

    SourceLoc loc;

    // Error bit: set once an error has been reported on this node, so
    // later passes skip it instead of reporting follow-on errors.
    bool isInvalid() const { return invalid_; }
    void setInvalid(bool v = true) { invalid_ = v; }

protected:
    ASTNode(NodeKind k, SourceLoc l) : loc(l), kind_(k) {}

private:
    const NodeKind kind_;
    bool invalid_ = false;
};

#define SAFEC_CLASSOF_ONE(K) \
    static bool classof(const ASTNode *n) { return n->kind() == NodeKind::K; }
#define SAFEC_CLASSOF_RANGE(First, Last)                  \
    static bool classof(const ASTNode *n) {               \
        return n->kind() >= NodeKind::First && n->kind() <= NodeKind::Last; \
    }

class Expr;
class Stmt;
class Decl;
class CompoundStmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;

// ===================================================================
// TypeSpec - a type as written
// ===================================================================

enum class PtrQualifier { None, Own, Mut, Raw };   // qual_base prefix

enum class BaseTypeKind { Void, Bool, Char, Int, Float, Struct };   // type_base

struct PointerLevel {   // one `*` or `*?` from `ptrs`
    SourceLoc loc;
    bool nullable = false;
};

/*
 * qual_base ptrs ... arrays, e.g. `own struct Node *? next[4]`.
 *
 * `int a, *b;` gives each declarator its own TypeSpec (the parser
 * copies the qual_base), because stars and array suffixes belong to
 * the declarator, not the base.
 *
 * pointers[0] is the star nearest the base (innermost).  Which pointer
 * level the qualifier governs when there are several, and rejecting a
 * qualifier with no pointer (`own int x`), are sema's job.
 */
class TypeSpec : public ASTNode {
public:
    TypeSpec(SourceLoc l, BaseTypeKind base, std::string structName = "")
        : ASTNode(NodeKind::TypeSpec, l), base(base), structName(std::move(structName)) {}

    PtrQualifier qualifier = PtrQualifier::None;
    BaseTypeKind base;
    std::string structName;              // only for BaseTypeKind::Struct
    std::vector<PointerLevel> pointers;
    std::vector<ExprPtr> arrayDims;      // outermost first; nullptr for `[]`

    const Type *resolved = nullptr;      // [sema]

    SAFEC_CLASSOF_ONE(TypeSpec)
};
using TypeSpecPtr = std::unique_ptr<TypeSpec>;

// ===================================================================
// Declarations
// ===================================================================

class Decl : public ASTNode {
public:
    std::string name;

    SAFEC_CLASSOF_RANGE(FirstDecl, LastDecl)

protected:
    Decl(NodeKind k, SourceLoc l, std::string n) : ASTNode(k, l), name(std::move(n)) {}
};

// Anything a NameExpr can refer to: variables, parameters, functions
class ValueDecl : public Decl {
public:
    const Type *type = nullptr;   // [sema]

    SAFEC_CLASSOF_RANGE(FirstValueDecl, LastValueDecl)

protected:
    using Decl::Decl;
};

// declarator: ptrs IDENTIFIER arrays [ '=' assignment ]
class VarDecl : public ValueDecl {
public:
    VarDecl(SourceLoc l, std::string n, TypeSpecPtr spec, ExprPtr init = nullptr, bool isGlobal = false)
        : VarDecl(NodeKind::VarDecl, l, std::move(n), std::move(spec), std::move(init), isGlobal) {}

    TypeSpecPtr typeSpec;
    ExprPtr init;            // may be null
    bool isGlobal;

    static bool classof(const ASTNode *n) {
        return n->kind() == NodeKind::VarDecl || n->kind() == NodeKind::ParamDecl;
    }

protected:
    VarDecl(NodeKind k, SourceLoc l, std::string n, TypeSpecPtr spec, ExprPtr init, bool isGlobal)
        : ValueDecl(k, l, std::move(n)), typeSpec(std::move(spec)), init(std::move(init)), isGlobal(isGlobal) {}
};

// param: qual_base ptrs IDENTIFIER arrays   (never has an init)
class ParamDecl : public VarDecl {
public:
    ParamDecl(SourceLoc l, std::string n, TypeSpecPtr spec)
        : VarDecl(NodeKind::ParamDecl, l, std::move(n), std::move(spec), nullptr, false) {}

    SAFEC_CLASSOF_ONE(ParamDecl)
};

// func_def: qual_base ptrs IDENTIFIER '(' params ')' block
// `f(void)` and `f()` both give an empty params list.
class FunctionDecl : public ValueDecl {
public:
    FunctionDecl(SourceLoc l, std::string n, TypeSpecPtr returnSpec,
                 std::vector<std::unique_ptr<ParamDecl>> params, std::unique_ptr<CompoundStmt> body);
    ~FunctionDecl() override;

    TypeSpecPtr returnSpec;
    std::vector<std::unique_ptr<ParamDecl>> params;
    std::unique_ptr<CompoundStmt> body;

    const FunctionType *functionType() const { return type ? cast<FunctionType>(type) : nullptr; }

    SAFEC_CLASSOF_ONE(FunctionDecl)
};

// field_declarator: ptrs IDENTIFIER arrays
class FieldDecl : public Decl {
public:
    FieldDecl(SourceLoc l, std::string n, TypeSpecPtr spec)
        : Decl(NodeKind::FieldDecl, l, std::move(n)), typeSpec(std::move(spec)) {}

    TypeSpecPtr typeSpec;
    StructDecl *parent = nullptr;   // set when added to a StructDecl
    unsigned index = 0;             // position in the struct (LLVM GEP index)
    const Type *type = nullptr;     // [sema]

    SAFEC_CLASSOF_ONE(FieldDecl)
};

// struct_def: 'struct' IDENTIFIER '{' fields '}' ';'
class StructDecl : public Decl {
public:
    StructDecl(SourceLoc l, std::string n) : Decl(NodeKind::StructDecl, l, std::move(n)) {}

    void addField(std::unique_ptr<FieldDecl> f) {
        f->parent = this;
        f->index = static_cast<unsigned>(fields.size());
        fields.push_back(std::move(f));
    }

    FieldDecl *findField(const std::string &fieldName) const {
        for (const auto &f : fields)
            if (f->name == fieldName) return f.get();
        return nullptr;
    }

    std::vector<std::unique_ptr<FieldDecl>> fields;   // use addField()
    const StructType *type = nullptr;                 // [sema]

    SAFEC_CLASSOF_ONE(StructDecl)
};

// ===================================================================
// Statements
// ===================================================================

class Stmt : public ASTNode {
public:
    SAFEC_CLASSOF_RANGE(FirstStmt, LastStmt)

protected:
    using ASTNode::ASTNode;
};

// block: '{' items '}'
class CompoundStmt : public Stmt {
public:
    explicit CompoundStmt(SourceLoc l) : Stmt(NodeKind::CompoundStmt, l) {}

    std::vector<StmtPtr> body;   // declarations appear as DeclStmt
    SourceLoc rbraceLoc;         // where block-exit work (defers, drops) happens

    SAFEC_CLASSOF_ONE(CompoundStmt)
};

// decl in a block or a for-init: one VarDecl per declarator
class DeclStmt : public Stmt {
public:
    explicit DeclStmt(SourceLoc l) : Stmt(NodeKind::DeclStmt, l) {}

    std::vector<std::unique_ptr<VarDecl>> decls;

    SAFEC_CLASSOF_ONE(DeclStmt)
};

// assignment ';'
class ExprStmt : public Stmt {
public:
    ExprStmt(SourceLoc l, ExprPtr e);
    ~ExprStmt() override;

    ExprPtr expr;

    SAFEC_CLASSOF_ONE(ExprStmt)
};

// ';'
class NullStmt : public Stmt {
public:
    explicit NullStmt(SourceLoc l) : Stmt(NodeKind::NullStmt, l) {}

    SAFEC_CLASSOF_ONE(NullStmt)
};

class IfStmt : public Stmt {
public:
    IfStmt(SourceLoc l, ExprPtr cond, StmtPtr thenS, StmtPtr elseS = nullptr);
    ~IfStmt() override;

    ExprPtr cond;
    StmtPtr thenStmt;
    StmtPtr elseStmt;   // may be null

    SAFEC_CLASSOF_ONE(IfStmt)
};

class WhileStmt : public Stmt {
public:
    WhileStmt(SourceLoc l, ExprPtr cond, StmtPtr body);
    ~WhileStmt() override;

    ExprPtr cond;
    StmtPtr body;

    SAFEC_CLASSOF_ONE(WhileStmt)
};

// 'for' '(' for_init opt_assign ';' opt_assign ')' stmt
class ForStmt : public Stmt {
public:
    ForStmt(SourceLoc l, StmtPtr init, ExprPtr cond, ExprPtr step, StmtPtr body);
    ~ForStmt() override;

    StmtPtr init;   // DeclStmt, ExprStmt, or null
    ExprPtr cond;   // may be null (loop forever)
    ExprPtr step;   // may be null
    StmtPtr body;

    SAFEC_CLASSOF_ONE(ForStmt)
};

// One `section` of a switch.  Unlike C, sections only exist inside a
// switch body, so they are owned by SwitchStmt rather than being labels.
class SwitchCase : public Stmt {
public:
    std::vector<StmtPtr> body;

    SAFEC_CLASSOF_RANGE(FirstSwitchCase, LastSwitchCase)

protected:
    using Stmt::Stmt;
};

// 'case' assignment ':' items
class CaseStmt : public SwitchCase {
public:
    CaseStmt(SourceLoc l, ExprPtr value);
    ~CaseStmt() override;

    ExprPtr value;   // sema checks it is a constant integer

    SAFEC_CLASSOF_ONE(CaseStmt)
};

// 'default' ':' items
class DefaultStmt : public SwitchCase {
public:
    explicit DefaultStmt(SourceLoc l) : SwitchCase(NodeKind::DefaultStmt, l) {}

    SAFEC_CLASSOF_ONE(DefaultStmt)
};

class SwitchStmt : public Stmt {
public:
    SwitchStmt(SourceLoc l, ExprPtr cond);
    ~SwitchStmt() override;

    ExprPtr cond;
    std::vector<std::unique_ptr<SwitchCase>> cases;   // in source order

    SAFEC_CLASSOF_ONE(SwitchStmt)
};

class ReturnStmt : public Stmt {
public:
    ReturnStmt(SourceLoc l, ExprPtr value = nullptr);
    ~ReturnStmt() override;

    ExprPtr value;   // may be null

    SAFEC_CLASSOF_ONE(ReturnStmt)
};

class BreakStmt : public Stmt {
public:
    explicit BreakStmt(SourceLoc l) : Stmt(NodeKind::BreakStmt, l) {}

    Stmt *target = nullptr;   // [sema] enclosing loop or switch

    SAFEC_CLASSOF_ONE(BreakStmt)
};

class ContinueStmt : public Stmt {
public:
    explicit ContinueStmt(SourceLoc l) : Stmt(NodeKind::ContinueStmt, l) {}

    Stmt *target = nullptr;   // [sema] enclosing loop

    SAFEC_CLASSOF_ONE(ContinueStmt)
};

// 'defer' stmt - runs `body` when the enclosing block exits
class DeferStmt : public Stmt {
public:
    DeferStmt(SourceLoc l, StmtPtr body) : Stmt(NodeKind::DeferStmt, l), body(std::move(body)) {}

    StmtPtr body;

    SAFEC_CLASSOF_ONE(DeferStmt)
};

// 'trusted' block
class TrustedStmt : public Stmt {
public:
    TrustedStmt(SourceLoc l, std::unique_ptr<CompoundStmt> body)
        : Stmt(NodeKind::TrustedStmt, l), body(std::move(body)) {}

    std::unique_ptr<CompoundStmt> body;

    SAFEC_CLASSOF_ONE(TrustedStmt)
};

// ===================================================================
// Expressions
// ===================================================================

enum class ValueCategory { RValue, LValue };

class Expr : public ASTNode {
public:
    const Type *type = nullptr;                       // [sema]
    ValueCategory category = ValueCategory::RValue;   // [sema]

    bool isLValue() const { return category == ValueCategory::LValue; }

    SAFEC_CLASSOF_RANGE(FirstExpr, LastExpr)

protected:
    using ASTNode::ASTNode;
};

// ---- literals (the parser stores decoded values) -------------------

class IntLiteral : public Expr {
public:
    IntLiteral(SourceLoc l, uint64_t v) : Expr(NodeKind::IntLiteral, l), value(v) {}
    uint64_t value;
    SAFEC_CLASSOF_ONE(IntLiteral)
};

class FloatLiteral : public Expr {
public:
    FloatLiteral(SourceLoc l, double v) : Expr(NodeKind::FloatLiteral, l), value(v) {}
    double value;
    SAFEC_CLASSOF_ONE(FloatLiteral)
};

class CharLiteral : public Expr {
public:
    CharLiteral(SourceLoc l, char v) : Expr(NodeKind::CharLiteral, l), value(v) {}
    char value;   // escapes already decoded
    SAFEC_CLASSOF_ONE(CharLiteral)
};

class StringLiteral : public Expr {
public:
    StringLiteral(SourceLoc l, std::string v) : Expr(NodeKind::StringLiteral, l), value(std::move(v)) {}
    std::string value;   // without quotes, escapes decoded
    SAFEC_CLASSOF_ONE(StringLiteral)
};

class BoolLiteral : public Expr {
public:
    BoolLiteral(SourceLoc l, bool v) : Expr(NodeKind::BoolLiteral, l), value(v) {}
    bool value;
    SAFEC_CLASSOF_ONE(BoolLiteral)
};

class NullLiteral : public Expr {
public:
    explicit NullLiteral(SourceLoc l) : Expr(NodeKind::NullLiteral, l) {}
    SAFEC_CLASSOF_ONE(NullLiteral)
};

// ---- names and operators -------------------------------------------

// IDENTIFIER used as a value
class NameExpr : public Expr {
public:
    NameExpr(SourceLoc l, std::string n) : Expr(NodeKind::NameExpr, l), name(std::move(n)) {}

    std::string name;
    ValueDecl *decl = nullptr;   // [sema] VarDecl, ParamDecl, or FunctionDecl

    SAFEC_CLASSOF_ONE(NameExpr)
};

enum class UnaryOp {
    Plus, Minus, Not, BitNot,
    Deref,        // *p
    AddrOf,       // &x      -> read borrow
    AddrOfMut,    // &mut x  -> write borrow
    PreInc, PreDec, PostInc, PostDec
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(SourceLoc l, UnaryOp op, ExprPtr operand)
        : Expr(NodeKind::UnaryExpr, l), op(op), operand(std::move(operand)) {}

    UnaryOp op;
    ExprPtr operand;

    bool isIncDec() const { return op >= UnaryOp::PreInc; }

    SAFEC_CLASSOF_ONE(UnaryExpr)
};

enum class BinaryOp {
    Mul, Div, Mod, Add, Sub, Shl, Shr,
    Lt, Gt, Le, Ge, Eq, Ne,
    BitAnd, BitXor, BitOr,
    LogAnd, LogOr
};

class BinaryExpr : public Expr {
public:
    BinaryExpr(SourceLoc l, BinaryOp op, ExprPtr lhs, ExprPtr rhs)
        : Expr(NodeKind::BinaryExpr, l), op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}

    BinaryOp op;
    ExprPtr lhs, rhs;

    bool isComparison() const { return op >= BinaryOp::Lt && op <= BinaryOp::Ne; }
    bool isLogical() const { return op == BinaryOp::LogAnd || op == BinaryOp::LogOr; }

    SAFEC_CLASSOF_ONE(BinaryExpr)
};

// Kept apart from BinaryExpr: the target must be an lvalue, and with
// own pointers a plain `=` is a move.
enum class AssignOp { Assign, Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr };

class AssignExpr : public Expr {
public:
    AssignExpr(SourceLoc l, AssignOp op, ExprPtr target, ExprPtr value)
        : Expr(NodeKind::AssignExpr, l), op(op), target(std::move(target)), value(std::move(value)) {}

    AssignOp op;
    ExprPtr target, value;

    bool isCompound() const { return op != AssignOp::Assign; }

    SAFEC_CLASSOF_ONE(AssignExpr)
};

// ---- postfix forms -------------------------------------------------

class CallExpr : public Expr {
public:
    CallExpr(SourceLoc l, ExprPtr callee) : Expr(NodeKind::CallExpr, l), callee(std::move(callee)) {}

    ExprPtr callee;
    std::vector<ExprPtr> args;

    // [sema] the called function, when the callee names one directly
    FunctionDecl *directCallee() const {
        auto *n = dyn_cast<NameExpr>(callee.get());
        return n ? dyn_cast<FunctionDecl>(n->decl) : nullptr;
    }

    SAFEC_CLASSOF_ONE(CallExpr)
};

// base '[' index ']'
class IndexExpr : public Expr {
public:
    IndexExpr(SourceLoc l, ExprPtr base, ExprPtr index)
        : Expr(NodeKind::IndexExpr, l), base(std::move(base)), index(std::move(index)) {}

    ExprPtr base, index;

    SAFEC_CLASSOF_ONE(IndexExpr)
};

// base.member  or  base->member
class MemberExpr : public Expr {
public:
    MemberExpr(SourceLoc l, ExprPtr base, std::string member, bool isArrow)
        : Expr(NodeKind::MemberExpr, l), base(std::move(base)), member(std::move(member)), isArrow(isArrow) {}

    ExprPtr base;
    std::string member;
    bool isArrow;
    FieldDecl *field = nullptr;   // [sema]

    SAFEC_CLASSOF_ONE(MemberExpr)
};

// ---- SafeC memory operations ---------------------------------------

// 'new' type_base  -> own T* (non-null)
class NewExpr : public Expr {
public:
    NewExpr(SourceLoc l, TypeSpecPtr allocated)
        : Expr(NodeKind::NewExpr, l), allocated(std::move(allocated)) {}

    TypeSpecPtr allocated;   // base only: no qualifier, stars, or arrays

    SAFEC_CLASSOF_ONE(NewExpr)
};

// 'free' '(' assignment ')'  - consumes an own pointer
class FreeExpr : public Expr {
public:
    FreeExpr(SourceLoc l, ExprPtr operand) : Expr(NodeKind::FreeExpr, l), operand(std::move(operand)) {}

    ExprPtr operand;

    SAFEC_CLASSOF_ONE(FreeExpr)
};

// init: '.' IDENTIFIER '=' assignment  |  assignment
struct Initializer {
    SourceLoc loc;
    std::string field;              // empty for a positional initializer
    ExprPtr value;
    FieldDecl *resolved = nullptr;  // [sema] target field, positional or designated

    bool isDesignated() const { return !field.empty(); }
};

// '(' qual_base ptrs ')' '{' inits '}'
class CompoundLiteralExpr : public Expr {
public:
    CompoundLiteralExpr(SourceLoc l, TypeSpecPtr spec)
        : Expr(NodeKind::CompoundLiteralExpr, l), typeSpec(std::move(spec)) {}

    TypeSpecPtr typeSpec;
    std::vector<Initializer> inits;   // non-empty per the grammar

    SAFEC_CLASSOF_ONE(CompoundLiteralExpr)
};

// ---- inserted by sema, never by the parser -------------------------

enum class CastKind {
    IntegralCast,     // between bool, char, int
    IntToFloat,
    FloatToInt,
    ToBool,           // scalar used as a condition
    NullToPointer,    // null -> T*?
    NonNullToNullable,// T* -> T*?
    Reborrow          // own/mut T* -> T* or own T* -> mut T* (borrow, not a move)
};

class ImplicitCastExpr : public Expr {
public:
    ImplicitCastExpr(CastKind ck, ExprPtr sub, const Type *to)
        : Expr(NodeKind::ImplicitCastExpr, sub->loc), castKind(ck), sub(std::move(sub)) { type = to; }

    CastKind castKind;
    ExprPtr sub;

    SAFEC_CLASSOF_ONE(ImplicitCastExpr)
};

#undef SAFEC_CLASSOF_ONE
#undef SAFEC_CLASSOF_RANGE

// ---- out-of-line members (need complete Expr / Stmt) ---------------

inline FunctionDecl::FunctionDecl(SourceLoc l, std::string n, TypeSpecPtr returnSpec,
                                  std::vector<std::unique_ptr<ParamDecl>> params,
                                  std::unique_ptr<CompoundStmt> body)
    : ValueDecl(NodeKind::FunctionDecl, l, std::move(n)), returnSpec(std::move(returnSpec)),
      params(std::move(params)), body(std::move(body)) {}
inline FunctionDecl::~FunctionDecl() = default;

inline ExprStmt::ExprStmt(SourceLoc l, ExprPtr e) : Stmt(NodeKind::ExprStmt, l), expr(std::move(e)) {}
inline ExprStmt::~ExprStmt() = default;

inline IfStmt::IfStmt(SourceLoc l, ExprPtr c, StmtPtr t, StmtPtr e)
    : Stmt(NodeKind::IfStmt, l), cond(std::move(c)), thenStmt(std::move(t)), elseStmt(std::move(e)) {}
inline IfStmt::~IfStmt() = default;

inline WhileStmt::WhileStmt(SourceLoc l, ExprPtr c, StmtPtr b)
    : Stmt(NodeKind::WhileStmt, l), cond(std::move(c)), body(std::move(b)) {}
inline WhileStmt::~WhileStmt() = default;

inline ForStmt::ForStmt(SourceLoc l, StmtPtr i, ExprPtr c, ExprPtr s, StmtPtr b)
    : Stmt(NodeKind::ForStmt, l), init(std::move(i)), cond(std::move(c)), step(std::move(s)), body(std::move(b)) {}
inline ForStmt::~ForStmt() = default;

inline CaseStmt::CaseStmt(SourceLoc l, ExprPtr v) : SwitchCase(NodeKind::CaseStmt, l), value(std::move(v)) {}
inline CaseStmt::~CaseStmt() = default;

inline SwitchStmt::SwitchStmt(SourceLoc l, ExprPtr c) : Stmt(NodeKind::SwitchStmt, l), cond(std::move(c)) {}
inline SwitchStmt::~SwitchStmt() = default;

inline ReturnStmt::ReturnStmt(SourceLoc l, ExprPtr v) : Stmt(NodeKind::ReturnStmt, l), value(std::move(v)) {}
inline ReturnStmt::~ReturnStmt() = default;

// ===================================================================
// Program - root of the tree
// ===================================================================

class Program {
public:
    Program() = default;
    Program(const Program &) = delete;
    Program &operator=(const Program &) = delete;

    // `external`s in source order: StructDecl, FunctionDecl, VarDecl
    // (a global `int a, b;` becomes two VarDecls with isGlobal set)
    std::vector<DeclPtr> decls;

    // Owns every semantic Type the AST points at, so it lives as long
    // as the tree does.
    TypeContext types;
};

// ===================================================================
// Spelling helpers for diagnostics
// ===================================================================

inline const char *unaryOpSpelling(UnaryOp op) {
    switch (op) {
    case UnaryOp::Plus: return "+";       case UnaryOp::Minus: return "-";
    case UnaryOp::Not: return "!";        case UnaryOp::BitNot: return "~";
    case UnaryOp::Deref: return "*";      case UnaryOp::AddrOf: return "&";
    case UnaryOp::AddrOfMut: return "&mut";
    case UnaryOp::PreInc: case UnaryOp::PostInc: return "++";
    case UnaryOp::PreDec: case UnaryOp::PostDec: return "--";
    }
    return "?";
}

inline const char *binaryOpSpelling(BinaryOp op) {
    switch (op) {
    case BinaryOp::Mul: return "*";    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";   case BinaryOp::Lt: return "<";
    case BinaryOp::Gt: return ">";     case BinaryOp::Le: return "<=";
    case BinaryOp::Ge: return ">=";    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitXor: return "^"; case BinaryOp::BitOr: return "|";
    case BinaryOp::LogAnd: return "&&"; case BinaryOp::LogOr: return "||";
    }
    return "?";
}

inline const char *assignOpSpelling(AssignOp op) {
    switch (op) {
    case AssignOp::Assign: return "=";  case AssignOp::Add: return "+=";
    case AssignOp::Sub: return "-=";    case AssignOp::Mul: return "*=";
    case AssignOp::Div: return "/=";    case AssignOp::Mod: return "%=";
    case AssignOp::And: return "&=";    case AssignOp::Or: return "|=";
    case AssignOp::Xor: return "^=";    case AssignOp::Shl: return "<<=";
    case AssignOp::Shr: return ">>=";
    }
    return "?";
}

// e.g. "own struct Node*?", "int[3][4]", "int(int, char*)"
inline std::string typeToString(const Type *t) {
    if (!t) return "<unresolved>";
    if (auto *b = dyn_cast<BuiltinType>(t)) {
        switch (b->builtinKind()) {
        case BuiltinKind::Void:  return "void";
        case BuiltinKind::Bool:  return "bool";
        case BuiltinKind::Char:  return "char";
        case BuiltinKind::Int:   return "int";
        case BuiltinKind::Float: return "float";
        case BuiltinKind::Null:  return "null";
        }
    }
    if (auto *p = dyn_cast<PointerType>(t)) {
        const char *q = "";
        switch (p->ptrKind()) {
        case PtrKind::Borrow:    q = "";     break;
        case PtrKind::MutBorrow: q = "mut "; break;
        case PtrKind::Own:       q = "own "; break;
        case PtrKind::Raw:       q = "raw "; break;
        }
        return q + typeToString(p->pointee()) + "*" + (p->isNullable() ? "?" : "");
    }
    if (isa<ArrayType>(t)) {
        std::string dims;
        while (auto *a = dyn_cast<ArrayType>(t)) {
            dims += a->size() ? "[" + std::to_string(*a->size()) + "]" : "[]";
            t = a->element();
        }
        return typeToString(t) + dims;
    }
    if (auto *s = dyn_cast<StructType>(t))
        return "struct " + (s->decl() ? s->decl()->name : std::string("?"));
    if (auto *f = dyn_cast<FunctionType>(t)) {
        std::string out = typeToString(f->returnType()) + "(";
        for (size_t i = 0; i < f->params().size(); i++)
            out += (i ? ", " : "") + typeToString(f->params()[i]);
        return out + ")";
    }
    return "?";
}

} // namespace safec

#endif
