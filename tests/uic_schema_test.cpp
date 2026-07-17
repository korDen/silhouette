// uic schema pass tests — schema modules (structs + enums, fixed arrays,
// defaults) emit self-contained trivially-copyable C++ headers; the
// no-strings/no-pointers contract is asserted INTO the output.
#include "uic/uic.hpp"

#include <gtest/gtest.h>

namespace {

std::string emit(std::string_view src, std::vector<uic::Diag> *diags) {
  std::vector<uic::Diag> parseDiags;
  const uic::Module m = uic::parseModule(src, "state.ui", parseDiags);
  EXPECT_TRUE(parseDiags.empty())
      << (parseDiags.empty() ? "" : parseDiags[0].msg);
  return uic::emitSchemaHeader(m, "app", *diags);
}

constexpr std::string_view kSchema =
    "enum Kind { Small, Wide, Tall }\n"
    "enum Wire { Off = 1, On = 4 }\n"
    "struct Member {\n"
    "    icon: texture;\n"
    "    hotkey: char[8];\n"
    "    ready: bool = true;\n"
    "    cost: int = 0;\n"
    "}\n"
    "struct Slot {\n"
    "    exists: bool;\n"
    "    kind: Kind = Small;\n"
    "    heat: float;\n"
    "    members: Member[3];\n"
    "}\n";

TEST(UicSchema, EnumsStructsArraysDefaults) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(kSchema, &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("namespace app {"), std::string::npos);
  EXPECT_NE(h.find("enum class Kind : int32_t {"), std::string::npos);
  EXPECT_NE(h.find("Off = 1,"), std::string::npos);
  EXPECT_NE(h.find("ui::TextureId icon{};"), std::string::npos);
  EXPECT_NE(h.find("std::array<char, 8> hotkey{};"), std::string::npos);
  EXPECT_NE(h.find("bool ready = true;"), std::string::npos);
  EXPECT_NE(h.find("Kind kind = Kind::Small;"), std::string::npos);
  EXPECT_NE(h.find("std::array<Member, 3> members{};"), std::string::npos);
}

TEST(UicSchema, ContractIsAssertedIntoTheOutput) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(kSchema, &diags);
  ASSERT_TRUE(diags.empty());
  EXPECT_NE(h.find("static_assert(std::is_trivially_copyable_v<Member>"),
            std::string::npos);
  EXPECT_NE(h.find("static_assert(std::is_trivially_copyable_v<Slot>"),
            std::string::npos);
  EXPECT_NE(h.find("DO NOT EDIT"), std::string::npos);
}

TEST(UicSchema, UnknownAndForwardTypesAreErrors) {
  // `str` is NOT a schema type (snapshots carry no strings), and structs
  // must be declared before use
  std::vector<uic::Diag> diags;
  emit("struct A { name: str; b: B; }\nstruct B { x: int; }\n", &diags);
  ASSERT_EQ(diags.size(), 2u);
  EXPECT_NE(diags[0].msg.find("unknown type 'str'"), std::string::npos);
  EXPECT_NE(diags[1].msg.find("unknown type 'B'"), std::string::npos);
}

TEST(UicParse, EnumAndArrayFieldShapes) {
  std::vector<uic::Diag> diags;
  const uic::Module m = uic::parseModule(
      "enum Kind { A, B = 2, C, }\nstruct S { xs: int[4]; k: Kind; }\n",
      "t.ui", diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  const std::string d = uic::dumpModule(m);
  EXPECT_NE(d.find("enum Kind { A, B=2, C }"), std::string::npos);
  EXPECT_NE(d.find("xs: int[4];"), std::string::npos);
}

} // namespace
