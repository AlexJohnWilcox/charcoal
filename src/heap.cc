#include "heap.h"

#include <cstdlib>
#include <cstring>
#include <utility>

// ASan manual poisoning: exactly [from_, from_+top_) is unpoisoned, so any
// access to evacuated/unused space trips the sanitizer.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
  #include <sanitizer/asan_interface.h>
  #define ASAN_POISON(p, n)   __asan_poison_memory_region((p), (n))
  #define ASAN_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
  #define ASAN_POISON(p, n)   ((void)0)
  #define ASAN_UNPOISON(p, n) ((void)0)
#endif

namespace coal {

namespace {

size_t align8(size_t n) { return (n + 7) & ~size_t(7); }

// Unaligned allocation footprint of an object, by kind. Used by both the
// allocator (to bump) and the collector (to copy and to walk to-space).
size_t size_of(const Object* o) {
  switch (o->kind) {
    case ObjKind::Bytes:
      return sizeof(BytesObj) + static_cast<const BytesObj*>(o)->len;
    case ObjKind::Slots:
      return sizeof(SlotsObj) +
             static_cast<size_t>(static_cast<const SlotsObj*>(o)->count) * sizeof(Value);
    case ObjKind::String:   return sizeof(StringObj);
    case ObjKind::Array:    return sizeof(ArrayObj);
    case ObjKind::Map:      return sizeof(MapObj);
    case ObjKind::Function: return sizeof(FunctionObj);
    case ObjKind::Closure:  return sizeof(ClosureObj);
    case ObjKind::Iter:     return sizeof(IterObj);
  }
  return sizeof(Object);
}

}  // namespace

void GcVisitor::visit(Value& v) {
  if (v.tag == Tag::Obj && v.as.obj) v.as.obj = heap->copy(v.as.obj);
}

Heap::Heap(size_t max_bytes) {
  semi_ = max_bytes ? max_bytes : 1;
  space_a_ = static_cast<uint8_t*>(std::malloc(semi_));
  space_b_ = static_cast<uint8_t*>(std::malloc(semi_));
  from_ = space_a_;
  to_ = space_b_;
  top_ = 0;
  ASAN_POISON(space_a_, semi_);
  ASAN_POISON(space_b_, semi_);
}

Heap::~Heap() {
  // Whole semispaces are freed at once — no per-object teardown.
  ASAN_UNPOISON(space_a_, semi_);
  ASAN_UNPOISON(space_b_, semi_);
  std::free(space_a_);
  std::free(space_b_);
}

void* Heap::bump(size_t n) {
  n = align8(n);
  if (top_ + n > semi_) {
    collect();
    if (top_ + n > semi_) { over_cap_ = true; return nullptr; }
  }
  void* p = from_ + top_;
  top_ += n;
  ASAN_UNPOISON(p, n);
  return p;
}

Object* Heap::copy(Object* o) {
  if (!o) return nullptr;
  if (o->fwd) return o->fwd;  // already evacuated this cycle
  size_t sz = size_of(o);
  size_t a = align8(sz);
  Object* n = reinterpret_cast<Object*>(to_ + to_top_);
  to_top_ += a;
  ASAN_UNPOISON(n, a);
  std::memcpy(n, o, sz);  // copy BEFORE stamping fwd, so the new copy's fwd is nullptr
  o->fwd = n;
  return n;
}

void Heap::trace(Object* o) {
  switch (o->kind) {
    case ObjKind::String: {
      StringObj* s = static_cast<StringObj*>(o);
      s->bytes = static_cast<BytesObj*>(copy(s->bytes));
      break;
    }
    case ObjKind::Array: {
      ArrayObj* a = static_cast<ArrayObj*>(o);
      a->slots = static_cast<SlotsObj*>(copy(a->slots));
      break;
    }
    case ObjKind::Map: {
      MapObj* mp = static_cast<MapObj*>(o);
      mp->keys = static_cast<SlotsObj*>(copy(mp->keys));
      mp->vals = static_cast<SlotsObj*>(copy(mp->vals));
      break;
    }
    case ObjKind::Slots: {
      SlotsObj* s = static_cast<SlotsObj*>(o);
      for (uint32_t i = 0; i < s->count; ++i) {
        Value& v = s->data[i];
        if (v.tag == Tag::Obj && v.as.obj) v.as.obj = copy(v.as.obj);
      }
      break;
    }
    case ObjKind::Closure: {
      ClosureObj* c = static_cast<ClosureObj*>(o);
      c->upvalues = static_cast<SlotsObj*>(copy(c->upvalues));  // upvalue values traced via the Slots
      break;
    }
    case ObjKind::Iter: {
      IterObj* it = static_cast<IterObj*>(o);
      it->arr = static_cast<ArrayObj*>(copy(it->arr));  // forward the iterated array
      break;
    }
    case ObjKind::Bytes:
    case ObjKind::Function:
      break;  // no outgoing references
  }
}

void Heap::collect() {
  to_top_ = 0;

  // Forward all roots into to-space. Forwarding pointers live in from_ (the old
  // space), which stays valid and unpoisoned until after the scan below.
  for (Object*& h : handles_) h = copy(h);
  if (roots_) { GcVisitor v{this}; roots_(v); }

  // Cheney scan: trace each object already in to-space; tracing may append more.
  size_t scan = 0;
  while (scan < to_top_) {
    Object* o = reinterpret_cast<Object*>(to_ + scan);
    trace(o);
    scan += align8(size_of(o));
  }

  std::swap(from_, to_);
  top_ = to_top_;

  // Invariant: exactly [from_, from_+top_) is unpoisoned.
  ASAN_POISON(to_, semi_);                  // the now-spare space is fully dead
  ASAN_POISON(from_ + top_, semi_ - top_);  // and the active space's tail
}

StringObj* Heap::new_string(const char* p, uint32_t n) {
  BytesObj* b = new_bytes(p, n);  // alloc #1: bytes settled before the next alloc
  if (!b) return nullptr;

  HandleScope hs(*this);
  size_t bi = hs.root(b);              // protect b across alloc #2
  void* mem = bump(sizeof(StringObj)); // may collect & move b
  if (!mem) return nullptr;

  StringObj* s = static_cast<StringObj*>(mem);
  s->kind = ObjKind::String;
  s->fwd = nullptr;
  s->len = n;
  s->bytes = hs.get<BytesObj>(bi);     // re-read the (possibly moved) bytes
  return s;
}

ArrayObj* Heap::new_array(uint32_t len) {
  SlotsObj* s = new_slots(len);  // alloc #1: slots initialized to nil
  if (!s) return nullptr;

  HandleScope hs(*this);
  size_t si = hs.root(s);             // protect s across alloc #2
  void* mem = bump(sizeof(ArrayObj)); // may collect & move s
  if (!mem) return nullptr;

  ArrayObj* a = static_cast<ArrayObj*>(mem);
  a->kind = ObjKind::Array;
  a->fwd = nullptr;
  a->len = len;
  a->cap = len;
  a->slots = hs.get<SlotsObj>(si);    // re-read the (possibly moved) slots
  return a;
}

ClosureObj* Heap::new_closure(uint32_t func_index, uint32_t n_upvals) {
  SlotsObj* s = new_slots(n_upvals);  // alloc #1: upvalue slots, nil-initialized
  if (!s) return nullptr;

  HandleScope hs(*this);
  size_t si = hs.root(s);                // protect s across alloc #2
  void* mem = bump(sizeof(ClosureObj));  // may collect & move s
  if (!mem) return nullptr;

  ClosureObj* c = static_cast<ClosureObj*>(mem);
  c->kind = ObjKind::Closure;
  c->fwd = nullptr;
  c->func_index = func_index;
  c->upvalues = hs.get<SlotsObj>(si);    // re-read the (possibly moved) slots
  return c;
}

IterObj* Heap::new_iter(ArrayObj* arr) {
  HandleScope hs(*this);
  size_t ai = hs.root(arr);              // protect arr across the bump
  void* mem = bump(sizeof(IterObj));     // may collect & move arr
  if (!mem) return nullptr;

  IterObj* it = static_cast<IterObj*>(mem);
  it->kind    = ObjKind::Iter;
  it->fwd     = nullptr;
  it->arr     = hs.get<ArrayObj>(ai);    // re-read the (possibly moved) array
  it->idx     = 0;
  it->len     = it->arr->len;
  return it;
}

MapObj* Heap::new_map() {
  void* mem = bump(sizeof(MapObj));
  if (!mem) return nullptr;
  MapObj* m = static_cast<MapObj*>(mem);
  m->kind = ObjKind::Map;
  m->fwd = nullptr;
  m->len = 0;
  m->cap = 0;
  m->keys = nullptr;
  m->vals = nullptr;
  return m;
}

FunctionObj* Heap::new_function(uint32_t func_index) {
  void* mem = bump(sizeof(FunctionObj));
  if (!mem) return nullptr;
  FunctionObj* f = static_cast<FunctionObj*>(mem);
  f->kind = ObjKind::Function;
  f->fwd = nullptr;
  f->func_index = func_index;
  return f;
}

SlotsObj* Heap::new_slots(uint32_t count) {
  void* mem = bump(sizeof(SlotsObj) + static_cast<size_t>(count) * sizeof(Value));
  if (!mem) return nullptr;
  SlotsObj* s = static_cast<SlotsObj*>(mem);
  s->kind = ObjKind::Slots;
  s->fwd = nullptr;
  s->count = count;
  for (uint32_t i = 0; i < count; ++i) s->data[i] = Value::nil();
  return s;
}

BytesObj* Heap::new_bytes(const char* p, uint32_t n) {
  void* mem = bump(sizeof(BytesObj) + n);
  if (!mem) return nullptr;
  BytesObj* b = static_cast<BytesObj*>(mem);
  b->kind = ObjKind::Bytes;
  b->fwd = nullptr;
  b->len = n;
  if (n) std::memcpy(b->data, p, n);
  return b;
}

size_t Heap::bytes_used() const { return top_; }

bool Heap::over_cap() const { return over_cap_; }

}  // namespace coal
