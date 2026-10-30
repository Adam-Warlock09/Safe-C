#ifndef SAFEC_SEMA_H
#define SAFEC_SEMA_H

#include "new_ast.h"
#include <string_view>
#include <unordered_map>
#include <vector>

namespace safec {

    class Scope {
        private:
            std::vector<std::unordered_map<std::string_view, VarDecl*>> frames;

        public :
            void push() { frames.emplace_back(); }
            void pop() { frames.pop_back(); }

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

            // Phase 1 : Type Coercion & Conversion Engine (Person B)
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
            bool isTrusted = false;

            // Phase 1 : Building Blocks
            const Type* resolveType(TypeSpec* spec);

            // Phase 2 : Structural Passes
            void collectGlobals(Program& prog);
            void resolveStructFields(Program& prog);
            void resolveSignatures(Program& prog);
            void checkBodies(Program& prog);

            void checkStmt(Stmt* stmt);
            void checkExpr(Expr* expr);
            
            // Phase 2: Expression Minimal Slice (Person B)
            void checkAssignment(AssignExpr* expr);
            void checkCall(CallExpr* expr, const FunctionType* fnType);
            void checkNew(NewExpr* expr, const Type* allocatedType, TypeContext& types);
            void checkFreeExpr(FreeExpr* expr, TypeContext& types);
            void checkUnary(UnaryExpr* expr, TypeContext& types);
            void checkMember(MemberExpr* expr);
            void checkBinary(BinaryExpr* expr, TypeContext& types);
            void checkIndex(IndexExpr* expr, TypeContext& types);
            void checkCompoundLiteral(CompoundLiteralExpr* expr);
            // Phase 4: Basic Flow-Sensitive Narrowing (Person B)
            void applyNarrowing(Expr* cond, bool isTrueBranch, TypeContext& types);
            void clearNarrowing(Expr* target);
            const Type* getNarrowedType(ValueDecl* decl, const Type* originalType);

        private:
            std::unordered_map<ValueDecl*, const Type*> narrowedVars;

            // Allow the local test function to access these private hooks
            friend void test_phase1_conversions();

            // Hooks for C
            void checkDeref(Expr* base, SourceLoc loc);
            void checkArrow(MemberExpr* e);
            void checkFree(FreeExpr* e);
            void handleMove(Expr* target, Expr* value, SourceLoc loc);
    };

    void test_phase1_conversions();

} // namespace safec

#endif // SAFEC_SEMA_H