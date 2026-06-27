#include "charcoal.h"
#include "disasm.h"
#include "loader.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
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
               "  charcoal dis <module.cbc>\n");
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  if (cmd == "compile") return do_compile(argc, argv);
  if (cmd == "run")     return do_run(argc, argv);
  if (cmd == "dis")     return do_dis(argc, argv);
  return usage();
}
