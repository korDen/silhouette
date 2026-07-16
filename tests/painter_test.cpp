// Painter<S> — pins the emit seam: group stacks (offset/alpha/clip) fold
// exactly once, here, and sinks receive fully resolved primitives. The
// RecordingSink below is also the living proof of the Sink concept: a plain
// struct with the six functions, statically bound, copying the borrowed
// string because a recorder is exactly the kind of sink that must do so
// before returning.
#include "paint/painter.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ui {
namespace {

struct Call {
    enum Kind { kFrameBegin, kQuad, kImage, kSweep, kText, kFrameEnd } kind;
    Rect dst{};
    Color color{};
    Rect clip{};
    // image
    TextureId tex = 0;
    Rect uv{};
    unsigned int flags = 0;
    TextureId mask = 0;
    // sweep
    float a0 = 0, a1 = 0, frac = 0;
    // text
    std::string str; // copied during the call — the string is only borrowed
    Font font{};
    TextStyle style{};
    // frame
    unsigned int w = 0, h = 0;
};

struct RecordingSink {
    std::vector<Call> calls;

    void frame_begin(unsigned int w, unsigned int h, Color clear) {
        Call c;
        c.kind = Call::kFrameBegin;
        c.w = w;
        c.h = h;
        c.color = clear;
        calls.push_back(c);
    }
    void quad(Rect dst, Color col, Rect clip) {
        Call c;
        c.kind = Call::kQuad;
        c.dst = dst;
        c.color = col;
        c.clip = clip;
        calls.push_back(c);
    }
    void image(Rect dst, TextureId t, Rect uv, Color tint, unsigned int flags,
               TextureId mask, Rect clip) {
        Call c;
        c.kind = Call::kImage;
        c.dst = dst;
        c.tex = t;
        c.uv = uv;
        c.color = tint;
        c.flags = flags;
        c.mask = mask;
        c.clip = clip;
        calls.push_back(c);
    }
    void sweep(Rect dst, Color col, float a0, float a1, float frac, Rect clip) {
        Call c;
        c.kind = Call::kSweep;
        c.dst = dst;
        c.color = col;
        c.a0 = a0;
        c.a1 = a1;
        c.frac = frac;
        c.clip = clip;
        calls.push_back(c);
    }
    void text(Rect dst, std::string_view s, Font f, const TextStyle& st,
              Color col, Rect clip) {
        Call c;
        c.kind = Call::kText;
        c.dst = dst;
        c.str = std::string(s); // consume before returning
        c.font = f;
        c.style = st;
        c.color = col;
        c.clip = clip;
        calls.push_back(c);
    }
    void frame_end() {
        Call c;
        c.kind = Call::kFrameEnd;
        calls.push_back(c);
    }
};

struct PainterTest : ::testing::Test {
    RecordingSink sink;
    Painter<RecordingSink> p{sink};

    void SetUp() override { p.frame_begin(64, 64, Color{0, 0, 0, 1}); }
    const Call& last() { return sink.calls.back(); }
};

TEST_F(PainterTest, FrameLifecycleForwards) {
    ASSERT_EQ(sink.calls.size(), 1u);
    EXPECT_EQ(last().kind, Call::kFrameBegin);
    EXPECT_EQ(last().w, 64u);
    EXPECT_EQ(last().h, 64u);
    p.frame_end();
    EXPECT_EQ(last().kind, Call::kFrameEnd);
}

TEST_F(PainterTest, QuadForwardsResolved) {
    p.quad({1, 2, 3, 4}, {0.5f, 0.25f, 1, 1});
    EXPECT_EQ(last().kind, Call::kQuad);
    EXPECT_TRUE(last().dst == (Rect{1, 2, 3, 4}));
    EXPECT_TRUE(last().color == (Color{0.5f, 0.25f, 1, 1}));
    EXPECT_TRUE(last().clip == kNoClip); // no clip pushed -> the sentinel
}

TEST_F(PainterTest, OffsetsNestAndUnwind) {
    p.push_offset({10, 20});
    p.quad({1, 1, 2, 2}, {});
    EXPECT_TRUE(last().dst == (Rect{11, 21, 2, 2}));
    p.push_offset({100, 0}); // nesting adds
    p.quad({1, 1, 2, 2}, {});
    EXPECT_TRUE(last().dst == (Rect{111, 21, 2, 2}));
    p.pop_offset();
    p.quad({1, 1, 2, 2}, {});
    EXPECT_TRUE(last().dst == (Rect{11, 21, 2, 2}));
    p.pop_offset();
    p.quad({1, 1, 2, 2}, {});
    EXPECT_TRUE(last().dst == (Rect{1, 1, 2, 2}));
}

TEST_F(PainterTest, AlphaNestsMultiplicatively) {
    p.push_alpha(0.5f);
    p.quad({0, 0, 1, 1}, {1, 1, 1, 0.8f});
    EXPECT_FLOAT_EQ(last().color.a, 0.4f);
    p.push_alpha(0.5f); // 0.5 inside 0.5 -> 0.25
    p.quad({0, 0, 1, 1}, {1, 1, 1, 1});
    EXPECT_FLOAT_EQ(last().color.a, 0.25f);
    p.pop_alpha();
    p.pop_alpha();
    p.quad({0, 0, 1, 1}, {1, 1, 1, 1});
    EXPECT_FLOAT_EQ(last().color.a, 1.0f);
}

TEST_F(PainterTest, AlphaReachesTextAndShadow) {
    TextStyle st;
    st.shadow = true;
    st.shadowColor = {0, 0, 0, 0.5f};
    p.push_alpha(0.5f);
    p.text({0, 0, 32, 8}, "hi", Font{1, 8}, st, {1, 1, 1, 1});
    EXPECT_FLOAT_EQ(last().color.a, 0.5f);
    EXPECT_FLOAT_EQ(last().style.shadowColor.a, 0.25f); // shadow fades too
    p.pop_alpha();
}

TEST_F(PainterTest, ClipIntersectsInTranslatedSpace) {
    p.push_offset({10, 10});
    p.push_clip({0, 0, 20, 20}); // local space -> absolute {10,10,20,20}
    p.quad({0, 0, 5, 5}, {});
    EXPECT_TRUE(last().clip == (Rect{10, 10, 20, 20}));
    p.push_clip({5, 5, 100, 100}); // nesting intersects
    p.quad({0, 0, 5, 5}, {});
    EXPECT_TRUE(last().clip == (Rect{15, 15, 15, 15}));
    p.pop_clip();
    p.pop_clip();
    p.pop_offset();
    p.quad({0, 0, 5, 5}, {});
    EXPECT_TRUE(last().clip == kNoClip);
}

TEST_F(PainterTest, ImageForwardsEverything) {
    p.push_offset({1, 1});
    p.push_alpha(0.5f);
    p.image({2, 2, 8, 8}, 7, {0.25f, 0, 0.5f, 1}, {1, 0, 0, 1},
            kBlendAdditive | kGrayscale, 9);
    p.pop_alpha();
    p.pop_offset();
    EXPECT_EQ(last().kind, Call::kImage);
    EXPECT_TRUE(last().dst == (Rect{3, 3, 8, 8}));
    EXPECT_EQ(last().tex, 7u);
    EXPECT_TRUE(last().uv == (Rect{0.25f, 0, 0.5f, 1}));
    EXPECT_FLOAT_EQ(last().color.a, 0.5f); // group alpha folded into tint
    EXPECT_EQ(last().flags, kBlendAdditive | kGrayscale);
    EXPECT_EQ(last().mask, 9u);
}

TEST_F(PainterTest, SweepForwardsAngles) {
    p.sweep({0, 0, 8, 8}, {0, 0, 0, 0.8f}, 0, 360, 0.25f);
    EXPECT_EQ(last().kind, Call::kSweep);
    EXPECT_FLOAT_EQ(last().a0, 0);
    EXPECT_FLOAT_EQ(last().a1, 360);
    EXPECT_FLOAT_EQ(last().frac, 0.25f);
}

TEST_F(PainterTest, TextIsBorrowedOnlyDuringTheCall) {
    // The sprintf pattern the contract exists for: the same buffer, reused.
    char buf[16];
    std::snprintf(buf, sizeof buf, "hello");
    p.text({0, 0, 64, 8}, buf, Font{1, 8}, {}, {});
    std::snprintf(buf, sizeof buf, "world");
    p.text({0, 0, 64, 8}, buf, Font{1, 8}, {}, {});
    ASSERT_EQ(sink.calls.size(), 3u); // frame_begin + 2 texts
    EXPECT_EQ(sink.calls[1].str, "hello");
    EXPECT_EQ(sink.calls[2].str, "world");
}

TEST_F(PainterTest, FrameBeginResetsGroupState) {
    p.push_offset({5, 5});
    p.push_alpha(0.5f);
    p.push_clip({0, 0, 8, 8});
    p.frame_begin(32, 32, {}); // a fresh frame forgets every group
    p.quad({1, 1, 2, 2}, {1, 1, 1, 1});
    EXPECT_TRUE(last().dst == (Rect{1, 1, 2, 2}));
    EXPECT_FLOAT_EQ(last().color.a, 1.0f);
    EXPECT_TRUE(last().clip == kNoClip);
    p.frame_end(); // balanced: the reset cleared the stacks
}

} // namespace
} // namespace ui
