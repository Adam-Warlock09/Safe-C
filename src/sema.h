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

            bool declare(VarDecl* d) {
                return frames.back().emplace(d->name, d).second;
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

        private:
            DiagnosticsEngine& diag;
            Scope scope;

            // Global Symbol Tables
            std::unordered_map<std::string_view, FunctionDecl*> funs;
            std::unordered_map<std::string_view, StructDecl*> structs;

            // Context State
            FunctionDecl* currFun = nullptr;
            bool isTrusted = false; // Tracks if traversal is inside a trusted block

            // Phase 1 : Building Blocks
            const Type* resolveType(TypeSpec* spec, Program& prog);

            // Phase 2 : Structural Passes
            void collectGlobals(Program& prog);
            void resolveStructFields(Program& prog);
            void resolveSignatures(Program& prog);
            void checkBodies(Program& prog);

            void checkStmt(Stmt* stmt);
            void checkExpr(Expr* expr);
            
            // Hooks for B (expressions)
            void checkAssignment(AssignExpr* expr);
            void checkCall(CallExpr* expr);
            // B will add binary/unary traversal signatures here

            // Hooks for C
            void checkDeref(Expr* base, SourceLoc loc);
            void checkArrow(MemberExpr* e);
            void checkFree(FreeExpr* e);
            void handleMove(Expr* target, Expr* value, SourceLoc loc); // Assignment moves


    };

} // namespace safec

#endif // SAFEC_SEMA_H