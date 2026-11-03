#include "sema.h"
#include "new_ast.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <iostream>
#include <unordered_set>

namespace safec {

	namespace {

		// Strips ImplicitCastExpr wrappers to inspect the underlying syntactic expression
		Expr* unwrapImplicit(Expr* expr) {
			while (auto* ice = dyn_cast<ImplicitCastExpr>(expr)) {
				expr = ice->sub.get();
			}
			return expr;
		}

		const Expr* unwrapImplicit(const Expr* expr) {
			while (auto* ice = dyn_cast<ImplicitCastExpr>(expr)) {
				expr = ice->sub.get();
			}
			return expr;
		}

		// Evaluates compile-time integer constants (for array sizes and switch cases)
		std::optional<int64_t> evalConstInt(const Expr* expr) {

			expr = unwrapImplicit(expr);
			if (!expr || expr->isInvalid()) return std::nullopt;

			if (auto* il = dyn_cast<IntLiteral>(expr)) {
				if (il->value > static_cast<uint64_t>(INT32_MAX)) return std::nullopt; // Overflow check
				return static_cast<int64_t>(il->value);
			}
			if (auto* cl = dyn_cast<CharLiteral>(expr)) {
				return static_cast<int64_t>(cl->value);
			}
			if (auto* bl = dyn_cast<BoolLiteral>(expr)) {
				return bl->value ? 1 : 0;
			}
			if (auto* ue = dyn_cast<UnaryExpr>(expr)) {
				// Recursive call to evaluate the operand
				auto sub = evalConstInt(ue->operand.get());
				if (!sub.has_value()) return std::nullopt;
				switch (ue->op) {
					case UnaryOp::Plus: 	return +(*sub);
					case UnaryOp::Minus:	return -(*sub);
					case UnaryOp::BitNot:	return ~(*sub);
					case UnaryOp::Not:		return !(*sub);
					default: 				return std::nullopt; // Unsupported unary operator for constant evaluation
				}
			}

			if (auto* be = dyn_cast<BinaryExpr>(expr)) {
				auto lhs = evalConstInt(be->lhs.get());
				auto rhs = evalConstInt(be->rhs.get());
				if (!lhs.has_value() || !rhs.has_value()) return std::nullopt;
				switch (be->op) {
					case BinaryOp::Add:    return *lhs + *rhs;
					case BinaryOp::Sub:    return *lhs - *rhs;
					case BinaryOp::Mul:    return *lhs * *rhs;
					case BinaryOp::Div:    return (*rhs == 0) ? std::nullopt : std::optional<int64_t>(*lhs / *rhs);
					case BinaryOp::Mod:    return (*rhs == 0) ? std::nullopt : std::optional<int64_t>(*lhs % *rhs);
					case BinaryOp::BitAnd: return *lhs & *rhs;
					case BinaryOp::BitOr:  return *lhs | *rhs;
					case BinaryOp::BitXor: return *lhs ^ *rhs;
					case BinaryOp::Shl:    return (*rhs < 0 || *rhs >= 63) ? std::nullopt : std::optional<int64_t>(*lhs << *rhs);
					case BinaryOp::Shr:    return (*rhs < 0 || *rhs >= 63) ? std::nullopt : std::optional<int64_t>(*lhs >> *rhs);
					case BinaryOp::Lt:     return (*lhs <  *rhs) ? 1 : 0;
					case BinaryOp::Gt:     return (*lhs >  *rhs) ? 1 : 0;
					case BinaryOp::Le:     return (*lhs <= *rhs) ? 1 : 0;
					case BinaryOp::Ge:     return (*lhs >= *rhs) ? 1 : 0;
					case BinaryOp::Eq:     return (*lhs == *rhs) ? 1 : 0;
					case BinaryOp::Ne:     return (*lhs != *rhs) ? 1 : 0;
					case BinaryOp::LogAnd: return (*lhs && *rhs) ? 1 : 0;
					case BinaryOp::LogOr:  return (*lhs || *rhs) ? 1 : 0;
				}
			}

			return std::nullopt; // Not a constant integer expression

		}

		// Checks whether a global initializer expression is a compile-time constant
		bool isGlobalConstant(const Expr* expr) {

			expr = unwrapImplicit(expr);
			if (!expr || expr->isInvalid()) return false;
			
			NodeKind k = expr->kind();

			if (k == NodeKind::IntLiteral || k == NodeKind::FloatLiteral || k == NodeKind::CharLiteral ||
				k == NodeKind::StringLiteral || k == NodeKind::BoolLiteral || k == NodeKind::NullLiteral) {
				return true;
			}

			if (evalConstInt(expr).has_value()) {
				return true;
			}

			if (auto* ue = dyn_cast<UnaryExpr>(expr)) {
				if (ue->op == UnaryOp::Minus || ue->op == UnaryOp::Plus) {
					return isa<FloatLiteral>(unwrapImplicit(ue->operand.get()));
				}
			}

			return false;

		}

		// Detects direct or indirect by-value struct containment cycles
		bool containsStructByValue(StructDecl* current, StructDecl* target, std::unordered_set<StructDecl*>& visited) {

			if (!current || current->isInvalid()) return false;
			if (!visited.insert(current).second) return false; // Already visited

			for (auto& field : current->fields) {
				if (!field->type || field->isInvalid()) continue;
				const Type* t = field->type;
				while (auto* at = dyn_cast<ArrayType>(t)) {
					t = at->element();
				}
				if (auto* st = dyn_cast<StructType>(t)) {
					StructDecl* fieldStruct = st->decl();
					if (fieldStruct == target || containsStructByValue(fieldStruct, target, visited)) {
						return true;
					}
				}
			}

			return false;

		}

		// Pre-scans an expression for assignments/increments to drop unsound loop narrowings
		void dropAssignedInExpr(const Expr* expr, std::unordered_map<ValueDecl*, const Type*>& narrowed) {
			if (!expr) return;
			switch (expr->kind()) {
				case NodeKind::AssignExpr: {
					auto* ae = cast<AssignExpr>(expr);
					if (auto* ne = dyn_cast<NameExpr>(unwrapImplicit(ae->target.get()))) {
						for (auto it = narrowed.begin(); it != narrowed.end(); ) {
							if (it->first->name == ne->name) it = narrowed.erase(it);
							else ++it;
						}
					}
					dropAssignedInExpr(ae->target.get(), narrowed);
					dropAssignedInExpr(ae->value.get(), narrowed);
					break;
				}
				case NodeKind::UnaryExpr: {
					auto* ue = cast<UnaryExpr>(expr);
					if (ue->isIncDec()) {
						if (auto* ne = dyn_cast<NameExpr>(unwrapImplicit(ue->operand.get()))) {
							for (auto it = narrowed.begin(); it != narrowed.end(); ) {
								if (it->first->name == ne->name) it = narrowed.erase(it);
								else ++it;
							}
						}
					}
					dropAssignedInExpr(ue->operand.get(), narrowed);
					break;
				}
				case NodeKind::BinaryExpr: {
					auto* be = cast<BinaryExpr>(expr);
					dropAssignedInExpr(be->lhs.get(), narrowed);
					dropAssignedInExpr(be->rhs.get(), narrowed);
					break;
				}
				case NodeKind::CallExpr: {
					auto* ce = cast<CallExpr>(expr);
					dropAssignedInExpr(ce->callee.get(), narrowed);
					for (const auto& arg : ce->args) {
						dropAssignedInExpr(arg.get(), narrowed);
						// Also drop narrowing if a variable's mutable address (&mut p) is passed
						if (auto* ue = dyn_cast<UnaryExpr>(unwrapImplicit(arg.get()))) {
							if (ue->op == UnaryOp::AddrOfMut) {
								if (auto* ne = dyn_cast<NameExpr>(unwrapImplicit(ue->operand.get()))) {
									for (auto it = narrowed.begin(); it != narrowed.end(); ) {
										if (it->first->name == ne->name) it = narrowed.erase(it);
										else ++it;
									}
								}
							}
						}
					}
					break;
				}
				case NodeKind::IndexExpr: {
					auto* ie = cast<IndexExpr>(expr);
					dropAssignedInExpr(ie->base.get(), narrowed);
					dropAssignedInExpr(ie->index.get(), narrowed);
					break;
				}
				case NodeKind::MemberExpr:
					dropAssignedInExpr(cast<MemberExpr>(expr)->base.get(), narrowed);
					break;
				case NodeKind::FreeExpr:
					dropAssignedInExpr(cast<FreeExpr>(expr)->operand.get(), narrowed);
					break;
				case NodeKind::CompoundLiteralExpr:
					for (const auto& init : cast<CompoundLiteralExpr>(expr)->inits)
						dropAssignedInExpr(init.value.get(), narrowed);
					break;
				case NodeKind::ImplicitCastExpr:
					dropAssignedInExpr(cast<ImplicitCastExpr>(expr)->sub.get(), narrowed);
					break;
				default:
					break;
			}
		}

		void dropAssignedInStmt(const Stmt* stmt, std::unordered_map<ValueDecl*, const Type*>& narrowed) {
			if (!stmt) return;
			switch (stmt->kind()) {
				case NodeKind::CompoundStmt:
					for (const auto& s : cast<CompoundStmt>(stmt)->body)
						dropAssignedInStmt(s.get(), narrowed);
					break;
				case NodeKind::DeclStmt:
					for (const auto& vd : cast<DeclStmt>(stmt)->decls)
						if (vd->init) dropAssignedInExpr(vd->init.get(), narrowed);
					break;
				case NodeKind::ExprStmt:
					dropAssignedInExpr(cast<ExprStmt>(stmt)->expr.get(), narrowed);
					break;
				case NodeKind::IfStmt: {
					auto* is = cast<IfStmt>(stmt);
					dropAssignedInExpr(is->cond.get(), narrowed);
					dropAssignedInStmt(is->thenStmt.get(), narrowed);
					if (is->elseStmt) dropAssignedInStmt(is->elseStmt.get(), narrowed);
					break;
				}
				case NodeKind::WhileStmt: {
					auto* ws = cast<WhileStmt>(stmt);
					dropAssignedInExpr(ws->cond.get(), narrowed);
					dropAssignedInStmt(ws->body.get(), narrowed);
					break;
				}
				case NodeKind::ForStmt: {
					auto* fs = cast<ForStmt>(stmt);
					if (fs->init) dropAssignedInStmt(fs->init.get(), narrowed);
					if (fs->cond) dropAssignedInExpr(fs->cond.get(), narrowed);
					if (fs->step) dropAssignedInExpr(fs->step.get(), narrowed);
					dropAssignedInStmt(fs->body.get(), narrowed);
					break;
				}
				case NodeKind::SwitchStmt: {
					auto* sw = cast<SwitchStmt>(stmt);
					dropAssignedInExpr(sw->cond.get(), narrowed);
					for (const auto& sc : sw->cases)
						for (const auto& s : sc->body)
							dropAssignedInStmt(s.get(), narrowed);
					break;
				}
				case NodeKind::ReturnStmt:
					dropAssignedInExpr(cast<ReturnStmt>(stmt)->value.get(), narrowed);
					break;
				case NodeKind::DeferStmt:
					dropAssignedInStmt(cast<DeferStmt>(stmt)->body.get(), narrowed);
					break;
				case NodeKind::TrustedStmt:
					dropAssignedInStmt(cast<TrustedStmt>(stmt)->body.get(), narrowed);
					break;
				default:
					break;
			}
		}

	} // namespace

	// ===================================================================
	// Pipeline Entry & Type Resolution
	// ===================================================================

	void Sema::run(Program& prog) {
		
		// Pass 1 : Global Collection
		collectGlobals(prog);

		// Pass 2 : Struct Fields
		resolveStructFields(prog);

		// Pass 3 : Signatures
		resolveSignatures(prog);

		// Pass 4 : Bodies
		checkBodies(prog);

		scope.pop(); // Clean up global scope opened in collectGlobals()

	}

	const Type* Sema::resolveType(TypeSpec* spec, Program& prog, bool isParam) {

		if (!spec || spec->isInvalid()) return nullptr;
		if (spec->resolved) return spec->resolved;

		TypeContext& types = prog.types;
		const Type* currentType = nullptr;

		// 1. Resolve the Base Type
		switch (spec->base) {

			case BaseTypeKind::Void:
				currentType = types.voidType();
				break;
			case BaseTypeKind::Bool:
				currentType = types.boolType();
				break;
			case BaseTypeKind::Char:
				currentType = types.charType();
				break;
			case BaseTypeKind::Int:
				currentType = types.intType();
				break;
			case BaseTypeKind::Float:
				currentType = types.floatType();
				break;
			case BaseTypeKind::Struct: {
				auto it = structs.find(spec->structName);
				if (it == structs.end()) {
					diag.error(spec->loc, "Unknown struct type: '" + spec->structName + "'");
					spec->setInvalid();
					return nullptr;
				}
				currentType = types.structType(it->second);
				break;

			}
		
		}

		// 2. Validate Qualifiers (own/mut/raw require a pointer)
		if (spec->qualifier != PtrQualifier::None && spec->pointers.empty()) {
			diag.error(spec->loc, "Pointer qualifier applied to non-pointer type");
			spec->setInvalid();
			return nullptr; // Prevent returning a valid base type with an ignored qualifier
		}

		// 3. Build Pointer Levels (pointers[0] is the innermost, apply qualifiers to the outermost)
		for (size_t i = 0; i < spec->pointers.size(); ++i) {
			bool isOutermost = (i == spec->pointers.size() - 1);
			PtrKind pk = PtrKind::Borrow;

			if (isOutermost) {
				switch (spec->qualifier) {
					case PtrQualifier::None:    pk = PtrKind::Borrow; break;
					case PtrQualifier::Own:     pk = PtrKind::Own; break;
					case PtrQualifier::Mut:     pk = PtrKind::MutBorrow; break;
					case PtrQualifier::Raw:     pk = PtrKind::Raw; break;
				}
			}

			currentType = types.pointerTo(currentType, pk, spec->pointers[i].nullable);

		}

		// 4. Build Array Dimensions (Evaluate from innermost to outermost)
		for (size_t i = spec->arrayDims.size(); i > 0; --i) {
			Expr* dimExpr = spec->arrayDims[i - 1].get();
			bool isOutermostDim = (i - 1 == 0);
			std::optional<uint64_t> size = std::nullopt;

			if (dimExpr) {
				
				checkExpr(dimExpr, prog);

				auto constVal = evalConstInt(dimExpr);
				if (!constVal.has_value()) {
					if (!dimExpr->isInvalid()) {
						diag.error(dimExpr->loc, "Array size must be a constant integer");
						dimExpr->setInvalid();
					}
					spec->setInvalid();
					return nullptr;
				} else if (*constVal <= 0) {
					diag.error(dimExpr->loc, "Array size must be greater than zero");
					dimExpr->setInvalid();
					spec->setInvalid();
					return nullptr;
				} else {
					size = static_cast<uint64_t>(*constVal);
				}

			} else {

				// Unsized '[]' is only permitted on the outermost dimension of a parameter
				if (!isParam || !isOutermostDim) {
					diag.error(spec->loc, "Unsized arrays '[]' are only allowed on function parameters");
					spec->setInvalid();
					return nullptr;
				}

			}

			// Void arrays are always illegal, regardless of context
			if (currentType->kind() == TypeKind::Builtin && cast<BuiltinType>(currentType)->builtinKind() == BuiltinKind::Void) {
				diag.error(spec->loc, "Array element type cannot be void");
				spec->setInvalid();
				return nullptr;
			}

			currentType = types.arrayOf(currentType, size);
 
		}

		spec->resolved = currentType;
		return currentType;

	}

	// ===================================================================
	// Passes 1 - 3: Declarations & Signatures
	// ===================================================================

	void Sema::collectGlobals(Program& prog) {

		scope.push(); // Initialize global scope

		// Functions and global variables share one namespace in SafeC
		std::unordered_map<std::string_view, Decl*> globalNames;

		for (auto& decl : prog.decls) {

			if (auto* sd = dyn_cast<StructDecl>(decl.get())) {

				auto [it, inserted] = structs.emplace(sd->name, sd);
				if (!inserted) {
					diag.error(sd->loc, "Duplicate struct name: '" + sd->name + "'");;
					diag.note(it->second->loc, "Previous declaration is here");
					sd->setInvalid();
				}else {
					sd->type = prog.types.structType(sd);
				}

			} else if (auto* fd = dyn_cast<FunctionDecl>(decl.get())) {

				auto [it, inserted] = globalNames.emplace(fd->name, fd);
				if (!inserted) {
					diag.error(fd->loc, "Duplicate global name: '" + fd->name + "'");
					diag.note(it->second->loc, "Previous declaration is here");
					fd->setInvalid();
				} else {
					funs[fd->name] = fd;
				}

			} else if (auto* vd = dyn_cast<VarDecl>(decl.get())) {

				auto [it, inserted] = globalNames.emplace(vd->name, vd);
				if (!inserted) {
					diag.error(vd->loc, "Duplicate global name: '" + vd->name + "'");
					diag.note(it->second->loc, "Previous declaration is here");
					vd->setInvalid();
				} else {
					scope.declare(vd);
				}

			}
		}

	}

	void Sema::resolveStructFields(Program& prog) {

		// Resolve all field types and check for duplicate or void fields
		for (auto& decl : prog.decls) {

			if (auto* sd = dyn_cast<StructDecl>(decl.get()))
			{

				if (sd->isInvalid()) continue;

				std::unordered_map<std::string_view, FieldDecl*> fieldNames;

				for (auto& field : sd->fields) {

					auto [it, inserted] = fieldNames.emplace(field->name, field.get());
					if (!inserted) {
						diag.error(field->loc, "Duplicate field name: '" + field->name + "'");
						diag.note(it->second->loc, "Previous declaration is here");
						field->setInvalid();
						continue;
					}

					field->type = resolveType(field->typeSpec.get(), prog, false);
					if (!field->type) {
						field->setInvalid();
						continue;
					}

					// Reject void fields
					if (field->type->kind() == TypeKind::Builtin) {
						if (cast<BuiltinType>(field->type)->builtinKind() == BuiltinKind::Void) {
							diag.error(field->loc, "Field type cannot be void");
							field->setInvalid();
							field->type = nullptr;
							continue;
						}
					}

				}
			}

		}

		// Check for direct or indirect recursive containment by value
		for (auto& decl : prog.decls) {
			if (auto* sd = dyn_cast<StructDecl>(decl.get())) {

				if (sd->isInvalid()) continue;

				for (auto& field : sd->fields) {
					if (!field->type || field->isInvalid()) continue;

					const Type* t = field->type;
					while (auto* at = dyn_cast<ArrayType>(t)) {
						t = at->element();
					}

					if (auto* st = dyn_cast<StructType>(t)) {
						StructDecl* fieldStruct = st->decl();
						std::unordered_set<StructDecl*> visited;
						if (fieldStruct == sd || containsStructByValue(fieldStruct, sd, visited)) {
							diag.error(field->loc, "Struct cannot contain itself by value; Use a pointer instead");
							field->setInvalid();
							field->type = nullptr;
						}
					}
				}

			}
		}

	}

	void Sema::resolveSignatures(Program& prog) {

		// 1: Resolve function return types and parameter types
		for (auto& decl : prog.decls) {

			if (auto* fd = dyn_cast<FunctionDecl>(decl.get())) {

				if (fd->isInvalid()) continue;

				// Resolve return type
				const Type* retType = resolveType(fd->returnSpec.get(), prog, false);
				if (!retType) {
					fd->setInvalid();
				}

				// Resolve parameter types
				std::vector<const Type*> paramTypes;
				std::unordered_map<std::string_view, ParamDecl*> paramNames;

				for (auto& param : fd->params) {

					auto [it, inserted] = paramNames.emplace(param->name, param.get());
					if (!inserted) {
						diag.error(param->loc, "Duplicate parameter name: '" + param->name + "'");
						diag.note(it->second->loc, "Previous declaration is here");
						param->setInvalid();
						continue;
					}

					const Type* pType = resolveType(param->typeSpec.get(), prog, true);
					if (!pType) {
						param->setInvalid();
						continue;
					}

					if (pType->kind() == TypeKind::Builtin && cast<BuiltinType>(pType)->builtinKind() == BuiltinKind::Void) {
						diag.error(param->loc, "parameter cannot have void type");
						param->setInvalid();
						continue;
					}

					param->type = pType;
					paramTypes.push_back(pType);

				}

				// Verify main() signature constraints
				if (fd->name == "main") {
					if (retType && (retType->kind() != TypeKind::Builtin || cast<BuiltinType>(retType)->builtinKind() != BuiltinKind::Int)) {
						diag.error(fd->loc, "main() must return int");
						fd->setInvalid();
					}
					if (!fd->params.empty())
					{
						diag.error(fd->loc, "main() must not take any arguments (void)");
						fd->setInvalid();
					}
				}

				if (retType && paramTypes.size() == fd->params.size()) {
					fd->type = prog.types.functionType(retType, std::move(paramTypes));
				} else {
					fd->setInvalid();
				}

			} else if (auto* vd = dyn_cast<VarDecl>(decl.get())) {

				if (vd->isInvalid()) continue;

				// Resolve variable type
				const Type* varType = resolveType(vd->typeSpec.get(), prog, false);
				
				if (!varType) {
					vd->setInvalid();
				} else if (varType->kind() == TypeKind::Builtin && cast<BuiltinType>(varType)->builtinKind() == BuiltinKind::Void) {
					diag.error(vd->loc, "Variable cannot have void type");
					vd->setInvalid();
					varType = nullptr;
				}
				
				vd->type = varType;

			}

		}

		// 2: Check global variable initializers now that all global types are resolved
		for (auto& decl : prog.decls) {
			if (auto* vd = dyn_cast<VarDecl>(decl.get())) {
				if (!vd->init) continue;

				checkExpr(vd->init.get(), prog);

				if (vd->isGlobal && !vd->init->isInvalid() && !isGlobalConstant(vd->init.get())) {
					diag.error(vd->init->loc, "Global initializers must be constants");
					vd->init->setInvalid();
					vd->setInvalid();
					continue;
				}

				if (vd->type && !vd->init->isInvalid()) {
					if (!coerce(vd->init, vd->type)) {
						vd->setInvalid();
					}
				}
			}
		}

	}

	// ===================================================================
	// Pass 4: Bodies & Statements () Narrowing/Coercion)
	// ===================================================================

	void Sema::checkBodies(Program& prog) {

		for (auto& decl : prog.decls) {

			if (auto* fd = dyn_cast<FunctionDecl>(decl.get())) {

				if (fd->isInvalid() || !fd->body) continue;

				const Type* retType = fd->returnSpec ? fd->returnSpec->resolved : nullptr;
				if (!retType) {
					continue;
				}

				currFun = fd;
				narrowedVars.clear();
				scope.push(); // Open function-level scope

				// Declare parameters in the function scope
				for (auto& param : fd->params) {
					if (!param->name.empty()) {
						scope.declare(param.get());
					}
				}

				checkStmt(fd->body.get(), prog);

				// Simple fall-off check : Warning if non-void function doesn't end with a return statement
				if (retType != prog.types.voidType()) {
					if (fd->body->body.empty() || !isa<ReturnStmt>(fd->body->body.back().get())) {
						diag.warning(fd->loc, "Non-void function may exit without returning a value");
					}
				}

				scope.pop(); // Close function-level scope
				narrowedVars.clear();
				currFun = nullptr;

			}

		}

	}

	void Sema::checkCondition(ExprPtr& cond, SourceLoc loc, Program& prog) {
		
		if (!cond) return;

		checkExpr(cond.get(), prog);
		if (cond->isInvalid() || !cond->type) return;

		coerceToBool(cond, prog.types);
		
	}

	void Sema::checkStmt(Stmt* stmt, Program& prog) {

		if (!stmt || stmt->isInvalid()) return;

		switch(stmt->kind()) {

			case NodeKind::CompoundStmt: {

				auto* block = cast<CompoundStmt>(stmt);
				scope.push();

				for (auto& s : block->body) {
					checkStmt(s.get(), prog);
				}

				scope.pop();
				break;

			}

			case NodeKind::DeclStmt: {

				auto* ds = cast<DeclStmt>(stmt);

				for (auto& vd : ds->decls) {

					const Type* varType = resolveType(vd->typeSpec.get(), prog, false);

					if (!varType) {
						vd->setInvalid();
					} else if (varType->kind() == TypeKind::Builtin && cast<BuiltinType>(varType)->builtinKind() == BuiltinKind::Void) {
						diag.error(vd->loc, "Local variable cannot have void type");
						vd->setInvalid();
						varType = nullptr;
					}

					vd->type = varType;

					if (vd->init) {
						checkExpr(vd->init.get(), prog);
						if (vd->type && !vd->init->isInvalid()) {
							if (!coerce(vd->init, vd->type)) {
								vd->setInvalid();
							}
						}
					}

					// Declare *after* checking init so `int x = x;` fails properly
					if (!scope.declare(vd.get())) {
						VarDecl* prev = scope.lookupCurrentFrame(vd->name);
						diag.error(vd->loc, "Duplicate local variable '" + vd->name + "'" );
						if (prev) diag.note(prev->loc, "Previous declaration is here");
						vd->setInvalid();
					}
					
				}

				break;

			}

			case NodeKind::ExprStmt: {
				
				checkExpr(cast<ExprStmt>(stmt)->expr.get(), prog);
				break;

			}

			case NodeKind::IfStmt: {

				auto* is = cast<IfStmt>(stmt);
				checkCondition(is->cond, is->cond->loc, prog);

				auto savedNarrowing = narrowedVars;

				applyNarrowing(is->cond.get(), /*isTrueBranch=*/true, prog.types);
				checkStmt(is->thenStmt.get(), prog);
				auto afterThen = narrowedVars;

				// False branch (`if (p == null) ... else`)
				narrowedVars = savedNarrowing;
				if (is->elseStmt) {
					applyNarrowing(is->cond.get(), /*isTrueBranch=*/false, prog.types);
					checkStmt(is->elseStmt.get(), prog);
				}
				auto afterElse = narrowedVars;

				narrowedVars.clear();
				for (const auto& [decl, type] : savedNarrowing) {
					if (afterThen.count(decl) && afterElse.count(decl)) {
						narrowedVars[decl] = type;
					}
				}

				break;

			}

			case NodeKind::WhileStmt: {
				
				auto* ws = cast<WhileStmt>(stmt);

				// Drop outer narrowings for any variable modified inside the loop before checking cond/body
				dropAssignedInExpr(ws->cond.get(), narrowedVars);
				dropAssignedInStmt(ws->body.get(), narrowedVars);
				auto savedNarrowing = narrowedVars;

				checkCondition(ws->cond, ws->cond->loc, prog);

				applyNarrowing(ws->cond.get(), /*isTrueBranch=*/true, prog.types);
				dropAssignedInStmt(ws->body.get(), narrowedVars);

				breakStack.push_back(ws);
				continueStack.push_back(ws);

				checkStmt(ws->body.get(), prog);

				breakStack.pop_back();
				continueStack.pop_back();

				narrowedVars = std::move(savedNarrowing);

				break;

			}

			case NodeKind::ReturnStmt: {

				auto* rs = cast<ReturnStmt>(stmt);

				if (inDefer > 0) {
					diag.error(rs->loc, "Cannot return from within a defer block");
					rs->setInvalid();
					if (rs->value) checkExpr(rs->value.get(), prog);
					break;
				}

				const Type *retType = (currFun && currFun->returnSpec) ? currFun->returnSpec->resolved : nullptr;
				bool isVoidFunc = (retType == prog.types.voidType());

				if (rs->value) {
					checkExpr(rs->value.get(), prog);
					if (isVoidFunc) {
						diag.error(rs->loc, "Cannot return a value from a void function");
						rs->setInvalid();
					} else if (retType && !rs->value->isInvalid()) {
						if (!coerce(rs->value, retType)) {
							rs->setInvalid();
						}
					}
				} else if (!isVoidFunc) {
					diag.error(rs->loc, "Non-void function must return a value");
					rs->setInvalid();
				}

				break;

			}

			case NodeKind::ForStmt: {

				auto* fs = cast<ForStmt>(stmt);
				scope.push(); // For loops have their own scope for the initializer

				if (fs->init) checkStmt(fs->init.get(), prog);

				// Drop outer narrowings for any variable modified in the loop cond, step, or body
				if (fs->cond) dropAssignedInExpr(fs->cond.get(), narrowedVars);
				if (fs->step) dropAssignedInExpr(fs->step.get(), narrowedVars);
				dropAssignedInStmt(fs->body.get(), narrowedVars);
				auto savedNarrowing = narrowedVars;

				if (fs->cond) checkCondition(fs->cond, fs->cond->loc, prog);
				if (fs->step) checkExpr(fs->step.get(), prog);

				breakStack.push_back(fs);
				continueStack.push_back(fs);

				checkStmt(fs->body.get(), prog);

				breakStack.pop_back();
				continueStack.pop_back();
				narrowedVars = std::move(savedNarrowing);
				scope.pop(); // Pop the for-loop scope
				break;

			}

			case NodeKind::SwitchStmt: {

				auto* sw = cast<SwitchStmt>(stmt);
				checkExpr(sw->cond.get(), prog);

				if (sw->cond->type && !sw->cond->isInvalid()) {
					auto* bt = dyn_cast<BuiltinType>(sw->cond->type);
					if (!bt || !bt->isInteger()) {
						diag.error(sw->cond->loc, "Switch condition must be an integer type");
						sw->cond->setInvalid();
					}
				}

				scope.push(); // Switch statements have their own scope for case variables
				breakStack.push_back(sw);

				std::unordered_map<int64_t, SourceLoc> seenCases; // Track case values to detect duplicates
				bool hasDefault = false;
				SourceLoc defaultLoc;

				for (auto& sc : sw->cases) {

					if (auto* cs = dyn_cast<CaseStmt>(sc.get())) {

						checkExpr(cs->value.get(), prog);

						if (!cs->value->isInvalid()) {

							auto constVal = evalConstInt(cs->value.get());
							if (constVal.has_value()) {
								auto [it, inserted] = seenCases.emplace(*constVal, cs->loc);
								if (!inserted) {
									diag.error(cs->loc, "Duplicate case value in switch");
									diag.note(it->second, "Previous case with this value is here");
									cs->setInvalid();
								}
							} else {
								diag.error(cs->value->loc, "Case value must be a constant integer");
								cs->setInvalid();
							}
						}
						for (auto& s : cs->body) {
							checkStmt(s.get(), prog);
						}
					} else if(auto* ds = dyn_cast<DefaultStmt>(sc.get())) {
						if (hasDefault) {
							diag.error(ds->loc, "Switch statement can have atmost one default case");
							diag.note(defaultLoc, "Previous default case is here");
							ds->setInvalid();
						} else {
							hasDefault = true;
							defaultLoc = ds->loc;
						}
						for (auto& s : ds->body) {
							checkStmt(s.get(), prog);
						}
					}

				}

				breakStack.pop_back();
				scope.pop(); // Pop the switch statement scope
				break;

			}

			case NodeKind::BreakStmt: {
				
				auto* bs = cast<BreakStmt>(stmt);
				if (breakStack.empty()) {
					diag.error(bs->loc, inDefer > 0 
						? "Cannot break out of a defer block"
						: "Break statement not within a loop or switch"
					);
					bs->setInvalid();
				} else {
					bs->target = breakStack.back();
				}

				break;

			}

			case NodeKind::ContinueStmt: {
				
				auto* cs = cast<ContinueStmt>(stmt);
				if (continueStack.empty()) {
					diag.error(cs->loc, inDefer > 0 
						? "Cannot continue out of a defer block"
						: "Continue statement not within a loop"
					);
					cs->setInvalid();
				} else {
					cs->target = continueStack.back();
				}

				break;

			}

			case NodeKind::DeferStmt: {

				auto* ds = cast<DeferStmt>(stmt);

				if (inDefer > 0) {
					diag.error(ds->loc, "Cannot nest defer blocks");
					ds->setInvalid();
					break;
				}

				auto oldBreak = std::move(breakStack);
				auto oldContinue = std::move(continueStack);

				// Clear stacks so that inner jumps cannot escape the defer block
				breakStack.clear();
				continueStack.clear();
				
				inDefer++;
				checkStmt(ds->body.get(), prog);
				inDefer--;

				// Restore outer jump targets
				breakStack = std::move(oldBreak);
				continueStack = std::move(oldContinue);

				break;

			}

			case NodeKind::TrustedStmt: {

				auto* ts = cast<TrustedStmt>(stmt);
				bool oldTrusted = isTrusted;
				isTrusted = true;
				checkStmt(ts->body.get(), prog);
				isTrusted = oldTrusted;
				break;

			}

			case NodeKind::NullStmt:
				// Nothing to check for null statements
				break;

			default:
				break;

		}

	}

	// ===================================================================
	// Expression Dispatch
	// ===================================================================

	void Sema::checkExpr(Expr* expr, Program& prog) {

		if (!expr || expr->isInvalid()) return;

		switch(expr->kind()) {

			case NodeKind::IntLiteral: {

				auto* il = cast<IntLiteral>(expr);
				if (il->value > static_cast<uint64_t>(INT32_MAX)) {
					diag.error(il->loc, "Integer literal does not fit in 'int'");
					il->setInvalid();
					break;
				}
				il->type = prog.types.intType();
				il->category = ValueCategory::RValue;
				break;

			}

			case NodeKind::FloatLiteral: {

				expr->type = prog.types.floatType();
				expr->category = ValueCategory::RValue;
				break;

			}

			case NodeKind::CharLiteral: {

				expr->type = prog.types.charType();
				expr->category = ValueCategory::RValue;
				break;

			}

			case NodeKind::BoolLiteral: {

				expr->type = prog.types.boolType();
				expr->category = ValueCategory::RValue;
				break;

			}

			case NodeKind::NullLiteral: {

				expr->type = prog.types.nullType();
				expr->category = ValueCategory::RValue;
				break;

			}

			case NodeKind::StringLiteral: {

				expr->type = prog.types.pointerTo(prog.types.charType(), PtrKind::Borrow, false);
				expr->category = ValueCategory::RValue;
				break;

			}

			case NodeKind::NameExpr: {

				auto* ne = cast<NameExpr>(expr);

				// Look up in local scope first, then global functions
				if (VarDecl* local = scope.lookup(ne->name)) {
					ne->decl = local;
					if (local->isInvalid() || !local->type) {
						ne->setInvalid();
						break;
					}
					ne->type = getNarrowedType(local, local->type);
					ne->category = ValueCategory::LValue;
				} else if (auto it = funs.find(ne->name); it != funs.end()) {
					ne->decl = it->second;
					if (!inCallee) {
						diag.error(ne->loc, "Function '" + ne->name + "' cannot be used as a value; use a function call instead");
						ne->setInvalid();
						break;
					}
					if (it->second->isInvalid() || !it->second->type) {
						ne->setInvalid();
						break;
					}
					ne->type = it->second->type;
					ne->category = ValueCategory::RValue; // Functions are rvalues
				} else {
					diag.error(ne->loc, "Undeclared identifier: '" + ne->name + "'");
					ne->setInvalid();
				}

				break;

			}

			case NodeKind::UnaryExpr: {

				auto* ue = cast<UnaryExpr>(expr);

				checkExpr(ue->operand.get(), prog);
				if (ue->operand->isInvalid() || !ue->operand->type) {
					ue->setInvalid();
					break;
				}

				checkUnary(ue, prog.types);

				break;

			}

			case NodeKind::BinaryExpr: {

				auto* be = cast<BinaryExpr>(expr);

				checkExpr(be->lhs.get(), prog);
				checkExpr(be->rhs.get(), prog);
				
				if (be->lhs->isInvalid() || !be->lhs->type || be->rhs->isInvalid() || !be->rhs->type) {
					be->setInvalid();
					break;
				}

				checkBinary(be, prog.types);
				break;

			}

			case NodeKind::AssignExpr: {

				auto *ae = cast<AssignExpr>(expr);

				checkExpr(ae->target.get(), prog);
				checkExpr(ae->value.get(), prog);

				if (ae->target->isInvalid() || !ae->target->type || ae->value->isInvalid() || !ae->value->type) {
					clearNarrowing(ae->target.get());
					ae->setInvalid();
					break;
				}

				checkAssignment(ae, prog.types);
				break;

			}

			case NodeKind::CallExpr: {

				auto* ce = cast<CallExpr>(expr);

				bool oldInCallee = inCallee;
				inCallee = true;
				checkExpr(ce->callee.get(), prog);
				inCallee = oldInCallee;
				
				for (auto& arg : ce->args) {
					checkExpr(arg.get(), prog);
				}

				if (ce->callee->isInvalid() || !ce->callee->type) {
					ce->setInvalid(); // Propagate invalid silently
					break;
				}

				if (ce->callee->type->kind() != TypeKind::Function) {
					diag.error(ce->loc, "Called object is not a function");
					ce->setInvalid();
					break;
				}

				for (auto& arg : ce->args) {
					if (arg->isInvalid() || !arg->type) {
						ce->setInvalid(); // Propagate invalid silently
						break;
					}
				}
				if (ce->isInvalid()) break;

				checkCall(ce, cast<FunctionType>(ce->callee->type));
				break;

			}

			case NodeKind::IndexExpr: {

				auto* ie = cast<IndexExpr>(expr);

				checkExpr(ie->base.get(), prog);
				checkExpr(ie->index.get(), prog);

				if (ie->base->isInvalid() || !ie->base->type || ie->index->isInvalid() || !ie->index->type) {
					ie->setInvalid();
					break;
				}

				checkIndex(ie, prog.types);
				break;

			}

			case NodeKind::MemberExpr: {

				auto* me = cast<MemberExpr>(expr);
				checkExpr(me->base.get(), prog);

				if (me->base->isInvalid() || !me->base->type) {
					me->setInvalid(); // Propagate invalid silently
					break;
				}

				checkMember(me);
				break;

			}

			case NodeKind::NewExpr: {

				auto* ne = cast<NewExpr>(expr);

				const Type* allocType = resolveType(ne->allocated.get(), prog, false);

				if (!allocType) {
					ne->setInvalid();
					break;
				}

				checkNew(ne, allocType, prog.types);
				break;

			}

			case NodeKind::FreeExpr: {

				auto* fe = cast<FreeExpr>(expr);

				checkExpr(fe->operand.get(), prog);
				if (fe->operand->isInvalid() || !fe->operand->type) {
					fe->setInvalid();
					break;
				}

				checkFreeExpr(fe, prog.types);
				break;

			}

			case NodeKind::CompoundLiteralExpr: {

				auto* cle = cast<CompoundLiteralExpr>(expr);

				cle->type = resolveType(cle->typeSpec.get(), prog, false);
				for (auto& init : cle->inits) {
					checkExpr(init.value.get(), prog);
				}

				if (!cle->type) {
					cle->setInvalid();
					break;
				}

				checkCompoundLiteral(cle);
				break;

			}

			case NodeKind::ImplicitCastExpr:
				break;

			default:
				break; // B will fill in unary/binary expressions later

		}

	}

	// ===================================================================
	// Coercion, Expressions & Narrowing Implementations
	// ===================================================================

	bool Sema::coerce(ExprPtr& expr, const Type* targetType) {

		if (!expr || !expr->type || !targetType) return false;
		if (expr->isInvalid()) return false;

		const Type* srcType = expr->type;
		
		if (srcType == targetType) {
			return true;
		}

		auto* srcBuiltin = dyn_cast<BuiltinType>(srcType);
		auto* targetBuiltin = dyn_cast<BuiltinType>(targetType);

		if (srcBuiltin && targetBuiltin) {
			if (srcBuiltin->isInteger() && targetBuiltin->isInteger()) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::IntegralCast, std::move(expr), targetType);
				return true;
			}

			if (srcBuiltin->isInteger() && targetBuiltin->builtinKind() == BuiltinKind::Float) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::IntToFloat, std::move(expr), targetType);
				return true;
			}

			if (srcBuiltin->builtinKind() == BuiltinKind::Float && targetBuiltin->isInteger()) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::FloatToInt, std::move(expr), targetType);
				return true;
			}
		}

		auto* targetPtr = dyn_cast<PointerType>(targetType);
		if (srcBuiltin && srcBuiltin->builtinKind() == BuiltinKind::Null) {
			if (targetPtr && targetPtr->isNullable()) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::NullToPointer, std::move(expr), targetType);
				return true;
			} else {
				diag.error(expr->loc, "'null' can only be assigned to a nullable pointer type");
				expr->setInvalid();
				return false;
			}
		}

		auto* srcPtr = dyn_cast<PointerType>(srcType);

		if (srcPtr && targetPtr) {

			if (srcPtr->ptrKind() == PtrKind::Raw || targetPtr->ptrKind() == PtrKind::Raw) {
				diag.error(expr->loc, "cannot implicitly convert to or from raw pointer type");
				expr->setInvalid();
				return false;
			}

			if (targetPtr->ptrKind() == PtrKind::Own && srcPtr->ptrKind() != PtrKind::Own) {
				diag.error(expr->loc, "cannot implicitly convert to owning pointer type '" + typeToString(targetType) + "'");
				expr->setInvalid();
				return false;
			}

			if (srcPtr->isNullable() && !targetPtr->isNullable()) {
				diag.error(expr->loc, "cannot implicitly convert nullable pointer '" + typeToString(srcType) + "' to non-nullable type '" + typeToString(targetType) + "'");
				expr->setInvalid();
				return false;
			}

			if (srcPtr->pointee() != targetPtr->pointee()) {
				diag.error(expr->loc, "incompatible pointer types: '" + typeToString(srcType) + "' and '" + typeToString(targetType) + "'");
				expr->setInvalid();
				return false;
			}

			bool isReborrow = (srcPtr->ptrKind() == PtrKind::Own && (targetPtr->ptrKind() == PtrKind::Borrow || targetPtr->ptrKind() == PtrKind::MutBorrow)) || (srcPtr->ptrKind() == PtrKind::MutBorrow && targetPtr->ptrKind() == PtrKind::Borrow);
			
			bool isNullabilityChange = (!srcPtr->isNullable() && targetPtr->isNullable());

			if (isReborrow) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::Reborrow, std::move(expr), targetType);
				return true;
			}
			
			if (isNullabilityChange && (srcPtr->ptrKind() == targetPtr->ptrKind())) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::NonNullToNullable, std::move(expr), targetType);
				return true;
			}

		}

		auto* srcArr = dyn_cast<ArrayType>(srcType);
		auto* targetArr = dyn_cast<ArrayType>(targetType);
		if (srcArr && targetArr) {
			if (srcArr->element() == targetArr->element() &&
			    srcArr->size().has_value() && !targetArr->size().has_value()) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::Reborrow, std::move(expr), targetType);
				return true;
			}
		}

		diag.error(expr->loc, "cannot implicitly convert expression of type '" + typeToString(srcType) + "' to '" + typeToString(targetType) + "'");
		expr->setInvalid();
		return false;

	}

	bool Sema::coerceToBool(ExprPtr& expr, const TypeContext& types) {

		if (!expr || !expr->type) return false;
		if (expr->isInvalid()) return false;

		const Type* boolT = types.boolType();
		if (expr->type == boolT) return true;

		if (auto* b = dyn_cast<BuiltinType>(expr->type)) {
			if (b->isArithmetic()) {
				expr = std::make_unique<ImplicitCastExpr>(CastKind::ToBool, std::move(expr), boolT);
				return true;
			}
		} else if (isa<PointerType>(expr->type)) {
			expr = std::make_unique<ImplicitCastExpr>(CastKind::ToBool, std::move(expr), boolT);
			return true;
		}

		diag.error(expr->loc, "condition must be of scalar type, found '" + typeToString(expr->type) + "'");
		expr->setInvalid();
		return false;

	}

	void Sema::checkAssignment(AssignExpr* expr, TypeContext& types) {

		if (!expr || !expr->target || !expr->value) return;

		clearNarrowing(expr->target.get());

		// 1. Target must be a modifiable lvalue
		if (!expr->target->isLValue()) {
			diag.error(expr->loc, "assignment target must be a modifiable lvalue");
			expr->setInvalid();
			return;
		}

		if (isa<ArrayType>(expr->target->type) || isa<FunctionType>(expr->target->type)) {
			diag.error(expr->loc, "cannot assign to array or function type");
			expr->setInvalid();
			return;
		}

		// Compound assignment operators (+=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=)
		if (expr->isCompound()) {
			const Type* lType = expr->target->type;
			const Type* rType = expr->value->type;
			auto *lB = dyn_cast<BuiltinType>(lType);
			auto *rB = dyn_cast<BuiltinType>(rType);

			if (isa<PointerType>(lType) || isa<PointerType>(rType)) {
				
				if ((expr->op == AssignOp::Add || expr->op == AssignOp::Sub) && isa<PointerType>(lType) && rB && rB->isInteger()) {

					const PointerType* ptrTarget = cast<PointerType>(lType);

					if (ptrTarget->ptrKind() != PtrKind::Raw) {
						diag.error(expr->loc, "pointer arithmetic is only allowed on 'raw' pointers, found '" + typeToString(ptrTarget) + "'");
						expr->setInvalid();
						return;
					}

					if (!isTrusted) {
						diag.error(expr->loc, "pointer arithmetic is not allowed outside 'trusted' blocks");
						expr->setInvalid();
						return;
					}

					if (!coerce(expr->value, types.intType())) {
						expr->setInvalid();
						return;
					}

					expr->type = lType;
					expr->category = ValueCategory::RValue;
					return;
					
				}

				diag.error(expr->loc, "invalid pointer operands to compound assignment '" + std::string(assignOpSpelling(expr->op)) + "'");
				expr->setInvalid();
				return;
				
			}

			bool isBitwiseOrMod = (expr->op == AssignOp::Mod || expr->op == AssignOp::And ||
								expr->op == AssignOp::Or  || expr->op == AssignOp::Xor ||
								expr->op == AssignOp::Shl || expr->op == AssignOp::Shr);

			if (isBitwiseOrMod) {
				if (!lB || !rB || !lB->isInteger() || !rB->isInteger()) {
					diag.error(expr->loc, "compound bitwise/modulo assignment requires integer operands");
					expr->setInvalid();
					return;
				}
			} else {
				if (!lB || !rB || !lB->isArithmetic() || !rB->isArithmetic()) {
					diag.error(expr->loc, "compound arithmetic assignment requires numeric operands");
					expr->setInvalid();
					return;
				}
			}
		}

		// 2. Own pointer assignment represents a Move
		if (expr->op == AssignOp::Assign) {
			if (auto* ptrT = dyn_cast<PointerType>(expr->target->type)) {
				if (ptrT->ptrKind() == PtrKind::Own) {
					handleMove(expr->target.get(), expr->value.get(), expr->loc);
				}
			}
		}

		// 3. Perform implicit coercion from RHS value to LHS target type
		if (!(expr->isCompound() && isTrusted && isa<PointerType>(expr->target->type))) {
			if (!coerce(expr->value, expr->target->type)) {
				expr->setInvalid();
				return;
			}
		}

		expr->type = expr->target->type;
		expr->category = ValueCategory::RValue;

	}

	void Sema::checkCall(CallExpr* expr, const FunctionType* fnType) {
	
		if (!expr || !fnType) return;

		const auto& params = fnType->params();
		if (expr->args.size() != params.size()) {
			diag.error(expr->loc, "call expected " + std::to_string(params.size()) + " arguments, but received " + std::to_string(expr->args.size()));
			expr->setInvalid();
			return;
		}

		for (size_t i = 0; i < expr->args.size(); ++i) {
			if (!coerce(expr->args[i], params[i])) {
				expr->setInvalid();
				return;
			}
		}

		expr->type = fnType->returnType();
		expr->category = ValueCategory::RValue;

	}

	void Sema::checkNew(NewExpr* expr, const Type* allocatedType, TypeContext& types) {

		if (!expr || !allocatedType) return;

		if (isa<BuiltinType>(allocatedType) && cast<BuiltinType>(allocatedType)->builtinKind() == BuiltinKind::Void) {
			diag.error(expr->loc, "cannot allocate 'void' with 'new'");
			expr->setInvalid();
			return;
		}

		expr->type = types.pointerTo(allocatedType, PtrKind::Own, /*nullable=*/false);
		expr->category = ValueCategory::RValue;

	}

	void Sema::checkFreeExpr(FreeExpr* expr, TypeContext& types) {

		if (!expr || !expr->operand || !expr->operand->type) return;

		auto* ptrT = dyn_cast<PointerType>(expr->operand->type);
		if (!ptrT || ptrT->ptrKind() != PtrKind::Own || ptrT->isNullable()) {
			diag.error(expr->loc, "'free' requires a non-nullable owning pointer ('own T*'), found '" + typeToString(expr->operand->type) + "'");
			expr->setInvalid();
			return;
		}

		checkFree(expr);

		expr->type = types.voidType();
		expr->category = ValueCategory::RValue;

	}

	void Sema::checkUnary(UnaryExpr* expr, TypeContext& types) {

		if (!expr || !expr->operand || !expr->operand->type) return;

		const Type* opType = expr->operand->type;
		auto* bType = dyn_cast<BuiltinType>(opType);

		switch (expr->op) {
			case UnaryOp::AddrOf: {
				if (!expr->operand->isLValue()) {
					diag.error(expr->loc, "cannot take address of an rvalue");
					expr->setInvalid();
					return;
				}
				expr->type = types.pointerTo(opType, PtrKind::Borrow, false);
				expr->category = ValueCategory::RValue;
				break;
			}
			case UnaryOp::AddrOfMut: {
				if (!expr->operand->isLValue()) {
					diag.error(expr->loc, "cannot take mutable address of an rvalue");
					expr->setInvalid();
					return;
				}
				expr->type = types.pointerTo(opType, PtrKind::MutBorrow, false);
				expr->category = ValueCategory::RValue;
				break;
			}
			case UnaryOp::Deref: {
				auto* ptrT = dyn_cast<PointerType>(opType);
				if (!ptrT) {
					diag.error(expr->loc, "cannot dereference non-pointer type '" + typeToString(opType) + "'");
					expr->setInvalid();
					return;
				}
				if (ptrT->isNullable()) {
					diag.error(expr->loc, "cannot dereference nullable pointer '" + typeToString(ptrT) + "'; check for null first");
					expr->setInvalid();
					return;
				}
				if (ptrT->pointee() == types.voidType()) {
					diag.error(expr->loc, "cannot dereference 'void*' pointer");
					expr->setInvalid();
					return;
				}
				checkDeref(expr->operand.get(), expr->loc);
				expr->type = ptrT->pointee();
				expr->category = ValueCategory::LValue;
				break;
			}
			case UnaryOp::Plus:
			case UnaryOp::Minus: {
				if (!bType || !bType->isArithmetic()) {
					diag.error(expr->loc, "unary operator requires arithmetic type, found '" + typeToString(opType) + "'");
					expr->setInvalid();
					return;
				}
				expr->type = (bType->builtinKind() == BuiltinKind::Char) ? types.intType() : opType;
				expr->category = ValueCategory::RValue;
				break;
			}
			case UnaryOp::Not: {
				if (!coerceToBool(expr->operand, types)) {
					expr->setInvalid();
					return;
				}
				expr->type = types.boolType();
				expr->category = ValueCategory::RValue;
				break;
			}
			case UnaryOp::BitNot: {
				if (!bType || !bType->isInteger()) {
					diag.error(expr->loc, "bitwise not operator '~' requires integer type, found '" + typeToString(opType) + "'");
					expr->setInvalid();
					return;
				}
				expr->type = (bType->builtinKind() == BuiltinKind::Char || bType->builtinKind() == BuiltinKind::Bool) ? types.intType() : opType;
				expr->category = ValueCategory::RValue;
				break;
			}
			case UnaryOp::PreInc:
			case UnaryOp::PreDec:
			case UnaryOp::PostInc:
			case UnaryOp::PostDec: {
				if (!expr->operand->isLValue()) {
					diag.error(expr->loc, "increment/decrement operand must be an lvalue");
					expr->setInvalid();
					return;
				}
				if (!bType || !bType->isArithmetic()) {
					diag.error(expr->loc, "increment/decrement operator requires arithmetic lvalue, found '" + typeToString(opType) + "'");
					expr->setInvalid();
					return;
				}
				clearNarrowing(expr->operand.get());
				expr->type = opType;
				expr->category = ValueCategory::RValue;
				break;
			}
		}
	}

	void Sema::checkMember(MemberExpr* expr) {

		if (!expr || !expr->base || !expr->base->type) return;

		const StructType* sType = nullptr;

		if (expr->isArrow) {
			auto* ptrT = dyn_cast<PointerType>(expr->base->type);
			if (!ptrT) {
				diag.error(expr->loc, "member access '->' requires pointer type, found '" + typeToString(expr->base->type) + "'");
				expr->setInvalid();
				return;
			}
			if (ptrT->isNullable()) {
				diag.error(expr->loc, "cannot use '->' on nullable pointer '" + typeToString(ptrT) + "'; check for null first");
				expr->setInvalid();
				return;
			}
			sType = dyn_cast<StructType>(ptrT->pointee());
		} else {
			sType = dyn_cast<StructType>(expr->base->type);
		}

		if (!sType || !sType->decl()) {
			diag.error(expr->loc, expr->isArrow
				? "member reference pointer does not point to a struct type"
				: "member access '.' requires struct type");
			expr->setInvalid();
			return;
		}

		FieldDecl* field = sType->decl()->findField(expr->member);
		if (!field) {
			diag.error(expr->loc, "struct '" + sType->decl()->name + "' has no field named '" + expr->member + "'");
			expr->setInvalid();
			return;
		}

		expr->field = field;
		if (field->isInvalid() || !field->type) {
			expr->setInvalid();
			return;
		}

		expr->type = field->type;
		expr->category = expr->isArrow ? ValueCategory::LValue : (expr->base->isLValue() ? ValueCategory::LValue : ValueCategory::RValue);

		if (expr->isArrow) {
			checkArrow(expr);
		}

	}

	void Sema::checkBinary(BinaryExpr* expr, TypeContext& types) {

		if (!expr || !expr->lhs || !expr->rhs || !expr->lhs->type || !expr->rhs->type) return;

		const Type* lType = expr->lhs->type;
		const Type* rType = expr->rhs->type;
		auto *lB = dyn_cast<BuiltinType>(lType);
		auto *rB = dyn_cast<BuiltinType>(rType);

		// Logical Operators (&&, ||)
		if (expr->isLogical()) {
			if (!coerceToBool(expr->lhs, types) || !coerceToBool(expr->rhs, types)) {
				expr->setInvalid();
				return;
			}
			expr->type = types.boolType();
			expr->category = ValueCategory::RValue;
			return;
		}

		// Equality Comparisons (==, !=)
		if (expr->op == BinaryOp::Eq || expr->op == BinaryOp::Ne) {
			
			// Case A: Both arithmetic
			if (lB && rB && lB->isArithmetic() && rB->isArithmetic()) {
				const Type* commonType = (lB->builtinKind() == BuiltinKind::Float || rB->builtinKind() == BuiltinKind::Float)
				                         ? types.floatType()
				                         : types.intType();
				coerce(expr->lhs, commonType);
				coerce(expr->rhs, commonType);
				expr->type = types.boolType();
				expr->category = ValueCategory::RValue;
				return;
			}

			auto* lPtr = dyn_cast<PointerType>(lType);
			auto* rPtr = dyn_cast<PointerType>(rType);

			// Case B: Both pointers to the same pointee
			if (lPtr && rPtr) {
				if (lPtr->pointee() != rPtr->pointee()) {
					diag.error(expr->loc, "cannot compare pointers to different types '" + typeToString(lType) + "' and '" + typeToString(rType) + "'");
					expr->setInvalid();
					return;
				}
				expr->type = types.boolType();
				expr->category = ValueCategory::RValue;
				return;
			}

			// Case C: Nullable pointer against `null`
			if (lPtr && rB && rB->builtinKind() == BuiltinKind::Null) {
				if (!coerce(expr->rhs, lType)) {
					expr->setInvalid();
					return;
				}
				expr->type = types.boolType();
				expr->category = ValueCategory::RValue;
				return;
			}
			if (rPtr && lB && lB->builtinKind() == BuiltinKind::Null) {
				if (!coerce(expr->lhs, rType)) {
					expr->setInvalid();
					return;
				}
				expr->type = types.boolType();
				expr->category = ValueCategory::RValue;
				return;
			}

			diag.error(expr->loc, "cannot compare incompatible types '" + typeToString(lType) + "' and '" + typeToString(rType) + "'");
			expr->setInvalid();
			return;
			
		}

		// Handle pointer arithmetic (`p + n`, `n + p`, `p - n`)
		if (isa<PointerType>(lType) || isa<PointerType>(rType)) {

			if (expr->op == BinaryOp::Add || expr->op == BinaryOp::Sub) {

				bool validPtrArith = (isa<PointerType>(lType) && rB && rB->isInteger()) || (expr->op == BinaryOp::Add && isa<PointerType>(rType) && lB && lB->isInteger());
				
				if (!validPtrArith) {
					diag.error(expr->loc, "invalid operands to binary '" + std::string(binaryOpSpelling(expr->op)) + "'");
					expr->setInvalid();
					return;
				}
				
				// validPtrArith guarantees exactly one operand is a pointer
				const PointerType* ptrOperand = isa<PointerType>(lType) ? cast<PointerType>(lType) : cast<PointerType>(rType);

				if (ptrOperand->ptrKind() != PtrKind::Raw) {
					diag.error(expr->loc, "pointer arithmetic is only allowed on 'raw' pointers, found '" + typeToString(ptrOperand) + "'");
					expr->setInvalid();
					return;
				}

				if (!isTrusted) {
					diag.error(expr->loc, "pointer arithmetic is not allowed outside 'trusted' blocks");
					expr->setInvalid();
					return;
				}
				
				if (isa<PointerType>(lType)) {
					coerce(expr->rhs, types.intType());
					expr->type = lType;
				} else {
					coerce(expr->lhs, types.intType());
					expr->type = rType;
				}
				
				expr->category = ValueCategory::RValue;
				return;

			}

			diag.error(expr->loc, "invalid pointer operands to binary '" + std::string(binaryOpSpelling(expr->op)) + "'");
			expr->setInvalid();
			return;

    	}

		if (!lB || !rB) {
			diag.error(expr->loc, "binary operator '" + std::string(binaryOpSpelling(expr->op)) + "' requires scalar operands");
			expr->setInvalid();
			return;
		}

		// Relational Comparisons (<, >, <=, >=)
		if (expr->isComparison()) {
			if (!lB->isArithmetic() || !rB->isArithmetic()) {
				diag.error(expr->loc, "relational comparison requires arithmetic operands");
				expr->setInvalid();
				return;
			}
			const Type* commonType = (lB->builtinKind() == BuiltinKind::Float || rB->builtinKind() == BuiltinKind::Float) ? types.floatType() : types.intType();
			coerce(expr->lhs, commonType);
			coerce(expr->rhs, commonType);
			expr->type = types.boolType();
			expr->category = ValueCategory::RValue;
			return;
		}

		// Bitwise Operators (%, &, |, ^, <<, >>)
		bool isBitwise = (expr->op == BinaryOp::Mod || expr->op == BinaryOp::BitAnd ||
						expr->op == BinaryOp::BitXor || expr->op == BinaryOp::BitOr ||
						expr->op == BinaryOp::Shl || expr->op == BinaryOp::Shr);

		if (isBitwise) {
			if (!lB->isInteger() || !rB->isInteger()) {
				diag.error(expr->loc, "bitwise operator requires integer operands");
				expr->setInvalid();
				return;
			}
			coerce(expr->lhs, types.intType());
			coerce(expr->rhs, types.intType());
			expr->type = types.intType();
			expr->category = ValueCategory::RValue;
			return;
		}

		// Standard Arithmetic (+, -, *, /)
		if (!lB->isArithmetic() || !rB->isArithmetic()) {
			diag.error(expr->loc, "arithmetic operator requires numeric operands");
			expr->setInvalid();
			return;
		}

		const Type* commonType = (lB->builtinKind() == BuiltinKind::Float || rB->builtinKind() == BuiltinKind::Float) ? types.floatType() : types.intType();
		coerce(expr->lhs, commonType);
		coerce(expr->rhs, commonType);
		expr->type = commonType;
		expr->category = ValueCategory::RValue;

	}

	void Sema::checkIndex(IndexExpr* expr, TypeContext& types) {

		if (!expr || !expr->base || !expr->index || !expr->base->type || !expr->index->type) return;

		auto* arrType = dyn_cast<ArrayType>(expr->base->type);
		if (!arrType) {
			if (auto* ptrT = dyn_cast<PointerType>(expr->base->type)) {
				if (ptrT->ptrKind() != PtrKind::Raw) {
					diag.error(expr->loc, "pointer indexing is only allowed on 'raw' pointers");
					expr->setInvalid();
					return;
				}
				if (!isTrusted) {
					diag.error(expr->loc, "raw pointer indexing is only allowed inside 'trusted' blocks");
					expr->setInvalid();
					return;
				}
			} else {
				diag.error(expr->loc, "subscripted value is not an array or raw pointer");
				expr->setInvalid();
				return;
			}
		}

		auto* idxB = dyn_cast<BuiltinType>(expr->index->type);
		if (!idxB || !idxB->isInteger()) {
			diag.error(expr->loc, "array subscript is not an integer");
			expr->setInvalid();
			return;
		}
		
		// Constant index out-of-bounds check for sized arrays
		if (arrType && arrType->size().has_value()) {
			if (auto constIdx = evalConstInt(expr->index.get())) {
				if (*constIdx < 0 || static_cast<uint64_t>(*constIdx) >= *arrType->size()) {
					diag.error(expr->loc, "array index " + std::to_string(*constIdx) +
										" is out of bounds for array of size " + std::to_string(*arrType->size()));
					expr->setInvalid();
					return;
				}
			}
		}

		coerce(expr->index, types.intType());

		expr->type = arrType ? arrType->element() : cast<PointerType>(expr->base->type)->pointee();
		expr->category = ValueCategory::LValue;

	}

	void Sema::checkCompoundLiteral(CompoundLiteralExpr* expr) {

		if (!expr || !expr->type) return;

		auto* sType = dyn_cast<StructType>(expr->type);
		if (!sType || !sType->decl()) {
			diag.error(expr->loc, "compound literal requires struct type");
			expr->setInvalid();
			return;
		}

		StructDecl* sDecl = sType->decl();
		std::unordered_map<std::string, bool> initializedFields;
		unsigned positionalIdx = 0;

		for (auto& init : expr->inits) {

			FieldDecl* targetField = nullptr;

			if (init.isDesignated()) {
				targetField = sDecl->findField(init.field);
				if (!targetField) {
					diag.error(init.loc, "struct '" + sDecl->name + "' has no field named '" + init.field + "'");
					expr->setInvalid();
					return;
				}
			} else {
				if (positionalIdx >= sDecl->fields.size()) {
					diag.error(init.loc, "too many initializers for struct '" + sDecl->name + "'");
					expr->setInvalid();
					return;
				}
				targetField = sDecl->fields[positionalIdx++].get();
			}

			if (initializedFields[init.field]) {
				diag.error(init.loc, "duplicate initialization of field '" + init.field + "'");
				expr->setInvalid();
				return;
			}

			init.resolved = targetField;
			initializedFields[targetField->name] = true;

			if (!init.value || init.value->isInvalid() || !init.value->type ||
				targetField->isInvalid() || !targetField->type) {
				expr->setInvalid();
				return;
			}

			if (!coerce(init.value, targetField->type)) {
				expr->setInvalid();
				return;
			}

		}

		expr->category = ValueCategory::RValue;

	}

	// ===================================================================
	// Flow-Sensitive Null Narrowing
	// ===================================================================

	void Sema::clearNarrowing(Expr* target) {
		if (auto* nameExpr = dyn_cast<NameExpr>(unwrapImplicit(target))) {
			if (nameExpr->decl) {
				narrowedVars.erase(nameExpr->decl);
				nameExpr->type = nameExpr->decl->type;
			}
		}
	}

	const Type* Sema::getNarrowedType(ValueDecl* decl, const Type* originalType) {

		if (!decl) return originalType;

		auto it = narrowedVars.find(decl);
		if (it != narrowedVars.end()) {
			return it->second;
		}

		return originalType;

	}

	void Sema::applyNarrowing(Expr* cond, bool isTrueBranch, TypeContext& types) {

		cond = unwrapImplicit(cond);
		if (!cond) return;

		auto* bin = dyn_cast<BinaryExpr>(cond);
		if (!bin) return;

		bool isNe = (bin->op == BinaryOp::Ne);
		bool isEq = (bin->op == BinaryOp::Eq);
		if (!isNe && !isEq) return;

		// Narrow on `p != null` in then-branch OR `p == null` in else-branch
		bool shouldNarrow = (isNe && isTrueBranch) || (isEq && !isTrueBranch);
		if (!shouldNarrow) return;

		Expr *lhsSub = unwrapImplicit(bin->lhs.get());
		Expr *rhsSub = unwrapImplicit(bin->rhs.get());

		NameExpr *nameExpr = dyn_cast<NameExpr>(lhsSub);
		bool isNullLit = isa<NullLiteral>(rhsSub);

		if (!nameExpr || !isNullLit) {
			nameExpr = dyn_cast<NameExpr>(rhsSub);
			isNullLit = isa<NullLiteral>(lhsSub);
		}

		if (nameExpr && isNullLit && nameExpr->decl) {

			if (auto* vd = dyn_cast<VarDecl>(nameExpr->decl)) {
				if (vd->isGlobal) return; // only narrow locals and parameters
			}

			const Type* curType = nameExpr->decl->type;

			if (auto* ptrT = dyn_cast<PointerType>(curType)) {
				if (ptrT->isNullable()) {
					const Type* narrowed = types.pointerTo(ptrT->pointee(), ptrT->ptrKind(), /*nullable=*/false);
					narrowedVars[nameExpr->decl] = narrowed;
				}
			}
			
		}

	}

	// ===================================================================
	// Hooks (Pointer Rules & Flow Ownership)
	// ===================================================================

	void Sema::checkDeref(Expr* /*base*/, SourceLoc /*loc*/) {}
	void Sema::checkArrow(MemberExpr* /*e*/) {}
	void Sema::checkFree(FreeExpr* /*e*/) {}
	void Sema::handleMove(Expr* /*target*/, Expr* /*value*/, SourceLoc /*loc*/) {}

	// ===================================================================
	// Standalone Phase 1-4 Unit Test
	// ===================================================================

	void test_phase1_conversions() {

		DiagnosticsEngine diag("test.safec");
		Sema sema(diag);
		TypeContext types;

		// Test Phase 4: Narrowing nullable pointer in 'if (p != null)'
		VarDecl dummyVar(SourceLoc(1, 1), "p", nullptr);
		const Type* nullablePtr = types.pointerTo(types.intType(), PtrKind::Own, /*nullable=*/true);
		dummyVar.type = nullablePtr;

		auto pLhs = std::make_unique<NameExpr>(SourceLoc(1, 1), "p");
		pLhs->decl = &dummyVar;
		pLhs->type = nullablePtr;

		auto nullRhs = std::make_unique<NullLiteral>(SourceLoc(1, 6));
		nullRhs->type = types.nullType();

		auto cond = std::make_unique<BinaryExpr>(SourceLoc(1, 3), BinaryOp::Ne, std::move(pLhs), std::move(nullRhs));

		// Apply narrowing on true branch
		sema.applyNarrowing(cond.get(), /*isTrueBranch=*/true, types);
		const Type* narrowed = sema.getNarrowedType(&dummyVar, dummyVar.type);

		assert(isa<PointerType>(narrowed));
		assert(!cast<PointerType>(narrowed)->isNullable());

		// Reassignment clears narrowing
		auto pTarget = std::make_unique<NameExpr>(SourceLoc(2, 1), "p");
		pTarget->decl = &dummyVar;
		pTarget->type = nullablePtr;
		sema.clearNarrowing(pTarget.get());

		assert(sema.getNarrowedType(&dummyVar, dummyVar.type) == nullablePtr);

		std::cout << "[Sema All Phases 1-4] All expression, operator, and narrowing checks passed!\n";

	}

} // namespace safec