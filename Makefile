# SafeC compiler - build file
#
# Targets:
#   make            build the safec-parser and the standalone lexer test binary
#   make test       run the lexer regression test and the parser test suite
#   make test-lexer run the existing lexer test (tests/lexer/test_lexer.safec)
#   make test-parser run the parser test suite (tests/parser/)
#   make clean      remove build artifacts

CC      = cc
FLEX    = flex
CFLAGS  = -std=c11 -Wall -Wextra -O2 -Isrc
# the Flex-generated file is not warning-clean; don't police it
LEXCFLAGS = -w -Isrc

SRC  = src/ast.c src/parser.c src/main_parser.c
HDRS = src/ast.h src/parser.h src/tokens.h

all: safec-parser lexer

lex.yy.c: src/lexer.l src/tokens.h
	$(FLEX) -o $@ src/lexer.l

# Two object files from the same generated lexer:
#   lexer.o      - embedded main() active  (standalone lexer test)
#   lexer-lib.o  - embedded main() disabled (linked into safec-parser)
lexer.o: lex.yy.c
	$(CC) $(LEXCFLAGS) -c -o $@ lex.yy.c

lexer-lib.o: lex.yy.c
	$(CC) $(LEXCFLAGS) -DSAFEC_NO_LEXER_MAIN -c -o $@ lex.yy.c

# Standalone lexer (embedded main() active) - unchanged team behaviour.
lexer: lexer.o
	$(CC) -o $@ lexer.o

# Parser build: the driver in src/main_parser.c provides main().
safec-parser: $(SRC) $(HDRS) lexer-lib.o
	$(CC) $(CFLAGS) -o $@ lexer-lib.o $(SRC)

test-lexer: lexer
	@echo "== lexer regression test (tests/lexer/test_lexer.safec) =="
	@./lexer < tests/lexer/test_lexer.safec > /dev/null && echo "lexer: OK"

test-parser: safec-parser
	bash tests/parser/run_tests.sh

test: test-lexer test-parser

clean:
	rm -f safec-parser lexer lexer.o lexer-lib.o lex.yy.c

.PHONY: all test test-lexer test-parser clean
