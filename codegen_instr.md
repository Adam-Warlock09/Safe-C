Strict File Ownership Rule:

Person A ONLY edits: src/codegen_expr.cpp

Person B ONLY edits: src/codegen_stmt.cpp and src/driver.cpp

src/codegen.h Rule: Neither person deletes or renames existing functions in codegen.h. If someone needs a new private helper, add it at the bottom of your section in codegen.h and tell the other person on chat before pushing.

Pull/Push Workflow (git pull --rebase):
Because you are editing completely different .cpp files, git pull --rebase origin codegen && git push origin codegen will auto-merge cleanly 100% of the time with zero merge conflicts, and you immediately get each other's latest working expressions/statements as soon as they are pushed.