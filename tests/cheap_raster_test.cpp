// CheapRaster — exact-pixel pins for every primitive, every blend mode, and
// both texture modes. These values ARE the renderer's contract: the
// equivalence gate (pixel_match) is only as strong as the arithmetic pinned
// here, so a change to any constant below is a breaking change.
#include "render/cheap_raster.hpp"

#include <gtest/gtest.h>

#include <array>

namespace ui {
namespace {

std::array<int, 3> px(const CheapRaster& r, int x, int y) {
    const uint8_t* p = &r.pixels()[(static_cast<size_t>(y) * r.width() + x) * 4];
    return {p[0], p[1], p[2]};
}

constexpr Color kBlack{0, 0, 0, 1};
constexpr Color kWhite{1, 1, 1, 1};

// ---- frame ----------------------------------------------------------------

TEST(CheapRasterFrame, ClearFillsEverythingOpaque) {
    CheapRaster r;
    r.frame_begin(4, 3, Color{0.5f, 0.25f, 1, 1});
    r.frame_end();
    EXPECT_EQ(r.width(), 4u);
    EXPECT_EQ(r.height(), 3u);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(px(r, x, y), (std::array<int, 3>{128, 64, 255}));
    EXPECT_EQ(r.pixels()[3], 255); // canvas alpha is always opaque
}

TEST(CheapRasterFrame, ResizeBetweenFramesWorks) {
    CheapRaster r;
    r.frame_begin(2, 2, kWhite);
    r.frame_end();
    r.frame_begin(8, 4, kBlack);
    r.frame_end();
    EXPECT_EQ(r.width(), 8u);
    EXPECT_EQ(r.height(), 4u);
    EXPECT_EQ(px(r, 7, 3), (std::array<int, 3>{0, 0, 0}));
}

// ---- quad ------------------------------------------------------------------

TEST(CheapRasterQuad, PixelCenterCoverage) {
    CheapRaster r;
    r.frame_begin(8, 8, kBlack);
    // {2,2,4,4} spans centers 2.5..5.5 -> pixels 2..5 inclusive, no bleed.
    r.quad({2, 2, 4, 4}, kWhite, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 2, 2), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(r, 5, 5), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(r, 1, 2), (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(px(r, 6, 5), (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(px(r, 2, 1), (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(px(r, 5, 6), (std::array<int, 3>{0, 0, 0}));
}

TEST(CheapRasterQuad, SrcOverBlends) {
    CheapRaster r;
    r.frame_begin(2, 1, kBlack);
    r.quad({0, 0, 2, 1}, Color{1, 1, 1, 0.5f}, kNoClip); // 50% white on black
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{128, 128, 128})); // 127.5 rounds up
}

TEST(CheapRasterQuad, ClipCuts) {
    CheapRaster r;
    r.frame_begin(8, 2, kBlack);
    r.quad({0, 0, 8, 2}, kWhite, Rect{0, 0, 3, 2});
    r.frame_end();
    EXPECT_EQ(px(r, 2, 0), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(r, 3, 0), (std::array<int, 3>{0, 0, 0}));
}

TEST(CheapRasterQuad, OffscreenAndDegenerateAreNoops) {
    CheapRaster r;
    r.frame_begin(4, 4, kBlack);
    r.quad({-100, -100, 50, 50}, kWhite, kNoClip); // fully off-canvas
    r.quad({1, 1, 0, 4}, kWhite, kNoClip);         // zero width
    r.quad({1, 1, 4, -2}, kWhite, kNoClip);        // negative height
    r.frame_end();
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(px(r, x, y), (std::array<int, 3>{0, 0, 0}));
}

// ---- image: real texture mode ----------------------------------------------

// A 2x1 texture: red | blue, both opaque.
const uint8_t kRedBlue[8] = {255, 0, 0, 255, 0, 0, 255, 255};

TEST(CheapRasterImageReal, NearestSamplingSplitsAtTexelBoundary) {
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {kRedBlue, 2, 1});
    r.frame_begin(8, 1, kBlack);
    r.image({0, 0, 8, 1}, 1, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 0, 0}));
    EXPECT_EQ(px(r, 3, 0), (std::array<int, 3>{255, 0, 0})); // u=0.4375 -> texel 0
    EXPECT_EQ(px(r, 4, 0), (std::array<int, 3>{0, 0, 255})); // u=0.5625 -> texel 1
    EXPECT_EQ(px(r, 7, 0), (std::array<int, 3>{0, 0, 255}));
}

TEST(CheapRasterImageReal, UvSubrectSelects) {
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {kRedBlue, 2, 1});
    r.frame_begin(4, 1, kBlack);
    r.image({0, 0, 4, 1}, 1, {0.5f, 0, 0.5f, 1}, {}, 0, 0, kNoClip); // right half
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{0, 0, 255}));
    EXPECT_EQ(px(r, 3, 0), (std::array<int, 3>{0, 0, 255}));
}

TEST(CheapRasterImageReal, NegativeUvExtentFlips) {
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {kRedBlue, 2, 1});
    r.frame_begin(8, 1, kBlack);
    r.image({0, 0, 8, 1}, 1, {1, 0, -1, 1}, {}, 0, 0, kNoClip); // horizontal flip
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{0, 0, 255})); // blue now left
    EXPECT_EQ(px(r, 7, 0), (std::array<int, 3>{255, 0, 0}));
}

TEST(CheapRasterImageReal, TintModulates) {
    const uint8_t white[4] = {255, 255, 255, 255};
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {white, 1, 1});
    r.frame_begin(1, 1, kBlack);
    r.image({0, 0, 1, 1}, 1, {0, 0, 1, 1}, Color{1, 0, 0, 1}, 0, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 0, 0}));
}

TEST(CheapRasterImageReal, GrayscaleIsLumaBeforeTint) {
    const uint8_t red[4] = {255, 0, 0, 255};
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {red, 1, 1});
    r.frame_begin(2, 1, kBlack);
    r.image({0, 0, 1, 1}, 1, {0, 0, 1, 1}, {}, kGrayscale, 0, kNoClip);
    // luma first, THEN tint: gray 76 tinted blue keeps only the blue channel
    r.image({1, 0, 1, 1}, 1, {0, 0, 1, 1}, Color{0, 0, 1, 1}, kGrayscale, 0, kNoClip);
    r.frame_end();
    // 0.299 * 255 = 76.245 -> 76
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{76, 76, 76}));
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{0, 0, 76}));
}

TEST(CheapRasterImageReal, AdditiveAddsAndClamps) {
    const uint8_t red[4] = {255, 0, 0, 255};
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {red, 1, 1});
    r.frame_begin(1, 1, Color{0.5f, 0.5f, 0.5f, 1}); // dst = 128
    r.image({0, 0, 1, 1}, 1, {0, 0, 1, 1}, {}, kBlendAdditive, 0, kNoClip);
    r.frame_end();
    // r: 0.50196 + 1.0 clamps to 255; g/b unchanged
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 128, 128}));
}

TEST(CheapRasterImageReal, OverlayIsTwiceDstTimesSrc) {
    const uint8_t half[4] = {128, 128, 128, 255};
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {half, 1, 1});
    r.frame_begin(2, 1, Color{0.5f, 0.5f, 0.5f, 1});
    r.image({0, 0, 1, 1}, 1, {0, 0, 1, 1}, {}, kBlendOverlay, 0, kNoClip);
    // black dst stays black no matter the src: 2*0*src = 0
    r.quad({1, 0, 1, 1}, kBlack, kNoClip);
    r.image({1, 0, 1, 1}, 1, {0, 0, 1, 1}, {}, kBlendOverlay, 0, kNoClip);
    r.frame_end();
    // 2 * (128/255)^2 * 255 = 128.5 -> 129
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{129, 129, 129}));
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{0, 0, 0}));
}

TEST(CheapRasterImageReal, TileUWrapsClampDoesNot) {
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {kRedBlue, 2, 1});
    r.frame_begin(8, 2, kBlack);
    // uv spans two texture widths: tiled repeats r,r,b,b,r,r,b,b ...
    r.image({0, 0, 8, 1}, 1, {0, 0, 2, 1}, {}, kTileU, 0, kNoClip);
    // ... unclamped it saturates at the last texel past u=1
    r.image({0, 1, 8, 1}, 1, {0, 0, 2, 1}, {}, 0, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 0, 0}));
    EXPECT_EQ(px(r, 2, 0), (std::array<int, 3>{0, 0, 255}));
    EXPECT_EQ(px(r, 4, 0), (std::array<int, 3>{255, 0, 0})); // wrapped
    EXPECT_EQ(px(r, 6, 0), (std::array<int, 3>{0, 0, 255}));
    EXPECT_EQ(px(r, 4, 1), (std::array<int, 3>{0, 0, 255})); // clamped
    EXPECT_EQ(px(r, 6, 1), (std::array<int, 3>{0, 0, 255}));
}

TEST(CheapRasterImageReal, MaskSamplesAcrossDstIndependentOfColorUv) {
    // Mask 2x1: opaque | transparent. Color uv picks ONLY the blue half, so a
    // mask that followed the color uv would be all-opaque; the dst-fraction
    // rule cuts the right half regardless.
    const uint8_t maskTex[8] = {255, 255, 255, 255, 255, 255, 255, 0};
    CheapRaster r(TextureMode::kReal);
    r.set_texture(1, {kRedBlue, 2, 1});
    r.set_texture(2, {maskTex, 2, 1});
    r.frame_begin(8, 1, kBlack);
    r.image({0, 0, 8, 1}, 1, {0.5f, 0, 0.5f, 1}, {}, 0, 2, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{0, 0, 255})); // left: blue shows
    EXPECT_EQ(px(r, 6, 0), (std::array<int, 3>{0, 0, 0}));   // right: masked out
}

TEST(CheapRasterImageReal, UnregisteredTextureIsLoudMagenta) {
    CheapRaster r(TextureMode::kReal);
    r.frame_begin(2, 1, kBlack);
    r.image({0, 0, 2, 1}, 42, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 0, 255}));
}

// ---- image: synthetic texture mode ------------------------------------------

TEST(CheapRasterImageSynthetic, DeterministicAcrossRenders) {
    auto render = [] {
        CheapRaster r(TextureMode::kSynthetic);
        r.frame_begin(16, 16, kBlack);
        r.image({1, 1, 12, 12}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 16 * 16 * 4);
    };
    EXPECT_EQ(render(), render());
}

TEST(CheapRasterImageSynthetic, TextureIdChangesPixels) {
    auto render = [](TextureId id) {
        CheapRaster r(TextureMode::kSynthetic);
        r.frame_begin(8, 8, kBlack);
        r.image({0, 0, 8, 8}, id, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 8 * 8 * 4);
    };
    EXPECT_NE(render(7), render(8));
}

TEST(CheapRasterImageSynthetic, NeverReadsRegisteredTexture) {
    // A garbage, non-null pointer: any read would crash. Synthetic mode must
    // never dereference it.
    CheapRaster r(TextureMode::kSynthetic);
    r.set_texture(1, {reinterpret_cast<const uint8_t*>(0x1), 64, 64});
    r.frame_begin(8, 8, kBlack);
    r.image({0, 0, 8, 8}, 1, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
    r.image({0, 0, 8, 8}, 1, {0, 0, 1, 1}, {}, 0, /*mask=*/1, kNoClip);
    r.frame_end();
    SUCCEED(); // reaching here IS the assertion
}

TEST(CheapRasterImageSynthetic, OverdrawRemainsVisible) {
    // Synthetic alpha lives in [64,192], never opaque — so drawing the same
    // image twice composites differently than once. A duplicated (or an
    // extra hidden) command can never be masked by what is on top of it.
    auto render = [](int copies) {
        CheapRaster r(TextureMode::kSynthetic);
        r.frame_begin(8, 8, kBlack);
        for (int i = 0; i < copies; ++i)
            r.image({0, 0, 8, 8}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 8 * 8 * 4);
    };
    EXPECT_NE(render(1), render(2));
}

TEST(CheapRasterImageSynthetic, FlagsStillShapeTheOutput) {
    // The full flag pipeline (gray/tint/blend/tile) runs on synthesized
    // texels, so a flag difference is caught with no texture data at all.
    // The setup must actually exercise the flag: additive is identical to
    // src-over exactly on a black dst (dst=0 makes both src·a), so the clear
    // is gray; tiling only differs once the UV leaves the unit square, so
    // the UV spans two widths.
    auto render = [](Rect uv, unsigned int flags) {
        CheapRaster r(TextureMode::kSynthetic);
        r.frame_begin(8, 8, Color{0.25f, 0.25f, 0.25f, 1});
        r.image({0, 0, 8, 8}, 7, uv, {}, flags, 0, kNoClip);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 8 * 8 * 4);
    };
    const Rect unit{0, 0, 1, 1}, wide{0, 0, 2, 1};
    EXPECT_NE(render(unit, 0), render(unit, kGrayscale));
    EXPECT_NE(render(unit, 0), render(unit, kBlendAdditive));
    EXPECT_NE(render(unit, 0), render(unit, kBlendOverlay));
    EXPECT_NE(render(wide, 0), render(wide, kTileU)); // wrap vs clamp past u=1
}

TEST(CheapRasterImageSynthetic, MaskIdParticipates) {
    auto render = [](TextureId mask) {
        CheapRaster r(TextureMode::kSynthetic);
        r.frame_begin(8, 8, kBlack);
        r.image({0, 0, 8, 8}, 7, {0, 0, 1, 1}, {}, 0, mask, kNoClip);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 8 * 8 * 4);
    };
    EXPECT_NE(render(0), render(9));
    EXPECT_NE(render(9), render(10)); // which mask also matters
}

// ---- sweep -------------------------------------------------------------------

TEST(CheapRasterSweep, QuarterCoversTopRightQuadrant) {
    CheapRaster r;
    r.frame_begin(8, 8, kBlack);
    r.sweep({0, 0, 8, 8}, kWhite, 0, 360, 0.25f, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 6, 1), (std::array<int, 3>{255, 255, 255})); // 45° in
    EXPECT_EQ(px(r, 6, 6), (std::array<int, 3>{0, 0, 0}));       // 135° out
    EXPECT_EQ(px(r, 1, 6), (std::array<int, 3>{0, 0, 0}));       // 225° out
    EXPECT_EQ(px(r, 1, 1), (std::array<int, 3>{0, 0, 0}));       // 315° out
}

TEST(CheapRasterSweep, NegativeSpanRunsCounterClockwise) {
    CheapRaster r;
    r.frame_begin(8, 8, kBlack);
    r.sweep({0, 0, 8, 8}, kWhite, 0, -360, 0.25f, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 1, 1), (std::array<int, 3>{255, 255, 255})); // 315° in
    EXPECT_EQ(px(r, 6, 1), (std::array<int, 3>{0, 0, 0}));       // 45° out
}

TEST(CheapRasterSweep, StartAngleRotatesTheWedge) {
    CheapRaster r;
    r.frame_begin(8, 8, kBlack);
    r.sweep({0, 0, 8, 8}, kWhite, 90, 450, 0.25f, kNoClip); // 90°..180°
    r.frame_end();
    EXPECT_EQ(px(r, 6, 6), (std::array<int, 3>{255, 255, 255})); // 135° in
    EXPECT_EQ(px(r, 6, 1), (std::array<int, 3>{0, 0, 0}));       // 45° out
}

TEST(CheapRasterSweep, FullAndZeroFractions) {
    CheapRaster r;
    r.frame_begin(4, 4, kBlack);
    r.sweep({0, 0, 4, 4}, kWhite, 0, 360, 0, kNoClip); // nothing
    r.frame_end();
    EXPECT_EQ(px(r, 2, 2), (std::array<int, 3>{0, 0, 0}));
    r.frame_begin(4, 4, kBlack);
    r.sweep({0, 0, 4, 4}, kWhite, 0, 360, 1, kNoClip); // the whole rect
    r.frame_end();
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(px(r, x, y), (std::array<int, 3>{255, 255, 255}));
}

// ---- text ---------------------------------------------------------------------

// Font 1 is registered as px=8 (no stroker) unless a test says otherwise.
// The normative layout for px=8 at pen (2,10): advance 4, ascent 6.4, cell i
// = {2 + i*4, 4.4, 3.4, 6} — cell 0 covers pixel centers x 2..4, y 4..9;
// cell 1 starts at x 6, leaving column 5 empty between cells.
TEST(CheapRasterText, CellsLandAtThePen) {
    CheapRaster r;
    r.set_font(1, {8, 0});
    r.frame_begin(16, 16, kBlack);
    r.text({2, 10}, "AB", 1, kWhite, kNoClip, false);
    r.frame_end();
    EXPECT_NE(px(r, 3, 6), (std::array<int, 3>{0, 0, 0}));  // inside cell 0
    EXPECT_EQ(px(r, 5, 6), (std::array<int, 3>{0, 0, 0}));  // the gap between cells
    EXPECT_NE(px(r, 6, 6), (std::array<int, 3>{0, 0, 0}));  // inside cell 1
    EXPECT_EQ(px(r, 3, 3), (std::array<int, 3>{0, 0, 0}));  // above the ascent
    EXPECT_EQ(px(r, 3, 10), (std::array<int, 3>{0, 0, 0})); // below the run
    EXPECT_EQ(px(r, 1, 6), (std::array<int, 3>{0, 0, 0}));  // left of the pen
}

TEST(CheapRasterText, PenMovesTheRun) {
    auto render = [](Vec2 pen) {
        CheapRaster r;
        r.set_font(1, {8, 0});
        r.frame_begin(32, 16, kBlack);
        r.text(pen, "A", 1, kWhite, kNoClip, false);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 32 * 16 * 4);
    };
    EXPECT_NE(render({2, 10}), render({3, 10})); // one pixel right
    EXPECT_NE(render({2, 10}), render({2, 11})); // one pixel down
    EXPECT_EQ(render({2, 10}), render({2, 10})); // determinism
}

TEST(CheapRasterText, ContentChangesPixels) {
    auto render = [](const char* s) {
        CheapRaster r;
        r.set_font(1, {8, 0});
        r.frame_begin(32, 16, kBlack);
        r.text({0, 10}, s, 1, kWhite, kNoClip, false);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 32 * 16 * 4);
    };
    EXPECT_NE(render("AB"), render("AC"));  // one byte differs -> pixels differ
    EXPECT_NE(render("AB"), render("ABC")); // length differs
    EXPECT_EQ(render("AB"), render("AB"));  // determinism
}

TEST(CheapRasterText, FontIdentityAndSizeMatter) {
    auto render = [](FontId id, float pxSize) {
        CheapRaster r;
        r.set_font(id, {pxSize, 0});
        r.frame_begin(32, 16, kBlack);
        r.text({0, 12}, "A", id, kWhite, kNoClip, false);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 32 * 16 * 4);
    };
    EXPECT_NE(render(1, 8), render(2, 8));  // the id seeds the cell hash
    EXPECT_NE(render(1, 8), render(1, 12)); // px scales the cells
}

TEST(CheapRasterText, StrokedInflatesCellsAtTheSameAdvances) {
    auto render = [](bool stroked) {
        CheapRaster r;
        r.set_font(1, {8, 1.5f}); // a declared stroker
        r.frame_begin(16, 16, kBlack);
        r.text({4, 10}, "A", 1, kWhite, kNoClip, stroked);
        r.frame_end();
        return r;
    };
    const CheapRaster plain = render(false), fat = render(true);
    // The plain cell is {4, 4.4, 3.4, 6}; stroked inflates by 1.5 on every
    // side -> {2.5, 2.9, 6.4, 9}. Pixel (3,3) is stroked-only territory.
    EXPECT_EQ(px(plain, 3, 3), (std::array<int, 3>{0, 0, 0}));
    EXPECT_NE(px(fat, 3, 3), (std::array<int, 3>{0, 0, 0}));
    // Both start their run at the same pen: the advance did not change.
    EXPECT_NE(px(plain, 4, 6), (std::array<int, 3>{0, 0, 0}));
    EXPECT_NE(px(fat, 4, 6), (std::array<int, 3>{0, 0, 0}));
}

TEST(CheapRasterText, FontSurfaceIsTheNormativeFormulas) {
    CheapRaster r;
    r.set_font(1, {8, 0});
    r.set_font(2, {8, 1.5f});
    EXPECT_FLOAT_EQ(r.measure(1, "abcd"), 16.0f); // 0.5 * 8 * 4
    EXPECT_FLOAT_EQ(r.measure(1, ""), 0.0f);
    EXPECT_FLOAT_EQ(r.ascent(1), 6.4f);       // 0.8 * 8
    EXPECT_FLOAT_EQ(r.line_height(1), 10.0f); // 1.25 * 8
    EXPECT_FLOAT_EQ(r.outline_width(1), 0.0f);
    EXPECT_FLOAT_EQ(r.outline_width(2), 1.5f); // the registered stroker
    EXPECT_FLOAT_EQ(r.measure(9, "abcd"), 0.0f); // unregistered answers 0
    EXPECT_FLOAT_EQ(r.outline_width(9), 0.0f);
}

TEST(CheapRasterText, EmptyIsANoopUnregisteredIsLoud) {
    CheapRaster r;
    r.set_font(1, {8, 0});
    r.frame_begin(16, 16, kBlack);
    r.text({0, 8}, "", 1, kWhite, kNoClip, false); // empty: nothing
    r.frame_end();
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            EXPECT_EQ(px(r, x, y), (std::array<int, 3>{0, 0, 0}));
    // An unregistered font cannot be silent: fixed-size magenta cells.
    r.frame_begin(16, 16, kBlack);
    r.text({2, 12}, "A", 42, kWhite, kNoClip, false);
    r.frame_end();
    const auto loud = px(r, 3, 8);
    EXPECT_GT(loud[0], 0);
    EXPECT_EQ(loud[1], 0);
    EXPECT_GT(loud[2], 0);
}

TEST(CheapRasterText, ClipCutsCells) {
    CheapRaster r;
    r.set_font(1, {8, 0});
    r.frame_begin(32, 8, kBlack);
    r.text({0, 7}, "AAAA", 1, kWhite, Rect{0, 0, 6, 8}, false);
    r.frame_end();
    EXPECT_NE(px(r, 1, 3), (std::array<int, 3>{0, 0, 0}));
    for (int x = 6; x < 32; ++x) // everything past the clip is untouched
        EXPECT_EQ(px(r, x, 3), (std::array<int, 3>{0, 0, 0}));
}

} // namespace
} // namespace ui
