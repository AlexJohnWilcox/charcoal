#include "test_main.h"

int g_failures = 0;

// Declare each module's test entry point. Add to this list as tasks land.
void test_value();
void test_object();
void test_heap();

int main() {
  test_value();
  test_object();
  test_heap();

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
