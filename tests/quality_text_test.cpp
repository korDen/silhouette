// QualityRaster text stage — the metrics rule, shaping, analytic coverage,
// stroked variants, gamma, and the loud failure grammar. Face-dependent
// tests load a real .ttf from the UI_TEST_FONT environment variable and
// SKIP LOUDLY when it is unset (the repo carries no fonts yet; a committed
// OFL subset font upgrades these to always-on — docs/quality-renderer.md).
#include "render/quality_raster.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace ui {
namespace {

constexpr Color kBlack{0, 0, 0, 1};
constexpr Color kWhite{1, 1, 1, 1};

std::array<int, 3> px(const QualityRaster& r, int x, int y) {
    const uint8_t* p = &r.pixels()[(static_cast<size_t>(y) * r.width() + x) * 4];
    return {p[0], p[1], p[2]};
}

// Count non-background pixels in a region — ink presence without pinning
// glyph shapes (those live in the goldens once the committed font exists).
int ink_count(const QualityRaster& r, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (px(r, x, y) != std::array<int, 3>{0, 0, 0}) ++n;
    return n;
}

const std::vector<uint8_t>* test_font() {
    static std::vector<uint8_t> bytes = [] {
        std::vector<uint8_t> b;
        if (const char* path = std::getenv("UI_TEST_FONT")) {
            std::ifstream f(path, std::ios::binary);
            b.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
        }
        return b;
    }();
    return bytes.empty() ? nullptr : &bytes;
}

#define REQUIRE_FONT()                                                       \
    const std::vector<uint8_t>* fontBytes = test_font();                     \
    if (fontBytes == nullptr)                                                \
        GTEST_SKIP() << "UI_TEST_FONT not set — face tests skipped";

TEST(QualityText, UnregisteredFontIsLoudAndAnswersZero) {
    QualityRaster r;
    r.frame_begin(32, 16, kBlack);
    r.text({2, 12}, "42", 9, kWhite, kNoClip);
    r.frame_end();
    // loud magenta cells, nothing silent
    EXPECT_GT(ink_count(r, 0, 0, 32, 16), 0);
    EXPECT_EQ(px(r, 3, 8), (std::array<int, 3>{255, 0, 255}));
    EXPECT_FLOAT_EQ(r.measure(9, "42"), 0.0f);
    EXPECT_FLOAT_EQ(r.ascent(9), 0.0f);
    EXPECT_FLOAT_EQ(r.line_height(9), 0.0f);
    EXPECT_FLOAT_EQ(r.outline_width(9), 0.0f);
}

TEST(QualityText, MetricsOnlyRegistrationAnswersTheTable) {
    // No face bytes: the surface answers with the registered numbers (the
    // metrics rule), text draws the loud cells (no ink source).
    const float adv[3] = {7, 8, 9}; // codepoints '0','1','2' with first=48
    QualityRaster r;
    QualityRaster::QualityFontDesc d;
    d.px = 12;
    d.strokeWidth = 1.5f;
    d.metrics = {10, 15, adv, 48, 3};
    r.set_font(5, d);
    EXPECT_FLOAT_EQ(r.measure(5, "012"), 24.0f); // 7+8+9
    EXPECT_FLOAT_EQ(r.ascent(5), 10.0f);
    EXPECT_FLOAT_EQ(r.line_height(5), 15.0f);
    EXPECT_FLOAT_EQ(r.outline_width(5), 1.5f);
    r.frame_begin(32, 16, kBlack);
    r.text({2, 12}, "01", 5, kWhite, kNoClip);
    r.frame_end();
    EXPECT_EQ(px(r, 3, 8), (std::array<int, 3>{255, 0, 255})); // loud cells
}

TEST(QualityText, ShapedRunDrawsRealInk) {
    REQUIRE_FONT();
    QualityRaster r;
    QualityRaster::QualityFontDesc d;
    d.faceBytes = fontBytes->data();
    d.faceSize = fontBytes->size();
    d.px = 16;
    r.set_font(1, d);
    EXPECT_GT(r.measure(1, "42"), 0.0f);
    EXPECT_GT(r.ascent(1), 0.0f);
    r.frame_begin(64, 24, kBlack);
    r.text({4, 18}, "42", 1, kWhite, kNoClip);
    r.frame_end();
    EXPECT_GT(ink_count(r, 4, 2, 40, 20), 0); // ink in the run's box
    EXPECT_EQ(ink_count(r, 0, 0, 4, 24), 0);  // nothing left of the pen
    // determinism
    QualityRaster r2;
    r2.set_font(1, d);
    r2.frame_begin(64, 24, kBlack);
    r2.text({4, 18}, "42", 1, kWhite, kNoClip);
    r2.frame_end();
    EXPECT_TRUE(std::equal(r.pixels(), r.pixels() + 64 * 24 * 4,
                           r2.pixels()));
}

TEST(QualityText, RegisteredTableGovernsPensAndMeasure) {
    REQUIRE_FONT();
    // A deliberately huge advance for '1' (24px at px=16): the second glyph
    // must start a table-advance to the right, and measure() must agree —
    // layout and ink cannot disagree, whatever the face's real advance is.
    const float adv[2] = {24, 24}; // '1','2' with first=49
    QualityRaster r;
    QualityRaster::QualityFontDesc d;
    d.faceBytes = fontBytes->data();
    d.faceSize = fontBytes->size();
    d.px = 16;
    d.metrics = {12, 20, adv, 49, 2};
    r.set_font(1, d);
    EXPECT_FLOAT_EQ(r.measure(1, "11"), 48.0f);
    r.frame_begin(96, 24, kBlack);
    r.text({4, 18}, "11", 1, kWhite, kNoClip);
    r.frame_end();
    const int leftInk = ink_count(r, 4, 0, 20, 24);   // first glyph's box
    const int gapInk = ink_count(r, 20, 0, 28, 24);   // the stretched gap
    const int rightInk = ink_count(r, 28, 0, 44, 24); // second glyph's box
    EXPECT_GT(leftInk, 0);
    EXPECT_EQ(gapInk, 0); // the table's 24px advance leaves the gap empty
    EXPECT_GT(rightInk, 0);
}

TEST(QualityText, StrokedFattensAtTheSameAdvances) {
    REQUIRE_FONT();
    QualityRaster::QualityFontDesc d;
    d.px = 16;
    d.strokeWidth = 2;
    auto render = [&](bool stroked) {
        QualityRaster r;
        QualityRaster::QualityFontDesc dd = d;
        dd.faceBytes = fontBytes->data();
        dd.faceSize = fontBytes->size();
        r.set_font(1, dd);
        r.frame_begin(64, 24, kBlack);
        if (stroked)
            r.text_stroked({8, 18}, "4", 1, kWhite, kNoClip);
        else
            r.text({8, 18}, "4", 1, kWhite, kNoClip);
        r.frame_end();
        return ink_count(r, 0, 0, 64, 24);
    };
    EXPECT_GT(render(true), render(false)); // fatter coverage
}

TEST(QualityText, GammaShapesTheCoverage) {
    REQUIRE_FONT();
    auto render = [&](float gamma) {
        QualityRaster r;
        QualityRaster::QualityFontDesc d;
        d.faceBytes = fontBytes->data();
        d.faceSize = fontBytes->size();
        d.px = 16;
        d.gamma = gamma;
        r.set_font(1, d);
        r.frame_begin(64, 24, kBlack);
        r.text({4, 18}, "8", 1, kWhite, kNoClip);
        r.frame_end();
        long sum = 0;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 64; ++x) sum += px(r, x, y)[0];
        return sum;
    };
    // gamma > 1 lifts partial coverage (fill^(1/gamma)): more total ink.
    EXPECT_GT(render(1.5f), render(1.0f));
}

} // namespace
} // namespace ui
