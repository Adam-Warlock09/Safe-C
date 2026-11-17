// codegen_abi.cpp - how values cross a function call: the C calling
// convention of x86-64 Linux (System V), so SafeC functions can call and be
// called by C code (whitepaper §4.6, §7.2).
//
// LLVM leaves part of the C calling convention to the front end. Scalars are
// passed as themselves, but char and bool must be marked signext/zeroext. A
// struct is passed according to what it contains:
//   - more than 16 bytes, or not enough registers left: in memory, through a
//     hidden pointer (byval for parameters, sret for the return value);
//   - up to 16 bytes: split into 8-byte chunks, each passed as an integer
//     (if it holds any non-float) or as floats, in one or two registers.
// The rules and the coerced types follow what clang emits for the same C code.

#include "codegen.h"
#include <algorithm>
#include <stdexcept>

namespace safec {

namespace {

constexpr int kIntArgRegs = 6;   // rdi, rsi, rdx, rcx, r8, r9
constexpr int kSseArgRegs = 8;   // xmm0 - xmm7

} // namespace

// Size and alignment in bytes, as C lays the type out (LLVM's x86-64 layout
// of the same IR types agrees)
void CodeGen::type_layout(const Type* t, uint64_t& size, uint64_t& align) const {
    if (isa<PointerType>(t) || isa<FunctionType>(t)) { size = align = 8; return; }
    if (auto* bt = dyn_cast<BuiltinType>(t)) {
        switch (bt->builtinKind()) {
            case BuiltinKind::Bool:
            case BuiltinKind::Char:  size = align = 1; return;
            case BuiltinKind::Int:
            case BuiltinKind::Float: size = align = 4; return;
            case BuiltinKind::Null:  size = align = 8; return;
            case BuiltinKind::Void:  size = 0; align = 1; return;
        }
    }
    if (auto* at = dyn_cast<ArrayType>(t)) {
        uint64_t elem_size, elem_align;
        type_layout(at->element(), elem_size, elem_align);
        size = elem_size * at->size().value_or(0);
        align = elem_align;
        return;
    }
    if (auto* st = dyn_cast<StructType>(t)) {
        size = 0;
        align = 1;
        for (const auto& f : st->decl()->fields) {
            uint64_t fs, fa;
            type_layout(f->type, fs, fa);
            size = (size + fa - 1) / fa * fa;
            size += fs;
            align = std::max(align, fa);
        }
        size = (size + align - 1) / align * align;
        return;
    }
    throw std::runtime_error("type_layout: unknown type");
}

// Every scalar in t, with its offset from the start of the outermost struct
void CodeGen::collect_struct_scalars(const Type* t, uint64_t offset, std::vector<ScalarSlotInfo>& out) const {
    if (auto* st = dyn_cast<StructType>(t)) {
        uint64_t pos = 0;
        for (const auto& f : st->decl()->fields) {
            uint64_t fs, fa;
            type_layout(f->type, fs, fa);
            pos = (pos + fa - 1) / fa * fa;
            collect_struct_scalars(f->type, offset + pos, out);
            pos += fs;
        }
    } else if (auto* at = dyn_cast<ArrayType>(t)) {
        uint64_t es, ea;
        type_layout(at->element(), es, ea);
        for (uint64_t i = 0; i < at->size().value_or(0); ++i)
            collect_struct_scalars(at->element(), offset + i * es, out);
    } else {
        auto* bt = dyn_cast<BuiltinType>(t);
        bool is_float = bt && bt->builtinKind() == BuiltinKind::Float;
        uint64_t size, align;
        type_layout(t, size, align);
        out.push_back({offset, size, is_float, isa<PointerType>(t)});
    }
}

// How a struct of at most 16 bytes travels in registers: one coerced IR type
// per 8-byte chunk. Counts the integer and SSE registers it needs
AbiArg CodeGen::classify_small_struct(const Type* t, uint64_t size, int& need_int, int& need_sse) const {
    std::vector<ScalarSlotInfo> scalars;
    collect_struct_scalars(t, 0, scalars);

    AbiArg arg;
    arg.kind = AbiArg::Coerce;
    need_int = need_sse = 0;
    for (uint64_t chunk = 0; chunk * 8 < size; ++chunk) {
        int floats = 0, others = 0;
        const ScalarSlotInfo* first = nullptr;       // the scalar at the chunk's start
        for (const auto& s : scalars) {
            if (s.offset / 8 != chunk) continue;
            if (s.is_float) ++floats; else ++others;
            if (s.offset == chunk * 8) first = &s;
        }
        if (floats > 0 && others == 0) {             // SSE: floats only
            arg.parts.push_back(floats == 1 ? "float" : "<2 x float>");
            ++need_sse;
            continue;
        }
        // INTEGER: anything else. A lone scalar at the chunk's start (the rest
        // is padding) goes as its own type; otherwise an integer covering the
        // chunk's bytes, as clang does
        if (others == 1 && floats == 0 && first) {
            arg.parts.push_back(first->is_pointer ? "ptr" : "i" + std::to_string(first->size * 8));
        } else {
            uint64_t bytes = std::min<uint64_t>(8, size - chunk * 8);
            arg.parts.push_back("i" + std::to_string(bytes * 8));
        }
        ++need_int;
    }
    arg.ir_type = arg.parts.size() == 1 ? arg.parts[0]
                                        : "{ " + arg.parts[0] + ", " + arg.parts[1] + " }";
    return arg;
}

// A scalar or array parameter / return value: passed as itself
AbiArg CodeGen::classify_scalar(const Type* t, bool is_return) const {
    AbiArg arg;
    arg.kind = AbiArg::Direct;
    arg.ir_type = isa<ArrayType>(t) ? "ptr" : llvm_type(t);   // arrays: pointer to the first element
    if (arg.ir_type == "i8") arg.attr = "signext";             // C's char is signed
    if (arg.ir_type == "i1") arg.attr = "zeroext";             // and _Bool is 0 or 1
    if (auto* pt = dyn_cast<PointerType>(t)) arg.attr = pointer_attrs(pt, is_return);
    return arg;
}

// What the type system proves about a safe pointer, told to LLVM so it can
// optimize with it (the facts C compilers have to guess):
//   T*, mut T*   nonnull dereferenceable(sizeof T): a borrow points to a live
//                object, which can't be freed while it is borrowed
//   own T*       nonnull (not dereferenceable: the callee may free it)
//   mut T*, own  noalias: a write borrow is exclusive and an owner unique, so
//                nothing else reaches the object during the call
//   T*?          the same, with dereferenceable_or_null instead of nonnull
// Read borrows get no readonly/noalias: trusted code may write through a T*.
// raw T* gets nothing. noalias relies on trusted code not creating aliases to
// a borrowed or owned object (the whitepaper's contract for trusted, 3.6.3).
// A returned own T* is nonnull (borrows can't be returned).
std::string CodeGen::pointer_attrs(const PointerType* pt, bool is_return) const {
    if (pt->ptrKind() == PtrKind::Raw) return "";
    bool nullable = pt->isNullable();
    bool borrow = pt->ptrKind() == PtrKind::Borrow || pt->ptrKind() == PtrKind::MutBorrow;
    if (is_return) return nullable ? "" : "nonnull";

    std::vector<std::string> attrs;
    uint64_t size = 0, align = 0;
    if (borrow) type_layout(pt->pointee(), size, align);
    if (borrow && size > 0) {
        if (nullable) attrs.push_back("dereferenceable_or_null(" + std::to_string(size) + ")");
        else attrs.push_back("nonnull dereferenceable(" + std::to_string(size) + ")");
    } else if (!nullable) {
        attrs.push_back("nonnull");
    }
    if (pt->ptrKind() == PtrKind::MutBorrow || pt->ptrKind() == PtrKind::Own) attrs.push_back("noalias");

    std::string s;
    for (size_t i = 0; i < attrs.size(); ++i) s += (i ? " " : "") + attrs[i];
    return s;
}

AbiSignature CodeGen::classify_signature(const FunctionType* ft) const {
    AbiSignature sig;
    int free_int = kIntArgRegs, free_sse = kSseArgRegs;

    const Type* ret = ft->returnType();
    if (ret && isa<StructType>(ret)) {
        uint64_t size, align;
        type_layout(ret, size, align);
        if (size == 0) {
            sig.ret.kind = AbiArg::Ignore;
        } else if (size > 16) {
            sig.ret.kind = AbiArg::Indirect;           // caller passes where to write it
            sig.ret.align = align;
            --free_int;                                // the sret pointer takes rdi
        } else {
            int ni, ns;                                // returned in rax/rdx, xmm0/xmm1
            sig.ret = classify_small_struct(ret, size, ni, ns);
        }
    } else {
        sig.ret = classify_scalar(ret, /*is_return=*/true);
    }

    for (const Type* pt : ft->params()) {
        if (!isa<StructType>(pt)) {
            AbiArg arg = classify_scalar(pt);
            if (arg.ir_type == "float") { if (free_sse > 0) --free_sse; }
            else if (free_int > 0) --free_int;
            sig.params.push_back(arg);                 // past the registers, LLVM uses the stack
            continue;
        }

        uint64_t size, align;
        type_layout(pt, size, align);
        AbiArg arg;
        if (size == 0) {
            arg.kind = AbiArg::Ignore;
        } else {
            int ni = 0, ns = 0;
            if (size <= 16) arg = classify_small_struct(pt, size, ni, ns);
            // A struct goes in registers only as a whole: if they ran out, or it is
            // too big, it is copied to the stack
            if (size <= 8 && free_int == 0) {
                // No integer registers left: a struct that fits in 8 bytes goes on
                // the stack as one integer instead of byval (same memory; clang's choice)
                arg = AbiArg{};
                arg.kind = AbiArg::Coerce;
                arg.ir_type = "i" + std::to_string(size * 8);
                arg.parts = {arg.ir_type};
            } else if (size > 16 || ni > free_int || ns > free_sse) {
                arg = AbiArg{};
                arg.kind = AbiArg::Indirect;
                arg.align = std::max<uint64_t>(align, 8);
            } else {
                free_int -= ni;
                free_sse -= ns;
            }
        }
        sig.params.push_back(arg);
    }
    return sig;
}

// "signext i8", "{ i64, i32 }", "void": the return type of a define/declare/call
std::string CodeGen::abi_return_type(const AbiSignature& sig) const {
    switch (sig.ret.kind) {
        case AbiArg::Direct:   return sig.ret.attr.empty() ? sig.ret.ir_type : sig.ret.attr + " " + sig.ret.ir_type;
        case AbiArg::Coerce:   return sig.ret.ir_type;
        case AbiArg::Indirect:
        case AbiArg::Ignore:   return "void";
    }
    return "void";
}

// The IR parameter list of a function with this signature. (*names)[i] is the
// base name for parameter i ("%x.arg", "%x.arg.0", ...); with names null, just
// the types (for a declare)
std::string CodeGen::abi_param_list(const AbiSignature& sig, const FunctionType* ft,
                                    const std::vector<std::string>* names) const {
    std::vector<std::string> out;
    auto named = [&](std::string s, const std::string& name) {
        if (names) s += " " + name;
        out.push_back(s);
    };

    if (sig.ret.kind == AbiArg::Indirect)
        named("ptr sret(" + llvm_type(ft->returnType()) + ") align " + std::to_string(sig.ret.align), "%agg.result");

    for (size_t i = 0; i < sig.params.size(); ++i) {
        const AbiArg& a = sig.params[i];
        std::string base = names ? (*names)[i] : "";
        switch (a.kind) {
            case AbiArg::Direct:
                named(a.attr.empty() ? a.ir_type : a.ir_type + " " + a.attr, base);
                break;
            case AbiArg::Coerce:
                for (size_t j = 0; j < a.parts.size(); ++j)
                    named(a.parts[j], base + "." + std::to_string(j));
                break;
            case AbiArg::Indirect:
                named("ptr byval(" + llvm_type(ft->params()[i]) + ") align " + std::to_string(a.align), base);
                break;
            case AbiArg::Ignore:
                break;
        }
    }

    std::string s;
    for (size_t i = 0; i < out.size(); ++i) s += (i ? ", " : "") + out[i];
    return s;
}

// The IR types of the parameter list, without names or attributes: the
// function type a variadic call has to spell out
std::string CodeGen::abi_param_types(const AbiSignature& sig) const {
    std::vector<std::string> types;
    if (sig.ret.kind == AbiArg::Indirect) types.push_back("ptr");
    for (const AbiArg& a : sig.params) {
        switch (a.kind) {
            case AbiArg::Direct:   types.push_back(a.ir_type); break;
            case AbiArg::Coerce:   types.insert(types.end(), a.parts.begin(), a.parts.end()); break;
            case AbiArg::Indirect: types.push_back("ptr"); break;
            case AbiArg::Ignore:   break;
        }
    }
    std::string s;
    for (size_t i = 0; i < types.size(); ++i) s += (i ? ", " : "") + types[i];
    return s;
}

// declare for a function defined elsewhere (in C): the same lowering as a
// definition, so C's calling convention matches on both sides
void CodeGen::emit_declare(const FunctionDecl& fn) {
    const FunctionType* ft = fn.functionType();
    AbiSignature sig = classify_signature(ft);
    std::string params = abi_param_list(sig, ft, nullptr);
    if (ft->isVariadic()) params += params.empty() ? "..." : ", ...";
    out_ << "\ndeclare " << abi_return_type(sig) << " @" << fn.name << "(" << params << ")\n";
}

// An argument for '...', passed the way C passes it: char and bool widened to
// int, float to double. Arrays (already a pointer to their first element),
// pointers and null are pointers
std::string CodeGen::promote_variadic_arg(const Type* t, const std::string& val) {
    if (isa<ArrayType>(t)) return "ptr " + val;
    std::string ty = llvm_type(t);
    if (ty == "i1" || ty == "i8") return "i32 " + convert_int(val, ty, "i32");   // bool: zext, char: sext
    if (ty == "float") {
        std::string wide = new_temp();
        emit(wide + " = fpext float " + val + " to double");
        return "double " + wide;
    }
    return ty + " " + val;
}

// A stack slot of an ABI type (always 8-aligned: enough for every coerced
// type and every SafeC struct)
std::string CodeGen::temp_alloca_ir(const std::string& ir_type) {
    std::string addr = "%agg.tmp." + std::to_string(++temp_counter_);
    temp_allocas_ << "  " << addr << " = alloca " << ir_type << ", align 8\n";
    return addr;
}

// The register pieces of a struct value: stores it to a slot of the coerced
// type (which is at least as big) and loads the pieces back
std::vector<std::string> CodeGen::split_for_abi(const AbiArg& a, const Type* t, const std::string& val) {
    std::string slot = temp_alloca_ir(a.ir_type);
    emit("store " + llvm_type(t) + " " + val + ", ptr " + slot);
    std::vector<std::string> pieces;
    for (size_t j = 0; j < a.parts.size(); ++j) {
        std::string addr = slot;
        if (a.parts.size() > 1) {
            addr = new_temp();
            emit(addr + " = getelementptr inbounds " + a.ir_type + ", ptr " + slot + ", i32 0, i32 " + std::to_string(j));
        }
        std::string piece = new_temp();
        emit(piece + " = load " + a.parts[j] + ", ptr " + addr);
        pieces.push_back(piece);
    }
    return pieces;
}

// The reverse of split_for_abi: writes the pieces into a slot of the coerced
// type and returns its address, where the struct can be loaded from
std::string CodeGen::join_from_abi(const AbiArg& a, const std::vector<std::string>& pieces) {
    std::string slot = temp_alloca_ir(a.ir_type);
    for (size_t j = 0; j < a.parts.size(); ++j) {
        std::string addr = slot;
        if (a.parts.size() > 1) {
            addr = new_temp();
            emit(addr + " = getelementptr inbounds " + a.ir_type + ", ptr " + slot + ", i32 0, i32 " + std::to_string(j));
        }
        emit("store " + a.parts[j] + " " + pieces[j] + ", ptr " + addr);
    }
    return slot;
}

// return val (empty for void) from the current function, the way its
// signature says the value travels
void CodeGen::emit_return(const std::string& val) {
    const Type* ret_type = current_fn_->functionType()->returnType();
    const AbiArg& r = current_abi_.ret;
    switch (r.kind) {
        case AbiArg::Direct:
            if (r.ir_type == "void" || val.empty()) emit_terminator("ret void");
            else emit_terminator("ret " + r.ir_type + " " + val);
            return;
        case AbiArg::Coerce: {
            std::string slot = temp_alloca_ir(r.ir_type);
            emit("store " + llvm_type(ret_type) + " " + val + ", ptr " + slot);
            std::string v = new_temp();
            emit(v + " = load " + r.ir_type + ", ptr " + slot);
            emit_terminator("ret " + r.ir_type + " " + v);
            return;
        }
        case AbiArg::Indirect:
            emit("store " + llvm_type(ret_type) + " " + val + ", ptr %agg.result");
            emit_terminator("ret void");
            return;
        case AbiArg::Ignore:
            emit_terminator("ret void");
            return;
    }
}

// A call to a function, with arguments and the result passed the way its
// signature says. Returns the result value ("" for void)
std::string CodeGen::gen_call(const CallExpr* c) {
    const FunctionDecl* fd = c->directCallee();
    if (!fd || !fd->functionType())
        throw std::runtime_error("only calls to a named function are supported");
    const FunctionType* ft = fd->functionType();
    AbiSignature sig = classify_signature(ft);

    std::vector<std::string> args;
    std::string result_slot;
    if (sig.ret.kind == AbiArg::Indirect) {
        result_slot = temp_alloca(c->type);
        args.push_back("ptr sret(" + llvm_type(c->type) + ") align " + std::to_string(sig.ret.align) + " " + result_slot);
    }

    // Arguments are evaluated left to right, each converted right after
    for (size_t i = 0; i < c->args.size(); ++i) {
        const Expr* arg = c->args[i].get();
        std::string val = gen_expr(arg);
        if (i >= ft->params().size()) {            // matches '...'
            args.push_back(promote_variadic_arg(arg->type, val));
            continue;
        }
        const Type* pt = ft->params()[i];
        const AbiArg& a = sig.params[i];
        switch (a.kind) {
            case AbiArg::Direct:
                args.push_back(a.ir_type + (a.attr.empty() ? "" : " " + a.attr) + " " + val);
                break;
            case AbiArg::Coerce: {
                std::vector<std::string> pieces = split_for_abi(a, pt, val);
                for (size_t j = 0; j < pieces.size(); ++j) args.push_back(a.parts[j] + " " + pieces[j]);
                break;
            }
            case AbiArg::Indirect: {
                std::string copy = temp_alloca(pt);
                emit("store " + llvm_type(pt) + " " + val + ", ptr " + copy);
                args.push_back("ptr byval(" + llvm_type(pt) + ") align " + std::to_string(a.align) + " " + copy);
                break;
            }
            case AbiArg::Ignore:
                break;
        }
    }

    // A variadic call names the function type, "i32 (ptr, ...)": on x86-64 the
    // callee is told how many float registers carry arguments only then
    std::string fn_type;
    if (ft->isVariadic()) {
        std::string fixed = abi_param_types(sig);
        fn_type = " (" + fixed + (fixed.empty() ? "..." : ", ...") + ")";
    }
    std::string call = "call " + abi_return_type(sig) + fn_type + " @" + fd->name + "(";
    for (size_t i = 0; i < args.size(); ++i) call += (i ? ", " : "") + args[i];
    call += ")";

    switch (sig.ret.kind) {
        case AbiArg::Direct: {
            if (sig.ret.ir_type == "void") { emit(call); return ""; }
            std::string res = new_temp();
            emit(res + " = " + call);
            return res;
        }
        case AbiArg::Coerce: {
            std::string res = new_temp();
            emit(res + " = " + call);
            std::vector<std::string> pieces;
            if (sig.ret.parts.size() == 1) {
                pieces.push_back(res);
            } else {
                for (size_t j = 0; j < sig.ret.parts.size(); ++j) {
                    std::string p = new_temp();
                    emit(p + " = extractvalue " + sig.ret.ir_type + " " + res + ", " + std::to_string(j));
                    pieces.push_back(p);
                }
            }
            std::string slot = join_from_abi(sig.ret, pieces);
            std::string v = new_temp();
            emit(v + " = load " + llvm_type(c->type) + ", ptr " + slot);
            return v;
        }
        case AbiArg::Indirect: {
            emit(call);
            std::string v = new_temp();
            emit(v + " = load " + llvm_type(c->type) + ", ptr " + result_slot);
            return v;
        }
        case AbiArg::Ignore:
            emit(call);
            return "zeroinitializer";
    }
    return "";
}

} // namespace safec
