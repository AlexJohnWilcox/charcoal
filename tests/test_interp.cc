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

  // ARRAY_PUSH: append grows the backing store and preserves elements.
  { auto r = SRC("a = [1, 2]; push(a, 3); push(a, 4); push(a, 5); print a[4];");
    CHECK(r.ok); CHECK(r.output == "5"); }

  // push onto an empty array literal, then read back.
  { auto r = SRC("a = []; push(a, 7); push(a, 8); push(a, 9); print a[0] + a[1] + a[2];");
    CHECK(r.ok); CHECK(r.output == "24"); }

  // push evaluates to nil.
  { auto r = SRC("a = []; x = push(a, 1); if (x) { print 1; } else { print 0; }");
    CHECK(r.ok); CHECK(r.output == "0"); }

  // push on a non-array is a runtime error, not a crash.
  { auto r = SRC("x = 5; push(x, 1);"); CHECK(!r.ok); }

  // --- A1 operators ---
  { auto r = SRC("print 3 < 5;");            CHECK(r.ok); CHECK(r.output == "true"); }
  { auto r = SRC("print 5 <= 5;");           CHECK(r.ok); CHECK(r.output == "true"); }
  { auto r = SRC("print 6 > 9;");            CHECK(r.ok); CHECK(r.output == "false"); }
  { auto r = SRC("print 2 == 2;");           CHECK(r.ok); CHECK(r.output == "true"); }
  { auto r = SRC("print 2 != 3;");           CHECK(r.ok); CHECK(r.output == "true"); }
  { auto r = SRC("print 1 < 2 && 3 > 2;");   CHECK(r.ok); CHECK(r.output == "true"); }
  { auto r = SRC("print 0 || 0;");           CHECK(r.ok); CHECK(r.output == "false"); }
  { auto r = SRC("print 1 || 0;");           CHECK(r.ok); CHECK(r.output == "true"); }
  { auto r = SRC("print 7 % 3;");            CHECK(r.ok); CHECK(r.output == "1"); }
  { auto r = SRC("print 0 - 5 + 2;");        CHECK(r.ok); CHECK(r.output == "-3"); }
  { auto r = SRC("print -5 + 2;");           CHECK(r.ok); CHECK(r.output == "-3"); }
  { auto r = SRC("print !0;");               CHECK(r.ok); CHECK(r.output == "true"); }
  { auto r = SRC("print !3;");               CHECK(r.ok); CHECK(r.output == "false"); }
  { auto r = SRC("print 6 & 3;");            CHECK(r.ok); CHECK(r.output == "2"); }
  { auto r = SRC("print 5 | 2;");            CHECK(r.ok); CHECK(r.output == "7"); }
  { auto r = SRC("print 6 ^ 3;");            CHECK(r.ok); CHECK(r.output == "5"); }
  { auto r = SRC("print 1 << 4;");           CHECK(r.ok); CHECK(r.output == "16"); }
  { auto r = SRC("print 32 >> 2;");          CHECK(r.ok); CHECK(r.output == "8"); }
  { auto r = SRC("print 1 + 2 * 3 == 7;");   CHECK(r.ok); CHECK(r.output == "true"); }  // precedence
  { auto r = SRC("a = \"ab\"; print a == \"ab\";"); CHECK(r.ok); CHECK(r.output == "true"); }
  // UB guards must produce runtime errors, not sanitizer aborts:
  { auto r = SRC("print 10 % 0;");           CHECK(!r.ok); }
  { auto r = SRC("print 1 << 99;");          CHECK(!r.ok); }
  { auto r = SRC("print 5 < \"x\";");        CHECK(!r.ok); }  // comparison on non-number

  // --- A3 control flow ---
  { auto r = SRC("s = 0; for (i = 0; i < 5; i = i + 1) { s = s + i; } print s;");
    CHECK(r.ok); CHECK(r.output == "10"); }
  { auto r = SRC("s = 0; for (i = 0; i < 10; i = i + 1) { if (i == 5) { break; } s = s + 1; } print s;");
    CHECK(r.ok); CHECK(r.output == "5"); }
  { auto r = SRC("s = 0; for (i = 0; i < 5; i = i + 1) { if (i == 2) { continue; } s = s + i; } print s;");
    CHECK(r.ok); CHECK(r.output == "8"); }   // 0+1+3+4
  { auto r = SRC("i = 0; s = 0; while (i < 5) { i = i + 1; if (i == 3) { continue; } s = s + i; } print s;");
    CHECK(r.ok); CHECK(r.output == "12"); }   // 1+2+4+5
  { auto r = SRC("x = 7; if (x < 0) { print 1; } elif (x == 7) { print 2; } else { print 3; }");
    CHECK(r.ok); CHECK(r.output == "2"); }
  { auto r = SRC("x = 9; if (x < 0) { print 1; } elif (x == 7) { print 2; } else { print 3; }");
    CHECK(r.ok); CHECK(r.output == "3"); }
  { auto r = SRC("n = 0; for (i = 0; i < 3; i = i + 1) { for (j = 0; j < 3; j = j + 1) { if (j == 1) { break; } n = n + 1; } } print n;");
    CHECK(r.ok); CHECK(r.output == "3"); }    // inner loop breaks after j==0 each time
  { auto r = SRC("break;"); CHECK(!r.ok); }   // break outside loop -> compile error
  { auto r = SRC("continue;"); CHECK(!r.ok); }
}
