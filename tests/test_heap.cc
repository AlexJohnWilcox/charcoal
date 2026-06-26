#include "heap.h"
#include "test_main.h"

using namespace coal;

void test_heap() {
  Heap h(1 << 20);
  HandleScope hs(h);

  StringObj* s = hs.keep(h.new_string("hi", 2));
  CHECK(s->len == 2);

  ArrayObj* a = h.new_array(3);
  CHECK(a->len == 3);
  CHECK(a->items[0].tag == Tag::Nil);

  CHECK(h.bytes_used() > 0);
  CHECK(h.over_cap() == false);

  // Dtor frees everything; running this binary under ASan proves no leak/UAF.
}
