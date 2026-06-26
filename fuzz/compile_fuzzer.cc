// Front-end fuzz target: source text through the whole pipeline.
// Bytes -> lexer -> parser -> compiler -> (serialize) -> loader -> verifier ->
// interpreter. Exercises the language front end and that compiler output always
// round-trips and runs.
#include "charcoal.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  using namespace coal;

  std::vector<uint8_t> cbc;
  std::string err;
  if (!compile_source(reinterpret_cast<const char *>(data), size, cbc, err))
    return 0;

  Limits limits;
  limits.max_insns = 500'000;
  limits.max_depth = 128;
  (void)run_cbc(cbc.data(), cbc.size(), limits);
  return 0;
}
