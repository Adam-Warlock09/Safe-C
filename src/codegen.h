// codegen.h
#pragma once
#include "new_ast.h"
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace safec {

struct CGVarInfo {
    std::string llvm_ptr; // e.g. "%x.addr" or "@g_var"
    const Type* type;     // CHANGED: Must be a pointer because Type is abstract
};

// How one parameter or the return value crosses a call under the C calling
// convention (x86-64 System V); see codegen_abi.cpp
struct AbiArg {
    enum Kind {
        Direct,     // as itself (scalars, array pointers), maybe with attr
        Coerce,     // a small struct, as 1 or 2 register-sized parts
        Indirect,   // a struct in memory: byval parameter or sret return
        Ignore,     // an empty struct: not passed at all
    } kind = Direct;
    std::string ir_type;              // Direct: the type; Coerce: "i64" or "{ i64, i32 }"
    std::vector<std::string> parts;   // Coerce: the type of each register-sized part
    std::string attr;                 // Direct: "signext" (char), "zeroext" (bool) or ""
    uint64_t align = 0;               // Indirect: alignment of the in-memory copy
};

struct AbiSignature {
    AbiArg ret;
    std::vector<AbiArg> params;
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
    // Rejects programs sema accepts but codegen can't build; run before generate
    static bool check_reserved_names(const Program& prog, DiagnosticsEngine& diag);
    // -g: also emit DWARF debug info for the program in source_path
    void enable_debug_info(const std::string& source_path);
    void generate(const Program& tu);

private:
    std::ostream& out_;
    std::ostringstream body_ir_;     // Buffers instructions for current function
    std::ostringstream temp_allocas_; // Entry-block allocas for temporaries (temp_alloca)
    const Program* prog_ = nullptr;
    const FunctionDecl* current_fn_ = nullptr;
    AbiSignature current_abi_;       // how the current function receives and returns values

    int temp_counter_ = 0;
    int label_counter_ = 0;
    int var_Rename_counter_ = 0;
    bool block_terminated_ = false;
    int trusted_depth_ = 0;

    // String literals pooled during expression codegen -> emitted at module top
    struct StringConst { std::string global_name; std::string raw_bytes; size_t byte_len; };
    std::vector<StringConst> string_pool_;
    std::set<std::string> used_intrinsics_;   // declare lines, emitted at module end

    // Struct name -> list of fields (for GEP field index lookup)
    std::unordered_map<std::string, const StructDecl*> struct_defs_;

    // Active lexical scopes inside current function
    std::vector<CGScopeFrame> scopes_;

    // Each local and parameter of the current function -> its alloca. Keyed by the
    // declaration sema resolved, so shadowed names and reused loop names stay apart
    std::unordered_map<const ValueDecl*, std::string> local_addr_;
    std::unordered_map<std::string, int> addr_uses_;   // alloca names already taken
    std::string current_block_;                        // label of the block being filled

    // --- Shared Emission Helpers ---
    std::string new_temp() { return "%t" + std::to_string(++temp_counter_); }
    std::string new_label(const std::string& pfx) { return pfx + "." + std::to_string(++label_counter_); }
    // With -g every instruction carries its source location (current_dbg_)
    void emit(const std::string& instr) {
        if (!block_terminated_) body_ir_ << "  " << instr << current_dbg_ << "\n";
    }
    void emit_label(const std::string& lbl) {
        body_ir_ << lbl << ":\n";
        block_terminated_ = false;
        current_block_ = lbl;
    }
    void emit_terminator(const std::string& instr) {
        if (!block_terminated_) {
            body_ir_ << "  " << instr << current_dbg_ << "\n";
            block_terminated_ = true;
        }
    }

    void emit_module_header();
    
    // CHANGED: Pass Type as pointer
    std::string llvm_type(const Type* t) const;
    std::string llvm_zero_init(const Type* t) const;
    
    int get_struct_field_index(const std::string& struct_name, const std::string& field_name) const;

    std::string gen_expr(const Expr* e);
    std::string gen_lvalue(const Expr* e);
    std::string gen_short_circuit_and(const BinaryExpr* b);
    std::string gen_short_circuit_or(const BinaryExpr* b);
    void emit_Store_compound_lit(const std::string& dest_ptr, const CompoundLiteralExpr* cl);

    std::string convert_int(const std::string& val, const std::string& from_t, const std::string& to_t);
    std::string convert_scalar(const std::string& val, const std::string& from_t, const std::string& to_t);
    std::string emit_binop(BinaryOp op, const std::string& t, const std::string& a, const std::string& b);
    void emit_trap_if(const std::string& cond, const std::string& what);
    std::string emit_checked_arith(BinaryOp op, const std::string& t, const std::string& a, const std::string& b);
    void emit_division_checks(const std::string& t, const std::string& a, const std::string& b);
    void emit_shift_check(const std::string& t, const std::string& n);
    std::pair<std::string, std::string> gen_update(const std::string& ptr, const Type* ty, BinaryOp op,
                                                   const std::string& rhs, const std::string& op_t);
    std::string float_const(double v) const;
    int64_t const_int(const Expr* e);
    double const_float(const Expr* e);
    std::string const_init(const Expr* e);
    std::string new_addr(const std::string& name);
    std::string temp_alloca(const Type* t);

    void gen_function(const FunctionDecl& fn);
    void emit_entry_allocas(const Stmt* s);
    void gen_stmt(const Stmt* s);
    void gen_compound_stmt(const CompoundStmt* cs, bool push_new_scope = true);
    void gen_branch_stmt(const Stmt* s);

    // --- Debug info for -g (codegen_debug.cpp) ---
    bool debug_ = false;
    std::string debug_file_name_, debug_dir_;
    std::vector<std::string> debug_nodes_;                  // !N = debug_nodes_[N]
    std::unordered_map<std::string, int> debug_node_ids_;   // shares identical nodes
    std::unordered_map<const Type*, int> debug_types_;
    std::vector<int> debug_globals_;
    std::set<const VarDecl*> debug_declared_;
    int debug_file_ = -1, debug_cu_ = -1;
    int current_subprogram_ = -1;
    std::vector<int> debug_scopes_;                         // lexical blocks, innermost last
    std::string current_dbg_;                               // ", !dbg !N" or empty
    int md(const std::string& body);
    int md_distinct(const std::string& body);
    void debug_begin_module();
    void debug_end_module();
    int debug_type(const Type* t);
    std::string debug_global(const VarDecl& vd);
    std::string debug_begin_function(const FunctionDecl& fn);
    void debug_end_function();
    int debug_scope() const;
    void debug_set_line(int line);
    void debug_push_block(int line);
    void debug_pop_block();
    void debug_declare_var(const VarDecl* vd, const std::string& addr, int arg_no, bool by_value = false);

    // --- C calling convention (codegen_abi.cpp) ---
    struct ScalarSlotInfo { uint64_t offset; uint64_t size; bool is_float; bool is_pointer; };
    void type_layout(const Type* t, uint64_t& size, uint64_t& align) const;
    void collect_struct_scalars(const Type* t, uint64_t offset, std::vector<ScalarSlotInfo>& out) const;
    AbiArg classify_scalar(const Type* t, bool is_return = false) const;
    std::string pointer_attrs(const PointerType* pt, bool is_return) const;
    AbiArg classify_small_struct(const Type* t, uint64_t size, int& need_int, int& need_sse) const;
    AbiSignature classify_signature(const FunctionType* ft) const;
    std::string abi_return_type(const AbiSignature& sig) const;
    std::string abi_param_list(const AbiSignature& sig, const FunctionType* ft,
                               const std::vector<std::string>* names) const;
    std::string abi_param_types(const AbiSignature& sig) const;
    void emit_declare(const FunctionDecl& fn);
    std::string promote_variadic_arg(const Type* t, const std::string& val);
    std::string temp_alloca_ir(const std::string& ir_type);
    std::vector<std::string> split_for_abi(const AbiArg& a, const Type* t, const std::string& val);
    std::string join_from_abi(const AbiArg& a, const std::vector<std::string>& pieces);
    void emit_return(const std::string& val);
    std::string gen_call(const CallExpr* c);

    void push_scope(bool is_loop_or_switch = false, std::string brk = "", std::string cont = "");
    void pop_scope_and_emit_defers();
    void emit_defers_for_return();
    void emit_defers_for_jump(bool is_break);
    CGVarInfo lookup_var(const std::string& name) const;
};

} // namespace safec