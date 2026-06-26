#include "test_main.h"

int g_failures = 0;

// Declare each module's test entry point. Add to this list as tasks land.
void test_value();
void test_object();
void test_heap();
void test_lexer();
void test_parser();

int main() {
  test_value();
  test_object();
  test_heap();
  test_lexer();
  test_parser();

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
