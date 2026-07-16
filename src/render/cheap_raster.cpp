#include "render/cheap_raster.hpp"

#include <cmath>

namespace ui {
namespace {

// The one hash behind every synthesized value — a 32-bit splitmix-style
// finalizer. Deterministic across platforms by construction (pure integer
// arithmetic).
constexpr uint32_t mix32(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}
constexpr uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    return mix32(mix32(mix32(a) ^ b) ^ c);
}
// Uniform-ish [0,1) from two keys — the text-cell alpha source.
constexpr float hash01(uint32_t a, uint32_t b) {
    return static_cast<float>(hash3(a, b, 0x9e3779b9u) & 0xffffu) / 65536.0f;
}

// Synthetic-mode texel: hashed RGB, alpha in the mid-range [64,192] (see the
// header for why saturation corners are deliberately avoided).
struct Rgba {
    float r, g, b, a;
};
constexpr Rgba synthetic_texel(uint32_t id, uint32_t tx, uint32_t ty) {
    const uint32_t h = hash3(id, tx, ty);
    return {static_cast<float>(h & 255u) / 255.0f,
            static_cast<float>((h >> 8) & 255u) / 255.0f,
            static_cast<float>((h >> 16) & 255u) / 255.0f,
            static_cast<float>(64u + (h >> 24) % 129u) / 255.0f};
}
// The synthetic sampling grid: fine enough that sub-rect UVs and small
// offsets land on different cells, coarse enough that adjacent screen pixels
// usually share one (so a 1px position error still moves cell boundaries).
constexpr uint32_t kSyntheticGrid = 64;

// Pixel-center coverage of dst ∩ clip ∩ [0,w)x[0,h): pixel (x,y) is covered
// iff its center (x+0.5, y+0.5) lies inside the intersection.
struct Span {
    int x0, y0, x1, y1;
    bool empty() const { return x0 >= x1 || y0 >= y1; }
};
Span span_of(Rect dst, Rect clip, uint32_t w, uint32_t h) {
    const Rect r = intersect(dst, clip);
    Span s;
    s.x0 = static_cast<int>(std::ceil(r.x - 0.5f));
    s.y0 = static_cast<int>(std::ceil(r.y - 0.5f));
    s.x1 = static_cast<int>(std::ceil(r.right() - 0.5f));
    s.y1 = static_cast<int>(std::ceil(r.bottom() - 0.5f));
    if (s.x0 < 0) s.x0 = 0;
    if (s.y0 < 0) s.y0 = 0;
    if (s.x1 > static_cast<int>(w)) s.x1 = static_cast<int>(w);
    if (s.y1 > static_cast<int>(h)) s.y1 = static_cast<int>(h);
    if (r.empty()) s.x1 = s.x0; // empty intersection covers nothing
    return s;
}

constexpr uint8_t to_u8(float v) {
    const float c = v < 0 ? 0.0f : (v > 1 ? 1.0f : v);
    return static_cast<uint8_t>(c * 255.0f + 0.5f);
}

int wrap_or_clamp(float coord, uint32_t extent, bool wrap) {
    // `coord` is a [0,1]-space sampling coordinate (possibly outside when the
    // UV rect exceeds the unit square, or negative when flipped past 0).
    if (wrap) coord -= std::floor(coord);
    int t = static_cast<int>(std::floor(coord * static_cast<float>(extent)));
    if (t < 0) t = 0;
    if (t > static_cast<int>(extent) - 1) t = static_cast<int>(extent) - 1;
    return t;
}

Rgba fetch(const TextureData& t, int tx, int ty) {
    const uint8_t* p = t.rgba + (static_cast<size_t>(ty) * t.width + tx) * 4;
    return {static_cast<float>(p[0]) / 255.0f, static_cast<float>(p[1]) / 255.0f,
            static_cast<float>(p[2]) / 255.0f, static_cast<float>(p[3]) / 255.0f};
}

} // namespace

void CheapRaster::frame_begin(uint32_t w, uint32_t h, Color clear) {
    w_ = w;
    h_ = h;
    buf_.resize(static_cast<size_t>(w) * h * 4); // keeps capacity across frames
    const uint8_t cr = to_u8(clear.r), cg = to_u8(clear.g), cb = to_u8(clear.b);
    for (size_t i = 0; i < buf_.size(); i += 4) {
        buf_[i] = cr;
        buf_[i + 1] = cg;
        buf_[i + 2] = cb;
        buf_[i + 3] = 255;
    }
}

void CheapRaster::blend_px(int x, int y, float r, float g, float b, float a,
                           uint32_t mode) {
    uint8_t* p = &buf_[(static_cast<size_t>(y) * w_ + x) * 4];
    const float dr = static_cast<float>(p[0]) / 255.0f;
    const float dg = static_cast<float>(p[1]) / 255.0f;
    const float db = static_cast<float>(p[2]) / 255.0f;
    float or_, og, ob;
    switch (mode) {
    case kBlendAdditive: // dst += src·a
        or_ = dr + r * a;
        og = dg + g * a;
        ob = db + b * a;
        break;
    case kBlendOverlay: // dst = 2·dst·src — fixed-function multiply-brighten;
                        // alpha does not participate (documented in painter.hpp)
        or_ = 2 * dr * r;
        og = 2 * dg * g;
        ob = 2 * db * b;
        break;
    default: // src-over, straight alpha
        or_ = r * a + dr * (1 - a);
        og = g * a + dg * (1 - a);
        ob = b * a + db * (1 - a);
        break;
    }
    p[0] = to_u8(or_);
    p[1] = to_u8(og);
    p[2] = to_u8(ob);
    p[3] = 255;
}

void CheapRaster::fill(Rect dst, Color c, Rect clip) {
    const Span s = span_of(dst, clip, w_, h_);
    for (int y = s.y0; y < s.y1; ++y)
        for (int x = s.x0; x < s.x1; ++x)
            blend_px(x, y, c.r, c.g, c.b, c.a, kBlendNormal);
}

void CheapRaster::quad(Rect dst, Color c, uint32_t flags, Rect clip) {
    // Blend-mode bits apply to solids; sampling modifiers are no-ops (the
    // implicit texel is white, and grayscale of white is white).
    const uint32_t mode = flags & kBlendModeMask;
    const Span s = span_of(dst, clip, w_, h_);
    for (int y = s.y0; y < s.y1; ++y)
        for (int x = s.x0; x < s.x1; ++x)
            blend_px(x, y, c.r, c.g, c.b, c.a, mode);
}

float CheapRaster::mask_alpha(TextureId mask, float fx, float fy) const {
    if (mask == 0) return 1.0f;
    if (mode_ == TextureMode::kSynthetic) {
        return synthetic_texel(mask,
                               static_cast<uint32_t>(wrap_or_clamp(fx, kSyntheticGrid, false)),
                               static_cast<uint32_t>(wrap_or_clamp(fy, kSyntheticGrid, false)))
            .a;
    }
    const auto it = textures_.find(mask);
    if (it == textures_.end() || it->second.rgba == nullptr ||
        it->second.width == 0 || it->second.height == 0)
        return 1.0f; // unregistered masks are ignored
    const TextureData& md = it->second;
    return fetch(md, wrap_or_clamp(fx, md.width, false),
                 wrap_or_clamp(fy, md.height, false))
        .a;
}

void CheapRaster::image(Rect dst, TextureId t, Rect uv, Color tint,
                        uint32_t flags, TextureId mask, Rect clip) {
    if (dst.w <= 0 || dst.h <= 0) return;
    const Span s = span_of(dst, clip, w_, h_);
    if (s.empty()) return;

    const uint32_t mode = flags & kBlendModeMask;
    const bool gray = (flags & kGrayscale) != 0;
    const bool tileU = (flags & kTileU) != 0;
    const bool tileV = (flags & kTileV) != 0;

    const TextureData* td = nullptr;
    if (mode_ == TextureMode::kReal) {
        if (auto it = textures_.find(t); it != textures_.end() &&
                                         it->second.rgba != nullptr &&
                                         it->second.width && it->second.height)
            td = &it->second;
    }

    for (int y = s.y0; y < s.y1; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f - dst.y) / dst.h;
        const float v = uv.y + fy * uv.h;
        for (int x = s.x0; x < s.x1; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f - dst.x) / dst.w;
            const float u = uv.x + fx * uv.w;

            Rgba src;
            if (mode_ == TextureMode::kSynthetic) {
                src = synthetic_texel(t,
                                      static_cast<uint32_t>(wrap_or_clamp(u, kSyntheticGrid, tileU)),
                                      static_cast<uint32_t>(wrap_or_clamp(v, kSyntheticGrid, tileV)));
            } else if (td != nullptr) {
                src = fetch(*td, wrap_or_clamp(u, td->width, tileU),
                            wrap_or_clamp(v, td->height, tileV));
            } else {
                src = {1, 0, 1, 1}; // unregistered: loud magenta
            }

            // The alpha mask is the SHAPE the quad is cut to: it samples
            // across the destination extent (fx, fy), independent of the
            // color UV rect and of tiling (mask_alpha).
            src.a *= mask_alpha(mask, fx, fy);

            if (gray) { // luma BEFORE tint — the order tinted-gray art depends on
                const float yl = 0.299f * src.r + 0.587f * src.g + 0.114f * src.b;
                src.r = src.g = src.b = yl;
            }
            blend_px(x, y, src.r * tint.r, src.g * tint.g, src.b * tint.b,
                     src.a * tint.a, mode);
        }
    }
}

void CheapRaster::sweep(Rect dst, Color c, float a0, float a1, float frac,
                        TextureId mask, Rect clip) {
    if (frac <= 0 || dst.w <= 0 || dst.h <= 0) return;
    if (frac > 1) frac = 1;
    const Span s = span_of(dst, clip, w_, h_);
    if (s.empty()) return;

    // Angles in degrees: 0° at 12 o'clock, positive clockwise. The wedge
    // runs from a0 toward a1, covering `frac` of that arc; `mask` cuts it
    // to a shape, sampled across dst like image()'s mask.
    const float spanDeg = (a1 - a0) * frac;
    const float cx = dst.x + dst.w * 0.5f;
    const float cy = dst.y + dst.h * 0.5f;
    constexpr float kRadToDeg = 57.29577951308232f; // 180/pi

    for (int y = s.y0; y < s.y1; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f - dst.y) / dst.h;
        for (int x = s.x0; x < s.x1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            float ang = std::atan2(dx, -dy) * kRadToDeg; // 0 up, +clockwise
            if (ang < 0) ang += 360.0f;
            float d = spanDeg >= 0 ? ang - a0 : a0 - ang;
            d = std::fmod(d, 360.0f);
            if (d < 0) d += 360.0f;
            const float extent = spanDeg >= 0 ? spanDeg : -spanDeg;
            if (extent < 360.0f && d > extent) continue;
            float a = c.a;
            if (mask != 0) {
                const float fx = (static_cast<float>(x) + 0.5f - dst.x) / dst.w;
                a *= mask_alpha(mask, fx, fy);
            }
            if (a > 0) blend_px(x, y, c.r, c.g, c.b, a, kBlendNormal);
        }
    }
}

void CheapRaster::text(Vec2 pen, std::string_view str, FontId f, Color c,
                       Rect clip) {
    run_cells(pen, str, f, c, clip, 0.0f);
}

void CheapRaster::text_stroked(Vec2 pen, std::string_view str, FontId f,
                               Color c, Rect clip) {
    run_cells(pen, str, f, c, clip, outline_width(f));
}

void CheapRaster::run_cells(Vec2 pen, std::string_view str, FontId f, Color c,
                            Rect clip, float inflate) {
    if (str.empty()) return;
    // The normative synthetic layout — see the header. Pinned by tests.
    // One run at the baseline-left pen; decorations (shadows, outlines) and
    // alignment are producer-side patterns built on the font surface.
    float px = px_of(f);
    Color col = c;
    if (px <= 0) { // unregistered or degenerate: loud, deterministic
        px = 10;
        inflate = 0;
        col = {1, 0, 1, 1};
    }
    const float adv = 0.5f * px;
    const float top = pen.y - 0.8f * px + 0.1f * px;
    const float cellW = 0.85f * adv;
    const float cellH = 0.75f * px;
    for (size_t i = 0; i < str.size(); ++i) {
        const float a01 = 0.4f + 0.4f * hash01(static_cast<uint8_t>(str[i]), f);
        const Rect cell{pen.x + static_cast<float>(i) * adv - inflate,
                        top - inflate, cellW + 2 * inflate, cellH + 2 * inflate};
        fill(cell, faded(col, a01), clip);
    }
}

} // namespace ui
