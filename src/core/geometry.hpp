#pragma once
// Core geometry — the plain value types every layer shares. Everything here
// is constexpr: geometry that can fold at compile time does. Screen space is
// top-left origin, +y down; sizes are in pixels unless a caller says
// otherwise. No dependencies.

namespace ui {

struct Vec2 {
    float x = 0, y = 0;
};
constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
constexpr bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

// Axis-aligned rectangle: origin + extent. A rect with w<=0 or h<=0 is
// empty — it contains nothing and intersects nothing.
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr bool empty() const { return w <= 0 || h <= 0; }
    // Half-open: the top/left edges are inside, the bottom/right are not.
    constexpr bool contains(Vec2 p) const {
        return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
    }
};
constexpr Rect operator+(Rect r, Vec2 o) { return {r.x + o.x, r.y + o.y, r.w, r.h}; }
constexpr bool operator==(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// Intersection; disjoint inputs yield an empty rect (extent clamped to 0).
constexpr Rect intersect(Rect a, Rect b) {
    const float x0 = a.x > b.x ? a.x : b.x;
    const float y0 = a.y > b.y ? a.y : b.y;
    const float x1 = a.right() < b.right() ? a.right() : b.right();
    const float y1 = a.bottom() < b.bottom() ? a.bottom() : b.bottom();
    return {x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 > y0 ? y1 - y0 : 0.0f};
}

// Straight (non-premultiplied) RGBA, each channel nominally 0..1.
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};
constexpr bool operator==(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
// `c` with its alpha scaled by `s` — how group alpha folds into a primitive.
constexpr Color faded(Color c, float s) { return {c.r, c.g, c.b, c.a * s}; }

// Sentinel "no clip": large enough to contain any real surface, small enough
// that intersection arithmetic stays exact in float.
inline constexpr Rect kNoClip{-1.0e6f, -1.0e6f, 2.0e6f, 2.0e6f};

} // namespace ui
