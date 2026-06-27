#include "parser.h"

#include <string>
#include <utility>

namespace coal {

namespace {

constexpr int kMaxDepth = 200;

// Binary/logical precedence (higher binds tighter). 0 = not a binary operator.
int bin_prec(Tok k) {
  switch (k) {
    case Tok::PipePipe: return 1;
    case Tok::AmpAmp:   return 2;
    case Tok::EqEq:
    case Tok::BangEq:   return 3;
    case Tok::Lt:
    case Tok::Le:
    case Tok::Gt:
    case Tok::Ge:       return 4;
    case Tok::Pipe:     return 5;
    case Tok::Caret:    return 6;
    case Tok::Amp:      return 7;
    case Tok::Shl:
    case Tok::Shr:      return 8;
    case Tok::Plus:
    case Tok::Minus:    return 9;
    case Tok::Star:
    case Tok::Slash:
    case Tok::Percent:  return 10;
    default:            return 0;
  }
}

// Maps a token to its documented char op code. 'a'/'o' denote the short-circuit
// logical operators (Logical nodes); everything else is a Binary node.
char bin_op(Tok k) {
  switch (k) {
    case Tok::Plus:     return '+';
    case Tok::Minus:    return '-';
    case Tok::Star:     return '*';
    case Tok::Slash:    return '/';
    case Tok::Percent:  return '%';
    case Tok::EqEq:     return 'E';
    case Tok::BangEq:   return 'N';
    case Tok::Lt:       return '<';
    case Tok::Le:       return 'l';
    case Tok::Gt:       return '>';
    case Tok::Ge:       return 'g';
    case Tok::Amp:      return '&';
    case Tok::Pipe:     return '|';
    case Tok::Caret:    return '^';
    case Tok::Shl:      return 'L';
    case Tok::Shr:      return 'R';
    case Tok::AmpAmp:   return 'a';
    case Tok::PipePipe: return 'o';
    default:            return 0;
  }
}

struct Parser {
  const std::vector<Token>& toks;
  ParseResult&              r;
  size_t                    pos = 0;
  int                       depth = 0;

  Parser(const std::vector<Token>& t, ParseResult& res) : toks(t), r(res) {}

  // RAII bound on recursion so malformed deeply-nested input errors instead of
  // overflowing the native stack.
  struct DepthGuard {
    Parser& p;
    bool ok;
    explicit DepthGuard(Parser& p_) : p(p_), ok(++p.depth <= kMaxDepth) {}
    ~DepthGuard() { --p.depth; }
  };

  const Token& peek() const { return toks[pos]; }
  bool check(Tok k) const { return toks[pos].kind == k; }

  const Token& advance() {
    const Token& t = toks[pos];
    if (toks[pos].kind != Tok::Eof) ++pos;  // never run past the Eof sentinel
    return t;
  }

  bool match(Tok k) {
    if (check(k)) { advance(); return true; }
    return false;
  }

  void fail(const char* msg) {
    if (r.error.empty()) {
      r.err_line = peek().line;
      r.err_col = peek().col;
      r.error = std::to_string(r.err_line) + ":" + std::to_string(r.err_col) + ": " + msg;
    }
  }

  bool expect(Tok k, const char* msg) {
    if (check(k)) { advance(); return true; }
    fail(msg);
    return false;
  }

  Node* make(NodeKind k) {
    r.arena.push_back(std::make_unique<Node>());
    Node* n = r.arena.back().get();
    n->kind = k;
    n->line = peek().line;
    return n;
  }

  bool failed() const { return !r.error.empty(); }

  // --- expressions -------------------------------------------------------

  std::string unescape(const Token& t) {
    std::string out;
    if (t.len < 2) return out;  // strip surrounding quotes
    const char* p = t.start + 1;
    uint32_t n = t.len - 2;
    for (uint32_t i = 0; i < n; ++i) {
      char c = p[i];
      if (c == '\\' && i + 1 < n) {
        char e = p[++i];
        switch (e) {
          case 'n':  out += '\n'; break;
          case 't':  out += '\t'; break;
          case '"':  out += '"';  break;
          case '\\': out += '\\'; break;
          default:   out += e;    break;
        }
      } else {
        out += c;
      }
    }
    return out;
  }

  Node* primary() {
    const Token& t = peek();
    switch (t.kind) {
      case Tok::Int:     { Node* n = make(NodeKind::IntLit);   n->ival = t.ival; advance(); return n; }
      case Tok::Float:   { Node* n = make(NodeKind::FloatLit); n->dval = t.dval; advance(); return n; }
      case Tok::Str:     { Node* n = make(NodeKind::StrLit);   n->str = unescape(t); advance(); return n; }
      case Tok::KwTrue:  { Node* n = make(NodeKind::BoolLit);  n->bval = true;  advance(); return n; }
      case Tok::KwFalse: { Node* n = make(NodeKind::BoolLit);  n->bval = false; advance(); return n; }
      case Tok::KwNil:   { Node* n = make(NodeKind::NilLit);   advance(); return n; }
      case Tok::Ident:   { Node* n = make(NodeKind::Ident);    n->str.assign(t.start, t.len); advance(); return n; }
      case Tok::LParen: {
        advance();
        Node* e = expr(1);
        if (!e || failed()) return nullptr;
        if (!expect(Tok::RParen, "expected ')'")) return nullptr;
        return e;
      }
      case Tok::LBracket: return array_lit();
      case Tok::LBrace:   return map_lit();
      case Tok::KwFn:     return lambda_expr();  // fn(params){body} as a value
      default:
        fail("expected expression");
        return nullptr;
    }
  }

  // fn ( params ) block  — an unnamed function expression. The leading `fn` has
  // not been consumed. (A `fn name(...)` at statement level stays a FnDecl.)
  Node* lambda_expr() {
    advance();  // fn
    if (!expect(Tok::LParen, "expected '(' in lambda")) return nullptr;
    std::vector<std::string> params;
    if (!check(Tok::RParen)) {
      do {
        if (!check(Tok::Ident)) { fail("expected parameter name"); return nullptr; }
        params.emplace_back(peek().start, peek().len);
        advance();
      } while (match(Tok::Comma));
    }
    if (!expect(Tok::RParen, "expected ')'")) return nullptr;
    Node* body = block();
    if (!body || failed()) return nullptr;
    Node* n = make(NodeKind::Lambda);
    n->params = std::move(params);
    n->kids = {body};
    return n;
  }

  Node* array_lit() {
    Node* n = make(NodeKind::ArrayLit);
    advance();  // [
    if (!check(Tok::RBracket)) {
      do {
        Node* e = expr(1);
        if (!e || failed()) return nullptr;
        n->kids.push_back(e);
      } while (match(Tok::Comma));
    }
    if (!expect(Tok::RBracket, "expected ']'")) return nullptr;
    return n;
  }

  // Map literal: { key = value, ... } with StrLit keys. The lexer has no ':'
  // token, so '=' is the separator. kids = [key0, val0, key1, val1, ...].
  Node* map_lit() {
    Node* n = make(NodeKind::MapLit);
    advance();  // {
    if (!check(Tok::RBrace)) {
      do {
        Node* k = expr(1);
        if (!k || failed()) return nullptr;
        if (!expect(Tok::Assign, "expected '=' in map literal")) return nullptr;
        Node* v = expr(1);
        if (!v || failed()) return nullptr;
        n->kids.push_back(k);
        n->kids.push_back(v);
      } while (match(Tok::Comma));
    }
    if (!expect(Tok::RBrace, "expected '}'")) return nullptr;
    return n;
  }

  Node* postfix(Node* base) {
    while (base && !failed()) {
      if (check(Tok::LBracket)) {
        advance();
        Node* idx = expr(1);
        if (!idx || failed()) return nullptr;
        if (!expect(Tok::RBracket, "expected ']'")) return nullptr;
        Node* n = make(NodeKind::Index);
        n->kids = {base, idx};
        base = n;
      } else if (check(Tok::Dot)) {
        advance();
        if (!check(Tok::Ident)) { fail("expected field name"); return nullptr; }
        Node* n = make(NodeKind::Field);
        n->str.assign(peek().start, peek().len);
        n->kids = {base};
        advance();
        base = n;
      } else if (check(Tok::LParen)) {
        if (base->kind != NodeKind::Ident) { fail("can only call a name"); return nullptr; }
        std::string callee = base->str;
        advance();
        Node* n = make(NodeKind::Call);
        n->str = std::move(callee);
        if (!check(Tok::RParen)) {
          do {
            Node* a = expr(1);
            if (!a || failed()) return nullptr;
            n->kids.push_back(a);
          } while (match(Tok::Comma));
        }
        if (!expect(Tok::RParen, "expected ')'")) return nullptr;
        base = n;
      } else {
        break;
      }
    }
    return base;
  }

  // Prefix unary: -x, !x, ~x. Binds tighter than any binary operator and is
  // chainable (!!x, - -x). The operand is itself a unary() so postfix (index /
  // call / field) still binds tighter than the prefix.
  Node* unary() {
    DepthGuard g(*this);
    if (!g.ok) { fail("nesting too deep"); return nullptr; }

    char op = 0;
    switch (peek().kind) {
      case Tok::Minus: op = '-'; break;
      case Tok::Bang:  op = '!'; break;
      case Tok::Tilde: op = '~'; break;
      default: break;
    }
    if (op) {
      advance();
      Node* operand = unary();
      if (!operand || failed()) return nullptr;
      Node* n = make(NodeKind::Unary);
      n->op = op;
      n->kids = {operand};
      return n;
    }
    return postfix(primary());
  }

  // Pratt / precedence climbing. minPrec is the lowest binding power this call
  // will consume.
  Node* expr(int minPrec) {
    DepthGuard g(*this);
    if (!g.ok) { fail("nesting too deep"); return nullptr; }

    Node* left = unary();
    if (!left || failed()) return nullptr;

    while (true) {
      int prec = bin_prec(peek().kind);
      if (prec == 0 || prec < minPrec) break;
      char op = bin_op(peek().kind);
      advance();
      Node* right = expr(prec + 1);  // left-associative
      if (!right || failed()) return nullptr;
      Node* n = make((op == 'a' || op == 'o') ? NodeKind::Logical : NodeKind::Binary);
      n->op = op;
      n->kids = {left, right};
      left = n;
    }
    return left;
  }

  // expr, then an optional `= value` (right associative) producing an Assign.
  Node* assignment() {
    DepthGuard g(*this);
    if (!g.ok) { fail("nesting too deep"); return nullptr; }

    Node* left = expr(1);
    if (!left || failed()) return nullptr;
    if (!check(Tok::Assign)) return left;

    advance();
    Node* value = assignment();
    if (!value || failed()) return nullptr;
    if (left->kind != NodeKind::Ident && left->kind != NodeKind::Index &&
        left->kind != NodeKind::Field) {
      fail("invalid assignment target");
      return nullptr;
    }
    Node* n = make(NodeKind::Assign);
    n->op = '=';
    n->kids = {left, value};
    return n;
  }

  // --- statements --------------------------------------------------------

  Node* block() {
    if (!expect(Tok::LBrace, "expected '{'")) return nullptr;
    Node* n = make(NodeKind::Block);
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
      Node* s = stmt();
      if (!s || failed()) return nullptr;
      n->kids.push_back(s);
    }
    if (!expect(Tok::RBrace, "expected '}'")) return nullptr;
    return n;
  }

  // Current token is `if` or `elif`. Parses `(cond) block` then chains: an `elif`
  // becomes a nested If in the else slot; an `else` becomes a Block there.
  Node* parse_if_after_keyword() {
    advance();  // consume 'if' / 'elif'
    if (!expect(Tok::LParen, "expected '(' after if")) return nullptr;
    Node* cond = assignment();
    if (!cond || failed()) return nullptr;
    if (!expect(Tok::RParen, "expected ')'")) return nullptr;
    Node* thenB = block();
    if (!thenB || failed()) return nullptr;
    Node* n = make(NodeKind::If);
    n->kids = {cond, thenB};
    if (check(Tok::KwElif)) {
      Node* elseIf = parse_if_after_keyword();  // nested If in the else slot
      if (!elseIf || failed()) return nullptr;
      n->kids.push_back(elseIf);
    } else if (match(Tok::KwElse)) {
      Node* elseB = block();
      if (!elseB || failed()) return nullptr;
      n->kids.push_back(elseB);
    }
    return n;
  }

  Node* if_stmt() { return parse_if_after_keyword(); }

  // A for init/step clause: an Assign stays a statement; a bare expression is
  // wrapped in ExprStmt so the compiler runs it for effect.
  Node* for_clause() {
    Node* e = assignment();
    if (!e || failed()) return nullptr;
    if (e->kind == NodeKind::Assign) return e;
    Node* s = make(NodeKind::ExprStmt);
    s->kids = {e};
    return s;
  }

  Node* for_stmt() {
    advance();  // for
    if (!expect(Tok::LParen, "expected '(' after for")) return nullptr;

    // init: optional clause, then ';'  (empty Block = omitted)
    Node* init;
    if (check(Tok::Semicolon)) init = make(NodeKind::Block);
    else { init = for_clause(); if (!init || failed()) return nullptr; }
    if (!expect(Tok::Semicolon, "expected ';' after for-init")) return nullptr;

    // cond: optional expression, then ';'
    Node* cond;
    if (check(Tok::Semicolon)) cond = make(NodeKind::Block);
    else { cond = assignment(); if (!cond || failed()) return nullptr; }
    if (!expect(Tok::Semicolon, "expected ';' after for-condition")) return nullptr;

    // step: optional clause, then ')'
    Node* step;
    if (check(Tok::RParen)) step = make(NodeKind::Block);
    else { step = for_clause(); if (!step || failed()) return nullptr; }
    if (!expect(Tok::RParen, "expected ')' after for-clauses")) return nullptr;

    Node* body = block();
    if (!body || failed()) return nullptr;

    Node* n = make(NodeKind::For);
    n->kids = {init, cond, step, body};  // always 4 kids
    return n;
  }

  Node* while_stmt() {
    advance();  // while
    if (!expect(Tok::LParen, "expected '(' after while")) return nullptr;
    Node* cond = assignment();
    if (!cond || failed()) return nullptr;
    if (!expect(Tok::RParen, "expected ')'")) return nullptr;
    Node* body = block();
    if (!body || failed()) return nullptr;
    Node* n = make(NodeKind::While);
    n->kids = {cond, body};
    return n;
  }

  Node* fn_decl() {
    advance();  // fn
    if (!check(Tok::Ident)) { fail("expected function name"); return nullptr; }
    std::string name(peek().start, peek().len);
    advance();
    if (!expect(Tok::LParen, "expected '(' after function name")) return nullptr;
    std::vector<std::string> params;
    if (!check(Tok::RParen)) {
      do {
        if (!check(Tok::Ident)) { fail("expected parameter name"); return nullptr; }
        params.emplace_back(peek().start, peek().len);
        advance();
      } while (match(Tok::Comma));
    }
    if (!expect(Tok::RParen, "expected ')'")) return nullptr;
    Node* body = block();
    if (!body || failed()) return nullptr;
    Node* n = make(NodeKind::FnDecl);
    n->str = std::move(name);
    n->params = std::move(params);
    n->kids = {body};
    return n;
  }

  Node* stmt() {
    DepthGuard g(*this);
    if (!g.ok) { fail("nesting too deep"); return nullptr; }

    switch (peek().kind) {
      case Tok::KwPrint: {
        advance();
        Node* e = assignment();
        if (!e || failed()) return nullptr;
        if (!expect(Tok::Semicolon, "expected ';'")) return nullptr;
        Node* n = make(NodeKind::Print);
        n->kids = {e};
        return n;
      }
      case Tok::KwReturn: {
        advance();
        Node* n = make(NodeKind::Return);
        if (!check(Tok::Semicolon)) {
          Node* e = assignment();
          if (!e || failed()) return nullptr;
          n->kids = {e};
        }
        if (!expect(Tok::Semicolon, "expected ';'")) return nullptr;
        return n;
      }
      case Tok::KwIf:    return if_stmt();
      case Tok::KwWhile: return while_stmt();
      case Tok::KwFor:   return for_stmt();
      case Tok::KwFn:    return fn_decl();
      case Tok::LBrace:  return block();
      case Tok::KwBreak: {
        advance();
        if (!expect(Tok::Semicolon, "expected ';'")) return nullptr;
        return make(NodeKind::Break);
      }
      case Tok::KwContinue: {
        advance();
        if (!expect(Tok::Semicolon, "expected ';'")) return nullptr;
        return make(NodeKind::Continue);
      }
      default: {
        Node* e = assignment();
        if (!e || failed()) return nullptr;
        // The terminating ';' is optional when the statement ends in a block
        // ('}'), e.g. `g = fn(x) { ... }` — like a function declaration.
        if (check(Tok::Semicolon)) {
          advance();
        } else if (!(pos > 0 && toks[pos - 1].kind == Tok::RBrace)) {
          fail("expected ';'");
          return nullptr;
        }
        if (e->kind == NodeKind::Assign) return e;
        Node* n = make(NodeKind::ExprStmt);
        n->kids = {e};
        return n;
      }
    }
  }

  Node* program() {
    Node* n = make(NodeKind::Program);
    while (!check(Tok::Eof)) {
      Node* s = stmt();
      if (!s || failed()) return nullptr;
      n->kids.push_back(s);
    }
    return n;
  }
};

}  // namespace

ParseResult parse(const std::vector<Token>& toks) {
  ParseResult r;
  if (toks.empty()) {  // lex() always appends Eof, but stay safe
    r.error = "empty token stream";
    return r;
  }
  Parser p(toks, r);
  Node* prog = p.program();
  if (r.error.empty()) r.program = prog;
  return r;
}

}  // namespace coal
