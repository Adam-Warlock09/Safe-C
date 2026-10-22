/* Safe-C grammar for bison, C++ skeleton.
   Token numbers are synced with src/tokens.h (EOF_TOKEN = 0 is
   bison's $end).  To re-sync after editing tokens.h, regenerate the
   %token block - see README.md.
   One language change from the whitepaper: struct types are always
   written with the 'struct' keyword (`struct List x`), which makes
   this grammar conflict-free for LALR(1).
   The actions build the AST from src/new_ast.h; the driver and the
   scanner bridge live in src/main_parser.cpp. */

%skeleton "lalr1.cc"
%define api.namespace {safec}
%define api.parser.class {SafeCParser}
%define parse.error verbose
%locations

%code requires {
#include "new_ast.h"
namespace safec { struct DeclParts; }
}

%{
#include <memory>
#include <string>
#include <vector>

#include "new_ast.h"

namespace safec {

// one declarator without its base type; the decl/field rules assemble
// one TypeSpec per declarator from these
struct DeclParts {
    SourceLoc nameLoc;
    std::string name;
    std::vector<PointerLevel> pointers;
    std::vector<ExprPtr> dims;            // nullptr = `[]`
    std::unique_ptr<Expr> init;           // may be null
};

// no columns from the lexer yet, so col stays 0
template <class Loc>
static SourceLoc SL(const Loc &l) { return SourceLoc(l.begin.line, 0); }

// TypeSpec isn't copyable, so clone the base for `int a, *b;`
static TypeSpec *cloneBase(const TypeSpec *b) {
    TypeSpec *t = new TypeSpec(b->loc, b->base, b->structName);
    t->qualifier = b->qualifier;
    return t;
}

// attach a declarator's ptrs/dims to its base type
static TypeSpec *attachParts(TypeSpec *base, DeclParts *p) {
    base->pointers = std::move(p->pointers);
    base->arrayDims = std::move(p->dims);
    return base;
}

// blocks and for-inits wrap a declarator list in a DeclStmt
static DeclStmt *wrapDecl(std::vector<VarDeclPtr> *vars, const SourceLoc &loc) {
    DeclStmt *ds = new DeclStmt(loc);
    for (VarDeclPtr &v : *vars) ds->decls.push_back(std::move(v));
    delete vars;
    return ds;
}

} // namespace safec
%}

%union
{
    // token payloads, filled in by safec::yylex
    std::string *str;     // IDENTIFIER, STRING_LITERAL (decoded)
    uint64_t     num;     // INT_LITERAL
    double       fnum;    // FLOAT_LITERAL
    char         ch;      // CHAR_LITERAL (escape decoded)
    bool         bval;    // BOOL_LITERAL

    // parser-internal
    DeclParts   *parts;   // one declarator
    AssignOp     aop;     // which assignment operator

    // single nodes
    TypeSpec     *tspec;
    ParamDecl    *param;
    StructDecl   *strct;
    FunctionDecl *fn;
    Stmt         *stmt;
    CompoundStmt *block;
    SwitchCase   *scase;
    Expr         *expr;
    Initializer  *initp;

    // node lists
    std::vector<DeclParts *>   *partslist;
    std::vector<VarDeclPtr>    *vars;
    std::vector<ParamDeclPtr>  *params;
    std::vector<FieldDeclPtr>  *fields;
    std::vector<StmtPtr>       *stmts;
    std::vector<ExprPtr>       *exprs;
    std::vector<SwitchCasePtr> *cases;
    std::vector<Initializer>   *inits;
    std::vector<PointerLevel>  *ptrs;
}

%token BREAK 1
%token CASE 2
%token CONTINUE 3
%token DEFAULT 4
%token ELSE 5
%token FOR 6
%token IF 7
%token RETURN 8
%token STRUCT 9
%token SWITCH 10
%token WHILE 11
%token VOID 12
%token BOOL 13
%token CHAR 14
%token INT 15
%token FLOAT 16
%token OWN_KEYWORD 17
%token MUT_KEYWORD 18
%token RAW_KEYWORD 19
%token TRUSTED_KEYWORD 20
%token NEW_KEYWORD 21
%token FREE_KEYWORD 22
%token DEFER_KEYWORD 23
%token NULL_KEYWORD 24
%token <bval> BOOL_LITERAL 25
%token <str> IDENTIFIER 26
%token <num> INT_LITERAL 27
%token <ch> CHAR_LITERAL 28
%token <fnum> FLOAT_LITERAL 29
%token <str> STRING_LITERAL 30
%token ELLIPSIS 31
%token RIGHT_SHIFT_ASSIGN 32
%token LEFT_SHIFT_ASSIGN 33
%token RIGHT_SHIFT 34
%token LEFT_SHIFT 35
%token ARROW 36
%token INCREMENT 37
%token DECREMENT 38
%token LOGICAL_AND 39
%token LOGICAL_OR 40
%token LESS_EQUAL 41
%token GREATER_EQUAL 42
%token EQUAL 43
%token NOT_EQUAL 44
%token ADD_ASSIGN 45
%token SUB_ASSIGN 46
%token MUL_ASSIGN 47
%token DIV_ASSIGN 48
%token MOD_ASSIGN 49
%token AND_ASSIGN 50
%token XOR_ASSIGN 51
%token OR_ASSIGN 52
%token ASSIGN 53
%token BITWISE_AND 54
%token LOGICAL_NOT 55
%token MINUS 56
%token PLUS 57
%token MULTIPLY 58
%token DIVIDE 59
%token MODULO 60
%token LESS_THAN 61
%token GREATER_THAN 62
%token BITWISE_XOR 63
%token BITWISE_OR 64
%token BITWISE_NOT 65
%token NULLABLE_PTR 66
%token SEMICOLON 67
%token LEFT_BRACE 68
%token RIGHT_BRACE 69
%token COMMA 70
%token COLON 71
%token LEFT_PAREN 72
%token RIGHT_PAREN 73
%token LEFT_BRACKET 74
%token RIGHT_BRACKET 75
%token DOT 76

/* dangling-else fix: ELSE binds to the nearest if */
%token THEN
%precedence THEN
%precedence ELSE

/* operator precedence, loosest first */
%left LOGICAL_OR
%left LOGICAL_AND
%left BITWISE_OR
%left BITWISE_XOR
%left BITWISE_AND
%left EQUAL NOT_EQUAL
%left LESS_THAN GREATER_THAN LESS_EQUAL GREATER_EQUAL
%left LEFT_SHIFT RIGHT_SHIFT
%left PLUS MINUS
%left MULTIPLY DIVIDE MODULO
%precedence UNARY

/* the actions push top-level decls straight into the program */
%parse-param {safec::Program &prog}
%parse-param {safec::DiagnosticsEngine &diag}

%type <tspec> type_base qual_base
%type <ptrs> ptrs
%type <exprs> arrays
%type <parts> declarator field_declarator
%type <partslist> declarators field_declarators
%type <vars> decl
%type <strct> struct_def
%type <fields> fields field
%type <fn> func_def
%type <params> params param_list
%type <param> param
%type <block> block
%type <stmts> items
%type <stmt> stmt item if_stmt while_stmt for_stmt switch_stmt for_init
%type <expr> opt_assign
%type <scase> section
%type <cases> sections
%type <expr> assignment logical_or logical_and bitwise_or bitwise_xor
%type <expr> bitwise_and equality relational shift additive multiplicative
%type <expr> unary postfix primary compound_literal
%type <exprs> args
%type <aop> assign_op
%type <initp> init
%type <inits> inits

%code {
// the scanner bridge, defined in src/main_parser.cpp
namespace safec {
int yylex(SafeCParser::semantic_type *yylval,
          SafeCParser::location_type *yylloc);
}
}

%%

program
    : %empty
    | program external
    ;

external
    : struct_def { prog.decls.push_back(DeclPtr($1)); }
    | func_def   { prog.decls.push_back(DeclPtr($1)); }
    | decl
      {
        for (VarDeclPtr &v : *$1)
        {
            v->isGlobal = true;
            prog.decls.push_back(std::move(v));
        }
        delete $1;
      }
    ;

struct_def
    : STRUCT IDENTIFIER LEFT_BRACE fields RIGHT_BRACE SEMICOLON
      {
        StructDecl *sd = new StructDecl(SL(@1), std::move(*$2));
        delete $2;
        for (FieldDeclPtr &f : *$4) sd->addField(std::move(f));
        delete $4;
        $$ = sd;
      }
    ;

fields
    : %empty { $$ = new std::vector<FieldDeclPtr>(); }
    | fields field
      {
        $$ = $1;
        for (FieldDeclPtr &f : *$2) $1->push_back(std::move(f));
        delete $2;
      }
    ;

field
    : qual_base field_declarators SEMICOLON
      {
        $$ = new std::vector<FieldDeclPtr>();
        for (DeclParts *p : *$2)
        {
            TypeSpec *ts = attachParts(cloneBase($1), p);
            $$->push_back(FieldDeclPtr(new FieldDecl(p->nameLoc, std::move(p->name),
                                                     TypeSpecPtr(ts))));
            delete p;
        }
        delete $2;
        delete $1;
      }
    ;

field_declarators
    : field_declarator
      { $$ = new std::vector<DeclParts *>(); $$->push_back($1); }
    | field_declarators COMMA field_declarator
      { $$ = $1; $1->push_back($3); }
    ;

field_declarator
    : ptrs IDENTIFIER arrays
      {
        $$ = new DeclParts{ SL(@2), std::move(*$2), std::move(*$1),
                            std::move(*$3), nullptr };
        delete $1; delete $2; delete $3;
      }
    ;

decl
    : qual_base declarators SEMICOLON
      {
        $$ = new std::vector<VarDeclPtr>();
        for (DeclParts *p : *$2)
        {
            TypeSpec *ts = attachParts(cloneBase($1), p);
            $$->push_back(VarDeclPtr(new VarDecl(p->nameLoc, std::move(p->name),
                                                 TypeSpecPtr(ts), std::move(p->init),
                                                 false)));
            delete p;
        }
        delete $2;
        delete $1;
      }
    ;

declarators
    : declarator
      { $$ = new std::vector<DeclParts *>(); $$->push_back($1); }
    | declarators COMMA declarator
      { $$ = $1; $1->push_back($3); }
    ;

declarator
    : ptrs IDENTIFIER arrays
      {
        $$ = new DeclParts{ SL(@2), std::move(*$2), std::move(*$1),
                            std::move(*$3), nullptr };
        delete $1; delete $2; delete $3;
      }
    | ptrs IDENTIFIER arrays ASSIGN assignment
      {
        $$ = new DeclParts{ SL(@2), std::move(*$2), std::move(*$1),
                            std::move(*$3), ExprPtr($5) };
        delete $1; delete $2; delete $3;
      }
    ;

/* stars belong to the declarator: in `int a, *b;` only b is a pointer */
ptrs
    : %empty { $$ = new std::vector<PointerLevel>(); }
    | ptrs MULTIPLY
      { $$ = $1; $1->push_back(PointerLevel{ SL(@2), false }); }
    | ptrs MULTIPLY NULLABLE_PTR
      { $$ = $1; $1->push_back(PointerLevel{ SL(@2), true }); }
    ;

arrays
    : %empty { $$ = new std::vector<ExprPtr>(); }
    | arrays LEFT_BRACKET RIGHT_BRACKET
      { $$ = $1; $1->push_back(nullptr); }            /* `int a[]` */
    | arrays LEFT_BRACKET assignment RIGHT_BRACKET
      { $$ = $1; $1->push_back(ExprPtr($3)); }
    ;

qual_base
    : type_base            { $$ = $1; }
    | OWN_KEYWORD type_base
      { $$ = $2; $2->qualifier = PtrQualifier::Own; $2->loc = SL(@1); }
    | MUT_KEYWORD type_base
      { $$ = $2; $2->qualifier = PtrQualifier::Mut; $2->loc = SL(@1); }
    | RAW_KEYWORD type_base
      { $$ = $2; $2->qualifier = PtrQualifier::Raw; $2->loc = SL(@1); }
    ;

/* the one language change: no bare identifier here, always `struct X` */
type_base
    : VOID  { $$ = new TypeSpec(SL(@1), BaseTypeKind::Void); }
    | BOOL  { $$ = new TypeSpec(SL(@1), BaseTypeKind::Bool); }
    | CHAR  { $$ = new TypeSpec(SL(@1), BaseTypeKind::Char); }
    | INT   { $$ = new TypeSpec(SL(@1), BaseTypeKind::Int); }
    | FLOAT { $$ = new TypeSpec(SL(@1), BaseTypeKind::Float); }
    | STRUCT IDENTIFIER
      {
        $$ = new TypeSpec(SL(@1), BaseTypeKind::Struct, std::move(*$2));
        delete $2;
      }
    ;

func_def
    : qual_base ptrs IDENTIFIER LEFT_PAREN params RIGHT_PAREN block
      {
        TypeSpec *ret = $1;
        ret->pointers = std::move(*$2);
        $$ = new FunctionDecl(SL(@3), std::move(*$3), TypeSpecPtr(ret),
                              std::move(*$5), CompoundStmtPtr($7));
        delete $2; delete $3; delete $5;
      }
    ;

params
    : %empty     { $$ = new std::vector<ParamDeclPtr>(); }   /* f() */
    | VOID       { $$ = new std::vector<ParamDeclPtr>(); }   /* f(void) */
    | param_list { $$ = $1; }
    ;

param_list
    : param
      { $$ = new std::vector<ParamDeclPtr>(); $$->push_back(ParamDeclPtr($1)); }
    | param_list COMMA param
      { $$ = $1; $1->push_back(ParamDeclPtr($3)); }
    ;

param
    : qual_base ptrs IDENTIFIER arrays
      {
        TypeSpec *ts = $1;
        ts->pointers = std::move(*$2);
        ts->arrayDims = std::move(*$4);
        $$ = new ParamDecl(SL(@3), std::move(*$3), TypeSpecPtr(ts));
        delete $2; delete $3; delete $4;
      }
    ;

block
    : LEFT_BRACE items RIGHT_BRACE
      {
        CompoundStmt *b = new CompoundStmt(SL(@1));
        b->body = std::move(*$2);
        b->rbraceLoc = SL(@3);
        $$ = b;
        delete $2;
      }
    ;

items
    : %empty { $$ = new std::vector<StmtPtr>(); }
    | items item
      { $$ = $1; $1->push_back(StmtPtr($2)); }
    ;

item
    : decl  { $$ = wrapDecl($1, SL(@1)); }
    | stmt  { $$ = $1; }
    ;

stmt
    : block       { $$ = $1; }
    | if_stmt     { $$ = $1; }
    | while_stmt  { $$ = $1; }
    | for_stmt    { $$ = $1; }
    | switch_stmt { $$ = $1; }
    | RETURN SEMICOLON
      { $$ = new ReturnStmt(SL(@1)); }
    | RETURN assignment SEMICOLON
      { $$ = new ReturnStmt(SL(@1), ExprPtr($2)); }
    | BREAK SEMICOLON
      { $$ = new BreakStmt(SL(@1)); }
    | CONTINUE SEMICOLON
      { $$ = new ContinueStmt(SL(@1)); }
    | DEFER_KEYWORD stmt
      { $$ = new DeferStmt(SL(@1), StmtPtr($2)); }
    | TRUSTED_KEYWORD block
      { $$ = new TrustedStmt(SL(@1), CompoundStmtPtr($2)); }
    | assignment SEMICOLON
      { $$ = new ExprStmt(SL(@1), ExprPtr($1)); }
    | SEMICOLON
      { $$ = new NullStmt(SL(@1)); }
    ;

if_stmt
    : IF LEFT_PAREN assignment RIGHT_PAREN stmt %prec THEN
      { $$ = new IfStmt(SL(@1), ExprPtr($3), StmtPtr($5)); }
    | IF LEFT_PAREN assignment RIGHT_PAREN stmt ELSE stmt %prec ELSE
      { $$ = new IfStmt(SL(@1), ExprPtr($3), StmtPtr($5), StmtPtr($7)); }
    ;

while_stmt
    : WHILE LEFT_PAREN assignment RIGHT_PAREN stmt
      { $$ = new WhileStmt(SL(@1), ExprPtr($3), StmtPtr($5)); }
    ;

for_stmt
    : FOR LEFT_PAREN for_init opt_assign SEMICOLON opt_assign RIGHT_PAREN stmt
      { $$ = new ForStmt(SL(@1), StmtPtr($3), ExprPtr($4), ExprPtr($6), StmtPtr($8)); }
    ;

/* a decl already ends with its own SEMICOLON */
for_init
    : decl
      { $$ = wrapDecl($1, SL(@1)); }
    | opt_assign SEMICOLON
      { $$ = $1 ? static_cast<Stmt*>(new ExprStmt(SL(@2), ExprPtr($1))) : nullptr; }
    ;

opt_assign
    : %empty      { $$ = nullptr; }
    | assignment  { $$ = $1; }
    ;

switch_stmt
    : SWITCH LEFT_PAREN assignment RIGHT_PAREN LEFT_BRACE sections RIGHT_BRACE
      {
        SwitchStmt *sw = new SwitchStmt(SL(@1), ExprPtr($3));
        for (SwitchCasePtr &c : *$6) sw->cases.push_back(std::move(c));
        delete $6;
        $$ = sw;
      }
    ;

sections
    : %empty { $$ = new std::vector<SwitchCasePtr>(); }
    | sections section
      { $$ = $1; $1->push_back(SwitchCasePtr($2)); }
    ;

section
    : CASE assignment COLON items
      {
        CaseStmt *c = new CaseStmt(SL(@1), ExprPtr($2));
        c->body = std::move(*$4);
        delete $4;
        $$ = c;
      }
    | DEFAULT COLON items
      {
        DefaultStmt *c = new DefaultStmt(SL(@1));
        c->body = std::move(*$3);
        delete $3;
        $$ = c;
      }
    ;

assignment
    : logical_or { $$ = $1; }
    | logical_or assign_op assignment
      { $$ = new AssignExpr(SL(@2), $2, ExprPtr($1), ExprPtr($3)); }
    ;

assign_op
    : ASSIGN             { $$ = AssignOp::Assign; }
    | ADD_ASSIGN         { $$ = AssignOp::Add; }
    | SUB_ASSIGN         { $$ = AssignOp::Sub; }
    | MUL_ASSIGN         { $$ = AssignOp::Mul; }
    | DIV_ASSIGN         { $$ = AssignOp::Div; }
    | MOD_ASSIGN         { $$ = AssignOp::Mod; }
    | AND_ASSIGN         { $$ = AssignOp::And; }
    | OR_ASSIGN          { $$ = AssignOp::Or; }
    | XOR_ASSIGN         { $$ = AssignOp::Xor; }
    | LEFT_SHIFT_ASSIGN  { $$ = AssignOp::Shl; }
    | RIGHT_SHIFT_ASSIGN { $$ = AssignOp::Shr; }
    ;

logical_or
    : logical_and { $$ = $1; }
    | logical_or LOGICAL_OR logical_and
      { $$ = new BinaryExpr(SL(@2), BinaryOp::LogOr, ExprPtr($1), ExprPtr($3)); }
    ;

logical_and
    : bitwise_or { $$ = $1; }
    | logical_and LOGICAL_AND bitwise_or
      { $$ = new BinaryExpr(SL(@2), BinaryOp::LogAnd, ExprPtr($1), ExprPtr($3)); }
    ;

bitwise_or
    : bitwise_xor { $$ = $1; }
    | bitwise_or BITWISE_OR bitwise_xor
      { $$ = new BinaryExpr(SL(@2), BinaryOp::BitOr, ExprPtr($1), ExprPtr($3)); }
    ;

bitwise_xor
    : bitwise_and { $$ = $1; }
    | bitwise_xor BITWISE_XOR bitwise_and
      { $$ = new BinaryExpr(SL(@2), BinaryOp::BitXor, ExprPtr($1), ExprPtr($3)); }
    ;

bitwise_and
    : equality { $$ = $1; }
    | bitwise_and BITWISE_AND equality
      { $$ = new BinaryExpr(SL(@2), BinaryOp::BitAnd, ExprPtr($1), ExprPtr($3)); }
    ;

equality
    : relational { $$ = $1; }
    | equality EQUAL relational
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Eq, ExprPtr($1), ExprPtr($3)); }
    | equality NOT_EQUAL relational
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Ne, ExprPtr($1), ExprPtr($3)); }
    ;

relational
    : shift { $$ = $1; }
    | relational LESS_THAN shift
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Lt, ExprPtr($1), ExprPtr($3)); }
    | relational GREATER_THAN shift
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Gt, ExprPtr($1), ExprPtr($3)); }
    | relational LESS_EQUAL shift
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Le, ExprPtr($1), ExprPtr($3)); }
    | relational GREATER_EQUAL shift
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Ge, ExprPtr($1), ExprPtr($3)); }
    ;

shift
    : additive { $$ = $1; }
    | shift LEFT_SHIFT additive
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Shl, ExprPtr($1), ExprPtr($3)); }
    | shift RIGHT_SHIFT additive
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Shr, ExprPtr($1), ExprPtr($3)); }
    ;

additive
    : multiplicative { $$ = $1; }
    | additive PLUS multiplicative
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Add, ExprPtr($1), ExprPtr($3)); }
    | additive MINUS multiplicative
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Sub, ExprPtr($1), ExprPtr($3)); }
    ;

multiplicative
    : unary { $$ = $1; }
    | multiplicative MULTIPLY unary
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Mul, ExprPtr($1), ExprPtr($3)); }
    | multiplicative DIVIDE unary
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Div, ExprPtr($1), ExprPtr($3)); }
    | multiplicative MODULO unary
      { $$ = new BinaryExpr(SL(@2), BinaryOp::Mod, ExprPtr($1), ExprPtr($3)); }
    ;

unary
    : postfix { $$ = $1; }
    | MINUS unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::Minus, ExprPtr($2)); }
    | PLUS unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::Plus, ExprPtr($2)); }
    | LOGICAL_NOT unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::Not, ExprPtr($2)); }
    | BITWISE_NOT unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::BitNot, ExprPtr($2)); }
    | MULTIPLY unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::Deref, ExprPtr($2)); }
    | BITWISE_AND unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::AddrOf, ExprPtr($2)); }
    | BITWISE_AND MUT_KEYWORD unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::AddrOfMut, ExprPtr($3)); }
    | INCREMENT unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::PreInc, ExprPtr($2)); }
    | DECREMENT unary %prec UNARY
      { $$ = new UnaryExpr(SL(@1), UnaryOp::PreDec, ExprPtr($2)); }
    ;

postfix
    : primary { $$ = $1; }
    | postfix LEFT_PAREN RIGHT_PAREN
      { $$ = new CallExpr(SL(@2), ExprPtr($1)); }
    | postfix LEFT_PAREN args RIGHT_PAREN
      {
        CallExpr *c = new CallExpr(SL(@2), ExprPtr($1));
        c->args = std::move(*$3);
        delete $3;
        $$ = c;
      }
    | postfix LEFT_BRACKET assignment RIGHT_BRACKET
      { $$ = new IndexExpr(SL(@2), ExprPtr($1), ExprPtr($3)); }
    | postfix DOT IDENTIFIER
      { $$ = new MemberExpr(SL(@2), ExprPtr($1), std::move(*$3), false); delete $3; }
    | postfix ARROW IDENTIFIER
      { $$ = new MemberExpr(SL(@2), ExprPtr($1), std::move(*$3), true); delete $3; }
    | postfix INCREMENT
      { $$ = new UnaryExpr(SL(@2), UnaryOp::PostInc, ExprPtr($1)); }
    | postfix DECREMENT
      { $$ = new UnaryExpr(SL(@2), UnaryOp::PostDec, ExprPtr($1)); }
    ;

args
    : assignment
      { $$ = new std::vector<ExprPtr>(); $$->push_back(ExprPtr($1)); }
    | args COMMA assignment
      { $$ = $1; $1->push_back(ExprPtr($3)); }
    ;

primary
    : INT_LITERAL    { $$ = new IntLiteral(SL(@1), $1); }
    | FLOAT_LITERAL  { $$ = new FloatLiteral(SL(@1), $1); }
    | CHAR_LITERAL   { $$ = new CharLiteral(SL(@1), $1); }
    | STRING_LITERAL
      { $$ = new StringLiteral(SL(@1), std::move(*$1)); delete $1; }
    | BOOL_LITERAL   { $$ = new BoolLiteral(SL(@1), $1); }
    | NULL_KEYWORD   { $$ = new NullLiteral(SL(@1)); }
    | IDENTIFIER     { $$ = new NameExpr(SL(@1), std::move(*$1)); delete $1; }
    | LEFT_PAREN assignment RIGHT_PAREN
      { $$ = $2; }
    | NEW_KEYWORD type_base
      { $$ = new NewExpr(SL(@1), TypeSpecPtr($2)); }
    | FREE_KEYWORD LEFT_PAREN assignment RIGHT_PAREN
      { $$ = new FreeExpr(SL(@1), ExprPtr($3)); }
    | compound_literal { $$ = $1; }
    ;

compound_literal
    : LEFT_PAREN qual_base ptrs RIGHT_PAREN LEFT_BRACE inits RIGHT_BRACE
      {
        TypeSpec *ts = $2;
        ts->pointers = std::move(*$3);
        CompoundLiteralExpr *cl = new CompoundLiteralExpr(SL(@1), TypeSpecPtr(ts));
        cl->inits = std::move(*$6);
        delete $3; delete $6;
        $$ = cl;
      }
    ;

inits
    : init
      { $$ = new std::vector<Initializer>(); $$->push_back(std::move(*$1)); delete $1; }
    | inits COMMA init
      { $$ = $1; $1->push_back(std::move(*$3)); delete $3; }
    ;

init
    : DOT IDENTIFIER ASSIGN assignment
      {
        $$ = new Initializer{ SL(@1), std::move(*$2), ExprPtr($4), nullptr };
        delete $2;
      }
    | assignment
      { $$ = new Initializer{ SL(@1), "", ExprPtr($1), nullptr }; }
    ;

%%
