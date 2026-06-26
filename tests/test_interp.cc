#include "compiler.h"
#include "heap.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "test_main.h"

using namespace coal;

static RunResult run_src(const char* s, size_t n) {
  auto l = lex(s, n);
  auto p = parse(l.tokens);
  auto c = compile(p.program);
  if (!c.ok) {
    RunResult r;
    r.error = c.error;
    return r;
  }
  Heap h(8u << 20);
  return run(c.module, h);
}

#define SRC(x) run_src(x, sizeof(x) - 1)

void test_interp() {
  // Arithmetic + precedence.
  { auto r = SRC("print 1 + 2 * 3;"); CHECK(r.ok); CHECK(r.output == "7"); }

  // Arrays: literal + index.
  { auto r = SRC("x = [10, 20, 30]; print x[1];"); CHECK(r.ok); CHECK(r.output == "20"); }

  // Maps: literal + field access.
  { auto r = SRC("m = {\"a\" = 5}; print m.a;"); CHECK(r.ok); CHECK(r.output == "5"); }

  // Functions + arguments.
  { auto r = SRC("fn sq(a) { return a * a; } print sq(6);"); CHECK(r.ok); CHECK(r.output == "36"); }

  // while loop with mutation.
  { auto r = SRC("i = 3; while (i) { i = i - 1; } print i;"); CHECK(r.ok); CHECK(r.output == "0"); }

  // if / else.
  { auto r = SRC("if (0) { print 1; } else { print 2; }"); CHECK(r.ok); CHECK(r.output == "2"); }

  // Recursion (exercises the call stack).
  { auto r = SRC("fn f(n) { if (n) { return f(n - 1); } return 9; } print f(5);");
    CHECK(r.ok); CHECK(r.output == "9"); }

  // Division by zero -> runtime error, NOT a SIGFPE crash.
  { auto r = SRC("print 1 / 0;"); CHECK(!r.ok); }

  // Index out of range -> runtime error, not OOB.
  { auto r = SRC("x = [1]; print x[5];"); CHECK(!r.ok); }

  // Type error: add a number and an array -> runtime error.
  { auto r = SRC("print 1 + [2];"); CHECK(!r.ok); }

  // Infinite loop -> instruction budget stops it (no hang).
  { auto r = SRC("while (1) {}"); CHECK(!r.ok); }
}
