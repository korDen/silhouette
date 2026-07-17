// uic parser tests — module items, the widget grammar, the expression
// grammar, the quoting law's lexical forms (verbatim raw values, path
// vs division disambiguation, dim/color literals), native bodies, and
// error recovery. Synthetic content only.
#include "uic/uic.hpp"

#include <gtest/gtest.h>

namespace {

uic::Module parse(std::string_view src, std::vector<uic::Diag> *diags) {
  return uic::parseModule(src, "test.ui", *diags);
}

std::string dumpOf(std::string_view src) {
  std::vector<uic::Diag> diags;
  const uic::Module m = parse(src, &diags);
  EXPECT_TRUE(diags.empty()) << (diags.empty() ? "" : diags[0].msg);
  return uic::dumpModule(m);
}

TEST(UicParse, WidgetAttrsAreVerbatim) {
  const std::string d = dumpOf(
      "panel {\n"
      "    name: main_row; y: -.4h; grow: 1; float: right; height: 100%;\n"
      "    color: 1 1 1 .15;\n"
      "    texture: /art/frames/plate.img;\n"
      "    label { content: \"Ready\"; font: small_bold; }\n"
      "}\n");
  EXPECT_NE(d.find("widget panel"), std::string::npos);
  EXPECT_NE(d.find("attr name=`main_row`"), std::string::npos);
  EXPECT_NE(d.find("attr y=`-.4h`"), std::string::npos);
  EXPECT_NE(d.find("attr color=`1 1 1 .15`"), std::string::npos);
  EXPECT_NE(d.find("attr texture=`/art/frames/plate.img`"),
            std::string::npos);
  EXPECT_NE(d.find("attr content=`\"Ready\"`"), std::string::npos);
}

TEST(UicParse, ImportsConstsStructsStyles) {
  const std::string d = dumpOf(
      "import { SlotState, Variant } from \"../schema/state.ui\";\n"
      "const kMask: asset = /art/mask.img;\n"
      "struct Frames { up: asset; over: asset; pad: dim = 1.5h; }\n"
      "const Standard = Frames { up: /art/f_up.img; over: /art/f_over.img; }\n"
      "style centered { align: center; valign: center; }\n");
  EXPECT_NE(d.find("import {SlotState, Variant} from \"../schema/state.ui\""),
            std::string::npos);
  EXPECT_NE(d.find("const kMask: asset = `/art/mask.img`"),
            std::string::npos);
  EXPECT_NE(d.find("struct Frames { up: asset; over: asset;"
                   " pad: dim = `1.5h`; }"),
            std::string::npos);
  EXPECT_NE(d.find("const Standard = Frames{ up=`/art/f_up.img`;"
                   " over=`/art/f_over.img`; }"),
            std::string::npos);
  EXPECT_NE(d.find("style centered { align=`center`; valign=`center`; }"),
            std::string::npos);
}

TEST(UicParse, ExpressionPrecedenceAndForms) {
  const std::string d = dumpOf(
      "fn pick(a: State, j: int) -> color =\n"
      "    a.level == 0 && a.maxLevel() > 0 ? #404040\n"
      "  : a.subs[j].busy ? #101010ff : #ffffff;\n");
  EXPECT_NE(
      d.find("fn pick(a: State, j: int) -> color = "
             "(?: (&& (== (. a level) 0) (> (call (. a maxLevel)) 0)) "
             "#404040 (?: (. ([] (. a subs) j) busy) #101010ff #ffffff))"),
      std::string::npos);
}

TEST(UicParse, SlashIsDivisionAfterOperandPathOtherwise) {
  const std::string d = dumpOf(
      "fn secs(ms: int) -> int = (ms + 999) / 1000;\n"
      "panel { bind texture: hot ? /art/hot.img : /art/cold.img; }\n");
  EXPECT_NE(d.find("= (/ (+ ms 999) 1000)"), std::string::npos);
  EXPECT_NE(d.find("bind texture= (?: hot /art/hot.img /art/cold.img)"),
            std::string::npos);
}

TEST(UicParse, DimLiteralsInExpressions) {
  const std::string d = dumpOf("fn wide(x: dim) -> dim = x + 12.1h;\n"
                               "fn neg() -> dim = -0.7h;\n");
  EXPECT_NE(d.find("= (+ x 12.1h)"), std::string::npos);
  EXPECT_NE(d.find("= (- 0.7h)"), std::string::npos);
}

TEST(UicParse, NativeFnBodyVerbatim) {
  std::vector<uic::Diag> diags;
  const uic::Module m = parse(
      "native fn fmtTime(seconds: double) -> str {{{\n"
      "    return fmt(\"%d\", (int)seconds); // '{' and '}' are fine\n"
      "}}}\n",
      &diags);
  ASSERT_TRUE(diags.empty());
  ASSERT_EQ(m.fns.size(), 1u);
  EXPECT_TRUE(m.fns[0].isNative);
  EXPECT_NE(m.fns[0].native.find("(int)seconds"), std::string::npos);
  EXPECT_NE(m.fns[0].native.find("'{'"), std::string::npos);
}

TEST(UicParse, TemplateInsAndInvocation) {
  const std::string d = dumpOf(
      "template member_cell {\n"
      "    in a: SlotState;\n"
      "    in col: align;\n"
      "    in pad: dim = 1.5h;\n"
      "    panel {\n"
      "        align: col;\n"
      "        bind visible: a.exists();\n"
      "        badge { n: 3; }\n"
      "    }\n"
      "}\n"
      "panel { member_cell { a: state; col: left; pad: 2h; } }\n");
  EXPECT_NE(d.find("template member_cell"), std::string::npos);
  EXPECT_NE(d.find("in a: SlotState"), std::string::npos);
  EXPECT_NE(d.find("in pad: dim = `1.5h`"), std::string::npos);
  EXPECT_NE(d.find("bind visible= (call (. a exists))"), std::string::npos);
  // invocation is a plain tag with verbatim args
  EXPECT_NE(d.find("widget member_cell"), std::string::npos);
  EXPECT_NE(d.find("attr col=`left`"), std::string::npos);
}

TEST(UicParse, StructuralIfChain) {
  const std::string d = dumpOf(
      "template slot {\n"
      "    in a: SlotState;\n"
      "    panel {\n"
      "        if (a.kind() == Kind.Small || !a.exists()) {\n"
      "            small_face { a: a; }\n"
      "        } else if (a.kind() == Kind.Wide) {\n"
      "            wide_face { a: a; }\n"
      "        } else {\n"
      "            tall_face { a: a; }\n"
      "        }\n"
      "    }\n"
      "}\n");
  EXPECT_NE(d.find("if (|| (== (call (. a kind)) (. Kind Small)) "
                   "(! (call (. a exists))))"),
            std::string::npos);
  EXPECT_NE(d.find("elif (== (call (. a kind)) (. Kind Wide))"),
            std::string::npos);
  EXPECT_NE(d.find("else\n"), std::string::npos);
  EXPECT_NE(d.find("widget tall_face"), std::string::npos);
}

TEST(UicParse, WidgetstateBindStateAction) {
  const std::string d = dumpOf(
      "button {\n"
      "    color: invisible;\n"
      "    action click: host.Primary(slot);\n"
      "    action rightclick: host.Secondary(slot);\n"
      "    bind state: !enabled(a) ? disabled : up;\n"
      "    widgetstate up { image { texture: /art/b_up.img; } }\n"
      "    widgetstate disabled { image { texture: /art/b_dis.img; } }\n"
      "}\n");
  EXPECT_NE(d.find("action click= (call (. host Primary) slot)"),
            std::string::npos);
  EXPECT_NE(d.find("bind state= (?: (! (call enabled a)) disabled up)"),
            std::string::npos);
  EXPECT_NE(d.find("widgetstate up"), std::string::npos);
  EXPECT_NE(d.find("widgetstate disabled"), std::string::npos);
}

TEST(UicParse, BindAndActionAreAttributesWhenColonFollows) {
  // ported trees carry an attribute literally NAMED `bind` (the source
  // material's key-binding attr); the keyword form needs a target ident
  const std::string d = dumpOf(
      "panel {\n"
      "    key_plate { bind: PrevUnit; keytype: general; }\n"
      "    bind visible: shown;\n"
      "}\n");
  EXPECT_NE(d.find("attr bind=`PrevUnit`"), std::string::npos);
  EXPECT_NE(d.find("bind visible= shown"), std::string::npos);
}

TEST(UicParse, RawValuesAreQuoteAware) {
  // text is the one quoted thing and may contain anything — a ';'
  // inside quotes must not terminate the value
  const std::string d = dumpOf(
      "panel { legacy: \"First(); Second();\"; width: 2h; }\n");
  EXPECT_NE(d.find("attr legacy=`\"First(); Second();\"`"),
            std::string::npos);
  EXPECT_NE(d.find("attr width=`2h`"), std::string::npos);
}

TEST(UicParse, StrayTokensNeverStallTheModuleLoop) {
  std::vector<uic::Diag> diags;
  const uic::Module m =
      parse("}\n) ;\nstyle ok { align: center; }\n", &diags);
  EXPECT_FALSE(diags.empty());
  ASSERT_EQ(m.styles.size(), 1u); // recovery reached the real item
}

TEST(UicParse, MissingSemiIsDiagnosedValueCannotSpanLines) {
  std::vector<uic::Diag> diags;
  const uic::Module m = parse("panel {\n    width: 7h\n    height: 3h;\n}\n",
                              &diags);
  ASSERT_FALSE(diags.empty());
  EXPECT_NE(diags[0].msg.find("missing ';'"), std::string::npos);
  // recovery kept parsing: the panel and the second attr still exist
  ASSERT_EQ(m.roots.size(), 1u);
}

TEST(UicParse, RecoveryProducesMultipleDiagsAndLaterItems) {
  std::vector<uic::Diag> diags;
  const uic::Module m = parse(
      "template broken {\n"
      "    in : bad;\n"
      "    panel { width: 1h; }\n"
      "}\n"
      "style fine { align: center; }\n",
      &diags);
  EXPECT_FALSE(diags.empty());
  ASSERT_EQ(m.styles.size(), 1u); // parsing continued past the error
  EXPECT_EQ(m.styles[0].name, "fine");
}

TEST(UicParse, DiagCarriesFileLineCol) {
  std::vector<uic::Diag> diags;
  parse("panel {\n    ?bad\n}\n", &diags);
  ASSERT_FALSE(diags.empty());
  EXPECT_EQ(diags[0].file, "test.ui");
  EXPECT_EQ(diags[0].line, 2);
  EXPECT_GT(diags[0].col, 0);
}

} // namespace
