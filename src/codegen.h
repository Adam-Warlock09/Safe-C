#pragma once
#include "new_ast.h"
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct CGVarInfo {
    std::string llvm_ptr; // e.g. "%x.addr" or "@g_var"
    Type type;
};

struct CGScopeFrame {
    std::unordered_map<std::string, CGVarInfo> vars;
    std::vector<const Stmt*> defers; // Registered DeferStmt bodies in this '{}' block
    bool is_loop_or_switch = false;
    std::string break_label;         // Non-empty if loop or switch
    std::string continue_label;      // Non-empty if loop
};

class CodeGen {
public:
    explicit CodeGen(std::ostream& out) : out_(out) {}
    void generate(const TranslationUnit& tu);

private:
    std::ostream& out_;
    std::ostringstream body_ir_;     // Buffers instructions for current function
    const TranslationUnit* tu_ = nullptr;
    const FunctionDecl* current_fn_ = nullptr;

    int temp_counter_ = 0;
    int label_counter_ = 0;
    int var_Rename_counter_ = 0;
    bool block_terminated_ = false;
    int trusted_depth_ = 0;

    // String literals pooled during expression codegen -> emitted at module top
    struct StringConst { std::string global_name; std::string raw_bytes; size_t byte_len; };
    std::vector<StringConst> string_pool_;

    // Struct name -> list of fields (for GEP field index lookup)
    std::unordered_map<std::string, const StructDecl*> struct_defs_;

    // Active lexical scopes inside current function
    std::vector<CGScopeFrame> scopes_;

    // --- Shared Emission Helpers ---
    std::string new_temp() { return "%t" + std::to_string(++temp_counter_); }
    std::string new_label(const std::string& pfx) { return pfx + "." + std::to_string(++label_counter_); }
    void emit(const std::string& instr) {
        if (!block_terminated_) body_ir_ << "  " << instr << "\n";
    }
    void emit_label(const std::string& lbl) {
        body_ir_ << lbl << ":\n";
        block_terminated_ = false;
    }
    void emit_terminator(const std::string& instr) {
        if (!block_terminated_) {
            body_ir_ << "  " << instr << "\n";
            block_terminated_ = true;
        }
    }

    // --- Person A (src/codegen_expr.cpp) ---
    void emit_module_header();
    std::string llvm_type(const Type& t) const;
    std::string llvm_sizeof_expr(const Type& t);
    std::string llvm_zero_init(const Type& t) const;
    int get_struct_field_index(const std::string& struct_name, const std::string& field_name) const;

    std::string gen_expr(const Expr* e);
    std::string gen_lvalue(const Expr* e);
    std::string gen_short_circuit_and(const BinaryExpr* b);
    std::string gen_short_circuit_or(const BinaryExpr* b);
    void emit_Store_compound_lit(const std::string& dest_ptr, const CompoundLitExpr* cl);
    std::string to_bool_i1(const std::string& val, const Type& t);

    // --- Person B (src/codegen_stmt.cpp) ---
    void gen_function(const FunctionDecl& fn);
    void emit_entry_allocas(const Stmt* s);
    void gen_stmt(const Stmt* s);
    void gen_compound_stmt(const CompoundStmt* cs, bool push_new_scope = true);

    void push_scope(bool is_loop_or_switch = false, std::string brk = "", std::string cont = "");
    void pop_scope_and_emit_defers();
    void emit_defers_for_return();
    void emit_defers_for_jump(bool is_break);
    CGVarInfo lookup_var(const std::string& name) const;
};