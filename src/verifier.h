#pragma once
#include "module.h"
#include <string>

namespace coal {

struct VerifyResult {
  std::string error;   // non-empty iff ok == false
  bool        ok = false;
};

// Structural verification of a loaded Module: for every instruction in every
// function, confirm the opcode is known, the stream doesn't end mid-instruction,
// register operands are < num_regs, constant operands are < consts.size(),
// CALL targets name an existing Str constant, and jump offsets land on an
// in-range byte. A Module that verifies can be executed without the interpreter
// re-checking these — so this MUST be sound.
VerifyResult verify(const Module& m);

}  // namespace coal
