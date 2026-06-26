// Temporary toolchain proof — DELETE in Task 9.
// Confirms clang + libFuzzer + AddressSanitizer are wired correctly by
// crashing deterministically on the input "bug".
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size >= 3 && data[0] == 'b' && data[1] == 'u' && data[2] == 'g') {
    volatile int *p = nullptr;
    return *p;  // null deref -> ASan SEGV
  }
  return 0;
}
