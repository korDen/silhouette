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
  std::string hierOut;
  std::string assetRoot;
  std::string missingAssetsFile;
  std::string schemaInclude = "schema.h"; // caller passes the real path
  std::string ns = "hud";
  std::vector<std::string> files;
  std::vector<std::string> styleFiles;
  std::vector<std::string> withFiles;
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
    } else if (std::strcmp(argv[i], "--emit-hier") == 0) {
      hierOut = next();
    } else if (std::strcmp(argv[i], "--assets") == 0) {
      assetRoot = next();
      // one asset path per line; '#' starts a comment. A missing file is a
      // hard error — this is the only way to accept one, and every entry
      // has to be written down (and justified) by hand.
    } else if (std::strcmp(argv[i], "--missing-assets") == 0) {
      missingAssetsFile = next();
    } else if (std::strcmp(argv[i], "--styles") == 0) {
      styleFiles.emplace_back(next());
    } else if (std::strcmp(argv[i], "--with") == 0) {
      withFiles.emplace_back(next());
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
      if (!missingAssetsFile.empty()) {
        std::ifstream mf(missingAssetsFile);
        if (!mf) {
          std::fprintf(stderr, "cannot open %s\n", missingAssetsFile.c_str());
          return 2;
        }
        std::string line;
        while (std::getline(mf, line)) {
          const size_t hash = line.find('#');
          if (hash != std::string::npos) {
            line = line.substr(0, hash);
          }
          const size_t b = line.find_first_not_of(" \t\r");
          if (b == std::string::npos) {
            continue;
          }
          const size_t e = line.find_last_not_of(" \t\r");
          opt.allowedMissingAssets.insert(line.substr(b, e - b + 1));
        }
      }
      // side modules (--styles/--with): parsed once, owned here for
      // the emit's duration
      std::vector<uic::Module> sideModules;
      sideModules.reserve(styleFiles.size() + withFiles.size());
      bool sidesOk = true;
      auto loadSide = [&](const std::string &sf) -> const uic::Module * {
        std::ifstream sin(sf, std::ios::binary);
        if (!sin) {
          std::fprintf(stderr, "%s: cannot read module\n", sf.c_str());
          return nullptr;
        }
        std::ostringstream sss;
        sss << sin.rdbuf();
        std::vector<uic::Diag> sd;
        sideModules.push_back(uic::parseModule(sss.str(), sf, sd));
        for (const uic::Diag &d : sd) {
          std::fprintf(stderr, "%s:%d:%d: %s\n", d.file.c_str(), d.line,
                       d.col, d.msg.c_str());
        }
        return uic::hasErrors(sd) ? nullptr : &sideModules.back();
      };
      for (const std::string &sf : styleFiles) {
        const uic::Module *sm = loadSide(sf);
        if (sm == nullptr) {
          sidesOk = false;
          break;
        }
        opt.styleModules.push_back(sm);
      }
      for (const std::string &wf : withFiles) {
        const uic::Module *wm = sidesOk ? loadSide(wf) : nullptr;
        if (wm == nullptr) {
          sidesOk = false;
          break;
        }
        opt.withModules.push_back(wm);
      }
      if (!sidesOk) {
        ++failures;
        continue;
      }
      std::vector<uic::Diag> emitDiags;
      std::string hierStr;
      const std::string header = uic::emitPanelHeader(
          m, opt, emitDiags, hierOut.empty() ? nullptr : &hierStr);
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
      if (!hierOut.empty()) {
        std::ofstream hf(hierOut, std::ios::binary);
        if (!hf) {
          std::fprintf(stderr, "%s: cannot write\n", hierOut.c_str());
          ++failures;
          continue;
        }
        hf << hierStr;
        std::printf("hier -> %s\n", hierOut.c_str());
      }
    }
  }
  return failures == 0 ? 0 : 1;
}
