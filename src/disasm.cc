#include "disasm.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coal {

namespace {

// Operand kinds, mirroring the verifier's layout table.
//   R = u8 register, K = u16 constant index, N = u8 count, O = i16 jump offset.
enum class Opnd { R, K, N, O };

size_t opnd_width(Opnd o) {
  switch (o) {
    case Opnd::R:
    case Opnd::N: return 1;
    case Opnd::K:
    case Opnd::O: return 2;
  }
  return 0;
}

// Fill `name` and `ops`/`nops` for an opcode. Returns false for an unknown one.
bool op_info(uint8_t op, const char*& name, Opnd ops[4], int& nops) {
  auto set = [&](const char* nm, std::initializer_list<Opnd> os) {
    name = nm;
    nops = 0;
    for (Opnd o : os) ops[nops++] = o;
  };
  switch (op) {
    case OP_HALT:         set("HALT", {}); break;
    case OP_LOAD_CONST:   set("LOAD_CONST", {Opnd::R, Opnd::K}); break;
    case OP_LOAD_NIL:     set("LOAD_NIL", {Opnd::R}); break;
    case OP_MOVE:         set("MOVE", {Opnd::R, Opnd::R}); break;
    case OP_ADD:          set("ADD", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_SUB:          set("SUB", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_MUL:          set("MUL", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_DIV:          set("DIV", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_MOD:          set("MOD", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_EQ:           set("EQ", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_NE:           set("NE", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_LT:           set("LT", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_LE:           set("LE", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_GT:           set("GT", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_GE:           set("GE", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_BAND:         set("BAND", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_BOR:          set("BOR", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_BXOR:         set("BXOR", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_SHL:          set("SHL", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_SHR:          set("SHR", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_NEW_ARRAY:    set("NEW_ARRAY", {Opnd::R, Opnd::N}); break;
    case OP_ARRAY_GET:    set("ARRAY_GET", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_ARRAY_SET:    set("ARRAY_SET", {Opnd::R, Opnd::R, Opnd::R}); break;
    case OP_ARRAY_PUSH:   set("ARRAY_PUSH", {Opnd::R, Opnd::R}); break;
    case OP_NEW_OBJECT:   set("NEW_OBJECT", {Opnd::R}); break;
    case OP_GET_PROP:     set("GET_PROP", {Opnd::R, Opnd::R, Opnd::K}); break;
    case OP_SET_PROP:     set("SET_PROP", {Opnd::R, Opnd::K, Opnd::R}); break;
    case OP_CALL:         set("CALL", {Opnd::R, Opnd::K, Opnd::N}); break;
    case OP_CALL_NATIVE:  set("CALL_NATIVE", {Opnd::R, Opnd::K, Opnd::N}); break;
    case OP_RET:          set("RET", {Opnd::R}); break;
    case OP_JUMP:         set("JUMP", {Opnd::O}); break;
    case OP_JUMP_IF_FALSE: set("JUMP_IF_FALSE", {Opnd::R, Opnd::O}); break;
    case OP_PRINT:        set("PRINT", {Opnd::R}); break;
    case OP_NEG:          set("NEG", {Opnd::R, Opnd::R}); break;
    case OP_NOT:          set("NOT", {Opnd::R, Opnd::R}); break;
    case OP_BNOT:         set("BNOT", {Opnd::R, Opnd::R}); break;
    default: return false;
  }
  return true;
}

// A short, quote-escaped preview of a string constant for an inline comment.
std::string quote_str(const std::string& s) {
  std::string out = "\"";
  for (size_t i = 0; i < s.size() && i < 32; ++i) {
    char c = s[i];
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:   out += c; break;
    }
  }
  if (s.size() > 32) out += "...";
  out += "\"";
  return out;
}

// Inline comment describing constant k, if it is in range.
std::string const_comment(const Module& m, uint16_t k) {
  if (k >= m.consts.size()) return "  ; <const out of range>";
  const Constant& c = m.consts[k];
  switch (c.tag) {
    case CTag::Int:    return "  ; " + std::to_string(c.i);
    case CTag::Double: return "  ; " + std::to_string(c.d);
    case CTag::Bool:   return c.b ? "  ; true" : "  ; false";
    case CTag::Nil:    return "  ; nil";
    case CTag::Str:    return "  ; " + quote_str(c.s);
  }
  return "";
}

void append_hex_offset(std::string& out, size_t pc) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "  %04zu: ", pc);
  out += buf;
}

void disassemble_fn(const Module& m, const Function& f, uint32_t fi, std::string& out) {
  // Header line.
  std::string name = "<bad-name>";
  if (f.name_const < m.consts.size() && m.consts[f.name_const].tag == CTag::Str)
    name = m.consts[f.name_const].s;
  {
    char buf[128];
    std::snprintf(buf, sizeof buf, "fn #%u \"%s\"  params=%u regs=%u code=%zu\n",
                  fi, name.c_str(), f.num_params, f.num_regs, f.code.size());
    out += buf;
  }

  const std::vector<uint8_t>& code = f.code;
  const size_t len = code.size();
  size_t pc = 0;
  while (pc < len) {
    uint8_t op = code[pc];
    const char* name_mn = nullptr;
    Opnd ops[4];
    int nops = 0;
    if (!op_info(op, name_mn, ops, nops)) {
      append_hex_offset(out, pc);
      char buf[32];
      std::snprintf(buf, sizeof buf, "??? 0x%02x\n", op);
      out += buf;
      break;  // can't know the width of an unknown opcode -> stop this function
    }

    size_t operand_bytes = 0;
    for (int i = 0; i < nops; ++i) operand_bytes += opnd_width(ops[i]);

    // Defensive bounds check (real input is verified, crafted input may not be).
    if (pc + 1 + operand_bytes > len) {
      append_hex_offset(out, pc);
      out += name_mn;
      out += " <truncated>\n";
      break;
    }

    append_hex_offset(out, pc);
    out += name_mn;

    size_t at = pc + 1;
    std::string comment;
    for (int i = 0; i < nops; ++i) {
      out += (i == 0) ? " " : ", ";
      switch (ops[i]) {
        case Opnd::R: {
          char buf[8];
          std::snprintf(buf, sizeof buf, "r%u", code[at]);
          out += buf;
          break;
        }
        case Opnd::N: {
          char buf[8];
          std::snprintf(buf, sizeof buf, "n%u", code[at]);
          out += buf;
          break;
        }
        case Opnd::K: {
          uint16_t k = static_cast<uint16_t>(code[at] | (code[at + 1] << 8));
          char buf[8];
          std::snprintf(buf, sizeof buf, "k%u", k);
          out += buf;
          comment = const_comment(m, k);
          break;
        }
        case Opnd::O: {
          int16_t off = static_cast<int16_t>(code[at] | (code[at + 1] << 8));
          size_t target = pc + 1 + operand_bytes + off;
          char buf[48];
          std::snprintf(buf, sizeof buf, "%+d -> @%04zu", off, target);
          out += buf;
          break;
        }
      }
      at += opnd_width(ops[i]);
    }
    out += comment;
    out += "\n";
    pc += 1 + operand_bytes;
  }
}

}  // namespace

std::string disassemble(const Module& m) {
  std::string out;
  {
    char buf[96];
    std::snprintf(buf, sizeof buf, "; module: %zu consts, %zu funcs, entry #%u\n",
                  m.consts.size(), m.funcs.size(), m.entry_func);
    out += buf;
  }
  for (uint32_t fi = 0; fi < m.funcs.size(); ++fi) {
    out += "\n";
    disassemble_fn(m, m.funcs[fi], fi, out);
  }
  return out;
}

}  // namespace coal
