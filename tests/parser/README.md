# Safe-C Parser Test Suite

## Overview

This directory contains test cases for validating the Safe-C parser.

The test suite checks whether:
- Valid Safe-C programs are parsed successfully.
- The generated Abstract Syntax Tree (AST) matches the corresponding expected output.
- Invalid Safe-C programs are rejected with a syntax error.

The test suite validates the existing parser implementation without modifying any parser source code.

## Test Categories

### 1. Valid Test Cases

The `valid/` directory contains programs that are expected to parse successfully.

These tests cover the following language constructs:

- Variable and function declarations
- Arithmetic expressions and operator precedence
- Unary operators and literals
- Relational and logical expressions
- Function definitions and calls
- Conditional statements (`if-else`)
- Loops
- Structures
- Nested blocks
- Compound literals
- Switch statements
- Arrays
- Safe-C extensions
- Pointer operations
- Array parameters
- Void functions
- Bitwise and shift operators
- Compound assignment operators
- Increment and decrement operators
- Empty statements
- `for` loops with optional expressions
- `for` loops with declaration initialization
- Structure member access
- Pointer member access
- `defer` and `trusted` constructs

For every valid input, the parser generates an AST using the `--ast` option.

The generated AST is compared against its corresponding expected output file in `expected/valid/`.

A test passes only when:
1. The parser exits successfully.
2. No parsing error is reported.
3. The generated AST exactly matches the expected output.

### 2. Invalid Test Cases

The `invalid/` directory contains programs that are expected to produce syntax errors.

These tests cover malformed constructs such as:

- Missing semicolons
- Missing braces
- Incomplete expressions
- Malformed declarations
- Unknown types
- Unbalanced parentheses
- `else` without a matching `if`
- Incorrect structure declarations
- Invalid function parameters
- Stray `case` labels
- Unclosed function calls
- Missing array brackets
- Trailing commas in function arguments
- Missing structure member names
- Missing separators in `for` loops
- Missing colons in `case` labels
- Empty compound literals
- Unsupported variadic parameter syntax

A test passes when:
1. The parser exits with a non-zero status.
2. The parser reports a syntax error.

Invalid tests check syntax-error detection rather than comparing exact error messages.

## Running the Tests

Run the complete parser test suite from the repository root:

    bash tests/parser/run_tests.sh

The test runner executes all valid and invalid test cases individually.

For valid tests, it:
- Runs the parser with the `--ast` option.
- Checks whether parsing succeeds.
- Compares the generated AST with the expected output.
- Reports whether the AST matches.

For invalid tests, it:
- Runs the parser on the invalid input.
- Checks whether the parser exits with a non-zero status.
- Checks whether a syntax error is reported.

Each test displays an individual PASS or FAIL status.

At the end, the runner displays a summary containing:
- Number of valid tests passed
- Number of valid tests failed
- Number of invalid tests passed
- Number of invalid tests failed
- Total number of tests passed and failed

## Updating Expected AST Outputs

If the parser's AST output changes intentionally, the expected output files can be regenerated using:

    bash tests/parser/run_tests.sh --update-expected

This updates the expected AST files inside `expected/valid/` using the current parser output.

After regenerating the expected outputs, run the normal test suite again:

    bash tests/parser/run_tests.sh

**Note:** Expected output files should be reviewed before committing. Regenerating them only updates the reference outputs; it does not independently verify that the generated AST is correct.

## Test Summary

The parser test suite currently contains 44 test cases.

| Category | Number of Tests |
|----------|----------------:|
| Valid programs | 25 |
| Invalid programs | 19 |
| **Total** | **44** |

Latest reported test result:

    Parser tests: 44 passed, 0 failed (out of 44)
    ALL PARSER TESTS PASSED

## Notes

- The test suite does not modify the parser implementation.
- Valid test cases use exact AST output comparison.
- Invalid test cases check whether syntax errors are detected.
- Expected AST outputs are maintained separately from the input programs.
- The test runner supports regenerating expected AST outputs using the `--update-expected` option.