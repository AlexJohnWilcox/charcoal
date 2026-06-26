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
}
