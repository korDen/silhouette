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

TEST(UicEmit, RenderModeBecomesABlendFlag) {
  // rendermode overlay/additive/grayscale -> the sink blend/grayscale flags.
  // A frame carries the flag on every 9-slice piece; an image on its draw.
  // Without it a tinted overlay renders as a flat fill, not a composite.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 10h; height: 3h;\n"
      "    frame { texture: /art/hl.img; rendermode: overlay;\n"
      "            borderthickness: 0.2h; }\n"
      "    image { texture: /art/i.img; rendermode: additive; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("ui::kBlendOverlay"), std::string::npos); // the frame
  EXPECT_NE(h.find("ui::kBlendAdditive"), std::string::npos); // the image
}

TEST(UicEmit, AlignAndDeltaSizes) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 100h; height: 20h;\n"
      "    panel { width: -2; height: -2; align: center; valign: bottom;\n"
      "            color: rgba(0, 0, 0, .85); }\n"
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
      "panel { listbox { color: white; } image { texture: /art/a.img; } }\n",
      &diags);
  ASSERT_FALSE(diags.empty());
  EXPECT_EQ(diags[0].severity, uic::Diag::Severity::kWarning);
  EXPECT_FALSE(uic::hasErrors(diags));
  EXPECT_NE(diags[0].msg.find("skipped (unsupported yet)"),
            std::string::npos);
  EXPECT_NE(h.find("// SKIP(unsupported): widget 'listbox'"),
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

TEST(UicEmit, FloatChainFollowsTheTargetRect) {
  // the position law: a chain child's
  // main axis advances from the previous TARGET's far edge; the cross
  // axis reads the target's rect; the chain's first child positions
  // plainly; and a child with ANY explicit x/y opts OUT of the chain
  // while still becoming the next target
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 4h; float: right; padding: .5h;\n"
      "    image { texture: /art/a.img; width: 2h; }\n"
      "    image { texture: /art/b.img; width: 3h; }\n"
      "    image { texture: /art/c.img; width: 3h; x: 0; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("hasT0 = 0;"), std::string::npos);
  EXPECT_NE(h.find("x1 = R(tx0 + tw0 + R("), std::string::npos); // chain
  EXPECT_NE(h.find("y1 = std::floor(ty0);"), std::string::npos); // cross
  EXPECT_NE(h.find("tx0 = x1; ty0 = y1; tw0 = w1; th0 = h1; hasT0 = 1;"),
            std::string::npos);
  // the x: 0 child does NOT chain (no tx0-based x3) but IS a target
  EXPECT_EQ(h.find("x3 = R(tx0"), std::string::npos);
  EXPECT_NE(h.find("tx0 = x3;"), std::string::npos);
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
  // pass loop with the four-edged union gated to pass 0: the child's
  // extent is offset by the current origin shift before uniting
  EXPECT_NE(h.find("for (int pass0 = 0; pass0 < 2; ++pass0)"),
            std::string::npos);
  EXPECT_NE(h.find("if (pass0 == 0) { const float nx1 = lox0 + x1, "
                   "ny1 = loy0 + y1; "
                   "hix0 = std::max(hix0, nx1 + w1); "
                   "hiy0 = std::max(hiy0, ny1 + h1); "
                   "lox0 = std::min(lox0, nx1); "
                   "loy0 = std::min(loy0, ny1); "
                   "w0 = hix0 - lox0; h0 = hiy0 - loy0; }"),
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

TEST(UicEmit, StylesResolveTheirInheritanceChain) {
  // a style with its own `style:` base inherits the base's attrs: the
  // derived style overrides what it names (font), the rest flows through
  // (shadow). Without following the chain, a text_label_small label loses
  // text_base's shadow.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "style base_s { font: bigfont; shadow: 1; shadowcolor: rgb(0, 0, 0); }\n"
      "style small_s { style: base_s; font: smallfont; }\n"
      "panel { label { style: small_s; content: \"-\"; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("smallfont"), std::string::npos); // derived font wins
  // the base's shadow flowed through: emitLabel draws a shadow pass under
  // the run, so there are two sink.text( calls, not one
  size_t n = 0, p = 0;
  while ((p = h.find("sink.text(", p)) != std::string::npos) {
    ++n;
    p += 9;
  }
  EXPECT_GE(n, 2u) << "shadow pass missing — base style not inherited";
}

TEST(UicEmit, TemplatesInstantiateWithFoldedParams) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "template cell {\n"
      "    in size: dim = 2h;\n"
      "    in art: asset;\n"
      "    image { texture: art; width: size; height: size; }\n"
      "}\n"
      "template pair {\n"
      "    in art: asset;\n"
      "    panel { grow: 1; float: right;\n"
      "        cell { art: art; }\n"
      "        cell { art: art; size: 3h; }\n"
      "    }\n"
      "}\n"
      "panel { pair { art: /art/a.img; } pair { art: /art/b.img; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // four cell INSTANCES (nested templates, args referencing outer
  // params), each with its own rect variables and folded values
  size_t n = 0, pos = 0;
  while ((pos = h.find("/art/a.img */", pos)) != std::string::npos) {
    ++n;
    ++pos;
  }
  EXPECT_EQ(n, 2u); // two cells of pair #1 draw art a
  EXPECT_NE(h.find("/art/b.img"), std::string::npos);
  EXPECT_NE(h.find("R(0.0299"), std::string::npos); // the 3h override
  EXPECT_NE(h.find(">>> pair"), std::string::npos); // provenance markers
}

TEST(UicEmit, InstantiationDiagnostics) {
  std::vector<uic::Diag> diags;
  emit("template t { in a: num; panel { width: a; } }\n"
       "panel { t { b: 1; } }\n",
       &diags);
  ASSERT_GE(diags.size(), 2u);
  EXPECT_NE(diags[0].msg.find("no param 'b'"), std::string::npos);
  EXPECT_NE(diags[1].msg.find("no argument and no default"),
            std::string::npos);
  // (a third warning follows: the empty substituted dim — also loud)
  EXPECT_FALSE(uic::hasErrors(diags)); // degradation, not failure
}

TEST(UicEmit, ArgsAreTypeChecked) {
  std::vector<uic::Diag> diags;
  emit("template t { in first: bool; in slot: num; in label: str;\n"
       "    panel { visible: first; } }\n"
       "template mid { in outer: num;\n"
       "    panel { t { first: true; slot: outer; label: word_ok; } } }\n"
       "panel {\n"
       "    t { first: 1; slot: \"nope\"; label: \"quoted ok\"; }\n"
       "    mid { outer: 3; }\n"
       "}\n",
       &diags);
  // the two bad args error; the good instantiations (bool literal,
  // same-typed param ref, bare word into str, quoted str) are silent
  int errors = 0;
  for (const uic::Diag &d : diags) {
    if (d.severity == uic::Diag::Severity::kError) {
      ++errors;
      EXPECT_TRUE(d.msg.find("is bool, got num '1'") != std::string::npos ||
                  d.msg.find("is num, got str") != std::string::npos)
          << d.msg;
    }
  }
  EXPECT_EQ(errors, 2);
}

TEST(UicEmit, BoolParamsFoldIntoVisibility) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "template cap { in first: bool = false;\n"
      "    image { texture: /art/cap.img; width: 2h; height: 2h;\n"
      "            visible: first; } }\n"
      "panel { width: 10h; height: 2h;\n"
      "    cap { first: true; }\n"
      "    cap { }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // first instance draws; the defaulted (false) one is a hidden subtree
  EXPECT_NE(h.find("/art/cap.img"), std::string::npos);
  EXPECT_NE(h.find("hidden subtree"), std::string::npos);
}

TEST(UicEmit, GrowUnionIsFourEdged) {
  // the union extends ALL edges — a bottom-valigned child in a 0-floor grow
  // parent lands at a negative offset in pass 0, pulls the origin (lo
  // marks), the parent sizes to hi - lo, and pass 1 repositions the
  // child inside the grown rect. A right/bottom-only max() leaves the
  // parent 0-tall and the child overflowing upward.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { grow: 1; float: right;\n"
      "    image { texture: /art/a.img; width: 2h; height: 2h;\n"
      "            valign: bottom; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("loy0 = std::min(loy0, ny1)"), std::string::npos);
  EXPECT_NE(h.find("h0 = hiy0 - loy0"), std::string::npos);
  // the accumulators reset from the explicit floor at pass 0
  EXPECT_NE(h.find("if (pass0 == 0) { lox0 = 0; loy0 = 0; hix0 = w0; "
                   "hiy0 = h0; }"),
            std::string::npos);
}

TEST(UicEmit, StructuralIfLowersToAbsenceGuards) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { grow: 1; growinvis: 1; float: right;\n"
      "    if (snapshot.stash.open) {\n"
      "        panel { width: 10h; height: 3h; }\n"
      "    } else {\n"
      "        panel { width: 5h; height: 3h; }\n"
      "    }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // absence, not invisibility: the guard gates the union even under
  // growinvis=1, gates the chain advance, and gates the draws — arm 2
  // is the negation of arm 1
  EXPECT_NE(h.find("pass0 == 0 && (s.stash.open)"), std::string::npos);
  EXPECT_NE(h.find("pass0 == 0 && (!(s.stash.open))"), std::string::npos);
  EXPECT_NE(h.find("if (s.stash.open) { tx0 = x1;"), std::string::npos);
  EXPECT_NE(h.find("if (!(s.stash.open)) { tx0 = x2;"),
            std::string::npos);
  EXPECT_NE(h.find("if (s.stash.open) {\n"), std::string::npos); // draw gate
  EXPECT_EQ(h.find("SKIP"), std::string::npos); // nothing degraded
}

TEST(UicEmit, LabelsDrawRunsAndTheManifestNamesTheirFonts) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 4h;\n"
      "    label { content: \"1998\"; font: dyn_bold_11; color: rgb(1, 1, 1);\n"
      "            textalign: center; textvalign: center; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // the font is a host resource named by the module, like a texture
  EXPECT_NE(h.find("FontRef kpanel_fonts[] = {"), std::string::npos);
  EXPECT_NE(h.find("\"dyn_bold_11\"},"), std::string::npos);
  // the run: measured by the SINK (layout and ink cannot disagree),
  // centered by the ceil law, pen on the baseline
  EXPECT_NE(h.find("const float tw1 = sink.measure("), std::string::npos);
  EXPECT_NE(h.find("std::ceil((w1 - tw1) / 2)"), std::string::npos);
  EXPECT_NE(h.find("std::ceil((h1 - lh1) / 2)"), std::string::npos);
  EXPECT_NE(h.find("+ sink.ascent("), std::string::npos);
  // a static content binds to a src view exactly like num()/fmt() — one
  // measure-and-draw path, no literal threaded through the sink call
  EXPECT_NE(h.find("src1 = std::string_view(\"1998\")"), std::string::npos);
  EXPECT_NE(h.find("sink.text(pen1, gen_detail::sv(src1),"),
            std::string::npos);
}

TEST(UicEmit, FitxLabelsMeasureInTheSolve) {
  // the fitx solve: a fitx label's WIDTH is its text —
  // layout asks the sink, so the solve carries the measure
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 4h; grow: 1; float: right;\n"
      "    label { content: \"43861\"; font: dyn_bold_11; fitx: 1;\n"
      "            fitxpadding: 0.5h; }\n"
      "    image { texture: /art/star.img; width: 2h; height: 2h; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("w1 = sink.measure("), std::string::npos);
  EXPECT_NE(h.find("w1 += "), std::string::npos); // fitxpadding
  // and the measured width feeds the chain: the icon follows the text
  EXPECT_NE(h.find("tx0 = x1; ty0 = y1; tw0 = w1;"), std::string::npos);
}

TEST(UicEmit, NumbersRenderIntoRunsWithoutInterpolation) {
  // the LANGUAGE has no string interpolation; num()/fixed() are the
  // compiler's typed conversions at a text sink, and their buffer is a
  // local value the run borrows (no allocation — the cost contract)
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 4h;\n"
      "    label { bind content: num(snapshot.player.score); font: dyn_9; }\n"
      "    label { bind content: fixed(snapshot.unit.hpRegen, 1);\n"
      "            font: dyn_9; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("const auto src1 = gen_detail::num((long long)(s.player.score))"),
            std::string::npos);
  EXPECT_NE(h.find("gen_detail::fixed((double)(s.unit.hpRegen), (int)(1))"),
            std::string::npos);
  EXPECT_NE(h.find("sink.text(pen1, gen_detail::sv(src1),"),
            std::string::npos);
}

TEST(UicEmit, FmtComposesARunFromAFormatAndArgs) {
  // the composed-run answer: fmt("{} / {}", a, b) fills each hole with
  // the next arg. Still no interpolation in the LANGUAGE — a call whose
  // result IS the run, its buffer a local.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 4h;\n"
      "    label { bind content: fmt(\"{} / {}\", snapshot.unit.hp,\n"
      "                              snapshot.unit.hpMax); font: dyn_9; }\n"
      "    label { bind content: fmt(\"+{}\", snapshot.unit.hpRegen);\n"
      "            font: dyn_9; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("gen_detail::fmt(\"{} / {}\", (s.unit.hp), (s.unit.hpMax))"),
            std::string::npos);
  EXPECT_NE(h.find("gen_detail::fmt(\"+{}\", (s.unit.hpRegen))"),
            std::string::npos);
  EXPECT_NE(h.find("const auto src1 = gen_detail::fmt("), std::string::npos);
  EXPECT_NE(h.find("sink.text(pen1, gen_detail::sv(src1),"),
            std::string::npos);
}

TEST(UicEmit, OutlinedLabelsPreferTheStroker) {
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { label { content: \"0s\"; font: dyn_bold_30; outline: 1;\n"
      "                outlinecolor: rgba(0, 0, 0, .6); } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("if (sink.outline_width("), std::string::npos);
  EXPECT_NE(h.find("sink.text_stroked(pen1,"), std::string::npos);
  EXPECT_NE(h.find("0.600000024f}"), std::string::npos); // the outline color
}

TEST(UicEmit, AnUnfontedUncoloredLabelIsGray) {
  // an untextured widget with no color is GRAY
  std::vector<uic::Diag> diags;
  const std::string h = emit("panel { label { content: \"-\"; } }\n", &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("\"system_medium\"},"), std::string::npos); // the default
  EXPECT_NE(h.find("ui::Color{0.5f, 0.5f, 0.5f, 1.0f}"), std::string::npos);
}

TEST(UicEmit, AColourBindPaintsFromAParamOrAColourLiteral) {
  // a bound colour: `color` (a colour-typed param) folds to the instance's
  // ui::Color, rgb(r,g,b) builds one from expressions (the only way to spell
  // a colour inside a bind — a bare "r g b a" is not one expression), and
  // the label's paint reads the whole ternary, not a static grey. The bare
  // `color` is the param; `rgb(...)` is the builtin — no collision.
  std::vector<uic::Diag> diags;
  const std::string h =
      emit("template chip { in color: color = white; in kind: warm | cool;\n"
           "    label { content: \"-\";\n"
           "        bind color: kind == warm ? color : rgb(0.9, 0.9, 0.9); } }\n"
           "panel { chip { color: rgb(1, 0.37, 0.34); kind: warm; } }\n",
           &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // the label's text paint is the whole ternary: the condition lowered to
  // sids, `color` folded to the instance's ui::Color, color(...) built from
  // exprs — and NOT the static unbound grey
  EXPECT_NE(h.find("(WARM == WARM) ? ui::Color{1.0f, "), std::string::npos);
  EXPECT_NE(h.find(": ui::Color{(float)(0.9), (float)(0.9), (float)(0.9), "
                   "(float)(1)}"),
            std::string::npos);
  EXPECT_EQ(h.find("ui::Color{0.5f, 0.5f, 0.5f, 1.0f}"), std::string::npos);
}

TEST(UicEmit, MatchOverAnEnumPicksAndValidatesEachArm) {
  // the interpolated-asset answer: an enum param + a match with every
  // path concrete. Each instantiation's folded value picks one arm; the
  // arg is checked against the enum. (Existence needs --assets; here we
  // pin the selection + the enum arg check.)
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "template slot { in shape: round | square;\n"
      "    image { texture: match shape {\n"
      "        round: /art/round_frame.tga;\n"
      "        square: /art/square_frame.tga;\n"
      "    } } }\n"
      "panel { slot { shape: round; } slot { shape: square; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("/art/round_frame.tga"), std::string::npos);
  EXPECT_NE(h.find("/art/square_frame.tga"), std::string::npos);
  EXPECT_EQ(h.find("match"), std::string::npos); // resolved, not emitted
}

TEST(UicEmit, AMatchInABindLowersToAConvertedTernary) {
  // `bind content: num = match kind { a: e0, b: e1, c: e2 }` — a readable
  // multi-way value. It lowers to num(kind == a ? e0 : kind == b ? e1 :
  // e2): the scrutinee folds to this instance's value, each arm value is
  // validated against the enum, and the LAST arm is the else.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "template chip { in kind: a | b | c;\n"
      "    label {\n"
      "        bind content: num = match kind {\n"
      "            a: snapshot.unit.low,\n"
      "            b: snapshot.unit.mid,\n"
      "            c: snapshot.unit.high }; } }\n"
      "panel { chip { kind: a; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("gen_detail::num("), std::string::npos);   // the conv
  EXPECT_NE(h.find("(A == A)"), std::string::npos);           // first arm
  EXPECT_NE(h.find("(A == B)"), std::string::npos);           // second arm
  EXPECT_NE(h.find("s.unit.high"), std::string::npos); // else, no cond
  EXPECT_EQ(h.find("A == C"), std::string::npos); // the else has no compare
}

TEST(UicEmit, AMatchOverANumberNeedsNoConversion) {
  // match works over a plain (non-enum) scrutinee: its values are literals
  // compared as written, and with no conversion the arms ARE the result (a
  // texture id here) — the else-chain a multi-branch bind would otherwise be.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "template slot { in kind: num;\n"
      "    image { bind texture: match kind {\n"
      "        0: /art/a.img, 1: /art/b.img, 2: /art/c.img }; } }\n"
      "panel { slot { kind: 1; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("(1) == 0"), std::string::npos);       // kind folds to 1
  EXPECT_NE(h.find("(1) == 1"), std::string::npos);       // compared as written
  EXPECT_EQ(h.find("gen_detail::num"), std::string::npos); // no conversion
}

TEST(UicEmit, AnInlineEnumComparisonLowersToNamedIds) {
  // an inline-enum param and a bare value both become a named UPPER id
  // constant in a bind — a uint32_t compare that folds at compile time (no
  // shared type). The generated file DEFINES the constant it uses (a
  // param-only enum, round|square here, has no schema home), so it compiles
  // stand-alone with the id folded to its literal.
  std::vector<uic::Diag> diags;
  const std::string h = emit("template slot { in shape: round | square;\n"
                             "    image { texture: /art/x.tga;\n"
                             "        bind visible: shape == square; } }\n"
                             "panel { slot { shape: square; } }\n",
                             &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("SQUARE == SQUARE"), std::string::npos);
  EXPECT_NE(h.find("inline constexpr uint32_t SQUARE = 0x"), std::string::npos);
}

TEST(UicEmit, ATextTernaryCoercesEachBranchToAView) {
  // a `content` value that is a ternary can mix text types with no common
  // C++ type — a fixed char[N] field and a str param, say — so each RESULT
  // is coerced to a string_view and the branches then unify. sv() is
  // idempotent, so textExpr's own outer sv() over the whole run still holds;
  // a non-ternary content is left for that outer sv() alone.
  std::vector<uic::Diag> diags;
  const std::string h =
      emit("template plate { in gate: on | off; in text: str;\n"
           "    label { bind content: gate == on ? snapshot.unit.name "
           ": text; } }\n"
           "panel { plate { gate: on; text: \"-\"; } }\n",
           &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // each result is wrapped, not the ternary as a whole
  EXPECT_NE(h.find("? gen_detail::sv(s.unit.name) : gen_detail::sv("),
            std::string::npos);
}

TEST(UicEmit, ComparingTwoDifferentEnumSetsIsLoud) {
  // both sides are uint32_t at runtime, but the .ui type-check rejects
  // comparing an {x,y} id to a {p,q} id (kept type-checked by design)
  std::vector<uic::Diag> diags;
  (void)emit("template t { in a: x | y; in b: p | q;\n"
             "    image { texture: /art/x.tga; bind visible: a == b; } }\n"
             "panel { t { a: x; b: p; } }\n",
             &diags);
  bool loud = false;
  for (const uic::Diag &d : diags) {
    loud = loud || d.msg.find("different enum sets") != std::string::npos;
  }
  EXPECT_TRUE(loud);
}

TEST(UicEmit, AMatchArgProjectsAFusedEnumBackToAComponent) {
  // a two-param hole fuses to one enum; a child that
  // wants just one of them gets it decoupled with a match ARG — the fusion
  // is never a dead end (by design)
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "template face { in tone: light | dark;\n"
      "    image { texture: match tone {\n"
      "        light: /art/light.tga;\n"
      "        dark: /art/dark.tga;\n"
      "    } } }\n"
      "template btn { in look: light_up | dark_over;\n"
      "    face { tone: match look {\n"
      "        light_up: light;\n"
      "        dark_over: dark;\n"
      "    } } }\n"
      "panel { btn { look: dark_over; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // look=dark_over -> tone=dark -> the dark face texture
  EXPECT_NE(h.find("/art/dark.tga"), std::string::npos);
  EXPECT_EQ(h.find("/art/light.tga"), std::string::npos);
  EXPECT_EQ(h.find("match"), std::string::npos); // both resolved
}

TEST(UicEmit, AMatchOnANonAssetAttrIsNotFileChecked) {
  // a `style:` match resolves to a style NAME, not a file — the asset
  // existence check must NOT fire (those bare names are no paths). The
  // arm's domain (a style decl) is validated by whoever owns styles.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "style round_frame_mask { color: rgb(1, 1, 1); }\n"
      "style square_frame_mask { color: rgb(0, 0, 0); }\n"
      "template slot { in shape: round | square;\n"
      "    image { texture: /art/x.tga; style: match shape {\n"
      "        round: round_frame_mask;\n"
      "        square: square_frame_mask;\n"
      "    } } }\n"
      "panel { slot { shape: square; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_EQ(h.find("match"), std::string::npos); // resolved to square's style
}

TEST(UicEmit, AnEnumArgOutsideItsSetIsLoud) {
  std::vector<uic::Diag> diags;
  emit("template slot { in shape: round | square;\n"
       "    image { texture: match shape { round: /a.tga; square: /b.tga; } } }\n"
       "panel { slot { shape: hexagon; } }\n",
       &diags);
  ASSERT_FALSE(diags.empty());
  EXPECT_TRUE(uic::hasErrors(diags));
  EXPECT_NE(diags[0].msg.find("not one of: round | square"),
            std::string::npos);
}

TEST(UicEmit, AnInterpolationHoleInAnAssetIsAHardError) {
  // holes must never reach the compiler — a converter's asset-match pass
  // turns each into a match over an enum param first
  std::vector<uic::Diag> diags;
  emit("template slot { in shape: ident;\n"
       "    image { texture: /art/{shape}_up.tga; } }\n"
       "panel { slot { shape: round; } }\n",
       &diags);
  ASSERT_FALSE(diags.empty());
  EXPECT_TRUE(uic::hasErrors(diags));
  EXPECT_NE(diags[0].msg.find("interpolation hole in asset"),
            std::string::npos);
}

TEST(UicEmit, ATexturelessFrameDrawsItsBorder) {
  // a frame with no texture but a bordercolor is a plain outline: four
  // edge quads of borderthickness
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 20h; height: 20h;\n"
      "    frame { color: invisible; borderthickness: 1;\n"
      "            bordercolor: rgb(0, 0, 0); } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_EQ(h.find("SKIP"), std::string::npos); // not skipped
  // top edge, then bottom/left/right — all in the border colour
  EXPECT_NE(h.find("sink.quad({ax1, ay1, w1, bth1}, "
                   "ui::Color{0.0f, 0.0f, 0.0f, 1.0f}"),
            std::string::npos);
  EXPECT_NE(h.find("ay1 + h1 - bth1"), std::string::npos); // bottom
  EXPECT_NE(h.find("ax1 + w1 - bth1"), std::string::npos); // right
}

TEST(UicEmit, AlphaMasksCutTheirWidgetToShape) {
  // usealphamask + alphamaskfile = the shape a widget's ink is cut to
  // (the sink's mask arg). $white + a mask is a stencil — the mask alone
  // defines the shape, the fill is pure colour.
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "style square_mask { usealphamask: 1;\n"
      "                  alphamaskfile: /ui/mask/square_cutout.tga; }\n"
      "panel { width: 4h; height: 4h;\n"
      "    image { texture: $white; style: square_mask; color: rgb(1, 0, 0); }\n"
      "    image { texture: /art/icon.img; style: square_mask; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  // the stencil: texture 0 (the white texel) cut by the mask, NOT a
  // bare quad
  EXPECT_NE(h.find("sink.image({ax1, ay1, w1, h1}, 0, {0, 0, 1, 1}, "
                   "ui::Color{1.0f, 0.0f, 0.0f, 1.0f}, 0, 0x"),
            std::string::npos);
  EXPECT_EQ(h.find("sink.quad"), std::string::npos);
  // a real texture wears the same mask
  EXPECT_NE(h.find("/ui/mask/square_cutout.tga */, ui::kNoClip);"),
            std::string::npos);
}

TEST(UicEmit, TheDialectsSolidTexelsAreFillsNotArt) {
  // $white / $black are the dialect's solid texels — fills, never
  // manifest entries (a host cannot load "$black" from its tree)
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 4h; height: 4h;\n"
      "    image { texture: $black; }\n"
      "    image { texture: $white; }\n"
      "    image { texture: $invis; }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_EQ(h.find("\"$black\""), std::string::npos);
  EXPECT_EQ(h.find("\"$white\""), std::string::npos);
  // unpainted: $black fills black, $white white, $invis nothing
  EXPECT_NE(h.find("sink.quad({ax1, ay1, w1, h1}, "
                   "ui::Color{0.0f, 0.0f, 0.0f, 1.0f}"),
            std::string::npos);
  EXPECT_NE(h.find("sink.quad({ax2, ay2, w2, h2}, "
                   "ui::Color{1.0f, 1.0f, 1.0f, 1.0f}"),
            std::string::npos);
  EXPECT_EQ(h.find("ax3, ay3"), std::string::npos);
}

TEST(UicEmit, AStyleNameCanComeFromAParam) {
  // a caller choosing this instance's look: the style NAME folds
  // before the lookup
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "style round_mask { usealphamask: 1;\n"
      "                  alphamaskfile: /ui/mask/round.tga; }\n"
      "style square_mask { usealphamask: 1;\n"
      "                  alphamaskfile: /ui/mask/square.tga; }\n"
      "template slot { in shape: ident = round_mask;\n"
      "    image { texture: $white; style: shape; } }\n"
      "panel { slot { } slot { shape: square_mask; } }\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("/ui/mask/round.tga"), std::string::npos);
  EXPECT_NE(h.find("/ui/mask/square.tga"), std::string::npos);
}

TEST(UicEmit, ButtonsRenderTheirRestingState) {
  // buttons are containers; only the 'up' widgetstate renders in the
  // static build, and states never union, chain, or become targets
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "panel { width: 40h; height: 4h;\n"
      "    button { grow: 1; float: right;\n"
      "        widgetstate up {\n"
      "            image { texture: /art/up.img; width: 2h; height: 2h; }\n"
      "        }\n"
      "        widgetstate over {\n"
      "            image { texture: /art/over.img; width: 2h; height: 2h; }\n"
      "        }\n"
      "        panel { width: 3h; height: 2h; color: rgb(1, 1, 1); }\n"
      "    }\n"
      "}\n",
      &diags);
  ASSERT_TRUE(diags.empty()) << diags[0].msg;
  EXPECT_NE(h.find("/art/up.img"), std::string::npos);
  EXPECT_EQ(h.find("/art/over.img"), std::string::npos);
  // the state layer (idx 2) neither unions nor targets; the plain
  // panel child does both
  EXPECT_EQ(h.find("nx2"), std::string::npos);
  EXPECT_EQ(h.find("tx1 = x2;"), std::string::npos);
  EXPECT_NE(h.find("tx1 = x4;"), std::string::npos);
}

TEST(UicEmit, EmptyAlignFoldsToLeft) {
  // a param-fed align with no argument folds to the empty string —
  // the default is LEFT, never right
  std::vector<uic::Diag> diags;
  const std::string h = emit(
      "template t { in a: ident;\n"
      "    panel { width: 2h; height: 2h; align: a; color: rgb(1, 1, 1); } }\n"
      "panel { width: 40h; height: 4h; t { } }\n",
      &diags);
  EXPECT_FALSE(uic::hasErrors(diags));
  EXPECT_NE(h.find("x1 = 0.0f + R(0.0f);"), std::string::npos);
  EXPECT_EQ(h.find("w0 - w1"), std::string::npos); // not right-aligned
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
  EXPECT_EQ(h.find("std::max(hix0"), std::string::npos);
  // stickytoinvis default 1: the first child still becomes the chain
  // target; the second (stickytoinvis: 0) does not
  EXPECT_NE(h.find("tx0 = x1;"), std::string::npos);
  EXPECT_EQ(h.find("tx0 = x2;"), std::string::npos);
}

} // namespace
