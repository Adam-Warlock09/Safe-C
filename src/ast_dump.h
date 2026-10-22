/*
 * ast_dump.h - print the AST for --ast and the test goldens.
 * One node per line, two-space indent, (kind @line detail).
 */
#ifndef SAFEC_AST_DUMP_H
#define SAFEC_AST_DUMP_H

#include <iosfwd>

namespace safec {

class Program;

void dumpProgram(const Program &prog, std::ostream &os);

} // namespace safec

#endif
