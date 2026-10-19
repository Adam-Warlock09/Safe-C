#include "new_ast.h"
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace safec;

class Scope {
public:
  void push() { frames.emplace_back(); }
  void pop() { frames.pop_back(); }

  bool declare(VarDecl *d) {
    return frames.back().emplace(d->name, d).second;
  }

  VarDecl* lookup(std::string_view name) {
    for (auto it = frames.rbegin(); it < frames.rend(); it++) {
      if ((*it).count(name)) return (*it)[name]; ;
    }
    return nullptr;
  }
  
private:
  std::vector<std::unordered_map<std::string_view, VarDecl*>> frames;
};

class Sema {
public:
  Sema(DiagnosticsEngine& diag): diag(diag) { }

  void run(Program& prog) {
    // Stuff
  }
  
private:
  DiagnosticsEngine& diag;
  Scope scope;
  std::unordered_map<std::string_view, FunctionDecl*> funs;
  FunctionDecl* currFun;
};