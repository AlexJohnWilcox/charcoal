#include "value.h"

namespace coal {

Value Value::nil() {
  Value v;
  v.tag = Tag::Nil;
  v.as.i = 0;
  return v;
}

Value Value::integer(int64_t v) {
  Value out;
  out.tag = Tag::Int;
  out.as.i = v;
  return out;
}

Value Value::number(double v) {
  Value out;
  out.tag = Tag::Double;
  out.as.d = v;
  return out;
}

Value Value::boolean(bool v) {
  Value out;
  out.tag = Tag::Bool;
  out.as.b = v;
  return out;
}

Value Value::object(Object* o) {
  Value out;
  out.tag = Tag::Obj;
  out.as.obj = o;
  return out;
}

bool Value::truthy() const {
  switch (tag) {
    case Tag::Nil:    return false;
    case Tag::Int:    return as.i != 0;
    case Tag::Double: return as.d != 0.0;
    case Tag::Bool:   return as.b;
    case Tag::Obj:    return true;
  }
  return true;
}

bool Value::is_number() const {
  return tag == Tag::Int || tag == Tag::Double;
}

}  // namespace coal
