#include "native.h"

#include "object.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
//  native.cc — Charcoal standard library (the C++ builtins)
// ============================================================================
//
//  Every builtin is a `NativeFn`:
//
//      bool fn(Value* args, uint32_t argc, Heap& h, Value& out, std::string& err)
//
//  It reads `argc` arguments from `args`, writes its result into `out`, and
//  returns true on success. On failure it returns false and sets `err`, which
//  the interpreter turns into a structured runtime error (never a crash). The
//  interpreter has already validated the arity (against the registry below) and
//  that `args[0..argc)` are real registers, so a fixed-arity builtin may read
//  exactly its declared number of arguments without further bounds checks.
//
//  ------------------------------------------------------------------------
//  The safepoint rule (the single most important invariant in this file)
//  ------------------------------------------------------------------------
//  Charcoal uses a moving (Cheney semispace) garbage collector. A collection
//  can fire ONLY inside a `Heap::new_*` call, and when it does it relocates
//  EVERY live object. Therefore any raw `Object*` or `Value` holding an object
//  pointer that was read BEFORE an allocation is dangling AFTER it.
//
//  Two disciplines keep the builtins correct:
//
//    * Single-allocation builtins read every scalar they need out of `args`
//      first, build the result LAST, and return. Because nothing is allocated
//      after the result, no pointer can go stale. (Most builtins are this kind.)
//
//    * Multi-allocation builtins (split, lines, zip, enumerate, flatten, chunk,
//      chars, entries, merge, invert, pick, ...) root the growing result in a
//      `HandleScope` and RE-READ the result and every source argument from a
//      root after EACH `new_*`. Source string/key bytes are copied into C++
//      `std::string` locals up front, since those live in non-moving system
//      memory and stay valid across collections.
//
//  Helpers below marked with the word "safepoint" allocate and therefore must
//  be treated as collection points by their callers.
//
//  ------------------------------------------------------------------------
//  Builtin index (registered in kNatives[] at the bottom of this file)
//  ------------------------------------------------------------------------
//  type/convert : type to_string to_int to_float is_nil is_num is_str
//                 is_array is_map is_int is_bool
//  math         : abs min max floor ceil round sqrt pow sign clamp gcd lcm
//                 exp log log2 log10 sin cos tan atan2 hypot trunc factorial
//                 is_prime fib cbrt deg2rad rad2deg log_base mean
//  string       : len substr char_at index_of contains starts_with ends_with
//                 upper lower repeat str_concat split join trim ord chr replace
//                 last_index_of pad_left pad_right count_sub capitalize
//                 reverse_str lines is_empty swapcase title_case zfill center
//                 chars to_bytes format is_digit is_alpha is_alnum is_space
//  array        : pop insert remove slice reverse sort arr_index_of range fill
//                 concat_arr sum product min_of max_of first last take drop
//                 count_of any all includes index_min index_max zip enumerate
//                 flatten unique rotate chunk
//  map          : keys values has remove_key get set merge entries invert pick
//                 get_or has_value omit
//  batch 4 math : asin acos atan sinh cosh tanh asinh acosh atanh expm1 log1p
//                 fmod copysign ldexp is_nan is_inf is_finite lerp smoothstep
//                 isqrt mod_floor bit_count next_pow2 divmod
//  batch 4 str  : is_upper is_lower strip_prefix strip_suffix left right
//                 count_char find_all replace_first rot13
//  batch 4 seq  : compact cumsum dot intersperse reverse_copy sorted is_sorted
//                 tail init   (conv: hex bin is_function)
//
// ============================================================================

namespace coal {

namespace {

// --- small value helpers ------------------------------------------------

bool is_num(const Value& v)   { return v.is_number(); }
bool is_str(const Value& v)   { return v.tag == Tag::Obj && v.as.obj && v.as.obj->kind == ObjKind::String; }
bool is_array(const Value& v) { return v.tag == Tag::Obj && v.as.obj && v.as.obj->kind == ObjKind::Array; }
bool is_map(const Value& v)   { return v.tag == Tag::Obj && v.as.obj && v.as.obj->kind == ObjKind::Map; }

double    to_double(const Value& v) { return v.tag == Tag::Double ? v.as.d : static_cast<double>(v.as.i); }
int64_t   as_i64(const Value& v)    { return v.tag == Tag::Int ? v.as.i : static_cast<int64_t>(v.as.d); }
StringObj* as_str(const Value& v)   { return static_cast<StringObj*>(v.as.obj); }
ArrayObj*  as_arr(const Value& v)   { return static_cast<ArrayObj*>(v.as.obj); }
MapObj*    as_map(const Value& v)   { return static_cast<MapObj*>(v.as.obj); }
const char* sbytes(const Value& v)  { return as_str(v)->bytes->data; }
uint32_t    slen(const Value& v)    { return as_str(v)->len; }

// Allocate a string Value (the LAST thing a builtin does — safepoint-safe).
bool make_string(Heap& h, const char* p, uint32_t len, Value& out, std::string& err) {
  StringObj* s = h.new_string(p, len);
  if (!s || h.over_cap()) { err = "out of memory"; return false; }
  out = Value::object(s);
  return true;
}

// Allocate an array Value (slots pre-initialized to nil). Safepoint: the caller
// must re-read any other live object pointer after calling this.
bool make_array(Heap& h, uint32_t len, Value& out, std::string& err) {
  ArrayObj* a = h.new_array(len);
  if (!a || h.over_cap()) { err = "out of memory"; return false; }
  out = Value::object(a);
  return true;
}

bool make_map(Heap& h, Value& out, std::string& err) {
  MapObj* m = h.new_map();
  if (!m || h.over_cap()) { err = "out of memory"; return false; }
  out = Value::object(m);
  return true;
}

std::string render(const Value& v) {
  switch (v.tag) {
    case Tag::Nil:    return "nil";
    case Tag::Int:    return std::to_string(v.as.i);
    case Tag::Double: return std::to_string(v.as.d);
    case Tag::Bool:   return v.as.b ? "true" : "false";
    case Tag::Obj:
      if (!v.as.obj) return "nil";
      switch (v.as.obj->kind) {
        case ObjKind::String: return std::string(sbytes(v), slen(v));
        case ObjKind::Array:  return "[array]";
        case ObjKind::Map:    return "[object]";
        default:              return "[fn]";
      }
  }
  return "nil";
}

// Total equality (numbers numeric, strings by content, else identity).
bool val_equal(const Value& x, const Value& y) {
  if (x.is_number() && y.is_number()) {
    if (x.tag == Tag::Int && y.tag == Tag::Int) return x.as.i == y.as.i;
    return to_double(x) == to_double(y);
  }
  if (x.tag != y.tag) return false;
  switch (x.tag) {
    case Tag::Nil:  return true;
    case Tag::Bool: return x.as.b == y.as.b;
    case Tag::Obj:
      if (x.as.obj == y.as.obj) return true;
      if (is_str(x) && is_str(y))
        return slen(x) == slen(y) && std::memcmp(sbytes(x), sbytes(y), slen(x)) == 0;
      return false;
    default: return false;
  }
}

constexpr int64_t kI64Max = 9223372036854775807LL;
constexpr double  kPi = 3.14159265358979323846;

// =======================================================================
// Type / convert
// =======================================================================

bool type_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  const char* t = "nil";
  switch (a[0].tag) {
    case Tag::Nil:    t = "nil"; break;
    case Tag::Int:    t = "int"; break;
    case Tag::Double: t = "double"; break;
    case Tag::Bool:   t = "bool"; break;
    case Tag::Obj:
      if (!a[0].as.obj) { t = "nil"; break; }
      switch (a[0].as.obj->kind) {
        case ObjKind::String:   t = "string"; break;
        case ObjKind::Array:    t = "array"; break;
        case ObjKind::Map:      t = "map"; break;
        case ObjKind::Function: t = "function"; break;
        default:                t = "nil"; break;
      }
      break;
  }
  return make_string(h, t, static_cast<uint32_t>(std::strlen(t)), out, err);
}

bool to_string_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  std::string s = render(a[0]);  // copies bytes before any allocation
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool to_int_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  switch (a[0].tag) {
    case Tag::Int:  out = a[0]; return true;
    case Tag::Bool: out = Value::integer(a[0].as.b ? 1 : 0); return true;
    case Tag::Double: {
      double d = a[0].as.d;
      if (!(d >= -9.2e18 && d <= 9.2e18)) { err = "value out of integer range"; return false; }
      out = Value::integer(static_cast<int64_t>(d));
      return true;
    }
    case Tag::Obj:
      if (is_str(a[0])) {
        std::string s(sbytes(a[0]), slen(a[0]));
        char* end = nullptr;
        errno = 0;
        long long v = std::strtoll(s.c_str(), &end, 10);
        if (s.empty() || end != s.c_str() + s.size() || errno != 0) {
          err = "cannot convert string to int"; return false;
        }
        out = Value::integer(static_cast<int64_t>(v));
        return true;
      }
      break;
    default: break;
  }
  err = "cannot convert to int";
  return false;
}

bool to_float_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  switch (a[0].tag) {
    case Tag::Int:    out = Value::number(static_cast<double>(a[0].as.i)); return true;
    case Tag::Double: out = a[0]; return true;
    case Tag::Bool:   out = Value::number(a[0].as.b ? 1.0 : 0.0); return true;
    case Tag::Obj:
      if (is_str(a[0])) {
        std::string s(sbytes(a[0]), slen(a[0]));
        char* end = nullptr;
        errno = 0;
        double d = std::strtod(s.c_str(), &end);
        if (s.empty() || end != s.c_str() + s.size() || errno != 0) {
          err = "cannot convert string to float"; return false;
        }
        out = Value::number(d);
        return true;
      }
      break;
    default: break;
  }
  err = "cannot convert to float";
  return false;
}

bool is_nil_fn(Value* a, uint32_t, Heap&, Value& out, std::string&)   { out = Value::boolean(a[0].tag == Tag::Nil); return true; }
bool is_num_fn(Value* a, uint32_t, Heap&, Value& out, std::string&)   { out = Value::boolean(is_num(a[0])); return true; }
bool is_str_fn(Value* a, uint32_t, Heap&, Value& out, std::string&)   { out = Value::boolean(is_str(a[0])); return true; }
bool is_array_fn(Value* a, uint32_t, Heap&, Value& out, std::string&) { out = Value::boolean(is_array(a[0])); return true; }
bool is_map_fn(Value* a, uint32_t, Heap&, Value& out, std::string&)   { out = Value::boolean(is_map(a[0])); return true; }

// =======================================================================
// Math
// =======================================================================

bool abs_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag == Tag::Int) {
    int64_t x = a[0].as.i;
    out = Value::integer(x < 0 ? static_cast<int64_t>(0u - static_cast<uint64_t>(x)) : x);
    return true;
  }
  if (a[0].tag == Tag::Double) { out = Value::number(std::fabs(a[0].as.d)); return true; }
  err = "abs expects a number";
  return false;
}

// min/max preserve integer type when both arguments are integers, and otherwise
// fall back to a double comparison (so `min(2, 1.5)` returns the double 1.5).
bool min_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  const Value& lhs = a[0];
  const Value& rhs = a[1];
  if (!is_num(lhs) || !is_num(rhs)) {
    err = "min expects numbers";
    return false;
  }
  const bool both_int = (lhs.tag == Tag::Int && rhs.tag == Tag::Int);
  if (both_int) {
    const int64_t smaller = (lhs.as.i < rhs.as.i) ? lhs.as.i : rhs.as.i;
    out = Value::integer(smaller);
  } else {
    const double a_d = to_double(lhs);
    const double b_d = to_double(rhs);
    const double smaller = (a_d < b_d) ? a_d : b_d;
    out = Value::number(smaller);
  }
  return true;
}

bool max_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  const Value& lhs = a[0];
  const Value& rhs = a[1];
  if (!is_num(lhs) || !is_num(rhs)) {
    err = "max expects numbers";
    return false;
  }
  const bool both_int = (lhs.tag == Tag::Int && rhs.tag == Tag::Int);
  if (both_int) {
    const int64_t larger = (lhs.as.i > rhs.as.i) ? lhs.as.i : rhs.as.i;
    out = Value::integer(larger);
  } else {
    const double a_d = to_double(lhs);
    const double b_d = to_double(rhs);
    const double larger = (a_d > b_d) ? a_d : b_d;
    out = Value::number(larger);
  }
  return true;
}

// floor/ceil/round share a path: int stays int; double rounds to an in-range int.
bool round_like(Value* a, double (*round_op)(double), const char* name,
                Value& out, std::string& err) {
  if (a[0].tag == Tag::Int) { out = a[0]; return true; }
  if (a[0].tag != Tag::Double) { err = std::string(name) + " expects a number"; return false; }
  double r = round_op(a[0].as.d);
  if (!(r >= -9.2e18 && r <= 9.2e18)) { err = "value out of integer range"; return false; }
  out = Value::integer(static_cast<int64_t>(r));
  return true;
}
bool floor_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return round_like(a, std::floor, "floor", out, err); }
bool ceil_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)  { return round_like(a, std::ceil,  "ceil",  out, err); }
bool round_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return round_like(a, std::round, "round", out, err); }

bool sqrt_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "sqrt expects a number"; return false; }
  double d = to_double(a[0]);
  if (d < 0) { err = "sqrt of negative number"; return false; }
  out = Value::number(std::sqrt(d));
  return true;
}

bool pow_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "pow expects numbers"; return false; }
  out = Value::number(std::pow(to_double(a[0]), to_double(a[1])));
  return true;
}

bool sign_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "sign expects a number"; return false; }
  double d = to_double(a[0]);
  out = Value::integer(d < 0 ? -1 : (d > 0 ? 1 : 0));
  return true;
}

// =======================================================================
// String (and the polymorphic len)
// =======================================================================

bool len_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (is_str(a[0]))   { out = Value::integer(slen(a[0])); return true; }
  if (is_array(a[0])) { out = Value::integer(as_arr(a[0])->len); return true; }
  if (is_map(a[0]))   { out = Value::integer(as_map(a[0])->len); return true; }
  err = "len expects a string, array, or map";
  return false;
}

bool substr_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "substr expects a string"; return false; }
  if (!is_num(a[1]) || !is_num(a[2])) { err = "substr indices must be numbers"; return false; }
  std::string src(sbytes(a[0]), slen(a[0]));  // copy first
  int64_t len = static_cast<int64_t>(src.size());
  int64_t start = as_i64(a[1]);
  int64_t count = as_i64(a[2]);
  if (start < 0) start = 0;
  if (start > len) start = len;
  if (count < 0) count = 0;
  if (count > len - start) count = len - start;
  return make_string(h, src.data() + start, static_cast<uint32_t>(count), out, err);
}

bool char_at_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "char_at expects a string"; return false; }
  if (!is_num(a[1])) { err = "char_at index must be a number"; return false; }
  int64_t i = as_i64(a[1]);
  if (i < 0 || i >= static_cast<int64_t>(slen(a[0]))) { err = "index out of range"; return false; }
  out = Value::integer(static_cast<uint8_t>(sbytes(a[0])[i]));
  return true;
}

bool index_of_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "index_of expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sub(sbytes(a[1]), slen(a[1]));
  size_t p = s.find(sub);
  out = Value::integer(p == std::string::npos ? -1 : static_cast<int64_t>(p));
  return true;
}

bool contains_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "contains expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sub(sbytes(a[1]), slen(a[1]));
  out = Value::boolean(s.find(sub) != std::string::npos);
  return true;
}

bool starts_with_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "starts_with expects strings"; return false; }
  uint32_t sl = slen(a[0]), pl = slen(a[1]);
  out = Value::boolean(pl <= sl && std::memcmp(sbytes(a[0]), sbytes(a[1]), pl) == 0);
  return true;
}

bool ends_with_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "ends_with expects strings"; return false; }
  uint32_t sl = slen(a[0]), pl = slen(a[1]);
  out = Value::boolean(pl <= sl && std::memcmp(sbytes(a[0]) + (sl - pl), sbytes(a[1]), pl) == 0);
  return true;
}

bool upper_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "upper expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool lower_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "lower expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool repeat_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "repeat expects a string"; return false; }
  if (!is_num(a[1])) { err = "repeat count must be a number"; return false; }
  std::string src(sbytes(a[0]), slen(a[0]));
  int64_t c = as_i64(a[1]);
  if (c < 0) { err = "repeat count must be non-negative"; return false; }
  const uint64_t kCap = 1u << 20;
  if (!src.empty() && static_cast<uint64_t>(c) > kCap / src.size()) {
    err = "repeat result too large"; return false;
  }
  std::string r;
  r.reserve(src.size() * static_cast<size_t>(c));
  for (int64_t i = 0; i < c; ++i) r += src;
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

bool str_concat_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "str_concat expects strings"; return false; }
  std::string r(sbytes(a[0]), slen(a[0]));
  r.append(sbytes(a[1]), slen(a[1]));
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

bool trim_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "trim expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  size_t b = 0, e = s.size();
  while (b < e && ws(s[b])) ++b;
  while (e > b && ws(s[e - 1])) --e;
  return make_string(h, s.data() + b, static_cast<uint32_t>(e - b), out, err);
}

bool ord_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "ord expects a string"; return false; }
  if (slen(a[0]) == 0) { err = "ord of empty string"; return false; }
  out = Value::integer(static_cast<uint8_t>(sbytes(a[0])[0]));
  return true;
}

bool chr_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "chr expects a number"; return false; }
  int64_t i = as_i64(a[0]);
  if (i < 0 || i > 255) { err = "chr value out of byte range"; return false; }
  char c = static_cast<char>(i);
  return make_string(h, &c, 1, out, err);
}

// split: multi-allocation. Source bytes are copied into C++ locals first, the
// result array is rooted, and it is re-read after every child-string allocation.
bool split_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "split expects (string, string)"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sep(sbytes(a[1]), slen(a[1]));
  if (sep.empty()) { err = "split separator must be non-empty"; return false; }

  std::vector<std::string> parts;
  size_t pos = 0;
  while (true) {
    size_t f = s.find(sep, pos);
    if (f == std::string::npos) { parts.push_back(s.substr(pos)); break; }
    parts.push_back(s.substr(pos, f - pos));
    pos = f + sep.size();
  }

  uint32_t count = static_cast<uint32_t>(parts.size());
  ArrayObj* arr = h.new_array(count);  // safepoint
  if (!arr || h.over_cap()) { err = "out of memory"; return false; }
  HandleScope hs(h);
  size_t ai = hs.root(arr);
  for (uint32_t i = 0; i < count; ++i) {
    StringObj* sObj = h.new_string(parts[i].data(), static_cast<uint32_t>(parts[i].size()));  // safepoint
    if (!sObj || h.over_cap()) { err = "out of memory"; return false; }
    hs.get<ArrayObj>(ai)->slots->data[i] = Value::object(sObj);  // re-read array
  }
  out = Value::object(hs.get<ArrayObj>(ai));
  return true;
}

// join: builds the whole string in a C++ local (no allocation while reading the
// array), then allocates exactly once.
bool join_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "join expects an array"; return false; }
  if (!is_str(a[1])) { err = "join separator must be a string"; return false; }
  ArrayObj* ar = as_arr(a[0]);
  std::string sep(sbytes(a[1]), slen(a[1]));
  std::string r;
  for (uint32_t i = 0; i < ar->len; ++i) {
    const Value& e = ar->slots->data[i];
    if (!is_str(e)) { err = "join elements must be strings"; return false; }
    if (i) r += sep;
    r.append(sbytes(e), slen(e));
  }
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

// =======================================================================
// Array
// =======================================================================

bool pop_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "pop expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  if (arr->len == 0) { err = "pop from empty array"; return false; }
  out = arr->slots->data[arr->len - 1];
  arr->len--;
  return true;
}

bool insert_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "insert expects an array"; return false; }
  if (!is_num(a[1])) { err = "insert index must be a number"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  int64_t idx = as_i64(a[1]);
  uint32_t len = arr->len;
  if (idx < 0 || idx > static_cast<int64_t>(len)) { err = "index out of range"; return false; }

  if (len == arr->cap) {
    uint32_t newcap = arr->cap ? arr->cap * 2 : 4;
    SlotsObj* ns = h.new_slots(newcap);   // safepoint: may move arr & its slots
    if (!ns || h.over_cap()) { err = "out of memory"; return false; }
    arr = as_arr(a[0]);                   // re-read after the safepoint
    for (uint32_t i = 0; i < arr->len; ++i) ns->data[i] = arr->slots->data[i];
    arr->slots = ns;
    arr->cap = newcap;
  }
  arr = as_arr(a[0]);          // re-read (harmless if no grow happened)
  Value x = a[2];              // read the value AFTER any allocation
  for (uint32_t j = len; j > idx; --j) arr->slots->data[j] = arr->slots->data[j - 1];
  arr->slots->data[idx] = x;
  arr->len = len + 1;
  out = Value::nil();
  return true;
}

bool remove_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "remove expects an array"; return false; }
  if (!is_num(a[1])) { err = "remove index must be a number"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  int64_t idx = as_i64(a[1]);
  if (idx < 0 || idx >= static_cast<int64_t>(arr->len)) { err = "index out of range"; return false; }
  out = arr->slots->data[idx];
  for (uint32_t j = static_cast<uint32_t>(idx); j + 1 < arr->len; ++j)
    arr->slots->data[j] = arr->slots->data[j + 1];
  arr->len--;
  return true;
}

bool slice_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "slice expects an array"; return false; }
  if (!is_num(a[1]) || !is_num(a[2])) { err = "slice indices must be numbers"; return false; }
  int64_t len = static_cast<int64_t>(as_arr(a[0])->len);
  int64_t start = as_i64(a[1]);
  int64_t count = as_i64(a[2]);
  if (start < 0) start = 0;
  if (start > len) start = len;
  if (count < 0) count = 0;
  if (count > len - start) count = len - start;

  ArrayObj* res = h.new_array(static_cast<uint32_t>(count));  // safepoint
  if (!res || h.over_cap()) { err = "out of memory"; return false; }
  ArrayObj* src = as_arr(a[0]);   // re-read after the alloc
  for (int64_t i = 0; i < count; ++i) res->slots->data[i] = src->slots->data[start + i];
  out = Value::object(res);
  return true;
}

bool reverse_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "reverse expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  for (uint32_t i = 0, j = arr->len; i + 1 < j; ++i) {
    --j;
    std::swap(arr->slots->data[i], arr->slots->data[j]);
  }
  out = a[0];  // in place; no allocation
  return true;
}

bool sort_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "sort expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  for (uint32_t i = 0; i < arr->len; ++i)
    if (!is_num(arr->slots->data[i])) { err = "sort expects numbers"; return false; }
  std::sort(arr->slots->data, arr->slots->data + arr->len,
            [](const Value& x, const Value& y) { return to_double(x) < to_double(y); });
  out = Value::nil();
  return true;
}

bool arr_index_of_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "arr_index_of expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  for (uint32_t i = 0; i < arr->len; ++i)
    if (val_equal(arr->slots->data[i], a[1])) { out = Value::integer(i); return true; }
  out = Value::integer(-1);
  return true;
}

// =======================================================================
// Map
// =======================================================================

// keys/values allocate exactly one array; the (immutable) entries are copied
// from the map after re-reading it past the allocation.
bool keys_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "keys expects a map"; return false; }
  uint32_t len = as_map(a[0])->len;
  ArrayObj* arr = h.new_array(len);  // safepoint
  if (!arr || h.over_cap()) { err = "out of memory"; return false; }
  MapObj* m = as_map(a[0]);          // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) arr->slots->data[i] = m->keys->data[i];
  out = Value::object(arr);
  return true;
}

bool values_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "values expects a map"; return false; }
  uint32_t len = as_map(a[0])->len;
  ArrayObj* arr = h.new_array(len);  // safepoint
  if (!arr || h.over_cap()) { err = "out of memory"; return false; }
  MapObj* m = as_map(a[0]);          // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) arr->slots->data[i] = m->vals->data[i];
  out = Value::object(arr);
  return true;
}

int map_index(MapObj* m, const char* key, uint32_t klen) {
  for (uint32_t i = 0; i < m->len; ++i) {
    StringObj* ks = static_cast<StringObj*>(m->keys->data[i].as.obj);
    if (ks->len == klen && std::memcmp(ks->bytes->data, key, klen) == 0) return static_cast<int>(i);
  }
  return -1;
}

bool has_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "has expects a map"; return false; }
  if (!is_str(a[1])) { err = "has key must be a string"; return false; }
  out = Value::boolean(map_index(as_map(a[0]), sbytes(a[1]), slen(a[1])) >= 0);
  return true;
}

bool remove_key_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "remove_key expects a map"; return false; }
  if (!is_str(a[1])) { err = "remove_key key must be a string"; return false; }
  MapObj* m = as_map(a[0]);
  int j = map_index(m, sbytes(a[1]), slen(a[1]));
  if (j < 0) { out = Value::boolean(false); return true; }
  for (uint32_t i = static_cast<uint32_t>(j); i + 1 < m->len; ++i) {
    m->keys->data[i] = m->keys->data[i + 1];
    m->vals->data[i] = m->vals->data[i + 1];
  }
  m->len--;
  out = Value::boolean(true);
  return true;
}

// Safepoint-correct insert/overwrite (mirrors interp's map_set): every new_*
// here can move m, its slots, and val, so each is rooted and re-read. `key`
// points to caller-owned, non-moving storage (a std::string), never a heap
// object, so it stays valid across the allocations.
void map_put(MapObj* m, const char* key, uint32_t klen, Value val, Heap& h) {
  int j = map_index(m, key, klen);
  if (j >= 0) { m->vals->data[j] = val; return; }

  HandleScope hs(h);
  size_t mi = hs.root(m);
  bool val_obj = (val.tag == Tag::Obj && val.as.obj);
  size_t vi = val_obj ? hs.root(val.as.obj) : 0;

  if (m->len == m->cap) {
    uint32_t newcap = m->cap ? m->cap * 2 : 4;
    SlotsObj* nk = h.new_slots(newcap);
    if (!nk) return;
    size_t ki = hs.root(nk);
    SlotsObj* nv = h.new_slots(newcap);
    if (!nv) return;
    size_t nvi = hs.root(nv);
    m = hs.get<MapObj>(mi);
    nk = hs.get<SlotsObj>(ki);
    nv = hs.get<SlotsObj>(nvi);
    for (uint32_t i = 0; i < m->len; ++i) {
      nk->data[i] = m->keys->data[i];
      nv->data[i] = m->vals->data[i];
    }
    m->keys = nk;
    m->vals = nv;
    m->cap = newcap;
  }

  StringObj* ks = h.new_string(key, klen);
  if (!ks) return;
  m = hs.get<MapObj>(mi);
  if (val_obj) val.as.obj = hs.get<Object>(vi);
  uint32_t i = m->len;
  m->keys->data[i] = Value::object(ks);
  m->vals->data[i] = val;
  m->len = i + 1;
}

// =======================================================================
// Batch 2 — more predicates / math / string / array / map
// =======================================================================

bool is_int_fn(Value* a, uint32_t, Heap&, Value& out, std::string&)  { out = Value::boolean(a[0].tag == Tag::Int); return true; }
bool is_bool_fn(Value* a, uint32_t, Heap&, Value& out, std::string&) { out = Value::boolean(a[0].tag == Tag::Bool); return true; }

// --- math ---

// clamp(x, lo, hi): constrain x to the inclusive range [lo, hi]. The bounds are
// validated, and integer inputs are kept as integers so `clamp(15, 0, 10)`
// returns the integer 10 rather than 10.0.
bool clamp_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  const Value& value = a[0];
  const Value& lo = a[1];
  const Value& hi = a[2];

  if (!is_num(value) || !is_num(lo) || !is_num(hi)) {
    err = "clamp expects numbers";
    return false;
  }
  if (to_double(lo) > to_double(hi)) {
    err = "clamp: lo > hi";
    return false;
  }

  const bool all_int = (value.tag == Tag::Int && lo.tag == Tag::Int && hi.tag == Tag::Int);
  if (all_int) {
    const int64_t x = value.as.i;
    const int64_t lower = lo.as.i;
    const int64_t upper = hi.as.i;
    int64_t result;
    if (x < lower) {
      result = lower;
    } else if (x > upper) {
      result = upper;
    } else {
      result = x;
    }
    out = Value::integer(result);
  } else {
    const double x = to_double(value);
    const double lower = to_double(lo);
    const double upper = to_double(hi);
    double result;
    if (x < lower) {
      result = lower;
    } else if (x > upper) {
      result = upper;
    } else {
      result = x;
    }
    out = Value::number(result);
  }
  return true;
}

uint64_t u_abs(int64_t v) { return v < 0 ? (0u - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v); }
uint64_t gcd_u(uint64_t x, uint64_t y) { while (y) { uint64_t t = x % y; x = y; y = t; } return x; }

bool gcd_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int || a[1].tag != Tag::Int) { err = "gcd expects integers"; return false; }
  uint64_t g = gcd_u(u_abs(a[0].as.i), u_abs(a[1].as.i));
  if (g > static_cast<uint64_t>(kI64Max)) { err = "gcd out of range"; return false; }
  out = Value::integer(static_cast<int64_t>(g));
  return true;
}

bool lcm_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int || a[1].tag != Tag::Int) { err = "lcm expects integers"; return false; }
  uint64_t x = u_abs(a[0].as.i), y = u_abs(a[1].as.i);
  if (x == 0 || y == 0) { out = Value::integer(0); return true; }
  uint64_t aa = x / gcd_u(x, y);
  if (aa > UINT64_MAX / y) { err = "lcm overflow"; return false; }
  uint64_t r = aa * y;
  if (r > static_cast<uint64_t>(kI64Max)) { err = "lcm overflow"; return false; }
  out = Value::integer(static_cast<int64_t>(r));
  return true;
}

bool math1(Value* a, double (*op)(double), const char* name, bool pos_only,
           Value& out, std::string& err) {
  if (!is_num(a[0])) { err = std::string(name) + " expects a number"; return false; }
  double d = to_double(a[0]);
  if (pos_only && d <= 0) { err = std::string(name) + " domain error"; return false; }
  out = Value::number(op(d));
  return true;
}
bool exp_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)   { return math1(a, std::exp,   "exp",   false, out, err); }
bool log_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)   { return math1(a, std::log,   "log",   true,  out, err); }
bool log2_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)  { return math1(a, std::log2,  "log2",  true,  out, err); }
bool log10_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return math1(a, std::log10, "log10", true,  out, err); }
bool sin_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)   { return math1(a, std::sin,   "sin",   false, out, err); }
bool cos_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)   { return math1(a, std::cos,   "cos",   false, out, err); }
bool tan_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)   { return math1(a, std::tan,   "tan",   false, out, err); }

bool atan2_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "atan2 expects numbers"; return false; }
  out = Value::number(std::atan2(to_double(a[0]), to_double(a[1])));
  return true;
}
bool hypot_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "hypot expects numbers"; return false; }
  out = Value::number(std::hypot(to_double(a[0]), to_double(a[1])));
  return true;
}
bool trunc_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag == Tag::Int) { out = a[0]; return true; }
  if (a[0].tag != Tag::Double) { err = "trunc expects a number"; return false; }
  double t = std::trunc(a[0].as.d);
  if (!(t >= -9.2e18 && t <= 9.2e18)) { err = "value out of integer range"; return false; }
  out = Value::integer(static_cast<int64_t>(t));
  return true;
}

// --- string ---

bool replace_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1]) || !is_str(a[2])) { err = "replace expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string from(sbytes(a[1]), slen(a[1]));
  std::string to(sbytes(a[2]), slen(a[2]));
  if (from.empty()) { err = "replace pattern must be non-empty"; return false; }
  std::string r;
  size_t pos = 0, f;
  while ((f = s.find(from, pos)) != std::string::npos) {
    r.append(s, pos, f - pos);
    r += to;
    pos = f + from.size();
  }
  r.append(s, pos, std::string::npos);
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

bool last_index_of_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "last_index_of expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sub(sbytes(a[1]), slen(a[1]));
  size_t p = s.rfind(sub);
  out = Value::integer(p == std::string::npos ? -1 : static_cast<int64_t>(p));
  return true;
}

bool pad_side(Value* a, bool left, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "pad expects a string"; return false; }
  if (!is_num(a[1])) { err = "pad width must be a number"; return false; }
  if (!is_str(a[2]) || slen(a[2]) != 1) { err = "pad fill must be a 1-char string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  char fill = sbytes(a[2])[0];
  int64_t width = as_i64(a[1]);
  if (width < 0) width = 0;
  if (width > (1 << 20)) { err = "pad width too large"; return false; }
  std::string r;
  if (static_cast<uint64_t>(width) <= s.size()) {
    r = s;
  } else {
    std::string padding(static_cast<size_t>(width) - s.size(), fill);
    r = left ? padding + s : s + padding;
  }
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}
bool pad_left_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err)  { return pad_side(a, true,  h, out, err); }
bool pad_right_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) { return pad_side(a, false, h, out, err); }

bool count_sub_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "count_sub expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sub(sbytes(a[1]), slen(a[1]));
  if (sub.empty()) { err = "count_sub pattern must be non-empty"; return false; }
  int64_t count = 0;
  size_t pos = 0, f;
  while ((f = s.find(sub, pos)) != std::string::npos) { ++count; pos = f + sub.size(); }
  out = Value::integer(count);
  return true;
}

bool capitalize_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "capitalize expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  for (size_t i = 0; i < s.size(); ++i) {
    char& c = s[i];
    if (i == 0) { if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32); }
    else        { if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32); }
  }
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool reverse_str_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "reverse_str expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::reverse(s.begin(), s.end());
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

// lines: multi-alloc, same discipline as split.
bool lines_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "lines expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::vector<std::string> parts;
  size_t pos = 0, f;
  while ((f = s.find('\n', pos)) != std::string::npos) { parts.push_back(s.substr(pos, f - pos)); pos = f + 1; }
  parts.push_back(s.substr(pos));
  uint32_t count = static_cast<uint32_t>(parts.size());
  Value rv;
  if (!make_array(h, count, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t ai = hs.root(rv.as.obj);
  for (uint32_t i = 0; i < count; ++i) {
    StringObj* sObj = h.new_string(parts[i].data(), static_cast<uint32_t>(parts[i].size()));
    if (!sObj || h.over_cap()) { err = "out of memory"; return false; }
    hs.get<ArrayObj>(ai)->slots->data[i] = Value::object(sObj);  // re-read result
  }
  out = Value::object(hs.get<ArrayObj>(ai));
  return true;
}

bool is_empty_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (is_str(a[0]))   { out = Value::boolean(slen(a[0]) == 0); return true; }
  if (is_array(a[0])) { out = Value::boolean(as_arr(a[0])->len == 0); return true; }
  if (is_map(a[0]))   { out = Value::boolean(as_map(a[0])->len == 0); return true; }
  err = "is_empty expects a string, array, or map";
  return false;
}

// --- array ---

bool range_fn(Value* a, uint32_t n, Heap& h, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int || a[1].tag != Tag::Int) { err = "range expects integers"; return false; }
  int64_t start = a[0].as.i, end = a[1].as.i, step = 1;
  if (n == 3) {
    if (a[2].tag != Tag::Int) { err = "range step must be an integer"; return false; }
    step = a[2].as.i;
  }
  if (step == 0) { err = "range step must be non-zero"; return false; }
  uint64_t count = 0;
  if (step > 0 && end > start) {
    uint64_t span = static_cast<uint64_t>(end) - static_cast<uint64_t>(start);
    count = (span + static_cast<uint64_t>(step) - 1) / static_cast<uint64_t>(step);
  } else if (step < 0 && end < start) {
    uint64_t span = static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
    uint64_t st = u_abs(step);
    count = (span + st - 1) / st;
  }
  if (count > (1u << 20)) { err = "range too large"; return false; }
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(count), rv, err)) return false;  // safepoint
  ArrayObj* arr = static_cast<ArrayObj*>(rv.as.obj);  // rv local; no more allocs follow
  int64_t v = start;
  for (uint64_t i = 0; i < count; ++i) {
    arr->slots->data[i] = Value::integer(v);
    v = static_cast<int64_t>(static_cast<uint64_t>(v) + static_cast<uint64_t>(step));  // wrap-safe
  }
  out = rv;
  return true;
}

bool fill_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "fill count must be a number"; return false; }
  int64_t c = as_i64(a[0]);
  if (c < 0) { err = "fill count must be non-negative"; return false; }
  if (c > (1 << 20)) { err = "fill count too large"; return false; }
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(c), rv, err)) return false;  // safepoint
  ArrayObj* arr = static_cast<ArrayObj*>(rv.as.obj);
  Value val = a[1];  // re-read the value AFTER the allocation (it may have moved)
  for (int64_t i = 0; i < c; ++i) arr->slots->data[i] = val;
  out = rv;
  return true;
}

bool concat_arr_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0]) || !is_array(a[1])) { err = "concat_arr expects two arrays"; return false; }
  uint64_t la = as_arr(a[0])->len, lb = as_arr(a[1])->len;  // lengths only
  if (la + lb > 0xFFFFFFFFu) { err = "concat_arr result too large"; return false; }
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(la + lb), rv, err)) return false;  // safepoint: moves a,b
  ArrayObj* r  = static_cast<ArrayObj*>(rv.as.obj);
  ArrayObj* ra = as_arr(a[0]);  // re-read after the alloc
  ArrayObj* rb = as_arr(a[1]);
  for (uint64_t i = 0; i < la; ++i) r->slots->data[i] = ra->slots->data[i];
  for (uint64_t i = 0; i < lb; ++i) r->slots->data[la + i] = rb->slots->data[i];
  out = rv;
  return true;
}

bool sum_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "sum expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  bool any_double = false;
  int64_t si = 0;
  double sd = 0;
  for (uint32_t i = 0; i < arr->len; ++i) {
    const Value& e = arr->slots->data[i];
    if (!is_num(e)) { err = "sum expects numbers"; return false; }
    if (e.tag == Tag::Double) any_double = true;
    si = static_cast<int64_t>(static_cast<uint64_t>(si) + static_cast<uint64_t>(as_i64(e)));
    sd += to_double(e);
  }
  out = any_double ? Value::number(sd) : Value::integer(si);
  return true;
}

bool product_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "product expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  bool any_double = false;
  int64_t pi = 1;
  double pd = 1;
  for (uint32_t i = 0; i < arr->len; ++i) {
    const Value& e = arr->slots->data[i];
    if (!is_num(e)) { err = "product expects numbers"; return false; }
    if (e.tag == Tag::Double) any_double = true;
    pi = static_cast<int64_t>(static_cast<uint64_t>(pi) * static_cast<uint64_t>(as_i64(e)));
    pd *= to_double(e);
  }
  out = any_double ? Value::number(pd) : Value::integer(pi);
  return true;
}

bool min_max_of(Value* a, bool want_min, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  if (arr->len == 0) { err = "empty array"; return false; }
  if (!is_num(arr->slots->data[0])) { err = "expects numbers"; return false; }
  Value best = arr->slots->data[0];
  double bd = to_double(best);
  for (uint32_t i = 1; i < arr->len; ++i) {
    const Value& e = arr->slots->data[i];
    if (!is_num(e)) { err = "expects numbers"; return false; }
    double d = to_double(e);
    if ((want_min && d < bd) || (!want_min && d > bd)) { best = e; bd = d; }
  }
  out = best;
  return true;
}
bool min_of_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return min_max_of(a, true,  out, err); }
bool max_of_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return min_max_of(a, false, out, err); }

bool first_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "first expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  if (arr->len == 0) { err = "first of empty array"; return false; }
  out = arr->slots->data[0];
  return true;
}
bool last_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "last expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  if (arr->len == 0) { err = "last of empty array"; return false; }
  out = arr->slots->data[arr->len - 1];
  return true;
}

bool take_drop(Value* a, bool take, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "expects an array"; return false; }
  if (!is_num(a[1])) { err = "count must be a number"; return false; }
  int64_t len = static_cast<int64_t>(as_arr(a[0])->len);
  int64_t k = as_i64(a[1]);
  if (k < 0) k = 0;
  if (k > len) k = len;
  int64_t start = take ? 0 : k;
  int64_t count = take ? k : len - k;
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(count), rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  ArrayObj* src = as_arr(a[0]);  // re-read after the alloc
  for (int64_t i = 0; i < count; ++i) r->slots->data[i] = src->slots->data[start + i];
  out = rv;
  return true;
}
bool take_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) { return take_drop(a, true,  h, out, err); }
bool drop_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) { return take_drop(a, false, h, out, err); }

bool count_of_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "count_of expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  int64_t c = 0;
  for (uint32_t i = 0; i < arr->len; ++i) if (val_equal(arr->slots->data[i], a[1])) ++c;
  out = Value::integer(c);
  return true;
}

// --- map ---

bool get_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "get expects a map"; return false; }
  if (!is_str(a[1])) { err = "get key must be a string"; return false; }
  int j = map_index(as_map(a[0]), sbytes(a[1]), slen(a[1]));
  out = (j >= 0) ? as_map(a[0])->vals->data[j] : Value::nil();
  return true;
}

bool set_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "set expects a map"; return false; }
  if (!is_str(a[1])) { err = "set key must be a string"; return false; }
  std::string key(sbytes(a[1]), slen(a[1]));  // copy: map_put allocates and moves the key string
  map_put(as_map(a[0]), key.data(), static_cast<uint32_t>(key.size()), a[2], h);
  if (h.over_cap()) { err = "out of memory"; return false; }
  out = Value::nil();
  return true;
}

// merge: new map with a's entries then b's (b wins). Re-reads sources each
// iteration; keys are copied into C++ locals before each (allocating) map_put.
bool merge_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0]) || !is_map(a[1])) { err = "merge expects two maps"; return false; }
  Value rv;
  if (!make_map(h, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t mi = hs.root(rv.as.obj);
  for (int src = 0; src < 2; ++src) {
    uint32_t len = as_map(a[src])->len;  // sources are not mutated -> len constant
    for (uint32_t i = 0; i < len; ++i) {
      MapObj* sm = as_map(a[src]);  // re-read source (register root)
      StringObj* ks = static_cast<StringObj*>(sm->keys->data[i].as.obj);
      std::string key(ks->bytes->data, ks->len);
      Value val = sm->vals->data[i];
      MapObj* nm = hs.get<MapObj>(mi);
      map_put(nm, key.data(), static_cast<uint32_t>(key.size()), val, h);  // allocates
      if (h.over_cap()) { err = "out of memory"; return false; }
    }
  }
  out = Value::object(hs.get<MapObj>(mi));
  return true;
}

// entries: array of [key, val] pairs. Outer array is rooted; each inner pair
// allocation can move the result + map, so both are re-read after it.
bool entries_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "entries expects a map"; return false; }
  uint32_t n = as_map(a[0])->len;
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t ri = hs.root(rv.as.obj);
  for (uint32_t i = 0; i < n; ++i) {
    Value pair;
    if (!make_array(h, 2, pair, err)) return false;  // safepoint: moves result + map
    MapObj* mm = as_map(a[0]);                        // re-read map
    ArrayObj* p = static_cast<ArrayObj*>(pair.as.obj);
    p->slots->data[0] = mm->keys->data[i];
    p->slots->data[1] = mm->vals->data[i];
    hs.get<ArrayObj>(ri)->slots->data[i] = Value::object(p);  // re-read result
  }
  out = Value::object(hs.get<ArrayObj>(ri));
  return true;
}

// =======================================================================
// Batch 3 — extended math / sequence / string utilities
//
// Same rules as the earlier batches: single-allocation builtins read their
// scalars from `args` first and allocate the result last; multi-allocation
// builtins root the growing result in a HandleScope and re-read both the result
// and any source argument after every `new_*`.
// =======================================================================

// --- math ---

// n! as an int64. 20! is the largest factorial that fits in a signed 64-bit
// integer, so anything larger is rejected rather than silently overflowing.
bool factorial_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "factorial expects an integer"; return false; }
  int64_t n = a[0].as.i;
  if (n < 0) { err = "factorial of a negative number"; return false; }
  if (n > 20) { err = "factorial too large"; return false; }
  int64_t result = 1;
  for (int64_t i = 2; i <= n; ++i) result *= i;
  out = Value::integer(result);
  return true;
}

// Trial division up to sqrt(n). Small, exact, and good enough for a scripting
// builtin; no allocation, so it is always safepoint-safe.
bool is_prime_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "is_prime expects an integer"; return false; }
  int64_t n = a[0].as.i;
  if (n < 2) { out = Value::boolean(false); return true; }
  if (n < 4) { out = Value::boolean(true); return true; }
  if ((n % 2) == 0) { out = Value::boolean(false); return true; }
  for (int64_t d = 3; d <= n / d; d += 2) {
    if ((n % d) == 0) { out = Value::boolean(false); return true; }
  }
  out = Value::boolean(true);
  return true;
}

// nth Fibonacci number. fib(92) is the largest that fits in a signed 64-bit
// integer; beyond that we report an error instead of wrapping.
bool fib_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "fib expects an integer"; return false; }
  int64_t n = a[0].as.i;
  if (n < 0) { err = "fib of a negative number"; return false; }
  if (n > 92) { err = "fib too large"; return false; }
  int64_t prev = 0, cur = 1;
  for (int64_t i = 0; i < n; ++i) {
    int64_t next = prev + cur;
    prev = cur;
    cur = next;
  }
  out = Value::integer(prev);
  return true;
}

bool cbrt_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "cbrt expects a number"; return false; }
  out = Value::number(std::cbrt(to_double(a[0])));
  return true;
}

bool deg2rad_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "deg2rad expects a number"; return false; }
  out = Value::number(to_double(a[0]) * (kPi / 180.0));
  return true;
}

bool rad2deg_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "rad2deg expects a number"; return false; }
  out = Value::number(to_double(a[0]) * (180.0 / kPi));
  return true;
}

bool log_base_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "log_base expects numbers"; return false; }
  double x = to_double(a[0]);
  double base = to_double(a[1]);
  if (x <= 0) { err = "log_base domain error"; return false; }
  if (base <= 0 || base == 1) { err = "log_base: invalid base"; return false; }
  out = Value::number(std::log(x) / std::log(base));
  return true;
}

bool mean_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "mean expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  if (arr->len == 0) { err = "mean of empty array"; return false; }
  double total = 0;
  for (uint32_t i = 0; i < arr->len; ++i) {
    const Value& e = arr->slots->data[i];
    if (!is_num(e)) { err = "mean expects numbers"; return false; }
    total += to_double(e);
  }
  out = Value::number(total / static_cast<double>(arr->len));
  return true;
}

// --- array reductions / predicates (no allocation) ---

bool any_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "any expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  for (uint32_t i = 0; i < arr->len; ++i)
    if (arr->slots->data[i].truthy()) { out = Value::boolean(true); return true; }
  out = Value::boolean(false);
  return true;
}

bool all_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "all expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  for (uint32_t i = 0; i < arr->len; ++i)
    if (!arr->slots->data[i].truthy()) { out = Value::boolean(false); return true; }
  out = Value::boolean(true);
  return true;
}

bool includes_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "includes expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  for (uint32_t i = 0; i < arr->len; ++i)
    if (val_equal(arr->slots->data[i], a[1])) { out = Value::boolean(true); return true; }
  out = Value::boolean(false);
  return true;
}

bool index_extreme(Value* a, bool want_min, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  if (arr->len == 0) { err = "empty array"; return false; }
  if (!is_num(arr->slots->data[0])) { err = "expects numbers"; return false; }
  uint32_t best = 0;
  double bd = to_double(arr->slots->data[0]);
  for (uint32_t i = 1; i < arr->len; ++i) {
    const Value& e = arr->slots->data[i];
    if (!is_num(e)) { err = "expects numbers"; return false; }
    double d = to_double(e);
    if ((want_min && d < bd) || (!want_min && d > bd)) { best = i; bd = d; }
  }
  out = Value::integer(best);
  return true;
}
bool index_min_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return index_extreme(a, true,  out, err); }
bool index_max_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return index_extreme(a, false, out, err); }

// --- array transforms (allocate) ---

// zip/enumerate: outer array of 2-element pairs. The outer array is rooted; each
// inner pair allocation can move the outer array AND the sources, so they are
// all re-read after every pair allocation.
bool zip_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0]) || !is_array(a[1])) { err = "zip expects two arrays"; return false; }
  uint32_t la = as_arr(a[0])->len, lb = as_arr(a[1])->len;
  uint32_t n = la < lb ? la : lb;
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t ri = hs.root(rv.as.obj);
  for (uint32_t i = 0; i < n; ++i) {
    Value pair;
    if (!make_array(h, 2, pair, err)) return false;  // safepoint: moves result + sources
    ArrayObj* p = static_cast<ArrayObj*>(pair.as.obj);
    p->slots->data[0] = as_arr(a[0])->slots->data[i];  // re-read sources
    p->slots->data[1] = as_arr(a[1])->slots->data[i];
    hs.get<ArrayObj>(ri)->slots->data[i] = Value::object(p);  // re-read result
  }
  out = Value::object(hs.get<ArrayObj>(ri));
  return true;
}

bool enumerate_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "enumerate expects an array"; return false; }
  uint32_t n = as_arr(a[0])->len;
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t ri = hs.root(rv.as.obj);
  for (uint32_t i = 0; i < n; ++i) {
    Value pair;
    if (!make_array(h, 2, pair, err)) return false;  // safepoint
    ArrayObj* p = static_cast<ArrayObj*>(pair.as.obj);
    p->slots->data[0] = Value::integer(i);
    p->slots->data[1] = as_arr(a[0])->slots->data[i];  // re-read source
    hs.get<ArrayObj>(ri)->slots->data[i] = Value::object(p);  // re-read result
  }
  out = Value::object(hs.get<ArrayObj>(ri));
  return true;
}

// flatten one level: the total length is computed up front (no allocation), so
// after the single result allocation the source is copied in one pass with no
// further allocation and stays valid throughout.
bool flatten_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "flatten expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  uint64_t total = 0;
  for (uint32_t i = 0; i < src->len; ++i) {
    const Value& e = src->slots->data[i];
    total += is_array(e) ? as_arr(e)->len : 1;
  }
  if (total > 0xFFFFFFFFu) { err = "flatten result too large"; return false; }
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(total), rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  uint32_t w = 0;
  for (uint32_t i = 0; i < src->len; ++i) {
    const Value& e = src->slots->data[i];
    if (is_array(e)) {
      ArrayObj* sub = as_arr(e);
      for (uint32_t j = 0; j < sub->len; ++j) r->slots->data[w++] = sub->slots->data[j];
    } else {
      r->slots->data[w++] = e;
    }
  }
  out = rv;
  return true;
}

// distinct values by value-equality. The surviving indices are collected first
// (no allocation), then copied into a single result array.
bool unique_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "unique expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  std::vector<uint32_t> keep;
  for (uint32_t i = 0; i < src->len; ++i) {
    bool seen = false;
    for (uint32_t k : keep) {
      if (val_equal(src->slots->data[k], src->slots->data[i])) { seen = true; break; }
    }
    if (!seen) keep.push_back(i);
  }
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(keep.size()), rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  for (size_t i = 0; i < keep.size(); ++i) r->slots->data[i] = src->slots->data[keep[i]];
  out = rv;
  return true;
}

bool rotate_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "rotate expects an array"; return false; }
  if (!is_num(a[1])) { err = "rotate amount must be a number"; return false; }
  uint32_t len = as_arr(a[0])->len;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  if (len == 0) { out = rv; return true; }
  // Normalize the (possibly negative) shift into [0, len) using modular math.
  int64_t raw = as_i64(a[1]) % static_cast<int64_t>(len);
  uint32_t shift = static_cast<uint32_t>((raw + static_cast<int64_t>(len)) % static_cast<int64_t>(len));
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  ArrayObj* src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) r->slots->data[i] = src->slots->data[(i + shift) % len];
  out = rv;
  return true;
}

// chunk: outer array of fixed-size sub-arrays (the final chunk may be shorter).
// Outer rooted; each inner chunk allocation re-reads the outer array + source.
bool chunk_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "chunk expects an array"; return false; }
  if (!is_num(a[1])) { err = "chunk size must be a number"; return false; }
  int64_t size = as_i64(a[1]);
  if (size <= 0) { err = "chunk size must be positive"; return false; }
  uint32_t len = as_arr(a[0])->len;
  uint32_t usize = static_cast<uint32_t>(size);
  uint32_t nchunks = (len + usize - 1) / usize;
  Value rv;
  if (!make_array(h, nchunks, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t ri = hs.root(rv.as.obj);
  for (uint32_t c = 0; c < nchunks; ++c) {
    uint32_t start = c * usize;
    uint32_t clen = (len - start < usize) ? (len - start) : usize;
    Value child;
    if (!make_array(h, clen, child, err)) return false;  // safepoint: moves result + source
    ArrayObj* ch = static_cast<ArrayObj*>(child.as.obj);
    ArrayObj* src = as_arr(a[0]);  // re-read source
    for (uint32_t j = 0; j < clen; ++j) ch->slots->data[j] = src->slots->data[start + j];
    hs.get<ArrayObj>(ri)->slots->data[c] = Value::object(ch);  // re-read result
  }
  out = Value::object(hs.get<ArrayObj>(ri));
  return true;
}

// --- string predicates (no allocation) ---

bool char_class(Value* a, bool (*pred)(unsigned char), const char* name,
                Value& out, std::string& err) {
  if (!is_str(a[0])) { err = std::string(name) + " expects a string"; return false; }
  uint32_t n = slen(a[0]);
  if (n == 0) { out = Value::boolean(false); return true; }
  const char* p = sbytes(a[0]);
  for (uint32_t i = 0; i < n; ++i)
    if (!pred(static_cast<unsigned char>(p[i]))) { out = Value::boolean(false); return true; }
  out = Value::boolean(true);
  return true;
}
bool cc_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool cc_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool cc_alnum(unsigned char c) { return cc_digit(c) || cc_alpha(c); }
bool cc_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool is_digit_str_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) { return char_class(a, cc_digit, "is_digit", out, err); }
bool is_alpha_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)     { return char_class(a, cc_alpha, "is_alpha", out, err); }
bool is_alnum_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)     { return char_class(a, cc_alnum, "is_alnum", out, err); }
bool is_space_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err)     { return char_class(a, cc_space, "is_space", out, err); }

// --- string transforms (allocate) ---

bool swapcase_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "swapcase expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool title_case_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "title_case expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  bool at_word_start = true;
  for (char& c : s) {
    bool is_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!is_letter) {
      at_word_start = true;
    } else if (at_word_start) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
      at_word_start = false;
    } else {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
  }
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool zfill_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "zfill expects a string"; return false; }
  if (!is_num(a[1])) { err = "zfill width must be a number"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  int64_t width = as_i64(a[1]);
  if (width < 0) width = 0;
  if (width > (1 << 20)) { err = "zfill width too large"; return false; }
  std::string r;
  if (static_cast<uint64_t>(width) <= s.size()) {
    r = s;
  } else {
    r = std::string(static_cast<size_t>(width) - s.size(), '0') + s;
  }
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

bool center_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "center expects a string"; return false; }
  if (!is_num(a[1])) { err = "center width must be a number"; return false; }
  if (!is_str(a[2]) || slen(a[2]) != 1) { err = "center fill must be a 1-char string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  char fill = sbytes(a[2])[0];
  int64_t width = as_i64(a[1]);
  if (width < 0) width = 0;
  if (width > (1 << 20)) { err = "center width too large"; return false; }
  std::string r;
  if (static_cast<uint64_t>(width) <= s.size()) {
    r = s;
  } else {
    size_t pad = static_cast<size_t>(width) - s.size();
    size_t left = pad / 2;
    size_t right = pad - left;
    r = std::string(left, fill) + s + std::string(right, fill);
  }
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

// chars: array of one-byte strings (multi-alloc; same discipline as split).
bool chars_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "chars expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));  // copy first
  uint32_t n = static_cast<uint32_t>(s.size());
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t ri = hs.root(rv.as.obj);
  for (uint32_t i = 0; i < n; ++i) {
    StringObj* one = h.new_string(s.data() + i, 1);  // safepoint
    if (!one || h.over_cap()) { err = "out of memory"; return false; }
    hs.get<ArrayObj>(ri)->slots->data[i] = Value::object(one);  // re-read result
  }
  out = Value::object(hs.get<ArrayObj>(ri));
  return true;
}

// to_bytes: array of int byte values (single allocation; source copied first).
bool to_bytes_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "to_bytes expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));  // copy first -> no source pointer held
  uint32_t n = static_cast<uint32_t>(s.size());
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  for (uint32_t i = 0; i < n; ++i)
    r->slots->data[i] = Value::integer(static_cast<uint8_t>(s[i]));
  out = rv;
  return true;
}

// format: replace each "{}" in the template with the rendered next argument.
// Variadic; renders args (read-only) into a C++ string, then allocates once.
bool format_fn(Value* a, uint32_t argc, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "format expects a string template"; return false; }
  std::string fmt(sbytes(a[0]), slen(a[0]));
  std::string r;
  uint32_t argi = 1;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (i + 1 < fmt.size() && fmt[i] == '{' && fmt[i + 1] == '}') {
      if (argi < argc) r += render(a[argi++]);
      else r += "{}";  // not enough arguments: leave the placeholder
      ++i;             // consume the '}'
    } else {
      r += fmt[i];
    }
  }
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

// --- map transforms (allocate) ---

// invert: new map mapping each value (which must be a string) back to its key.
// Multi-alloc via map_put; keys are copied into C++ locals before each insert.
bool invert_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "invert expects a map"; return false; }
  Value rv;
  if (!make_map(h, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t mi = hs.root(rv.as.obj);
  uint32_t len = as_map(a[0])->len;
  for (uint32_t i = 0; i < len; ++i) {
    MapObj* sm = as_map(a[0]);  // re-read source
    const Value& v = sm->vals->data[i];
    if (!is_str(v)) { err = "invert requires string values"; return false; }
    std::string newkey(sbytes(v), slen(v));
    Value newval = sm->keys->data[i];  // the old key (a StringObj) becomes the value
    MapObj* nm = hs.get<MapObj>(mi);
    map_put(nm, newkey.data(), static_cast<uint32_t>(newkey.size()), newval, h);  // allocates
    if (h.over_cap()) { err = "out of memory"; return false; }
  }
  out = Value::object(hs.get<MapObj>(mi));
  return true;
}

// pick: new map containing only the keys listed in the key array that the source
// map actually has. Keys are copied to C++ locals before each (allocating) put.
bool pick_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "pick expects a map"; return false; }
  if (!is_array(a[1])) { err = "pick expects a key array"; return false; }
  Value rv;
  if (!make_map(h, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t mi = hs.root(rv.as.obj);
  uint32_t klen = as_arr(a[1])->len;
  for (uint32_t i = 0; i < klen; ++i) {
    const Value& kv = as_arr(a[1])->slots->data[i];
    if (!is_str(kv)) { err = "pick keys must be strings"; return false; }
    std::string key(sbytes(kv), slen(kv));
    MapObj* sm = as_map(a[0]);
    int j = map_index(sm, key.data(), static_cast<uint32_t>(key.size()));
    if (j < 0) continue;
    Value val = sm->vals->data[j];
    MapObj* nm = hs.get<MapObj>(mi);
    map_put(nm, key.data(), static_cast<uint32_t>(key.size()), val, h);  // allocates
    if (h.over_cap()) { err = "out of memory"; return false; }
  }
  out = Value::object(hs.get<MapObj>(mi));
  return true;
}

// =======================================================================
// Batch 4 — extended math, sequence, string, and map utilities
//
// Same discipline as the earlier batches: a builtin reads every scalar it
// needs from `args` before it allocates and builds the result last; the few
// that allocate more than once root the result and re-read sources after each
// `new_*`. Integer-only entry points reject non-integers up front and do their
// arithmetic in unsigned where signed overflow would otherwise be UB.
// =======================================================================

constexpr int64_t kI64Min = -kI64Max - 1;

// --- math: inverse trig / hyperbolic / misc (no allocation) ---

bool asin_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "asin expects a number"; return false; }
  double d = to_double(a[0]);
  if (d < -1.0 || d > 1.0) { err = "asin domain error"; return false; }
  out = Value::number(std::asin(d));
  return true;
}
bool acos_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "acos expects a number"; return false; }
  double d = to_double(a[0]);
  if (d < -1.0 || d > 1.0) { err = "acos domain error"; return false; }
  out = Value::number(std::acos(d));
  return true;
}
bool atan_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "atan expects a number"; return false; }
  out = Value::number(std::atan(to_double(a[0])));
  return true;
}
bool sinh_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "sinh expects a number"; return false; }
  out = Value::number(std::sinh(to_double(a[0])));
  return true;
}
bool cosh_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "cosh expects a number"; return false; }
  out = Value::number(std::cosh(to_double(a[0])));
  return true;
}
bool tanh_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "tanh expects a number"; return false; }
  out = Value::number(std::tanh(to_double(a[0])));
  return true;
}
bool asinh_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "asinh expects a number"; return false; }
  out = Value::number(std::asinh(to_double(a[0])));
  return true;
}
bool acosh_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "acosh expects a number"; return false; }
  double d = to_double(a[0]);
  if (d < 1.0) { err = "acosh domain error"; return false; }
  out = Value::number(std::acosh(d));
  return true;
}
bool atanh_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "atanh expects a number"; return false; }
  double d = to_double(a[0]);
  if (d <= -1.0 || d >= 1.0) { err = "atanh domain error"; return false; }
  out = Value::number(std::atanh(d));
  return true;
}
bool expm1_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "expm1 expects a number"; return false; }
  out = Value::number(std::expm1(to_double(a[0])));
  return true;
}
bool log1p_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "log1p expects a number"; return false; }
  double d = to_double(a[0]);
  if (d <= -1.0) { err = "log1p domain error"; return false; }
  out = Value::number(std::log1p(d));
  return true;
}

bool fmod_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "fmod expects numbers"; return false; }
  double y = to_double(a[1]);
  if (y == 0.0) { err = "fmod by zero"; return false; }
  out = Value::number(std::fmod(to_double(a[0]), y));
  return true;
}
bool copysign_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "copysign expects numbers"; return false; }
  out = Value::number(std::copysign(to_double(a[0]), to_double(a[1])));
  return true;
}
bool ldexp_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "ldexp expects a number"; return false; }
  if (a[1].tag != Tag::Int) { err = "ldexp exponent must be an integer"; return false; }
  int64_t n = a[1].as.i;
  if (n < -100000) n = -100000;
  if (n > 100000) n = 100000;
  out = Value::number(std::ldexp(to_double(a[0]), static_cast<int>(n)));
  return true;
}

bool is_nan_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "is_nan expects a number"; return false; }
  out = Value::boolean(a[0].tag == Tag::Double && std::isnan(a[0].as.d));
  return true;
}
bool is_inf_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "is_inf expects a number"; return false; }
  out = Value::boolean(a[0].tag == Tag::Double && std::isinf(a[0].as.d));
  return true;
}
bool is_finite_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "is_finite expects a number"; return false; }
  out = Value::boolean(a[0].tag == Tag::Int || std::isfinite(a[0].as.d));
  return true;
}

bool lerp_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1]) || !is_num(a[2])) { err = "lerp expects numbers"; return false; }
  double x = to_double(a[0]), y = to_double(a[1]), t = to_double(a[2]);
  out = Value::number(x + (y - x) * t);
  return true;
}
bool smoothstep_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1]) || !is_num(a[2])) { err = "smoothstep expects numbers"; return false; }
  double e0 = to_double(a[0]), e1 = to_double(a[1]), x = to_double(a[2]);
  if (e0 == e1) { err = "smoothstep edges must differ"; return false; }
  double t = (x - e0) / (e1 - e0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  out = Value::number(t * t * (3.0 - 2.0 * t));
  return true;
}

// Integer square root: floor(sqrt(n)) for n >= 0. The float seed is corrected
// both ways; n <= INT64_MAX keeps (r+1)^2 inside uint64 so the compare is exact.
bool isqrt_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "isqrt expects an integer"; return false; }
  if (a[0].as.i < 0) { err = "isqrt of a negative number"; return false; }
  uint64_t x = static_cast<uint64_t>(a[0].as.i);
  uint64_t r = static_cast<uint64_t>(std::sqrt(static_cast<double>(x)));
  while (r != 0 && r * r > x) --r;
  while ((r + 1) * (r + 1) <= x) ++r;
  out = Value::integer(static_cast<int64_t>(r));
  return true;
}

// Floored modulo: the result takes the sign of the divisor (like Python's %).
bool mod_floor_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int || a[1].tag != Tag::Int) { err = "mod_floor expects integers"; return false; }
  int64_t x = a[0].as.i, m = a[1].as.i;
  if (m == 0) { err = "mod_floor by zero"; return false; }
  if (m == -1) { out = Value::integer(0); return true; }  // avoid INT64_MIN % -1 UB
  int64_t r = x % m;
  if (r != 0 && ((r < 0) != (m < 0))) r += m;
  out = Value::integer(r);
  return true;
}

bool bit_count_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "bit_count expects an integer"; return false; }
  uint64_t u = static_cast<uint64_t>(a[0].as.i);
  int64_t c = 0;
  while (u) { c += static_cast<int64_t>(u & 1u); u >>= 1; }
  out = Value::integer(c);
  return true;
}

bool next_pow2_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "next_pow2 expects an integer"; return false; }
  int64_t n = a[0].as.i;
  if (n < 1) { err = "next_pow2 expects a positive integer"; return false; }
  uint64_t p = 1;
  while (p < static_cast<uint64_t>(n)) {
    if (p > (static_cast<uint64_t>(kI64Max) >> 1)) { err = "next_pow2 overflow"; return false; }
    p <<= 1;
  }
  out = Value::integer(static_cast<int64_t>(p));
  return true;
}

// divmod(x, m) -> [quotient, remainder]. One allocation, no live object held.
bool divmod_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int || a[1].tag != Tag::Int) { err = "divmod expects integers"; return false; }
  int64_t x = a[0].as.i, m = a[1].as.i;
  if (m == 0) { err = "divmod by zero"; return false; }
  if (x == kI64Min && m == -1) { err = "divmod overflow"; return false; }
  int64_t q = x / m, r = x % m;
  Value rv;
  if (!make_array(h, 2, rv, err)) return false;  // safepoint
  ArrayObj* arr = static_cast<ArrayObj*>(rv.as.obj);
  arr->slots->data[0] = Value::integer(q);
  arr->slots->data[1] = Value::integer(r);
  out = rv;
  return true;
}

// --- string predicates (no allocation) ---

bool is_upper_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "is_upper expects a string"; return false; }
  const char* p = sbytes(a[0]);
  uint32_t n = slen(a[0]);
  bool has_letter = false;
  for (uint32_t i = 0; i < n; ++i) {
    char c = p[i];
    if (c >= 'a' && c <= 'z') { out = Value::boolean(false); return true; }
    if (c >= 'A' && c <= 'Z') has_letter = true;
  }
  out = Value::boolean(has_letter);
  return true;
}
bool is_lower_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "is_lower expects a string"; return false; }
  const char* p = sbytes(a[0]);
  uint32_t n = slen(a[0]);
  bool has_letter = false;
  for (uint32_t i = 0; i < n; ++i) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') { out = Value::boolean(false); return true; }
    if (c >= 'a' && c <= 'z') has_letter = true;
  }
  out = Value::boolean(has_letter);
  return true;
}

// --- string transforms (single allocation) ---

bool strip_prefix_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "strip_prefix expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string pre(sbytes(a[1]), slen(a[1]));
  if (pre.size() <= s.size() && std::memcmp(s.data(), pre.data(), pre.size()) == 0)
    return make_string(h, s.data() + pre.size(), static_cast<uint32_t>(s.size() - pre.size()), out, err);
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}
bool strip_suffix_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "strip_suffix expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string suf(sbytes(a[1]), slen(a[1]));
  if (suf.size() <= s.size() &&
      std::memcmp(s.data() + (s.size() - suf.size()), suf.data(), suf.size()) == 0)
    return make_string(h, s.data(), static_cast<uint32_t>(s.size() - suf.size()), out, err);
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool left_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "left expects a string"; return false; }
  if (!is_num(a[1])) { err = "left count must be a number"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  int64_t k = as_i64(a[1]);
  if (k < 0) k = 0;
  if (k > static_cast<int64_t>(s.size())) k = static_cast<int64_t>(s.size());
  return make_string(h, s.data(), static_cast<uint32_t>(k), out, err);
}
bool right_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "right expects a string"; return false; }
  if (!is_num(a[1])) { err = "right count must be a number"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  int64_t k = as_i64(a[1]);
  if (k < 0) k = 0;
  if (k > static_cast<int64_t>(s.size())) k = static_cast<int64_t>(s.size());
  size_t start = s.size() - static_cast<size_t>(k);
  return make_string(h, s.data() + start, static_cast<uint32_t>(k), out, err);
}

bool count_char_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "count_char expects strings"; return false; }
  if (slen(a[1]) != 1) { err = "count_char expects a 1-char string"; return false; }
  char target = sbytes(a[1])[0];
  const char* p = sbytes(a[0]);
  uint32_t n = slen(a[0]);
  int64_t c = 0;
  for (uint32_t i = 0; i < n; ++i) if (p[i] == target) ++c;
  out = Value::integer(c);
  return true;
}

// find_all: indices of every non-overlapping occurrence. Only ints are written
// into the result, so the single allocation holds no live object pointer.
bool find_all_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "find_all expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sub(sbytes(a[1]), slen(a[1]));
  if (sub.empty()) { err = "find_all pattern must be non-empty"; return false; }
  std::vector<int64_t> hits;
  size_t pos = 0, f;
  while ((f = s.find(sub, pos)) != std::string::npos) { hits.push_back(static_cast<int64_t>(f)); pos = f + sub.size(); }
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(hits.size()), rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  for (size_t i = 0; i < hits.size(); ++i) r->slots->data[i] = Value::integer(hits[i]);
  out = rv;
  return true;
}

bool replace_first_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1]) || !is_str(a[2])) { err = "replace_first expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string from(sbytes(a[1]), slen(a[1]));
  std::string to(sbytes(a[2]), slen(a[2]));
  if (from.empty()) { err = "replace_first pattern must be non-empty"; return false; }
  size_t f = s.find(from);
  std::string r = (f == std::string::npos) ? s : s.substr(0, f) + to + s.substr(f + from.size());
  return make_string(h, r.data(), static_cast<uint32_t>(r.size()), out, err);
}

bool rot13_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "rot13 expects a string"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>('a' + (c - 'a' + 13) % 26);
    else if (c >= 'A' && c <= 'Z') c = static_cast<char>('A' + (c - 'A' + 13) % 26);
  }
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

// --- array transforms / reductions ---

bool compact_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "compact expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  uint32_t keep = 0;
  for (uint32_t i = 0; i < src->len; ++i) if (src->slots->data[i].tag != Tag::Nil) ++keep;
  Value rv;
  if (!make_array(h, keep, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  uint32_t w = 0;
  for (uint32_t i = 0; i < src->len; ++i) {
    const Value& e = src->slots->data[i];
    if (e.tag != Tag::Nil) r->slots->data[w++] = e;
  }
  out = rv;
  return true;
}

bool cumsum_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "cumsum expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  bool any_double = false;
  for (uint32_t i = 0; i < src->len; ++i) {
    if (!is_num(src->slots->data[i])) { err = "cumsum expects numbers"; return false; }
    if (src->slots->data[i].tag == Tag::Double) any_double = true;
  }
  uint32_t n = src->len;
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  int64_t si = 0;
  double sd = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Value& e = src->slots->data[i];
    si = static_cast<int64_t>(static_cast<uint64_t>(si) + static_cast<uint64_t>(as_i64(e)));
    sd += to_double(e);
    r->slots->data[i] = any_double ? Value::number(sd) : Value::integer(si);
  }
  out = rv;
  return true;
}

bool dot_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0]) || !is_array(a[1])) { err = "dot expects two arrays"; return false; }
  ArrayObj* x = as_arr(a[0]);
  ArrayObj* y = as_arr(a[1]);
  if (x->len != y->len) { err = "dot expects equal-length arrays"; return false; }
  bool any_double = false;
  int64_t si = 0;
  double sd = 0;
  for (uint32_t i = 0; i < x->len; ++i) {
    const Value& xe = x->slots->data[i];
    const Value& ye = y->slots->data[i];
    if (!is_num(xe) || !is_num(ye)) { err = "dot expects numbers"; return false; }
    if (xe.tag == Tag::Double || ye.tag == Tag::Double) any_double = true;
    si = static_cast<int64_t>(static_cast<uint64_t>(si) +
                              static_cast<uint64_t>(as_i64(xe)) * static_cast<uint64_t>(as_i64(ye)));
    sd += to_double(xe) * to_double(ye);
  }
  out = any_double ? Value::number(sd) : Value::integer(si);
  return true;
}

bool intersperse_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "intersperse expects an array"; return false; }
  uint32_t len = as_arr(a[0])->len;
  uint32_t n = len <= 1 ? len : len * 2 - 1;
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  ArrayObj* src = as_arr(a[0]);  // re-read after the alloc
  Value sep = a[1];              // re-read separator from its register after the alloc
  for (uint32_t i = 0; i < len; ++i) {
    r->slots->data[i * 2] = src->slots->data[i];
    if (i + 1 < len) r->slots->data[i * 2 + 1] = sep;
  }
  out = rv;
  return true;
}

bool reverse_copy_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "reverse_copy expects an array"; return false; }
  uint32_t len = as_arr(a[0])->len;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  ArrayObj* src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) r->slots->data[i] = src->slots->data[len - 1 - i];
  out = rv;
  return true;
}

// sorted: a sorted copy (the in-place `sort` mutates; this leaves the source
// untouched). Numbers are validated before the allocation; the sort itself does
// not allocate, so the result pointer stays valid through it.
bool sorted_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "sorted expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  for (uint32_t i = 0; i < src->len; ++i)
    if (!is_num(src->slots->data[i])) { err = "sorted expects numbers"; return false; }
  uint32_t len = src->len;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) r->slots->data[i] = src->slots->data[i];
  std::sort(r->slots->data, r->slots->data + len,
            [](const Value& x, const Value& y) { return to_double(x) < to_double(y); });
  out = rv;
  return true;
}

bool is_sorted_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "is_sorted expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  for (uint32_t i = 0; i < arr->len; ++i)
    if (!is_num(arr->slots->data[i])) { err = "is_sorted expects numbers"; return false; }
  for (uint32_t i = 1; i < arr->len; ++i)
    if (to_double(arr->slots->data[i]) < to_double(arr->slots->data[i - 1])) {
      out = Value::boolean(false);
      return true;
    }
  out = Value::boolean(true);
  return true;
}

bool tail_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "tail expects an array"; return false; }
  if (as_arr(a[0])->len == 0) { err = "tail of empty array"; return false; }
  uint32_t len = as_arr(a[0])->len - 1;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  ArrayObj* src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) r->slots->data[i] = src->slots->data[i + 1];
  out = rv;
  return true;
}
bool init_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "init expects an array"; return false; }
  if (as_arr(a[0])->len == 0) { err = "init of empty array"; return false; }
  uint32_t len = as_arr(a[0])->len - 1;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  ArrayObj* src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) r->slots->data[i] = src->slots->data[i];
  out = rv;
  return true;
}

// --- map utilities ---

bool get_or_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "get_or expects a map"; return false; }
  if (!is_str(a[1])) { err = "get_or key must be a string"; return false; }
  int j = map_index(as_map(a[0]), sbytes(a[1]), slen(a[1]));
  out = (j >= 0) ? as_map(a[0])->vals->data[j] : a[2];
  return true;
}

bool has_value_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "has_value expects a map"; return false; }
  MapObj* m = as_map(a[0]);
  for (uint32_t i = 0; i < m->len; ++i)
    if (val_equal(m->vals->data[i], a[1])) { out = Value::boolean(true); return true; }
  out = Value::boolean(false);
  return true;
}

// omit: the complement of pick — a new map with every key NOT named in the key
// array. Multi-alloc via map_put; keys are copied into C++ locals before the
// (allocating) insert and sources are re-read after it.
bool omit_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "omit expects a map"; return false; }
  if (!is_array(a[1])) { err = "omit expects a key array"; return false; }
  ArrayObj* ks0 = as_arr(a[1]);
  for (uint32_t i = 0; i < ks0->len; ++i)
    if (!is_str(ks0->slots->data[i])) { err = "omit keys must be strings"; return false; }
  Value rv;
  if (!make_map(h, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t mi = hs.root(rv.as.obj);
  uint32_t len = as_map(a[0])->len;
  for (uint32_t i = 0; i < len; ++i) {
    MapObj* sm = as_map(a[0]);  // re-read source
    StringObj* kobj = static_cast<StringObj*>(sm->keys->data[i].as.obj);
    std::string key(kobj->bytes->data, kobj->len);
    bool drop = false;
    ArrayObj* ks = as_arr(a[1]);  // re-read the omit-key array
    for (uint32_t j = 0; j < ks->len; ++j) {
      StringObj* ok = static_cast<StringObj*>(ks->slots->data[j].as.obj);
      if (ok->len == key.size() && std::memcmp(ok->bytes->data, key.data(), key.size()) == 0) {
        drop = true;
        break;
      }
    }
    if (drop) continue;
    sm = as_map(a[0]);            // re-read before reading the value (no alloc since)
    Value val = sm->vals->data[i];
    MapObj* nm = hs.get<MapObj>(mi);
    map_put(nm, key.data(), static_cast<uint32_t>(key.size()), val, h);  // allocates
    if (h.over_cap()) { err = "out of memory"; return false; }
  }
  out = Value::object(hs.get<MapObj>(mi));
  return true;
}

// --- conversion / predicate ---

// Render |n| in the given base with a prefix, handling the sign separately so
// INT64_MIN never has to be negated as a signed value.
void int_to_base(int64_t n, unsigned base, const char* prefix, std::string& s) {
  uint64_t u = n < 0 ? (0u - static_cast<uint64_t>(n)) : static_cast<uint64_t>(n);
  static const char kDigits[] = "0123456789abcdef";
  std::string digits;
  if (u == 0) {
    digits = "0";
  } else {
    while (u) { digits += kDigits[u % base]; u /= base; }
    std::reverse(digits.begin(), digits.end());
  }
  s.clear();
  if (n < 0) s += '-';
  s += prefix;
  s += digits;
}

bool hex_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "hex expects an integer"; return false; }
  std::string s;
  int_to_base(a[0].as.i, 16, "0x", s);
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}
bool bin_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "bin expects an integer"; return false; }
  std::string s;
  int_to_base(a[0].as.i, 2, "0b", s);
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool is_function_fn(Value* a, uint32_t, Heap&, Value& out, std::string&) {
  bool f = a[0].tag == Tag::Obj && a[0].as.obj &&
           (a[0].as.obj->kind == ObjKind::Function || a[0].as.obj->kind == ObjKind::Closure);
  out = Value::boolean(f);
  return true;
}

// =======================================================================
// Batch 5 — statistics, special functions, and classic algorithms
//
// Same safepoint discipline throughout. Several reductions here work over a
// C++ `std::vector<double>` copied out of the array first, which keeps them
// allocation-free on the Charcoal heap (the source can never move under them).
// =======================================================================

// --- math: integer ---

bool is_even_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "is_even expects an integer"; return false; }
  out = Value::boolean((a[0].as.i & 1) == 0);
  return true;
}
bool is_odd_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int) { err = "is_odd expects an integer"; return false; }
  out = Value::boolean((a[0].as.i & 1) != 0);
  return true;
}

// Floored integer division: the quotient rounds toward negative infinity.
bool floor_div_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int || a[1].tag != Tag::Int) { err = "floor_div expects integers"; return false; }
  int64_t x = a[0].as.i, y = a[1].as.i;
  if (y == 0) { err = "floor_div by zero"; return false; }
  if (x == kI64Min && y == -1) { err = "floor_div overflow"; return false; }
  int64_t q = x / y;
  if ((x % y != 0) && ((x < 0) != (y < 0))) q -= 1;  // round toward -inf
  out = Value::integer(q);
  return true;
}

// Modular exponentiation (base^exp mod m) via square-and-multiply. The modulus
// is capped so every intermediate product stays inside uint64 without 128-bit
// arithmetic: (m-1)^2 < 2^64 requires m <= 3037000499.
bool pow_mod_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (a[0].tag != Tag::Int || a[1].tag != Tag::Int || a[2].tag != Tag::Int) {
    err = "pow_mod expects integers"; return false;
  }
  int64_t e = a[1].as.i, ms = a[2].as.i;
  if (e < 0) { err = "pow_mod exponent must be non-negative"; return false; }
  if (ms <= 0) { err = "pow_mod modulus must be positive"; return false; }
  if (ms > 3037000499LL) { err = "pow_mod modulus too large"; return false; }
  uint64_t m = static_cast<uint64_t>(ms);
  if (m == 1) { out = Value::integer(0); return true; }
  int64_t bb = a[0].as.i % ms;
  if (bb < 0) bb += ms;
  uint64_t base = static_cast<uint64_t>(bb), result = 1, ue = static_cast<uint64_t>(e);
  while (ue) {
    if (ue & 1u) result = (result * base) % m;
    base = (base * base) % m;
    ue >>= 1;
  }
  out = Value::integer(static_cast<int64_t>(result));
  return true;
}

// Round to `nd` decimal places (0..15), returning a double.
bool round_to_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "round_to expects a number"; return false; }
  if (a[1].tag != Tag::Int) { err = "round_to places must be an integer"; return false; }
  int64_t nd = a[1].as.i;
  if (nd < 0) nd = 0;
  if (nd > 15) nd = 15;
  double scale = std::pow(10.0, static_cast<double>(nd));
  out = Value::number(std::round(to_double(a[0]) * scale) / scale);
  return true;
}

// --- math: statistics over an array (allocation-free) ---

bool collect_nums(Value* a, const char* name, std::vector<double>& v, std::string& err) {
  if (!is_array(a[0])) { err = std::string(name) + " expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  v.reserve(arr->len);
  for (uint32_t i = 0; i < arr->len; ++i) {
    const Value& e = arr->slots->data[i];
    if (!is_num(e)) { err = std::string(name) + " expects numbers"; return false; }
    v.push_back(to_double(e));
  }
  return true;
}

bool variance_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  std::vector<double> v;
  if (!collect_nums(a, "variance", v, err)) return false;
  if (v.empty()) { err = "variance of empty array"; return false; }
  double mean = 0;
  for (double x : v) mean += x;
  mean /= static_cast<double>(v.size());
  double acc = 0;
  for (double x : v) acc += (x - mean) * (x - mean);
  out = Value::number(acc / static_cast<double>(v.size()));
  return true;
}
bool stddev_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  std::vector<double> v;
  if (!collect_nums(a, "stddev", v, err)) return false;
  if (v.empty()) { err = "stddev of empty array"; return false; }
  double mean = 0;
  for (double x : v) mean += x;
  mean /= static_cast<double>(v.size());
  double acc = 0;
  for (double x : v) acc += (x - mean) * (x - mean);
  out = Value::number(std::sqrt(acc / static_cast<double>(v.size())));
  return true;
}
bool median_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  std::vector<double> v;
  if (!collect_nums(a, "median", v, err)) return false;
  if (v.empty()) { err = "median of empty array"; return false; }
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  out = Value::number((n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0);
  return true;
}
// The most frequent value (the smallest such on a tie). O(n^2), no allocation.
bool mode_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  std::vector<double> v;
  if (!collect_nums(a, "mode", v, err)) return false;
  if (v.empty()) { err = "mode of empty array"; return false; }
  double best = v[0];
  size_t best_count = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    size_t c = 0;
    for (size_t j = 0; j < v.size(); ++j) if (v[j] == v[i]) ++c;
    if (c > best_count || (c == best_count && v[i] < best)) { best = v[i]; best_count = c; }
  }
  out = Value::number(best);
  return true;
}

// --- math: special functions (no allocation) ---

bool gamma_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "gamma expects a number"; return false; }
  out = Value::number(std::tgamma(to_double(a[0])));
  return true;
}
bool lgamma_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "lgamma expects a number"; return false; }
  out = Value::number(std::lgamma(to_double(a[0])));
  return true;
}
bool erf_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "erf expects a number"; return false; }
  out = Value::number(std::erf(to_double(a[0])));
  return true;
}
bool erfc_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "erfc expects a number"; return false; }
  out = Value::number(std::erfc(to_double(a[0])));
  return true;
}
bool fdim_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "fdim expects numbers"; return false; }
  out = Value::number(std::fdim(to_double(a[0]), to_double(a[1])));
  return true;
}
bool fma_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1]) || !is_num(a[2])) { err = "fma expects numbers"; return false; }
  out = Value::number(std::fma(to_double(a[0]), to_double(a[1]), to_double(a[2])));
  return true;
}
// frexp(x) -> [mantissa, exponent]; one allocation holding only scalars.
bool frexp_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "frexp expects a number"; return false; }
  int e = 0;
  double m = std::frexp(to_double(a[0]), &e);
  Value rv;
  if (!make_array(h, 2, rv, err)) return false;  // safepoint
  ArrayObj* arr = static_cast<ArrayObj*>(rv.as.obj);
  arr->slots->data[0] = Value::number(m);
  arr->slots->data[1] = Value::integer(e);
  out = rv;
  return true;
}
// modf(x) -> [integer_part, fractional_part]; both doubles.
bool modf_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_num(a[0])) { err = "modf expects a number"; return false; }
  double ip = 0;
  double f = std::modf(to_double(a[0]), &ip);
  Value rv;
  if (!make_array(h, 2, rv, err)) return false;  // safepoint
  ArrayObj* arr = static_cast<ArrayObj*>(rv.as.obj);
  arr->slots->data[0] = Value::number(ip);
  arr->slots->data[1] = Value::number(f);
  out = rv;
  return true;
}

// --- string ---

// partition(s, sep) -> [before, sep, after]; if `sep` is absent, [s, "", ""].
bool partition_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "partition expects strings"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sep(sbytes(a[1]), slen(a[1]));
  if (sep.empty()) { err = "partition separator must be non-empty"; return false; }
  std::string before, mid, after;
  size_t f = s.find(sep);
  if (f == std::string::npos) { before = s; }
  else { before = s.substr(0, f); mid = sep; after = s.substr(f + sep.size()); }
  std::string parts[3] = {before, mid, after};
  Value rv;
  if (!make_array(h, 3, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t ri = hs.root(rv.as.obj);
  for (uint32_t i = 0; i < 3; ++i) {
    StringObj* so = h.new_string(parts[i].data(), static_cast<uint32_t>(parts[i].size()));  // safepoint
    if (!so || h.over_cap()) { err = "out of memory"; return false; }
    hs.get<ArrayObj>(ri)->slots->data[i] = Value::object(so);  // re-read result
  }
  out = Value::object(hs.get<ArrayObj>(ri));
  return true;
}

bool index_from_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "index_from expects strings"; return false; }
  if (!is_num(a[2])) { err = "index_from start must be a number"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  std::string sub(sbytes(a[1]), slen(a[1]));
  int64_t start = as_i64(a[2]);
  if (start < 0) start = 0;
  if (start > static_cast<int64_t>(s.size())) { out = Value::integer(-1); return true; }
  size_t p = s.find(sub, static_cast<size_t>(start));
  out = Value::integer(p == std::string::npos ? -1 : static_cast<int64_t>(p));
  return true;
}

bool is_palindrome_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "is_palindrome expects a string"; return false; }
  const char* p = sbytes(a[0]);
  uint32_t n = slen(a[0]);
  for (uint32_t i = 0, j = n; i + 1 < j; ++i) { --j; if (p[i] != p[j]) { out = Value::boolean(false); return true; } }
  out = Value::boolean(true);
  return true;
}

bool common_prefix_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "common_prefix expects strings"; return false; }
  std::string x(sbytes(a[0]), slen(a[0]));
  std::string y(sbytes(a[1]), slen(a[1]));
  size_t n = 0, lim = x.size() < y.size() ? x.size() : y.size();
  while (n < lim && x[n] == y[n]) ++n;
  return make_string(h, x.data(), static_cast<uint32_t>(n), out, err);
}
bool common_suffix_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "common_suffix expects strings"; return false; }
  std::string x(sbytes(a[0]), slen(a[0]));
  std::string y(sbytes(a[1]), slen(a[1]));
  size_t n = 0, lim = x.size() < y.size() ? x.size() : y.size();
  while (n < lim && x[x.size() - 1 - n] == y[y.size() - 1 - n]) ++n;
  return make_string(h, x.data() + (x.size() - n), static_cast<uint32_t>(n), out, err);
}

// Levenshtein edit distance via a rolling two-row DP. Bytes are copied to C++
// locals up front, so there is no Charcoal-heap allocation at all.
bool levenshtein_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "levenshtein expects strings"; return false; }
  std::string x(sbytes(a[0]), slen(a[0]));
  std::string y(sbytes(a[1]), slen(a[1]));
  size_t n = x.size(), m = y.size();
  std::vector<int64_t> prev(m + 1), cur(m + 1);
  for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int64_t>(j);
  for (size_t i = 1; i <= n; ++i) {
    cur[0] = static_cast<int64_t>(i);
    for (size_t j = 1; j <= m; ++j) {
      int64_t cost = (x[i - 1] == y[j - 1]) ? 0 : 1;
      int64_t del = prev[j] + 1, ins = cur[j - 1] + 1, sub = prev[j - 1] + cost;
      int64_t best = del < ins ? del : ins;
      cur[j] = best < sub ? best : sub;
    }
    std::swap(prev, cur);
  }
  out = Value::integer(prev[m]);
  return true;
}

bool hamming_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0]) || !is_str(a[1])) { err = "hamming expects strings"; return false; }
  if (slen(a[0]) != slen(a[1])) { err = "hamming expects equal-length strings"; return false; }
  const char* p = sbytes(a[0]);
  const char* q = sbytes(a[1]);
  uint32_t n = slen(a[0]);
  int64_t c = 0;
  for (uint32_t i = 0; i < n; ++i) if (p[i] != q[i]) ++c;
  out = Value::integer(c);
  return true;
}

bool caesar_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "caesar expects a string"; return false; }
  if (a[1].tag != Tag::Int) { err = "caesar shift must be an integer"; return false; }
  std::string s(sbytes(a[0]), slen(a[0]));
  int shift = static_cast<int>(((a[1].as.i % 26) + 26) % 26);
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>('a' + (c - 'a' + shift) % 26);
    else if (c >= 'A' && c <= 'Z') c = static_cast<char>('A' + (c - 'A' + shift) % 26);
  }
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

// from_bytes: inverse of to_bytes. Values are validated and copied into a C++
// string before the single allocation, so no array pointer is held across it.
bool from_bytes_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "from_bytes expects an array"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  std::string s;
  s.reserve(arr->len);
  for (uint32_t i = 0; i < arr->len; ++i) {
    const Value& e = arr->slots->data[i];
    if (e.tag != Tag::Int || e.as.i < 0 || e.as.i > 255) { err = "from_bytes expects byte integers (0-255)"; return false; }
    s += static_cast<char>(e.as.i);
  }
  return make_string(h, s.data(), static_cast<uint32_t>(s.size()), out, err);
}

bool ascii_sum_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_str(a[0])) { err = "ascii_sum expects a string"; return false; }
  const char* p = sbytes(a[0]);
  uint32_t n = slen(a[0]);
  int64_t total = 0;
  for (uint32_t i = 0; i < n; ++i) total += static_cast<uint8_t>(p[i]);
  out = Value::integer(total);
  return true;
}

// --- array ---

// argsort: the indices that would sort the array ascending (stable). The order
// is computed against the source before allocating, then only ints are stored.
bool argsort_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "argsort expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  for (uint32_t i = 0; i < src->len; ++i)
    if (!is_num(src->slots->data[i])) { err = "argsort expects numbers"; return false; }
  std::vector<uint32_t> idx(src->len);
  for (uint32_t i = 0; i < src->len; ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&](uint32_t i, uint32_t j) {
    return to_double(src->slots->data[i]) < to_double(src->slots->data[j]);
  });
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(idx.size()), rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  for (size_t i = 0; i < idx.size(); ++i) r->slots->data[i] = Value::integer(idx[i]);
  out = rv;
  return true;
}

bool swap_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "swap expects an array"; return false; }
  if (!is_num(a[1]) || !is_num(a[2])) { err = "swap indices must be numbers"; return false; }
  ArrayObj* arr = as_arr(a[0]);
  int64_t i = as_i64(a[1]), j = as_i64(a[2]);
  if (i < 0 || j < 0 || i >= static_cast<int64_t>(arr->len) || j >= static_cast<int64_t>(arr->len)) {
    err = "swap index out of range"; return false;
  }
  std::swap(arr->slots->data[i], arr->slots->data[j]);
  out = Value::nil();
  return true;
}

// unzip: an array of [a, b] pairs -> [array_of_a, array_of_b].
bool unzip_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "unzip expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  uint32_t n = src->len;
  for (uint32_t i = 0; i < n; ++i)
    if (!is_array(src->slots->data[i]) || as_arr(src->slots->data[i])->len < 2) {
      err = "unzip expects an array of pairs"; return false;
    }
  HandleScope hs(h);
  Value outer;
  if (!make_array(h, 2, outer, err)) return false;  // safepoint
  size_t oi = hs.root(outer.as.obj);
  Value firsts;
  if (!make_array(h, n, firsts, err)) return false;  // safepoint
  hs.get<ArrayObj>(oi)->slots->data[0] = firsts;     // re-read outer
  Value seconds;
  if (!make_array(h, n, seconds, err)) return false;  // safepoint
  hs.get<ArrayObj>(oi)->slots->data[1] = seconds;     // re-read outer
  for (uint32_t i = 0; i < n; ++i) {
    ArrayObj* s = as_arr(a[0]);                  // re-read source
    ArrayObj* pair = as_arr(s->slots->data[i]);
    Value v0 = pair->slots->data[0];
    Value v1 = pair->slots->data[1];
    ArrayObj* o = hs.get<ArrayObj>(oi);
    as_arr(o->slots->data[0])->slots->data[i] = v0;  // no alloc in this loop body
    as_arr(o->slots->data[1])->slots->data[i] = v1;
  }
  out = Value::object(hs.get<ArrayObj>(oi));
  return true;
}

// diff: consecutive differences (length n-1; empty for n <= 1).
bool diff_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "diff expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  for (uint32_t i = 0; i < src->len; ++i)
    if (!is_num(src->slots->data[i])) { err = "diff expects numbers"; return false; }
  uint32_t n = src->len == 0 ? 0 : src->len - 1;
  Value rv;
  if (!make_array(h, n, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < n; ++i) {
    const Value& x = src->slots->data[i];
    const Value& y = src->slots->data[i + 1];
    if (x.tag == Tag::Double || y.tag == Tag::Double)
      r->slots->data[i] = Value::number(to_double(y) - to_double(x));
    else
      r->slots->data[i] = Value::integer(static_cast<int64_t>(static_cast<uint64_t>(as_i64(y)) - static_cast<uint64_t>(as_i64(x))));
  }
  out = rv;
  return true;
}

bool running_extreme(Value* a, bool want_max, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  for (uint32_t i = 0; i < src->len; ++i)
    if (!is_num(src->slots->data[i])) { err = "expects numbers"; return false; }
  uint32_t len = src->len;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  if (len == 0) { out = rv; return true; }
  Value best = src->slots->data[0];
  double bd = to_double(best);
  for (uint32_t i = 0; i < len; ++i) {
    double d = to_double(src->slots->data[i]);
    if ((want_max && d > bd) || (!want_max && d < bd)) { best = src->slots->data[i]; bd = d; }
    r->slots->data[i] = best;
  }
  out = rv;
  return true;
}
bool running_max_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) { return running_extreme(a, true,  h, out, err); }
bool running_min_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) { return running_extreme(a, false, h, out, err); }

// linspace(start, end, n): n evenly spaced doubles from start to end inclusive.
bool linspace_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "linspace expects numbers"; return false; }
  if (a[2].tag != Tag::Int) { err = "linspace count must be an integer"; return false; }
  int64_t n = a[2].as.i;
  if (n < 0) { err = "linspace count must be non-negative"; return false; }
  if (n > (1 << 20)) { err = "linspace count too large"; return false; }
  double start = to_double(a[0]), end = to_double(a[1]);
  Value rv;
  if (!make_array(h, static_cast<uint32_t>(n), rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  for (int64_t i = 0; i < n; ++i) {
    double t = (n == 1) ? 0.0 : static_cast<double>(i) / static_cast<double>(n - 1);
    r->slots->data[i] = Value::number(start + (end - start) * t);
  }
  out = rv;
  return true;
}

bool scale_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "scale expects an array"; return false; }
  if (!is_num(a[1])) { err = "scale factor must be a number"; return false; }
  ArrayObj* src = as_arr(a[0]);
  for (uint32_t i = 0; i < src->len; ++i)
    if (!is_num(src->slots->data[i])) { err = "scale expects numbers"; return false; }
  uint32_t len = src->len;
  double k = to_double(a[1]);
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) r->slots->data[i] = Value::number(to_double(src->slots->data[i]) * k);
  out = rv;
  return true;
}

bool normalize_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "normalize expects an array"; return false; }
  ArrayObj* src = as_arr(a[0]);
  double total = 0;
  for (uint32_t i = 0; i < src->len; ++i) {
    if (!is_num(src->slots->data[i])) { err = "normalize expects numbers"; return false; }
    total += to_double(src->slots->data[i]);
  }
  if (total == 0.0) { err = "normalize of a zero-sum array"; return false; }
  uint32_t len = src->len;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  for (uint32_t i = 0; i < len; ++i) r->slots->data[i] = Value::number(to_double(src->slots->data[i]) / total);
  out = rv;
  return true;
}

// clamp_all: clamp every element into [lo, hi], keeping each element's own
// representation (the element, or the bound it is pinned to).
bool clamp_all_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "clamp_all expects an array"; return false; }
  if (!is_num(a[1]) || !is_num(a[2])) { err = "clamp_all bounds must be numbers"; return false; }
  if (to_double(a[1]) > to_double(a[2])) { err = "clamp_all: lo > hi"; return false; }
  ArrayObj* src = as_arr(a[0]);
  for (uint32_t i = 0; i < src->len; ++i)
    if (!is_num(src->slots->data[i])) { err = "clamp_all expects numbers"; return false; }
  uint32_t len = src->len;
  Value rv;
  if (!make_array(h, len, rv, err)) return false;  // safepoint
  ArrayObj* r = static_cast<ArrayObj*>(rv.as.obj);
  src = as_arr(a[0]);  // re-read after the alloc
  Value lo = a[1], hi = a[2];  // re-read bounds from registers after the alloc
  double lod = to_double(lo), hid = to_double(hi);
  for (uint32_t i = 0; i < len; ++i) {
    const Value& e = src->slots->data[i];
    double d = to_double(e);
    r->slots->data[i] = (d < lod) ? lo : (d > hid) ? hi : e;
  }
  out = rv;
  return true;
}

// --- map ---

// from_keys(keys, value): a map mapping each (string) key to the same value.
bool from_keys_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_array(a[0])) { err = "from_keys expects a key array"; return false; }
  ArrayObj* ks0 = as_arr(a[0]);
  for (uint32_t i = 0; i < ks0->len; ++i)
    if (!is_str(ks0->slots->data[i])) { err = "from_keys keys must be strings"; return false; }
  Value rv;
  if (!make_map(h, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t mi = hs.root(rv.as.obj);
  uint32_t n = as_arr(a[0])->len;
  for (uint32_t i = 0; i < n; ++i) {
    ArrayObj* ks = as_arr(a[0]);  // re-read key array
    StringObj* kobj = static_cast<StringObj*>(ks->slots->data[i].as.obj);
    std::string key(kobj->bytes->data, kobj->len);
    Value val = a[1];             // re-read value register
    MapObj* nm = hs.get<MapObj>(mi);
    map_put(nm, key.data(), static_cast<uint32_t>(key.size()), val, h);  // allocates
    if (h.over_cap()) { err = "out of memory"; return false; }
  }
  out = Value::object(hs.get<MapObj>(mi));
  return true;
}

// key_of(m, value): the first key whose value equals `value`, or nil.
bool key_of_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_map(a[0])) { err = "key_of expects a map"; return false; }
  MapObj* m = as_map(a[0]);
  for (uint32_t i = 0; i < m->len; ++i)
    if (val_equal(m->vals->data[i], a[1])) { out = m->keys->data[i]; return true; }
  out = Value::nil();
  return true;
}

// defaults(m, fallback): m's entries win; fallback fills in any missing keys.
bool defaults_fn(Value* a, uint32_t, Heap& h, Value& out, std::string& err) {
  if (!is_map(a[0]) || !is_map(a[1])) { err = "defaults expects two maps"; return false; }
  Value rv;
  if (!make_map(h, rv, err)) return false;  // safepoint
  HandleScope hs(h);
  size_t mi = hs.root(rv.as.obj);
  // Insert the fallback first, then the primary, so the primary overrides.
  for (int src = 1; src >= 0; --src) {
    uint32_t len = as_map(a[src])->len;
    for (uint32_t i = 0; i < len; ++i) {
      MapObj* sm = as_map(a[src]);  // re-read source
      StringObj* ks = static_cast<StringObj*>(sm->keys->data[i].as.obj);
      std::string key(ks->bytes->data, ks->len);
      Value val = sm->vals->data[i];
      MapObj* nm = hs.get<MapObj>(mi);
      map_put(nm, key.data(), static_cast<uint32_t>(key.size()), val, h);  // allocates
      if (h.over_cap()) { err = "out of memory"; return false; }
    }
  }
  out = Value::object(hs.get<MapObj>(mi));
  return true;
}

// =======================================================================
// Registry
// =======================================================================

const NativeEntry kNatives[] = {
    // type / convert
    {"type", 1, 1, type_fn},          {"to_string", 1, 1, to_string_fn},
    {"to_int", 1, 1, to_int_fn},       {"to_float", 1, 1, to_float_fn},
    {"is_nil", 1, 1, is_nil_fn},       {"is_num", 1, 1, is_num_fn},
    {"is_str", 1, 1, is_str_fn},       {"is_array", 1, 1, is_array_fn},
    {"is_map", 1, 1, is_map_fn},
    // math
    {"abs", 1, 1, abs_fn},             {"min", 2, 2, min_fn},
    {"max", 2, 2, max_fn},             {"floor", 1, 1, floor_fn},
    {"ceil", 1, 1, ceil_fn},           {"round", 1, 1, round_fn},
    {"sqrt", 1, 1, sqrt_fn},           {"pow", 2, 2, pow_fn},
    {"sign", 1, 1, sign_fn},
    // string
    {"len", 1, 1, len_fn},             {"substr", 3, 3, substr_fn},
    {"char_at", 2, 2, char_at_fn},     {"index_of", 2, 2, index_of_fn},
    {"contains", 2, 2, contains_fn},   {"starts_with", 2, 2, starts_with_fn},
    {"ends_with", 2, 2, ends_with_fn}, {"upper", 1, 1, upper_fn},
    {"lower", 1, 1, lower_fn},         {"repeat", 2, 2, repeat_fn},
    {"str_concat", 2, 2, str_concat_fn}, {"split", 2, 2, split_fn},
    {"join", 2, 2, join_fn},           {"trim", 1, 1, trim_fn},
    {"ord", 1, 1, ord_fn},             {"chr", 1, 1, chr_fn},
    // array
    {"pop", 1, 1, pop_fn},             {"insert", 3, 3, insert_fn},
    {"remove", 2, 2, remove_fn},       {"slice", 3, 3, slice_fn},
    {"reverse", 1, 1, reverse_fn},     {"sort", 1, 1, sort_fn},
    {"arr_index_of", 2, 2, arr_index_of_fn},
    // map
    {"keys", 1, 1, keys_fn},           {"values", 1, 1, values_fn},
    {"has", 2, 2, has_fn},             {"remove_key", 2, 2, remove_key_fn},

    // --- batch 2 ---
    // predicates
    {"is_int", 1, 1, is_int_fn},       {"is_bool", 1, 1, is_bool_fn},
    // math
    {"clamp", 3, 3, clamp_fn},         {"gcd", 2, 2, gcd_fn},
    {"lcm", 2, 2, lcm_fn},             {"exp", 1, 1, exp_fn},
    {"log", 1, 1, log_fn},             {"log2", 1, 1, log2_fn},
    {"log10", 1, 1, log10_fn},         {"sin", 1, 1, sin_fn},
    {"cos", 1, 1, cos_fn},             {"tan", 1, 1, tan_fn},
    {"atan2", 2, 2, atan2_fn},         {"hypot", 2, 2, hypot_fn},
    {"trunc", 1, 1, trunc_fn},
    // string
    {"replace", 3, 3, replace_fn},     {"last_index_of", 2, 2, last_index_of_fn},
    {"pad_left", 3, 3, pad_left_fn},   {"pad_right", 3, 3, pad_right_fn},
    {"count_sub", 2, 2, count_sub_fn}, {"capitalize", 1, 1, capitalize_fn},
    {"reverse_str", 1, 1, reverse_str_fn}, {"lines", 1, 1, lines_fn},
    {"is_empty", 1, 1, is_empty_fn},
    // array
    {"range", 2, 3, range_fn},         {"fill", 2, 2, fill_fn},
    {"concat_arr", 2, 2, concat_arr_fn}, {"sum", 1, 1, sum_fn},
    {"product", 1, 1, product_fn},     {"min_of", 1, 1, min_of_fn},
    {"max_of", 1, 1, max_of_fn},       {"first", 1, 1, first_fn},
    {"last", 1, 1, last_fn},           {"take", 2, 2, take_fn},
    {"drop", 2, 2, drop_fn},           {"count_of", 2, 2, count_of_fn},
    // map
    {"get", 2, 2, get_fn},             {"set", 3, 3, set_fn},
    {"merge", 2, 2, merge_fn},         {"entries", 1, 1, entries_fn},

    // --- batch 3 ---
    // math
    {"factorial", 1, 1, factorial_fn}, {"is_prime", 1, 1, is_prime_fn},
    {"fib", 1, 1, fib_fn},             {"cbrt", 1, 1, cbrt_fn},
    {"deg2rad", 1, 1, deg2rad_fn},     {"rad2deg", 1, 1, rad2deg_fn},
    {"log_base", 2, 2, log_base_fn},   {"mean", 1, 1, mean_fn},
    // array reductions / predicates
    {"any", 1, 1, any_fn},             {"all", 1, 1, all_fn},
    {"includes", 2, 2, includes_fn},   {"index_min", 1, 1, index_min_fn},
    {"index_max", 1, 1, index_max_fn},
    // array transforms
    {"zip", 2, 2, zip_fn},             {"enumerate", 1, 1, enumerate_fn},
    {"flatten", 1, 1, flatten_fn},     {"unique", 1, 1, unique_fn},
    {"rotate", 2, 2, rotate_fn},       {"chunk", 2, 2, chunk_fn},
    // string predicates
    {"is_digit", 1, 1, is_digit_str_fn}, {"is_alpha", 1, 1, is_alpha_fn},
    {"is_alnum", 1, 1, is_alnum_fn},   {"is_space", 1, 1, is_space_fn},
    // string transforms
    {"swapcase", 1, 1, swapcase_fn},   {"title_case", 1, 1, title_case_fn},
    {"zfill", 2, 2, zfill_fn},         {"center", 3, 3, center_fn},
    {"chars", 1, 1, chars_fn},         {"to_bytes", 1, 1, to_bytes_fn},
    {"format", 1, 255, format_fn},
    // map transforms
    {"invert", 1, 1, invert_fn},       {"pick", 2, 2, pick_fn},

    // --- batch 4 ---
    // math: inverse trig / hyperbolic
    {"asin", 1, 1, asin_fn},           {"acos", 1, 1, acos_fn},
    {"atan", 1, 1, atan_fn},           {"sinh", 1, 1, sinh_fn},
    {"cosh", 1, 1, cosh_fn},           {"tanh", 1, 1, tanh_fn},
    {"asinh", 1, 1, asinh_fn},         {"acosh", 1, 1, acosh_fn},
    {"atanh", 1, 1, atanh_fn},         {"expm1", 1, 1, expm1_fn},
    {"log1p", 1, 1, log1p_fn},
    // math: misc
    {"fmod", 2, 2, fmod_fn},           {"copysign", 2, 2, copysign_fn},
    {"ldexp", 2, 2, ldexp_fn},         {"is_nan", 1, 1, is_nan_fn},
    {"is_inf", 1, 1, is_inf_fn},       {"is_finite", 1, 1, is_finite_fn},
    {"lerp", 3, 3, lerp_fn},           {"smoothstep", 3, 3, smoothstep_fn},
    {"isqrt", 1, 1, isqrt_fn},         {"mod_floor", 2, 2, mod_floor_fn},
    {"bit_count", 1, 1, bit_count_fn}, {"next_pow2", 1, 1, next_pow2_fn},
    {"divmod", 2, 2, divmod_fn},
    // string predicates / transforms
    {"is_upper", 1, 1, is_upper_fn},   {"is_lower", 1, 1, is_lower_fn},
    {"strip_prefix", 2, 2, strip_prefix_fn}, {"strip_suffix", 2, 2, strip_suffix_fn},
    {"left", 2, 2, left_fn},           {"right", 2, 2, right_fn},
    {"count_char", 2, 2, count_char_fn}, {"find_all", 2, 2, find_all_fn},
    {"replace_first", 3, 3, replace_first_fn}, {"rot13", 1, 1, rot13_fn},
    // array transforms / reductions
    {"compact", 1, 1, compact_fn},     {"cumsum", 1, 1, cumsum_fn},
    {"dot", 2, 2, dot_fn},             {"intersperse", 2, 2, intersperse_fn},
    {"reverse_copy", 1, 1, reverse_copy_fn}, {"sorted", 1, 1, sorted_fn},
    {"is_sorted", 1, 1, is_sorted_fn}, {"tail", 1, 1, tail_fn},
    {"init", 1, 1, init_fn},
    // map utilities
    {"get_or", 3, 3, get_or_fn},       {"has_value", 2, 2, has_value_fn},
    {"omit", 2, 2, omit_fn},
    // conversion / predicate
    {"hex", 1, 1, hex_fn},             {"bin", 1, 1, bin_fn},
    {"is_function", 1, 1, is_function_fn},

    // --- batch 5 ---
    // math: integer / rounding
    {"is_even", 1, 1, is_even_fn},     {"is_odd", 1, 1, is_odd_fn},
    {"floor_div", 2, 2, floor_div_fn}, {"pow_mod", 3, 3, pow_mod_fn},
    {"round_to", 2, 2, round_to_fn},
    // math: statistics
    {"variance", 1, 1, variance_fn},   {"stddev", 1, 1, stddev_fn},
    {"median", 1, 1, median_fn},       {"mode", 1, 1, mode_fn},
    // math: special functions
    {"gamma", 1, 1, gamma_fn},         {"lgamma", 1, 1, lgamma_fn},
    {"erf", 1, 1, erf_fn},             {"erfc", 1, 1, erfc_fn},
    {"fdim", 2, 2, fdim_fn},           {"fma", 3, 3, fma_fn},
    {"frexp", 1, 1, frexp_fn},         {"modf", 1, 1, modf_fn},
    // string
    {"partition", 2, 2, partition_fn}, {"index_from", 3, 3, index_from_fn},
    {"is_palindrome", 1, 1, is_palindrome_fn},
    {"common_prefix", 2, 2, common_prefix_fn}, {"common_suffix", 2, 2, common_suffix_fn},
    {"levenshtein", 2, 2, levenshtein_fn}, {"hamming", 2, 2, hamming_fn},
    {"caesar", 2, 2, caesar_fn},       {"from_bytes", 1, 1, from_bytes_fn},
    {"ascii_sum", 1, 1, ascii_sum_fn},
    // array
    {"argsort", 1, 1, argsort_fn},     {"swap", 3, 3, swap_fn},
    {"unzip", 1, 1, unzip_fn},         {"diff", 1, 1, diff_fn},
    {"running_max", 1, 1, running_max_fn}, {"running_min", 1, 1, running_min_fn},
    {"linspace", 3, 3, linspace_fn},   {"scale", 2, 2, scale_fn},
    {"normalize", 1, 1, normalize_fn}, {"clamp_all", 3, 3, clamp_all_fn},
    // map
    {"from_keys", 2, 2, from_keys_fn}, {"key_of", 2, 2, key_of_fn},
    {"defaults", 2, 2, defaults_fn},
};

constexpr size_t kNativeCount = sizeof(kNatives) / sizeof(kNatives[0]);

}  // namespace

const NativeEntry* find_native(const char* name, uint32_t len) {
  for (size_t i = 0; i < kNativeCount; ++i) {
    const char* nm = kNatives[i].name;
    if (std::strlen(nm) == len && std::memcmp(nm, name, len) == 0) return &kNatives[i];
  }
  return nullptr;
}

const NativeEntry* find_native(const std::string& name) {
  return find_native(name.data(), static_cast<uint32_t>(name.size()));
}

}  // namespace coal
