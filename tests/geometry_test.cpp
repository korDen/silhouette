// Core geometry — pins the value-type semantics everything else builds on:
// half-open containment, empty-rect behavior, intersection (including the
// disjoint→empty clamp), translation, and alpha folding. The constexpr-ness
// itself is part of the contract, so several pins are static_asserts.
#include "core/geometry.hpp"

#include <gtest/gtest.h>

namespace ui {
namespace {

TEST(Vec2, Arithmetic) {
    static_assert(Vec2{1, 2} + Vec2{3, 4} == Vec2{4, 6});
    static_assert(Vec2{5, 7} - Vec2{2, 3} == Vec2{3, 4});
    static_assert(Vec2{1, -2} * 2.0f == Vec2{2, -4});
    EXPECT_TRUE((Vec2{1, 2} + Vec2{0, 0} == Vec2{1, 2}));
}

TEST(Rect, EdgesAndEmpty) {
    constexpr Rect r{10, 20, 30, 40};
    static_assert(r.right() == 40);
    static_assert(r.bottom() == 60);
    static_assert(!r.empty());
    static_assert(Rect{0, 0, 0, 10}.empty());
    static_assert(Rect{0, 0, 10, -1}.empty());
    EXPECT_TRUE(Rect{}.empty()); // default rect is empty, not 1x1
}

TEST(Rect, ContainsIsHalfOpen) {
    constexpr Rect r{0, 0, 10, 10};
    static_assert(r.contains({0, 0}));    // top-left edge inside
    static_assert(r.contains({9.5f, 9.5f}));
    static_assert(!r.contains({10, 0}));  // right edge outside
    static_assert(!r.contains({0, 10}));  // bottom edge outside
    static_assert(!r.contains({-0.1f, 5}));
    EXPECT_FALSE((Rect{0, 0, 0, 0}.contains({0, 0}))); // empty holds nothing
}

TEST(Rect, Translate) {
    static_assert(Rect{1, 2, 3, 4} + Vec2{10, 20} == Rect{11, 22, 3, 4});
    EXPECT_TRUE((Rect{1, 2, 3, 4} + Vec2{0, 0} == Rect{1, 2, 3, 4}));
}

TEST(Rect, IntersectOverlap) {
    static_assert(intersect({0, 0, 10, 10}, {5, 5, 10, 10}) == Rect{5, 5, 5, 5});
    // containment: the smaller rect wins
    static_assert(intersect({0, 0, 10, 10}, {2, 3, 4, 5}) == Rect{2, 3, 4, 5});
    // identity against the no-clip sentinel
    static_assert(intersect(kNoClip, {1, 2, 3, 4}) == Rect{1, 2, 3, 4});
    EXPECT_TRUE((intersect({0, 0, 4, 4}, {0, 0, 4, 4}) == Rect{0, 0, 4, 4}));
}

TEST(Rect, IntersectDisjointIsEmpty) {
    constexpr Rect d = intersect({0, 0, 10, 10}, {20, 0, 5, 5});
    static_assert(d.empty());
    static_assert(d.w == 0); // extent clamps to zero, never negative
    constexpr Rect t = intersect({0, 0, 10, 10}, {10, 0, 5, 5}); // edge-touch
    static_assert(t.empty());
    EXPECT_TRUE(intersect({0, 0, 5, 5}, {0, 30, 5, 5}).empty());
}

TEST(Color, FadedScalesAlphaOnly) {
    constexpr Color c{0.5f, 0.25f, 1.0f, 0.8f};
    static_assert(faded(c, 0.5f) == Color{0.5f, 0.25f, 1.0f, 0.4f});
    static_assert(faded(c, 1.0f) == c);
    static_assert(faded(c, 0.0f).a == 0.0f);
    EXPECT_TRUE(faded(Color{}, 0.5f) == (Color{1, 1, 1, 0.5f}));
}

} // namespace
} // namespace ui
