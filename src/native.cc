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

bool min_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "min expects numbers"; return false; }
  if (a[0].tag == Tag::Int && a[1].tag == Tag::Int)
    out = Value::integer(std::min(a[0].as.i, a[1].as.i));
  else
    out = Value::number(std::min(to_double(a[0]), to_double(a[1])));
  return true;
}

bool max_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1])) { err = "max expects numbers"; return false; }
  if (a[0].tag == Tag::Int && a[1].tag == Tag::Int)
    out = Value::integer(std::max(a[0].as.i, a[1].as.i));
  else
    out = Value::number(std::max(to_double(a[0]), to_double(a[1])));
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

bool clamp_fn(Value* a, uint32_t, Heap&, Value& out, std::string& err) {
  if (!is_num(a[0]) || !is_num(a[1]) || !is_num(a[2])) { err = "clamp expects numbers"; return false; }
  if (to_double(a[1]) > to_double(a[2])) { err = "clamp: lo > hi"; return false; }
  if (a[0].tag == Tag::Int && a[1].tag == Tag::Int && a[2].tag == Tag::Int) {
    int64_t x = a[0].as.i, lo = a[1].as.i, hi = a[2].as.i;
    out = Value::integer(x < lo ? lo : (x > hi ? hi : x));
  } else {
    double x = to_double(a[0]), lo = to_double(a[1]), hi = to_double(a[2]);
    out = Value::number(x < lo ? lo : (x > hi ? hi : x));
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
