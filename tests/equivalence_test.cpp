// The equivalence relation itself: pixel match through the same CheapRaster
// configuration. These tests pin the doctrine — order-tolerant exactly where
// order cannot matter, strict where it must, sensitive to content, position,
// flags, and size — using the synthetic mode, i.e. with zero texture bytes.
#include "render/pixel_match.hpp"

#include <gtest/gtest.h>

namespace ui {
namespace {

constexpr Color kBlack{0, 0, 0, 1};

template <class F>
CheapRaster render(F&& draw) {
    CheapRaster r(TextureMode::kSynthetic);
    Painter<CheapRaster> p(r);
    p.frame_begin(16, 16, kBlack);
    draw(p);
    p.frame_end();
    return r;
}

TEST(PixelMatch, IdenticalStreamsMatch) {
    auto draw = [](Painter<CheapRaster>& p) {
        p.image({1, 1, 6, 6}, 7);
        p.quad({8, 8, 4, 4}, {1, 0, 0, 1});
        p.text({1, 14}, "42", Font{1, 6}, {1, 1, 1, 1});
    };
    const auto a = render(draw), b = render(draw);
    const PixelDiff d = match_pixels(a, b);
    EXPECT_TRUE(d.equal());
    EXPECT_EQ(d.differing, 0u);
}

TEST(PixelMatch, NonOverlappingOrderIsIrrelevant) {
    // Disjoint draws in either order — the relation tolerates emit-order
    // differences exactly where a renderer's output cannot depend on them.
    const auto a = render([](Painter<CheapRaster>& p) {
        p.image({0, 0, 6, 6}, 7);
        p.quad({10, 10, 5, 5}, {0, 1, 0, 1});
    });
    const auto b = render([](Painter<CheapRaster>& p) {
        p.quad({10, 10, 5, 5}, {0, 1, 0, 1});
        p.image({0, 0, 6, 6}, 7);
    });
    EXPECT_TRUE(match_pixels(a, b).equal());
}

TEST(PixelMatch, OverlappingOrderMattersAndBboxPinpointsIt) {
    const auto a = render([](Painter<CheapRaster>& p) {
        p.quad({2, 2, 8, 8}, {1, 0, 0, 1});
        p.quad({6, 6, 8, 8}, {0, 0, 1, 0.5f}); // translucent blue on top
    });
    const auto b = render([](Painter<CheapRaster>& p) {
        p.quad({6, 6, 8, 8}, {0, 0, 1, 0.5f});
        p.quad({2, 2, 8, 8}, {1, 0, 0, 1}); // red now covers the overlap
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
    const auto base = render([](Painter<CheapRaster>& p) {
        p.image({2, 2, 8, 8}, 7);
        p.quad({12, 2, 3, 3}, {1, 0, 0, 1});
    });
    const auto wrongTex = render([](Painter<CheapRaster>& p) {
        p.image({2, 2, 8, 8}, 8); // different texture id
        p.quad({12, 2, 3, 3}, {1, 0, 0, 1});
    });
    const auto wrongColor = render([](Painter<CheapRaster>& p) {
        p.image({2, 2, 8, 8}, 7);
        p.quad({12, 2, 3, 3}, {1, 0.1f, 0, 1});
    });
    const auto nudged = render([](Painter<CheapRaster>& p) {
        p.image({3, 2, 8, 8}, 7); // one pixel to the right
        p.quad({12, 2, 3, 3}, {1, 0, 0, 1});
    });
    EXPECT_FALSE(match_pixels(base, wrongTex).equal());
    EXPECT_FALSE(match_pixels(base, wrongColor).equal());
    EXPECT_FALSE(match_pixels(base, nudged).equal());
}

TEST(PixelMatch, FlagAndMaskDifferencesAreDetectedTextureFree) {
    const auto plain = render([](Painter<CheapRaster>& p) { p.image({2, 2, 10, 10}, 7); });
    const auto gray = render([](Painter<CheapRaster>& p) {
        p.image({2, 2, 10, 10}, 7, {0, 0, 1, 1}, {}, kGrayscale);
    });
    const auto masked = render([](Painter<CheapRaster>& p) {
        p.image({2, 2, 10, 10}, 7, {0, 0, 1, 1}, {}, 0, /*mask=*/9);
    });
    EXPECT_FALSE(match_pixels(plain, gray).equal());
    EXPECT_FALSE(match_pixels(plain, masked).equal());
}

TEST(PixelMatch, TextDifferencesAreDetected) {
    const auto a = render([](Painter<CheapRaster>& p) {
        p.text({1, 8}, "10", Font{1, 8}, {1, 1, 1, 1});
    });
    const auto b = render([](Painter<CheapRaster>& p) {
        p.text({1, 8}, "16", Font{1, 8}, {1, 1, 1, 1});
    });
    EXPECT_FALSE(match_pixels(a, b).equal());
}

TEST(PixelMatch, DecorationsAreProducerPatterns) {
    // A drop shadow is nothing but the same run drawn first, offset, in
    // another color — so two producers writing the pattern independently
    // match, and dropping the under-pass is detected.
    // (The shadow must be a color that shows on the black canvas — a black
    // shadow on black is genuinely invisible, and the gate rightly calls
    // streams with and without it equivalent.)
    auto shadowed = [](Painter<CheapRaster>& p) {
        p.text({3, 9}, "42", Font{1, 8}, {1, 0, 0, 1}); // the under-pass
        p.text({2, 8}, "42", Font{1, 8}, {1, 1, 1, 1}); // the fill
    };
    const auto a = render(shadowed), b = render(shadowed);
    EXPECT_TRUE(match_pixels(a, b).equal());
    const auto noShadow = render([](Painter<CheapRaster>& p) {
        p.text({2, 8}, "42", Font{1, 8}, {1, 1, 1, 1});
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
    const auto a = render([](Painter<CheapRaster>& p) { p.quad({1, 1, 4, 4}, {1, 0, 0, 1}); });
    const auto b = render([](Painter<CheapRaster>& p) { p.quad({1, 1, 4, 4}, {1, 0, 0, 1}); });
    EXPECT_TRUE(match_pixels(a.pixels(), b.pixels(), 16, 16).equal());
}

} // namespace
} // namespace ui
