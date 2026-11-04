/*
 * borrowck.cpp - the ownership and borrow checker
 *
 * SafeC has no goto, so we walk each function body directly instead of
 * building a CFG. While walking we carry a FlowState: for every own path,
 * the set of states it could be in, plus the loans that are still alive.
 * At the end of an if/else the two branch states are merged by union.
 * Loops are run to a fixpoint with errors turned off, then walked one
 * more time with errors on.
 *
 * Rule names in the comments (M1, B2, D9, ...) from my notes
 */
#include "borrowck.h"

#include <algorithm>
#include <functional>

namespace safec {

using namespace ownership;

// ===================================================================
// Paths
// ===================================================================

bool Proj::operator<(const Proj &o) const {
    if (kind != o.kind) return kind < o.kind;
    return std::less<const FieldDecl *>()(field, o.field);
}

bool Path::isPrefixOf(const Path &o) const {
    if (root != o.root || projs.size() > o.projs.size()) return false;
    return std::equal(projs.begin(), projs.end(), o.projs.begin());
}

Path Path::deref() const {
    Path p = *this;
    p.projs.push_back({ProjKind::Deref, nullptr});
    return p;
}

Path Path::field(const FieldDecl *f) const {
    Path p = *this;
    p.projs.push_back({ProjKind::Field, f});
    return p;
}

Path Path::index() const {
    Path p = *this;
    p.projs.push_back({ProjKind::Index, nullptr});
    return p;
}

const Type *Path::type() const {
    const Type *t = root ? root->type : nullptr;
    for (const Proj &p : projs) {
        if (!t) return nullptr;
        switch (p.kind) {
        case ProjKind::Deref: {
            auto *pt = dyn_cast<PointerType>(t);
            t = pt ? pt->pointee() : nullptr;
            break;
        }
        case ProjKind::Field:
            t = p.field ? p.field->type : nullptr;
            break;
        case ProjKind::Index: {
            auto *at = dyn_cast<ArrayType>(t);
            t = at ? at->element() : nullptr;
            break;
        }
        }
    }
    return t;
}

std::string Path::str() const {
    std::string s = root ? root->name : std::string("?");
    bool prefix = false;   // s starts with a '*' we added, so it needs parens before ., -> or [_]
    auto postfix = [&](const std::string &op) {
        if (prefix) s = "(" + s + ")";
        s += op;
        prefix = false;
    };
    for (size_t i = 0; i < projs.size(); ++i) {
        const Proj &p = projs[i];
        switch (p.kind) {
        case ProjKind::Deref:
            if (i + 1 < projs.size() && projs[i + 1].kind == ProjKind::Field) {
                const FieldDecl *f = projs[++i].field;
                postfix("->" + (f ? f->name : std::string("?")));
            } else {
                s = "*" + s;
                prefix = true;
            }
            break;
        case ProjKind::Field:
            postfix("." + (p.field ? p.field->name : std::string("?")));
            break;
        case ProjKind::Index:
            postfix("[_]");
            break;
        }
    }
    return s;
}

bool Path::operator<(const Path &o) const {
    if (root != o.root) return std::less<const VarDecl *>()(root, o.root);
    return std::lexicographical_compare(projs.begin(), projs.end(), o.projs.begin(), o.projs.end());
}

// ===================================================================
// Helpers for types and expressions
// ===================================================================

namespace {

constexpr int kMaxDepth = 16;        // in case Sema missed a struct that contains itself
constexpr int kMaxLoopIters = 64;    // just a safety limit, loops settle much sooner

const PointerType *asPtr(const Type *t) { return t ? dyn_cast<PointerType>(t) : nullptr; }

bool isOwnPtr(const Type *t) {
    auto *p = asPtr(t);
    return p && p->ptrKind() == PtrKind::Own;
}

bool isBorrowPtr(const Type *t) {
    auto *p = asPtr(t);
    return p && (p->ptrKind() == PtrKind::Borrow || p->ptrKind() == PtrKind::MutBorrow);
}

bool isRawPtr(const Type *t) {
    auto *p = asPtr(t);
    return p && p->ptrKind() == PtrKind::Raw;
}

const StructDecl *structOf(const Type *t) {
    auto *s = t ? dyn_cast<StructType>(t) : nullptr;
    return s ? s->decl() : nullptr;
}

// True for own pointers, and for structs/arrays that hold one by value
bool containsOwn(const Type *t, int depth = 0) {
    if (!t || depth > kMaxDepth) return false;
    if (isOwnPtr(t)) return true;
    if (auto *a = dyn_cast<ArrayType>(t)) return containsOwn(a->element(), depth + 1);
    if (const StructDecl *sd = structOf(t))
        for (const auto &f : sd->fields)
            if (containsOwn(f->type, depth + 1)) return true;
    return false;
}

// Collects the paths of the own pointers stored by value in an object of
// type t that lives at base
void ownFieldPaths(const Type *t, const Path &base, std::vector<Path> &out, int depth = 0) {
    const StructDecl *sd = structOf(t);
    if (!sd || depth > kMaxDepth) return;
    for (const auto &f : sd->fields) {
        if (isOwnPtr(f->type))
            out.push_back(base.field(f.get()));
        else if (structOf(f->type))
            ownFieldPaths(f->type, base.field(f.get()), out, depth + 1);
    }
}

// Every own field of a zeroed object of type t starts out null (D1: new zero-fills)
void zeroFill(const Type *t, const std::vector<Proj> &prefix, Value::Sub &out, int depth = 0) {
    const StructDecl *sd = structOf(t);
    if (!sd || depth > kMaxDepth) return;
    for (const auto &f : sd->fields) {
        std::vector<Proj> p = prefix;
        p.push_back({ProjKind::Field, f.get()});
        if (isOwnPtr(f->type)) {
            OwnState z;
            z.bits = Null;
            out.push_back({p, z});
        } else if (structOf(f->type)) {
            zeroFill(f->type, p, out, depth + 1);
        }
    }
}

// The state of a path we have no entry for, going only by its type
uint8_t defaultBits(const Type *t) {
    auto *p = asPtr(t);
    if (!p || p->ptrKind() != PtrKind::Own) return 0;
    return p->isNullable() ? (Owned | Null) : Owned;
}

OwnState defaultState(const Type *t) {
    OwnState s;
    s.bits = defaultBits(t);
    return s;
}

// Union of two states, used when branches meet
OwnState merge(const OwnState &a, const OwnState &b) {
    OwnState r = a;
    r.bits = a.bits | b.bits;
    if (!(a.bits & Moved) && (b.bits & Moved)) {
        r.movedAt = b.movedAt;
        r.movedByFree = b.movedByFree;
    }
    if (!r.origin.isValid()) r.origin = b.origin;
    r.prevIter = a.prevIter || b.prevIter;
    return r;
}

// Expressions that name a place: a variable, *p, s.f, p->f, a[i]
bool isPlaceExpr(const Expr *e) {
    if (auto *n = dyn_cast<NameExpr>(e)) return isa<VarDecl>(n->decl);
    if (auto *u = dyn_cast<UnaryExpr>(e)) return u->op == UnaryOp::Deref;
    return isa<MemberExpr>(e) || isa<IndexExpr>(e);
}

bool isNullExpr(const Expr *e) {
    while (auto *c = dyn_cast<ImplicitCastExpr>(e)) e = c->sub.get();
    return isa<NullLiteral>(e);
}

// The path of an expression, without evaluating anything. We don't track
// anything behind a raw pointer, so those give nullopt.
std::optional<Path> pathOf(const Expr *e) {
    while (auto *c = dyn_cast<ImplicitCastExpr>(e)) e = c->sub.get();
    if (!e) return std::nullopt;
    if (auto *n = dyn_cast<NameExpr>(e)) {
        if (auto *v = dyn_cast<VarDecl>(n->decl)) return Path{v, {}};
        return std::nullopt;
    }
    if (auto *u = dyn_cast<UnaryExpr>(e)) {
        if (u->op != UnaryOp::Deref) return std::nullopt;
        auto b = pathOf(u->operand.get());
        if (!b || isRawPtr(b->type())) return std::nullopt;
        return b->deref();
    }
    if (auto *m = dyn_cast<MemberExpr>(e)) {
        if (!m->field) return std::nullopt;
        auto b = pathOf(m->base.get());
        if (!b) return std::nullopt;
        if (m->isArrow) {
            if (isRawPtr(b->type())) return std::nullopt;
            b = b->deref();
        }
        return b->field(m->field);
    }
    if (auto *ix = dyn_cast<IndexExpr>(e)) {
        auto b = pathOf(ix->base.get());
        if (!b || asPtr(b->type())) return std::nullopt;
        return b->index();
    }
    return std::nullopt;
}

// True if the path goes through a shared borrow (T*). You can't write,
// move or free anything reached that way.
bool behindShared(const Path &p) {
    const Type *t = p.root ? p.root->type : nullptr;
    for (const Proj &pr : p.projs) {
        if (!t) return false;
        switch (pr.kind) {
        case ProjKind::Deref: {
            auto *pt = asPtr(t);
            if (!pt) return false;
            if (pt->ptrKind() == PtrKind::Borrow) return true;
            t = pt->pointee();
            break;
        }
        case ProjKind::Field:
            t = pr.field ? pr.field->type : nullptr;
            break;
        case ProjKind::Index: {
            auto *at = dyn_cast<ArrayType>(t);
            t = at ? at->element() : nullptr;
            break;
        }
        }
    }
    return false;
}

// For leak messages: "'x' is leaked <phrase>"
const char *exitPhrase(int kind) {
    switch (kind) {
    case 0:  return "at end of scope";
    case 1:  return "when returning";
    case 2:  return "on 'break'";
    default: return "on 'continue'";
    }
}

std::string quote(const Path &p) { return "'" + p.str() + "'"; }

} // namespace

// ===================================================================
// Entry point
// ===================================================================

void BorrowChecker::run(Program &prog) {
    for (auto &d : prog.decls)
        if (auto *fd = dyn_cast<FunctionDecl>(d.get()))
            if (!fd->isInvalid() && fd->body && fd->type) checkFunction(fd);
}

void BorrowChecker::checkFunction(FunctionDecl *fd) {
    fn_ = fd;
    cur_ = FlowState{};
    frames_.clear();
    jumps_.clear();
    deferCtx_.clear();
    silent_ = 0;

    // The parameters get their own frame around the body. Own params start
    // in their type's default state. Borrow params start with no loans,
    // since whatever they point to is the caller's business.
    frames_.emplace_back();
    for (auto &p : fd->params) frames_.back().locals.push_back(p.get());

    // Falling off the end counts as `return;`. The body's locals and the
    // params die together, so pointing a borrow param at a body local isn't
    // reported as the local not living long enough.
    frames_.emplace_back();
    for (auto &s : fd->body->body) walkStmt(s.get());
    SourceLoc end = fd->body->rbraceLoc.isValid() ? fd->body->rbraceLoc : fd->loc;
    exitFrames(0, end, Exit::BlockEnd);

    frames_.clear();
    fn_ = nullptr;
}

// ===================================================================
// Statements
// ===================================================================

void BorrowChecker::walkStmt(Stmt *s) {
    if (!s || !cur_.reachable) return;   // unreachable code, nothing to track

    switch (s->kind()) {
    case NodeKind::CompoundStmt:
        walkBlock(cast<CompoundStmt>(s));
        break;

    case NodeKind::DeclStmt:
        for (auto &vd : cast<DeclStmt>(s)->decls) declareLocal(vd.get());
        dropTemps();
        break;

    case NodeKind::ExprStmt:
        eval(cast<ExprStmt>(s)->expr.get(), Use::Read);
        dropTemps();
        break;

    case NodeKind::IfStmt:
        walkIf(cast<IfStmt>(s));
        break;

    case NodeKind::WhileStmt: {
        auto *w = cast<WhileStmt>(s);
        walkLoop(w, w->cond.get(), nullptr, w->body.get());
        break;
    }

    case NodeKind::ForStmt: {
        // init only runs once, and lives in a frame around the whole loop
        auto *f = cast<ForStmt>(s);
        frames_.emplace_back();
        walkStmt(f->init.get());
        walkLoop(f, f->cond.get(), f->step.get(), f->body.get());
        SourceLoc end = f->loc;
        if (auto *b = dyn_cast<CompoundStmt>(f->body.get()))
            if (b->rbraceLoc.isValid()) end = b->rbraceLoc;
        exitFrames(frames_.size() - 1, end, Exit::BlockEnd);
        frames_.pop_back();
        break;
    }

    case NodeKind::SwitchStmt:
        walkSwitch(cast<SwitchStmt>(s));
        break;

    case NodeKind::ReturnStmt:
        walkReturn(cast<ReturnStmt>(s));
        break;

    case NodeKind::BreakStmt:
        walkJump(s->loc, cast<BreakStmt>(s)->target, true);
        break;

    case NodeKind::ContinueStmt:
        walkJump(s->loc, cast<ContinueStmt>(s)->target, false);
        break;

    case NodeKind::DeferStmt:
        // Just remember it here. It runs when the block is left.
        frames_.back().defers.push_back(cast<DeferStmt>(s));
        break;

    case NodeKind::TrustedStmt:
        // No errors inside trusted, but its effects still count, so
        // `trusted { free(x); }` still frees x (R1, D2).
        ++silent_;
        walkStmt(cast<TrustedStmt>(s)->body.get());
        --silent_;
        break;

    default:   // NullStmt. Case labels are handled in walkSwitch.
        break;
    }
}

void BorrowChecker::walkBlock(CompoundStmt *b) {
    frames_.emplace_back();
    for (auto &s : b->body) walkStmt(s.get());
    exitFrames(frames_.size() - 1, b->rbraceLoc.isValid() ? b->rbraceLoc : b->loc, Exit::BlockEnd);
    frames_.pop_back();
}

// Like in C, the body of an if/else/while/for is its own scope even
// without braces. So in `if (c) defer free(x);` the defer runs at the end
// of that body, and only on the paths that actually went through it.
void BorrowChecker::walkBody(Stmt *s) {
    if (!s || isa<CompoundStmt>(s)) {
        walkStmt(s);
        return;
    }
    frames_.emplace_back();
    walkStmt(s);
    exitFrames(frames_.size() - 1, s->loc, Exit::BlockEnd);
    frames_.pop_back();
}

void BorrowChecker::declareLocal(VarDecl *vd) {
    if (!cur_.reachable) return;
    const Type *t = vd->type;
    const Path p{vd, {}};

    if (vd->init) {
        // a borrow variable holds the loans its initializer creates
        const VarDecl *holder = isBorrowPtr(t) ? vd : nullptr;
        Value v = eval(vd->init.get(), useFor(t), holder);
        eraseUnder(cur_, p, true);
        if (isOwnPtr(t)) cur_.own[p] = v.st.bits ? v.st : defaultState(t);
        install(cur_, p, v);
    } else {
        eraseUnder(cur_, p, true);
        markUninit(cur_, vd);   // no initializer: uninitialized until assigned (D5)
    }
    frames_.back().locals.push_back(vd);
}

void BorrowChecker::evalCondition(Expr *c) {
    eval(c, Use::Read);
    dropTemps();
}

void BorrowChecker::dropTemps() {
    auto &ls = cur_.loans;
    ls.erase(std::remove_if(ls.begin(), ls.end(), [](const Loan &l) { return l.holder == nullptr; }),
             ls.end());
}

void BorrowChecker::walkIf(IfStmt *s) {
    evalCondition(s->cond.get());
    FlowState base = cur_;

    refine(s->cond.get(), true, cur_);
    walkBody(s->thenStmt.get());
    FlowState thenOut = std::move(cur_);

    cur_ = std::move(base);
    refine(s->cond.get(), false, cur_);
    walkBody(s->elseStmt.get());

    cur_ = join(thenOut, cur_);
}

void BorrowChecker::walkLoop(Stmt *loop, Expr *cond, Expr *step, Stmt *body) {
    const FlowState before = cur_;
    jumps_.push_back({loop, frames_.size(), {}, {}});
    const size_t ji = jumps_.size() - 1;

    // One trip around the loop, starting from `start`. `back` gets the
    // state that goes back to the top (end of the body, continues, then
    // the step) and `exit` the state that leaves the loop (condition
    // false, or a break).
    auto trip = [&](const FlowState &start, FlowState &back, FlowState &exit) {
        jumps_[ji].breaks.clear();
        jumps_[ji].continues.clear();

        cur_ = start;
        if (cond) evalCondition(cond);
        FlowState afterCond = cur_;

        if (cond) refine(cond, true, cur_);
        walkBody(body);
        for (const FlowState &c : jumps_[ji].continues) cur_ = join(cur_, c);
        if (step && cur_.reachable) {
            eval(step, Use::Read);
            dropTemps();
        }
        back = std::move(cur_);

        exit = std::move(afterCond);
        if (cond) refine(cond, false, exit);
        else exit.reachable = false;   // for (;;) can only be left with a break
        for (const FlowState &b : jumps_[ji].breaks) exit = join(exit, b);
    };

    // First find the fixpoint with errors turned off. The states only ever
    // grow, and there are only so many paths and bits, so this always stops.
    std::set<Path> flagged;   // paths that got their Moved bit from an earlier iteration
    FlowState entry = before, back, exit;
    ++silent_;
    for (int iter = 0; iter < kMaxLoopIters; ++iter) {
        trip(entry, back, exit);
        FlowState next = join(entry, back);
        for (auto &kv : next.own) {
            if ((kv.second.bits & Moved) && !(get(before, kv.first).bits & Moved)) {
                kv.second.prevIter = true;
                flagged.insert(kv.first);
            }
        }
        const bool stable = sameState(next, entry);
        entry = std::move(next);
        if (stable) break;
    }
    --silent_;

    // Then one last trip from the stable state, this time reporting errors.
    trip(entry, back, exit);
    cur_ = std::move(exit);
    for (const Path &p : flagged) {
        auto it = cur_.own.find(p);
        if (it != cur_.own.end()) it->second.prevIter = false;
    }
    jumps_.pop_back();
}

void BorrowChecker::walkSwitch(SwitchStmt *sw) {
    evalCondition(sw->cond.get());

    // All the cases share one scope, same as in Sema. A break leaves the
    // cases but not that scope, which we exit once after the switch.
    frames_.emplace_back();
    jumps_.push_back({sw, frames_.size(), {}, {}});
    const size_t ji = jumps_.size() - 1;

    // A local declared directly in a case stays in scope for the rest of
    // the switch, but it's only initialized on paths that went through it.
    for (auto &c : sw->cases)
        for (auto &st : c->body)
            if (auto *ds = dyn_cast<DeclStmt>(st.get()))
                for (auto &vd : ds->decls) markUninit(cur_, vd.get());

    const FlowState atSwitch = cur_;
    FlowState fall;
    fall.reachable = false;
    bool hasDefault = false;

    for (auto &c : sw->cases) {
        if (isa<DefaultStmt>(c.get())) hasDefault = true;
        cur_ = join(atSwitch, fall);   // we either jumped to this case or fell into it
        for (auto &st : c->body) walkStmt(st.get());
        fall = std::move(cur_);
    }

    cur_ = std::move(fall);
    for (const FlowState &b : jumps_[ji].breaks) cur_ = join(cur_, b);
    if (!hasDefault) cur_ = join(cur_, atSwitch);   // no case matched
    jumps_.pop_back();

    exitFrames(frames_.size() - 1, sw->loc, Exit::BlockEnd);
    frames_.pop_back();
}

void BorrowChecker::walkReturn(ReturnStmt *s) {
    // The return value is computed first, and then the defers run (D9).
    if (s->value) {
        const FunctionType *ft = fn_->functionType();
        eval(s->value.get(), useFor(ft ? ft->returnType() : nullptr));
        dropTemps();
    }
    exitFrames(0, s->loc, Exit::Return);
    cur_.reachable = false;
}

void BorrowChecker::walkJump(SourceLoc loc, const Stmt *target, bool isBreak) {
    int idx = -1;
    for (int i = static_cast<int>(jumps_.size()) - 1; i >= 0; --i) {
        if (jumps_[i].target == target) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {   // Sema already complained about this one
        cur_.reachable = false;
        return;
    }
    exitFrames(jumps_[idx].depth, loc, isBreak ? Exit::Break : Exit::Continue);
    (isBreak ? jumps_[idx].breaks : jumps_[idx].continues).push_back(cur_);
    cur_.reachable = false;
}

// ===================================================================
// Leaving scopes
// ===================================================================

// Leaves frames_[from..], innermost first. Every way out of a block comes
// through here: reaching the `}`, return, break and continue.
void BorrowChecker::exitFrames(size_t from, SourceLoc loc, Exit kind) {
    if (!cur_.reachable) return;
    std::set<const VarDecl *> dying;
    for (size_t i = from; i < frames_.size(); ++i)
        dying.insert(frames_[i].locals.begin(), frames_[i].locals.end());
    for (size_t i = frames_.size(); i-- > from;) exitFrame(i, loc, kind, dying);
}

void BorrowChecker::exitFrame(size_t idx, SourceLoc loc, Exit kind,
                              const std::set<const VarDecl *> &dying) {
    if (!cur_.reachable) return;

    // Copy these, since running a defer can push and pop frames
    const std::vector<const DeferStmt *> defers = frames_[idx].defers;
    const std::vector<const VarDecl *> locals = frames_[idx].locals;

    // 1. Run the defers, last registered first
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) runDefer(*it, loc, kind);

    // 2. Check for leaks (M4) and for things not put back (M5), last declared first
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
        checkLeaks(*it, loc, kind);
        if (isBorrowPtr((*it)->type)) checkRestore(Path{*it, {}}, loc);
    }

    // 3. Loans held by these locals end here
    const std::set<const VarDecl *> mine(locals.begin(), locals.end());
    auto &ls = cur_.loans;
    ls.erase(std::remove_if(ls.begin(), ls.end(), [&](const Loan &l) { return mine.count(l.holder) > 0; }),
             ls.end());

    // 4. A loan on one of these locals whose holder is still alive would
    //    dangle, since the borrow would outlive its owner. We only report
    //    one error per holder, because a reborrow can hold loans on both
    //    `r` and whatever r points to.
    std::set<const VarDecl *> blamed;
    for (auto it = ls.begin(); it != ls.end();) {
        if (!mine.count(it->path.root)) {
            ++it;
            continue;
        }
        if (it->holder && !dying.count(it->holder) && blamed.insert(it->holder).second) {
            report(loc, "'" + it->path.root->name + "' does not live long enough",
                   {{it->loc, quote(it->path) + " was borrowed here"},
                    {it->holder->loc, "the borrow is held by '" + it->holder->name +
                                          "', which is still in scope after this point"}});
        }
        it = ls.erase(it);
    }

    // 5. Forget about these locals
    for (auto it = cur_.own.begin(); it != cur_.own.end();)
        it = mine.count(it->first.root) ? cur_.own.erase(it) : std::next(it);
}

void BorrowChecker::runDefer(const DeferStmt *d, SourceLoc exitLoc, Exit kind) {
    deferCtx_.push_back({d->loc, exitLoc, kind});
    Stmt *body = d->body.get();
    if (isa<CompoundStmt>(body)) {
        walkStmt(body);
    } else {   // a single statement still gets a scope of its own
        frames_.emplace_back();
        walkStmt(body);
        exitFrames(frames_.size() - 1, d->loc, Exit::BlockEnd);
        frames_.pop_back();
    }
    deferCtx_.pop_back();
}

void BorrowChecker::checkLeaks(const VarDecl *v, SourceLoc loc, Exit kind) {
    const Path p{v, {}};
    if (isOwnPtr(v->type)) {
        leakCheckPath(p, loc, kind);
        return;
    }
    std::vector<Path> fields;
    ownFieldPaths(v->type, p, fields);
    for (const Path &q : fields) leakCheckPath(q, loc, kind);
}

void BorrowChecker::leakCheckPath(const Path &p, SourceLoc loc, Exit kind) {
    const OwnState s = get(cur_, p);
    if (!(s.bits & Owned)) return;

    const std::string where = exitPhrase(static_cast<int>(kind));
    std::string msg;
    Notes notes;
    if (s.bits & (Moved | Uninit)) {
        msg = quote(p) + " is leaked on some paths " + where;
        if (s.bits & Moved)
            notes.push_back({s.movedAt, std::string(s.movedByFree ? "freed" : "moved") +
                                            " here, but only on some paths"});
    } else if (s.bits & Null) {
        msg = quote(p) + " may be leaked " + where + "; free it if it is non-null";
    } else {
        msg = quote(p) + " is leaked " + where;
    }
    notes.push_back({s.origin, "allocated here"});
    report(loc, msg, notes);
}

// M5: anything moved out through borrow b has to be put back before b
// goes out of scope or gets pointed somewhere else.
void BorrowChecker::checkRestore(const Path &b, SourceLoc loc) {
    const Path inner = b.deref();
    for (const auto &kv : cur_.own) {
        const OwnState &s = kv.second;
        if (!inner.isPrefixOf(kv.first) || !(s.bits & (Moved | Uninit))) continue;
        const bool always = (s.bits & ~(Moved | Uninit)) == 0;
        report(loc, quote(kv.first) + (always ? " was" : " may have been") +
                        " moved out of borrowed data and not restored",
               {{s.movedAt, std::string(s.movedByFree ? "freed" : "moved") + " here"}});
    }
}

// ===================================================================
// Expressions
// ===================================================================

BorrowChecker::Use BorrowChecker::useFor(const Type *dest) {
    // Storing into a struct moves an rvalue. For an lvalue it's just a
    // copy (Sema already rejects copies of owning structs, T2).
    if (isOwnPtr(dest) || structOf(dest)) return Use::Move;
    if (auto *p = asPtr(dest)) {
        if (p->ptrKind() == PtrKind::MutBorrow) return Use::LoanMut;
        if (p->ptrKind() == PtrKind::Borrow) return Use::LoanShared;
    }
    return Use::Read;
}

Value BorrowChecker::eval(Expr *e, Use use, const VarDecl *holder) {
    if (!e || !cur_.reachable) return {};

    if (isPlaceExpr(e)) {
        std::optional<Path> p = evalPlace(e);
        return p ? applyUse(*p, use, e->loc, holder) : Value{};
    }

    switch (e->kind()) {
    case NodeKind::NullLiteral: {
        Value v;
        v.st.bits = Null;
        return v;
    }

    case NodeKind::UnaryExpr:
        return evalUnary(cast<UnaryExpr>(e), holder);

    case NodeKind::BinaryExpr: {
        auto *b = cast<BinaryExpr>(e);
        eval(b->lhs.get(), Use::Read);
        if (b->isLogical()) {
            // the right side only runs some of the time
            const bool isAnd = b->op == BinaryOp::LogAnd;
            FlowState skipped = cur_;
            refine(b->lhs.get(), !isAnd, skipped);
            refine(b->lhs.get(), isAnd, cur_);
            eval(b->rhs.get(), Use::Read);
            cur_ = join(skipped, cur_);
        } else {
            eval(b->rhs.get(), Use::Read);
        }
        return {};
    }

    case NodeKind::AssignExpr:
        return evalAssign(cast<AssignExpr>(e), use, holder);

    case NodeKind::CallExpr:
        return evalCall(cast<CallExpr>(e), use);

    case NodeKind::NewExpr:
        return evalNew(cast<NewExpr>(e), use);

    case NodeKind::FreeExpr:
        evalFree(cast<FreeExpr>(e));
        return {};

    case NodeKind::CompoundLiteralExpr:
        return evalCompound(cast<CompoundLiteralExpr>(e), use);

    case NodeKind::ImplicitCastExpr: {
        auto *c = cast<ImplicitCastExpr>(e);
        switch (c->castKind) {
        case CastKind::Reborrow: {
            // an own/mut T* used as a T* (or own used as mut): that's a borrow, not a move
            auto *to = asPtr(c->type);
            const bool m = to && to->ptrKind() == PtrKind::MutBorrow;
            return eval(c->sub.get(), m ? Use::LoanMut : Use::LoanShared, holder);
        }
        case CastKind::NonNullToNullable:
        case CastKind::NullToPointer:
            // only the type changes, the value is used the same way
            return eval(c->sub.get(), use, holder);
        default:
            eval(c->sub.get(), Use::Read);
            return {};
        }
    }

    default:   // literals and function names
        return {};
    }
}

Value BorrowChecker::evalUnary(UnaryExpr *u, const VarDecl *holder) {
    Expr *op = u->operand.get();

    if (u->op == UnaryOp::AddrOf || u->op == UnaryOp::AddrOfMut) {
        const LoanKind k = u->op == UnaryOp::AddrOfMut ? LoanKind::Mut : LoanKind::Shared;
        if (isPlaceExpr(op)) {
            if (auto p = evalPlace(op)) borrowPath(*p, k, holder, u->loc);
        } else {
            eval(op, Use::Read);
        }
        return {};
    }

    if (u->isIncDec()) {
        if (isPlaceExpr(op)) {
            if (auto p = evalPlace(op)) {
                readPath(*p, u->loc);
                writePath(*p, Value{}, u->loc);
            }
        } else {
            eval(op, Use::Read);
        }
        return {};
    }

    eval(op, Use::Read);   // + - ! ~  (a deref is a place, so eval already handled it)
    return {};
}

Value BorrowChecker::evalAssign(AssignExpr *a, Use use, const VarDecl *holder) {
    Expr *target = a->target.get();

    if (a->isCompound()) {   // x += ... never involves a tracked pointer
        eval(a->value.get(), Use::Read);
        if (isPlaceExpr(target)) {
            if (auto p = evalPlace(target)) {
                readPath(*p, a->loc);
                writePath(*p, Value{}, a->loc);
            }
        } else {
            eval(target, Use::Read);
        }
        return {};
    }

    // Right side first (D8), then the write. If the target is a borrow
    // variable r, the loans made on the right side of `r = ...` belong to r.
    const std::optional<Path> tp = pathOf(target);
    const Type *tt = target->type ? target->type : (tp ? tp->type() : nullptr);
    const VarDecl *h = (tp && isBorrowPtr(tt)) ? tp->root : nullptr;
    Value v = eval(a->value.get(), useFor(tt), h);

    if (!isPlaceExpr(target)) {
        eval(target, Use::Read);
        return {};
    }
    std::optional<Path> p = evalPlace(target);
    if (!p) return {};
    writePath(*p, v, a->loc);

    // for `x = (y = e)` the result is y, used however the parent wants it
    if (use != Use::Read) return applyUse(*p, use, a->loc, holder);
    return {};
}

Value BorrowChecker::evalCall(CallExpr *c, Use use) {
    const FunctionType *ft = nullptr;
    if (c->callee && c->callee->type) ft = dyn_cast<FunctionType>(c->callee->type);
    if (!ft)
        if (FunctionDecl *fd = c->directCallee()) ft = fd->functionType();

    eval(c->callee.get(), Use::Read);
    for (size_t i = 0; i < c->args.size(); ++i) {
        const Type *pt = (ft && i < ft->params().size()) ? ft->params()[i] : nullptr;
        // left to right (D8), each one used the way its parameter type says
        eval(c->args[i].get(), pt ? useFor(pt) : Use::Read);
    }

    const Type *rt = ft ? ft->returnType() : c->type;
    Value v;
    if (isOwnPtr(rt)) {
        v.st = defaultState(rt);
        v.st.origin = c->loc;
    }
    if (use != Use::Move && containsOwn(rt)) discardedOwner(c, rt);
    return v;
}

Value BorrowChecker::evalNew(NewExpr *n, Use use) {
    Value v;
    v.st.bits = Owned;
    v.st.origin = n->loc;
    const Type *pointee = nullptr;
    if (auto *pt = asPtr(n->type)) pointee = pt->pointee();
    else if (n->allocated) pointee = n->allocated->resolved;
    zeroFill(pointee, {Proj{ProjKind::Deref, nullptr}}, v.sub);   // new zero-fills (D1)
    if (use != Use::Move) discardedOwner(n, n->type);
    return v;
}

Value BorrowChecker::evalCompound(CompoundLiteralExpr *cl, Use use) {
    const Type *t = cl->type ? cl->type : (cl->typeSpec ? cl->typeSpec->resolved : nullptr);
    const StructDecl *sd = structOf(t);
    Value v;
    std::set<const FieldDecl *> seen;
    size_t next = 0;

    for (auto &init : cl->inits) {
        const FieldDecl *f = init.resolved;
        if (!f && sd) {   // in case Sema didn't fill in `resolved`
            if (init.isDesignated()) f = sd->findField(init.field);
            else if (next < sd->fields.size()) f = sd->fields[next].get();
        }
        if (f) next = f->index + 1;

        Value iv = eval(init.value.get(), f ? useFor(f->type) : Use::Read);
        if (!f) continue;
        seen.insert(f);

        const std::vector<Proj> base{Proj{ProjKind::Field, f}};
        if (isOwnPtr(f->type)) v.sub.push_back({base, iv.st.bits ? iv.st : defaultState(f->type)});
        for (auto &rs : iv.sub) {
            std::vector<Proj> r = base;
            r.insert(r.end(), rs.first.begin(), rs.first.end());
            v.sub.push_back({r, rs.second});
        }
    }

    // fields left out are zero, so their own pointers start out null
    if (sd) {
        for (auto &f : sd->fields) {
            if (seen.count(f.get())) continue;
            const std::vector<Proj> base{Proj{ProjKind::Field, f.get()}};
            if (isOwnPtr(f->type)) {
                OwnState z;
                z.bits = Null;
                v.sub.push_back({base, z});
            } else {
                zeroFill(f->type, base, v.sub);
            }
        }
    }

    if (use != Use::Move) {
        bool owns = false;
        for (auto &rs : v.sub) owns |= (rs.second.bits & Owned) != 0;
        if (owns) discardedOwner(cl, t);
    }
    return v;
}

void BorrowChecker::evalFree(FreeExpr *f) {
    Expr *op = f->operand.get();
    while (auto *c = dyn_cast<ImplicitCastExpr>(op)) {
        if (c->castKind != CastKind::NonNullToNullable) break;
        op = c->sub.get();
    }

    // free(new T), free(make()): nothing we track changes
    if (!isPlaceExpr(op)) {
        eval(op, Use::Move);
        return;
    }
    std::optional<Path> p = evalPlace(op);
    if (!p || !isOwnPtr(p->type())) return;   // raw (not tracked), or Sema already complained

    if (behindShared(*p)) {   // report it, but leave the state alone
        report(f->loc, "cannot free " + quote(*p) + ", which is behind a shared borrow");
        return;
    }
    const bool ok = checkUsable(*p, f->loc, Action::Free);
    checkLoans(*p, Access::Free, f->loc);   // B2: can't free something that is borrowed

    if (ok) {
        const OwnState s = get(cur_, *p);
        if (s.bits & Null) {
            report(f->loc, quote(*p) + " may be null; check it before free");   // M8
        } else {
            // M7: every own field of *p has to be null or moved out first (D7)
            std::vector<Path> fields;
            ownFieldPaths(asPtr(p->type())->pointee(), p->deref(), fields);
            for (const Path &q : fields)
                if (get(cur_, q).bits & Owned)
                    report(f->loc, "free(" + quote(*p) + ") would leak " + quote(q) +
                                       "; set it to null or move it first");
        }
    }

    OwnState m;
    m.bits = Moved;
    m.movedAt = f->loc;
    m.movedByFree = true;
    cur_.own[*p] = m;
    eraseUnder(cur_, *p, false);
}

// Evaluates a place expression and returns its path. On the way it reads
// every pointer that gets dereferenced and evaluates any index expressions.
std::optional<Path> BorrowChecker::evalPlace(Expr *e) {
    if (auto *n = dyn_cast<NameExpr>(e)) {
        if (auto *v = dyn_cast<VarDecl>(n->decl)) return Path{v, {}};
        return std::nullopt;
    }
    if (auto *u = dyn_cast<UnaryExpr>(e)) {
        if (u->op != UnaryOp::Deref) return std::nullopt;
        return derefOf(u->operand.get());
    }
    if (auto *m = dyn_cast<MemberExpr>(e)) {
        std::optional<Path> base = m->isArrow ? derefOf(m->base.get()) : placeOrEval(m->base.get());
        if (!base || !m->field) return std::nullopt;
        return base->field(m->field);
    }
    if (auto *ix = dyn_cast<IndexExpr>(e)) {
        std::optional<Path> base = placeOrEval(ix->base.get());
        eval(ix->index.get(), Use::Read);
        if (!base) return std::nullopt;
        if (asPtr(base->type())) {   // indexing a raw pointer inside trusted: not tracked
            readPath(*base, ix->loc);
            return std::nullopt;
        }
        return base->index();   // all elements share one path (T3 keeps own pointers out of arrays)
    }
    return std::nullopt;
}

std::optional<Path> BorrowChecker::placeOrEval(Expr *e) {
    if (isPlaceExpr(e)) return evalPlace(e);
    eval(e, Use::Read);
    return std::nullopt;
}

// Reads the pointer expression `ptr` and returns the path of what it
// points to.
//
// The pointer itself has to be usable (M1/M2). We don't check its loans
// here: whatever access happens at the end of the path overlaps them
// anyway. Checking here as well would report the same mistake twice, and
// would wrongly reject borrows of different fields (`&mut p->a` and then
// `p->b = 1`).
std::optional<Path> BorrowChecker::derefOf(Expr *ptr) {
    if (!isPlaceExpr(ptr)) {   // *f(), new T->x, ...: the pointer is an rvalue
        eval(ptr, Use::Read);
        return std::nullopt;
    }
    std::optional<Path> p = evalPlace(ptr);
    if (!p) return std::nullopt;
    if (isOwnPtr(p->type())) checkUsable(*p, ptr->loc, Action::Use);
    if (isRawPtr(p->type())) {   // raw pointers aren't tracked, so there's no access after this
        checkLoans(*p, Access::Read, ptr->loc);
        return std::nullopt;
    }
    return p->deref();
}

Value BorrowChecker::applyUse(const Path &p, Use use, SourceLoc loc, const VarDecl *holder) {
    const Type *t = p.type();
    switch (use) {
    case Use::Read:
        readPath(p, loc);
        return {};
    case Use::Move:
        if (isOwnPtr(t)) return movePath(p, loc);
        readPath(p, loc);   // not an owner, so it's just a copy
        return {};
    case Use::LoanShared:
    case Use::LoanMut:
        // a pointer stored into a borrow: take a loan on what it points to
        readPath(p, loc);
        if (asPtr(t) && !isRawPtr(t))
            borrowPath(p.deref(), use == Use::LoanMut ? LoanKind::Mut : LoanKind::Shared, holder, loc);
        return {};
    }
    return {};
}

// ===================================================================
// Reading, moving, borrowing and writing paths
// ===================================================================

void BorrowChecker::readPath(const Path &p, SourceLoc loc) {
    if (isOwnPtr(p.type())) checkUsable(p, loc, Action::Use);
    checkLoans(p, Access::Read, loc);
}

Value BorrowChecker::movePath(const Path &p, SourceLoc loc) {
    Value v;
    if (behindShared(p)) {   // report it, but leave the state alone
        report(loc, "cannot move out of " + quote(p) + ", which is behind a shared borrow");
        v.st = defaultState(p.type());
        return v;
    }
    const bool ok = checkUsable(p, loc, Action::Move);
    checkLoans(p, Access::Move, loc);

    if (ok) {   // M6: whoever receives p would get a dangling field
        for (const auto &kv : cur_.own) {
            if (!p.isStrictPrefixOf(kv.first) || !(kv.second.bits & Moved)) continue;
            report(loc, "cannot move " + quote(p) + " while " + quote(kv.first) + " is moved out",
                   {{kv.second.movedAt, quote(kv.first) + (kv.second.movedByFree ? " freed" : " moved") + " here"}});
            break;
        }
    }

    v.st = get(cur_, p);
    if (v.st.bits & (Moved | Uninit)) {   // already reported, so don't pass the error on to the receiver
        v.st.bits &= static_cast<uint8_t>(~(Moved | Uninit));
        if (!v.st.bits) v.st.bits = Owned;
    }
    v.st.movedAt = {};
    v.st.movedByFree = false;
    v.st.prevIter = false;
    for (const auto &kv : cur_.own)
        if (p.isStrictPrefixOf(kv.first))
            v.sub.push_back({std::vector<Proj>(kv.first.projs.begin() + p.projs.size(), kv.first.projs.end()),
                             kv.second});

    OwnState m;
    m.bits = Moved;
    m.movedAt = loc;
    cur_.own[p] = m;
    eraseUnder(cur_, p, false);
    return v;
}

void BorrowChecker::borrowPath(const Path &p, LoanKind k, const VarDecl *holder, SourceLoc loc) {
    if (isOwnPtr(p.type())) checkUsable(p, loc, Action::Borrow);

    // The borrow could reach an owner inside p that is moved out or not set yet.
    for (const auto &kv : cur_.own) {
        const OwnState &s = kv.second;
        if (!p.isStrictPrefixOf(kv.first) || !(s.bits & (Moved | Uninit))) continue;
        Notes notes;
        if (s.bits & Moved) notes.push_back({s.movedAt, quote(kv.first) + (s.movedByFree ? " freed" : " moved") + " here"});
        report(loc, "cannot borrow " + quote(p) + " while " + quote(kv.first) + " is " +
                        ((s.bits & Moved) ? "moved out" : "uninitialized"),
               notes);
        break;
    }

    checkLoans(p, k == LoanKind::Mut ? Access::BorrowMut : Access::BorrowShared, loc);

    // Through a mut borrow, the own paths inside p can be swapped out (as
    // long as they're put back, M5), so after this all we know about them
    // is their type's default.
    if (k == LoanKind::Mut) eraseUnder(cur_, p, true);

    Loan l{p, k, holder, loc};
    if (std::find(cur_.loans.begin(), cur_.loans.end(), l) == cur_.loans.end()) cur_.loans.push_back(l);

    // Borrowing through another borrow r (a loan on *r...) reaches whatever
    // r points to. So the new holder also takes copies of r's loans. That
    // way the target stays borrowed even if r is pointed somewhere else
    // (which kills the loans through *r) or goes out of scope first.
    const VarDecl *r = p.root;
    if (holder && r != holder && !p.projs.empty() && p.projs[0].kind == ProjKind::Deref &&
        isBorrowPtr(r->type)) {
        std::vector<Loan> inherited;
        for (const Loan &rl : cur_.loans)
            if (rl.holder == r)
                inherited.push_back({rl.path, (k == LoanKind::Mut && rl.kind == LoanKind::Mut) ? LoanKind::Mut : LoanKind::Shared,
                                     holder, rl.loc});
        for (const Loan &il : inherited)
            if (std::find(cur_.loans.begin(), cur_.loans.end(), il) == cur_.loans.end()) cur_.loans.push_back(il);
    }
}

void BorrowChecker::writePath(const Path &p, const Value &v, SourceLoc loc) {
    const Type *t = p.type();
    const bool borrow = isBorrowPtr(t);
    checkLoans(p, Access::Write, loc, borrow);   // B2, shallow when p is a borrow

    if (isOwnPtr(t)) {
        const OwnState old = get(cur_, p);
        if (old.bits & Owned)   // M3
            report(loc, "assignment to " + quote(p) + ((old.bits & ~Owned) ? " may leak" : " leaks") +
                            " the value it owns",
                   {{loc, "move or free " + quote(p) + " first"}});
        eraseUnder(cur_, p, true);
        cur_.own[p] = v.st.bits ? v.st : defaultState(t);
        install(cur_, p, v);
    } else if (borrow) {
        // Pointing a borrow somewhere else (D4). Anything moved out through
        // it has to be back first, and loans through its old target die.
        checkRestore(p, loc);
        eraseUnder(cur_, p, false);
        const Path through = p.deref();
        auto &ls = cur_.loans;
        ls.erase(std::remove_if(ls.begin(), ls.end(), [&](const Loan &l) { return through.isPrefixOf(l.path); }),
                 ls.end());
    } else if (containsOwn(t)) {
        // assigning a whole struct: any own field it overwrites that still owns something is M3
        std::vector<Path> fields;
        ownFieldPaths(t, p, fields);
        for (const Path &q : fields)
            if (get(cur_, q).bits & Owned)
                report(loc, "assignment to " + quote(p) + " leaks the value owned by " + quote(q),
                       {{loc, "move or free " + quote(q) + " first"}});
        eraseUnder(cur_, p, false);
        install(cur_, p, v);
    }
}

bool BorrowChecker::checkUsable(const Path &p, SourceLoc loc, Action a) {
    const OwnState s = get(cur_, p);
    const std::string name = quote(p);

    if (s.bits & Moved) {   // M1
        const char *verb = s.movedByFree ? "freed" : "moved";
        Notes notes{{s.movedAt, name + " " + verb + " here"}};
        std::string msg;
        if (s.prevIter) {
            msg = name + " was " + verb + " in a previous iteration of the loop";
        } else if (s.bits != Moved) {
            msg = name + " may have been " + verb;
        } else if (s.movedByFree && a == Action::Free) {
            msg = "double free of " + name;
            notes = {{s.movedAt, "first freed here"}};
        } else if (s.movedByFree) {
            msg = "use of " + name + " after free";
        } else {
            msg = "use of moved pointer " + name;
        }
        report(loc, msg, notes);
        return false;
    }
    if (s.bits & Uninit) {   // M2
        report(loc, s.bits == Uninit ? "use of uninitialized owner " + name : name + " may be uninitialized",
               {{p.root->loc, "'" + p.root->name + "' declared here"}});
        return false;
    }
    return true;
}

bool BorrowChecker::checkLoans(const Path &p, Access acc, SourceLoc loc, bool shallow) {
    for (const Loan &l : cur_.loans) {
        if (!l.path.overlaps(p)) continue;
        const bool mut = l.kind == LoanKind::Mut;
        if ((acc == Access::Read || acc == Access::BorrowShared) && !mut) continue;
        // overwriting a borrow doesn't touch what it used to point to
        if (shallow && acc == Access::Write && p.deref().isPrefixOf(l.path)) continue;

        const std::string name = quote(p);
        std::string msg;
        switch (acc) {
        case Access::Read:         msg = "cannot read " + name + " while it is mutably borrowed"; break;
        case Access::Write:        msg = "cannot assign to " + name + " while it is borrowed"; break;
        case Access::Move:         msg = "cannot move " + name + " while it is borrowed"; break;
        case Access::Free:         msg = "cannot free " + name + " while it is borrowed"; break;
        case Access::BorrowShared: msg = "cannot borrow " + name + " because it is mutably borrowed"; break;
        case Access::BorrowMut:
            msg = mut ? "cannot borrow " + name + " as mutable more than once at a time"
                      : "cannot borrow " + name + " as mutable because it is also borrowed";
            break;
        }
        Notes notes{{l.loc, quote(l.path) + " was borrowed here"}};
        if (l.holder)
            notes.push_back({l.holder->loc, "borrow held by '" + l.holder->name + "' until the end of its block"});
        report(loc, msg, notes);
        return false;
    }
    return true;
}

// An owning rvalue (new, a call, a compound literal) whose value just gets
// thrown away, like `new int;`, `make()->v` or `f(new int)` when f takes a borrow.
void BorrowChecker::discardedOwner(const Expr *e, const Type *t) {
    report(e->loc, "owned value of type '" + typeToString(t) + "' is leaked: it is never freed or stored");
}

// ===================================================================
// Flow state
// ===================================================================

OwnState BorrowChecker::get(const FlowState &s, const Path &p) {
    auto it = s.own.find(p);
    return it != s.own.end() ? it->second : defaultState(p.type());
}

void BorrowChecker::eraseUnder(FlowState &s, const Path &p, bool inclusive) {
    for (auto it = s.own.begin(); it != s.own.end();) {
        const bool under = inclusive ? p.isPrefixOf(it->first) : p.isStrictPrefixOf(it->first);
        it = under ? s.own.erase(it) : std::next(it);
    }
}

void BorrowChecker::markUninit(FlowState &s, const VarDecl *vd) {
    OwnState u;
    u.bits = Uninit;
    const Path p{vd, {}};
    if (isOwnPtr(vd->type)) {
        s.own[p] = u;
        return;
    }
    std::vector<Path> fields;
    ownFieldPaths(vd->type, p, fields);
    for (const Path &q : fields) s.own[q] = u;
}

void BorrowChecker::install(FlowState &s, const Path &base, const Value &v) {
    for (const auto &rs : v.sub) {
        Path q = base;
        q.projs.insert(q.projs.end(), rs.first.begin(), rs.first.end());
        s.own[q] = rs.second;
    }
}

// An unreachable state adds nothing. Otherwise take the union of both.
FlowState BorrowChecker::join(const FlowState &a, const FlowState &b) {
    if (!a.reachable) return b;
    if (!b.reachable) return a;
    FlowState out;
    for (const auto &kv : a.own) out.own[kv.first] = merge(kv.second, get(b, kv.first));
    for (const auto &kv : b.own)
        if (!a.own.count(kv.first)) out.own[kv.first] = merge(get(a, kv.first), kv.second);
    out.loans = a.loans;
    for (const Loan &l : b.loans)
        if (std::find(out.loans.begin(), out.loans.end(), l) == out.loans.end()) out.loans.push_back(l);
    return out;
}

bool BorrowChecker::sameState(const FlowState &a, const FlowState &b) {
    if (a.reachable != b.reachable) return false;
    if (!a.reachable) return true;
    for (const auto &kv : a.own)
        if (get(b, kv.first).bits != kv.second.bits) return false;
    for (const auto &kv : b.own)
        if (get(a, kv.first).bits != kv.second.bits) return false;
    if (a.loans.size() != b.loans.size()) return false;
    for (const Loan &l : a.loans)
        if (std::find(b.loans.begin(), b.loans.end(), l) == b.loans.end()) return false;
    return true;
}

// Narrows s down to the runs where cond comes out as `truth`.
void BorrowChecker::refine(Expr *cond, bool truth, FlowState &s) {
    if (!cond || !s.reachable) return;
    while (auto *c = dyn_cast<ImplicitCastExpr>(cond)) cond = c->sub.get();   // ToBool and friends

    // constant conditions, like while (1) or if (false)
    if (auto *il = dyn_cast<IntLiteral>(cond)) {
        if ((il->value != 0) != truth) s.reachable = false;
        return;
    }
    if (auto *bl = dyn_cast<BoolLiteral>(cond)) {
        if (bl->value != truth) s.reachable = false;
        return;
    }
    if (auto *cl = dyn_cast<CharLiteral>(cond)) {
        if ((cl->value != 0) != truth) s.reachable = false;
        return;
    }

    if (auto *u = dyn_cast<UnaryExpr>(cond)) {
        if (u->op == UnaryOp::Not) {
            refine(u->operand.get(), !truth, s);
            return;
        }
    }

    if (auto *b = dyn_cast<BinaryExpr>(cond)) {
        if (b->isLogical()) {
            const bool isAnd = b->op == BinaryOp::LogAnd;
            if (truth == isAnd) {   // a && b is true, or a || b is false: both sides hold
                refine(b->lhs.get(), truth, s);
                refine(b->rhs.get(), truth, s);
            } else {                // either the left side decided it, or the right side did
                FlowState byLhs = s;
                refine(b->lhs.get(), truth, byLhs);
                refine(b->lhs.get(), !truth, s);
                refine(b->rhs.get(), truth, s);
                s = join(byLhs, s);
            }
            return;
        }
        if (b->op == BinaryOp::Eq || b->op == BinaryOp::Ne) {
            Expr *other = isNullExpr(b->rhs.get()) ? b->lhs.get()
                        : isNullExpr(b->lhs.get()) ? b->rhs.get()
                                                   : nullptr;
            if (!other) return;
            if (auto p = pathOf(other))
                if (isOwnPtr(p->type())) refineNull(*p, (b->op == BinaryOp::Ne) == truth, s);
        }
        return;
    }

    // a pointer used directly as the condition: if (p)
    if (isPlaceExpr(cond))
        if (auto p = pathOf(cond))
            if (isOwnPtr(p->type())) refineNull(*p, truth, s);
}

void BorrowChecker::refineNull(const Path &p, bool nonNull, FlowState &s) {
    OwnState st = get(s, p);
    if (st.bits & (Moved | Uninit)) return;   // already an error
    if (nonNull) {
        st.bits &= static_cast<uint8_t>(~Null);
        if (!st.bits) {   // it's always null, so this branch never runs
            s.reachable = false;
            return;
        }
    } else {
        if (!(st.bits & Null)) {   // it's never null
            s.reachable = false;
            return;
        }
        st.bits = Null;
        eraseUnder(s, p, false);
    }
    s.own[p] = st;
}

// ===================================================================
// Errors
// ===================================================================

void BorrowChecker::report(SourceLoc loc, const std::string &msg, const Notes &notes) {
    if (silent_ > 0) return;
    if (!reported_.insert({loc.line, loc.col, msg}).second) return;   // seen it already (e.g. one defer, several exits)
    diag_.error(loc, msg);
    for (const auto &n : notes)
        if (n.first.isValid()) diag_.note(n.first, n.second);
    if (!deferCtx_.empty()) {
        static const char *const via[] = {"at its end", "by 'return'", "by 'break'", "by 'continue'"};
        const DeferCtx &d = deferCtx_.back();
        diag_.note(d.deferLoc, "in code deferred here");
        diag_.note(d.exitLoc, std::string("which runs when the scope is exited ") + via[static_cast<int>(d.kind)]);
    }
}

} // namespace safec
