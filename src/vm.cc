#include "charcoal.h"

#include "compiler.h"
#include "heap.h"
#include "lexer.h"
#include "loader.h"
#include "optimize.h"
#include "parser.h"
#include "verifier.h"

namespace coal {

bool compile_source(const char* src, size_t n,
                    std::vector<uint8_t>& out_cbc, std::string& err) {
  auto l = lex(src, n);
  if (!l.error.empty()) { err = l.error; return false; }

  auto p = parse(l.tokens);
  if (!p.error.empty() || !p.program) { err = p.error; return false; }

  fold_constants(p.program);  // collapse constant sub-expressions before lowering

  auto c = compile(p.program);
  if (!c.ok) { err = c.error; return false; }

  out_cbc = serialize(c.module);
  return true;
}

RunResult run_cbc(const uint8_t* data, size_t size, Limits limits) {
  RunResult r;

  auto ld = load_cbc(data, size);
  if (!ld.ok) { r.error = ld.error; return r; }  // r.ok stays false

  auto v = verify(ld.module);
  if (!v.ok) { r.error = v.error; return r; }

  Heap h(8u << 20);
  return run(ld.module, h, limits);
}

}  // namespace coal
