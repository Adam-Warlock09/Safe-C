// codegen_debug.cpp - DWARF debug info for -g (whitepaper §5.1: GDB/LLDB).
//
// The IR gets LLVM's debug metadata and the backend turns it into DWARF:
//   - a compile unit and file for the module
//   - a subprogram (name, line, signature) for each function definition
//   - a source location on every instruction (the line table), scoped by a
//     lexical block per { } so shadowed variables are told apart
//   - llvm.dbg.declare for each parameter and local: its stack slot, name,
//     type and line
//   - types: int/char/bool/float, pointers, arrays, structs with their fields
//   - global variables
// Lines only: the lexer doesn't track columns, so every column is 0.

#include "codegen.h"
#include <filesystem>
#include <stdexcept>

namespace safec {

namespace {

// A string inside metadata: " and \ and non-printable bytes as \XX
std::string md_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "\\%02X", c);
            out += hex;
        }
    }
    return out + "\"";
}

std::string ref(int id) { return id < 0 ? "null" : "!" + std::to_string(id); }

} // namespace

// -g: describe the program for a debugger. source_path is the .safec file as
// given on the command line; the debugger finds it by directory + file name
void CodeGen::enable_debug_info(const std::string& source_path) {
    namespace fs = std::filesystem;
    fs::path abs = fs::absolute(fs::path(source_path)).lexically_normal();
    debug_ = true;
    debug_file_name_ = abs.filename().string();
    debug_dir_ = abs.parent_path().string();
}

// A metadata node; identical nodes are shared
int CodeGen::md(const std::string& body) {
    auto it = debug_node_ids_.find(body);
    if (it != debug_node_ids_.end()) return it->second;
    debug_nodes_.push_back(body);
    int id = static_cast<int>(debug_nodes_.size()) - 1;
    debug_node_ids_[body] = id;
    return id;
}

// A metadata node that is never shared (subprograms, scopes, the compile unit)
int CodeGen::md_distinct(const std::string& body) {
    debug_nodes_.push_back("distinct " + body);
    return static_cast<int>(debug_nodes_.size()) - 1;
}

// Called before anything is emitted: the file and compile unit everything
// else refers to. The compile unit is filled in at the end (its globals)
void CodeGen::debug_begin_module() {
    debug_file_ = md("!DIFile(filename: " + md_string(debug_file_name_) + ", directory: " + md_string(debug_dir_) + ")");
    debug_nodes_.push_back("");                         // the compile unit, filled in by debug_end_module
    debug_cu_ = static_cast<int>(debug_nodes_.size()) - 1;
}

// The metadata, after everything else in the module
void CodeGen::debug_end_module() {
    std::string globals;
    for (size_t i = 0; i < debug_globals_.size(); ++i) globals += (i ? ", " : "") + ref(debug_globals_[i]);
    debug_nodes_[debug_cu_] = "distinct !DICompileUnit(language: DW_LANG_C99, file: " + ref(debug_file_)
        + ", producer: \"safec\", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug"
        + ", nameTableKind: None"     // as clang on Linux: no .debug_names (gdb would ignore it, with a warning)
        + (globals.empty() ? "" : ", globals: !{" + globals + "}") + ")";
    int dwarf = md("!{i32 7, !\"Dwarf Version\", i32 5}");
    int version = md("!{i32 2, !\"Debug Info Version\", i32 3}");

    out_ << "\n!llvm.dbg.cu = !{" << ref(debug_cu_) << "}\n";
    out_ << "!llvm.module.flags = !{" << ref(dwarf) << ", " << ref(version) << "}\n";
    for (size_t i = 0; i < debug_nodes_.size(); ++i) out_ << "!" << i << " = " << debug_nodes_[i] << "\n";
}

// The debug type of t (-1 for void). Structs are created before their fields,
// so a struct can contain pointers to itself (struct Node { own Node*? next; })
int CodeGen::debug_type(const Type* t) {
    if (!t) return -1;
    if (auto it = debug_types_.find(t); it != debug_types_.end()) return it->second;

    int id = -1;
    if (auto* bt = dyn_cast<BuiltinType>(t)) {
        switch (bt->builtinKind()) {
            case BuiltinKind::Void:  return -1;
            case BuiltinKind::Int:   id = md("!DIBasicType(name: \"int\", size: 32, encoding: DW_ATE_signed)"); break;
            case BuiltinKind::Char:  id = md("!DIBasicType(name: \"char\", size: 8, encoding: DW_ATE_signed_char)"); break;
            case BuiltinKind::Bool:  id = md("!DIBasicType(name: \"bool\", size: 8, encoding: DW_ATE_boolean)"); break;
            case BuiltinKind::Float: id = md("!DIBasicType(name: \"float\", size: 32, encoding: DW_ATE_float)"); break;
            case BuiltinKind::Null:  id = md("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)"); break;
        }
    } else if (auto* pt = dyn_cast<PointerType>(t)) {
        int pointee = debug_type(pt->pointee());
        id = md("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + ref(pointee) + ", size: 64)");
    } else if (auto* at = dyn_cast<ArrayType>(t)) {
        int elem = debug_type(at->element());
        if (!at->size()) {                            // int xs[]: a pointer to the first element
            id = md("!DIDerivedType(tag: DW_TAG_pointer_type, baseType: " + ref(elem) + ", size: 64)");
        } else {
            uint64_t size, align;
            type_layout(t, size, align);
            int range = md("!DISubrange(count: " + std::to_string(*at->size()) + ")");
            id = md("!DICompositeType(tag: DW_TAG_array_type, baseType: " + ref(elem)
                    + ", size: " + std::to_string(size * 8) + ", elements: !{" + ref(range) + "})");
        }
    } else if (auto* st = dyn_cast<StructType>(t)) {
        const StructDecl* sd = st->decl();
        debug_nodes_.push_back("");                   // filled in below, once the fields exist
        id = static_cast<int>(debug_nodes_.size()) - 1;
        debug_types_[t] = id;

        std::string members;
        uint64_t offset = 0;
        for (const auto& f : sd->fields) {
            uint64_t fs, fa;
            type_layout(f->type, fs, fa);
            offset = (offset + fa - 1) / fa * fa;
            int member = md("!DIDerivedType(tag: DW_TAG_member, name: " + md_string(f->name) + ", scope: " + ref(id)
                            + ", file: " + ref(debug_file_) + ", line: " + std::to_string(f->loc.line)
                            + ", baseType: " + ref(debug_type(f->type)) + ", size: " + std::to_string(fs * 8)
                            + ", offset: " + std::to_string(offset * 8) + ")");
            members += (members.empty() ? "" : ", ") + ref(member);
            offset += fs;
        }
        uint64_t size, align;
        type_layout(t, size, align);
        debug_nodes_[id] = "distinct !DICompositeType(tag: DW_TAG_structure_type, name: " + md_string(sd->name)
            + ", file: " + ref(debug_file_) + ", line: " + std::to_string(sd->loc.line)
            + ", size: " + std::to_string(size * 8) + ", elements: !{" + members + "})";
        return id;
    }
    debug_types_[t] = id;
    return id;
}

// A global defined here (not extern): `print counter` in the debugger.
// Returns the attachment for its definition, ", !dbg !N"
std::string CodeGen::debug_global(const VarDecl& vd) {
    int var = md_distinct("!DIGlobalVariable(name: " + md_string(vd.name) + ", scope: " + ref(debug_cu_)
                          + ", file: " + ref(debug_file_) + ", line: " + std::to_string(vd.loc.line)
                          + ", type: " + ref(debug_type(vd.type)) + ", isLocal: false, isDefinition: true)");
    int expr = md("!DIGlobalVariableExpression(var: " + ref(var) + ", expr: !DIExpression())");
    debug_globals_.push_back(expr);
    return ", !dbg " + ref(expr);
}

// Starts a function's debug info. Returns the attachment for its define,
// " !dbg !N". The entry block (parameters copied to their slots) gets no
// line, as with clang: a breakpoint on the function then stops at its first
// statement, after the parameters are in place, not in the middle of that copy
std::string CodeGen::debug_begin_function(const FunctionDecl& fn) {
    const FunctionType* ft = fn.functionType();
    std::string types = ref(debug_type(ft->returnType()));
    for (const Type* p : ft->params()) types += ", " + ref(debug_type(p));
    int sig = md("!DISubroutineType(types: !{" + types + "})");
    current_subprogram_ = md_distinct("!DISubprogram(name: " + md_string(fn.name) + ", scope: " + ref(debug_file_)
        + ", file: " + ref(debug_file_) + ", line: " + std::to_string(fn.loc.line) + ", type: " + ref(sig)
        + ", scopeLine: " + std::to_string(fn.loc.line) + ", spFlags: DISPFlagDefinition, unit: " + ref(debug_cu_) + ")");
    debug_scopes_.clear();
    current_dbg_.clear();
    return " !dbg " + ref(current_subprogram_);
}

void CodeGen::debug_end_function() {
    current_subprogram_ = -1;
    debug_scopes_.clear();
    current_dbg_.clear();
}

int CodeGen::debug_scope() const {
    return debug_scopes_.empty() ? current_subprogram_ : debug_scopes_.back();
}

// Instructions emitted from now on are at this line, in the innermost scope
void CodeGen::debug_set_line(int line) {
    if (!debug_ || current_subprogram_ < 0 || line <= 0) return;
    int loc = md("!DILocation(line: " + std::to_string(line) + ", column: 0, scope: " + ref(debug_scope()) + ")");
    current_dbg_ = ", !dbg " + ref(loc);
}

// A { } block: variables declared in it belong to it, so a debugger shows the
// right one when names are shadowed
void CodeGen::debug_push_block(int line) {
    if (!debug_ || current_subprogram_ < 0) return;
    debug_scopes_.push_back(md_distinct("!DILexicalBlock(scope: " + ref(debug_scope()) + ", file: " + ref(debug_file_)
                                        + ", line: " + std::to_string(line) + ", column: 0)"));
}

void CodeGen::debug_pop_block() {
    if (!debug_ || current_subprogram_ < 0 || debug_scopes_.empty()) return;
    debug_scopes_.pop_back();
}

// Tells the debugger that vd lives at addr (arg_no: 1-based for parameters,
// 0 for locals). An array parameter's addr is the incoming pointer itself, so
// it is described by value instead of by its stack slot
void CodeGen::debug_declare_var(const VarDecl* vd, const std::string& addr, int arg_no, bool by_value) {
    if (!debug_ || current_subprogram_ < 0 || vd->name.empty()) return;
    if (!debug_declared_.insert(vd).second) return;     // a defer body is generated once per exit
    std::string var_body = "!DILocalVariable(name: " + md_string(vd->name)
        + (arg_no > 0 ? ", arg: " + std::to_string(arg_no) : "") + ", scope: " + ref(debug_scope())
        + ", file: " + ref(debug_file_) + ", line: " + std::to_string(vd->loc.line)
        + ", type: " + ref(debug_type(vd->type)) + ")";
    int var = md(var_body);
    int loc = md("!DILocation(line: " + std::to_string(vd->loc.line) + ", column: 0, scope: " + ref(debug_scope()) + ")");
    const char* intrinsic = by_value ? "llvm.dbg.value" : "llvm.dbg.declare";
    used_intrinsics_.insert(std::string("declare void @") + intrinsic + "(metadata, metadata, metadata)");
    if (!block_terminated_)
        body_ir_ << "  call void @" << intrinsic << "(metadata ptr " << addr << ", metadata " << ref(var)
                 << ", metadata !DIExpression()), !dbg " << ref(loc) << "\n";
}

} // namespace safec
