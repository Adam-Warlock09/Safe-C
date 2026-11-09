# SafeC compiler - build file
# Needs flex, bison 3.x and a C++17 compiler (the parser is C++).
#
# Targets:
#   make            build the safec-parser, the standalone lexer test binary and the driver
#   make driver     build the driver (driver --till-lexer|--till-parser|--till-sema|--till-codegen <file>)
#   make test       run the lexer regression test and the parser test suite
#   make test-lexer run the existing lexer test (tests/lexer/test_lexer.safec)
#   make test-parser run the parser test suite (tests/parser/)
#   make clean      remove build artifacts

CC          = cc
CXX         = c++
FLEX        = flex
BISON       = bison
CFLAGS      = -std=c11 -Wall -Wextra -O2 -Isrc
CXXFLAGS    = -std=c++17 -Wall -Wextra -O2 -I. -Isrc
LEXCFLAGS   = -w -Isrc
YACCCFLAGS  = -w -Isrc -std=c++17

all: safec-parser lexer driver

lex.yy.c: src/lexer.l src/tokens.h
	$(FLEX) -o $@ src/lexer.l

parser.tab.cpp parser.tab.hpp: src/parser.y src/tokens.h
	$(BISON) -d -o parser.tab.cpp src/parser.y

lexer.o: lex.yy.c
	$(CC) $(LEXCFLAGS) -c -o $@ lex.yy.c

lexer-lib.o: lex.yy.c
	$(CC) $(LEXCFLAGS) -DSAFEC_NO_LEXER_MAIN -c -o $@ lex.yy.c

parser.tab.o: parser.tab.cpp
	$(CXX) $(YACCCFLAGS) -c -o $@ parser.tab.cpp

sema.o: src/sema.cpp src/sema.h src/new_ast.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/sema.cpp

main_parser.o: src/main_parser.cpp parser.tab.hpp src/new_ast.h src/ast_dump.h src/sema.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/main_parser.cpp

ast_dump.o: src/ast_dump.cpp src/ast_dump.h src/new_ast.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/ast_dump.cpp

borrowck.o: src/borrowck.cpp src/borrowck.h src/new_ast.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/borrowck.cpp

codegen_expr.o: src/codegen_expr.cpp src/codegen.h src/new_ast.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/codegen_expr.cpp

codegen_stmt.o: src/codegen_stmt.cpp src/codegen.h src/new_ast.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/codegen_stmt.cpp

driver.o: src/driver.cpp parser.tab.hpp src/new_ast.h src/ast_dump.h src/sema.h src/tokens.h src/codegen.h
	$(CXX) $(CXXFLAGS) -c -o $@ src/driver.cpp

driver: driver.o parser.tab.o lexer-lib.o ast_dump.o sema.o borrowck.o codegen_expr.o codegen_stmt.o
	$(CXX) -o $@ $^

lexer: lexer.o
	$(CC) -o $@ lexer.o

safec-parser: parser.tab.o lexer-lib.o main_parser.o ast_dump.o sema.o borrowck.o codegen_expr.o codegen_stmt.o
	$(CXX) -o $@ $^

test-lexer: lexer
	@echo "== lexer regression test (tests/lexer/test_lexer.safec) =="
	@./lexer < tests/lexer/test_lexer.safec > /dev/null && echo "lexer: OK"

test-parser: safec-parser
	bash tests/parser/run_tests.sh

test: test-lexer test-parser

clean:
	rm -f safec-parser lexer lexer.o lexer-lib.o lex.yy.c \
	      parser.tab.o parser.tab.cpp parser.tab.hpp parser.tab.c parser.tab.h \
	      location.hh position.hh stack.hh main_parser.o ast_dump.o sema.o \
	      driver driver.o borrowck.o codegen_expr.o codegen_stmt.o

.PHONY: all test test-lexer test-parser clean