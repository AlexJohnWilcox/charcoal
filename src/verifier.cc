#include "verifier.h"

#include <string>
#include <vector>

namespace coal {

namespace {

VerifyResult ok_result() {
  VerifyResult r;
  r.ok = true;
  return r;
}

VerifyResult err(const char* msg) {
  VerifyResult r;
  r.error = msg;
  r.ok = false;
  return r;
}

// Operand roles, validated by kind. The table below mirrors the opcode table.
enum class Operand { Reg, Const, ConstStr, Count, Jump };

// Returns false for an unknown opcode. Otherwise fills `ops` with this
// instruction's operand layout (operand byte count = sum of operand widths;
// Const/ConstStr/Jump are 2 bytes, Reg/Count are 1).
bool operands_of(uint8_t op, std::vector<Operand>& ops) {
  ops.clear();
  switch (op) {
    case OP_HALT:
      break;
    case OP_LOAD_NIL:
    case OP_PRINT:
    case OP_RET:
    case OP_NEW_OBJECT:
      ops = {Operand::Reg};
      break;
    case OP_MOVE:
      ops = {Operand::Reg, Operand::Reg};
      break;
    case OP_NEW_ARRAY:
      ops = {Operand::Reg, Operand::Count};
      break;
    case OP_LOAD_CONST:
      ops = {Operand::Reg, Operand::Const};
      break;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_ARRAY_GET:
    case OP_ARRAY_SET:
      ops = {Operand::Reg, Operand::Reg, Operand::Reg};
      break;
    case OP_GET_PROP:
      ops = {Operand::Reg, Operand::Reg, Operand::ConstStr};
      break;
    case OP_SET_PROP:
      ops = {Operand::Reg, Operand::ConstStr, Operand::Reg};
      break;
    case OP_CALL:
      ops = {Operand::Reg, Operand::ConstStr, Operand::Count};
      break;
    case OP_JUMP:
      ops = {Operand::Jump};
      break;
    case OP_JUMP_IF_FALSE:
      ops = {Operand::Reg, Operand::Jump};
      break;
    default:
      return false;
  }
  return true;
}

size_t width(Operand o) {
  switch (o) {
    case Operand::Reg:
    case Operand::Count:
      return 1;
    case Operand::Const:
    case Operand::ConstStr:
    case Operand::Jump:
      return 2;
  }
  return 0;
}

VerifyResult verify_fn(const Module& m, const Function& f) {
  const std::vector<uint8_t>& code = f.code;
  const size_t len = code.size();
  std::vector<bool> is_start(len, false);

  struct PendingJump { size_t target_base; int16_t off; };
  std::vector<PendingJump> jumps;

  std::vector<Operand> ops;

  // Pass 1: decode, bounds-check, validate operands.
  size_t pc = 0;
  while (pc < len) {
    is_start[pc] = true;
    uint8_t op = code[pc];
    if (!operands_of(op, ops)) return err("unknown opcode");

    size_t nops = 0;
    for (Operand o : ops) nops += width(o);

    // Bounds gate: after this, every operand byte below is safe to read.
    if (pc + 1 + nops > len) return err("truncated instruction");

    size_t at = pc + 1;
    // For CALL we need the base register and count together for the window check.
    int call_base = -1, call_n = -1;
    for (Operand o : ops) {
      switch (o) {
        case Operand::Reg: {
          uint8_t r = code[at];
          if (r >= f.num_regs) return err("register operand out of range");
          if (op == OP_CALL) call_base = r;
          break;
        }
        case Operand::Const: {
          uint16_t k = static_cast<uint16_t>(code[at] | (code[at + 1] << 8));
          if (k >= m.consts.size()) return err("constant operand out of range");
          break;
        }
        case Operand::ConstStr: {
          uint16_t k = static_cast<uint16_t>(code[at] | (code[at + 1] << 8));
          if (k >= m.consts.size()) return err("constant operand out of range");
          if (m.consts[k].tag != CTag::Str) return err("constant operand not a string");
          break;
        }
        case Operand::Count:
          if (op == OP_CALL) call_n = code[at];
          break;
        case Operand::Jump: {
          int16_t off = static_cast<int16_t>(code[at] | (code[at + 1] << 8));
          jumps.push_back({pc + 1 + nops, off});  // relative to end of instruction
          break;
        }
      }
      at += width(o);
    }

    // CALL: the argument window [base, base+n) must fit in the register file.
    if (op == OP_CALL) {
      if (call_base + call_n > f.num_regs) return err("call register window out of range");
    }

    pc += 1 + nops;
  }
  // The truncation gate guarantees we land exactly on len.

  // Pass 2: every jump must target an instruction boundary.
  for (const PendingJump& j : jumps) {
    long long target = static_cast<long long>(j.target_base) + j.off;
    if (target < 0 || target >= static_cast<long long>(len))
      return err("jump out of range");
    if (!is_start[static_cast<size_t>(target)])
      return err("jump into mid-instruction");
  }

  return ok_result();
}

}  // namespace

VerifyResult verify(const Module& m) {
  for (const Function& f : m.funcs) {
    VerifyResult r = verify_fn(m, f);
    if (!r.ok) return r;
  }
  return ok_result();
}

}  // namespace coal
