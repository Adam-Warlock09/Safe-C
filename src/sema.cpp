#include "sema.h"
#include "new_ast.h"

namespace safec {

	void Sema::run(Program& prog) {
		
		// Pass 1 : Global Collection
		// collectGlobals(prog);

		// Pass 2 : Struct Fields
		// resolveStructFields(prog);

		// Pass 3 : Signatures
		// resolveSignatures(prog);

		// Pass 4 : Bodies
		// checkBodies(prog);

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
				currentType = it->second;
				break;

			}
		
		}

		// 2. Validate Qualifiers (own/mut/raw require a pointer)
		if (spec->qualifier != PtrQualifier::None && spec->pointers.empty()) {
			diag.error(spec->loc, "Pointer qualifier applied to non-pointer type");
			spec->setInvalid();
		}

		// 3. Build Pointer Levels (pointers[0] is the innermost, apply qualifiers to the outermost)
		for (size_t i = 0; i < spec->pointers.size(); ++i) {
			bool isOutermost = (i == spec->pointers.size() - 1);
			PtrKind pk = PtrKind::Borrow;

			if (outermost) {
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
					if (intList->value == 0) {
						diag.error(dimExpr->loc, "Array size must be greater than zero");
						spec->setInvalid();
					} else {
						size = intLit->value;
					}
				} else {
					diag.error(dimExpr->loc, "Array size must be a constant integer");
					spec->setInvalid();
				}

			}

			// Void arrays are always illegal, regardless of context
			if (currentType->kind() == TypeKind::Builtin && cast<BuiltinType>(currentType)->builtinKind() == BuiltinKind::Void) {
				diag.error(spec->loc, "Array element type cannot be void");
				spec->setInvalid();
			}

			currentType = types.arrayOf(currentType, size);
 
		}

		spec->resolved = currentType;
		return currentType;

	}

} // namespace safec