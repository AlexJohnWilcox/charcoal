#include "loader.h"
#include "module.h"
#include "test_main.h"

#include <cstdint>
#include <vector>

using namespace coal;

namespace {

// Build a .cbc with one Str constant ("m") and one function whose code is a
// single HALT. `const_count`, `func_count`, and `entry` are written into the
// header independently of what the body actually contains, so tests can make
// the header fields lie and confirm the loader rejects the mismatch.
std::vector<uint8_t> build(uint32_t const_count, uint32_t func_count,
                           uint32_t entry) {
  std::vector<uint8_t> b;
  auto u16 = [&](uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); };
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xff);
  };
  b.insert(b.end(), {'C', 'B', 'C', 0x01});  // magic
  u16(1);                                     // version
  u16(0);                                     // flags
  u32(const_count);
  u32(func_count);
  u32(entry);
  // constant pool: one Str "m"
  b.push_back(static_cast<uint8_t>(CTag::Str));
  u32(1);
  b.push_back('m');
  // function table: name_const=0, params=0, regs=1, max_stack=0, code=[HALT]
  u32(0);
  b.push_back(0);
  b.push_back(1);
  u16(0);
  u32(1);
  b.push_back(0x00);
  return b;
}

}  // namespace

void test_loader() {
  // Null / empty input -> error, no crash.
  CHECK(!load_cbc(nullptr, 0).ok);
  CHECK(!load_cbc(reinterpret_cast<const uint8_t*>(""), 0).ok);

  // Truncated header -> error.
  uint8_t junk[5] = {'C', 'B', 'C', 0x01, 0x00};
  CHECK(!load_cbc(junk, sizeof junk).ok);

  // Bad magic -> error.
  std::vector<uint8_t> bad(40, 0);
  bad[0] = 'X';
  CHECK(!load_cbc(bad.data(), bad.size()).ok);

  // Well-formed single-constant, single-function module loads.
  auto good = build(1, 1, 0);
  CHECK(load_cbc(good.data(), good.size()).ok);

  // entry_func out of range (>= func_count) -> error.
  auto baked = build(1, 1, 5);
  CHECK(!load_cbc(baked.data(), baked.size()).ok);

  // Header claims more functions than the body provides -> error, no overread.
  auto liar = build(1, 9, 0);
  CHECK(!load_cbc(liar.data(), liar.size()).ok);
}
