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

// Host textures/masks register ABOVE the reserved intrinsics; anchoring to
// Texture::FirstIndex keeps these fixtures correct if an intrinsic is added.
constexpr TextureId kTex = Texture::FirstIndex;      // a host texture slot
constexpr TextureId kMask = Texture::FirstIndex + 1; // a host mask slot

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
    r.set_texture(kTex, kRedBlue, 2, 1);
    r.frame_begin(4, 1, kBlack);
    r.image({0, 0, 4, 1}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
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
    r.set_texture(kTex, checker.data(), 4, 4);
    r.frame_begin(2, 2, kBlack);
    r.image({0, 0, 2, 2}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
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
        r.set_texture(kTex, checker.data(), 4, 4);
        r.frame_begin(4, 4, kBlack);
        r.image({0, 0, (float)side, (float)side}, kTex, {0, 0, 1, 1}, kWhite, 0,
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
    r.image({0, 0, 1, 1}, Texture::White, {0, 0, 1, 1}, kWhite, kBlendAdditive, 0, kNoClip);
    r.quad({0, 0, 1, 1}, Color{0.4f, 0.4f, 0.4f, 1}, kBlendOverlay, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{204, 204, 204}));
}

TEST(QualityImage, SolidWhiteMagentaAndModifierNoops) {
    QualityRaster r;
    r.frame_begin(4, 1, kBlack);
    r.image({0, 0, 1, 1}, Texture::White, {0, 0, 1, 1}, Color{0, 1, 0, 0.5f}, 0, 0,
            kNoClip); // the white texel: tint is the fill
    r.image({1, 0, 1, 1}, 42, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip); // magenta
    r.image({2, 0, 1, 1}, Texture::White, {0, 0, 1, 1}, Color{1, 0, 0, 1},
            kGrayscale | kTileU, 0, kNoClip); // modifiers no-op on solids
    r.image({3, 0, 1, 1}, Texture::White, {0, 0, 1, 1}, Color{1, 0, 0, 1}, 0, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{0, 128, 0}));
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{255, 0, 255}));
    EXPECT_EQ(px(r, 2, 0), px(r, 3, 0));
}

TEST(QualityImage, InvisibleDrawsNothingBlackIsTheBlackTexel) {
    QualityRaster r;
    r.frame_begin(2, 1, Color{0, 0, 1, 1}); // blue canvas
    // Invisible: no draw, even with an opaque tint — the canvas survives.
    r.image({0, 0, 1, 1}, Texture::Invisible, {0, 0, 1, 1}, kWhite, 0, 0,
            kNoClip);
    // Black: the solid black texel (tint identity).
    r.image({1, 0, 1, 1}, Texture::Black, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{0, 0, 255})); // untouched blue
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{0, 0, 0}));   // black texel
}

TEST(QualityImage, GrayscaleIsLumaBeforeTint) {
    const uint8_t red[4] = {255, 0, 0, 255};
    QualityRaster r;
    r.set_texture(kTex, red, 1, 1);
    r.frame_begin(2, 1, kBlack);
    r.image({0, 0, 1, 1}, kTex, {0, 0, 1, 1}, kWhite, kGrayscale, 0, kNoClip);
    r.image({1, 0, 1, 1}, kTex, {0, 0, 1, 1}, Color{0, 0, 1, 1}, kGrayscale, 0,
            kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{76, 76, 76})); // .299*255
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{0, 0, 76}));   // luma THEN tint
}

TEST(QualityImage, MaskIsBilinearAcrossDst) {
    // 2x1 mask (opaque | transparent) over a solid red White texel: mask taps
    // at dst fractions .125/.375/.625/.875 -> alphas 1.0/.75/.25/0 — the
    // GRADIENT distinguishes quality's bilinear mask from cheap's nearest.
    const uint8_t maskTex[8] = {255, 255, 255, 255, 255, 255, 255, 0};
    QualityRaster r;
    r.set_texture(kMask, maskTex, 2, 1);
    r.frame_begin(4, 1, kBlack);
    r.image({0, 0, 4, 1}, Texture::White, {0, 0, 1, 1}, Color{1, 0, 0, 1}, 0, kMask, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 0, 0), (std::array<int, 3>{255, 0, 0}));
    EXPECT_EQ(px(r, 1, 0), (std::array<int, 3>{191, 0, 0}));
    EXPECT_EQ(px(r, 2, 0), (std::array<int, 3>{64, 0, 0}));
    EXPECT_EQ(px(r, 3, 0), (std::array<int, 3>{0, 0, 0}));
}

TEST(QualityImage, FlipsTilesAndSubrects) {
    QualityRaster r;
    r.set_texture(kTex, kRedBlue, 2, 1);
    r.frame_begin(4, 3, kBlack);
    r.image({0, 0, 4, 1}, kTex, {1, 0, -1, 1}, kWhite, 0, 0, kNoClip); // hflip
    r.image({0, 1, 4, 1}, kTex, {0.75f, 0, 0.0f, 1}, kWhite, 0, 0, kNoClip);
    r.image({0, 2, 4, 1}, kTex, {0, 0, 2, 1}, kWhite, kTileU, 0, kNoClip);
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

// 4x4, horizontal stripes varying in v: rows 0-1 white, rows 2-3 black.
// Mips: level1 rows = white|black, level2 = 0.5 gray.
std::vector<uint8_t> hstripes() {
    std::vector<uint8_t> t(4 * 4 * 4, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* p = &t[(size_t)(y * 4 + x) * 4];
            p[0] = p[1] = p[2] = y < 2 ? 255 : 0;
            p[3] = 255;
        }
    return t;
}

TEST(QualitySampler, AnisoKeepsTheUnminifiedAxis) {
    // dst 1x4 from 4x4: footprint 4 texels/px in u, 1:1 in v. Isotropic
    // trilinear takes level 2 (flat 0.5 gray). Aniso derives the level from
    // the MINOR axis (v, 1:1 -> level 0) and integrates u with 4 taps at
    // u = .125/.375/.625/.875 (exact texel centers): each row keeps its own
    // color — the one-axis-minified sharpness hardware aniso gives.
    const std::vector<uint8_t> t = hstripes();
    QualityRaster iso;
    iso.set_texture(kTex, t.data(), 4, 4);
    iso.frame_begin(1, 4, kBlack);
    iso.image({0, 0, 1, 4}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    iso.frame_end();
    for (int y = 0; y < 4; ++y)
        EXPECT_EQ(px(iso, 0, y), (std::array<int, 3>{128, 128, 128})) << y;

    QualityRaster an(QualityRaster::Sampler{0.0f, 16});
    an.set_texture(kTex, t.data(), 4, 4);
    an.frame_begin(1, 4, kBlack);
    an.image({0, 0, 1, 4}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    an.frame_end();
    EXPECT_EQ(px(an, 0, 0), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(an, 0, 1), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(an, 0, 2), (std::array<int, 3>{0, 0, 0}));
    EXPECT_EQ(px(an, 0, 3), (std::array<int, 3>{0, 0, 0}));
}

TEST(QualitySampler, AnisoStillIntegratesTheMinifiedAxis) {
    // Vertical stripes: col 0 white, cols 1-3 black. The same 1x4 aniso
    // draw taps texels 0..3 across u -> every row averages to 0.25 (64).
    // A single center tap at level 0 would give 0 (cols 1|2) — the taps
    // must cover the footprint, not just sharpen.
    std::vector<uint8_t> t(4 * 4 * 4, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* p = &t[(size_t)(y * 4 + x) * 4];
            p[0] = p[1] = p[2] = x == 0 ? 255 : 0;
            p[3] = 255;
        }
    QualityRaster an(QualityRaster::Sampler{0.0f, 16});
    an.set_texture(kTex, t.data(), 4, 4);
    an.frame_begin(1, 4, kBlack);
    an.image({0, 0, 1, 4}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    an.frame_end();
    for (int y = 0; y < 4; ++y)
        EXPECT_EQ(px(an, 0, y), (std::array<int, 3>{64, 64, 64})) << y;
}

TEST(QualitySampler, LodBiasShiftsTheLevel) {
    const std::vector<uint8_t> t = hstripes();
    // Sharper: the 1x4 draw's isotropic level 2 biased by -2 lands on
    // level 0 — one center tap per row (u=.5 -> cols 1|2, rows uniform)
    // recovers the stripe colors.
    QualityRaster sharp(QualityRaster::Sampler{-2.0f, 1});
    sharp.set_texture(kTex, t.data(), 4, 4);
    sharp.frame_begin(1, 4, kBlack);
    sharp.image({0, 0, 1, 4}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    sharp.frame_end();
    EXPECT_EQ(px(sharp, 0, 0), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(sharp, 0, 3), (std::array<int, 3>{0, 0, 0}));

    // Blurrier: a 1:1 draw biased +1 samples level 1 (rows white|black):
    // dst y=1 (v=.375) -> level-1 vy=.25 -> .75*white+.25*black = 191,
    // where level 0 gives the exact row-1 texel, pure white.
    QualityRaster blur(QualityRaster::Sampler{1.0f, 1});
    blur.set_texture(kTex, t.data(), 4, 4);
    blur.frame_begin(4, 4, kBlack);
    blur.image({0, 0, 4, 4}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    blur.frame_end();
    EXPECT_EQ(px(blur, 0, 1), (std::array<int, 3>{191, 191, 191}));
    QualityRaster plain;
    plain.set_texture(kTex, t.data(), 4, 4);
    plain.frame_begin(4, 4, kBlack);
    plain.image({0, 0, 4, 4}, kTex, {0, 0, 1, 1}, kWhite, 0, 0, kNoClip);
    plain.frame_end();
    EXPECT_EQ(px(plain, 0, 1), (std::array<int, 3>{255, 255, 255}));
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
    m.set_texture(kMask, maskTex, 2, 1);
    m.frame_begin(8, 8, kBlack);
    m.sweep({0, 0, 8, 8}, kWhite, 0, 360, 1, kMask, kNoClip);
    m.frame_end();
    EXPECT_EQ(px(m, 0, 4), (std::array<int, 3>{255, 255, 255}));
    EXPECT_EQ(px(m, 7, 4), (std::array<int, 3>{0, 0, 0}));
}

TEST(QualityDeterminism, SameStreamSameBytes) {
    auto render = [] {
        QualityRaster r;
        r.set_texture(kTex, kRedBlue, 2, 1);
        r.frame_begin(8, 8, kBlack);
        r.image({0, 0, 7, 5}, kTex, {0, 0, 1, 1}, Color{1, 1, 0.5f, 0.8f}, 0, 0,
                kNoClip);
        r.sweep({1, 1, 6, 6}, Color{0, 0, 0, 0.7f}, 0, 360, 0.6f, 0, kNoClip);
        r.frame_end();
        return std::vector<uint8_t>(r.pixels(), r.pixels() + 8 * 8 * 4);
    };
    EXPECT_EQ(render(), render());
}

} // namespace
} // namespace ui
