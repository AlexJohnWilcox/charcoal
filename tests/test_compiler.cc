#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "test_main.h"

using namespace coal;

static CompileResult compile_src(const char* s, size_t n) {
  auto l = lex(s, n);
  auto p = parse(l.tokens);
  return compile(p.program);
}

void test_compiler() {
  // Simple program compiles; entry function has code.
  {
    auto c = compile_src("print 1 + 2;", 12);
    CHECK(c.ok);
    CHECK(c.module.funcs.size() >= 1);
    CHECK(c.module.entry_func < c.module.funcs.size());
    CHECK(!c.module.funcs[c.module.entry_func].code.empty());
  }

  // Constants are recorded (the literal 7 shows up in the pool).
  {
    auto c = compile_src("print 7;", 8);
    CHECK(c.ok);
    bool has7 = false;
    for (auto& k : c.module.consts)
      if (k.tag == CTag::Int && k.i == 7) has7 = true;
    CHECK(has7);
  }

  // A user function plus a call to it compiles.
  {
    auto c = compile_src("fn add(a, b) { return a + b; } print add(2, 3);", 47);
    CHECK(c.ok);
    CHECK(c.module.funcs.size() >= 2);
  }

  // Calling an unknown function name is a compile error, not a crash.
  {
    auto c = compile_src("print nope(1);", 14);
    CHECK(!c.ok);
  }
}
