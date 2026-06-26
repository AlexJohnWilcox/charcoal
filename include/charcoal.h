#pragma once
#include "interp.h"
#include "module.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coal {

// Compile source text to a .cbc byte buffer. Returns false on lex/parse/compile
// error (with a message in `err`); never throws, never crashes on bad input.
bool compile_source(const char* src, size_t n,
                    std::vector<uint8_t>& out_cbc, std::string& err);

// Load + verify + run a .cbc byte buffer on a fresh heap. A malformed module or
// failed verification comes back as RunResult{ ok=false, error=... }.
RunResult run_cbc(const uint8_t* data, size_t size, Limits limits = {});

}  // namespace coal
