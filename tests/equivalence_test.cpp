// The equivalence relation itself: pixel match through the same CheapRaster
// configuration. These tests pin the doctrine — order-tolerant exactly where
// order cannot matter, strict where it must, sensitive to content, position,
// flags, and size — using the synthetic mode, i.e. with zero texture bytes.
// Sinks are called directly: no emit-side state exists, so every call reads
// exactly as it renders.
#include "render/pixel_match.hpp"

#include <gtest/gtest.h>

namespace ui {
namespace {

constexpr Color kBlack{0, 0, 0, 1};

template <class F>
CheapRaster render(F&& draw) {
    CheapRaster r(TextureMode::kSynthetic);
    r.set_font(1, {8, 0}); // both sides of a comparison share one font world
    r.set_font(2, {6, 0});
    r.frame_begin(16, 16, kBlack);
    draw(r);
    r.frame_end();
    return r;
}

TEST(PixelMatch, IdenticalStreamsMatch) {
    auto draw = [](CheapRaster& r) {
        r.image({1, 1, 6, 6}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
        r.quad({8, 8, 4, 4}, {1, 0, 0, 1}, kNoClip);
        r.text({1, 14}, "42", 2, {1, 1, 1, 1}, kNoClip);
    };
    const auto a = render(draw), b = render(draw);
    const PixelDiff d = match_pixels(a, b);
    EXPECT_TRUE(d.equal());
    EXPECT_EQ(d.differing, 0u);
}

TEST(PixelMatch, NonOverlappingOrderIsIrrelevant) {
    // Disjoint draws in either order — the relation tolerates emit-order
    // differences exactly where a renderer's output cannot depend on them.
    const auto a = render([](CheapRaster& r) {
        r.image({0, 0, 6, 6}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
        r.quad({10, 10, 5, 5}, {0, 1, 0, 1}, kNoClip);
    });
    const auto b = render([](CheapRaster& r) {
        r.quad({10, 10, 5, 5}, {0, 1, 0, 1}, kNoClip);
        r.image({0, 0, 6, 6}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
    });
    EXPECT_TRUE(match_pixels(a, b).equal());
}

TEST(PixelMatch, OverlappingOrderMattersAndBboxPinpointsIt) {
    const auto a = render([](CheapRaster& r) {
        r.quad({2, 2, 8, 8}, {1, 0, 0, 1}, kNoClip);
        r.quad({6, 6, 8, 8}, {0, 0, 1, 0.5f}, kNoClip); // translucent blue on top
    });
    const auto b = render([](CheapRaster& r) {
        r.quad({6, 6, 8, 8}, {0, 0, 1, 0.5f}, kNoClip);
        r.quad({2, 2, 8, 8}, {1, 0, 0, 1}, kNoClip); // red now covers the overlap
    });
    const PixelDiff d = match_pixels(a, b);
    EXPECT_FALSE(d.equal());
    EXPECT_GT(d.differing, 0u);
    // Only the overlap region {6,6}..{10,10} can differ.
    EXPECT_GE(d.x0, 6u);
    EXPECT_GE(d.y0, 6u);
    EXPECT_LE(d.x1, 10u);
    EXPECT_LE(d.y1, 10u);
}

TEST(PixelMatch, WrongTextureWrongColorWrongPlaceAllDetected) {
    const auto base = render([](CheapRaster& r) {
        r.image({2, 2, 8, 8}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
        r.quad({12, 2, 3, 3}, {1, 0, 0, 1}, kNoClip);
    });
    const auto wrongTex = render([](CheapRaster& r) {
        r.image({2, 2, 8, 8}, 8, {0, 0, 1, 1}, {}, 0, 0, kNoClip); // wrong id
        r.quad({12, 2, 3, 3}, {1, 0, 0, 1}, kNoClip);
    });
    const auto wrongColor = render([](CheapRaster& r) {
        r.image({2, 2, 8, 8}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
        r.quad({12, 2, 3, 3}, {1, 0.1f, 0, 1}, kNoClip);
    });
    const auto nudged = render([](CheapRaster& r) {
        r.image({3, 2, 8, 8}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip); // 1px right
        r.quad({12, 2, 3, 3}, {1, 0, 0, 1}, kNoClip);
    });
    EXPECT_FALSE(match_pixels(base, wrongTex).equal());
    EXPECT_FALSE(match_pixels(base, wrongColor).equal());
    EXPECT_FALSE(match_pixels(base, nudged).equal());
}

TEST(PixelMatch, FlagAndMaskDifferencesAreDetectedTextureFree) {
    const auto plain = render([](CheapRaster& r) {
        r.image({2, 2, 10, 10}, 7, {0, 0, 1, 1}, {}, 0, 0, kNoClip);
    });
    const auto gray = render([](CheapRaster& r) {
        r.image({2, 2, 10, 10}, 7, {0, 0, 1, 1}, {}, kGrayscale, 0, kNoClip);
    });
    const auto masked = render([](CheapRaster& r) {
        r.image({2, 2, 10, 10}, 7, {0, 0, 1, 1}, {}, 0, /*mask=*/9, kNoClip);
    });
    EXPECT_FALSE(match_pixels(plain, gray).equal());
    EXPECT_FALSE(match_pixels(plain, masked).equal());
}

TEST(PixelMatch, TextDifferencesAreDetected) {
    const auto a = render([](CheapRaster& r) {
        r.text({1, 8}, "10", 1, {1, 1, 1, 1}, kNoClip);
    });
    const auto b = render([](CheapRaster& r) {
        r.text({1, 8}, "16", 1, {1, 1, 1, 1}, kNoClip);
    });
    EXPECT_FALSE(match_pixels(a, b).equal());
    const auto wrongFont = render([](CheapRaster& r) {
        r.text({1, 8}, "10", 2, {1, 1, 1, 1}, kNoClip);
    });
    EXPECT_FALSE(match_pixels(a, wrongFont).equal());
    const auto stroked = render([](CheapRaster& r) {
        r.text_stroked({1, 8}, "10", 1, {1, 1, 1, 1}, kNoClip);
    });
    // Font 1 declares no stroker: stroked degrades to the plain run, and the
    // relation rightly calls them the same picture.
    EXPECT_TRUE(match_pixels(a, stroked).equal());
}

TEST(PixelMatch, DecorationsAreProducerPatterns) {
    // A drop shadow is nothing but the same run drawn first, offset, in
    // another color — so two producers writing the pattern independently
    // match, and dropping the under-pass is detected.
    // (The shadow must be a color that shows on the black canvas — a black
    // shadow on black is genuinely invisible, and the gate rightly calls
    // streams with and without it equivalent.)
    auto shadowed = [](CheapRaster& r) {
        r.text({3, 9}, "42", 1, {1, 0, 0, 1}, kNoClip); // the under-pass
        r.text({2, 8}, "42", 1, {1, 1, 1, 1}, kNoClip); // the fill
    };
    const auto a = render(shadowed), b = render(shadowed);
    EXPECT_TRUE(match_pixels(a, b).equal());
    const auto noShadow = render([](CheapRaster& r) {
        r.text({2, 8}, "42", 1, {1, 1, 1, 1}, kNoClip);
    });
    EXPECT_FALSE(match_pixels(a, noShadow).equal());
}

TEST(PixelMatch, SizeMismatchIsItsOwnVerdict) {
    CheapRaster a(TextureMode::kSynthetic), b(TextureMode::kSynthetic);
    a.frame_begin(8, 8, kBlack);
    a.frame_end();
    b.frame_begin(16, 8, kBlack);
    b.frame_end();
    const PixelDiff d = match_pixels(a, b);
    EXPECT_TRUE(d.size_mismatch);
    EXPECT_FALSE(d.equal());
}

TEST(PixelMatch, RawBufferOverloadAgrees) {
    const auto a = render([](CheapRaster& r) { r.quad({1, 1, 4, 4}, {1, 0, 0, 1}, kNoClip); });
    const auto b = render([](CheapRaster& r) { r.quad({1, 1, 4, 4}, {1, 0, 0, 1}, kNoClip); });
    EXPECT_TRUE(match_pixels(a.pixels(), b.pixels(), 16, 16).equal());
}

} // namespace
} // namespace ui
