#include "heap.h"
#include "object.h"
#include "test_main.h"

using namespace coal;

// Object construction happens through the Heap, so we exercise the object
// layout via freshly-allocated objects here.
void test_object() {
  Heap h(1 << 20);

  StringObj* s = h.new_string("hello", 5);
  CHECK(s->kind == ObjKind::String);
  CHECK(s->len == 5);
  CHECK(s->bytes->data[0] == 'h' && s->bytes->data[4] == 'o');

  ArrayObj* a = h.new_array(2);
  CHECK(a->kind == ObjKind::Array);
  CHECK(a->len == 2);
  CHECK(a->slots->data[0].tag == Tag::Nil && a->slots->data[1].tag == Tag::Nil);

  MapObj* m = h.new_map();
  CHECK(m->kind == ObjKind::Map);
  CHECK(m->len == 0);

  FunctionObj* f = h.new_function(7);
  CHECK(f->kind == ObjKind::Function);
  CHECK(f->func_index == 7);
}
