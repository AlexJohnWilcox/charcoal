// Primary fuzz target: the binary module pipeline.
// Bytes -> loader -> verifier -> interpreter. This is the deep surface where a
// crafted .cbc drives the VM into stateful corners. Execution is bounded so the
// fuzzer surfaces memory-safety bugs rather than OOM/timeouts.
#include "heap.h"
#include "interp.h"
#include "loader.h"
#include "verifier.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  using namespace coal;

  LoadResult loaded = load_cbc(data, size);
  if (!loaded.ok) return 0;

  if (!verify(loaded.module).ok) return 0;

  Heap heap(8u << 20);  // 8 MiB cap
  Limits limits;
  limits.max_insns = 500'000;
  limits.max_depth = 128;
  (void)run(loaded.module, heap, limits);
  return 0;
}
