#pragma once
// uic — the markup compiler's public surface (docs/ui-language.md).
// Pure std; built by host regen scripts and by the test suite, never
// part of the library build.
#include "uic/ast.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace uic {

struct Diag {
  enum class Severity { kWarning, kError };
  std::string file;
  int line = 0;
  int col = 0;
  std::string msg;
  Severity severity = Severity::kError;
};

inline bool hasErrors(const std::vector<Diag> &diags) {
  for (const Diag &d : diags) {
    if (d.severity == Diag::Severity::kError) {
      return true;
    }
  }
  return false;
}

// Parse one module. `fileName` labels diagnostics and becomes
// Module::name (basename, extension stripped). Recovers at statement
// boundaries — a failed parse still returns everything it understood,
// with every error in `diags`.
Module parseModule(std::string_view source, std::string_view fileName,
                   std::vector<Diag> &diags);

// Deterministic tree dump — the parser tests' comparison form.
std::string dumpModule(const Module &m);

// The printer: AST -> canonical .ui text. The one writer of .ui files
// (hosts transform the AST and print here); parse(print(m)) reproduces
// m exactly — comments are not part of the AST and do not survive.
std::string printModule(const Module &m);

// The schema pass: a schema module (structs + enums) becomes a
// self-contained C++ header in namespace `ns` — trivially copyable
// snapshot types, machine-asserted. Type errors land in `diags`.
std::string emitSchemaHeader(const Module &m, std::string_view ns,
                             std::vector<Diag> &diags);

// The emit pass (absolute subset — src/uic/emit.cpp documents the
// laws): a panel module becomes a self-contained header with
//   template <class Sink> void <module>(const UiSnapshot&, float, float,
//   Sink&)
// plus the {id, path} texture manifest. With assetRoot set, every
// authored path (frame families expanded) must exist on disk.
struct EmitOptions {
  std::string ns = "hud";
  std::string schemaInclude = "generated/UiSnapshot.h";
  std::string assetRoot; // empty = skip validation
  bool rectLog = false;  // emit the rect-gate hook (RectLog parameter)
  // styles, templates, and asset consts resolve from the module itself
  // plus these (parsed modules, caller-owned): --styles/--with
  std::vector<const Module *> styleModules;
  std::vector<const Module *> withModules;
};
std::string emitPanelHeader(const Module &m, const EmitOptions &opt,
                            std::vector<Diag> &diags);

} // namespace uic
