# Parser tests (common C-like grammar)

Tests for the common C-like grammar parser (`src/parser.c`).
Run from the repository root:

```
make test-parser     # or: bash tests/parser/run_tests.sh
```

## Test groups

1. **Valid programs** (`NN_*.safec`, 12 files). Each must parse with
   exit code 0 and produce an AST dump identical to the golden file in
   `expected/`. Covers declarations, precedence, literals, functions,
   if/else, loops, structs, nested blocks, compound literals, switch,
   and array syntax.
2. **Invalid programs** (`eNN_*.safec`, 12 files). Each must be
   rejected with exit code 1 and stderr containing the expected
   message fragments (listed in `run_tests.sh`). Covers missing
   semicolons/braces/parens, incomplete expressions, malformed
   declarations, unknown types, stray labels, duplicate structs.

SafeC extension grammar (`own`, `mut`, `raw`, `?`, `new`, `free`,
`defer`, `trusted`, `null`, `&mut`) is another team member's part and
is deliberately not tested here. The parser still rejects extension
keywords with a clear "SafeC extension" error when it meets them
(see `perr_if_safec_extension` in `src/parser.c`), but that is the
extensions module's input, not ours. Programs written with extensions -
like the whitepaper's linked-list case study - will not parse until
the extensions module is integrated.

## History note

The earlier valid tests `10_safec_extensions.safec` and
`13_case_study.safec` covered SafeC extension grammar inside this
suite. When my assigned scope was clarified to the **common C-like
grammar only**, their content was first moved to a separate
`safec_extensions/` group, and finally removed from the suite
altogether - the SafeC grammar is a teammate's part, so this suite
now contains common-grammar tests only. The extension constructs
were also removed from the common test files (`01`, `05`, `08`), and
those goldens were regenerated.

## Adding or changing tests

1. Add `tests/parser/NN_name.safec`.
2. Run `bash tests/parser/run_tests.sh --update-expected` to generate
   the golden dump.
3. **Read the golden file** and check the tree is actually correct
   before committing it — the golden only proves the output is stable,
   not that it is right.
4. Run `bash tests/parser/run_tests.sh` to confirm everything passes.
