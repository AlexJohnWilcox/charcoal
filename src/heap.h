#pragma once
#include "object.h"
#include <cstddef>
#include <vector>

namespace coal {

// All object creation goes through Heap. M1 is a trivial allocator: each object
// is malloc'd and tracked, everything freed in the dtor. M2 swaps this for a
// moving mark-compact GC WITHOUT changing this interface, so consumers must
// never `new`/`delete` an Object directly and must hold live objects through a
// HandleScope across any allocation.
class Heap {
 public:
  explicit Heap(size_t max_bytes);
  ~Heap();

  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  StringObj*   new_string(const char* p, uint32_t n);
  ArrayObj*    new_array(uint32_t len);
  MapObj*      new_map();
  FunctionObj* new_function(uint32_t func_index);

  size_t bytes_used() const;
  bool   over_cap() const;   // callers must check and raise (not crash) when true

 private:
  std::vector<Object*> objects_;  // M1 registry; M2 replaces with a managed heap
  size_t max_bytes_;
  size_t used_ = 0;
};

// Pins objects as GC roots for the scope's lifetime. M1: tracking is a no-op
// (the heap doesn't move), but the API must EXIST and be USED everywhere a raw
// Object* is held across an allocation, so M2's moving GC is correct with zero
// consumer changes.
class HandleScope {
 public:
  explicit HandleScope(Heap& h) : heap_(h) {}
  ~HandleScope() = default;

  HandleScope(const HandleScope&) = delete;
  HandleScope& operator=(const HandleScope&) = delete;

  template <class T>
  T* keep(T* o) { return o; }  // M1 pass-through; M2 records a root

 private:
  Heap& heap_;
};

}  // namespace coal
