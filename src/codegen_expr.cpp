#include "codegen.h"
#include <stdexcept>
#include <cassert>

// -----------------------------------------------------------------------------
// Type Lowering & Erasure
// -----------------------------------------------------------------------------
std::string CodeGen::llvm_type(const Type& t) const {
    if (t.is_pointer()) return "ptr"; // Pointer erasure (own, mut, raw, ? -> ptr)
    if (t.is_array()) {
        std::string elem_t = llvm_type(t.pointee()); // Base element type
        return "[" + std::to_string(t.array_sizes[0]) + " x " + elem_t + "]";
    }
    
    switch (t.base) {
        case BaseType::Void:  return "void";
        case BaseType::Bool:  return "i1";
        case BaseType::Char:  return "i8";
        case BaseType::Int:   return "i32";
        case BaseType::Float: return "float";
        case BaseType::Struct: return "%struct." + t.struct_name;
        default: throw std::runtime_error("Unknown BaseType in llvm_type");
    }
}

std::string CodeGen::llvm_zero_init(const Type& t) const {
    if (t.is_pointer()) return "null";
    if (t.is_array() || t.base == BaseType::Struct) return "zeroinitializer";
    if (t.base == BaseType::Float) return "0.0";
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
    for (const auto& sd : tu_->structs) {
        struct_defs_[sd->name] = sd.get();
        std::string layout = "%struct." + sd->name + " = type { ";
        for (size_t i = 0; i < sd->fields.size(); ++i) {
            layout += llvm_type(sd->fields[i]->type);
            if (i + 1 < sd->fields.size()) layout += ", ";
        }
        layout += " }";
        out_ << layout << "\n";
    }
    out_ << "\n";

    // 2. Emit Runtime Declarations
    out_ << "declare ptr @malloc(i64)\n";
    out_ << "declare void @free(ptr)\n";
    out_ << "declare void @llvm.trap()\n";
    out_ << "declare i32 @printf(ptr, ...)\n\n";

    // 3. Emit Globals
    for (const auto& gd : tu_->globals) {
        std::string t = llvm_type(gd->type);
        std::string init = llvm_zero_init(gd->type); // Or evaluate gd->init if constant
        out_ << "@" << gd->name << " = global " << t << " " << init << "\n";
    }
    out_ << "\n";
}

// -----------------------------------------------------------------------------
// LValue Address Calculation (Returns 'ptr')
// -----------------------------------------------------------------------------
std::string CodeGen::gen_lvalue(const Expr* e) {
    if (auto* v = dynamic_cast<const VarExpr*>(e)) {
        return lookup_var(v->name).llvm_ptr;
    }
    
    if (auto* d = dynamic_cast<const DerefExpr*>(e)) {
        return gen_expr(d->operand.get()); // Evaluates to the pointer itself
    }
    
    if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
        std::string base_ptr;
        std::string struct_name;
        
        if (m->is_arrow) {
            base_ptr = gen_expr(m->base.get());
            struct_name = m->base->inferred_type.pointee().struct_name;
        } else {
            base_ptr = gen_lvalue(m->base.get());
            struct_name = m->base->inferred_type.struct_name;
        }
        
        int fidx = get_struct_field_index(struct_name, m->member);
        std::string gep = new_temp();
        emit(gep + " = getelementptr inbounds %struct." + struct_name + ", ptr " + base_ptr + ", i32 0, i32 " + std::to_string(fidx));
        return gep;
    }
    
    if (auto* idx = dynamic_cast<const IndexExpr*>(e)) {
        std::string idx_val = gen_expr(idx->index.get());
        
        if (idx->base->inferred_type.is_array()) {
            std::string base_ptr = gen_lvalue(idx->base.get());
            int array_size = idx->base->inferred_type.array_sizes[0];
            std::string elem_t = llvm_type(idx->base->inferred_type.pointee());
            
            // Runtime Bounds Check (if not in trusted block)
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
            // Pointer indexing (raw pointers in trusted blocks)
            std::string base_ptr = gen_expr(idx->base.get());
            std::string elem_t = llvm_type(idx->inferred_type);
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
    // --- Literals ---
    if (auto* il = dynamic_cast<const IntLitExpr*>(e)) return std::to_string(il->value);
    if (auto* bl = dynamic_cast<const BoolLitExpr*>(e)) return bl->value ? "1" : "0";
    if (auto* cl = dynamic_cast<const CharLitExpr*>(e)) return std::to_string(static_cast<int>(cl->value));
    if (dynamic_cast<const NullLitExpr*>(e)) return "null";
    
    if (auto* sl = dynamic_cast<const StringLitExpr*>(e)) {
        std::string global_name = "@.str." + std::to_string(string_pool_.size());
        string_pool_.push_back({global_name, sl->value, sl->value.length() + 1}); // +1 for null terminator
        return global_name;
    }

    // --- LValue Reads (Variables, Derefs, Struct Fields, Array Elements) ---
    if (dynamic_cast<const VarExpr*>(e) || dynamic_cast<const DerefExpr*>(e) || 
        dynamic_cast<const MemberExpr*>(e) || dynamic_cast<const IndexExpr*>(e)) {
        
        std::string lval = gen_lvalue(e);
        if (e->inferred_type.is_array()) {
            // Arrays decay to pointers when used as values
            std::string arr_t = llvm_type(e->inferred_type);
            std::string ptr = new_temp();
            emit(ptr + " = getelementptr inbounds " + arr_t + ", ptr " + lval + ", i32 0, i32 0");
            return ptr;
        }
        
        std::string val = new_temp();
        emit(val + " = load " + llvm_type(e->inferred_type) + ", ptr " + lval);
        return val;
    }

    // --- Unary Operators ---
    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        if (u->op == "&" || u->op == "&mut") {
            return gen_lvalue(u->operand.get()); // &x simply yields its LLVM address
        }
        std::string op_val = gen_expr(u->operand.get());
        std::string res = new_temp();
        if (u->op == "-") emit(res + " = sub nsw i32 0, " + op_val);
        else if (u->op == "!") emit(res + " = xor i1 " + op_val + ", true");
        else if (u->op == "~") emit(res + " = xor i32 " + op_val + ", -1");
        return res;
    }

    // --- Binary Operators ---
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (b->op == "&&") return gen_short_circuit_and(b);
        if (b->op == "||") return gen_short_circuit_or(b);
        
        std::string lhs = gen_expr(b->lhs.get());
        std::string rhs = gen_expr(b->rhs.get());
        std::string res = new_temp();
        std::string t = llvm_type(b->lhs->inferred_type);
        
        // Arithmetic
        if (b->op == "+") emit(res + " = add nsw " + t + " " + lhs + ", " + rhs);
        else if (b->op == "-") emit(res + " = sub nsw " + t + " " + lhs + ", " + rhs);
        else if (b->op == "*") emit(res + " = mul nsw " + t + " " + lhs + ", " + rhs);
        else if (b->op == "/") emit(res + " = sdiv " + t + " " + lhs + ", " + rhs);
        // Comparisons (Int/Ptr)
        else if (b->op == "==") emit(res + " = icmp eq " + t + " " + lhs + ", " + rhs);
        else if (b->op == "!=") emit(res + " = icmp ne " + t + " " + lhs + ", " + rhs);
        else if (b->op == "<")  emit(res + " = icmp slt " + t + " " + lhs + ", " + rhs);
        else if (b->op == ">")  emit(res + " = icmp sgt " + t + " " + lhs + ", " + rhs);
        
        return res;
    }

    // --- Assignment ---
    if (auto* a = dynamic_cast<const AssignExpr*>(e)) {
        std::string lhs_ptr = gen_lvalue(a->lhs.get());
        std::string rhs_val = gen_expr(a->rhs.get());
        std::string t = llvm_type(a->rhs->inferred_type);
        emit("store " + t + " " + rhs_val + ", ptr " + lhs_ptr);
        return rhs_val; // Assignment returns the assigned value
    }

    // --- Allocation (new T) ---
    if (auto* nw = dynamic_cast<const NewExpr*>(e)) {
        std::string target_t = llvm_type(nw->target_type);
        
        // Target-independent sizeof via GEP trick
        std::string gep = new_temp();
        emit(gep + " = getelementptr " + target_t + ", ptr null, i32 1");
        std::string sz = new_temp();
        emit(sz + " = ptrtoint ptr " + gep + " to i64");
        
        std::string raw = new_temp();
        emit(raw + " = call ptr @malloc(i64 " + sz + ")");
        return raw;
    }

    // --- Function Call ---
    if (auto* c = dynamic_cast<const CallExpr*>(e)) {
        std::string ret_t = llvm_type(c->inferred_type);
        std::string call_str = "call " + ret_t + " @" + c->function_name + "(";
        
        for (size_t i = 0; i < c->args.size(); ++i) {
            std::string arg_val = gen_expr(c->args[i].get());
            std::string arg_t = llvm_type(c->args[i]->inferred_type);
            
            // C ABI: Variadic float args promote to double (useful for printf)
            if (arg_t == "float" && c->is_variadic_call && i >= c->fixed_param_count) {
                std::string promoted = new_temp();
                emit(promoted + " = fpext float " + arg_val + " to double");
                arg_val = promoted;
                arg_t = "double";
            }
            
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

    throw std::runtime_error("Unsupported expression node in CodeGen");
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
