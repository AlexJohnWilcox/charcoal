#pragma once
#include <cstdint>

namespace coal {

struct Object;  // defined in object.h

enum class Tag : uint8_t { Nil, Int, Double, Bool, Obj };

struct Value {
  Tag tag;
  union {
    int64_t i;
    double  d;
    bool    b;
    Object* obj;
  } as;

  static Value nil();
  static Value integer(int64_t v);
  static Value number(double v);
  static Value boolean(bool v);
  static Value object(Object* o);

  bool truthy() const;      // nil / false / 0 / 0.0 -> false; everything else true
  bool is_number() const;   // Int or Double
};

}  // namespace coal
