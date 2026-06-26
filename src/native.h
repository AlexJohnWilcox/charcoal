#pragma once
#include "heap.h"
#include "value.h"
#include <cstdint>
#include <string>

namespace coal {

// A builtin: reads `argc` args, writes its result to `out`, returns true on
// success. On failure returns false and sets `err` (which becomes a VM runtime
// error). `args` points into the caller's register window (GC roots). If the
// native allocates, it must obey the safepoint rule: read what it needs out of
// args before allocating, or re-read after — never hold a stale Object*.
using NativeFn = bool (*)(Value* args, uint32_t argc, Heap& h,
                          Value& out, std::string& err);

struct NativeEntry {
  const char* name;
  uint8_t     arity_min;
  uint8_t     arity_max;  // 255 = variadic upper bound
  NativeFn    fn;
};

// Lookup by name; nullptr if not a builtin. Both forms search the same table.
const NativeEntry* find_native(const char* name, uint32_t len);
const NativeEntry* find_native(const std::string& name);

}  // namespace coal
