#include "interp.h"

#include "object.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace coal {

namespace {

struct Frame {
  uint32_t           func_idx;
  size_t             pc = 0;
  std::vector<Value> regs;
  int                result_reg = 0;  // register in the caller to receive RET
};

// Well-defined wrapping integer arithmetic (signed overflow is UB; do the math
// in uint64_t and cast back so UBSan stays quiet — M1 semantics are wrapping).
int64_t wadd(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)); }
int64_t wsub(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b)); }
int64_t wmul(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b)); }

double as_double(const Value& v) {
  return v.tag == Tag::Double ? v.as.d : static_cast<double>(v.as.i);
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
        case ObjKind::String: {
          StringObj* s = static_cast<StringObj*>(v.as.obj);
          return std::string(s->data, s->len);
        }
        case ObjKind::Array:    return "[array]";
        case ObjKind::Map:      return "[object]";
        case ObjKind::Function: return "[fn]";
      }
  }
  return "nil";
}

// Find a key in a map; returns its slot index or -1.
int map_find(MapObj* m, const std::string& key) {
  for (uint32_t j = 0; j < m->len; ++j) {
    if (m->keylens[j] == key.size() &&
        std::memcmp(m->keys[j], key.data(), key.size()) == 0) {
      return static_cast<int>(j);
    }
  }
  return -1;
}

// Grow the three parallel arrays and append a malloc'd key copy + value. Stays
// consistent with Heap's dtor, which frees keys[i] for i<len plus the arrays.
void map_set(MapObj* m, const std::string& key, const Value& val) {
  int j = map_find(m, key);
  if (j >= 0) { m->vals[j] = val; return; }

  if (m->len == m->cap) {
    uint32_t newcap = m->cap ? m->cap * 2 : 4;
    m->keys    = static_cast<char**>(std::realloc(m->keys, sizeof(char*) * newcap));
    m->keylens = static_cast<uint32_t*>(std::realloc(m->keylens, sizeof(uint32_t) * newcap));
    m->vals    = static_cast<Value*>(std::realloc(m->vals, sizeof(Value) * newcap));
    m->cap = newcap;
  }
  char* kc = static_cast<char*>(std::malloc(key.size() ? key.size() : 1));
  std::memcpy(kc, key.data(), key.size());
  m->keys[m->len] = kc;
  m->keylens[m->len] = static_cast<uint32_t>(key.size());
  m->vals[m->len] = val;
  ++m->len;
}

}  // namespace

RunResult run(const Module& m, Heap& h, Limits limits) {
  RunResult res;

  // Resolve callee names once (verifier/loader guarantee name_const is a Str).
  std::unordered_map<std::string, uint32_t> fn_by_name;
  for (size_t i = 0; i < m.funcs.size(); ++i) {
    uint32_t nc = m.funcs[i].name_const;
    if (nc < m.consts.size() && m.consts[nc].tag == CTag::Str)
      fn_by_name[m.consts[nc].s] = static_cast<uint32_t>(i);
  }

  if (m.entry_func >= m.funcs.size()) { res.error = "entry function out of range"; return res; }

  std::vector<Frame> frames;
  auto push_frame = [&](uint32_t idx, int result_reg) {
    Frame f;
    f.func_idx = idx;
    f.result_reg = result_reg;
    f.regs.assign(m.funcs[idx].num_regs, Value::nil());
    frames.push_back(std::move(f));
  };
  push_frame(m.entry_func, 0);

  uint64_t insns = 0;

  // Pop the current frame, delivering `v` to the caller (or finishing the run).
  auto do_return = [&](const Value& v) {
    int rr = frames.back().result_reg;
    frames.pop_back();
    if (!frames.empty()) frames.back().regs[rr] = v;
  };

  while (!frames.empty()) {
    if (++insns > limits.max_insns) { res.error = "instruction budget exceeded"; break; }

    Frame& fr = frames.back();
    const Function& f = m.funcs[fr.func_idx];
    const std::vector<uint8_t>& code = f.code;

    // No terminator is required by the verifier: running off the end (or an
    // empty function) is an implicit `return nil`, never an OOB read.
    if (fr.pc >= code.size()) { do_return(Value::nil()); continue; }

    size_t pc = fr.pc;
    uint8_t op = code[pc];
    auto u8at  = [&](size_t k) { return code[pc + k]; };
    auto u16at = [&](size_t k) { return static_cast<uint16_t>(code[pc + k] | (code[pc + k + 1] << 8)); };
    auto i16at = [&](size_t k) { return static_cast<int16_t>(u16at(k)); };

    switch (op) {
      case OP_HALT:
        frames.clear();
        break;

      case OP_LOAD_CONST: {
        int r = u8at(1);
        uint16_t k = u16at(2);
        const Constant& c = m.consts[k];
        switch (c.tag) {
          case CTag::Int:    fr.regs[r] = Value::integer(c.i); break;
          case CTag::Double: fr.regs[r] = Value::number(c.d); break;
          case CTag::Bool:   fr.regs[r] = Value::boolean(c.b); break;
          case CTag::Nil:    fr.regs[r] = Value::nil(); break;
          case CTag::Str: {
            StringObj* s = h.new_string(c.s.data(), static_cast<uint32_t>(c.s.size()));
            if (h.over_cap()) { res.error = "out of memory"; break; }
            fr.regs[r] = Value::object(s);
            break;
          }
        }
        fr.pc += 4;
        break;
      }

      case OP_LOAD_NIL: {
        fr.regs[u8at(1)] = Value::nil();
        fr.pc += 2;
        break;
      }

      case OP_MOVE: {
        fr.regs[u8at(1)] = fr.regs[u8at(2)];
        fr.pc += 3;
        break;
      }

      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV: {
        int dst = u8at(1), a = u8at(2), b = u8at(3);
        const Value& va = fr.regs[a];
        const Value& vb = fr.regs[b];
        if (!va.is_number() || !vb.is_number()) {
          res.error = "type error: arithmetic on non-number";
          break;
        }
        if (va.tag == Tag::Int && vb.tag == Tag::Int) {
          int64_t x = va.as.i, y = vb.as.i, r;
          switch (op) {
            case OP_ADD: r = wadd(x, y); break;
            case OP_SUB: r = wsub(x, y); break;
            case OP_MUL: r = wmul(x, y); break;
            default:  // OP_DIV
              if (y == 0) { res.error = "division by zero"; }
              else if (x == std::numeric_limits<int64_t>::min() && y == -1) { res.error = "integer overflow in division"; }
              if (!res.error.empty()) { r = 0; break; }
              r = x / y;
              break;
          }
          if (!res.error.empty()) break;
          fr.regs[dst] = Value::integer(r);
        } else {
          double x = as_double(va), y = as_double(vb), r;
          switch (op) {
            case OP_ADD: r = x + y; break;
            case OP_SUB: r = x - y; break;
            case OP_MUL: r = x * y; break;
            default:     r = x / y; break;  // x/0.0 -> inf/nan, allowed
          }
          fr.regs[dst] = Value::number(r);
        }
        fr.pc += 4;
        break;
      }

      case OP_NEW_ARRAY: {
        int r = u8at(1), n = u8at(2);
        ArrayObj* a = h.new_array(static_cast<uint32_t>(n));
        if (h.over_cap()) { res.error = "out of memory"; break; }
        fr.regs[r] = Value::object(a);
        fr.pc += 3;
        break;
      }

      case OP_ARRAY_GET: {
        int dst = u8at(1), ar = u8at(2), ir = u8at(3);
        const Value& va = fr.regs[ar];
        const Value& vi = fr.regs[ir];
        if (va.tag != Tag::Obj || !va.as.obj || va.as.obj->kind != ObjKind::Array) {
          res.error = "type error: not an array"; break;
        }
        if (vi.tag != Tag::Int) { res.error = "type error: index not an integer"; break; }
        ArrayObj* arr = static_cast<ArrayObj*>(va.as.obj);
        int64_t idx = vi.as.i;
        if (idx < 0 || idx >= static_cast<int64_t>(arr->len)) { res.error = "index out of range"; break; }
        fr.regs[dst] = arr->items[idx];
        fr.pc += 4;
        break;
      }

      case OP_ARRAY_SET: {
        int ar = u8at(1), ir = u8at(2), vr = u8at(3);
        const Value& va = fr.regs[ar];
        const Value& vi = fr.regs[ir];
        if (va.tag != Tag::Obj || !va.as.obj || va.as.obj->kind != ObjKind::Array) {
          res.error = "type error: not an array"; break;
        }
        if (vi.tag != Tag::Int) { res.error = "type error: index not an integer"; break; }
        ArrayObj* arr = static_cast<ArrayObj*>(va.as.obj);
        int64_t idx = vi.as.i;
        if (idx < 0 || idx >= static_cast<int64_t>(arr->len)) { res.error = "index out of range"; break; }
        arr->items[idx] = fr.regs[vr];
        fr.pc += 4;
        break;
      }

      case OP_NEW_OBJECT: {
        int r = u8at(1);
        MapObj* mp = h.new_map();
        if (h.over_cap()) { res.error = "out of memory"; break; }
        fr.regs[r] = Value::object(mp);
        fr.pc += 2;
        break;
      }

      case OP_GET_PROP: {
        int dst = u8at(1), o = u8at(2);
        uint16_t k = u16at(3);
        const Value& vo = fr.regs[o];
        if (vo.tag != Tag::Obj || !vo.as.obj || vo.as.obj->kind != ObjKind::Map) {
          res.error = "type error: not an object"; break;
        }
        MapObj* mp = static_cast<MapObj*>(vo.as.obj);
        int j = map_find(mp, m.consts[k].s);
        fr.regs[dst] = (j >= 0) ? mp->vals[j] : Value::nil();
        fr.pc += 5;
        break;
      }

      case OP_SET_PROP: {
        int o = u8at(1);
        uint16_t k = u16at(2);
        int vr = u8at(4);
        const Value& vo = fr.regs[o];
        if (vo.tag != Tag::Obj || !vo.as.obj || vo.as.obj->kind != ObjKind::Map) {
          res.error = "type error: not an object"; break;
        }
        MapObj* mp = static_cast<MapObj*>(vo.as.obj);
        map_set(mp, m.consts[k].s, fr.regs[vr]);
        fr.pc += 5;
        break;
      }

      case OP_CALL: {
        int base = u8at(1);
        uint16_t k = u16at(2);
        int n = u8at(4);
        auto it = fn_by_name.find(m.consts[k].s);
        if (it == fn_by_name.end()) { res.error = "call to undefined function"; break; }
        uint32_t callee = it->second;
        if (frames.size() >= limits.max_depth) { res.error = "call depth exceeded"; break; }

        uint8_t np = m.funcs[callee].num_params;
        size_t caller = frames.size() - 1;
        Frame cf;
        cf.func_idx = callee;
        cf.result_reg = base;
        cf.regs.assign(m.funcs[callee].num_regs, Value::nil());
        for (int i = 0; i < np; ++i)
          if (i < n) cf.regs[i] = frames[caller].regs[base + i];

        frames[caller].pc += 5;  // resume past the CALL when the callee returns
        frames.push_back(std::move(cf));
        continue;  // re-grab the new top frame
      }

      case OP_RET: {
        Value v = fr.regs[u8at(1)];
        do_return(v);
        continue;
      }

      case OP_JUMP: {
        int16_t off = i16at(1);
        fr.pc = (pc + 3) + off;
        break;
      }

      case OP_JUMP_IF_FALSE: {
        int r = u8at(1);
        int16_t off = i16at(2);
        if (!fr.regs[r].truthy()) fr.pc = (pc + 4) + off;
        else                      fr.pc += 4;
        break;
      }

      case OP_PRINT: {
        res.output += render(fr.regs[u8at(1)]);
        fr.pc += 2;
        break;
      }

      default:
        res.error = "invalid opcode";
        break;
    }

    if (!res.error.empty()) break;
  }

  res.ok = res.error.empty();
  return res;
}

}  // namespace coal
