#pragma once
#include "object.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace coal {

class HandleScope;

// All object creation goes through Heap. M2 is a Cheney semispace collector: a
// collection can happen only inside a new_* call, and it MOVES every live
// object. Consumers must never hold a raw Object* (or interior pointer) across a
// call that can allocate — root it through a HandleScope and re-read it after.
class Heap {
 public:
  explicit Heap(size_t max_bytes);
  ~Heap();

  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  // Composite objects (allocate more than once internally; GC-safe).
  StringObj*   new_string(const char* p, uint32_t n);
  ArrayObj*    new_array(uint32_t len);
  MapObj*      new_map();
  FunctionObj* new_function(uint32_t func_index);

  // Leaf storage blocks (single allocation each).
  SlotsObj*    new_slots(uint32_t count);            // all slots initialized to nil
  BytesObj*    new_bytes(const char* p, uint32_t n); // copies n bytes

  // A closure over `n_upvals` captured values (initialized to nil; the caller
  // fills them in by value). Allocates the upvalue Slots first, then the closure.
  ClosureObj*  new_closure(uint32_t func_index, uint32_t n_upvals);

  // A cursor over `arr` for external iteration. Single allocation; roots `arr`
  // across it and caches arr->slots at open time.
  IterObj* new_iter(ArrayObj* arr);

  size_t bytes_used() const;
  bool   over_cap() const;   // callers must check and raise (not crash) when true

  // Generation membership is by address range. `age`/promotion move survivors
  // from the young semispace into the old arena; minor collections never scan
  // the old arena except through the remembered set.
  static constexpr uint8_t PROMOTE_THRESHOLD = 2;
  bool   is_young(const void* p) const;
  bool   is_old(const void* p) const;
  size_t remset_size() const { return remembered_.size(); }  // test accessor

  // Record that old object `holder` now stores a pointer to `stored` if that is
  // a young object. Called by every store of a Value into a live container.
  void write_barrier(Object* holder, Value stored);

  // The interpreter installs an enumerator that visits every root (the register
  // files). Called at the start of each collection.
  void set_root_enumerator(std::function<void(GcVisitor&)> roots) {
    roots_ = std::move(roots);
  }

  // Evacuate `o` to to-space (or return its existing forwarding pointer). Public
  // so GcVisitor can drive it; not for general consumer use.
  Object* copy(Object* o);

 private:
  friend class HandleScope;

  void* bump(size_t n);
  void  collect();
  void  trace(Object* o);

  uint8_t* space_a_ = nullptr;
  uint8_t* space_b_ = nullptr;
  uint8_t* from_ = nullptr;   // active semispace
  uint8_t* to_ = nullptr;     // spare semispace (target during a collection)
  size_t   semi_ = 0;         // size of ONE semispace, in bytes
  size_t   top_ = 0;          // bump offset within from_
  size_t   to_top_ = 0;       // bump offset within to_ during a collection
  bool     over_cap_ = false;

  uint8_t* old_ = nullptr;     // non-moving old generation arena
  size_t   old_size_ = 0;
  size_t   old_top_ = 0;
  std::vector<Object*> remembered_;  // old objects holding a young pointer

  std::function<void(GcVisitor&)> roots_;
  std::vector<Object*> handles_;  // HandleScope root slots
};

// Pins objects as GC roots for the scope's lifetime. Because the collector
// moves objects, holding a raw Object* across an allocation is a use-after-move;
// keep() roots the object and get() re-reads its (possibly relocated) address.
class HandleScope {
 public:
  explicit HandleScope(Heap& h) : heap_(h), mark_(h.handles_.size()) {}
  ~HandleScope() { heap_.handles_.resize(mark_); }

  HandleScope(const HandleScope&) = delete;
  HandleScope& operator=(const HandleScope&) = delete;

  // Root `o`; returns its current address for convenience. To use the object
  // across a later allocation, capture the index from root() and re-read it.
  template <class T>
  T* keep(T* o) { heap_.handles_.push_back(o); return o; }

  // Root `o` and return the slot index within this scope (0 = first rooted).
  size_t root(Object* o) {
    size_t idx = heap_.handles_.size() - mark_;
    heap_.handles_.push_back(o);
    return idx;
  }

  // Re-read a rooted object after a safepoint. Reads the live slot each time, so
  // it survives vector reallocation.
  template <class T>
  T* get(size_t i) const { return static_cast<T*>(heap_.handles_[mark_ + i]); }

 private:
  Heap&  heap_;
  size_t mark_;
};

}  // namespace coal
