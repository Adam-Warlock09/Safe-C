# Parser tests

Tests for the SafeC parser: the bison (LALR(1)) grammar in
`src/parser.y`, whose actions build the `new_ast.h` tree.
Run from the repository root:

```
make test-parser     # or: bash tests/parser/run_tests.sh
```

## Test groups

1. **Valid programs** (`NN_*.safec`, 13 files). Each must parse with
   exit code 0 and produce an AST dump (`--ast`) identical to the
   golden file in `expected/`. Covers declarations, precedence,
   unary operators and literals, relational/logical operators,
   functions, if/else, loops, structs, nested blocks, compound
   literals, switch, array syntax, and the SafeC extensions
   (`15_safec_extensions.safec` — qualifiers, `*?`, `new`/`free`,
   `defer`, `trusted`, `null`, `&mut`).
2. **Invalid programs** (`eNN_*.safec`, 11 files). Each must be
   rejected with exit code 1 and a "syntax error" on stderr. Covers
   missing semicolons/braces/parens, incomplete expressions, malformed
   declarations, unknown types, stray labels, bad parameter lists.

The grammar has no error-recovery rules yet, so it reports the first
syntax error and stops; the invalid tests therefore assert only that
an error is reported, not which one.

## Notes

- Struct types are always written with the `struct` keyword
  (`struct List x`), per the language decision in the top-level README
  that keeps the grammar LALR(1)-clean.
- Duplicate struct definitions are **legal syntax** — rejecting them
  needs a symbol table, which is semantic analysis' job (see
  `docs/SEMA_CHECKLIST.md`, section 1).

## Adding or changing tests

1. Add `tests/parser/NN_name.safec` (valid) or `eNN_name.safec`
   (invalid).
2. Run `bash tests/parser/run_tests.sh --update-expected` to generate
   the golden dump.
3. **Read the golden file** and check the tree is actually correct
   before committing it — the golden only proves the output is stable,
   not that it is right.
4. Run `bash tests/parser/run_tests.sh` to confirm everything passes.
