#include "codegen.h"
#include <stdexcept>
#include <cassert>

namespace safec {

// -----------------------------------------------------------------------------
// Type Lowering & Erasure
// -----------------------------------------------------------------------------
std::string CodeGen::llvm_type(const Type* t) const {
    if (!t) return "void";
    if (isa<PointerType>(t)) return "ptr";
    
    if (auto* at = dyn_cast<ArrayType>(t)) {
        std::string elem_t = llvm_type(at->element());
        uint64_t sz = at->size().value_or(0);
        return "[" + std::to_string(sz) + " x " + elem_t + "]";
    }
    
    if (auto* bt = dyn_cast<BuiltinType>(t)) {
        switch (bt->builtinKind()) {
            case BuiltinKind::Void:  return "void";
            case BuiltinKind::Bool:  return "i1";
            case BuiltinKind::Char:  return "i8";
            case BuiltinKind::Int:   return "i32";
            case BuiltinKind::Float: return "float";
            case BuiltinKind::Null:  return "ptr";
        }
    }
    
    if (auto* st = dyn_cast<StructType>(t)) {
        return "%struct." + st->decl()->name;
    }
    
    if (isa<FunctionType>(t)) return "ptr";
    
    return "void";
}

std::string CodeGen::llvm_zero_init(const Type* t) const {
    if (isa<PointerType>(t)) return "null";
    if (isa<ArrayType>(t) || isa<StructType>(t)) return "zeroinitializer";
    if (auto* bt = dyn_cast<BuiltinType>(t)) {
        if (bt->builtinKind() == BuiltinKind::Float) return "0.0";
    }
    return "0";
}

int CodeGen::get_struct_field_index(const std::string& struct_name, const std::string& field_name) const {
    auto it = struct_defs_.find(struct_name);
    if (it == struct_defs_.end()) throw std::runtime_error("Struct not found: " + struct_name);
    
    const StructDecl* sd = it->second;
    for (size_t i = 0; i < sd->fields.size(); ++i) {
        if (sd->fields[i]->name == field_name) return i;
    }
    throw std::runtime_error("Field " + field_name + " not found in " + struct_name);
}

// -----------------------------------------------------------------------------
// Module Header Emission
// -----------------------------------------------------------------------------
void CodeGen::emit_module_header() {
    // 1. Emit Struct Layouts
    for (const auto& d : prog_->decls) {
        if (auto* sd = dyn_cast<StructDecl>(d.get())) {
            struct_defs_[sd->name] = sd;
            std::string layout = "%struct." + sd->name + " = type { ";
            for (size_t i = 0; i < sd->fields.size(); ++i) {
                layout += llvm_type(sd->fields[i]->type);
                if (i + 1 < sd->fields.size()) layout += ", ";
            }
            layout += " }";
            out_ << layout << "\n";
        }
    }
    out_ << "\n";

    // 2. Emit Runtime Declarations
    out_ << "declare ptr @malloc(i64)\n";
    out_ << "declare void @free(ptr)\n";
    out_ << "declare void @llvm.trap()\n";
    out_ << "declare i32 @printf(ptr, ...)\n\n";

    // 3. Emit Globals
    for (const auto& d : prog_->decls) {
        if (auto* vd = dyn_cast<VarDecl>(d.get())) {
            if (vd->isGlobal) {
                std::string t = llvm_type(vd->type);
                std::string init = llvm_zero_init(vd->type);
                out_ << "@" << vd->name << " = global " << t << " " << init << "\n";
            }
        }
    }
    out_ << "\n";
}

// -----------------------------------------------------------------------------
// LValue Address Calculation (Returns 'ptr')
// -----------------------------------------------------------------------------
std::string CodeGen::gen_lvalue(const Expr* e) {
    if (auto* v = dyn_cast<NameExpr>(e)) {
        return lookup_var(v->name).llvm_ptr;
    }
    
    if (auto* u = dyn_cast<UnaryExpr>(e)) {
        if (u->op == UnaryOp::Deref) {
            return gen_expr(u->operand.get());
        }
    }
    
    if (auto* m = dyn_cast<MemberExpr>(e)) {
        std::string base_ptr;
        std::string struct_name;
        
        if (m->isArrow) {
            base_ptr = gen_expr(m->base.get());
            auto* pt = cast<PointerType>(m->base->type);
            struct_name = cast<StructType>(pt->pointee())->decl()->name;
        } else {
            base_ptr = gen_lvalue(m->base.get());
            struct_name = cast<StructType>(m->base->type)->decl()->name;
        }
        
        int fidx = m->field ? m->field->index : get_struct_field_index(struct_name, m->member);
        std::string gep = new_temp();
        emit(gep + " = getelementptr inbounds %struct." + struct_name + ", ptr " + base_ptr + ", i32 0, i32 " + std::to_string(fidx));
        return gep;
    }
    
    if (auto* idx = dyn_cast<IndexExpr>(e)) {
        std::string idx_val = gen_expr(idx->index.get());
        
        if (auto* at = dyn_cast<ArrayType>(idx->base->type)) {
            std::string base_ptr = gen_lvalue(idx->base.get());
            uint64_t array_size = at->size().value_or(0);
            std::string elem_t = llvm_type(at->element());
            
            if (trusted_depth_ == 0) {
                std::string oob = new_temp();
                emit(oob + " = icmp uge i32 " + idx_val + ", " + std::to_string(array_size));
                std::string ok_lbl = new_label("bounds.ok");
                std::string fail_lbl = new_label("bounds.fail");
                
                emit_terminator("br i1 " + oob + ", label %" + fail_lbl + ", label %" + ok_lbl);
                emit_label(fail_lbl);
                emit("call void @llvm.trap()");
                emit_terminator("unreachable");
                emit_label(ok_lbl);
            }
            
            std::string gep = new_temp();
            emit(gep + " = getelementptr inbounds [" + std::to_string(array_size) + " x " + elem_t + "], ptr " + base_ptr + ", i32 0, i32 " + idx_val);
            return gep;
        } else {
            std::string base_ptr = gen_expr(idx->base.get());
            std::string elem_t = llvm_type(idx->type);
            std::string gep = new_temp();
            emit(gep + " = getelementptr inbounds " + elem_t + ", ptr " + base_ptr + ", i32 " + idx_val);
            return gep;
        }
    }
    
    throw std::runtime_error("Expression is not an LValue");
}

// -----------------------------------------------------------------------------
// Expression Evaluation (Returns SSA value)
// -----------------------------------------------------------------------------
std::string CodeGen::gen_expr(const Expr* e) {
    if (auto* il = dyn_cast<IntLiteral>(e)) return std::to_string(il->value);
    if (auto* bl = dyn_cast<BoolLiteral>(e)) return bl->value ? "1" : "0";
    if (auto* cl = dyn_cast<CharLiteral>(e)) return std::to_string(static_cast<int>(cl->value));
    if (dyn_cast<NullLiteral>(e)) return "null";
    
    if (auto* sl = dyn_cast<StringLiteral>(e)) {
        std::string global_name = "@.str." + std::to_string(string_pool_.size());
        string_pool_.push_back({global_name, sl->value, sl->value.length() + 1});
        return global_name;
    }

    if (dyn_cast<NameExpr>(e) || dyn_cast<MemberExpr>(e) || dyn_cast<IndexExpr>(e) || 
       (isa<UnaryExpr>(e) && cast<UnaryExpr>(e)->op == UnaryOp::Deref)) {
        
        std::string lval = gen_lvalue(e);
        if (isa<ArrayType>(e->type)) {
            std::string arr_t = llvm_type(e->type);
            std::string ptr = new_temp();
            emit(ptr + " = getelementptr inbounds " + arr_t + ", ptr " + lval + ", i32 0, i32 0");
            return ptr;
        }
        
        std::string val = new_temp();
        emit(val + " = load " + llvm_type(e->type) + ", ptr " + lval);
        return val;
    }

    if (auto* u = dyn_cast<UnaryExpr>(e)) {
        if (u->op == UnaryOp::AddrOf || u->op == UnaryOp::AddrOfMut) {
            return gen_lvalue(u->operand.get());
        }
        std::string op_val = gen_expr(u->operand.get());
        std::string res = new_temp();
        if (u->op == UnaryOp::Minus) emit(res + " = sub nsw i32 0, " + op_val);
        else if (u->op == UnaryOp::Not) emit(res + " = xor i1 " + op_val + ", true");
        else if (u->op == UnaryOp::BitNot) emit(res + " = xor i32 " + op_val + ", -1");
        return res;
    }

    if (auto* b = dyn_cast<BinaryExpr>(e)) {
        if (b->op == BinaryOp::LogAnd) return gen_short_circuit_and(b);
        if (b->op == BinaryOp::LogOr) return gen_short_circuit_or(b);
        
        std::string lhs = gen_expr(b->lhs.get());
        std::string rhs = gen_expr(b->rhs.get());
        std::string res = new_temp();
        std::string t = llvm_type(b->lhs->type);
        
        switch (b->op) {
            case BinaryOp::Add: emit(res + " = add nsw " + t + " " + lhs + ", " + rhs); break;
            case BinaryOp::Sub: emit(res + " = sub nsw " + t + " " + lhs + ", " + rhs); break;
            case BinaryOp::Mul: emit(res + " = mul nsw " + t + " " + lhs + ", " + rhs); break;
            case BinaryOp::Div: emit(res + " = sdiv " + t + " " + lhs + ", " + rhs); break;
            case BinaryOp::Eq:  emit(res + " = icmp eq " + t + " " + lhs + ", " + rhs); break;
            case BinaryOp::Ne:  emit(res + " = icmp ne " + t + " " + lhs + ", " + rhs); break;
            case BinaryOp::Lt:  emit(res + " = icmp slt " + t + " " + lhs + ", " + rhs); break;
            case BinaryOp::Gt:  emit(res + " = icmp sgt " + t + " " + lhs + ", " + rhs); break;
            default: break;
        }
        return res;
    }

    if (auto* a = dyn_cast<AssignExpr>(e)) {
        std::string lhs_ptr = gen_lvalue(a->target.get());
        std::string rhs_val = gen_expr(a->value.get());
        std::string t = llvm_type(a->value->type);
        emit("store " + t + " " + rhs_val + ", ptr " + lhs_ptr);
        return rhs_val;
    }

    if (auto* nw = dyn_cast<NewExpr>(e)) {
        std::string target_t = llvm_type(nw->allocated->resolved);
        std::string gep = new_temp();
        emit(gep + " = getelementptr " + target_t + ", ptr null, i32 1");
        std::string sz = new_temp();
        emit(sz + " = ptrtoint ptr " + gep + " to i64");
        
        std::string raw = new_temp();
        emit(raw + " = call ptr @malloc(i64 " + sz + ")");
        return raw;
    }

    if (auto* c = dyn_cast<CallExpr>(e)) {
        std::string ret_t = llvm_type(c->type);
        std::string fn_name;
        if (auto* ne = dyn_cast<NameExpr>(c->callee.get())) fn_name = ne->name;
        
        std::string call_str = "call " + ret_t + " @" + fn_name + "(";
        for (size_t i = 0; i < c->args.size(); ++i) {
            std::string arg_val = gen_expr(c->args[i].get());
            std::string arg_t = llvm_type(c->args[i]->type);
            call_str += arg_t + " " + arg_val;
            if (i + 1 < c->args.size()) call_str += ", ";
        }
        call_str += ")";
        
        if (ret_t == "void") {
            emit(call_str);
            return "";
        } else {
            std::string res = new_temp();
            emit(res + " = " + call_str);
            return res;
        }
    }

    // --- Compound Literals (Structs & Arrays) ---
    if (auto* cl = dyn_cast<CompoundLiteralExpr>(e)) {
        std::string current_val = "undef";
        std::string agg_t = llvm_type(cl->type);
        
        for (size_t i = 0; i < cl->inits.size(); ++i) {
            const auto& init = cl->inits[i];
            std::string field_val = gen_expr(init.value.get());
            std::string field_t = llvm_type(init.value->type);
            
            // Get the GEP index (either resolved by Sema, or positional)
            int fidx = init.resolved ? init.resolved->index : i;
            
            std::string next_val = new_temp();
            emit(next_val + " = insertvalue " + agg_t + " " + current_val + ", " + field_t + " " + field_val + ", " + std::to_string(fidx));
            current_val = next_val;
        }
        return current_val; // Returns the fully constructed struct/array SSA value
    }
    
    if (auto* ic = dyn_cast<ImplicitCastExpr>(e)) {
        return gen_expr(ic->sub.get());
    }

    return "";
}

// -----------------------------------------------------------------------------
// Short-Circuit Logical Operators
// -----------------------------------------------------------------------------
std::string CodeGen::gen_short_circuit_and(const BinaryExpr* b) {
    std::string lhs_val = gen_expr(b->lhs.get());
    std::string rhs_lbl = new_label("and.rhs");
    std::string end_lbl = new_label("and.end");
    std::string res_ptr = new_temp();
    
    emit(res_ptr + " = alloca i1");
    emit("store i1 " + lhs_val + ", ptr " + res_ptr);
    emit_terminator("br i1 " + lhs_val + ", label %" + rhs_lbl + ", label %" + end_lbl);
    
    emit_label(rhs_lbl);
    std::string rhs_val = gen_expr(b->rhs.get());
    emit("store i1 " + rhs_val + ", ptr " + res_ptr);
    emit_terminator("br label %" + end_lbl);
    
    emit_label(end_lbl);
    std::string final_val = new_temp();
    emit(final_val + " = load i1, ptr " + res_ptr);
    return final_val;
}

std::string CodeGen::gen_short_circuit_or(const BinaryExpr* b) {
    std::string lhs_val = gen_expr(b->lhs.get());
    std::string rhs_lbl = new_label("or.rhs");
    std::string end_lbl = new_label("or.end");
    std::string res_ptr = new_temp();
    
    emit(res_ptr + " = alloca i1");
    emit("store i1 " + lhs_val + ", ptr " + res_ptr);
    emit_terminator("br i1 " + lhs_val + ", label %" + end_lbl + ", label %" + rhs_lbl);
    
    emit_label(rhs_lbl);
    std::string rhs_val = gen_expr(b->rhs.get());
    emit("store i1 " + rhs_val + ", ptr " + res_ptr);
    emit_terminator("br label %" + end_lbl);
    
    emit_label(end_lbl);
    std::string final_val = new_temp();
    emit(final_val + " = load i1, ptr " + res_ptr);
    return final_val;
}

} // namespace safec