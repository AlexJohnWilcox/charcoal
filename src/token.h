#pragma once
#include <cstdint>

namespace coal {

enum class Tok : uint8_t {
  Eof,
  Int, Float, Str, Ident,
  Plus, Minus, Star, Slash,
  LParen, RParen, LBrace, RBrace, LBracket, RBracket,
  Comma, Dot, Assign, Semicolon,
  // operators (A1)
  EqEq, BangEq, Lt, Le, Gt, Ge, AmpAmp, PipePipe, Bang,
  Percent, Amp, Pipe, Caret, Tilde, Shl, Shr,
  KwFn, KwReturn, KwIf, KwElse, KwWhile, KwNil, KwTrue, KwFalse, KwPrint,
  KwFor, KwBreak, KwContinue, KwElif,
  Error
};

struct Token {
  Tok      kind;
  const char* start;  // points into the source buffer (not owned)
  uint32_t len;
  int      line;
  int      col = 0;   // 1-based column of the token's first byte
  int64_t  ival;      // valid when kind == Int
  double   dval;      // valid when kind == Float
};

}  // namespace coal
