// uic emit pass tests — the absolute subset compiles to straight-line
// sink calls: unit-grammar folding, align arithmetic, nine-slice
// expansion, the uscale inversion law, snapshot binds, manifests, and
// loud diagnostics at the subset's edges.
#include "uic/uic.hpp"

#include <gtest/gtest.h>

namespace {

std::string emit(std::string_view src, std::vector<uic::Diag> *diags) {
  std::vector<uic::Diag> parseDiags;
  const uic::Module m = uic::parseModule(src, "panel.ui", parseDiags);
  EXPECT_TRUE(parseDiags.empty())
      << (parseDiags.empty() ? "" : parseDiags[0].msg);
  uic::EmitOptions opt;
  return uic::emitPanelHeader(m, opt, *diags);
}

TEST(UicEmit, UnitGrammarFoldsToArithmetic) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { x: 47.04h; y: 87.5h; width: 83.888h; height: 12.5h;\n"
      "    image { texture: /art/rail.img; y: -0.9h; width: 100%;\n"
      "            height: 1.45h; utile: 1; uscale: .35; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("R(0.125f * H)"), std::string::npos); // height: 12.5h
  EXPECT_NE(h.find("ui::kTileU"), std::string::npos);
}

TEST(UicEmit, TheUscaleInversionLaw) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { image { texture: /art/rail.img; utile: 1; uscale: .35; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty());
  // span = 1 / 0.35 = 2.857...
  EXPECT_NE(h.find("{0, 0, 2.85714293f, 1}"), std::string::npos);
}

TEST(UicEmit, AlignAndDeltaSizes) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 100h; height: 20h;\n"
      "    panel { width: -2; height: -2; align: center; valign: bottom;\n"
      "            color: 0 0 0 .85; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty());
  EXPECT_NE(h.find("w0 + (-2.0f)"), std::string::npos); // parent + value
  EXPECT_NE(h.find("R((w0 - w1) / 2)"), std::string::npos);
  EXPECT_NE(h.find("sink.quad("), std::string::npos);
}

TEST(UicEmit, NineSliceExpandsWithParentRelativeBorder) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 3h;\n"
      "    frame { texture: /art/bar.img; borderthickness: 0.2h; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty());
  for (const char *piece : {"/art/bar_tl.img", "/art/bar_c.img",
                            "/art/bar_br.img"}) {
    EXPECT_NE(h.find(piece), std::string::npos) << piece;
  }
  EXPECT_NE(h.find("std::min("), std::string::npos); // border clamp
}

TEST(UicEmit, SnapshotBindsTranspile) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 3h;\n"
      "    panel { color: #058d27;\n"
      "        bind width: fracf(snapshot.unit.hp, snapshot.unit.hpMax);\n"
      "        bind visible: snapshot.unit.hp > 0; }\n"
      "    image { bind texture: snapshot.unit.icon; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("gen_detail::fracf((float)(s.unit.hp), "
                   "(float)(s.unit.hpMax))"),
            std::string::npos);
  EXPECT_NE(h.find("if ((s.unit.hp > 0))"), std::string::npos);
  EXPECT_NE(h.find("sink.image({"), std::string::npos);
  EXPECT_NE(h.find("s.unit.icon"), std::string::npos);
}

TEST(UicEmit, ManifestCarriesIdsAndPaths) {
  std::vector<uic::Diag> diags;
  const std::string h =
      emit("panel { image { texture: /art/one.img; } }\n", &diags);
  ASSERT_TRUE(diags.empty());
  EXPECT_NE(h.find("inline constexpr TexRef kpanel_textures[]"),
            std::string::npos);
  EXPECT_NE(h.find("\"/art/one.img\""), std::string::npos);
}

TEST(UicEmit, OutsideTheSubsetDegradesLoudly) {
  // the functional bar: unsupported constructs WARN and skip — the rest
  // of the module still compiles and renders (never a hard stop)
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { button { color: white; } image { texture: /art/a.img; } }\n",
      &diags);
  ASSERT_FALSE(diags.empty());
  EXPECT_EQ(diags[0].severity, uic::Diag::Severity::kWarning);
  EXPECT_FALSE(uic::hasErrors(diags));
  EXPECT_NE(diags[0].msg.find("skipped (unsupported yet)"),
            std::string::npos);
  EXPECT_NE(h.find("// SKIP(unsupported): widget 'button'"),
            std::string::npos);
  EXPECT_NE(h.find("/art/a.img"), std::string::npos); // the rest emitted
}

TEST(UicEmit, VisibleZeroGatesDrawsNotLayout) {
  // the visible=0 adjudication: hidden subtrees SOLVE (occupancy per
  // growinvis/stickytoinvis) but draw nothing
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { panel { visible: 0; image { texture: /art/x.img; } } }\n",
      &diags);
  ASSERT_TRUE(diags.empty());
  EXPECT_EQ(h.find("/art/x.img"), std::string::npos); // no draw call
  EXPECT_NE(h.find("hidden subtree (visible: 0)"), std::string::npos);
  EXPECT_NE(h.find("w1 = "), std::string::npos); // still solved
}

TEST(UicEmit, FloatChainThreadsACursor) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 4h; float: right; padding: .5h;\n"
      "    image { texture: /art/a.img; width: 2h; }\n"
      "    image { texture: /art/b.img; width: 3h; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("cur0 = 0;"), std::string::npos);
  EXPECT_NE(h.find("x1 = cur0 + R(0.0f);"), std::string::npos);
  EXPECT_NE(h.find("cur0 = x1 + w1 + R("), std::string::npos);
  EXPECT_NE(h.find("x2 = cur0 + R(0.0f);"), std::string::npos);
}

TEST(UicEmit, GrowUnionsInAPassLoop) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { grow: 1; height: 4h;\n"
      "    image { texture: /art/a.img; width: 2h; height: 100%; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // the floor law: explicit height floors the union; missing width = 0
  EXPECT_NE(h.find("w0 = 0.0f;"), std::string::npos);
  EXPECT_NE(h.find("h0 = R(0.0399"), std::string::npos); // 4h floor
  // pass loop with unions gated to pass 0
  EXPECT_NE(h.find("for (int pass0 = 0; pass0 < 2; ++pass0)"),
            std::string::npos);
  EXPECT_NE(h.find("if (pass0 == 0) { w0 = std::max(w0, x1 + w1); h0 = "
                   "std::max(h0, y1 + h1); }"),
            std::string::npos);
}

TEST(UicEmit, StylesMergeWidgetWins) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "style grow_all { grow: 1; height: 9h; }\n"
      "panel { style: grow_all; height: 4h;\n"
      "    image { texture: /art/a.img; width: 2h; height: 2h; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // widget's height wins over the style's; the style's grow applies
  EXPECT_NE(h.find("h0 = R(0.0399"), std::string::npos); // 4h, not 9h
  EXPECT_EQ(h.find("R(0.0899"), std::string::npos);
  EXPECT_NE(h.find("for (int pass0"), std::string::npos);
}

TEST(UicEmit, HiddenChildOccupancyLaws) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { grow: 1; growinvis: 0; float: right;\n"
      "    image { texture: /art/a.img; width: 2h; height: 2h;\n"
      "            visible: 0; }\n"
      "    image { texture: /art/b.img; width: 3h; height: 2h;\n"
      "            visible: 0; stickytoinvis: 0; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // growinvis=0: neither hidden child unions
  EXPECT_EQ(h.find("std::max(w0"), std::string::npos);
  // stickytoinvis default 1: the first child still advances the chain;
  // the second (stickytoinvis: 0) does not
  EXPECT_NE(h.find("cur0 = x1 + w1"), std::string::npos);
  EXPECT_EQ(h.find("cur0 = x2 + w2"), std::string::npos);
}

} // namespace
