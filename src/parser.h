#pragma once
#include "ast.h"
#include "token.h"
#include <vector>

namespace coal {

// Parse a token stream (as produced by lex()) into an AST. Never crashes:
// syntax errors and over-deep nesting set ParseResult::error and stop. The
// returned Program node and all descendants are owned by ParseResult::arena.
ParseResult parse(const std::vector<Token>& toks);

}  // namespace coal
