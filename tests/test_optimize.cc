#include "optimize.h"
#include "ast.h"
#include "astprint.h"
#include "compiler.h"
#include "heap.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "test_main.h"

#include <string>

using namespace coal;

namespace {

RunResult run_opt(const std::string& s, bool opt) {
  auto l = lex(s.data(), s.size());
  auto p = parse(l.tokens);
  if (!p.program) { RunResult r; r.error = "parse"; return r; }
  if (opt) fold_constants(p.program);
  auto c = compile(p.program);
  if (!c.ok) { RunResult r; r.error = c.error; return r; }
  Heap h(8u << 20);
  return run(c.module, h);
}

// The folder's contract: optimized and unoptimized compilation observe the same
// behavior (same ok-ness, same printed output).
void same(const char* src) {
  RunResult a = run_opt(src, false);
  RunResult b = run_opt(src, true);
  CHECK(a.ok == b.ok);
  CHECK(a.output == b.output);
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

}  // namespace

void test_optimize() {
  // The pass really collapses a nested constant expression to one literal.
  {
    std::string s = "print 2 + 3 * 4;";
    auto l = lex(s.data(), s.size());
    auto p = parse(l.tokens);
    fold_constants(p.program);
    std::string d = dump_ast(p.program);
    CHECK(contains(d, "IntLit 14"));
    CHECK(!contains(d, "Binary"));
  }

  // Behavior is preserved across the operator set.
  same("print 2 + 3 * 4;");          // 14
  same("print (10 - 2) * 3;");       // 24
  same("print 7 / 2;");              // integer division -> 3
  same("print 7 % 3;");              // 1
  same("print 1 << 10;");            // 1024
  same("print 255 & 15;");           // 15
  same("print 6 | 1;");              // 7
  same("print 5 ^ 1;");              // 4
  same("print ~0;");                 // -1
  same("print 0 - 5;");              // -5
  same("print 2 < 3;");              // true
  same("print 3 <= 3;");             // true
  same("print 4 == 4;");             // true
  same("print 4 != 5;");             // true
  same("print !0;");                 // true
  same("print !5;");                 // false
  same("print true && false;");      // false
  same("print true || false;");      // true
  same("print 1.5 + 2.5 == 4.0;");   // true (double folding)
  same("print 3.0 * 2.0 == 6.0;");   // true

  // Trapping/non-finite cases are left unfolded -> identical (erroring) behavior.
  same("print 10 / 0;");             // division by zero
  same("print 5 % 0;");              // modulo by zero
  same("print 1 << 64;");            // shift out of range

  // Folding inside larger programs doesn't change behavior.
  same("x = 2 + 3; print x * 2;");                       // 10
  same("a = [1 + 1, 2 * 2]; print a[0] + a[1];");        // 6
  same("fn f(n) { return n + (2 * 3); } print f(10);");  // 16
  same("i = 0; s = 0; while (i < 2 + 1) { s = s + i; i = i + 1; } print s;");  // 3
  same("print 2 + 3 < 4 * 2;");                          // 5 < 8 -> true
}
