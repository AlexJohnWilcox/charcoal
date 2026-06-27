#include "lexer.h"

#include <stdexcept>
#include <string>

namespace coal {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

Tok keyword(const char* s, size_t len) {
  std::string w(s, len);
  if (w == "fn")     return Tok::KwFn;
  if (w == "return") return Tok::KwReturn;
  if (w == "if")     return Tok::KwIf;
  if (w == "else")   return Tok::KwElse;
  if (w == "while")  return Tok::KwWhile;
  if (w == "nil")    return Tok::KwNil;
  if (w == "true")   return Tok::KwTrue;
  if (w == "false")  return Tok::KwFalse;
  if (w == "print")  return Tok::KwPrint;
  if (w == "for")    return Tok::KwFor;
  if (w == "break")  return Tok::KwBreak;
  if (w == "continue") return Tok::KwContinue;
  if (w == "elif")   return Tok::KwElif;
  return Tok::Ident;
}

}  // namespace

LexResult lex(const char* src, size_t n) {
  LexResult r;
  size_t i = 0;
  int line = 1;

  auto push = [&](Tok k, const char* start, size_t len) {
    Token t{};
    t.kind = k;
    t.start = start;
    t.len = static_cast<uint32_t>(len);
    t.line = line;
    r.tokens.push_back(t);
    return &r.tokens.back();
  };

  while (i < n) {
    char c = src[i];

    // Whitespace.
    if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
    if (c == '\n') { ++line; ++i; continue; }

    // Line comment.
    if (c == '/' && i + 1 < n && src[i + 1] == '/') {
      i += 2;
      while (i < n && src[i] != '\n') ++i;
      continue;
    }

    // Number: int or float.
    if (is_digit(c)) {
      size_t start = i;
      while (i < n && is_digit(src[i])) ++i;
      bool is_float = false;
      if (i + 1 < n && src[i] == '.' && is_digit(src[i + 1])) {
        is_float = true;
        ++i;
        while (i < n && is_digit(src[i])) ++i;
      }
      std::string num(src + start, i - start);
      Token* t = push(is_float ? Tok::Float : Tok::Int, src + start, i - start);
      try {
        if (is_float) t->dval = std::stod(num);
        else          t->ival = std::stoll(num);
      } catch (const std::out_of_range&) {
        r.error = "number literal out of range";
        r.err_line = line;
        return r;
      }
      continue;
    }

    // Identifier or keyword.
    if (is_alpha(c)) {
      size_t start = i;
      while (i < n && is_alnum(src[i])) ++i;
      push(keyword(src + start, i - start), src + start, i - start);
      continue;
    }

    // String literal.
    if (c == '"') {
      size_t start = i;
      ++i;
      bool closed = false;
      while (i < n) {
        if (src[i] == '\\' && i + 1 < n) { i += 2; continue; }
        if (src[i] == '"') { ++i; closed = true; break; }
        if (src[i] == '\n') ++line;
        ++i;
      }
      if (!closed) {
        r.error = "unterminated string";
        r.err_line = line;
        return r;
      }
      push(Tok::Str, src + start, i - start);
      continue;
    }

    // Operators and punctuation (longest-match for the multi-char operators;
    // every lookahead stays bounds-checked via i + 1 < n).
    Tok k;
    uint32_t oplen = 1;
    auto next_is = [&](char want) { return i + 1 < n && src[i + 1] == want; };
    switch (c) {
      case '+': k = Tok::Plus;      break;
      case '-': k = Tok::Minus;     break;
      case '*': k = Tok::Star;      break;
      case '/': k = Tok::Slash;     break;
      case '%': k = Tok::Percent;   break;
      case '^': k = Tok::Caret;     break;
      case '~': k = Tok::Tilde;     break;
      case '(': k = Tok::LParen;    break;
      case ')': k = Tok::RParen;    break;
      case '{': k = Tok::LBrace;    break;
      case '}': k = Tok::RBrace;    break;
      case '[': k = Tok::LBracket;  break;
      case ']': k = Tok::RBracket;  break;
      case ',': k = Tok::Comma;     break;
      case '.': k = Tok::Dot;       break;
      case ';': k = Tok::Semicolon; break;
      case '=': if (next_is('=')) { k = Tok::EqEq;   oplen = 2; } else k = Tok::Assign; break;
      case '!': if (next_is('=')) { k = Tok::BangEq; oplen = 2; } else k = Tok::Bang;   break;
      case '&': if (next_is('&')) { k = Tok::AmpAmp; oplen = 2; } else k = Tok::Amp;    break;
      case '|': if (next_is('|')) { k = Tok::PipePipe; oplen = 2; } else k = Tok::Pipe; break;
      case '<':
        if (next_is('=')) { k = Tok::Le;  oplen = 2; }
        else if (next_is('<')) { k = Tok::Shl; oplen = 2; }
        else k = Tok::Lt;
        break;
      case '>':
        if (next_is('=')) { k = Tok::Ge;  oplen = 2; }
        else if (next_is('>')) { k = Tok::Shr; oplen = 2; }
        else k = Tok::Gt;
        break;
      default:
        r.error = "unexpected character";
        r.err_line = line;
        return r;
    }
    push(k, src + i, oplen);
    i += oplen;
  }

  push(Tok::Eof, src + n, 0);
  return r;
}

}  // namespace coal
