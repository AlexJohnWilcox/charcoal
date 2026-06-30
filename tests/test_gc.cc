#include "compiler.h"
#include "heap.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "test_main.h"

#include <cstring>
#include <string>

using namespace coal;

void test_gc() {
  // (1) An object rooted by a HandleScope survives forced collections intact.
  {
    Heap h(64 * 1024);
    HandleScope hs(h);
    size_t si = hs.root(h.new_string("survivor", 8));
    for (int i = 0; i < 20000; ++i) (void)h.new_array(1);  // force collection(s)
    StringObj* s = hs.get<StringObj>(si);                  // re-read moved object
    CHECK(s->len == 8);
    CHECK(std::memcmp(s->bytes->data, "survivor", 8) == 0);
  }

  // (2) Unrooted garbage is reclaimed: a tiny heap never overflows under a huge
  // allocation count, because each collection frees everything.
  {
    Heap h(64 * 1024);
    for (int i = 0; i < 50000; ++i) (void)h.new_array(2);
    CHECK(h.over_cap() == false);
    CHECK(h.bytes_used() < 64 * 1024);
  }

  // (3) Transitive structure survives: an array of strings, rooted, keeps its
  // elements alive and intact across collections.
  {
    Heap h(128 * 1024);
    HandleScope hs(h);
    size_t ai = hs.root(h.new_array(3));
    for (int i = 0; i < 3; ++i) {
      StringObj* e = h.new_string("x", 1);     // safepoint
      ArrayObj* a = hs.get<ArrayObj>(ai);      // re-read after the alloc
      a->slots->data[i] = Value::object(e);    // store immediately (no alloc between)
    }
    for (int i = 0; i < 20000; ++i) (void)h.new_array(1);  // force collection(s)
    ArrayObj* a = hs.get<ArrayObj>(ai);
    CHECK(a->len == 3);
    for (int i = 0; i < 3; ++i) {
      CHECK(a->slots->data[i].tag == Tag::Obj);
      StringObj* e = static_cast<StringObj*>(a->slots->data[i].as.obj);
      CHECK(e->len == 1 && e->bytes->data[0] == 'x');
    }
  }

  // (4) Objects actually move (proves the collector is copying, not no-op).
  {
    Heap h(64 * 1024);
    HandleScope hs(h);
    size_t ai = hs.root(h.new_array(1));
    void* before = static_cast<void*>(hs.get<ArrayObj>(ai));
    for (int i = 0; i < 20000; ++i) (void)h.new_array(1);
    void* after = static_cast<void*>(hs.get<ArrayObj>(ai));
    CHECK(before != after);
  }

  // (5) End-to-end: register values are roots, so a live array survives many
  // allocations forced mid-execution on a small heap.
  {
    const char* src =
        "xs = [10, 20, 30];"
        "i = 200;"
        "while (i) { junk = [0, 0, 0, 0, 0]; i = i - 1; }"
        "print xs[1];";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(32 * 1024);                 // small -> the loop forces collections
    Limits lim;
    auto r = run(c.module, h, lim);
    CHECK(r.ok);
    CHECK(r.output == "20");           // xs survived; xs[1] still 20
  }

  // (6) push under heap pressure: a safepoint bug in ARRAY_PUSH would corrupt or
  // crash here, since collections fire mid-push while the array must survive.
  {
    const char* src =
        "a = [];"
        "i = 300;"
        "while (i) { push(a, i); junk = [0, 0, 0, 0]; i = i - 1; }"
        "print a[0];";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "300");
  }

  // (7) format_join under heap pressure: a relocation mid-format must not leave
  // the input array's backing store cached. The buggy version reads poisoned
  // from-space here; the fix re-reads the array each iteration.
  {
    const char* src =
        "filler = range(0, 1500);"
        "a = range(0, 400);"
        "print format_join(a, \",\");";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(48 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output.substr(0, 6) == "0,1,2,");   // joined ints, fixed version
  }

  // (8) Lazy iterator happy path: drain a small array with no heap pressure.
  {
    const char* src =
        "a = [5, 7, 9];"
        "it = iter(a);"
        "s = 0;"
        "while (has_next(it)) { s = s + next(it); }"
        "print s;";
    auto l = lex(src, std::strlen(src));
    auto p = parse(l.tokens);
    auto c = compile(p.program);
    CHECK(c.ok);
    Heap h(64 * 1024);
    auto r = run(c.module, h, Limits{});
    CHECK(r.ok);
    CHECK(r.output == "21");   // 5 + 7 + 9
  }
}
