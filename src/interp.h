#pragma once
#include "heap.h"
#include "module.h"
#include "value.h"
#include <cstdint>
#include <string>

namespace coal {

struct RunResult {
  std::string output;  // accumulated PRINT output
  std::string error;   // non-empty iff a runtime error stopped execution
  bool        ok = false;
};

struct Limits {
  uint64_t max_insns = 2'000'000;  // instruction budget (stops infinite loops)
  uint32_t max_depth = 256;        // call-stack depth cap
};

// Execute a VERIFIED module's entry function. Assumes verify(m) returned ok, so
// every operand is in range and pc lands on instruction boundaries. Runtime
// conditions (type errors, index out of range, division by zero, budget/depth
// exhaustion) produce a structured error, never a crash. All object creation
// goes through `h`.
RunResult run(const Module& m, Heap& h, Limits limits = {});

}  // namespace coal
