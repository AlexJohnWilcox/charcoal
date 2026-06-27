#pragma once
#include "token.h"
#include <string>
#include <vector>

namespace coal {

struct Node;

// Human-readable dump of a token stream: one token per line, each prefixed with
// its `line:col` position and followed by its lexeme or literal value. Used by
// the `charcoal tokens` CLI command and as a lexer debugging aid.
std::string dump_tokens(const std::vector<Token>& toks);

// Indented structural dump of an AST rooted at a Program node. Each node prints
// its kind plus the salient field (operator, name, literal value) and then its
// children one indent level deeper. Used by `charcoal ast`.
std::string dump_ast(const Node* program);

// Re-emit canonical Charcoal source from an AST. Statements are laid out with
// two-space indentation; every compound sub-expression (binary, logical, unary,
// assignment) is wrapped in parentheses, so the formatted text always re-parses
// to a tree equivalent to the input. Used by `charcoal fmt`.
std::string format_source(const Node* program);

}  // namespace coal
