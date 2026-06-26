#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coal {

enum class NodeKind {
  // expressions
  IntLit, FloatLit, StrLit, BoolLit, NilLit, Ident,
  Binary, Assign, Index, Field, Call, ArrayLit, MapLit,
  // statements
  ExprStmt, Print, Return, If, While, Block, FnDecl, Program
};

struct Node {
  NodeKind kind;
  int      line = 0;

  int64_t     ival = 0;   // IntLit
  double      dval = 0;   // FloatLit
  std::string str;        // StrLit (unescaped), Ident/Field/FnDecl name
  bool        bval = false;  // BoolLit
  char        op = 0;     // Binary / Assign operator: + - * / =

  std::vector<std::string> params;  // FnDecl parameter names
  std::vector<Node*> kids;  // operands / children (see per-kind shape below)
};

// Per-kind kids shape (the contract the compiler in Task 4 relies on):
//   Binary   : [lhs, rhs]              op in {+,-,*,/}
//   Assign   : [target, value]         target is Ident | Index | Field
//   Index    : [container, index]
//   Field    : [container]             field name in str
//   Call      : [arg0, arg1, ...]       callee name in str
//   ArrayLit : [elem0, elem1, ...]
//   MapLit   : [key0, val0, key1, val1, ...]  keys are StrLit
//   ExprStmt : [expr]
//   Print    : [expr]
//   Return   : [expr]   (empty kids => return nil)
//   If       : [cond, thenBlock]  or  [cond, thenBlock, elseBlock]
//   While    : [cond, body]
//   Block    : [stmt0, stmt1, ...]
//   FnDecl   : [body]   name in str, params in `params`
//   Program  : [stmt-or-fndecl, ...]

struct ParseResult {
  Node*       program = nullptr;  // a Program node (owned by `arena`)
  std::string error;              // non-empty iff parsing failed
  int         err_line = 0;
  std::vector<std::unique_ptr<Node>> arena;  // owns every Node
};

}  // namespace coal
