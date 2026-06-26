#pragma once
#include "token.h"
#include <cstddef>
#include <string>
#include <vector>

namespace coal {

struct LexResult {
  std::vector<Token> tokens;  // always ends with a Tok::Eof on success
  std::string        error;   // non-empty iff lexing failed
  int                err_line = 0;
};

// Tokenize `n` bytes of source. MUST treat input as raw bytes of known length
// (the fuzzer feeds non-NUL-terminated buffers) and never read past src + n.
// Never crashes; malformed input (e.g. unterminated string) sets `error`.
LexResult lex(const char* src, size_t n);

}  // namespace coal
