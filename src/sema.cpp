#include "sema.h"
#include "new_ast.h"

namespace safec {

	void Sema::run(Program& prog) {
		
		// Pass 1 : Global Collection
		collectGlobals(prog);

		// Pass 2 : Struct Fields
		resolveStructFields(prog);

		// Pass 3 : Signatures
		resolveSignatures(prog);

		// Pass 4 : Bodies
		checkBodies(prog);

	}

	const Type* Sema::resolveType(TypeSpec* spec, Program& prog) {

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
					diag.error(spec->loc, "Unknown struct type: " + spec->structName + " ");
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

		// 4. Build Array Dimensions (Evaluate outermost-first to Innermost)
		for (auto it = spec->arrayDims.rbegin(); it != spec->arrayDims.rend(); ++it) {
			Expr* dimExpr = it->get();
			std::optional<uint64_t> size = std::nullopt;

			if (dimExpr) {
				
				if (auto* intLit = dyn_cast<IntLiteral>(dimExpr)) {
					if (intLit->value == 0) {
						diag.error(dimExpr->loc, "Array size must be greater than zero");
						spec->setInvalid();
						dimExpr->setInvalid();
						return nullptr;
					} else {
						size = intLit->value;
					}
				} else {
					diag.error(dimExpr->loc, "Array size must be a constant integer");
					spec->setInvalid();
					dimExpr->setInvalid();
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
				if (!structs.emplace(sd->name, sd).second) {
					diag.error(sd->loc, "Duplicate struct name: '" + sd->name + "'");;
					sd->setInvalid();
				}
			} else if (auto* fd = dyn_cast<FunctionDecl>(decl.get())) {
				if (!globalNames.emplace(fd->name, fd).second) {
					diag.error(fd->loc, "Duplicate global name: '" + fd->name + "'");
					fd->setInvalid();
				} else {
					funs[fd->name] = fd;
				}
			} else if (auto* vd = dyn_cast<VarDecl>(decl.get())) {
				if (!globalNames.emplace(vd->name, vd).second) {
					diag.error(vd->loc, "Duplicate global name: '" + vd->name + "'");
					vd->setInvalid();
				} else {
					scope.declare(vd);
				}
			}
		}

	}

	void Sema::resolveStructFields(Program& prog) {

		for (auto& decl : prog.decls) {

			if (auto* sd = dyn_cast<StructDecl>(decl.get())) {

				if (sd->isInvalid()) continue;

				std::unordered_map<std::string_view, FieldDecl*> fieldNames;

				for (auto& field : sd->fields) {

					if (!fieldNames.emplace(field->name, field.get()).second) {
						diag.error(field->loc, "Duplicate field name: '" + field->name + "'");
						field->setInvalid();
						continue;
					}

					field->type = resolveType(field->typeSpec.get(), prog);

					// Reject void fields
					if (field->type && field->type->kind() == TypeKind::Builtin) {
						if (cast<BuiltinType>(field->type)->builtinKind() == BuiltinKind::Void) {
							diag.error(field->loc, "Field type cannot be void");
							field->setInvalid();
						}
					}

					// Check for recursive containment (struct containing itself by value)
					if (field->type && field->type->kind() == TypeKind::Struct) {
						if (cast<StructType>(field->type)->decl() == sd) {
							diag.error(field->loc, "Struct cannot contain itself by value; Use a pointer instead");
							field->setInvalid();
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
				const Type* retType = resolveType(fd->returnSpec.get(), prog);

				// Resolve parameter types
				std::vector<const Type*> paramTypes;
				std::unordered_map<std::string_view, ParamDecl*> paramNames;

				for (auto& param : fd->params) {

					if (!paramNames.emplace(param->name, param.get()).second) {
						diag.error(param->loc, "Duplicate parameter name: '" + param->name + "'");
						param->setInvalid();
						continue;
					}

					const Type* pType = resolveType(param->typeSpec.get(), prog);
					param->type = pType;


					if (pType && pType->kind() == TypeKind::Builtin && cast<BuiltinType>(pType)->builtinKind() == BuiltinKind::Void) {
						diag.error(param->loc, "parameter cannot have void type");
						param->setInvalid();
						continue;
					}

					if (pType) paramTypes.push_back(pType);

				}

				// Verify main() signature constraints
				if (fd->name == "main") {
					if (!retType || retType->kind() != TypeKind::Builtin || cast<BuiltinType>(retType)->builtinKind() != BuiltinKind::Int) {
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
				}

			} else if (auto* vd = dyn_cast<VarDecl>(decl.get())) {

				if (vd->isInvalid()) continue;

				// Resolve variable type
				const Type* varType = resolveType(vd->typeSpec.get(), prog);
				vd->type = varType;

				if (varType && varType->kind() == TypeKind::Builtin && cast<BuiltinType>(varType)->builtinKind() == BuiltinKind::Void) {
					diag.error(vd->loc, "Variable cannot have void type");
					vd->setInvalid();
					vd->type = nullptr;
				}

				// Unsized arrays '[]' are only permitted on parameters
				if (vd->type && isa<ArrayType>(vd->type)) {
					const Type* t = vd->type;
					while (auto* at = dyn_cast<ArrayType>(t)) {
						if (!at->size().has_value()) {
							diag.error(vd->loc, "Unsized arrays '[]' are only allowed on function parameters");
							vd->setInvalid();
							break;
						}
						t = at->element();
					}
				}

			}

		}

	}

	void Sema::checkBodies(Program& prog) {

		for (auto& decl : prog.decls) {

			if (auto* fd = dyn_cast<FunctionDecl>(decl.get())) {

				if (fd->isInvalid() || !fd->body) continue;

				currFun = fd;
				scope.push(); // Open function-level scope

				// Declare parameters in the function scope
				for (auto& param : fd->params) {
					scope.declare(param.get());
				}

				checkStmt(fd->body.get(), prog);

				// Simple fall-off check : Warning if non-void function doesn't end with a return statement
				const Type* retType = cast<FunctionType>(fd->type)->returnType();
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

					vd->type = resolveType(vd->typeSpec.get(), prog);

					if (vd->type && vd->type->kind() == TypeKind::Builtin && cast<BuiltinType>(vd->type)->builtinKind() == BuiltinKind::Void) {
						diag.error(vd->loc, "Local variable cannot have void type");
						vd->setInvalid();
						vd->type = nullptr;
					}

					if (vd->type && isa<ArrayType>(vd->type)) {

						const Type *t = vd->type;

						while (auto *at = dyn_cast<ArrayType>(t)) {
							if (!at->size().has_value()) {
								diag.error(vd->loc, "Unsized arrays '[]' are only allowed on function parameters");
								vd->setInvalid();
								break;
							}
							t = at->element();
						}

					}

					if (vd->init) {
						checkExpr(vd->init.get(), prog);
						// B's conversion mechanism will verify init converts to vd->type here
					}

					// Declare *after* checking init so `int x = x;` fails properly
					if (!scope.declare(vd.get())) {
						diag.error(vd->loc, "Duplicate local variable '" + vd->name + "'" );
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
				checkExpr(is->cond.get(), prog);
				// B's coonversion mechanism will wrap condition in ToBool here
				checkStmt(is->thenStmt.get(), prog);
				if (is->elseStmt) checkStmt(is->elseStmt.get(), prog);
				break;

			}

			case NodeKind::WhileStmt: {
				
				auto* ws = cast<WhileStmt>(stmt);
				checkExpr(ws->cond.get(), prog);
				checkStmt(ws->body.get(), prog);
				break;

			}

			case NodeKind::ReturnStmt: {

				auto* rs = cast<ReturnStmt>(stmt);
				const Type *retType = cast<FunctionType>(currFun->type)->returnType();
				bool isVoidFunc = (retType == prog.types.voidType());

				if (rs->value) {
					if (isVoidFunc) {
						diag.error(rs->loc, "Cannot return a value from a void function");
						rs->setInvalid();
					} else {
						checkExpr(rs->value.get(), prog);
						// B's conversion mechanism will verify return value converts to currFun->returnSpec here
					}
				} else if (!isVoidFunc) {
					diag.error(rs->loc, "Non-void function must return a value");
					rs->setInvalid();
				}

				break;

			}

			default:
				break; // For, Switch, Break, Continue, Defer, Trusted, etc are not implemented yet

		}

	}

	void Sema::checkExpr(Expr* expr, Program& prog) {

		if (!expr || expr->isInvalid()) return;

		switch(expr->kind()) {

			case NodeKind::NameExpr: {

				auto* ne = cast<NameExpr>(expr);

				// Look up in local scope first, then global functions
				if (VarDecl* local = scope.lookup(ne->name)) {
					ne->decl = local;
					ne->type = local->type;
					ne->category = ValueCategory::LValue;
				} else if (auto it = funs.find(ne->name); it != funs.end()) {
					ne->decl = it->second;
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
				// Route to B's Hook
				checkCall(cast<CallExpr>(expr));
				break;
			}

			case NodeKind::MemberExpr: {
				auto* me = cast<MemberExpr>(expr);
				checkExpr(me->base.get(), prog);
				// Route to C's Hook
				checkArrow(me);
				break;
			}

			default:
				break; // B will fill in unary/binary expressions later

		}

	}

} // namespace safec