#pragma once
#include "module.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace coal {

struct LoadResult {
  Module      module;
  std::string error;   // non-empty iff ok == false
  bool        ok = false;
};

// Decode a .cbc v1 byte buffer into a Module. MUST be fully defensive: every
// read is bounds-checked against `size`; any malformed/truncated input yields a
// structured error and never crashes or reads out of bounds.
LoadResult load_cbc(const uint8_t* data, size_t size);

}  // namespace coal
