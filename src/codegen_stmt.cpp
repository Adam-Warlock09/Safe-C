#include "codegen.h"

namespace safec {

void CodeGen::generate(const Program& prog) {
    prog_ = &prog;
    
    emit_module_header();

    // Iterate over all declarations looking for Functions with bodies
    for (const auto& d : prog_->decls) {
        if (auto* fn = dyn_cast<FunctionDecl>(d.get())) {
            if (fn->body) gen_function(*fn);
        }
    }

    // Emit the accumulated string pool at the end of the module
    for (const auto& str : string_pool_) {
        out_ << str.global_name << " = private unnamed_addr constant [" 
             << str.byte_len << " x i8] c\"" << str.raw_bytes << "\\00\"\n";
    }
}

void CodeGen::gen_function(const FunctionDecl& fn) {
    current_fn_ = &fn;
    temp_counter_ = 0;
    label_counter_ = 0;
    block_terminated_ = false;
    body_ir_.str(""); 
    scopes_.clear();

    const Type* ret_type = fn.type ? cast<FunctionType>(fn.type)->returnType() : nullptr;
    out_ << "\ndefine " << llvm_type(ret_type) << " @" << fn.name << "(";
    
    for (size_t i = 0; i < fn.params.size(); ++i) {
        out_ << llvm_type(fn.params[i]->type) << " %" << fn.params[i]->name;
        if (i + 1 < fn.params.size()) out_ << ", ";
    }
    out_ << ") {\n";

    emit_label("entry");
    push_scope(); 
    
    for (const auto& p : fn.params) {
        std::string addr = "%" + p->name + ".addr";
        emit(addr + " = alloca " + llvm_type(p->type));
        emit("store " + llvm_type(p->type) + " %" + p->name + ", ptr " + addr);
        scopes_.back().vars[p->name] = {addr, p->type};
    }

    emit_entry_allocas(fn.body.get());
    emit("  br label %body\n");

    emit_label("body");
    gen_compound_stmt(fn.body.get(), false); 
    
    if (!block_terminated_) {
        std::string ret_t = llvm_type(ret_type);
        if (ret_t == "void") emit_terminator("ret void");
        else emit_terminator("ret " + ret_t + " 0"); 
    }

    out_ << body_ir_.str() << "}\n";
    pop_scope_and_emit_defers(); 
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
    } else if (auto* ds = dyn_cast<DeclStmt>(s)) {
        for (const auto& vd : ds->decls) {
            std::string addr = "%" + vd->name + ".addr";
            emit(addr + " = alloca " + llvm_type(vd->type));
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
            std::string addr = "%" + vd->name + ".addr";
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
        
        const Type* ret_type = current_fn_->type ? cast<FunctionType>(current_fn_->type)->returnType() : nullptr;
        if (rs->value) emit_terminator("ret " + llvm_type(ret_type) + " " + ret_val);
        else emit_terminator("ret void");
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
        gen_stmt(is->thenStmt.get());
        emit("br label %" + lbl_end);
        
        if (is->elseStmt) {
            emit_label(lbl_else);
            gen_stmt(is->elseStmt.get());
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
        std::string cond_val = gen_expr(sw->cond.get());
        std::string cond_type = llvm_type(sw->cond->type); 
        std::string lbl_end = new_label("sw.end");
        
        std::string default_lbl = lbl_end; 
        std::vector<std::pair<std::string, std::string>> case_vals_and_labels;
        std::vector<std::string> case_labels(sw->cases.size());
        
        // Pass 1: Scan for Case/Default statements and allocate labels
        for (size_t i = 0; i < sw->cases.size(); ++i) {
            auto* switch_case = sw->cases[i].get();
            if (auto* case_st = dyn_cast<CaseStmt>(switch_case)) {
                case_labels[i] = new_label("sw.case");
                std::string case_val = gen_expr(case_st->value.get());
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

void CodeGen::emit_defers_for_return() {
    for (auto scope_it = scopes_.rbegin(); scope_it != scopes_.rend(); ++scope_it) {
        for (auto defer_it = scope_it->defers.rbegin(); defer_it != scope_it->defers.rend(); ++defer_it) {
            gen_stmt(*defer_it);
        }
    }
}

void CodeGen::emit_defers_for_jump(bool is_break) {
    for (auto scope_it = scopes_.rbegin(); scope_it != scopes_.rend(); ++scope_it) {
        for (auto defer_it = scope_it->defers.rbegin(); defer_it != scope_it->defers.rend(); ++defer_it) {
            gen_stmt(*defer_it);
        }
        if (is_break && scope_it->is_loop_or_switch) break;
        if (!is_break && !scope_it->continue_label.empty()) break;
    }
}

CGVarInfo CodeGen::lookup_var(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->vars.count(name)) return it->vars.at(name);
    }
    return {"@" + name, nullptr}; // Global fallback
}

} // namespace safec