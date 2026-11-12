#ifndef SAFEC_SEMA_H
#define SAFEC_SEMA_H

#include "new_ast.h"
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace safec {

    // What a null check narrows: a local or parameter, optionally followed by
    // fields, so p, list->head and n.next can all be narrowed.
    struct NarrowKey {
        ValueDecl* root = nullptr;
        std::vector<const FieldDecl*> fields;   // empty for the variable itself

        bool operator<(const NarrowKey& o) const {
            return std::tie(root, fields) < std::tie(o.root, o.fields);
        }
    };

    struct InitState {
        std::set<NarrowKey> unassigned;
        bool reachable = true;
    };

    class Scope {
        
        private:
            std::vector<std::unordered_map<std::string_view, VarDecl*>> frames;

        public :
            void push() { frames.emplace_back(); }
            void pop() { frames.pop_back(); }

            bool declare(VarDecl* d) {
                return frames.back().emplace(d->name, d).second;
            }

            VarDecl* lookupCurrentFrame(std::string_view name) {
                if (frames.empty()) return nullptr;
                auto it = frames.back().find(name);
                return (it != frames.back().end()) ? it->second : nullptr;
            }

            VarDecl* lookup(std::string_view name) {
                for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
                    if (it->count(name)) {
                        return (*it)[name];
                    }
                }
                return nullptr;
            }

    };

    class Sema {

        public:
            explicit Sema(DiagnosticsEngine& diag) : diag(diag) {}
            void run(Program& prog);

            // Phase 1 : Type Coercion & Conversion Engine
            bool coerce(ExprPtr& expr, const Type* targetType);
            bool coerceToBool(ExprPtr& expr, const TypeContext& types);

        private:
            DiagnosticsEngine& diag;
            Scope scope;

            // Global Symbol Tables
            std::unordered_map<std::string_view, FunctionDecl*> funs;
            std::unordered_map<std::string_view, StructDecl*> structs;

            // Context State
            FunctionDecl* currFun = nullptr;
            bool isTrusted = false; // Tracks if traversal is inside a trusted block
            bool inCallee = false;  // Tracks if NameExpr is the direct callee of CallExpr
            int inDefer = 0;        // Tracks if traversal is inside a defer block
            std::vector<Stmt*> breakStack;
            std::vector<Stmt*> continueStack;

            // Phase 1 : Building Blocks
            const Type* resolveType(TypeSpec* spec, Program& prog, bool isParam = false);

            // Phase 2 : Structural Passes
            void collectGlobals(Program& prog);
            void resolveStructFields(Program& prog);
            void resolveSignatures(Program& prog);
            void checkBodies(Program& prog);

            void checkStmt(Stmt* stmt, Program& prog);
            void checkExpr(Expr* expr, Program& prog);
            void checkCondition(ExprPtr &cond, SourceLoc loc, Program &prog);

            // Phases 2-3 : Expression Checking
            void checkAssignment(AssignExpr* expr, TypeContext& types);
            void checkCall(CallExpr* expr, const FunctionType* fnType);
            void checkNew(NewExpr* expr, const Type* allocatedType, TypeContext& types);
            void checkFreeExpr(FreeExpr* expr, TypeContext& types);
            void checkUnary(UnaryExpr* expr, TypeContext& types);
            void checkMember(MemberExpr* expr);
            void checkBinary(BinaryExpr* expr, TypeContext& types);
            void checkIndex(IndexExpr* expr, TypeContext& types);
            void checkCompoundLiteral(CompoundLiteralExpr* expr);

            // Phase 4: Basic Flow-Sensitive Narrowing
            void applyNarrowing(Expr* cond, bool isTrueBranch, TypeContext& types);
            void clearNarrowing(Expr* target);
            const Type* getNarrowedType(ValueDecl* decl, const Type* originalType);
            void forgetFieldNarrowing();

            std::map<NarrowKey, const Type*> narrowedVars;


            //reading a variable before it's assigned
            InitState init;
            std::vector<std::vector<InitState>> breakInits;      // one list per loop/switch like breakStack
            std::vector<std::vector<InitState>> continueInits;   // one list per loop like continueStack
            void flowExpr(Expr* e);
            void flowPlace(Expr* target);
            void flowRead(const NarrowKey& key, SourceLoc loc);
            void flowWrite(const NarrowKey& key);
            void addUnassigned(const NarrowKey& key, const Type* t);

            // Allow the local test function to access these private hooks
            friend void test_phase1_conversions();

            // Hooks for C
            bool checkDeref(Expr* base, SourceLoc loc);
            void checkArrow(MemberExpr* e);
            void checkFree(FreeExpr* e);
            void handleMove(Expr* target, Expr* value, SourceLoc loc);
            
    };

    void test_phase1_conversions();

} // namespace safec

#endif // SAFEC_SEMA_H