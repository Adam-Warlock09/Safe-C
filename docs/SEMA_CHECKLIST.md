# Sema checklist

Checks for the first version of semantic analysis over `src/new_ast.h`.
Flow-sensitive ownership checking (moves, leaks, borrow exclusivity)
is out of scope for now; see [Later](#later).

General rules for every check:

- Report through `DiagnosticsEngine` at the node's `loc`; add a `note`
  pointing at the earlier declaration where one exists.
- Call `setInvalid()` on the node after reporting. A node whose operand
  is invalid or has no type gets marked invalid **silently**: one
  mistake, one error.
- Fill in the `[sema]` fields as you go (`type`, `category`, `decl`,
  `field`, `target`, `resolved`).

## Pass order

1. **Collect:** register every `StructDecl` and `FunctionDecl` name in
   the global scope before checking any body. SafeC has no prototypes,
   so this is what allows calls to functions defined later and mutual
   recursion.
2. **Resolve struct fields:** resolve every field's `TypeSpec`.
3. **Resolve signatures:** resolve function signatures and global variables.
4. **Check bodies:** walk each function body with a scope stack.

## 1. Declarations and scopes

- [ ] Duplicate struct name (`struct A {..}; struct A {..};`)
- [ ] Duplicate field name within one struct
- [ ] Duplicate global name (functions and global variables share one namespace)
- [ ] Duplicate parameter name
- [ ] Duplicate local in the **same** block (shadowing an outer scope is allowed)
- [ ] A local is visible only *after* its declarator: `int x = x;` is an error
- [ ] If `main` is defined, it must be `int main()` / `int main(void)`

## 2. Type resolution (`TypeSpec` -> `Type`)

- [ ] `struct X` with no definition of `X` -> unknown struct type
- [ ] `void` only as a return type or behind a pointer: reject `void x;`,
      `void` params, `void` fields, `void` array elements
- [ ] A struct can't contain itself by value (directly or through another
      struct); it must go through a pointer
- [ ] Array size must be an integer constant `> 0`
- [ ] `[]` (no size) only on parameters, which have no initializer to size it from
- [ ] `own` / `mut` / `raw` with no `*` -> error (`own int x;`)
- [ ] Apply the qualifier to the **outermost** pointer level (the one the
      variable itself holds); inner levels are plain borrows. The team can
      change this rule, but every place must use the same one.

## 3. Names

- [ ] Undeclared identifier
- [ ] Calling something that isn't a function; using a function name as a value
- [ ] Unknown field in `.` / `->` / designated initializer `.name =`

## 4. Expressions

**Value category.** `NameExpr` (variable or param), `*p`, `a[i]`, `s.f`
and `p->f` are lvalues; everything else is an rvalue.

- [ ] Arithmetic (`+ - * /`): both operands `int`/`char`/`float`; result
      uses the usual promotion (`char` -> `int`, any `float` -> `float`)
- [ ] `%`, `& | ^`, `<< >>`, `~`: integer operands only
- [ ] **No pointer arithmetic** (`p + 1` is an error outside `trusted`)
- [ ] `< > <= >=`: arithmetic operands; result `bool`
- [ ] `== !=`: both arithmetic, or both pointers to the same pointee, or a
      nullable pointer against `null`; result `bool`
- [ ] `&& || !`: scalar operands (`bool`, number or pointer); result `bool`
- [ ] Integer literal doesn't fit in `int`
- [ ] `*p` and `p->f`: `p` must be a **non-nullable** pointer
- [ ] `s.f`: `s` must have struct type
- [ ] `&x` / `&mut x`: operand must be an lvalue; result `T*` / `mut T*`
- [ ] `++ --`: arithmetic lvalue operand
- [ ] Assignment: target is a modifiable lvalue, not an array or a function;
      compound ops (`+=` ...) follow the matching binary-operator rule
- [ ] Call: right number of arguments; each argument converts to its parameter type
- [ ] Index: base is an array, index is an integer; a constant index out
      of bounds of a sized array is an error
- [ ] `new T`: `T` is not `void`; result is non-nullable `own T*`
- [ ] `free(e)`: `e` must be a non-nullable `own` pointer; result `void`
- [ ] Compound literal: the type must be a struct; designated fields exist
      and aren't repeated; no more positional initializers than fields;
      each value converts to its field type
- [ ] Allowed implicit conversions (insert an `ImplicitCastExpr` for each):
      `char`<->`int`<->`bool`, `int`<->`float`, `null` -> `T*?`,
      `T*` -> `T*?`, `own`/`mut T*` -> `T*`, `own T*` -> `mut T*`.
      **Everything else is an error**, including `T*?` -> `T*`, any
      conversion to or from `raw`, and any conversion that produces `own`.

## 5. Statements

- [ ] `if` / `while` / `for` condition is scalar (wrap with `ToBool`)
- [ ] `break` outside a loop or switch; `continue` outside a loop; set `target`
- [ ] `return;` in a non-`void` function, `return e;` in a `void` function,
      returned value converts to the return type
- [ ] Non-`void` function can fall off the end -> warning (simple version:
      the last statement isn't a `return`)
- [ ] `switch`: condition is an integer; each `case` value is an integer
      constant; no duplicate case values; at most one `default`
- [ ] `defer` body may not contain `return`, `break` or `continue` that jumps
      out of it, and may not itself contain `defer`
- [ ] Variable initializer converts to the declared type; **global**
      initializers must be constants
- [ ] Track "inside `trusted`" while walking (needed by section 6)

## 6. SafeC pointer rules (no flow analysis yet)

- [ ] `null` only goes into nullable pointers (assign, init, argument, return)
- [ ] Nullable pointer can't be dereferenced, used with `->`, passed where a
      non-nullable pointer is expected, or freed
- [ ] **Basic narrowing:** inside `if (p != null) {...}` and
      `while (p != null) {...}`, where `p` is a local or parameter name,
      treat `p` as non-nullable, **unless** the body assigns to `p`. Also
      handle `if (p == null) ... else {...}` for the else-branch. Without
      this, no program with nullable pointers can compile.
- [ ] `raw` pointers: dereference, `->` and indexing only inside `trusted`
- [ ] Writing through a read borrow (`T*`) is an error: `*p = 1` and
      `p->f = 1` need `own` or `mut`

## Later

These need flow-sensitive analysis on top of the above:

- **Moves:** use after move (`own T* b = a; *a;`) and double free
- **Leaks:** an `own` pointer goes out of scope without being freed or moved
- **Borrow exclusivity:** no `&mut` while another borrow is live, and no
  borrow outliving its owner
- **Borrows are second-class:** they can't be stored in struct fields or returned
- **No copying structs with `own` fields:** a struct containing an `own`
  field can't be copied by value
- **Proper narrowing:** full data-flow narrowing in place of the
  lexical version above
