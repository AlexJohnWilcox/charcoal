#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace coal {

enum class CTag : uint8_t { Int = 0, Double = 1, Str = 2, Nil = 3, Bool = 4 };

struct Constant {
  CTag        tag;
  int64_t     i = 0;   // Int
  double      d = 0;   // Double
  bool        b = false;  // Bool
  std::string s;       // Str
};

struct Function {
  uint32_t             name_const = 0;  // index into Module::consts (a Str)
  uint8_t              num_params = 0;
  uint8_t              num_regs = 0;    // register window size; >= num_params
  uint16_t             max_stack = 0;   // advisory in M1
  std::vector<uint8_t> code;            // bytecode (see opcode table)
};

struct Module {
  std::vector<Constant> consts;
  std::vector<Function> funcs;
  uint32_t              entry_func = 0;  // index into funcs
};

// Opcodes (1-byte) — operand encodings: r=u8 reg, k=u16 const, n=u8 count,
// o=i16 jump offset relative to the byte AFTER the operand.
enum Op : uint8_t {
  OP_HALT       = 0x00,
  OP_LOAD_CONST = 0x01,  // r, k
  OP_LOAD_NIL   = 0x02,  // r
  OP_MOVE       = 0x03,  // r, r2
  OP_ADD        = 0x10,  // r, r2, r3
  OP_SUB        = 0x11,  // r, r2, r3
  OP_MUL        = 0x12,  // r, r2, r3
  OP_DIV        = 0x13,  // r, r2, r3
  OP_NEW_ARRAY  = 0x20,  // r, n
  OP_ARRAY_GET  = 0x21,  // r, r2, r3
  OP_ARRAY_SET  = 0x22,  // r, r2, r3   (r[r2] = r3)
  OP_NEW_OBJECT = 0x23,  // r
  OP_GET_PROP   = 0x24,  // r, r2, k
  OP_SET_PROP   = 0x25,  // r, k, r2    (r.k = r2)
  OP_CALL       = 0x30,  // r, k, n     (call const[k] by name->index)
  OP_RET        = 0x31,  // r
  OP_JUMP       = 0x40,  // o
  OP_JUMP_IF_FALSE = 0x41,  // r, o
  OP_PRINT      = 0x50,  // r
};

// Serialize to the .cbc v1 binary format (implemented in Task 5).
std::vector<uint8_t> serialize(const Module& m);

}  // namespace coal
