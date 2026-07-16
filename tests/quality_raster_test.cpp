// QualityRaster (images stage) — exact-pixel pins for the reference
// renderer's contract: area-filtered mip chains, trilinear selection,
// bilinear magnification with hand-computed lerps, per-operation clamping
// (the GPU-semantics decision), bilinear dst-fraction masks, and the same
// solid/magenta/sweep vocabulary the cheap rung pins. Values are ARITHMETIC,
// derived in the comments — not baked from a run.
#include "render/quality_raster.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace ui {
namespace {

constexpr Color kBlack{0, 0, 0, 1};
constexpr Color kWhite{1, 1, 1, 1};

std::array<int, 3> px(const QualityRaster& r, int x, int y) {
    const uint8_t* p = &r.pixels()[(static_cast<size_t>(y) * r.width() + x) * 4];
    return {p[0], p[1], p[2]};
}

// A 2x1 texture: red | blue, both opaque.
const uint8_t kRedBlue[8] = {255, 0, 0, 255, 0, 0, 255, 255};

TEST(QualityFrame, ClearQuantizesOnceAtFrameEnd) {
    QualityRaster r;
    r.frame_begin(2, 1, Color{0.5f, 0.25f, 1, 1});
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{128, 64, 255}));
    EXPECT_EQ(r.pixels()[3], 255);
    r.frame_begin(4, 2, kBlack); // resize + reuse
    r.frame_end();
    EXPECT_EQ(r.width(), 4u);
    EXPECT_EQ(px(r, 3, 1), (std::array<int, 3>{0, 0, 0}));
}

TEST(QualityImage, BilinearMagnificationExactLerps) {
    QualityRaster r;
    r.set_texture(1, kRedBlue, 2, 1);
    r.frame_begin(4, 1, kBlack);
    r.image({0, 0, 4, 1}, 1, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    r.frame_end();
    // texel centers at u=0.25/0.75; pixel centers sample u=.125/.375/.625/.875
    // -> tap mixes red:blue = 1:0, .75:.25, .25:.75, 0:1
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 0, 0}));
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{191, 0, 64})); // .75*255=191.25
    EXPECT_EQ(px(r, 2, 0), (std::array<int, 3>{64, 0, 191}));
    EXPECT_EQ(px(r, 3, 0), (std::array<int, 3>{0, 0, 255}));
}

TEST(QualityImage, MipLevelIsAreaAverage) {
    // A 4x4 per-texel checkerboard red/blue: level 1 is exactly the 2x2 of
    // (0.5, 0, 0.5) averages. Drawing 4x4 -> 2x2 has a footprint of exactly
    // 2 texels/px = level 1 flat: every pixel (128, 0, 128).
    std::vector<uint8_t> checker(4 * 4 * 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* p = &checker[(size_t)(y * 4 + x) * 4];
            const bool red = ((x + y) & 1) == 0;
            p[0] = red ? 255 : 0;
            p[1] = 0;
            p[2] = red ? 0 : 255;
            p[3] = 255;
        }
    QualityRaster r;
    r.set_texture(1, checker.data(), 4, 4);
    r.frame_begin(2, 2, kBlack);
    r.image({0, 0, 2, 2}, 1, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    r.frame_end();
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            EXPECT_EQ(px(r, x, y), (std::array<int, 3>{128, 0, 128}));
}

TEST(QualityImage, TrilinearSitsBetweenLevels) {
    // red/black checkerboard: at 4x4 (level 0) a corner pixel is pure red;
    // at 2x2 (level 1) it is the 0.5 average. A 3x3 draw (footprint 4/3)
    // must land strictly between, and identically on repeat runs.
    std::vector<uint8_t> checker(4 * 4 * 4, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* p = &checker[(size_t)(y * 4 + x) * 4];
            p[0] = ((x + y) & 1) == 0 ? 255 : 0;
            p[3] = 255;
        }
    auto render = [&](int side) {
        QualityRaster r;
        r.set_texture(1, checker.data(), 4, 4);
        r.frame_begin(4, 4, kBlack);
        r.image({0, 0, (float)side, (float)side}, 1, {0, 0, 1, 1}, kWhite, 0,
                0, kNoClip);
        r.frame_end();
        return px(r, 0, 0)[0];
    };
    const int at4 = render(4), at3 = render(3), at2 = render(2);
    EXPECT_EQ(at4, 255); // level 0: the corner texel
    EXPECT_EQ(at2, 128); // level 1: the 2x2 average
    EXPECT_GT(at3, at2); // trilinear: strictly between
    EXPECT_LT(at3, at4);
    EXPECT_EQ(render(3), at3); // deterministic
}

TEST(QualityImage, PerOpClampMatchesGpuSemantics) {
    // additive white onto 0.5 gray clamps to 1.0 AT THE OP (a UNORM target
    // clamps every draw); the following overlay sees 1.0, not 1.5:
    // 2 * 1.0 * 0.4 = 0.8 -> 204. An unclamped float canvas would give
    // 2 * 1.5 * 0.4 = 1.2 -> 255. The reference must match the GPU.
    QualityRaster r;
    r.frame_begin(1, 1, Color{0.5f, 0.5f, 0.5f, 1});
    r.image({0, 0, 1, 1}, 0, {0, 0, 1, 1}, kWhite, kBlendAdditive, 0, kNoClip);
    r.quad({0, 0, 1, 1}, Color{0.4f, 0.4f, 0.4f, 1}, kBlendOverlay, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{204, 204, 204}));
}

TEST(QualityImage, SolidZeroMagentaAndModifierNoops) {
    QualityRaster r;
    r.frame_begin(4, 1, kBlack);
    r.image({0, 0, 1, 1}, 0, {0, 0, 1, 1}, Color{0, 1, 0, 0.5f}, 0, 0,
            kNoClip); // texture 0: tint is the fill
    r.image({1, 0, 1, 1}, 42, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip); // magenta
    r.image({2, 0, 1, 1}, 0, {0, 0, 1, 1}, Color{1, 0, 0, 1},
            kGrayscale | kTileU, 0, kNoClip); // modifiers no-op on solids
    r.image({3, 0, 1, 1}, 0, {0, 0, 1, 1}, Color{1, 0, 0, 1}, 0, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{0, 128, 0}));
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{255, 0, 255}));
    EXPECT_EQ(px(r, 2, 0), px(r, 3, 0));
}

TEST(QualityImage, GrayscaleIsLumaBeforeTint) {
    const uint8_t red[4] = {255, 0, 0, 255};
    QualityRaster r;
    r.set_texture(1, red, 1, 1);
    r.frame_begin(2, 1, kBlack);
    r.image({0, 0, 1, 1}, 1, {0, 0, 1, 1}, kWhite, kGrayscale, 0, kNoClip);
    r.image({1, 0, 1, 1}, 1, {0, 0, 1, 1}, Color{0, 0, 1, 1}, kGrayscale, 0,
            kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{76, 76, 76})); // .299*255
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{0, 0, 76}));   // luma THEN tint
}

TEST(QualityImage, MaskIsBilinearAcrossDst) {
    // 2x1 mask (opaque | transparent) over a solid red image(0): mask taps
    // at dst fractions .125/.375/.625/.875 -> alphas 1.0/.75/.25/0 — the
    // GRADIENT distinguishes quality's bilinear mask from cheap's nearest.
    const uint8_t maskTex[8] = {255, 255, 255, 255, 255, 255, 255, 0};
    QualityRaster r;
    r.set_texture(2, maskTex, 2, 1);
    r.frame_begin(4, 1, kBlack);
    r.image({0, 0, 4, 1}, 0, {0, 0, 1, 1}, Color{1, 0, 0, 1}, 0, 2, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 0, 0}));
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{191, 0, 0}));
    EXPECT_EQ(px(r, 2, 0), (std::array<int, 3>{64, 0, 0}));
    EXPECT_EQ(px(r, 3, 0), (std::array<int, 3>{0, 0, 0}));
}

TEST(QualityImage, FlipsTilesAndSubrects) {
    QualityRaster r;
    r.set_texture(1, kRedBlue, 2, 1);
    r.frame_begin(4, 3, kBlack);
    r.image({0, 0, 4, 1}, 1, {1, 0, -1, 1}, kWhite, 0, 0, kNoClip); // hflip
    r.image({0, 1, 4, 1}, 1, {0.75f, 0, 0.0f, 1}, kWhite, 0, 0, kNoClip);
    r.image({0, 2, 4, 1}, 1, {0, 0, 2, 1}, kWhite, kTileU, 0, kNoClip);
    r.frame_end();
    // hflip mirrors the magnification row
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{0, 0, 255}));
    EXPECT_EQ(px(r, 3, 0), (std::array<int, 3>{255, 0, 0}));
    // a zero-width uv at u=0.75 samples the blue texel center everywhere
    EXPECT_EQ(px(r, 1, 1), (std::array<int, 3>{0, 0, 255}));
    // tiled u in [0,2): at x=1 center u=.75 -> texel 1 (blue); wrap makes
    // x=2 (u=1.25 -> .25) red again — the seam blends through wrap taps
    EXPECT_EQ(px(r, 1, 2)[2], 255);
    EXPECT_EQ(px(r, 2, 2)[0], 255);
}

TEST(QualitySweep, QuadrantAndMask) {
    QualityRaster r;
    r.frame_begin(8, 8, kBlack);
    r.sweep({0, 0, 8, 8}, kWhite, 0, 360, 0.25f, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 6, 1), (std::array<int, 3>{255, 255, 255})); // 45° in
    EXPECT_EQ(px(r, 1, 1), (std::array<int, 3>{0, 0, 0}));       // 315° out
    // masked full sweep: 2x1 opaque|transparent mask cuts the right side
    const uint8_t maskTex[8] = {255, 255, 255, 255, 255, 255, 255, 0};
    QualityRaster m;
    m.set_texture(2, maskTex, 2, 1);
    m.frame_begin(8, 8, kBlack);
    m.sweep({0, 0, 8, 8}, kWhite, 0, 360, 1, 2, kNoClip);
    m.frame_end();
    EXPECT_EQ(px(m, 0, 4), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(m, 7, 4), (std::array<int, 3>{0, 0, 0}));
}

TEST(QualityDeterminism, SameStreamSameBytes) {
    auto render = [] {
        QualityRaster r;
        r.set_texture(1, kRedBlue, 2, 1);
        r.frame_begin(8, 8, kBlack);
        r.image({0, 0, 7, 5}, 1, {0, 0, 1, 1}, Color{1, 1, 0.5f, 0.8f}, 0, 0,
                kNoClip);
        r.sweep({1, 1, 6, 6}, Color{0, 0, 0, 0.7f}, 0, 360, 0.6f, 0, kNoClip);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 8 * 8 * 4);
    };
    EXPECT_EQ(render(), render());
}

} // namespace
} // namespace ui
