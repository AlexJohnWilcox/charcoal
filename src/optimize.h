#pragma once

namespace coal {

struct Node;

// Constant-fold an AST in place. A Binary/Unary/Logical node whose operands are
// all literals is replaced by the literal it evaluates to, using the SAME
// numeric semantics as the interpreter: wrapping integer arithmetic, integer
// division/modulo guarded against divide-by-zero and INT64_MIN/-1, logical
// shifts with an in-range shift count, IEEE-754 doubles when either operand is a
// float, and Bool results for comparisons and logical connectives.
//
// Anything that would raise a runtime error (divide by zero, out-of-range
// shift, bitwise op on a float) or produce a non-finite double is deliberately
// left unfolded, so the interpreter still observes exactly the behavior it would
// have without the pass. Logical `&&`/`||` are folded only when both operands
// are literal booleans, sidestepping any value-vs-bool result question.
//
// This runs at compile time on a trusted, freshly parsed AST — never on
// untrusted bytecode — so it allocates nothing on the VM heap.
void fold_constants(Node* program);

}  // namespace coal
