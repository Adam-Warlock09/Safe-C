# Safe-C Lexer Test Suite

## Overview

This directory contains test cases for validating the lexical analysis of the Safe-C programming language.

The test suite consists of 43 input programs covering different lexical components, including keywords, identifiers, literals, operators, comments, punctuation, Safe-C-specific constructs, and edge cases.

Each input program is processed by the lexer, and its generated token stream is compared against the corresponding expected output.

## Test Categories

| Directory | Description | Test Cases |
|---|---|---:|
| `1-keywords/` | C keywords, Safe-C keywords, boolean literals, and keyword boundaries | 9 |
| `2-identifiers/` | Basic identifiers, underscores, digits, and keyword-like identifiers | 5 |
| `3-literals/` | Integer, floating-point, character, string, boolean, and null literals | 5 |
| `4-operators/` | Arithmetic, relational, logical, assignment, bitwise, and unary operators | 5 |
| `5-comments/` | Single-line, multiline, and comments containing special characters | 4 |
| `6-punctuation/` | Braces, parentheses, brackets, commas, semicolons, and pointer-related punctuation | 5 |
| `7-safec/` | Ownership, mutable borrowing, raw pointers, defer, and nullable pointers | 6 |
| `8-edge_cases/` | Empty input, whitespace, keyword boundaries, adjacent operators, and multiple comments | 4 |
| **Total** | | **43** |

## Running the Tests

From the project root directory, execute:

```bash
bash tests/lexer/run_tests.sh
```

The test runner:

1. Executes the lexer on each `.safec` input file.
2. Captures the generated token output in a temporary directory.
3. Compares the generated output with the corresponding expected output using `diff`.
4. Reports each test as PASS or FAIL.
5. Displays the total number of passed and failed tests.

Temporary output files are automatically removed after execution.

## Test Results

A successful test run should report:

```text
Passed: 43
Failed: 0
```