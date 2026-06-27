#include "astprint.h"
#include "ast.h"
#include "compiler.h"
#include "heap.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "test_main.h"

#include <string>
#include <vector>

using namespace coal;

namespace {

RunResult run_src(const std::string& s) {
  auto l = lex(s.data(), s.size());
  auto p = parse(l.tokens);
  auto c = compile(p.program);
  if (!c.ok) { RunResult r; r.error = c.error; return r; }
  Heap h(8u << 20);
  return run(c.module, h);
}

std::string fmt_src(const std::string& s) {
  auto l = lex(s.data(), s.size());
  auto p = parse(l.tokens);
  if (!p.program) return "";
  return format_source(p.program);
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// Format the program, then confirm the formatted text (a) still parses and runs
// to the SAME output as the original, and (b) formatting it again is behaviorally
// stable. This pins the formatter's core invariant — behavior preservation —
// without asserting byte-for-byte layout.
void check_roundtrip(const char* src) {
  std::string original(src);
  RunResult r0 = run_src(original);

  std::string f1 = fmt_src(original);
  CHECK(!f1.empty());
  RunResult r1 = run_src(f1);
  CHECK(r1.ok == r0.ok);
  CHECK(r1.output == r0.output);

  std::string f2 = fmt_src(f1);
  RunResult r2 = run_src(f2);
  CHECK(r2.ok == r0.ok);
  CHECK(r2.output == r0.output);
}

}  // namespace

void test_astprint() {
  // --- token dump ---
  {
    std::string s = "x = 1 + 2;";
    auto l = lex(s.data(), s.size());
    std::string d = dump_tokens(l.tokens);
    CHECK(contains(d, "Ident"));
    CHECK(contains(d, "Assign"));
    CHECK(contains(d, "Plus"));
    CHECK(contains(d, "Int"));
    CHECK(contains(d, "Eof"));
  }

  // --- AST dump ---
  {
    std::string s = "fn sq(n) { return n * n; } print sq(3);";
    auto l = lex(s.data(), s.size());
    auto p = parse(l.tokens);
    CHECK(p.program != nullptr);
    std::string d = dump_ast(p.program);
    CHECK(contains(d, "Program"));
    CHECK(contains(d, "FnDecl"));
    CHECK(contains(d, "Return"));
    CHECK(contains(d, "Binary"));
    CHECK(contains(d, "Print"));
    CHECK(contains(d, "Call"));
  }

  // --- formatter: behavior preserved across a battery of programs ---
  check_roundtrip("print 1 + 2 * 3;");
  check_roundtrip("print (1 + 2) * 3;");
  check_roundtrip("print 10 - 2 - 3;");                 // left-assoc grouping
  check_roundtrip("print 2 < 3 && 4 > 1;");
  check_roundtrip("x = 5; print 0 - x + 1;");
  check_roundtrip("x = 7; print !(x == 7);");
  check_roundtrip("a = [1, 2, 3]; print a[1] + a[2];");
  check_roundtrip("m = { \"a\" = 1, \"b\" = 2 }; print m.a + m.b;");
  check_roundtrip("fn add(a, b) { return a + b; } print add(20, 22);");
  check_roundtrip("i = 3; s = 0; while (i) { s = s + i; i = i - 1; } print s;");
  check_roundtrip("s = 0; for (i = 0; i < 5; i = i + 1) { s = s + i; } print s;");
  check_roundtrip("if (1) { print 10; } else { print 20; }");
  check_roundtrip("x = 2; if (x == 1) { print 1; } elif (x == 2) { print 2; } else { print 3; }");
  check_roundtrip("f = fn(x) { return x * x; } print f(6);");
  check_roundtrip("fn adder(n) { return fn(x) { return x + n; }; } g = adder(5); print g(10);");
  check_roundtrip("print 7 % 3 + (8 & 6) - (1 << 2);");
  check_roundtrip("print \"a\\nb\";");                  // string escapes survive

  // --- formatter output shape (spot checks) ---
  {
    std::string f = fmt_src("print 1+2*3;");
    CHECK(contains(f, "1 + (2 * 3)"));                  // compound operand parenthesized
  }
  {
    std::string f = fmt_src("fn f(a,b){return a+b;}");
    CHECK(contains(f, "fn f(a, b) {"));
    CHECK(contains(f, "return a + b;"));
  }
}
