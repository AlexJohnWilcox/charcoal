#include "loader.h"

#include <cstring>

namespace coal {

namespace {

// Cursor reader: the ONLY way to advance `i` is through a need()-guarded read,
// so `i` can never exceed `n` and indexing is never out of bounds no matter how
// hostile the input. Every read returns 0 once `bad` is set.
struct Reader {
  const uint8_t* p;
  size_t n;
  size_t i = 0;
  bool bad = false;

  bool need(size_t k) {
    if (i + k > n) { bad = true; return false; }
    return true;
  }

  uint8_t u8() {
    if (!need(1)) return 0;
    return p[i++];
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(p[i] | (p[i + 1] << 8));
    i += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int b = 0; b < 4; ++b) v |= static_cast<uint32_t>(p[i + b]) << (8 * b);
    i += 4;
    return v;
  }
  int64_t i64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b) v |= static_cast<uint64_t>(p[i + b]) << (8 * b);
    i += 8;
    return static_cast<int64_t>(v);
  }
  double f64() {
    uint64_t bits = static_cast<uint64_t>(i64());
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }
};

LoadResult err(const char* msg) {
  LoadResult r;
  r.error = msg;
  r.ok = false;
  return r;
}

}  // namespace

LoadResult load_cbc(const uint8_t* data, size_t size) {
  Reader r{data, size};
  LoadResult out;
  Module& m = out.module;

  // 1. Header.
  uint8_t m0 = r.u8(), m1 = r.u8(), m2 = r.u8(), m3 = r.u8();
  if (r.bad) return err("truncated header");
  if (m0 != 'C' || m1 != 'B' || m2 != 'C' || m3 != 0x01) return err("bad magic");

  uint16_t version = r.u16();
  r.u16();  // flags (reserved)
  if (r.bad) return err("truncated header");
  if (version != 1) return err("unsupported version");

  uint32_t const_count = r.u32();
  uint32_t func_count = r.u32();
  uint32_t entry_func = r.u32();
  if (r.bad) return err("truncated header");

  // 2. Sanity caps: each entry is >= 1 byte, so a count exceeding the remaining
  // bytes is provably a lie. Rejects giant declared counts before any
  // count-sized allocation.
  if (const_count > size || func_count > size) return err("corrupt counts");

  // 3. Constant pool.
  for (uint32_t c = 0; c < const_count; ++c) {
    uint8_t tag = r.u8();
    if (r.bad) return err("truncated constant");
    Constant k;
    k.tag = static_cast<CTag>(tag);
    switch (static_cast<CTag>(tag)) {
      case CTag::Int:
        k.i = r.i64();
        break;
      case CTag::Double:
        k.d = r.f64();
        break;
      case CTag::Str: {
        uint32_t len = r.u32();
        if (r.bad) return err("truncated constant");
        if (!r.need(len)) return err("truncated string constant");
        k.s.assign(reinterpret_cast<const char*>(r.p + r.i), len);
        r.i += len;
        break;
      }
      case CTag::Nil:
        break;
      case CTag::Bool:
        k.b = r.u8() != 0;
        break;
      default:
        return err("unknown constant tag");
    }
    if (r.bad) return err("truncated constant");
    m.consts.push_back(std::move(k));
  }

  // 4. Function table.
  for (uint32_t fi = 0; fi < func_count; ++fi) {
    Function f;
    f.name_const = r.u32();
    f.num_params = r.u8();
    f.num_regs = r.u8();
    f.max_stack = r.u16();
    uint32_t code_len = r.u32();
    if (r.bad) return err("truncated function header");

    // Structural: name_const must point at an existing Str constant.
    if (f.name_const >= m.consts.size()) return err("function name out of range");
    if (m.consts[f.name_const].tag != CTag::Str) return err("function name not a string");

    if (!r.need(code_len)) return err("truncated function code");
    f.code.assign(r.p + r.i, r.p + r.i + code_len);
    r.i += code_len;

    m.funcs.push_back(std::move(f));
  }

  // 5. Final structural check. Trailing bytes are ignored (forgiving).
  if (entry_func >= m.funcs.size()) return err("entry out of range");
  m.entry_func = entry_func;

  out.ok = true;
  return out;
}

}  // namespace coal
