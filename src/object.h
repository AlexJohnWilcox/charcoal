#pragma once
#include "value.h"
#include <cstdint>

namespace coal {

enum class ObjKind : uint8_t { String, Array, Map, Function };

// Common object header. M2's moving GC will add mark/forwarding fields here;
// keep this struct the single source of truth for "what every object carries".
struct Object {
  ObjKind kind;
};

struct StringObj : Object {
  uint32_t len;
  char*    data;   // owns: heap-allocated, length `len`, not NUL-terminated
};

struct ArrayObj : Object {
  uint32_t len;
  uint32_t cap;
  Value*   items;  // owns: `cap` Values, first `len` live
};

// M1 map: parallel key/value arrays. M3 replaces the key storage with a Shape*
// (hidden class) + a flat slot array; consumers must go through accessors so
// that swap is invisible.
struct MapObj : Object {
  uint32_t len;
  uint32_t cap;
  char**   keys;     // owns: `cap` heap strings
  uint32_t* keylens; // owns: length of each key
  Value*   vals;     // owns: `cap` Values
};

struct FunctionObj : Object {
  uint32_t func_index;  // index into Module::funcs
};

}  // namespace coal
