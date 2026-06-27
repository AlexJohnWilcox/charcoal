#pragma once
#include "module.h"
#include <string>

namespace coal {

// Human-readable dump of a whole module: a per-function header followed by the
// decoded instructions. Defensive: never reads past a function's code — a
// truncated trailing instruction is reported, not read into.
std::string disassemble(const Module& m);

}  // namespace coal
