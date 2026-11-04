#ifndef SAFEC_BORROWCK_H
#define SAFEC_BORROWCK_H

#include "new_ast.h"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace safec {
namespace ownership {

// ---- paths

enum class ProjKind : uint8_t {
    Deref,   // *p
    Field,   // .f  (p->f is a Deref followed by a Field)
    Index    // a[i], every index counts as the same place
};

struct Proj {
    ProjKind kind;
    const FieldDecl *field = nullptr;   // only set for Field

    bool operator==(const Proj &o) const { return kind == o.kind && field == o.field; }
    bool operator!=(const Proj &o) const { return !(*this == o); }
    bool operator<(const Proj &o) const;
};

// A place we can name: x, *x, x->f, s.f, a[i]
struct Path {
    const VarDecl *root = nullptr;      // the local, parameter or global it starts from
    std::vector<Proj> projs;

    bool isPrefixOf(const Path &o) const;          // same root, and projs is a prefix of o's
    bool isStrictPrefixOf(const Path &o) const {
        return projs.size() < o.projs.size() && isPrefixOf(o);
    }
    bool overlaps(const Path &o) const { return isPrefixOf(o) || o.isPrefixOf(*this); }

    Path deref() const;
    Path field(const FieldDecl *f) const;
    Path index() const;

    const Type *type() const;           // type at the end of the path, nullptr if we can't tell
    std::string str() const;            // how it looks in source, for messages: "n->next", "*p"

    bool operator==(const Path &o) const { return root == o.root && projs == o.projs; }
    bool operator<(const Path &o) const;
};

// ---- ownership state

// Every own path keeps the set of states it might be in, so joining two
// branches is just OR-ing the bits together.
enum OwnBits : uint8_t {
    Owned  = 1,   // points to a live allocation
    Null   = 2,   // is null
    Moved  = 4,   // was moved somewhere else, or freed
    Uninit = 8    // declared but never assigned
};

struct OwnState {
    uint8_t bits = 0;
    SourceLoc movedAt;          // last move or free, for the "moved here" note
    bool movedByFree = false;   // so we can say "after free" instead of "after move"
    SourceLoc origin;           // where the allocation came from, for leak notes
    bool prevIter = false;      // the move happened in an earlier loop iteration
};

// ---- loans

enum class LoanKind : uint8_t { Shared, Mut };

struct Loan {
    Path path;                  // what's borrowed
    LoanKind kind;
    const VarDecl *holder;      // borrow variable holding it, nullptr if it's a temporary
    SourceLoc loc;              // where the borrow was taken

    bool operator==(const Loan &o) const {
        return path == o.path && kind == o.kind && holder == o.holder &&
               loc.line == o.loc.line && loc.col == o.loc.col;
    }
};

struct FlowState {
    bool reachable = true;              // false right after return/break/continue
    std::map<Path, OwnState> own;       // paths with no entry use their type's default
    std::vector<Loan> loans;            // loans that are still alive
};

// What we know about the value of an expression, ownership-wise
struct Value {
    using Sub = std::vector<std::pair<std::vector<Proj>, OwnState>>;

    OwnState st;   // its state if it's an own pointer (bits == 0 if not, or unknown)
    Sub sub;       // states of own paths inside the value, relative to it
                   // (fields of a compound literal, the zeroed fields after
                   // `new`, or field states that come along with a move)
};

} // namespace ownership

class BorrowChecker {
public:
    explicit BorrowChecker(DiagnosticsEngine &diag) : diag_(diag) {}

    // Checks every function body in the program and reports through diag
    void run(Program &prog);

private:
    using Path      = ownership::Path;
    using Proj      = ownership::Proj;
    using OwnState  = ownership::OwnState;
    using Loan      = ownership::Loan;
    using LoanKind  = ownership::LoanKind;
    using FlowState = ownership::FlowState;
    using Value     = ownership::Value;
    using Notes     = std::vector<std::pair<SourceLoc, std::string>>;

    // How the parent expression uses a value
    enum class Use { Read, Move, LoanShared, LoanMut };
    // Kinds of access, for checking against live loans
    enum class Access { Read, Write, Move, Free, BorrowShared, BorrowMut };
    // What we were trying to do, only used to pick the error message
    enum class Action { Use, Move, Borrow, Free };
    // The ways a scope can be left
    enum class Exit { BlockEnd, Return, Break, Continue };

    struct Frame {                                  // one block
        std::vector<const VarDecl *> locals;        // in the order they were declared
        std::vector<const DeferStmt *> defers;      // in the order they were registered
    };
    struct JumpCtx {                                // a loop or switch we're inside
        const Stmt *target;
        size_t depth;                               // frames_.size() inside it
        std::vector<FlowState> breaks, continues;
    };
    struct DeferCtx {                               // a defer we're running, for the notes
        SourceLoc deferLoc, exitLoc;
        Exit kind;
    };

    DiagnosticsEngine &diag_;
    const FunctionDecl *fn_ = nullptr;
    FlowState cur_;
    std::deque<Frame> frames_;                      // a deque so references survive push/pop
    std::vector<JumpCtx> jumps_;
    std::vector<DeferCtx> deferCtx_;
    int silent_ = 0;                                // > 0 means errors are off (loop fixpoint, trusted)
    std::set<std::tuple<int, int, std::string>> reported_;

    // statements
    void checkFunction(FunctionDecl *fd);
    void walkStmt(Stmt *s);
    void walkBlock(CompoundStmt *b);
    void walkBody(Stmt *s);
    void declareLocal(VarDecl *vd);
    void walkIf(IfStmt *s);
    void walkLoop(Stmt *loop, Expr *cond, Expr *step, Stmt *body);
    void walkSwitch(SwitchStmt *s);
    void walkReturn(ReturnStmt *s);
    void walkJump(SourceLoc loc, const Stmt *target, bool isBreak);
    void evalCondition(Expr *c);
    void dropTemps();

    // leaving scopes
    void exitFrames(size_t from, SourceLoc loc, Exit kind);
    void exitFrame(size_t idx, SourceLoc loc, Exit kind, const std::set<const VarDecl *> &dying);
    void runDefer(const DeferStmt *d, SourceLoc exitLoc, Exit kind);
    void checkLeaks(const VarDecl *v, SourceLoc loc, Exit kind);
    void leakCheckPath(const Path &p, SourceLoc loc, Exit kind);
    void checkRestore(const Path &borrow, SourceLoc loc);

    // expressions
    Value eval(Expr *e, Use use, const VarDecl *holder = nullptr);
    Value evalUnary(UnaryExpr *u, const VarDecl *holder);
    Value evalAssign(AssignExpr *a, Use use, const VarDecl *holder);
    Value evalCall(CallExpr *c, Use use);
    Value evalNew(NewExpr *n, Use use);
    Value evalCompound(CompoundLiteralExpr *cl, Use use);
    void evalFree(FreeExpr *f);
    std::optional<Path> evalPlace(Expr *e);
    std::optional<Path> placeOrEval(Expr *e);
    std::optional<Path> derefOf(Expr *ptr);
    Value applyUse(const Path &p, Use use, SourceLoc loc, const VarDecl *holder);
    static Use useFor(const Type *dest);

    // reading, moving, borrowing and writing a path
    void readPath(const Path &p, SourceLoc loc);
    Value movePath(const Path &p, SourceLoc loc);
    void borrowPath(const Path &p, LoanKind k, const VarDecl *holder, SourceLoc loc);
    void writePath(const Path &p, const Value &v, SourceLoc loc);
    bool checkUsable(const Path &p, SourceLoc loc, Action a);
    bool checkLoans(const Path &p, Access acc, SourceLoc loc, bool shallow = false);
    void discardedOwner(const Expr *e, const Type *t);

    // flow state
    static OwnState get(const FlowState &s, const Path &p);
    static void eraseUnder(FlowState &s, const Path &p, bool inclusive);
    static void markUninit(FlowState &s, const VarDecl *vd);
    static void install(FlowState &s, const Path &base, const Value &v);
    static FlowState join(const FlowState &a, const FlowState &b);
    static bool sameState(const FlowState &a, const FlowState &b);
    void refine(Expr *cond, bool truth, FlowState &s);
    static void refineNull(const Path &p, bool nonNull, FlowState &s);

    // errors
    void report(SourceLoc loc, const std::string &msg, const Notes &notes = {});
};

} // namespace safec

#endif // SAFEC_BORROWCK_H
