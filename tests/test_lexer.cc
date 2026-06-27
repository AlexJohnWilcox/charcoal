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

  // --- A1 operators: each lexes to the right token ---
  {
    auto o = lex("== != <= >= && || ! % & | ^ ~ << >>", 35);
    CHECK(o.error.empty());
    const Tok want[] = {Tok::EqEq, Tok::BangEq, Tok::Le, Tok::Ge, Tok::AmpAmp,
                        Tok::PipePipe, Tok::Bang, Tok::Percent, Tok::Amp, Tok::Pipe,
                        Tok::Caret, Tok::Tilde, Tok::Shl, Tok::Shr, Tok::Eof};
    CHECK(o.tokens.size() == sizeof(want) / sizeof(want[0]));
    for (size_t i = 0; i < o.tokens.size(); ++i) CHECK(o.tokens[i].kind == want[i]);
  }

  // Longest-match disambiguation: multi-char vs single-char neighbours.
  {
    auto d = lex("< << <= = == & &&", 17);
    CHECK(d.error.empty());
    const Tok want[] = {Tok::Lt, Tok::Shl, Tok::Le, Tok::Assign, Tok::EqEq,
                        Tok::Amp, Tok::AmpAmp, Tok::Eof};
    CHECK(d.tokens.size() == sizeof(want) / sizeof(want[0]));
    for (size_t i = 0; i < d.tokens.size(); ++i) CHECK(d.tokens[i].kind == want[i]);
  }

  // A trailing two-char-capable operator at EOF must not overread: "<" alone.
  {
    auto t = lex("<", 1);
    CHECK(t.error.empty());
    CHECK(t.tokens[0].kind == Tok::Lt);
    CHECK(t.tokens.back().kind == Tok::Eof);
  }

  // --- A4: source spans (1-based columns) ---

  // "cd" in "ab cd" begins at column 4.
  {
    auto cr = lex("ab cd", 5);
    CHECK(cr.error.empty());
    CHECK(cr.tokens[0].col == 1);   // "ab"
    CHECK(cr.tokens[1].col == 4);   // "cd"
  }

  // Columns reset after a newline.
  {
    auto cr = lex("a\nbc", 4);
    CHECK(cr.error.empty());
    CHECK(cr.tokens[0].line == 1 && cr.tokens[0].col == 1);  // "a"
    CHECK(cr.tokens[1].line == 2 && cr.tokens[1].col == 1);  // "bc"
  }

  // A lex error carries line/col and a "line:col:" message prefix.
  {
    auto cr = lex("  @", 3);
    CHECK(!cr.error.empty());
    CHECK(cr.err_line == 1 && cr.err_col == 3);
    CHECK(cr.error.rfind("1:3:", 0) == 0);  // message starts with the span
  }
}
