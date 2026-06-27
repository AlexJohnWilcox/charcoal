#include "optimize.h"

#include "ast.h"

#include <cmath>
#include <cstdint>
#include <limits>

// ============================================================================
//  optimize.cc — constant folding over the AST
// ============================================================================
//
//  A single post-order walk: fold each node's children first, then fold the
//  node itself if it has become a constant expression. Because folding is
//  in-place (a Binary node is rewritten into an IntLit / FloatLit / BoolLit and
//  its kids dropped), nested constants collapse in one pass — `(1 + 2) * 3`
//  folds the `1 + 2` to `3`, then the multiply to `9`.
//
//  Correctness rests on matching the interpreter's arithmetic exactly (see
//  interp.cc): integer ops wrap through uint64, division/modulo bail on the two
//  trapping cases, shifts require a [0,64) count, and any float operand promotes
//  the whole operation to double. Operations the interpreter would reject at
//  runtime are left intact so the program's observable behavior is unchanged.
//
// ============================================================================

namespace coal {

namespace {

// Wrapping integer arithmetic, done in uint64 so signed overflow stays defined
// (mirrors interp.cc's wadd/wsub/wmul).
int64_t wadd(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)); }
int64_t wsub(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b)); }
int64_t wmul(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b)); }

// Rewrite a node into a literal, discarding its (now-dead) operand subtrees.
// The orphaned children remain owned by the parse arena; nothing is freed here.
void set_int(Node* n, int64_t v)  { n->kind = NodeKind::IntLit;  n->ival = v; n->kids.clear(); }
void set_bool(Node* n, bool v)    { n->kind = NodeKind::BoolLit; n->bval = v; n->kids.clear(); }
void set_float(Node* n, double v) { n->kind = NodeKind::FloatLit; n->dval = v; n->kids.clear(); }

bool num_lit(const Node* n, double& d) {
  if (n->kind == NodeKind::IntLit)   { d = static_cast<double>(n->ival); return true; }
  if (n->kind == NodeKind::FloatLit) { d = n->dval; return true; }
  return false;
}

// Truthiness of a *pure* literal — one with no sub-expressions, hence no side
// effects to preserve. Matches Value::truthy(): nil/false/0/0.0 are falsey, and
// any string object is truthy.
bool lit_truthy(const Node* n, bool& t) {
  switch (n->kind) {
    case NodeKind::IntLit:   t = (n->ival != 0); return true;
    case NodeKind::FloatLit: t = (n->dval != 0); return true;
    case NodeKind::BoolLit:  t = n->bval;        return true;
    case NodeKind::NilLit:   t = false;          return true;
    case NodeKind::StrLit:   t = true;           return true;
    default: return false;
  }
}

void fold_binary(Node* n) {
  const Node* a = n->kids[0];
  const Node* b = n->kids[1];
  const char op = n->op;
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

  if (a->kind == NodeKind::IntLit && b->kind == NodeKind::IntLit) {
    const int64_t x = a->ival, y = b->ival;
    switch (op) {
      case '+': set_int(n, wadd(x, y)); break;
      case '-': set_int(n, wsub(x, y)); break;
      case '*': set_int(n, wmul(x, y)); break;
      case '/': if (y == 0 || (x == kMin && y == -1)) return; set_int(n, x / y); break;
      case '%': if (y == 0 || (x == kMin && y == -1)) return; set_int(n, x % y); break;
      case 'E': set_bool(n, x == y); break;
      case 'N': set_bool(n, x != y); break;
      case '<': set_bool(n, x <  y); break;
      case 'l': set_bool(n, x <= y); break;
      case '>': set_bool(n, x >  y); break;
      case 'g': set_bool(n, x >= y); break;
      case '&': set_int(n, x & y); break;
      case '|': set_int(n, x | y); break;
      case '^': set_int(n, x ^ y); break;
      case 'L': if (y < 0 || y >= 64) return; set_int(n, static_cast<int64_t>(static_cast<uint64_t>(x) << y)); break;
      case 'R': if (y < 0 || y >= 64) return; set_int(n, static_cast<int64_t>(static_cast<uint64_t>(x) >> y)); break;
      default: break;
    }
    return;
  }

  double x, y;
  if (num_lit(a, x) && num_lit(b, y)) {   // mixed/float operands -> double math
    double r;
    switch (op) {
      case '+': r = x + y; break;
      case '-': r = x - y; break;
      case '*': r = x * y; break;
      case '/': r = x / y; break;
      case '%': r = std::fmod(x, y); break;
      case 'E': set_bool(n, x == y); return;
      case 'N': set_bool(n, x != y); return;
      case '<': set_bool(n, x <  y); return;
      case 'l': set_bool(n, x <= y); return;
      case '>': set_bool(n, x >  y); return;
      case 'g': set_bool(n, x >= y); return;
      default: return;   // bitwise / shift on a float would be a runtime error
    }
    if (std::isfinite(r)) set_float(n, r);   // leave inf/nan to the interpreter
  }
}

void fold_unary(Node* n) {
  const Node* k = n->kids[0];
  switch (n->op) {
    case '-':
      if (k->kind == NodeKind::IntLit)        set_int(n, static_cast<int64_t>(0u - static_cast<uint64_t>(k->ival)));
      else if (k->kind == NodeKind::FloatLit) set_float(n, -k->dval);
      break;
    case '~':
      if (k->kind == NodeKind::IntLit) set_int(n, ~k->ival);
      break;
    case '!': {
      bool t;
      if (lit_truthy(k, t)) set_bool(n, !t);
      break;
    }
    default: break;
  }
}

// Logical connectives yield a Bool. Fold only when both operands are literal
// booleans, which makes the result independent of any value-vs-bool subtlety
// and free of skipped side effects.
void fold_logical(Node* n) {
  if (n->kids[0]->kind == NodeKind::BoolLit && n->kids[1]->kind == NodeKind::BoolLit) {
    const bool l = n->kids[0]->bval, r = n->kids[1]->bval;
    set_bool(n, n->op == 'a' ? (l && r) : (l || r));
  }
}

void fold(Node* n) {
  if (!n) return;
  for (Node* k : n->kids) fold(k);   // post-order: fold children before the node
  switch (n->kind) {
    case NodeKind::Binary:  fold_binary(n);  break;
    case NodeKind::Unary:   fold_unary(n);   break;
    case NodeKind::Logical: fold_logical(n); break;
    default: break;
  }
}

}  // namespace

void fold_constants(Node* program) { fold(program); }

}  // namespace coal
