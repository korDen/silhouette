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
  std::string schemaOut;
  std::string emitOut;
  std::string assetRoot;
  std::string schemaInclude = "generated/UiSnapshot.h";
  std::string ns = "hud";
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s wants a value\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(argv[i], "--dump") == 0) {
      dump = true;
    } else if (std::strcmp(argv[i], "--schema") == 0) {
      schemaOut = next();
    } else if (std::strcmp(argv[i], "--emit") == 0) {
      emitOut = next();
    } else if (std::strcmp(argv[i], "--assets") == 0) {
      assetRoot = next();
    } else if (std::strcmp(argv[i], "--schema-include") == 0) {
      schemaInclude = next();
    } else if (std::strcmp(argv[i], "--namespace") == 0) {
      ns = next();
    } else if (argv[i][0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    } else {
      files.emplace_back(argv[i]);
    }
  }
  if (files.empty() ||
      ((!schemaOut.empty() || !emitOut.empty()) && files.size() != 1)) {
    std::fprintf(
        stderr,
        "usage: uic [--dump] <file.ui>...\n"
        "       uic --schema <out.h> [--namespace NS] <schema.ui>\n"
        "       uic --emit <out.h> [--namespace NS] [--assets ROOT]\n"
        "           [--schema-include PATH] <panel.ui>\n");
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
      std::fprintf(stderr, "%s:%d:%d: %s: %s\n", d.file.c_str(), d.line,
                   d.col,
                   d.severity == uic::Diag::Severity::kError ? "error"
                                                             : "warning",
                   d.msg.c_str());
    }
    if (uic::hasErrors(diags)) {
      ++failures;
    }
    if (dump) {
      std::fputs(uic::dumpModule(m).c_str(), stdout);
    }
    if (!schemaOut.empty() && !uic::hasErrors(diags)) {
      std::vector<uic::Diag> schemaDiags;
      const std::string header = uic::emitSchemaHeader(m, ns, schemaDiags);
      for (const uic::Diag &d : schemaDiags) {
        std::fprintf(stderr, "%s:%d: %s\n", d.file.c_str(), d.line,
                     d.msg.c_str());
      }
      if (uic::hasErrors(schemaDiags)) {
        ++failures;
        continue;
      }
      std::ofstream out(schemaOut, std::ios::binary);
      if (!out) {
        std::fprintf(stderr, "%s: cannot write\n", schemaOut.c_str());
        ++failures;
        continue;
      }
      out << header;
      std::printf("schema -> %s\n", schemaOut.c_str());
    }
    if (!emitOut.empty() && !uic::hasErrors(diags)) {
      uic::EmitOptions opt;
      opt.ns = ns;
      opt.schemaInclude = schemaInclude;
      opt.assetRoot = assetRoot;
      std::vector<uic::Diag> emitDiags;
      const std::string header = uic::emitPanelHeader(m, opt, emitDiags);
      int skips = 0;
      for (const uic::Diag &d : emitDiags) {
        if (d.severity == uic::Diag::Severity::kWarning) {
          ++skips;
          continue; // summarized below — degradation is loud but compact
        }
        std::fprintf(stderr, "%s:%d: error: %s\n", d.file.c_str(), d.line,
                     d.msg.c_str());
      }
      if (skips != 0) {
        std::fprintf(stderr, "%s: %d unsupported construct(s) skipped "
                             "(SKIP comments mark them in the output)\n",
                     f.c_str(), skips);
      }
      if (uic::hasErrors(emitDiags)) {
        ++failures;
        continue;
      }
      std::ofstream out(emitOut, std::ios::binary);
      if (!out) {
        std::fprintf(stderr, "%s: cannot write\n", emitOut.c_str());
        ++failures;
        continue;
      }
      out << header;
      std::printf("panel -> %s\n", emitOut.c_str());
    }
  }
  return failures == 0 ? 0 : 1;
}
