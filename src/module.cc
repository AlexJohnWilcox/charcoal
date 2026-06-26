#include "module.h"

#include <cstring>

namespace coal {

namespace {

void put8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }

void put16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xff));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

void put64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

void put_double(std::vector<uint8_t>& b, double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  put64(b, bits);
}

}  // namespace

std::vector<uint8_t> serialize(const Module& m) {
  std::vector<uint8_t> b;

  // Header.
  b.push_back('C');
  b.push_back('B');
  b.push_back('C');
  b.push_back(0x01);
  put16(b, 1);  // version
  put16(b, 0);  // flags
  put32(b, static_cast<uint32_t>(m.consts.size()));
  put32(b, static_cast<uint32_t>(m.funcs.size()));
  put32(b, m.entry_func);

  // Constant pool.
  for (const Constant& c : m.consts) {
    put8(b, static_cast<uint8_t>(c.tag));
    switch (c.tag) {
      case CTag::Int:
        put64(b, static_cast<uint64_t>(c.i));
        break;
      case CTag::Double:
        put_double(b, c.d);
        break;
      case CTag::Str:
        put32(b, static_cast<uint32_t>(c.s.size()));
        b.insert(b.end(), c.s.begin(), c.s.end());
        break;
      case CTag::Nil:
        break;
      case CTag::Bool:
        put8(b, c.b ? 1 : 0);
        break;
    }
  }

  // Function table.
  for (const Function& f : m.funcs) {
    put32(b, f.name_const);
    put8(b, f.num_params);
    put8(b, f.num_regs);
    put16(b, f.max_stack);
    put32(b, static_cast<uint32_t>(f.code.size()));
    b.insert(b.end(), f.code.begin(), f.code.end());
  }

  return b;
}

}  // namespace coal
