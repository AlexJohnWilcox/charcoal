#pragma once
#include "ast.h"
#include "module.h"
#include <string>

namespace coal {

struct CompileResult {
  Module      module;
  std::string error;       // non-empty iff ok == false ("line: msg")
  bool        ok = false;
  int         err_line = 0;
};

// Lower a parsed Program into an in-memory Module. Top-level statements become
// an implicit entry function. Unknown called names are a compile error.
CompileResult compile(const Node* program);

}  // namespace coal
