#include "render/quality_raster.hpp"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

constexpr float kRadToDeg = 57.29577951308232f; // 180/pi

float clamp01(float v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); }

// Proper modulo for wrap sampling (negative-safe).
int wrap_index(int i, int n) {
    const int m = i % n;
    return m < 0 ? m + n : m;
}

int clamp_index(int i, int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); }

struct Rgba {
    float r, g, b, a;
};

// Bilinear tap on one float level, texel-center convention (fx = u*w - 0.5),
// per-tap wrap or clamp.
Rgba sample_level(const std::vector<float>& px, uint32_t w, uint32_t h,
                  float u, float v, bool tileU, bool tileV) {
    const float fx = u * (float)w - 0.5f;
    const float fy = v * (float)h - 0.5f;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    const int xi[2] = {tileU ? wrap_index(x0, (int)w) : clamp_index(x0, (int)w),
                       tileU ? wrap_index(x0 + 1, (int)w)
                             : clamp_index(x0 + 1, (int)w)};
    const int yi[2] = {tileV ? wrap_index(y0, (int)h) : clamp_index(y0, (int)h),
                       tileV ? wrap_index(y0 + 1, (int)h)
                             : clamp_index(y0 + 1, (int)h)};
    const float wgt[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty,
                          tx * ty};
    Rgba out{0, 0, 0, 0};
    const int pairs[4][2] = {{xi[0], yi[0]}, {xi[1], yi[0]},
                             {xi[0], yi[1]}, {xi[1], yi[1]}};
    for (int k = 0; k < 4; ++k) {
        const float* t =
            &px[((size_t)pairs[k][1] * w + (size_t)pairs[k][0]) * 4];
        out.r += wgt[k] * t[0];
        out.g += wgt[k] * t[1];
        out.b += wgt[k] * t[2];
        out.a += wgt[k] * t[3];
    }
    return out;
}

struct Span {
    int x0, y0, x1, y1;
    bool empty() const { return x0 >= x1 || y0 >= y1; }
};
// Pixel-center coverage of dst ∩ clip ∩ canvas — deliberately the same rule
// as the cheap rung (fractional-edge coverage is rejected: abutting chrome
// would double-composite its seams).
Span span_of(Rect dst, Rect clip, uint32_t w, uint32_t h) {
    const Rect r = intersect(dst, clip);
    Span s;
    s.x0 = std::max(0, (int)std::ceil(r.x - 0.5f));
    s.y0 = std::max(0, (int)std::ceil(r.y - 0.5f));
    s.x1 = std::min((int)w, (int)std::ceil(r.right() - 0.5f));
    s.y1 = std::min((int)h, (int)std::ceil(r.bottom() - 0.5f));
    if (r.empty()) s.x1 = s.x0;
    return s;
}

} // namespace

struct QualityRaster::FontWorld {}; // the text stage lands next

QualityRaster::QualityRaster() = default;
QualityRaster::~QualityRaster() = default;

void QualityRaster::set_texture(TextureId id, const uint8_t* rgba,
                                uint32_t width, uint32_t height) {
    if (rgba == nullptr || width == 0 || height == 0) return;
    MipTexture tex;
    MipTexture::Level base;
    base.w = width;
    base.h = height;
    base.rgba.resize((size_t)width * height * 4);
    for (size_t i = 0; i < base.rgba.size(); ++i)
        base.rgba[i] = (float)rgba[i] / 255.0f;
    tex.levels.push_back(std::move(base));
    // Area-filtered chain down to 1x1: each texel averages its (clamped)
    // 2x2 source block — exact box filtering in float, no re-quantization.
    while (tex.levels.back().w > 1 || tex.levels.back().h > 1) {
        const MipTexture::Level& src = tex.levels.back();
        MipTexture::Level next;
        next.w = std::max(1u, src.w / 2);
        next.h = std::max(1u, src.h / 2);
        next.rgba.resize((size_t)next.w * next.h * 4);
        for (uint32_t y = 0; y < next.h; ++y) {
            for (uint32_t x = 0; x < next.w; ++x) {
                const uint32_t sx0 = std::min(2 * x, src.w - 1);
                const uint32_t sx1 = std::min(2 * x + 1, src.w - 1);
                const uint32_t sy0 = std::min(2 * y, src.h - 1);
                const uint32_t sy1 = std::min(2 * y + 1, src.h - 1);
                for (int c = 0; c < 4; ++c) {
                    const float sum =
                        src.rgba[((size_t)sy0 * src.w + sx0) * 4 + c] +
                        src.rgba[((size_t)sy0 * src.w + sx1) * 4 + c] +
                        src.rgba[((size_t)sy1 * src.w + sx0) * 4 + c] +
                        src.rgba[((size_t)sy1 * src.w + sx1) * 4 + c];
                    next.rgba[((size_t)y * next.w + x) * 4 + c] = sum * 0.25f;
                }
            }
        }
        tex.levels.push_back(std::move(next));
    }
    textures_[id] = std::move(tex);
}

void QualityRaster::frame_begin(uint32_t w, uint32_t h, Color clear) {
    w_ = w;
    h_ = h;
    canvas_.resize((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        canvas_[i * 3 + 0] = clamp01(clear.r);
        canvas_[i * 3 + 1] = clamp01(clear.g);
        canvas_[i * 3 + 2] = clamp01(clear.b);
    }
}

void QualityRaster::blend_px(int x, int y, float r, float g, float b, float a,
                             uint32_t mode) {
    float* d = &canvas_[((size_t)y * w_ + x) * 3];
    // Per-operation clamping: a GPU UNORM render target clamps every draw's
    // result — the reference renderer matches that, never accumulating
    // out-of-range energy no backend could represent.
    switch (mode) {
    case kBlendAdditive:
        d[0] = clamp01(d[0] + r * a);
        d[1] = clamp01(d[1] + g * a);
        d[2] = clamp01(d[2] + b * a);
        break;
    case kBlendOverlay:
        d[0] = clamp01(2 * d[0] * r);
        d[1] = clamp01(2 * d[1] * g);
        d[2] = clamp01(2 * d[2] * b);
        break;
    default:
        d[0] = clamp01(r * a + d[0] * (1 - a));
        d[1] = clamp01(g * a + d[1] * (1 - a));
        d[2] = clamp01(b * a + d[2] * (1 - a));
        break;
    }
}

void QualityRaster::fill_span(Rect dst, Color c, Rect clip, uint32_t mode) {
    const Span s = span_of(dst, clip, w_, h_);
    for (int y = s.y0; y < s.y1; ++y)
        for (int x = s.x0; x < s.x1; ++x)
            blend_px(x, y, c.r, c.g, c.b, c.a, mode);
}

void QualityRaster::quad(Rect dst, Color c, uint32_t flags, Rect clip) {
    fill_span(dst, c, clip, flags & kBlendModeMask);
}

float QualityRaster::mask_alpha(TextureId mask, float fx, float fy) const {
    if (mask == 0) return 1.0f;
    const auto it = textures_.find(mask);
    if (it == textures_.end()) return 1.0f; // unregistered masks are ignored
    const MipTexture::Level& base = it->second.levels.front();
    return sample_level(base.rgba, base.w, base.h, fx, fy, false, false).a;
}

void QualityRaster::image(Rect dst, TextureId t, Rect uv, Color tint,
                          uint32_t flags, TextureId mask, Rect clip) {
    if (dst.w <= 0 || dst.h <= 0) return;
    const Span s = span_of(dst, clip, w_, h_);
    if (s.empty()) return;

    const uint32_t mode = flags & kBlendModeMask;
    const bool gray = (flags & kGrayscale) != 0;
    const bool tileU = (flags & kTileU) != 0;
    const bool tileV = (flags & kTileV) != 0;

    const MipTexture* tex = nullptr;
    bool missing = false;
    if (t != 0) {
        const auto it = textures_.find(t);
        if (it != textures_.end() && !it->second.levels.empty())
            tex = &it->second;
        else
            missing = true;
    }

    // Trilinear level selection from the UV footprint in BASE texels per
    // destination pixel (the larger axis governs).
    int l0 = 0, l1 = 0;
    float lfrac = 0;
    if (tex != nullptr) {
        const MipTexture::Level& base = tex->levels.front();
        const float tpx = std::abs(uv.w) * (float)base.w / dst.w;
        const float tpy = std::abs(uv.h) * (float)base.h / dst.h;
        const float t2 = std::max(1.0f, std::max(tpx, tpy));
        const float lambda = std::log2(t2);
        l0 = std::min((int)tex->levels.size() - 1, (int)std::floor(lambda));
        l1 = std::min((int)tex->levels.size() - 1, l0 + 1);
        lfrac = std::min(1.0f, std::max(0.0f, lambda - (float)l0));
        if (l0 == l1) lfrac = 0;
    }

    for (int y = s.y0; y < s.y1; ++y) {
        const float fy = ((float)y + 0.5f - dst.y) / dst.h;
        const float v = uv.y + fy * uv.h;
        for (int x = s.x0; x < s.x1; ++x) {
            const float fx = ((float)x + 0.5f - dst.x) / dst.w;
            const float u = uv.x + fx * uv.w;

            Rgba src;
            if (t == 0) {
                src = {1, 1, 1, 1}; // solid white texel: tint is the fill
            } else if (missing) {
                src = {1, 0, 1, 1}; // loud magenta
            } else {
                const MipTexture::Level& a = tex->levels[(size_t)l0];
                src = sample_level(a.rgba, a.w, a.h, u, v, tileU, tileV);
                if (lfrac > 0) {
                    const MipTexture::Level& b = tex->levels[(size_t)l1];
                    const Rgba s1 =
                        sample_level(b.rgba, b.w, b.h, u, v, tileU, tileV);
                    src.r += (s1.r - src.r) * lfrac;
                    src.g += (s1.g - src.g) * lfrac;
                    src.b += (s1.b - src.b) * lfrac;
                    src.a += (s1.a - src.a) * lfrac;
                }
            }

            src.a *= mask_alpha(mask, fx, fy);
            if (gray) { // luma BEFORE tint
                const float yl =
                    0.299f * src.r + 0.587f * src.g + 0.114f * src.b;
                src.r = src.g = src.b = yl;
            }
            blend_px(x, y, src.r * tint.r, src.g * tint.g, src.b * tint.b,
                     src.a * tint.a, mode);
        }
    }
}

void QualityRaster::sweep(Rect dst, Color c, float a0, float a1, float frac,
                          TextureId mask, Rect clip) {
    if (frac <= 0 || dst.w <= 0 || dst.h <= 0) return;
    if (frac > 1) frac = 1;
    const Span s = span_of(dst, clip, w_, h_);
    if (s.empty()) return;

    const float spanDeg = (a1 - a0) * frac;
    const float cx = dst.x + dst.w * 0.5f;
    const float cy = dst.y + dst.h * 0.5f;

    for (int y = s.y0; y < s.y1; ++y) {
        const float fy = ((float)y + 0.5f - dst.y) / dst.h;
        for (int x = s.x0; x < s.x1; ++x) {
            const float dx = (float)x + 0.5f - cx;
            const float dy = (float)y + 0.5f - cy;
            float ang = std::atan2(dx, -dy) * kRadToDeg; // 0 up, +clockwise
            if (ang < 0) ang += 360.0f;
            float d = spanDeg >= 0 ? ang - a0 : a0 - ang;
            d = std::fmod(d, 360.0f);
            if (d < 0) d += 360.0f;
            const float extent = spanDeg >= 0 ? spanDeg : -spanDeg;
            if (extent < 360.0f && d > extent) continue;
            float a = c.a;
            if (mask != 0) {
                const float fx = ((float)x + 0.5f - dst.x) / dst.w;
                a *= mask_alpha(mask, fx, fy);
            }
            if (a > 0) blend_px(x, y, c.r, c.g, c.b, a, kBlendNormal);
        }
    }
}

void QualityRaster::frame_end() {
    out_.resize((size_t)w_ * h_ * 4);
    for (size_t i = 0; i < (size_t)w_ * h_; ++i) {
        out_[i * 4 + 0] = (uint8_t)(canvas_[i * 3 + 0] * 255.0f + 0.5f);
        out_[i * 4 + 1] = (uint8_t)(canvas_[i * 3 + 1] * 255.0f + 0.5f);
        out_[i * 4 + 2] = (uint8_t)(canvas_[i * 3 + 2] * 255.0f + 0.5f);
        out_[i * 4 + 3] = 255;
    }
}

} // namespace ui
