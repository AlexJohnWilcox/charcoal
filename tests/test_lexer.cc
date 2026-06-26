#include "lexer.h"
#include "test_main.h"

using namespace coal;

void test_lexer() {
  auto r = lex("fn x = 12 + 3.5; // hi", 22);
  CHECK(r.error.empty());
  CHECK(r.tokens.front().kind == Tok::KwFn);
  bool sawFloat = false;
  bool sawInt = false;
  for (auto& t : r.tokens) {
    if (t.kind == Tok::Float) sawFloat = true;
    if (t.kind == Tok::Int) sawInt = true;
  }
  CHECK(sawInt);
  CHECK(sawFloat);
  CHECK(r.tokens.back().kind == Tok::Eof);

  // Unterminated string must set error, not crash, and not read past the buffer.
  auto bad = lex("\"unterminated", 13);
  CHECK(!bad.error.empty());

  // Empty input yields just Eof.
  auto empty = lex("", 0);
  CHECK(empty.error.empty());
  CHECK(empty.tokens.size() == 1);
  CHECK(empty.tokens.back().kind == Tok::Eof);

  // Keyword vs identifier disambiguation.
  auto kw = lex("ifx if", 6);
  CHECK(kw.tokens[0].kind == Tok::Ident);  // "ifx" is not a keyword
  CHECK(kw.tokens[1].kind == Tok::KwIf);

  // Overflowing integer literal must be a structured error, never an abort.
  std::string big(40, '9');
  auto of = lex(big.c_str(), big.size());
  CHECK(!of.error.empty());
}
