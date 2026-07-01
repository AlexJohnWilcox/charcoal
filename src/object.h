#pragma once
#include "value.h"
#include <cstdint>

namespace coal {

class Heap;  // for GcVisitor

enum class ObjKind : uint8_t { String, Array, Map, Function, Bytes, Slots, Closure, Iter };

// Common object header. `fwd` is GC scratch used ONLY during a collection: on
// the old copy of an object it points at the new copy (nullptr = not yet
// copied). Outside a collection it is always nullptr.
struct Object {
  ObjKind  kind;
  uint8_t  age;   // minor collections survived; used for promotion (GC-managed)
  Object*  fwd;
};

// Variable-size raw bytes (string contents). `data` is a flexible array; an
// allocation of a BytesObj carries `len` trailing bytes after the struct.
struct BytesObj : Object {
  uint32_t len;
  char     data[1];
};

// Variable-size block of Values, used for array storage and for a map's key and
// value arrays. `data` is a flexible array of `count` Values.
struct SlotsObj : Object {
  uint32_t count;
  Value    data[1];
};

struct StringObj : Object {
  uint32_t  len;     // logical length in bytes
  BytesObj* bytes;   // owns: `len` bytes, not NUL-terminated
};

struct ArrayObj : Object {
  uint32_t  len;
  uint32_t  cap;
  SlotsObj* slots;   // `cap` Values, first `len` live
};

// Map as two parallel Slots blocks: keys[i] is a Value holding a StringObj,
// vals[i] is the associated value.
struct MapObj : Object {
  uint32_t  len;
  uint32_t  cap;
  SlotsObj* keys;    // nullptr until first insert
  SlotsObj* vals;
};

struct FunctionObj : Object {
  uint32_t func_index;  // index into Module::funcs
};

// A first-class function value: a proto (func_index) plus its captured upvalues.
// Capture is by VALUE — `upvalues` holds copies taken at closure-creation time.
struct ClosureObj : Object {
  uint32_t  func_index;  // index into Module::funcs (the proto)
  SlotsObj* upvalues;    // captured values; a 0-length Slots if none
};

// A lazy cursor over an array, produced by the iter() builtin. `idx` and `len`
// track the position and the length snapshot taken when opened.
struct IterObj : Object {
  ArrayObj* arr;       // the array being iterated
  uint32_t  idx;       // next index to yield
  uint32_t  len;       // arr->len at open time
};

// Passed to a Heap's root enumerator. `visit` forwards a root Value in place: if
// it holds an object, the object is evacuated and the Value rewritten to point
// at the moved copy.
struct GcVisitor {
  Heap* heap;
  void  visit(Value& v);  // defined in heap.cc
};

}  // namespace coal
