#include "sema.h"
#include "new_ast.h"
#include <algorithm>
#include <climits>
#include <unordered_set>

namespace safec {

	namespace {

		// Evaluates compile-time integer constants (for array sizes and switch cases)
		std::optional<int64_t> evalConstInt(const Expr* expr) {

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

			return std::nullopt; // Not a constant integer expression

		}

		// Checks whether a global initializer expression is a compile-time constant
		bool isGlobalConstant(const Expr* expr) {

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
					return isa<FloatLiteral>(ue->operand.get());
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

	} // namespace

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

					if (pType && pType->kind() == TypeKind::Builtin && cast<BuiltinType>(pType)->builtinKind() == BuiltinKind::Void) {
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
					if (!paramTypes.empty()) {
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

				if (vd->init) {

					checkExpr(vd->init.get(), prog);
					if (vd->isGlobal && !vd->init->isInvalid() && !isGlobalConstant(vd->init.get())) {
						diag.error(vd->init->loc, "Global initializers must be constants");
						vd->init->setInvalid();
					}

				}

			}

		}

	}

	void Sema::checkBodies(Program& prog) {

		for (auto& decl : prog.decls) {

			if (auto* fd = dyn_cast<FunctionDecl>(decl.get())) {

				if (fd->isInvalid() || !fd->body) continue;

				const Type* retType = fd->returnSpec ? fd->returnSpec->resolved : nullptr;
				if (!retType) {
					continue;
				}

				currFun = fd;
				scope.push(); // Open function-level scope

				// Declare parameters in the function scope
				for (auto& param : fd->params) {
					scope.declare(param.get());
				}

				checkStmt(fd->body.get(), prog);

				// Simple fall-off check : Warning if non-void function doesn't end with a return statement
				if (retType != prog.types.voidType()) {
					if (fd->body->body.empty() || !isa<ReturnStmt>(fd->body->body.back().get())) {
						diag.warning(fd->loc, "Non-void function may exit without returning a value");
					}
				}

				scope.pop(); // Close function-level scope
				currFun = nullptr;

			}

		}

	}

	void Sema::checkCondition(ExprPtr& cond, SourceLoc loc, Program& prog) {
		
		if (!cond) return;

		checkExpr(cond.get(), prog);
		if (cond->isInvalid() || !cond->type) return;

		const Type* t = cond->type;
		bool isScalar = false;

		if (auto* bt = dyn_cast<BuiltinType>(t)) {
			isScalar = bt->isArithmetic();
		} else if (isa<PointerType>(t)) {
			isScalar = true;
		}

		if (!isScalar) {
			diag.error(loc, "Condition must be a scalar type");
			cond->setInvalid();
			return;
		}

		if (t != prog.types.boolType()) {
			// Implicit conversion to bool
			cond = std::make_unique<ImplicitCastExpr>(CastKind::ToBool, std::move(cond), prog.types.boolType());
		}
		
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
					}else if (varType->kind() == TypeKind::Builtin && cast<BuiltinType>(varType)->builtinKind() == BuiltinKind::Void) {
						diag.error(vd->loc, "Local variable cannot have void type");
						vd->setInvalid();
						varType = nullptr;
					}

					vd->type = varType;

					if (vd->init) {
						checkExpr(vd->init.get(), prog);
						// B's conversion mechanism will verify init converts to vd->type here
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
				checkStmt(is->thenStmt.get(), prog);
				if (is->elseStmt) checkStmt(is->elseStmt.get(), prog);
				break;

			}

			case NodeKind::WhileStmt: {
				
				auto* ws = cast<WhileStmt>(stmt);
				checkCondition(ws->cond, ws->cond->loc, prog);

				breakStack.push_back(ws);
				continueStack.push_back(ws);

				checkStmt(ws->body.get(), prog);

				breakStack.pop_back();
				continueStack.pop_back();
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
					} 
					// B's conversion mechanism will verify return value converts to currFun->returnSpec here
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
				if (fs->cond) checkCondition(fs->cond, fs->cond->loc, prog);
				if (fs->step) checkExpr(fs->step.get(), prog);

				breakStack.push_back(fs);
				continueStack.push_back(fs);

				checkStmt(fs->body.get(), prog);

				breakStack.pop_back();
				continueStack.pop_back();
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
					ne->type = local->type;
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

			case NodeKind::AssignExpr: {
				// Route to B's Hook
				checkAssignment(cast<AssignExpr>(expr));
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

				ce->type = cast<FunctionType>(ce->callee->type)->returnType();
				ce->category = ValueCategory::RValue;

				// Route to B's Hook
				checkCall(cast<CallExpr>(expr));
				break;

			}

			case NodeKind::MemberExpr: {

				auto* me = cast<MemberExpr>(expr);
				checkExpr(me->base.get(), prog);

				if (me->base->isInvalid() || !me->base->type) {
					me->setInvalid(); // Propagate invalid silently
					break;
				}

				const Type *baseType = me->base->type;

				if (me->isArrow) {
					if (auto* pt = dyn_cast<PointerType>(baseType)) {
						baseType = pt->pointee();
					} else {
						diag.error(me->base->loc, "Arrow operator '->' used on non-pointer type");
						me->setInvalid();
						break;
					}
				}

				if (auto* st = dyn_cast<StructType>(baseType)) {
					if (FieldDecl* fd = st->decl()->findField(me->member)) {
						me->field = fd;
						if (fd->isInvalid() || !fd->type) {
							me->setInvalid();
							break;
						}
						me->type = fd->type;
						me->category = ValueCategory::LValue; // Struct fields are lvalues
					} else {
						diag.error(me->loc, "Unknown field '" + me->member + "' in struct '" + st->decl()->name + "'");
						me->setInvalid();
						break;
					}
				} else {
					diag.error(me->base->loc, me->isArrow
						? "Member reference pointer does not point to a struct type"
						: "Base of '.' operator is not a struct type");
					me->setInvalid();
					break;
				}

				// Route to C's Hook
				if (me->isArrow) {
					checkArrow(me);
				}
				break;

			}

			default:
				break; // B will fill in unary/binary expressions later

		}

	}

	// Temporary stubs for Person B and Person C hooks (remove once implemented)
	void Sema::checkAssignment(AssignExpr * /*expr*/) {}
	void Sema::checkCall(CallExpr * /*expr*/) {}
	void Sema::checkDeref(Expr * /*base*/, SourceLoc /*loc*/) {}
	void Sema::checkArrow(MemberExpr * /*e*/) {}
	void Sema::checkFree(FreeExpr * /*e*/) {}
	void Sema::handleMove(Expr * /*target*/, Expr * /*value*/, SourceLoc /*loc*/) {}

} // namespace safec