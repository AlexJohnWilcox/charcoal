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

bool Heap::is_young(const void* p) const {
  auto b = reinterpret_cast<const uint8_t*>(p);
  return (b >= from_ && b < from_ + semi_);   // live young objects are in from_
}
bool Heap::is_old(const void* p) const {
  auto b = reinterpret_cast<const uint8_t*>(p);
  return (b >= old_ && b < old_ + old_top_);
}
void Heap::write_barrier(Object* holder, Value stored) {
  if (stored.tag != Tag::Obj || !stored.as.obj) return;
  if (is_old(holder) && is_young(stored.as.obj)) {
    remembered_.push_back(holder);   // dedup is unnecessary; scan tolerates repeats
  }
}

void GcVisitor::visit_obj(Object*& p) {
  if (!p) return;
  switch (heap->phase_) {
    case GcPhase::MinorForward: p = heap->copy(p);         break;
    case GcPhase::MajorMark:    heap->mark_old(p);         break;
    case GcPhase::MajorUpdate:  p = heap->forward_old(p);  break;
  }
}

void GcVisitor::visit(Value& v) {
  if (v.tag == Tag::Obj && v.as.obj) visit_obj(v.as.obj);
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

  old_size_ = semi_;                                       // one arena, same size
  old_ = static_cast<uint8_t*>(std::malloc(old_size_));
  old_top_ = 0;
  ASAN_POISON(old_, old_size_);
}

Heap::~Heap() {
  // Whole semispaces are freed at once — no per-object teardown.
  ASAN_UNPOISON(space_a_, semi_);
  ASAN_UNPOISON(space_b_, semi_);
  std::free(space_a_);
  std::free(space_b_);
  ASAN_UNPOISON(old_, old_size_);
  std::free(old_);
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

// Age-based forwarder for strong roots and the young frontier. Old objects are
// never moved. A young survivor that has now lived through PROMOTE_THRESHOLD
// collections is promoted into the old arena (age reset); otherwise it is copied
// to young to-space with its age bumped. The memcpy precedes stamping o->fwd so
// the new copy's own fwd is nullptr.
Object* Heap::copy(Object* o) {
  if (!o) return nullptr;
  if (is_old(o)) return o;    // old generation is non-moving
  if (o->fwd) return o->fwd;  // already evacuated this cycle
  size_t sz = size_of(o), a = align8(sz);
  Object* n;
  if (o->age + 1 >= PROMOTE_THRESHOLD && old_top_ + a <= old_size_) {
    n = reinterpret_cast<Object*>(old_ + old_top_);   // promote
    old_top_ += a;
    ASAN_UNPOISON(n, a);
    std::memcpy(n, o, sz);
    n->age = 0;
  } else if (to_top_ + a <= semi_) {
    n = reinterpret_cast<Object*>(to_ + to_top_);      // keep young, one cycle older
    to_top_ += a;
    ASAN_UNPOISON(n, a);
    std::memcpy(n, o, sz);
    n->age = static_cast<uint8_t>(o->age < 255 ? o->age + 1 : 255);
  } else {
    over_cap_ = true;
    return o;
  }
  o->fwd = n;
  return n;
}

// Force a young object into the old arena. Returns o unchanged if already old,
// or its existing forward if one was set this cycle (which may be a YOUNG copy,
// when a strong root evacuated it first -- the caller detects that and
// re-remembers). If the old arena is full, spill back to a young copy.
Object* Heap::copy_promote(Object* o) {
  if (!o) return nullptr;
  if (is_old(o)) return o;
  if (o->fwd) return o->fwd;  // already forwarded (possibly to a young copy)
  size_t sz = size_of(o), a = align8(sz);
  if (old_top_ + a > old_size_) { over_cap_ = true; return copy(o); }
  Object* n = reinterpret_cast<Object*>(old_ + old_top_);
  old_top_ += a;
  ASAN_UNPOISON(n, a);
  std::memcpy(n, o, sz);
  n->age = 0;
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

// Old-frontier tracer. Parallel to trace(), but children are force-promoted into
// the old arena via copy_promote(). If any child ends up still young this cycle
// (because a strong root already evacuated it to to-space), the old->young edge
// persists and `o` must be re-remembered so next cycle forwards it again --
// otherwise a later collection moves that child and leaves `o` dangling.
void Heap::trace_from_old(Object* o) {
  bool young_child = false;
  auto fwd_val = [&](Value& v) {
    if (v.tag == Tag::Obj && v.as.obj) {
      v.as.obj = copy_promote(v.as.obj);
      if (in_to_space(v.as.obj)) young_child = true;
    }
  };
  auto fwd_obj = [&](Object*& p) {
    if (p) { p = copy_promote(p); if (in_to_space(p)) young_child = true; }
  };
  switch (o->kind) {
    case ObjKind::String: {
      auto* s = static_cast<StringObj*>(o);
      Object* b = s->bytes; fwd_obj(b); s->bytes = static_cast<BytesObj*>(b);
      break;
    }
    case ObjKind::Array: {
      auto* a = static_cast<ArrayObj*>(o);
      Object* s = a->slots; fwd_obj(s); a->slots = static_cast<SlotsObj*>(s);
      break;
    }
    case ObjKind::Map: {
      auto* m = static_cast<MapObj*>(o);
      Object* k = m->keys; fwd_obj(k); m->keys = static_cast<SlotsObj*>(k);
      Object* v = m->vals; fwd_obj(v); m->vals = static_cast<SlotsObj*>(v);
      break;
    }
    case ObjKind::Slots: {
      auto* s = static_cast<SlotsObj*>(o);
      for (uint32_t i = 0; i < s->count; ++i) fwd_val(s->data[i]);
      break;
    }
    case ObjKind::Closure: {
      auto* c = static_cast<ClosureObj*>(o);
      Object* u = c->upvalues; fwd_obj(u); c->upvalues = static_cast<SlotsObj*>(u);
      break;
    }
    case ObjKind::Iter: {
      auto* it = static_cast<IterObj*>(o);
      Object* ar = it->arr; fwd_obj(ar); it->arr = static_cast<ArrayObj*>(ar);
      break;
    }
    case ObjKind::Bytes:
    case ObjKind::Function:
      break;  // no outgoing references
  }
  if (young_child) remembered_next_.push_back(o);
}

void Heap::mark_old(Object* o) {
  if (!o || !is_old(o) || o->mark) return;
  o->mark = 1;
  mark_work_.push_back(o);
}

Object* Heap::forward_old(Object* o) {
  if (o && is_old(o) && o->fwd) return o->fwd;
  return o;
}

// MINOR collection. Old objects are NEVER scanned wholesale -- the only way a
// young object reachable solely from an old object survives is the remembered
// set. PHASE ORDER MATTERS: force-promote remembered edges before roots.
void Heap::collect() {
  to_top_ = 0;
  remembered_next_.clear();
  size_t old_scan = old_top_;  // objects promoted THIS cycle start here

  // Phase 1: process the remembered set BEFORE forwarding roots. Nothing else is
  // forwarded yet, so each barriered young child promotes cleanly into old.
  std::vector<Object*> rem;
  rem.swap(remembered_);
  for (Object* r : rem) trace_from_old(r);

  // Phase 2: forward strong roots (age-based; a survivor may promote). Forwarding
  // pointers live in from_, which stays valid and unpoisoned until the swap below.
  for (Object*& h : handles_) h = copy(h);
  if (roots_) { GcVisitor v{this}; roots_(v); }

  // Phase 3: two-frontier Cheney scan to a fixpoint. The young frontier may
  // age-promote children into old; the old frontier force-promotes young children
  // (or, on old-arena overflow, spills back to young). Either can feed the other,
  // so loop until BOTH are drained.
  size_t yscan = 0;
  bool progress = true;
  while (progress) {
    progress = false;
    while (yscan < to_top_) {
      Object* o = reinterpret_cast<Object*>(to_ + yscan);
      trace(o);
      yscan += align8(size_of(o));
      progress = true;
    }
    while (old_scan < old_top_) {
      Object* o = reinterpret_cast<Object*>(old_ + old_scan);
      trace_from_old(o);
      old_scan += align8(size_of(o));
      progress = true;
    }
  }

  // Install next cycle's remembered set and flip the young semispaces.
  remembered_.swap(remembered_next_);
  std::swap(from_, to_);
  top_ = to_top_;

  // Invariant: in the young generation, exactly [from_, from_+top_) is unpoisoned.
  // The old arena keeps [old_, old_+old_top_) unpoisoned (never re-poisoned here).
  ASAN_POISON(to_, semi_);                  // the now-spare young space is fully dead
  ASAN_POISON(from_ + top_, semi_ - top_);  // and the active space's tail
}

// Mark every old object directly referenced by o (o may be young or old).
void Heap::mark_children_old(Object* o) {
  switch (o->kind) {
    case ObjKind::String: mark_old(static_cast<StringObj*>(o)->bytes); break;
    case ObjKind::Array:  mark_old(static_cast<ArrayObj*>(o)->slots);  break;
    case ObjKind::Map: {
      auto* m = static_cast<MapObj*>(o);
      mark_old(m->keys); mark_old(m->vals); break;
    }
    case ObjKind::Slots: {
      auto* s = static_cast<SlotsObj*>(o);
      for (uint32_t i = 0; i < s->count; ++i) {
        Value& v = s->data[i];
        if (v.tag == Tag::Obj && v.as.obj) mark_old(v.as.obj);
      }
      break;
    }
    case ObjKind::Closure: mark_old(static_cast<ClosureObj*>(o)->upvalues); break;
    case ObjKind::Iter:    mark_old(static_cast<IterObj*>(o)->arr);         break;
    case ObjKind::Bytes:
    case ObjKind::Function: break;
  }
}

// Rewrite every old-pointing child of o to its forwarded (compacted) address.
void Heap::update_children_old(Object* o) {
  switch (o->kind) {
    case ObjKind::String: {
      auto* s = static_cast<StringObj*>(o);
      s->bytes = static_cast<BytesObj*>(forward_old(s->bytes)); break;
    }
    case ObjKind::Array: {
      auto* a = static_cast<ArrayObj*>(o);
      a->slots = static_cast<SlotsObj*>(forward_old(a->slots)); break;
    }
    case ObjKind::Map: {
      auto* m = static_cast<MapObj*>(o);
      m->keys = static_cast<SlotsObj*>(forward_old(m->keys));
      m->vals = static_cast<SlotsObj*>(forward_old(m->vals)); break;
    }
    case ObjKind::Slots: {
      auto* s = static_cast<SlotsObj*>(o);
      for (uint32_t i = 0; i < s->count; ++i) {
        Value& v = s->data[i];
        if (v.tag == Tag::Obj && v.as.obj) v.as.obj = forward_old(v.as.obj);
      }
      break;
    }
    case ObjKind::Closure: {
      auto* c = static_cast<ClosureObj*>(o);
      c->upvalues = static_cast<SlotsObj*>(forward_old(c->upvalues)); break;
    }
    case ObjKind::Iter: {
      auto* it = static_cast<IterObj*>(o);
      it->arr = static_cast<ArrayObj*>(forward_old(it->arr)); break;
    }
    case ObjKind::Bytes:
    case ObjKind::Function: break;
  }
}

// MAJOR collection: Lisp2 sliding compaction of the old arena. Must be called
// right after a minor (young lives in from_, remembered_ holds old->young edges).
// Reclaims dead old objects; slides live ones down; poisons the freed tail.
void Heap::compact_old() {
  // 1. Clear scratch on every old object.
  for (size_t s = 0; s < old_top_; ) {
    Object* o = reinterpret_cast<Object*>(old_ + s);
    o->mark = 0; o->fwd = nullptr;
    s += align8(size_of(o));
  }

  // 2. Mark live old objects: from strong roots, from young survivors (live),
  //    and transitively through old->old edges.
  mark_work_.clear();
  phase_ = GcPhase::MajorMark;
  for (Object* h : handles_) mark_old(h);
  if (roots_) { GcVisitor v{this}; roots_(v); }
  for (size_t s = 0; s < top_; ) {                 // young survivors are roots
    Object* y = reinterpret_cast<Object*>(from_ + s);
    mark_children_old(y);
    s += align8(size_of(y));
  }
  while (!mark_work_.empty()) {                     // old->old closure
    Object* o = mark_work_.back(); mark_work_.pop_back();
    mark_children_old(o);
  }

  // 3. Compute forwarding addresses (pack live objects downward).
  size_t new_top = 0;
  for (size_t s = 0; s < old_top_; ) {
    Object* o = reinterpret_cast<Object*>(old_ + s);
    size_t a = align8(size_of(o));
    if (o->mark) { o->fwd = reinterpret_cast<Object*>(old_ + new_top); new_top += a; }
    s += a;
  }

  // 4. Update every pointer-into-old to its forwarded address.
  phase_ = GcPhase::MajorUpdate;
  for (Object*& h : handles_) h = forward_old(h);
  if (roots_) { GcVisitor v{this}; roots_(v); }
  for (size_t s = 0; s < top_; ) {                 // young survivors' old children
    Object* y = reinterpret_cast<Object*>(from_ + s);
    update_children_old(y);
    s += align8(size_of(y));
  }
  for (size_t s = 0; s < old_top_; ) {             // old live objects' old children
    Object* o = reinterpret_cast<Object*>(old_ + s);
    if (o->mark) update_children_old(o);
    s += align8(size_of(o));
  }
  for (Object*& h : remembered_) h = forward_old(h);   // remembered-set fixup
  phase_ = GcPhase::MinorForward;

  // 5. Slide live objects down (memmove: source/dest may overlap within an obj).
  for (size_t s = 0; s < old_top_; ) {
    Object* o = reinterpret_cast<Object*>(old_ + s);
    size_t sz = size_of(o), a = align8(sz);
    if (o->mark) {
      Object* dst = o->fwd;
      if (dst != o) std::memmove(dst, o, sz);
    }
    s += a;
  }

  // 6. Clear scratch on the compacted live region, poison the freed tail.
  for (size_t s = 0; s < new_top; ) {
    Object* o = reinterpret_cast<Object*>(old_ + s);
    o->mark = 0; o->fwd = nullptr;
    s += align8(size_of(o));
  }
  ASAN_POISON(old_ + new_top, old_size_ - new_top);
  old_top_ = new_top;
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
  s->age = 0;
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
  a->age = 0;
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
  c->age = 0;
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
  it->age     = 0;
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
  m->age = 0;
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
  f->age = 0;
  f->func_index = func_index;
  return f;
}

SlotsObj* Heap::new_slots(uint32_t count) {
  void* mem = bump(sizeof(SlotsObj) + static_cast<size_t>(count) * sizeof(Value));
  if (!mem) return nullptr;
  SlotsObj* s = static_cast<SlotsObj*>(mem);
  s->kind = ObjKind::Slots;
  s->fwd = nullptr;
  s->age = 0;
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
  b->age = 0;
  b->len = n;
  if (n) std::memcpy(b->data, p, n);
  return b;
}

size_t Heap::bytes_used() const { return top_; }

bool Heap::over_cap() const { return over_cap_; }

}  // namespace coal
