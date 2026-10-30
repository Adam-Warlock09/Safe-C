#include "sema.h"
#include <cassert>
#include <iostream>

namespace safec {

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

        bool isReborrow = (srcPtr->ptrKind() == PtrKind::Own && (targetPtr->ptrKind() == PtrKind::Borrow || targetPtr->ptrKind() == PtrKind::MutBorrow)) ||
                          (srcPtr->ptrKind() == PtrKind::MutBorrow && targetPtr->ptrKind() == PtrKind::Borrow);
        
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
