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
  std::string file;
  int line = 0;
  int col = 0;
  std::string msg;
};

// Parse one module. `fileName` labels diagnostics and becomes
// Module::name (basename, extension stripped). Recovers at statement
// boundaries — a failed parse still returns everything it understood,
// with every error in `diags`.
Module parseModule(std::string_view source, std::string_view fileName,
                   std::vector<Diag> &diags);

// Deterministic tree dump — the parser tests' comparison form.
std::string dumpModule(const Module &m);

} // namespace uic
