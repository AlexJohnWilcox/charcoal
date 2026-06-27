#include "lexer.h"
#include "parser.h"
#include "test_main.h"

#include <string>

using namespace coal;

void test_parser() {
  // Well-formed program parses to a Program node.
  {
    auto l = lex("print 1 + 2 * 3;", 16);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    CHECK(r.program != nullptr);
    CHECK(r.program->kind == NodeKind::Program);
    CHECK(r.program->kids.size() == 1);
    CHECK(r.program->kids[0]->kind == NodeKind::Print);
  }

  // Precedence: 1 + 2 * 3 parses as 1 + (2 * 3).
  {
    auto l = lex("print 1 + 2 * 3;", 16);
    auto r = parse(l.tokens);
    Node* add = r.program->kids[0]->kids[0];   // Print -> Binary(+)
    CHECK(add->kind == NodeKind::Binary);
    CHECK(add->op == '+');
    CHECK(add->kids[1]->kind == NodeKind::Binary);  // rhs is (2 * 3)
    CHECK(add->kids[1]->op == '*');
  }

  // A function declaration with params.
  {
    auto l = lex("fn add(a, b) { return a + b; }", 30);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* fn = r.program->kids[0];
    CHECK(fn->kind == NodeKind::FnDecl);
    CHECK(fn->str == "add");
    CHECK(fn->params.size() == 2);
  }

  // Syntax error: missing semicolon -> error, not crash.
  {
    auto l = lex("print 1", 7);
    auto r = parse(l.tokens);
    CHECK(!r.error.empty());
  }

  // Deeply nested parens must error (depth bound), never blow the native stack.
  {
    std::string deep(5000, '(');
    deep += "1";
    deep += std::string(5000, ')');
    deep += ";";
    auto l = lex(deep.c_str(), deep.size());
    auto r = parse(l.tokens);
    CHECK(!r.error.empty());
  }

  // --- A1: operator precedence and node kinds ---

  // && binds tighter than ||: a && b || c parses as (a && b) || c.
  {
    auto l = lex("print a && b || c;", 18);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* top = r.program->kids[0]->kids[0];     // Print -> Logical(||)
    CHECK(top->kind == NodeKind::Logical);
    CHECK(top->op == 'o');
    CHECK(top->kids[0]->kind == NodeKind::Logical);  // lhs is (a && b)
    CHECK(top->kids[0]->op == 'a');
  }

  // Comparison binds tighter than equality: 1 < 2 == true -> (1 < 2) == true.
  {
    auto l = lex("print 1 < 2 == true;", 20);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* eq = r.program->kids[0]->kids[0];      // Binary(==)
    CHECK(eq->kind == NodeKind::Binary);
    CHECK(eq->op == 'E');
    CHECK(eq->kids[0]->kind == NodeKind::Binary); // lhs is (1 < 2)
    CHECK(eq->kids[0]->op == '<');
  }

  // Unary binds tighter than binary: -a * b parses as (-a) * b, and unary chains.
  {
    auto l = lex("print -a * b;", 13);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* mul = r.program->kids[0]->kids[0];     // Binary(*)
    CHECK(mul->kind == NodeKind::Binary);
    CHECK(mul->op == '*');
    CHECK(mul->kids[0]->kind == NodeKind::Unary); // lhs is (-a)
    CHECK(mul->kids[0]->op == '-');
  }
  {
    auto l = lex("print !!x;", 10);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* u = r.program->kids[0]->kids[0];
    CHECK(u->kind == NodeKind::Unary && u->op == '!');
    CHECK(u->kids[0]->kind == NodeKind::Unary && u->kids[0]->op == '!');
  }

  // --- A3: control flow shapes ---

  // for(...) -> a For node with exactly 4 kids [init, cond, step, body].
  {
    auto l = lex("for (i = 0; i < 5; i = i + 1) { print i; }", 42);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* f = r.program->kids[0];
    CHECK(f->kind == NodeKind::For);
    CHECK(f->kids.size() == 4);
    CHECK(f->kids[0]->kind == NodeKind::Assign);   // init
    CHECK(f->kids[3]->kind == NodeKind::Block);     // body
  }

  // Omitted clauses become empty-Block sentinels (still 4 kids).
  {
    auto l = lex("for (;;) { break; }", 19);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* f = r.program->kids[0];
    CHECK(f->kind == NodeKind::For && f->kids.size() == 4);
    CHECK(f->kids[1]->kind == NodeKind::Block && f->kids[1]->kids.empty());  // cond omitted
  }

  // elif -> the If's else slot holds a nested If.
  {
    auto l = lex("if (a) { print 1; } elif (b) { print 2; } else { print 3; }", 59);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* iff = r.program->kids[0];
    CHECK(iff->kind == NodeKind::If && iff->kids.size() == 3);
    Node* elseSlot = iff->kids[2];
    CHECK(elseSlot->kind == NodeKind::If);          // the elif
    CHECK(elseSlot->kids.size() == 3);              // elif has its own else
    CHECK(elseSlot->kids[2]->kind == NodeKind::Block);
  }

  // break / continue parse to their own statement nodes.
  {
    auto l = lex("while (1) { break; continue; }", 30);
    auto r = parse(l.tokens);
    CHECK(r.error.empty());
    Node* body = r.program->kids[0]->kids[1];
    CHECK(body->kids[0]->kind == NodeKind::Break);
    CHECK(body->kids[1]->kind == NodeKind::Continue);
  }
}
