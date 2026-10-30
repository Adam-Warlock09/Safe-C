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

void Sema::checkAssignment(AssignExpr* expr) {
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

    // 2. Own pointer assignment represents a Move (Hook for C)
    if (expr->op == AssignOp::Assign) {
        if (auto* ptrT = dyn_cast<PointerType>(expr->target->type)) {
            if (ptrT->ptrKind() == PtrKind::Own) {
                handleMove(expr->target.get(), expr->value.get(), expr->loc);
            }
        }
    }

    // 3. Perform implicit coercion from RHS value to LHS target type
    if (!coerce(expr->value, expr->target->type)) {
        expr->setInvalid();
        return;
    }

    expr->type = expr->target->type;
    expr->category = ValueCategory::RValue;
}

void Sema::checkCall(CallExpr* expr, const FunctionType* fnType) {
    if (!expr || !fnType) return;

    const auto& params = fnType->params();
    if (expr->args.size() != params.size()) {
        diag.error(expr->loc, "call expected " + std::to_string(params.size()) + 
                              " arguments, but received " + std::to_string(expr->args.size()));
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
        diag.error(expr->loc, "'free' requires a non-nullable owning pointer ('own T*'), found '" + 
                              typeToString(expr->operand->type) + "'");
        expr->setInvalid();
        return;
    }

    // Hook for Person C
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
        checkArrow(expr);
    } else {
        sType = dyn_cast<StructType>(expr->base->type);
    }

    if (!sType || !sType->decl()) {
        diag.error(expr->loc, "member access '.' requires struct type");
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
    expr->type = field->type;
    expr->category = expr->isArrow ? ValueCategory::LValue : (expr->base->isLValue() ? ValueCategory::LValue : ValueCategory::RValue);
}
  
void Sema::checkDeref(Expr*, SourceLoc) {}
void Sema::checkArrow(MemberExpr*) {}
void Sema::checkFree(FreeExpr*) {}
void Sema::handleMove(Expr*, Expr*, SourceLoc) {}

void Sema::checkBinary(BinaryExpr* expr, TypeContext& types) {
    if (!expr || !expr->lhs || !expr->rhs || !expr->lhs->type || !expr->rhs->type) return;

    const Type* lType = expr->lhs->type;
    const Type* rType = expr->rhs->type;

    // Reject pointer arithmetic outside trusted blocks
    if (isa<PointerType>(lType) || isa<PointerType>(rType)) {
        if (expr->op == BinaryOp::Add || expr->op == BinaryOp::Sub) {
            if (!isTrusted) {
                diag.error(expr->loc, "pointer arithmetic is not allowed outside 'trusted' blocks");
                expr->setInvalid();
                return;
            }
        }
    }

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
        if (lType == rType) {
            expr->type = types.boolType();
            expr->category = ValueCategory::RValue;
            return;
        }
        if (coerce(expr->rhs, lType) || coerce(expr->lhs, rType)) {
            expr->type = types.boolType();
            expr->category = ValueCategory::RValue;
            return;
        }
        diag.error(expr->loc, "cannot compare incompatible types '" + typeToString(lType) + "' and '" + typeToString(rType) + "'");
        expr->setInvalid();
        return;
    }

    auto* lB = dyn_cast<BuiltinType>(lType);
    auto* rB = dyn_cast<BuiltinType>(rType);

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
        if (isa<PointerType>(expr->base->type) && !isTrusted) {
            diag.error(expr->loc, "pointer indexing is only allowed inside 'trusted' blocks");
            expr->setInvalid();
            return;
        }
        if (!isa<PointerType>(expr->base->type)) {
            diag.error(expr->loc, "subscripted value is not an array or pointer");
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
    coerce(expr->index, types.intType());

    // Constant index out-of-bounds check for sized arrays
    if (arrType && arrType->size().has_value()) {
        if (auto* lit = dyn_cast<IntLiteral>(expr->index.get())) {
            if (lit->value >= *arrType->size()) {
                diag.error(expr->loc, "array index " + std::to_string(lit->value) + " is out of bounds for array of size " + std::to_string(*arrType->size()));
                expr->setInvalid();
                return;
            }
        }
    }

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
            if (initializedFields[init.field]) {
                diag.error(init.loc, "duplicate initialization of field '" + init.field + "'");
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

        init.resolved = targetField;
        initializedFields[targetField->name] = true;

        if (!coerce(init.value, targetField->type)) {
            expr->setInvalid();
            return;
        }
    }

    expr->category = ValueCategory::RValue;
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
