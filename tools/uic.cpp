// uic CLI — parse .ui modules; report diagnostics; optionally dump the
// AST (--dump). The compilation passes land behind this same entry as
// they arrive (docs/ui-language.md "Status"). Pure std; built by host
// regen scripts, never part of the library build.
#include "uic/uic.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  bool dump = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dump") == 0) {
      dump = true;
    } else if (argv[i][0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    } else {
      files.emplace_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: uic [--dump] <file.ui>...\n");
    return 2;
  }

  int failures = 0;
  for (const std::string &f : files) {
    std::ifstream in(f, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "%s: cannot read\n", f.c_str());
      ++failures;
      continue;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    std::vector<uic::Diag> diags;
    const uic::Module m = uic::parseModule(text, f, diags);
    for (const uic::Diag &d : diags) {
      std::fprintf(stderr, "%s:%d:%d: %s\n", d.file.c_str(), d.line, d.col,
                   d.msg.c_str());
    }
    if (!diags.empty()) {
      ++failures;
    }
    if (dump) {
      std::fputs(uic::dumpModule(m).c_str(), stdout);
    }
  }
  return failures == 0 ? 0 : 1;
}
