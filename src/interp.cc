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
          return std::string(s->bytes->data, s->len);
        }
        case ObjKind::Array:    return "[array]";
        case ObjKind::Map:      return "[object]";
        case ObjKind::Function: return "[fn]";
        case ObjKind::Bytes:
        case ObjKind::Slots:    return "[internal]";  // never user-visible
      }
  }
  return "nil";
}

// Find a key in a map; returns its slot index or -1. No allocation, so it never
// moves anything — safe to hold `m` across it.
int map_find(MapObj* m, const char* key, uint32_t klen) {
  for (uint32_t j = 0; j < m->len; ++j) {
    StringObj* ks = static_cast<StringObj*>(m->keys->data[j].as.obj);
    if (ks->len == klen && std::memcmp(ks->bytes->data, key, klen) == 0) {
      return static_cast<int>(j);
    }
  }
  return -1;
}

// Insert or overwrite key->val. Every new_* call here is a safepoint that can
// move `m`, its key/val Slots, and `val`'s object, so all of them are rooted and
// re-read after each allocation — never reuse a pointer from before a safepoint.
void map_set(MapObj* m, const char* key, uint32_t klen, Value val, Heap& h) {
  int j = map_find(m, key, klen);
  if (j >= 0) { m->vals->data[j] = val; return; }  // overwrite: no allocation

  HandleScope hs(h);
  size_t mi = hs.root(m);
  bool val_obj = (val.tag == Tag::Obj && val.as.obj);
  size_t vi = val_obj ? hs.root(val.as.obj) : 0;

  if (m->len == m->cap) {
    uint32_t newcap = m->cap ? m->cap * 2 : 4;
    SlotsObj* nk = h.new_slots(newcap);          // safepoint
    if (!nk) return;
    size_t ki = hs.root(nk);
    SlotsObj* nv = h.new_slots(newcap);          // safepoint; m, nk re-rooted
    if (!nv) return;
    size_t nvi = hs.root(nv);

    m = hs.get<MapObj>(mi);                       // re-read after the safepoints
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

  StringObj* ks = h.new_string(key, klen);        // safepoint
  if (!ks) return;
  m = hs.get<MapObj>(mi);                          // re-read m and val
  if (val_obj) val.as.obj = hs.get<Object>(vi);

  uint32_t i = m->len;
  m->keys->data[i] = Value::object(ks);
  m->vals->data[i] = val;
  m->len = i + 1;
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

  // The register files ARE the GC roots. Capturing `frames` by reference means
  // every collection (which can only happen inside a new_* call below) sees the
  // live stack and forwards every object register in place.
  h.set_root_enumerator([&frames](GcVisitor& v) {
    for (Frame& f : frames)
      for (Value& r : f.regs)
        v.visit(r);
  });

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
        fr.regs[dst] = arr->slots->data[idx];
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
        arr->slots->data[idx] = fr.regs[vr];
        fr.pc += 4;
        break;
      }

      case OP_ARRAY_PUSH: {
        int ra = u8at(1), rv = u8at(2);
        Value av = fr.regs[ra];
        if (av.tag != Tag::Obj || !av.as.obj || av.as.obj->kind != ObjKind::Array) {
          res.error = "type error: push on non-array"; break;
        }
        ArrayObj* arr = static_cast<ArrayObj*>(av.as.obj);

        if (arr->len == arr->cap) {
          uint32_t newcap = arr->cap ? arr->cap * 2 : 4;
          SlotsObj* ns = h.new_slots(newcap);   // SAFEPOINT: may move arr & its slots
          if (h.over_cap()) { res.error = "out of memory"; break; }
          // Rule 1: re-read arr from the register (a GC root) after the safepoint.
          arr = static_cast<ArrayObj*>(fr.regs[ra].as.obj);
          for (uint32_t i = 0; i < arr->len; ++i) ns->data[i] = arr->slots->data[i];
          arr->slots = ns;
          arr->cap = newcap;
        }
        // Rule 2: read the stored value from the register AFTER any allocation.
        arr->slots->data[arr->len] = fr.regs[rv];
        arr->len++;
        fr.pc += 3;
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
        const std::string& key = m.consts[k].s;
        int j = map_find(mp, key.data(), static_cast<uint32_t>(key.size()));
        fr.regs[dst] = (j >= 0) ? mp->vals->data[j] : Value::nil();
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
        const std::string& key = m.consts[k].s;
        map_set(mp, key.data(), static_cast<uint32_t>(key.size()), fr.regs[vr], h);
        if (h.over_cap()) { res.error = "out of memory"; break; }
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
