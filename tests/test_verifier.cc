#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "verifier.h"
#include "test_main.h"

using namespace coal;

static Module compile_src(const char* s, size_t n) {
  auto l = lex(s, n);
  auto p = parse(l.tokens);
  return compile(p.program).module;
}

void test_verifier() {
  // Compiler output always verifies.
  {
    Module m = compile_src("fn add(a,b){return a+b;} print add(1,2);", 40);
    CHECK(verify(m).ok);
  }

  // Out-of-range register operand is rejected.
  {
    Module m = compile_src("print 1;", 8);
    // LOAD_CONST r=200, k=0 with a small register window.
    m.funcs[0].code = {OP_LOAD_CONST, 200, 0, 0, OP_HALT};
    m.funcs[0].num_regs = 4;
    CHECK(!verify(m).ok);
  }

  // Out-of-range constant operand is rejected.
  {
    Module m = compile_src("print 1;", 8);
    m.funcs[0].code = {OP_LOAD_CONST, 0, 0xff, 0xff, OP_HALT};  // k = 65535
    m.funcs[0].num_regs = 4;
    CHECK(!verify(m).ok);
  }

  // Truncated instruction (opcode with missing operands) is rejected.
  {
    Module m = compile_src("print 1;", 8);
    m.funcs[0].code = {OP_LOAD_CONST, 0};  // needs r + k, only r present
    m.funcs[0].num_regs = 4;
    CHECK(!verify(m).ok);
  }

  // Unknown opcode is rejected.
  {
    Module m = compile_src("print 1;", 8);
    m.funcs[0].code = {0xEE, OP_HALT};
    m.funcs[0].num_regs = 4;
    CHECK(!verify(m).ok);
  }

  // Jump landing outside the code is rejected.
  {
    Module m = compile_src("print 1;", 8);
    m.funcs[0].code = {OP_JUMP, 0x7f, 0x7f, OP_HALT};  // huge forward offset
    m.funcs[0].num_regs = 4;
    CHECK(!verify(m).ok);
  }
}
