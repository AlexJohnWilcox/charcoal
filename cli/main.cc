#include "charcoal.h"
#include "astprint.h"
#include "disasm.h"
#include "lexer.h"
#include "loader.h"
#include "parser.h"
#include "verifier.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Read a whole file into a byte buffer. Returns false (with a message) if the
// file can't be opened, so a missing path errors cleanly instead of crashing.
bool read_file(const char* path, std::vector<uint8_t>& out, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { err = std::string("cannot open file: ") + path; return false; }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool write_file(const char* path, const std::vector<uint8_t>& data, std::string& err) {
  std::ofstream out(path, std::ios::binary);
  if (!out) { err = std::string("cannot write file: ") + path; return false; }
  if (!data.empty())
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  if (!out) { err = std::string("write failed: ") + path; return false; }
  return true;
}

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  charcoal compile <in.char> -o <out.cbc>\n"
               "  charcoal run <module.cbc>\n"
               "  charcoal dis <module.cbc>\n"
               "  charcoal tokens <in.char>\n"
               "  charcoal ast <in.char>\n"
               "  charcoal fmt <in.char>\n"
               "  charcoal check <in.char>\n"
               "  charcoal eval <source>\n"
               "  charcoal repl\n");
  return 1;
}

int do_compile(int argc, char** argv) {
  // charcoal compile <in.char> -o <out.cbc>
  if (argc != 5 || std::string(argv[3]) != "-o") return usage();
  const char* in_path = argv[2];
  const char* out_path = argv[4];

  std::vector<uint8_t> srcbytes;
  std::string err;
  if (!read_file(in_path, srcbytes, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

  std::vector<uint8_t> cbc;
  if (!coal::compile_source(reinterpret_cast<const char*>(srcbytes.data()),
                            srcbytes.size(), cbc, err)) {
    std::fprintf(stderr, "compile error: %s\n", err.c_str());
    return 1;
  }
  if (!write_file(out_path, cbc, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  return 0;
}

int do_run(int argc, char** argv) {
  // charcoal run <module.cbc>
  if (argc != 3) return usage();
  std::vector<uint8_t> cbc;
  std::string err;
  if (!read_file(argv[2], cbc, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

  coal::RunResult r = coal::run_cbc(cbc.data(), cbc.size());
  std::fputs(r.output.c_str(), stdout);
  if (!r.ok) {
    std::fprintf(stderr, "runtime error: %s\n", r.error.c_str());
    return 1;
  }
  return 0;
}

int do_dis(int argc, char** argv) {
  // charcoal dis <module.cbc>
  if (argc != 3) return usage();
  std::vector<uint8_t> cbc;
  std::string err;
  if (!read_file(argv[2], cbc, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

  coal::LoadResult ld = coal::load_cbc(cbc.data(), cbc.size());
  if (!ld.ok) {
    std::fprintf(stderr, "load error: %s\n", ld.error.c_str());
    return 1;
  }
  std::fputs(coal::disassemble(ld.module).c_str(), stdout);
  return 0;
}

// Lex a source file and read it into a token vector, reporting a clean error on
// a missing file or a lex failure. Shared by the source-inspection commands.
bool lex_source(const char* path, coal::LexResult& lr, std::vector<uint8_t>& src) {
  std::string err;
  if (!read_file(path, src, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return false; }
  lr = coal::lex(reinterpret_cast<const char*>(src.data()), src.size());
  if (!lr.error.empty()) { std::fprintf(stderr, "lex error: %s\n", lr.error.c_str()); return false; }
  return true;
}

int do_tokens(int argc, char** argv) {
  // charcoal tokens <in.char>
  if (argc != 3) return usage();
  coal::LexResult lr;
  std::vector<uint8_t> src;
  if (!lex_source(argv[2], lr, src)) return 1;
  std::fputs(coal::dump_tokens(lr.tokens).c_str(), stdout);
  return 0;
}

int do_ast(int argc, char** argv) {
  // charcoal ast <in.char>
  if (argc != 3) return usage();
  coal::LexResult lr;
  std::vector<uint8_t> src;
  if (!lex_source(argv[2], lr, src)) return 1;
  coal::ParseResult pr = coal::parse(lr.tokens);
  if (!pr.error.empty() || !pr.program) {
    std::fprintf(stderr, "parse error: %s\n", pr.error.c_str());
    return 1;
  }
  std::fputs(coal::dump_ast(pr.program).c_str(), stdout);
  return 0;
}

int do_fmt(int argc, char** argv) {
  // charcoal fmt <in.char>
  if (argc != 3) return usage();
  coal::LexResult lr;
  std::vector<uint8_t> src;
  if (!lex_source(argv[2], lr, src)) return 1;
  coal::ParseResult pr = coal::parse(lr.tokens);
  if (!pr.error.empty() || !pr.program) {
    std::fprintf(stderr, "parse error: %s\n", pr.error.c_str());
    return 1;
  }
  std::fputs(coal::format_source(pr.program).c_str(), stdout);
  return 0;
}

// Compile and statically check a program — lex, parse, fold, compile, and run it
// back through the loader and verifier — without executing it. Prints "ok" when
// the module is well-formed, or the first error encountered.
int do_check(int argc, char** argv) {
  // charcoal check <in.char>
  if (argc != 3) return usage();
  std::vector<uint8_t> src;
  std::string err;
  if (!read_file(argv[2], src, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

  std::vector<uint8_t> cbc;
  if (!coal::compile_source(reinterpret_cast<const char*>(src.data()), src.size(), cbc, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  coal::LoadResult ld = coal::load_cbc(cbc.data(), cbc.size());
  if (!ld.ok) { std::fprintf(stderr, "load error: %s\n", ld.error.c_str()); return 1; }
  auto v = coal::verify(ld.module);
  if (!v.ok) { std::fprintf(stderr, "verify error: %s\n", v.error.c_str()); return 1; }
  std::fputs("ok\n", stdout);
  return 0;
}

int do_eval(int argc, char** argv) {
  // charcoal eval <source>
  if (argc != 3) return usage();
  std::string src = argv[2];
  std::string err;
  std::vector<uint8_t> cbc;
  if (!coal::compile_source(src.data(), src.size(), cbc, err)) {
    std::fprintf(stderr, "compile error: %s\n", err.c_str());
    return 1;
  }
  coal::RunResult r = coal::run_cbc(cbc.data(), cbc.size());
  std::fputs(r.output.c_str(), stdout);
  if (!r.ok) { std::fprintf(stderr, "runtime error: %s\n", r.error.c_str()); return 1; }
  return 0;
}

// A line-at-a-time REPL: each input line is compiled and run as its own program,
// with output echoed to stdout and errors reported without stopping the loop.
int do_repl(int argc, char**) {
  if (argc != 2) return usage();
  std::string line;
  while (true) {
    std::fputs("charcoal> ", stderr);   // prompt on stderr; stdout stays results-only
    if (!std::getline(std::cin, line)) break;
    if (line.empty()) continue;

    std::string err;
    std::vector<uint8_t> cbc;
    if (!coal::compile_source(line.data(), line.size(), cbc, err)) {
      std::fprintf(stderr, "compile error: %s\n", err.c_str());
      continue;
    }
    coal::RunResult r = coal::run_cbc(cbc.data(), cbc.size());
    std::fputs(r.output.c_str(), stdout);
    if (!r.output.empty() && r.output.back() != '\n') std::fputc('\n', stdout);
    if (!r.ok) std::fprintf(stderr, "runtime error: %s\n", r.error.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  if (cmd == "compile") return do_compile(argc, argv);
  if (cmd == "run")     return do_run(argc, argv);
  if (cmd == "dis")     return do_dis(argc, argv);
  if (cmd == "tokens")  return do_tokens(argc, argv);
  if (cmd == "ast")     return do_ast(argc, argv);
  if (cmd == "fmt")     return do_fmt(argc, argv);
  if (cmd == "check")   return do_check(argc, argv);
  if (cmd == "eval")    return do_eval(argc, argv);
  if (cmd == "repl")    return do_repl(argc, argv);
  return usage();
}
