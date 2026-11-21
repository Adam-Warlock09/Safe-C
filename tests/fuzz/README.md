# Crash fuzzing

Feeds the compiler thousands of damaged versions of the test programs. Bad input
must be rejected with an error message: the compiler must never crash, hang, or
produce invalid LLVM IR.

```bash
make fuzz                                  # 20000 programs, seed 1 (about 10 minutes)
make fuzz N=100000 SEED=7                  # more, and different ones
bash tests/fuzz/run_fuzz.sh 100000 7       # the same, without make
```

Needs Python 3, a compiler that supports `-fsanitize=address,undefined` (clang
or gcc), and LLVM's `opt` for the IR check (skipped if it isn't installed).

## How it works

1. `run_fuzz.sh` builds a second copy of `safec` in `tests/fuzz/build/` with
   AddressSanitizer and UBSan. They catch memory errors (use after free, reading
   past an array) and undefined behaviour inside the compiler the moment they
   happen, even when the compiler wouldn't visibly crash. Your normal `./safec`
   isn't touched.
2. `fuzz.py` takes every `.safec` file in `tests/cases` and makes damaged
   copies of it, two kinds mixed together:
   - **token damage**: delete, duplicate, swap or replace tokens, cut the file
     short. Mostly syntax errors, so this tests the lexer and parser.
   - **syntax-keeping damage**: swap a name, type or operator, add or remove
     `own`/`mut`/`*?`, move or delete a line, wrap one in `trusted { }`. These
     get past the parser, so they test sema, the borrow checker and codegen.
3. Each damaged program goes through `safec --till-sema`. If it's accepted, it
   also goes through `--emit-llvm` and `opt -passes=verify`.

A seed always gives the same damaged programs, so a run can be repeated.

## Results

Problems are grouped (one bug isn't reported a thousand times) and each one is
saved in `tests/fuzz/out/` as `<id>.safec`, the smallest input that shows it,
and `<id>.log`, the compiler's output:

| kind         | meaning |
|--------------|---------|
| crash        | the compiler was killed by a signal, or a sanitizer reported an error |
| hang         | no answer within 10 seconds |
| throw        | an uncaught C++ exception |
| codegen-fail | sema accepted the program, then code generation failed without an error message |
| bad-ir       | code generation worked but the LLVM IR is invalid |

Rerun one by hand to see it: `tests/fuzz/build/safec tests/fuzz/out/<id>.safec --emit-llvm`.

The run exits with 0 when no problem was found.

## Found so far

One bug: a block `defer` (`defer { ... }`) that ran at a `return`, `break` or
`continue` read memory that had already been freed inside codegen. Fixed, with a
regression test in `codegen/pass/defer`. Over 50,000 programs have run clean
since.
