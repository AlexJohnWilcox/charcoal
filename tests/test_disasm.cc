#include "compiler.h"
#include "disasm.h"
#include "lexer.h"
#include "module.h"
#include "parser.h"
#include "test_main.h"

#include <string>

using namespace coal;

static bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

void test_disasm() {
  // A normal program disassembles to readable mnemonics + its function name.
  {
    auto l = lex("fn dbl(a) { return a + a; } print dbl(2) + 3;", 45);
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    std::string d = disassemble(c.module);
    CHECK(contains(d, "LOAD_CONST"));
    CHECK(contains(d, "ADD"));
    CHECK(contains(d, "CALL"));
    CHECK(contains(d, "dbl"));     // function name shown
    CHECK(contains(d, "RET"));     // dbl returns
  }

  // Defensive: a truncated final instruction is reported, not read past.
  {
    Module m;
    Constant name; name.tag = CTag::Str; name.s = "main";
    m.consts.push_back(name);
    Function f;
    f.name_const = 0; f.num_params = 0; f.num_regs = 1; f.max_stack = 0;
    f.code = {OP_LOAD_CONST, 0};   // needs r + k(2 bytes); only r present
    m.funcs.push_back(f);
    m.entry_func = 0;
    std::string d = disassemble(m);  // must not crash / overread
    CHECK(contains(d, "truncated"));
  }
}
