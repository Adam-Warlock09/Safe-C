#include "codegen.h"

// --- Person B Implementation ---

void CodeGen::generate(const TranslationUnit& tu) {
    tu_ = &tu;
    
    // Map struct definitions for Person A's GEP lookups
    for (const auto& s : tu_->structs) {
        struct_defs_[s->name] = s.get();
    }

    emit_module_header(); // Implemented by Person A

    for (const auto& fn : tu_->functions) {
        if (fn->body) {
            gen_function(*fn.get());
        }
    }
}

void CodeGen::gen_function(const FunctionDecl& fn) {
    current_fn_ = &fn;
    temp_counter_ = 0;
    label_counter_ = 0;
    block_terminated_ = false;
    body_ir_.str(""); // Clear function body buffer
    scopes_.clear();

    // 1. Function signature
    out_ << "\ndefine " << llvm_type(fn.return_type) << " @" << fn.name << "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        out_ << llvm_type(fn.params[i].type) << " %" << fn.params[i].name;
        if (i + 1 < fn.params.size()) out_ << ", ";
    }
    out_ << ") {\n";

    // 2. Entry block: Setup Alloca for params and locals
    emit_label("entry");
    push_scope(); // Root function scope
    
    for (const auto& p : fn.params) {
        std::string addr = "%" + p.name + ".addr";
        emit(addr + " = alloca " + llvm_type(p.type));
        emit("store " + llvm_type(p.type) + " %" + p.name + ", ptr " + addr);
        scopes_.back().vars[p.name] = {addr, p.type};
    }

    emit_entry_allocas(fn.body.get());
    emit("  br label %body\n");

    // 3. Body execution
    emit_label("body");
    if (auto cs = dynamic_cast<const CompoundStmt*>(fn.body.get())) {
        gen_compound_stmt(cs, false); // false = don't push new scope, use root scope
    }
    
    // 4. Fallback terminator for blocks missing a return
    if (!block_terminated_) {
        // Safe fallback until Sema Issue C2 is fixed
        emit_terminator("ret " + llvm_type(fn.return_type) + " 0"); 
    }

    // 5. Write buffer to output
    out_ << body_ir_.str() << "}\n";
    pop_scope_and_emit_defers(); // Pop root scope
}

void CodeGen::emit_entry_allocas(const Stmt* s) {
    if (!s) return;
    if (auto cs = dynamic_cast<const CompoundStmt*>(s)) {
        for (const auto& stmt : cs->statements) emit_entry_allocas(stmt.get());
    } else if (auto is = dynamic_cast<const IfStmt*>(s)) {
        emit_entry_allocas(is->then_branch.get());
        emit_entry_allocas(is->else_branch.get());
    } else if (auto ws = dynamic_cast<const WhileStmt*>(s)) {
        emit_entry_allocas(ws->body.get());
    } else if (auto fs = dynamic_cast<const ForStmt*>(s)) {
        emit_entry_allocas(fs->init.get());
        emit_entry_allocas(fs->body.get());
    } else if (auto ts = dynamic_cast<const TrustedStmt*>(s)) {
        emit_entry_allocas(ts->body.get());
    } else if (auto vd = dynamic_cast<const VarDeclStmt*>(s)) {
        std::string addr = "%" + vd->name + ".addr";
        emit(addr + " = alloca " + llvm_type(vd->type));
    }
}

void CodeGen::gen_compound_stmt(const CompoundStmt* cs, bool push_new_scope) {
    if (push_new_scope) push_scope();
    for (const auto& stmt : cs->statements) {
        gen_stmt(stmt.get());
    }
    if (push_new_scope) pop_scope_and_emit_defers();
}

void CodeGen::gen_stmt(const Stmt* s) {
    if (block_terminated_ || !s) return;
    
    if (auto expr = dynamic_cast<const Expr*>(s)) {
        gen_expr(expr);
    } else if (auto cs = dynamic_cast<const CompoundStmt*>(s)) {
        gen_compound_stmt(cs);
    } else if (auto ds = dynamic_cast<const DeferStmt*>(s)) {
        // Register the defer in the current scope. Do NOT execute it yet!
        scopes_.back().defers.push_back(ds->body.get());
    } else if (auto vd = dynamic_cast<const VarDeclStmt*>(s)) {
        std::string addr = "%" + vd->name + ".addr";
        scopes_.back().vars[vd->name] = {addr, vd->type};
        if (vd->init) {
            std::string rhs = gen_expr(vd->init.get());
            emit("store " + llvm_type(vd->type) + " " + rhs + ", ptr " + addr);
        }
    } else if (auto rs = dynamic_cast<const ReturnStmt*>(s)) {
        std::string ret_val = "";
        if (rs->value) ret_val = gen_expr(rs->value.get()); // Eval FIRST
        
        emit_defers_for_return(); // Execute all registered defers
        
        if (rs->value) {
            emit_terminator("ret " + llvm_type(current_fn_->return_type) + " " + ret_val);
        } else {
            emit_terminator("ret void");
        }
    } else if (auto free = dynamic_cast<const FreeStmt*>(s)) {
        std::string ptr = gen_expr(free->target.get());
        emit("call void @free(ptr " + ptr + ")");
    } else if (auto is = dynamic_cast<const IfStmt*>(s)) {
        std::string cond = gen_expr(is->cond.get());
        std::string lbl_then = new_label("if.then");
        std::string lbl_else = new_label("if.else");
        std::string lbl_end = new_label("if.end");
        
        if (is->else_branch) {
            emit("br i1 " + cond + ", label %" + lbl_then + ", label %" + lbl_else);
        } else {
            emit("br i1 " + cond + ", label %" + lbl_then + ", label %" + lbl_end);
        }
        
        emit_label(lbl_then);
        gen_stmt(is->then_branch.get());
        emit("br label %" + lbl_end);
        
        if (is->else_branch) {
            emit_label(lbl_else);
            gen_stmt(is->else_branch.get());
            emit("br label %" + lbl_end);
        }
        emit_label(lbl_end);
    } else if (auto ws = dynamic_cast<const WhileStmt*>(s)) {
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
    } else if (auto bs = dynamic_cast<const BreakStmt*>(s)) {
        emit_defers_for_jump(true);
        // Find the active break label
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->is_loop_or_switch && !it->break_label.empty()) {
                emit_terminator("br label %" + it->break_label);
                break;
            }
        }
    } else if (auto cs = dynamic_cast<const ContinueStmt*>(s)) {
        emit_defers_for_jump(false);
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (!it->continue_label.empty()) {
                emit_terminator("br label %" + it->continue_label);
                break;
            }
        }
    } else if (auto ts = dynamic_cast<const TrustedStmt*>(s)) {
        trusted_depth_++;
        gen_compound_stmt(dynamic_cast<const CompoundStmt*>(ts->body.get()), true);
        trusted_depth_--;
    } else if (auto fs = dynamic_cast<const ForStmt*>(s)) {
        // 1. Outer scope: Handles the init statement (e.g., 'int i = 0;')
        push_scope(); 
        if (fs->init) {
            gen_stmt(fs->init.get());
        }

        std::string lbl_cond = new_label("for.cond");
        std::string lbl_body = new_label("for.body");
        std::string lbl_inc = new_label("for.inc");
        std::string lbl_end = new_label("for.end");

        emit("br label %" + lbl_cond);
        
        // 2. Condition Block
        emit_label(lbl_cond);
        if (fs->cond) {
            std::string cond_val = gen_expr(fs->cond.get());
            emit("br i1 " + cond_val + ", label %" + lbl_body + ", label %" + lbl_end);
        } else {
            emit("br label %" + lbl_body); // Empty condition is true
        }

        // 3. Body Block (Inner Scope)
        emit_label(lbl_body);
        push_scope(true, lbl_end, lbl_inc); // break -> for.end, continue -> for.inc
        
        if (fs->body) {
            gen_stmt(fs->body.get());
        }
        
        pop_scope_and_emit_defers(); // Flushes 'defer' statements inside the loop body
        emit("br label %" + lbl_inc);

        // 4. Increment Block
        emit_label(lbl_inc);
        if (fs->inc) {
            gen_stmt(fs->inc.get());
        }
        emit("br label %" + lbl_cond);

        // 5. End Block
        emit_label(lbl_end);
        pop_scope_and_emit_defers(); // Pops outer scope (cleans up init if needed)
} else if (auto sw = dynamic_cast<const SwitchStmt*>(s)) {
        std::string cond_val = gen_expr(sw->cond.get());
        std::string cond_type = llvm_type(sw->cond->inferred_type); // usually "i32"
        std::string lbl_end = new_label("sw.end");
        
        auto cs = dynamic_cast<const CompoundStmt*>(sw->body.get());
        if (!cs) return; // Malformed AST

        std::string default_lbl = lbl_end; // If no default exists, jump to end
        std::vector<std::pair<std::string, std::string>> case_vals_and_labels;
        std::vector<std::string> stmt_labels(cs->statements.size());
        
        // Pass 1: Scan for Case/Default statements and allocate labels
        for (size_t i = 0; i < cs->statements.size(); ++i) {
            auto inner_s = cs->statements[i].get();
            if (auto case_st = dynamic_cast<const CaseStmt*>(inner_s)) {
                stmt_labels[i] = new_label("sw.case");
                std::string case_val = gen_expr(case_st->value.get()); // Evaluates to constant string
                case_vals_and_labels.push_back({case_val, stmt_labels[i]});
            } else if (auto def_st = dynamic_cast<const DefaultStmt*>(inner_s)) {
                stmt_labels[i] = new_label("sw.default");
                default_lbl = stmt_labels[i];
            } else {
                stmt_labels[i] = new_label("sw.stmt"); // Regular statement label
            }
        }
        
        // Emit the LLVM switch terminator instruction
        emit("switch " + cond_type + " " + cond_val + ", label %" + default_lbl + " [");
        for (const auto& c : case_vals_and_labels) {
            // Note: raw output to buffer to avoid internal 'emit' indenting rules
            body_ir_ << "    " << cond_type << " " << c.first << ", label %" << c.second << "\n";
        }
        body_ir_ << "  ]\n";
        block_terminated_ = true; // The switch instruction terminates the current block
        
        // Pass 2: Emit the bodies sequentially (supporting C-style fallthrough)
        push_scope(true, lbl_end, ""); // Scope for break; statements
        
        for (size_t i = 0; i < cs->statements.size(); ++i) {
            auto inner_s = cs->statements[i].get();
            
            // If the previous block didn't hit a break/return, we fall through!
            if (!block_terminated_) {
                emit("br label %" + stmt_labels[i]);
            }
            emit_label(stmt_labels[i]);
            
            if (auto case_st = dynamic_cast<const CaseStmt*>(inner_s)) {
                gen_stmt(case_st->body.get());
            } else if (auto def_st = dynamic_cast<const DefaultStmt*>(inner_s)) {
                gen_stmt(def_st->body.get());
            } else {
                gen_stmt(inner_s);
            }
        }
        
        // Fallthrough from the very last case
        if (!block_terminated_) {
            emit("br label %" + lbl_end);
        }
        
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
        // Emit defers in current frame
        for (auto defer_it = scope_it->defers.rbegin(); defer_it != scope_it->defers.rend(); ++defer_it) {
            gen_stmt(*defer_it);
        }
        // Stop walking once we reach the loop/switch boundary we're jumping out of
        if (is_break && scope_it->is_loop_or_switch) break;
        if (!is_break && !scope_it->continue_label.empty()) break;
    }
}

CGVarInfo CodeGen::lookup_var(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->vars.count(name)) return it->vars.at(name);
    }
    // Assumes global if not found (Person A will handle actual globals)
    return {"@" + name, Type(BaseType::Int)};
}