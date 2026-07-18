#include "render/quality_raster.hpp"

// slughorn/render.hpp uses std::sort/std::unique and std::bit_cast without
// including them (third-party submodule, unpatched) — satisfy them first.
#include <algorithm>
#include <bit>

#include "text/font.hpp"

#include <slughorn/render.hpp>

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

namespace {

// Minimal UTF-8 decode (the sink's pinned encoding). Invalid sequences
// decode as U+FFFD and advance one byte — deterministic, never stuck.
uint32_t utf8_next(std::string_view s, size_t& i) {
    const uint8_t b0 = (uint8_t)s[i++];
    if (b0 < 0x80) return b0;
    uint32_t cp = 0;
    int extra = 0;
    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        extra = 1;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        extra = 2;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        extra = 3;
    } else {
        return 0xFFFD;
    }
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || ((uint8_t)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((uint8_t)s[i++] & 0x3F);
    }
    return cp;
}

} // namespace

// The text stage: one Font (face + atlas + shaping) per registered id, the
// registered-metrics rule, a per-glyph coverage-sampler cache, and the
// gamma LUT law applied to analytic fill.
struct QualityRaster::FontWorld {
    struct Entry {
        std::unique_ptr<Font> font; // null = metrics-only registration
        float px = 0, strokeWidth = 0, invGamma = 1;
        bool hasTable = false;
        float ascent = 0, lineHeight = 0;
        std::vector<float> adv; // per codepoint, COPIED at registration
        uint32_t first = 32;
        // scratch + caches (cleared when the atlas rebuilds)
        std::vector<ShapedGlyph> run;
        std::vector<uint32_t> gids;
        ankerl::unordered_dense::map<uint32_t, slughorn::render::Sampler>
            samplers;
    };
    ankerl::unordered_dense::map<FontId, Entry> fonts;

    Entry* find(FontId f) {
        auto it = fonts.find(f);
        return it == fonts.end() ? nullptr : &it->second;
    }
};

QualityRaster::QualityRaster(Sampler sampler)
    : sampler_(sampler), fonts_(std::make_unique<FontWorld>()) {}
QualityRaster::~QualityRaster() = default;

void QualityRaster::set_font(FontId id, const QualityFontDesc& d) {
    FontWorld::Entry e;
    e.px = d.px;
    e.strokeWidth = d.strokeWidth;
    e.invGamma = d.gamma > 0 ? 1.0f / d.gamma : 1.0f;
    if (d.metrics.count > 0 && d.metrics.advances != nullptr) {
        e.hasTable = true;
        e.ascent = d.metrics.ascent;
        e.lineHeight = d.metrics.lineHeight;
        e.first = d.metrics.firstCodepoint;
        e.adv.assign(d.metrics.advances, d.metrics.advances + d.metrics.count);
    }
    if (d.faceBytes != nullptr && d.faceSize > 0 && d.px > 0) {
        auto font = std::make_unique<Font>();
        if (font->load_from_memory(d.faceBytes, d.faceSize)) {
            if (d.strokeWidth > 0) {
                font->set_stroke_radius(d.strokeWidth / d.px); // px -> em
            }
            e.font = std::move(font);
        }
    }
    fonts_->fonts[id] = std::move(e);
}

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
    if (t == Texture::Invisible) return; // the empty id draws nothing
    const Span s = span_of(dst, clip, w_, h_);
    if (s.empty()) return;

    const uint32_t mode = flags & kBlendModeMask;
    const bool gray = (flags & kGrayscale) != 0;
    const bool tileU = (flags & kTileU) != 0;
    const bool tileV = (flags & kTileV) != 0;

    const MipTexture* tex = nullptr;
    bool missing = false;
    if (t >= Texture::FirstIndex) { // White/Black are synthesized, not stored
        const auto it = textures_.find(t);
        if (it != textures_.end() && !it->second.levels.empty())
            tex = &it->second;
        else
            missing = true;
    }

    // Level selection from the UV footprint in BASE texels per destination
    // pixel. Isotropic (maxAniso 1): the larger axis governs. Anisotropic:
    // the level comes from the minor axis and `taps` trilinear samples
    // integrate the major axis across one pixel's footprint — each tap then
    // covers major/taps ≈ minor texels, so a one-axis-minified draw keeps
    // the other axis at full detail (the hardware ANISOTROPIC model for
    // unrotated quads). lodBias applies after, clamped at the sharp end.
    int l0 = 0, l1 = 0;
    float lfrac = 0;
    int taps = 1;
    bool tapsAlongU = true;
    if (tex != nullptr) {
        const MipTexture::Level& base = tex->levels.front();
        const float tpx =
            std::max(1.0f, std::abs(uv.w) * (float)base.w / dst.w);
        const float tpy =
            std::max(1.0f, std::abs(uv.h) * (float)base.h / dst.h);
        const float major = std::max(tpx, tpy);
        const float minor = std::min(tpx, tpy);
        tapsAlongU = tpx >= tpy;
        if (sampler_.maxAniso > 1) {
            const float ratio =
                std::min((float)sampler_.maxAniso, major / minor);
            taps = std::max(1, (int)std::ceil(ratio - 1e-4f));
        }
        const float lambda = std::max(
            0.0f, std::log2(major / (float)taps) + sampler_.lodBias);
        l0 = std::min((int)tex->levels.size() - 1, (int)std::floor(lambda));
        l1 = std::min((int)tex->levels.size() - 1, l0 + 1);
        lfrac = std::min(1.0f, std::max(0.0f, lambda - (float)l0));
        if (l0 == l1) lfrac = 0;
    }
    // One destination pixel's footprint in UV, for the tap offsets.
    const float stepU = uv.w / dst.w;
    const float stepV = uv.h / dst.h;

    for (int y = s.y0; y < s.y1; ++y) {
        const float fy = ((float)y + 0.5f - dst.y) / dst.h;
        const float v = uv.y + fy * uv.h;
        for (int x = s.x0; x < s.x1; ++x) {
            const float fx = ((float)x + 0.5f - dst.x) / dst.w;
            const float u = uv.x + fx * uv.w;

            Rgba src;
            if (t == Texture::White) {
                src = {1, 1, 1, 1}; // solid white texel: tint is the fill
            } else if (t == Texture::Black) {
                src = {0, 0, 0, 1}; // solid black texel
            } else if (missing) {
                src = {1, 0, 1, 1}; // loud magenta
            } else {
                src = {0, 0, 0, 0};
                for (int i = 0; i < taps; ++i) {
                    const float off =
                        ((float)i + 0.5f) / (float)taps - 0.5f;
                    const float tu = tapsAlongU ? u + off * stepU : u;
                    const float tv = tapsAlongU ? v : v + off * stepV;
                    const MipTexture::Level& a = tex->levels[(size_t)l0];
                    Rgba tap =
                        sample_level(a.rgba, a.w, a.h, tu, tv, tileU, tileV);
                    if (lfrac > 0) {
                        const MipTexture::Level& b = tex->levels[(size_t)l1];
                        const Rgba s1 = sample_level(b.rgba, b.w, b.h, tu,
                                                     tv, tileU, tileV);
                        tap.r += (s1.r - tap.r) * lfrac;
                        tap.g += (s1.g - tap.g) * lfrac;
                        tap.b += (s1.b - tap.b) * lfrac;
                        tap.a += (s1.a - tap.a) * lfrac;
                    }
                    src.r += tap.r;
                    src.g += tap.g;
                    src.b += tap.b;
                    src.a += tap.a;
                }
                const float inv = 1.0f / (float)taps;
                src.r *= inv;
                src.g *= inv;
                src.b *= inv;
                src.a *= inv;
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

void QualityRaster::text(Vec2 pen, std::string_view s, FontId f, Color c,
                         Rect clip) {
    draw_run(pen, s, f, c, clip, false);
}

void QualityRaster::text_stroked(Vec2 pen, std::string_view s, FontId f,
                                 Color c, Rect clip) {
    draw_run(pen, s, f, c, clip, true);
}

// Unregistered fonts (and metrics-only registrations, which have no ink
// source) draw loud magenta cells — the same failure grammar as textures,
// impossible to miss, deterministic. Per byte at a fixed 10px.
void QualityRaster::loud_cells(Vec2 pen, std::string_view s, Rect clip) {
    const float px = 10, adv = 5;
    const float top = pen.y - 0.8f * px + 0.1f * px;
    for (size_t i = 0; i < s.size(); ++i) {
        fill_span({pen.x + (float)i * adv, top, 0.85f * adv, 0.75f * px},
                  {1, 0, 1, 1}, clip, kBlendNormal);
    }
}

void QualityRaster::draw_run(Vec2 pen, std::string_view s, FontId f, Color c,
                             Rect clip, bool stroked) {
    if (s.empty() || c.a <= 0) return;
    FontWorld::Entry* e = fonts_->find(f);
    if (e == nullptr || e->font == nullptr) {
        loud_cells(pen, s, clip);
        return;
    }
    Font& font = *e->font;
    const float px = e->px;
    // stroked degrades to the plain run where no stroker is declared —
    // the same doctrine the cheap rung pins.
    const bool wantStroked = stroked && e->strokeWidth > 0;

    // 1) The glyph run. THE METRICS RULE: with a registered table, glyphs
    //    are selected per codepoint and pens step by the table's numbers
    //    (face advance for codepoints outside it); without one, HarfBuzz
    //    shapes and its advances/offsets govern. Everything below is px.
    e->run.clear();
    if (e->hasTable) {
        size_t i = 0;
        while (i < s.size()) {
            const uint32_t cp = utf8_next(s, i);
            const uint32_t gid = font.glyph_index(cp);
            float adv;
            if (cp >= e->first && cp - e->first < e->adv.size()) {
                adv = e->adv[cp - e->first];
            } else {
                adv = font.advance_em(gid) * px;
            }
            e->run.push_back(ShapedGlyph{gid, adv, 0, 0, (int)i});
        }
    } else {
        font.shape(s, e->run);
        for (ShapedGlyph& g : e->run) { // em -> px
            g.x_advance *= px;
            g.x_offset *= px;
            g.y_offset *= px;
        }
    }

    // 2) Atlas registration (a rebuild invalidates every decoded sampler).
    e->gids.clear();
    for (const ShapedGlyph& g : e->run) {
        e->gids.push_back(wantStroked ? (g.gid | Font::kStrokedBit) : g.gid);
    }
    if (font.ensure_glyphs(e->gids)) {
        e->samplers.clear();
    }

    // 3) Pen walk: analytic banded coverage per pixel, through the gamma
    //    law, blended src-over. em rects expand a hair so edge AA is not
    //    clipped by the bearing box.
    constexpr float kExpand = 0.01f;
    float penX = pen.x;
    for (const ShapedGlyph& g : e->run) {
        const uint32_t key =
            wantStroked ? (g.gid | Font::kStrokedBit) : g.gid;
        const GlyphInfo* gi = font.glyph(key);
        if (gi != nullptr) {
            auto it = e->samplers.find(key);
            if (it == e->samplers.end()) {
                it = e->samplers
                         .emplace(key, slughorn::render::decode(font.atlas(),
                                                                key))
                         .first;
            }
            const slughorn::render::Sampler& smp = it->second;

            const float ox = penX + g.x_offset;
            const float oy = pen.y - g.y_offset; // em y-up
            const float emX0 = gi->bearingX - kExpand;
            const float emY0 = (gi->bearingY - gi->height) - kExpand;
            const float emX1 = gi->bearingX + gi->width + kExpand;
            const float emY1 = gi->bearingY + kExpand;
            const Rect quad{ox + emX0 * px, oy - emY1 * px,
                            (emX1 - emX0) * px, (emY1 - emY0) * px};
            const Span sp = span_of(quad, clip, w_, h_);
            const float emW = emX1 - emX0, emH = emY1 - emY0;
            const float ppeX = quad.w / emW, ppeY = quad.h / emH;
            for (int y = sp.y0; y < sp.y1; ++y) {
                const float fy = ((float)y + 0.5f - quad.y) / quad.h;
                const float ey = emY1 - fy * emH; // y-flip into em space
                for (int x = sp.x0; x < sp.x1; ++x) {
                    const float fx = ((float)x + 0.5f - quad.x) / quad.w;
                    const float ex = emX0 + fx * emW;
                    float fill = (float)smp.renderSampleBanded(ex, ey, ppeX,
                                                               ppeY)
                                     .fill;
                    if (fill <= 0) continue;
                    if (e->invGamma != 1.0f) {
                        fill = std::pow(fill, e->invGamma);
                    }
                    blend_px(x, y, c.r, c.g, c.b, c.a * fill, kBlendNormal);
                }
            }
        }
        penX += g.x_advance;
    }
}

float QualityRaster::measure(FontId f, std::string_view s) const {
    FontWorld::Entry* e = fonts_->find(f);
    if (e == nullptr) return 0;
    float w = 0;
    if (e->hasTable) {
        size_t i = 0;
        while (i < s.size()) {
            const uint32_t cp = utf8_next(s, i);
            if (cp >= e->first && cp - e->first < e->adv.size()) {
                w += e->adv[cp - e->first];
            } else if (e->font != nullptr) {
                w += e->font->advance_em(e->font->glyph_index(cp)) * e->px;
            }
        }
        return w;
    }
    if (e->font == nullptr) return 0;
    e->font->shape(s, e->run);
    for (const ShapedGlyph& g : e->run) w += g.x_advance;
    return w * e->px;
}

float QualityRaster::ascent(FontId f) const {
    FontWorld::Entry* e = fonts_->find(f);
    if (e == nullptr) return 0;
    if (e->hasTable) return e->ascent;
    return e->font != nullptr ? e->font->ascent() * e->px : 0;
}

float QualityRaster::line_height(FontId f) const {
    FontWorld::Entry* e = fonts_->find(f);
    if (e == nullptr) return 0;
    if (e->hasTable) return e->lineHeight;
    return e->font != nullptr ? e->font->line_height() * e->px : 0;
}

float QualityRaster::outline_width(FontId f) const {
    FontWorld::Entry* e = fonts_->find(f);
    return e == nullptr ? 0 : e->strokeWidth;
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
