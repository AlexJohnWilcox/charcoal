#include "compiler.h"
#include "lexer.h"
#include "loader.h"
#include "module.h"
#include "parser.h"
#include "test_main.h"

using namespace coal;

void test_module_roundtrip() {
  auto l = lex("fn add(a,b){return a+b;} print add(3,4);", 40);
  auto p = parse(l.tokens);
  auto c = compile(p.program);
  CHECK(c.ok);

  auto bytes = serialize(c.module);
  CHECK(!bytes.empty());

  auto r = load_cbc(bytes.data(), bytes.size());
  CHECK(r.ok);
  CHECK(r.module.funcs.size() == c.module.funcs.size());
  CHECK(r.module.consts.size() == c.module.consts.size());
  CHECK(r.module.entry_func == c.module.entry_func);

  // Function code survives the round-trip byte-for-byte.
  CHECK(r.module.funcs[0].code == c.module.funcs[0].code);
  CHECK(r.module.funcs[1].num_params == c.module.funcs[1].num_params);
}
