#include "test_main.h"

int g_failures = 0;

// Declare each module's test entry point. Add to this list as tasks land.
void test_value();
void test_object();
void test_heap();
void test_lexer();
void test_parser();
void test_compiler();
void test_module_roundtrip();
void test_loader();
void test_verifier();
void test_interp();
void test_e2e();

int main() {
  test_value();
  test_object();
  test_heap();
  test_lexer();
  test_parser();
  test_compiler();
  test_module_roundtrip();
  test_loader();
  test_verifier();
  test_interp();
  test_e2e();

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("ok");
  return 0;
}
