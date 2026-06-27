#include "astprint.h"

#include "ast.h"

#include <cstdio>
#include <string>

// ============================================================================
//  astprint.cc — token / AST dumps and a canonical source formatter
// ============================================================================
//
//  None of this code runs on untrusted input: the dumps and the formatter take
//  an already-lexed token stream or an already-parsed AST and only read them,
//  so they never allocate on the VM heap and never touch the interpreter. They
//  exist for the `tokens`, `ast`, and `fmt` CLI commands and for debugging.
//
//  The formatter's one invariant worth stating: it fully parenthesizes every
//  compound sub-expression. That sidesteps any operator-precedence bookkeeping
//  and guarantees the emitted source re-parses to an equivalent tree, which the
//  unit tests pin down by compiling and running both the original and the
//  formatted program and comparing their output.
//
// ============================================================================

namespace coal {

namespace {

// --- name tables --------------------------------------------------------

const char* tok_name(Tok k) {
  switch (k) {
    case Tok::Eof:       return "Eof";
    case Tok::Int:       return "Int";
    case Tok::Float:     return "Float";
    case Tok::Str:       return "Str";
    case Tok::Ident:     return "Ident";
    case Tok::Plus:      return "Plus";
    case Tok::Minus:     return "Minus";
    case Tok::Star:      return "Star";
    case Tok::Slash:     return "Slash";
    case Tok::LParen:    return "LParen";
    case Tok::RParen:    return "RParen";
    case Tok::LBrace:    return "LBrace";
    case Tok::RBrace:    return "RBrace";
    case Tok::LBracket:  return "LBracket";
    case Tok::RBracket:  return "RBracket";
    case Tok::Comma:     return "Comma";
    case Tok::Dot:       return "Dot";
    case Tok::Assign:    return "Assign";
    case Tok::Semicolon: return "Semicolon";
    case Tok::EqEq:      return "EqEq";
    case Tok::BangEq:    return "BangEq";
    case Tok::Lt:        return "Lt";
    case Tok::Le:        return "Le";
    case Tok::Gt:        return "Gt";
    case Tok::Ge:        return "Ge";
    case Tok::AmpAmp:    return "AmpAmp";
    case Tok::PipePipe:  return "PipePipe";
    case Tok::Bang:      return "Bang";
    case Tok::Percent:   return "Percent";
    case Tok::Amp:       return "Amp";
    case Tok::Pipe:      return "Pipe";
    case Tok::Caret:     return "Caret";
    case Tok::Tilde:     return "Tilde";
    case Tok::Shl:       return "Shl";
    case Tok::Shr:       return "Shr";
    case Tok::KwFn:      return "KwFn";
    case Tok::KwReturn:  return "KwReturn";
    case Tok::KwIf:      return "KwIf";
    case Tok::KwElse:    return "KwElse";
    case Tok::KwWhile:   return "KwWhile";
    case Tok::KwNil:     return "KwNil";
    case Tok::KwTrue:    return "KwTrue";
    case Tok::KwFalse:   return "KwFalse";
    case Tok::KwPrint:   return "KwPrint";
    case Tok::KwFor:     return "KwFor";
    case Tok::KwBreak:   return "KwBreak";
    case Tok::KwContinue:return "KwContinue";
    case Tok::KwElif:    return "KwElif";
    case Tok::Error:     return "Error";
  }
  return "?";
}

const char* node_name(NodeKind k) {
  switch (k) {
    case NodeKind::IntLit:   return "IntLit";
    case NodeKind::FloatLit: return "FloatLit";
    case NodeKind::StrLit:   return "StrLit";
    case NodeKind::BoolLit:  return "BoolLit";
    case NodeKind::NilLit:   return "NilLit";
    case NodeKind::Ident:    return "Ident";
    case NodeKind::Binary:   return "Binary";
    case NodeKind::Unary:    return "Unary";
    case NodeKind::Logical:  return "Logical";
    case NodeKind::Assign:   return "Assign";
    case NodeKind::Index:    return "Index";
    case NodeKind::Field:    return "Field";
    case NodeKind::Call:     return "Call";
    case NodeKind::ArrayLit: return "ArrayLit";
    case NodeKind::MapLit:   return "MapLit";
    case NodeKind::Lambda:   return "Lambda";
    case NodeKind::ExprStmt: return "ExprStmt";
    case NodeKind::Print:    return "Print";
    case NodeKind::Return:   return "Return";
    case NodeKind::If:       return "If";
    case NodeKind::While:    return "While";
    case NodeKind::For:      return "For";
    case NodeKind::Break:    return "Break";
    case NodeKind::Continue: return "Continue";
    case NodeKind::Block:    return "Block";
    case NodeKind::FnDecl:   return "FnDecl";
    case NodeKind::Program:  return "Program";
  }
  return "?";
}

// Binary/Logical/Assign operators are stored as a single tag byte; map each one
// back to its source spelling. (See the kids-shape contract in ast.h.)
const char* binop_str(char op) {
  switch (op) {
    case '+': return "+";   case '-': return "-";  case '*': return "*";
    case '/': return "/";   case '%': return "%";
    case 'E': return "==";  case 'N': return "!=";
    case '<': return "<";   case 'l': return "<="; case '>': return ">"; case 'g': return ">=";
    case '&': return "&";   case '|': return "|";  case '^': return "^";
    case 'L': return "<<";  case 'R': return ">>";
    case 'a': return "&&";  case 'o': return "||";
    case '=': return "=";
    default:  return "?";
  }
}
const char* unop_str(char op) {
  switch (op) {
    case '-': return "-";
    case '!': return "!";
    case '~': return "~";
    default:  return "?";
  }
}

// --- AST dump -----------------------------------------------------------

void dump_node(const Node* n, int depth, std::string& out) {
  if (!n) return;
  out.append(static_cast<size_t>(depth) * 2, ' ');
  out += node_name(n->kind);
  switch (n->kind) {
    case NodeKind::IntLit:   out += ' '; out += std::to_string(n->ival); break;
    case NodeKind::FloatLit: out += ' '; out += std::to_string(n->dval); break;
    case NodeKind::StrLit:   out += " \""; out += n->str; out += '"';     break;
    case NodeKind::BoolLit:  out += n->bval ? " true" : " false";         break;
    case NodeKind::Ident:    out += ' '; out += n->str;                   break;
    case NodeKind::Field:    out += " ."; out += n->str;                  break;
    case NodeKind::Call:     out += ' '; out += n->str;                   break;
    case NodeKind::Binary:
    case NodeKind::Logical:
    case NodeKind::Assign:   out += ' '; out += binop_str(n->op);         break;
    case NodeKind::Unary:    out += ' '; out += unop_str(n->op);          break;
    case NodeKind::FnDecl:
    case NodeKind::Lambda: {
      if (n->kind == NodeKind::FnDecl) { out += ' '; out += n->str; }
      out += '(';
      for (size_t i = 0; i < n->params.size(); ++i) { if (i) out += ", "; out += n->params[i]; }
      out += ')';
      break;
    }
    default: break;
  }
  out += '\n';
  for (const Node* k : n->kids) dump_node(k, depth + 1, out);
}

// --- source formatter ---------------------------------------------------

bool is_compound(const Node* n) {
  return n && (n->kind == NodeKind::Binary || n->kind == NodeKind::Logical ||
               n->kind == NodeKind::Unary || n->kind == NodeKind::Assign);
}

// Whether an expression statement ends with a `}` (a lambda body, possibly via
// an assignment to one). Such statements are self-terminating in Charcoal — the
// closing brace ends them — so the formatter omits the trailing semicolon, the
// same way a block-bodied `fn` declaration takes none.
bool ends_in_block(const Node* n) {
  if (!n) return false;
  if (n->kind == NodeKind::Lambda) return true;
  if (n->kind == NodeKind::Assign) return ends_in_block(n->kids[1]);
  return false;
}

void emit_str_lit(const std::string& s, std::string& out) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      case '\t': out += "\\t";  break;
      case '\r': out += "\\r";  break;
      default:   out += c;      break;
    }
  }
  out += '"';
}

struct Printer {
  std::string out;

  void pad(int d) { out.append(static_cast<size_t>(d) * 2, ' '); }

  // An operand of an operator: parenthesized when it is itself compound, so the
  // tree shape is preserved without consulting a precedence table.
  void operand(const Node* n, int d) {
    if (is_compound(n)) { out += '('; expr(n, d); out += ')'; }
    else expr(n, d);
  }

  void expr(const Node* n, int d) {
    if (!n) return;
    switch (n->kind) {
      case NodeKind::IntLit:   out += std::to_string(n->ival); break;
      case NodeKind::FloatLit: out += std::to_string(n->dval); break;
      case NodeKind::StrLit:   emit_str_lit(n->str, out);      break;
      case NodeKind::BoolLit:  out += n->bval ? "true" : "false"; break;
      case NodeKind::NilLit:   out += "nil";                   break;
      case NodeKind::Ident:    out += n->str;                  break;

      case NodeKind::Binary:
      case NodeKind::Logical:
        operand(n->kids[0], d);
        out += ' '; out += binop_str(n->op); out += ' ';
        operand(n->kids[1], d);
        break;

      case NodeKind::Unary:
        out += unop_str(n->op);
        operand(n->kids[0], d);
        break;

      case NodeKind::Assign:
        expr(n->kids[0], d);          // target: Ident, Index, or Field
        out += " = ";
        expr(n->kids[1], d);          // RHS is unambiguous after `=`, no parens
        break;

      case NodeKind::Index:
        operand(n->kids[0], d);
        out += '['; expr(n->kids[1], d); out += ']';
        break;

      case NodeKind::Field:
        operand(n->kids[0], d);
        out += '.'; out += n->str;
        break;

      case NodeKind::Call:
        out += n->str; out += '(';
        for (size_t i = 0; i < n->kids.size(); ++i) { if (i) out += ", "; expr(n->kids[i], d); }
        out += ')';
        break;

      case NodeKind::ArrayLit:
        out += '[';
        for (size_t i = 0; i < n->kids.size(); ++i) { if (i) out += ", "; expr(n->kids[i], d); }
        out += ']';
        break;

      case NodeKind::MapLit:
        out += '{';
        for (size_t i = 0; i + 1 < n->kids.size(); i += 2) {
          if (i) out += ", ";
          expr(n->kids[i], d);          // key (a StrLit)
          out += " = ";
          expr(n->kids[i + 1], d);
        }
        out += '}';
        break;

      case NodeKind::Lambda:
        out += "fn(";
        for (size_t i = 0; i < n->params.size(); ++i) { if (i) out += ", "; out += n->params[i]; }
        out += ") ";
        block(n->kids[0], d);
        break;

      default: break;
    }
  }

  // Emit a `{ ... }` block. Children are printed one per line at depth d+1; an
  // empty block collapses to `{}`.
  void block(const Node* n, int d) {
    out += '{';
    if (!n || n->kids.empty()) { out += '}'; return; }
    out += '\n';
    for (const Node* k : n->kids) {
      pad(d + 1);
      stmt(k, d + 1);
      out += '\n';
    }
    pad(d);
    out += '}';
  }

  // A `for` clause is either an omitted-slot sentinel (an empty Block), a bare
  // expression, or an ExprStmt wrapping one. Returns the source for the slot.
  void clause(const Node* n, int d) {
    if (!n) return;
    if (n->kind == NodeKind::Block) return;            // empty-slot sentinel
    if (n->kind == NodeKind::ExprStmt) { expr(n->kids[0], d); return; }
    expr(n, d);
  }

  // Emit a statement WITHOUT leading indentation or a trailing newline; the
  // caller positions it. Nested blocks indent their own contents at depth d.
  void stmt(const Node* n, int d) {
    if (!n) return;
    switch (n->kind) {
      case NodeKind::ExprStmt: expr(n->kids[0], d); out += ';'; break;
      case NodeKind::Print:    out += "print "; expr(n->kids[0], d); out += ';'; break;
      case NodeKind::Return:
        out += "return";
        if (!n->kids.empty()) { out += ' '; expr(n->kids[0], d); }
        out += ';';
        break;
      case NodeKind::Break:    out += "break;"; break;
      case NodeKind::Continue: out += "continue;"; break;
      case NodeKind::Block:    block(n, d); break;

      case NodeKind::If: {
        out += "if ("; expr(n->kids[0], d); out += ") ";
        block(n->kids[1], d);
        // An `elif` is parsed as an If nested in the else slot. Re-emit the
        // chain with the `elif` keyword (the language has no `else if` form),
        // and a plain trailing else as `else { ... }`.
        const Node* cur = n;
        while (cur->kids.size() == 3) {
          const Node* els = cur->kids[2];
          if (els->kind == NodeKind::If) {
            out += " elif ("; expr(els->kids[0], d); out += ") ";
            block(els->kids[1], d);
            cur = els;
          } else {
            out += " else ";
            block(els, d);
            break;
          }
        }
        break;
      }

      case NodeKind::While:
        out += "while ("; expr(n->kids[0], d); out += ") ";
        block(n->kids[1], d);
        break;

      case NodeKind::For:
        out += "for (";
        clause(n->kids[0], d); out += "; ";
        clause(n->kids[1], d); out += "; ";
        clause(n->kids[2], d); out += ") ";
        block(n->kids[3], d);
        break;

      case NodeKind::FnDecl:
        out += "fn "; out += n->str; out += '(';
        for (size_t i = 0; i < n->params.size(); ++i) { if (i) out += ", "; out += n->params[i]; }
        out += ") ";
        block(n->kids[0], d);
        break;

      default:  // an expression used in statement position
        expr(n, d);
        out += ';';
        break;
    }
  }
};

}  // namespace

std::string dump_tokens(const std::vector<Token>& toks) {
  std::string out;
  char pos[24];
  for (const Token& t : toks) {
    std::snprintf(pos, sizeof pos, "%4d:%-3d ", t.line, t.col);
    out += pos;
    out += tok_name(t.kind);
    switch (t.kind) {
      case Tok::Int:   out += ' '; out += std::to_string(t.ival); break;
      case Tok::Float: out += ' '; out += std::to_string(t.dval); break;
      case Tok::Str:   out += " \""; out.append(t.start, t.len); out += '"'; break;
      case Tok::Ident: out += ' '; out.append(t.start, t.len); break;
      default: break;
    }
    out += '\n';
  }
  return out;
}

std::string dump_ast(const Node* program) {
  std::string out;
  dump_node(program, 0, out);
  return out;
}

std::string format_source(const Node* program) {
  Printer p;
  if (!program) return p.out;
  for (const Node* stmt : program->kids) {
    p.stmt(stmt, 0);
    p.out += '\n';
  }
  return p.out;
}

}  // namespace coal
