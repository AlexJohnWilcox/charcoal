#include "charcoal.h"
#include "test_main.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace coal;

void test_e2e() {
  // Full path: source -> .cbc -> load/verify/run.
  {
    std::vector<uint8_t> cbc;
    std::string err;
    CHECK(compile_source("print 6 * 7;", 12, cbc, err));
    CHECK(!cbc.empty());
    auto r = run_cbc(cbc.data(), cbc.size());
    CHECK(r.ok);
    CHECK(r.output == "42");
  }

  // A function round-trips through the binary format and runs.
  {
    const char* s = "fn dbl(a) { return a + a; } print dbl(21);";
    std::vector<uint8_t> cbc;
    std::string err;
    CHECK(compile_source(s, __builtin_strlen(s), cbc, err));
    auto r = run_cbc(cbc.data(), cbc.size());
    CHECK(r.ok);
    CHECK(r.output == "42");
  }

  // Garbage bytes to run_cbc: structured failure, no crash.
  {
    uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto r = run_cbc(junk, sizeof junk);
    CHECK(!r.ok);
  }

  // Source compile error surfaces, doesn't crash.
  {
    std::vector<uint8_t> cbc;
    std::string err;
    CHECK(!compile_source("print 1 +", 9, cbc, err));
    CHECK(!err.empty());
  }
}
