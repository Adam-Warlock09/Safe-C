#include "codegen.h"
#include <cstdio>

namespace safec {

// new and free are calloc and free from the C library. A global with one of
// the allocator's names would clash with those declarations, or replace the
// C library's function at link time (C reserves these names too)
bool CodeGen::check_reserved_names(const Program& prog, DiagnosticsEngine& diag) {
    static const char* const reserved[] = {"malloc", "calloc", "realloc", "free"};
    bool ok = true;
    for (const auto& d : prog.decls) {
        if (!isa<FunctionDecl>(d.get()) && !isa<VarDecl>(d.get())) continue;
        for (const char* name : reserved) {
            if (d->name == name) {
                diag.error(d->loc, "'" + d->name + "' is reserved for the C library's allocator, which new and free use; choose another name");
                ok = false;
            }
        }
    }
    return ok;
}

void CodeGen::generate(const Program& prog) {
    prog_ = &prog;

    emit_module_header();

    // Iterate over all declarations looking for Functions with bodies
    for (const auto& d : prog_->decls) {
        if (auto* fn = dyn_cast<FunctionDecl>(d.get())) {
            if (fn->body) gen_function(*fn);
        }
    }

    // Intrinsics the checked arithmetic used
    for (const std::string& decl : used_intrinsics_) out_ << decl << "\n";

    // Emit the accumulated string pool at the end of the module
    // Inside c"..." only printable characters can appear as themselves; everything
    // else, and " and \ themselves, is written as \XX (two hex digits)
    for (const auto& str : string_pool_) {
        out_ << str.global_name << " = private unnamed_addr constant ["
             << str.byte_len << " x i8] c\"";
        for (unsigned char c : str.raw_bytes) {
            if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
                out_ << c;
            } else {
                char hex[4];
                std::snprintf(hex, sizeof hex, "\\%02X", c);
                out_ << hex;
            }
        }
        out_ << "\\00\"\n";
    }
}

void CodeGen::gen_function(const FunctionDecl& fn) {
    current_fn_ = &fn;
    temp_counter_ = 0;
    label_counter_ = 0;
    block_terminated_ = false;
    body_ir_.str(""); 
    scopes_.clear();
    local_addr_.clear();
    addr_uses_.clear();

    const Type* ret_type = fn.type ? cast<FunctionType>(fn.type)->returnType() : nullptr;
    current_abi_ = classify_signature(fn.functionType());

    // Parameters arrive the way the C calling convention passes them
    std::vector<std::string> arg_names;
    for (const auto& p : fn.params) arg_names.push_back("%" + p->name + ".arg");
    out_ << "\ndefine " << abi_return_type(current_abi_) << " @" << fn.name << "("
         << abi_param_list(current_abi_, fn.functionType(), &arg_names) << ") {\n";

    emit_label("entry");
    push_scope();

    for (size_t i = 0; i < fn.params.size(); ++i) {
        const ParamDecl* p = fn.params[i].get();
        const AbiArg& a = current_abi_.params[i];
        std::string arg = arg_names[i];
        std::string addr;
        if (isa<ArrayType>(p->type) || a.kind == AbiArg::Indirect) {
            // already in memory: the array itself, or the callee's own byval copy
            addr = arg;
        } else if (a.kind == AbiArg::Coerce) {
            // the struct's register-sized parts, stored back together. The slot
            // has the coerced type, which is at least as big as the struct
            addr = new_addr(p->name);
            emit(addr + " = alloca " + a.ir_type + ", align 8");
            for (size_t j = 0; j < a.parts.size(); ++j) {
                std::string part_addr = addr;
                if (a.parts.size() > 1) {
                    part_addr = new_temp();
                    emit(part_addr + " = getelementptr inbounds " + a.ir_type + ", ptr " + addr + ", i32 0, i32 " + std::to_string(j));
                }
                emit("store " + a.parts[j] + " " + arg + "." + std::to_string(j) + ", ptr " + part_addr);
            }
        } else {
            addr = new_addr(p->name);
            emit(addr + " = alloca " + llvm_type(p->type));
            if (a.kind == AbiArg::Direct)            // an empty struct (Ignore) has nothing to store
                emit("store " + llvm_type(p->type) + " " + arg + ", ptr " + addr);
        }
        scopes_.back().vars[p->name] = {addr, p->type};
        local_addr_[p] = addr;
    }

    emit_entry_allocas(fn.body.get());

    // The entry block is written out last: allocas for temporaries the body
    // creates (temp_alloca) are added to it while the body is generated
    std::string entry_ir = body_ir_.str();
    body_ir_.str("");
    temp_allocas_.str("");

    emit_label("body");
    gen_compound_stmt(fn.body.get(), false); 
    pop_scope_and_emit_defers();    // falling off the end runs the function-level defers
    
    if (!block_terminated_) {      // falling off the end returns zero (and void returns nothing)
        emit_return(llvm_type(ret_type) == "void" ? "" : llvm_zero_init(ret_type));
    }

    out_ << entry_ir << temp_allocas_.str() << "  br label %body\n"
         << body_ir_.str() << "}\n";
}

// A stack slot of type t in the entry block, for a value that has no address
// of its own (e.g. a struct returned by a call). In the entry block it is
// allocated once per call, not once per loop iteration, and mem2reg can remove it
std::string CodeGen::temp_alloca(const Type* t) {
    // new_addr only makes %name.addr[.N], so %agg.tmp.N can't clash with a local
    std::string addr = "%agg.tmp." + std::to_string(++temp_counter_);
    temp_allocas_ << "  " << addr << " = alloca " << llvm_type(t) << "\n";
    return addr;
}

// A fresh alloca name for a local: %x.addr, then %x.addr.1, %x.addr.2 ... for
// later variables with the same name (shadowing, two loops that both declare i)
std::string CodeGen::new_addr(const std::string& name) {
    int n = addr_uses_[name]++;
    if (n == 0) return "%" + name + ".addr";
    return "%" + name + ".addr." + std::to_string(n);
}

void CodeGen::emit_entry_allocas(const Stmt* s) {
    if (!s) return;
    if (auto* cs = dyn_cast<CompoundStmt>(s)) {
        for (const auto& stmt : cs->body) emit_entry_allocas(stmt.get());
    } else if (auto* is = dyn_cast<IfStmt>(s)) {
        emit_entry_allocas(is->thenStmt.get());
        emit_entry_allocas(is->elseStmt.get());
    } else if (auto* ws = dyn_cast<WhileStmt>(s)) {
        emit_entry_allocas(ws->body.get());
    } else if (auto* fs = dyn_cast<ForStmt>(s)) {
        emit_entry_allocas(fs->init.get());
        emit_entry_allocas(fs->body.get());
    } else if (auto* ts = dyn_cast<TrustedStmt>(s)) {
        emit_entry_allocas(ts->body.get());
    } else if (auto* sw = dyn_cast<SwitchStmt>(s)) {
        for (const auto& c : sw->cases)
            for (const auto& stmt : c->body) emit_entry_allocas(stmt.get());
    } else if (auto* df = dyn_cast<DeferStmt>(s)) {
        emit_entry_allocas(df->body.get());
    } else if (auto* ds = dyn_cast<DeclStmt>(s)) {
        for (const auto& vd : ds->decls) {
            std::string addr = new_addr(vd->name);
            emit(addr + " = alloca " + llvm_type(vd->type));
            local_addr_[vd.get()] = addr;
        }
    }
}

void CodeGen::gen_compound_stmt(const CompoundStmt* cs, bool push_new_scope) {
    if (push_new_scope) push_scope();
    for (const auto& stmt : cs->body) {
        gen_stmt(stmt.get());
    }
    if (push_new_scope) pop_scope_and_emit_defers();
}

void CodeGen::gen_stmt(const Stmt* s) {
    if (block_terminated_ || !s) return;
    
    if (auto* expr_stmt = dyn_cast<ExprStmt>(s)) {
        // Intercept FreeExpr since it is technically an expression in the AST
        if (auto* free_expr = dyn_cast<FreeExpr>(expr_stmt->expr.get())) {
            std::string ptr = gen_expr(free_expr->operand.get());
            emit("call void @free(ptr " + ptr + ")");
        } else {
            gen_expr(expr_stmt->expr.get());
        }
    } else if (auto* cs = dyn_cast<CompoundStmt>(s)) {
        gen_compound_stmt(cs);
    } else if (auto* ds = dyn_cast<DeferStmt>(s)) {
        scopes_.back().defers.push_back(ds->body.get());
    } else if (auto* decls = dyn_cast<DeclStmt>(s)) {
        for (const auto& vd : decls->decls) {
            std::string addr = local_addr_.at(vd.get());
            scopes_.back().vars[vd->name] = {addr, vd->type};
            if (vd->init) {
                std::string rhs = gen_expr(vd->init.get());
                emit("store " + llvm_type(vd->type) + " " + rhs + ", ptr " + addr);
            }
        }
    } else if (auto* rs = dyn_cast<ReturnStmt>(s)) {
        std::string ret_val = "";
        if (rs->value) ret_val = gen_expr(rs->value.get()); 
        
        emit_defers_for_return();
        emit_return(ret_val);
    } else if (auto* is = dyn_cast<IfStmt>(s)) {
        std::string cond = gen_expr(is->cond.get());
        std::string lbl_then = new_label("if.then");
        std::string lbl_else = new_label("if.else");
        std::string lbl_end = new_label("if.end");
        
        if (is->elseStmt) {
            emit("br i1 " + cond + ", label %" + lbl_then + ", label %" + lbl_else);
        } else {
            emit("br i1 " + cond + ", label %" + lbl_then + ", label %" + lbl_end);
        }
        
        emit_label(lbl_then);
        gen_branch_stmt(is->thenStmt.get());
        emit("br label %" + lbl_end);

        if (is->elseStmt) {
            emit_label(lbl_else);
            gen_branch_stmt(is->elseStmt.get());
            emit("br label %" + lbl_end);
        }
        emit_label(lbl_end);
    } else if (auto* ws = dyn_cast<WhileStmt>(s)) {
        std::string lbl_cond = new_label("while.cond");
        std::string lbl_body = new_label("while.body");
        std::string lbl_end = new_label("while.end");
        
        emit("br label %" + lbl_cond);
        emit_label(lbl_cond);
        std::string cond = gen_expr(ws->cond.get());
        emit("br i1 " + cond + ", label %" + lbl_body + ", label %" + lbl_end);
        
        emit_label(lbl_body);
        push_scope(true, lbl_end, lbl_cond);
        gen_stmt(ws->body.get());
        pop_scope_and_emit_defers();
        emit("br label %" + lbl_cond);
        
        emit_label(lbl_end);
    } else if (isa<BreakStmt>(s)) {
        emit_defers_for_jump(true);
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->is_loop_or_switch && !it->break_label.empty()) {
                emit_terminator("br label %" + it->break_label);
                break;
            }
        }
    } else if (isa<ContinueStmt>(s)) {
        emit_defers_for_jump(false);
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (!it->continue_label.empty()) {
                emit_terminator("br label %" + it->continue_label);
                break;
            }
        }
    } else if (auto* ts = dyn_cast<TrustedStmt>(s)) {
        trusted_depth_++;
        gen_compound_stmt(ts->body.get(), true);
        trusted_depth_--;
    } else if (auto* fs = dyn_cast<ForStmt>(s)) {
        push_scope(); 
        if (fs->init) gen_stmt(fs->init.get());

        std::string lbl_cond = new_label("for.cond");
        std::string lbl_body = new_label("for.body");
        std::string lbl_inc = new_label("for.inc");
        std::string lbl_end = new_label("for.end");

        emit("br label %" + lbl_cond);
        emit_label(lbl_cond);
        if (fs->cond) {
            std::string cond_val = gen_expr(fs->cond.get());
            emit("br i1 " + cond_val + ", label %" + lbl_body + ", label %" + lbl_end);
        } else emit("br label %" + lbl_body);

        emit_label(lbl_body);
        push_scope(true, lbl_end, lbl_inc); 
        
        if (fs->body) gen_stmt(fs->body.get());
        pop_scope_and_emit_defers(); 
        emit("br label %" + lbl_inc);

        emit_label(lbl_inc);
        if (fs->step) gen_expr(fs->step.get()); // Evaluate the step (increment)
        emit("br label %" + lbl_cond);

        emit_label(lbl_end);
        pop_scope_and_emit_defers(); 
    } else if (auto* sw = dyn_cast<SwitchStmt>(s)) {
        // Compare as int: sema doesn't convert case values to the condition's type,
        // so a char switch can have int cases (and sema checks them as int values)
        std::string cond_type = "i32";
        std::string cond_val = convert_int(gen_expr(sw->cond.get()), llvm_type(sw->cond->type), cond_type);
        std::string lbl_end = new_label("sw.end");
        
        std::string default_lbl = lbl_end; 
        std::vector<std::pair<std::string, std::string>> case_vals_and_labels;
        std::vector<std::string> case_labels(sw->cases.size());
        
        // Pass 1: Scan for Case/Default statements and allocate labels
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            auto* switch_case = sw->cases[i].get();
            if (auto* case_st = dyn_cast<CaseStmt>(switch_case)) {
                case_labels[i] = new_label("sw.case");
                std::string case_val = std::to_string(const_int(case_st->value.get()));
                case_vals_and_labels.push_back({case_val, case_labels[i]});
            } else if (dyn_cast<DefaultStmt>(switch_case)) {
                case_labels[i] = new_label("sw.default");
                default_lbl = case_labels[i];
            }
        }
        
        // Emit the LLVM switch terminator instruction
        emit("switch " + cond_type + " " + cond_val + ", label %" + default_lbl + " [");
        for (const auto& c : case_vals_and_labels) {
            body_ir_ << "    " << cond_type << " " << c.first << ", label %" << c.second << "\n";
        }
        body_ir_ << "  ]\n";
        block_terminated_ = true;
        
        // Pass 2: Emit the bodies sequentially (supporting C-style fallthrough)
        push_scope(true, lbl_end, ""); 
        
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            if (!block_terminated_) emit("br label %" + case_labels[i]);
            emit_label(case_labels[i]);
            
            for (const auto& stmt : sw->cases[i]->body) {
                gen_stmt(stmt.get());
            }
        }
        
        if (!block_terminated_) emit("br label %" + lbl_end);
        
        pop_scope_and_emit_defers();
        emit_label(lbl_end);
    }
}

// The then/else branch of an if. A branch without braces still gets its own
// scope, so `if (c) defer f();` only runs f when the branch was taken (in the
// enclosing block's scope it would run on every path)
void CodeGen::gen_branch_stmt(const Stmt* s) {
    if (!s || isa<CompoundStmt>(s)) {   // a block already opens a scope
        gen_stmt(s);
        return;
    }
    push_scope();
    gen_stmt(s);
    pop_scope_and_emit_defers();
}

// --- Defer Engine Helpers ---

void CodeGen::push_scope(bool is_loop_or_switch, std::string brk, std::string cont) {
    scopes_.push_back({{}, {}, is_loop_or_switch, brk, cont});
}

void CodeGen::pop_scope_and_emit_defers() {
    if (scopes_.empty()) return;
    auto frame = scopes_.back();
    scopes_.pop_back();
    
    if (!block_terminated_) {
        for (auto it = frame.defers.rbegin(); it != frame.defers.rend(); ++it) {
            gen_stmt(*it);
        }
    }
}

// Both collect the defers first and run them after: running a defer body like
// `defer { ... }` pushes a scope, which can move scopes_ in memory and leave a
// loop over it reading freed memory
void CodeGen::emit_defers_for_return() {
    std::vector<const Stmt*> to_run;
    for (auto scope_it = scopes_.rbegin(); scope_it != scopes_.rend(); ++scope_it) {
        for (auto defer_it = scope_it->defers.rbegin(); defer_it != scope_it->defers.rend(); ++defer_it) {
            to_run.push_back(*defer_it);
        }
    }
    for (const Stmt* d : to_run) gen_stmt(d);
}

void CodeGen::emit_defers_for_jump(bool is_break) {
    std::vector<const Stmt*> to_run;
    for (auto scope_it = scopes_.rbegin(); scope_it != scopes_.rend(); ++scope_it) {
        for (auto defer_it = scope_it->defers.rbegin(); defer_it != scope_it->defers.rend(); ++defer_it) {
            to_run.push_back(*defer_it);
        }
        if (is_break && scope_it->is_loop_or_switch) break;
        if (!is_break && !scope_it->continue_label.empty()) break;
    }
    for (const Stmt* d : to_run) gen_stmt(d);
}

CGVarInfo CodeGen::lookup_var(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->vars.count(name)) return it->vars.at(name);
    }
    return {"@" + name, nullptr}; // Global fallback
}

} // namespace safec