#include "heap.h"

#include <cstdlib>
#include <cstring>

namespace coal {

Heap::Heap(size_t max_bytes) : max_bytes_(max_bytes) {}

Heap::~Heap() {
  // M1: free everything we own.
  for (Object* o : objects_) {
    switch (o->kind) {
      case ObjKind::String:
        std::free(static_cast<StringObj*>(o)->data);
        break;
      case ObjKind::Array:
        std::free(static_cast<ArrayObj*>(o)->items);
        break;
      case ObjKind::Map: {
        MapObj* m = static_cast<MapObj*>(o);
        for (uint32_t i = 0; i < m->len; ++i) std::free(m->keys[i]);
        std::free(m->keys);
        std::free(m->keylens);
        std::free(m->vals);
        break;
      }
      case ObjKind::Function:
        break;
    }
    std::free(o);
  }
}

StringObj* Heap::new_string(const char* p, uint32_t n) {
  StringObj* s = static_cast<StringObj*>(std::malloc(sizeof(StringObj)));
  s->kind = ObjKind::String;
  s->len = n;
  s->data = static_cast<char*>(std::malloc(n ? n : 1));
  std::memcpy(s->data, p, n);  // n bytes, not NUL-terminated

  objects_.push_back(s);
  used_ += sizeof(StringObj) + n;
  return s;
}

ArrayObj* Heap::new_array(uint32_t len) {
  ArrayObj* a = static_cast<ArrayObj*>(std::malloc(sizeof(ArrayObj)));
  a->kind = ObjKind::Array;
  a->len = len;
  a->cap = len;
  a->items = static_cast<Value*>(std::malloc(sizeof(Value) * (len ? len : 1)));
  for (uint32_t i = 0; i < len; ++i) a->items[i] = Value::nil();

  objects_.push_back(a);
  used_ += sizeof(ArrayObj) + sizeof(Value) * len;
  return a;
}

MapObj* Heap::new_map() {
  MapObj* m = static_cast<MapObj*>(std::malloc(sizeof(MapObj)));
  m->kind = ObjKind::Map;
  m->len = 0;
  m->cap = 0;
  m->keys = nullptr;
  m->keylens = nullptr;
  m->vals = nullptr;

  objects_.push_back(m);
  used_ += sizeof(MapObj);
  return m;
}

FunctionObj* Heap::new_function(uint32_t func_index) {
  FunctionObj* f = static_cast<FunctionObj*>(std::malloc(sizeof(FunctionObj)));
  f->kind = ObjKind::Function;
  f->func_index = func_index;

  objects_.push_back(f);
  used_ += sizeof(FunctionObj);
  return f;
}

size_t Heap::bytes_used() const { return used_; }

bool Heap::over_cap() const { return used_ > max_bytes_; }

}  // namespace coal
