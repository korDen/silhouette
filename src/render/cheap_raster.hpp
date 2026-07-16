#pragma once
// CheapRaster — the fast, deliberately inaccurate CPU sink. Its purpose is
// to answer "do these two call streams produce the same image?" instantly,
// and to give a rough-but-real look at a frame. It is the workhorse of the
// automated suite; the quality rasterizer (later) owns fidelity, the GPU
// backends own speed.
//
// Cheapness, by design:
//   - nearest-neighbour sampling, base level only (no mipmaps, no filtering)
//   - hard-edge coverage: a pixel is filled iff its CENTER lies inside
//     dst ∩ clip ∩ canvas
//   - synthetic text: per-byte cells positioned by normative arithmetic
//     (documented at text()), filled with content-hashed alpha — placement-
//     and content-sensitive with no font backend at all
//
// Two texture modes:
//   kReal      — sample host-registered RGBA8 textures (scene eyeballing).
//                An unregistered id draws opaque magenta: impossible to miss.
//   kSynthetic — NEVER touches texture data; a texel is a hash of
//                (texture id, cell x, cell y) on a fixed 64x64 grid. Alpha
//                maps into the mid-range [64,192] so every layer keeps
//                influencing the composite: overdraw cannot hide a wrong
//                command underneath, and blend saturation corners (additive
//                pinned at 255, overlay at 0) stay off the common path.
//                Match tests need no texture bytes at all.
//
// Determinism: the same call stream in the same mode (and, in kReal, the
// same registered textures) produces byte-identical buffers. That is the
// property render/pixel_match.hpp builds on.
#include "core/geometry.hpp"
#include "paint/sink.hpp"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace ui {

// A host-owned RGBA8 texture: row-major, tightly packed, non-owning. The
// pointer must stay valid while frames render against it.
struct TextureData {
    const uint8_t* rgba = nullptr;
    uint32_t width = 0, height = 0;
};

// What the cheap sink knows about a registered font: the pixel size its
// synthetic metrics derive from, and the declared outline stroke radius
// (0 = the font has no pre-stroked variants; producers then fall back to
// offset under-passes). Richer backends register real face data instead.
struct FontDesc {
    float px = 0;
    float strokeWidth = 0;
};

enum class TextureMode { kReal, kSynthetic };

class CheapRaster {
  public:
    explicit CheapRaster(TextureMode mode = TextureMode::kReal) : mode_(mode) {}

    // Register or replace the pixels for an id. kSynthetic mode never reads
    // them (registration is allowed and ignored).
    void set_texture(TextureId id, TextureData t) { textures_[id] = t; }

    // Register or replace a font id. Both texture modes use fonts: metrics
    // and text cells derive from the registered px, and outline_width
    // reports the registered stroke radius so producers take the same
    // stroked-vs-fallback branch they would against a real backend.
    void set_font(FontId id, FontDesc d) { fonts_[id] = d; }

    // ---- Sink concept ------------------------------------------------
    void frame_begin(uint32_t w, uint32_t h, Color clear);
    // Solid fill. Blend-mode bits of `flags` apply (additive/overlay);
    // sampling modifiers are no-ops on a solid (grayscale of white = white).
    void quad(Rect dst, Color c, uint32_t flags, Rect clip);
    void image(Rect dst, TextureId t, Rect uv, Color tint, uint32_t flags,
               TextureId mask, Rect clip);
    // Radial wedge, optionally cut to `mask` (sampled across dst, like
    // image()'s mask; 0 = none, unregistered masks are ignored in kReal).
    void sweep(Rect dst, Color c, float a0, float a1, float frac,
               TextureId mask, Rect clip);
    // Synthetic text: one run of per-byte cells at a baseline-left pen.
    // Normative layout, all in pixels of the REGISTERED font size (pinned
    // by tests — changing any constant is a breaking change to the
    // equivalence gate):
    //   cell i = { pen.x + i*advance, pen.y - ascent + 0.1*px,
    //              0.85*advance, 0.75*px }
    //   fill   = color with alpha scaled by 0.4 + 0.4*hash01(byte, f)
    // text_stroked draws the same run with every cell inflated by the
    // registered strokeWidth on all sides — same pen advances, fatter
    // coverage. An unregistered (or degenerate px<=0) font draws loud
    // magenta cells at a fixed 10px so the failure is impossible to miss.
    // Alignment and decorations are producer-side patterns over the font
    // surface. The string is borrowed only for the call.
    void text(Vec2 pen, std::string_view s, FontId f, Color c, Rect clip);
    void text_stroked(Vec2 pen, std::string_view s, FontId f, Color c,
                      Rect clip);
    void frame_end() {}

    // The font surface — synthetic metrics, normative pure functions of
    // the registered px. Unregistered fonts answer 0 everywhere.
    float measure(FontId f, std::string_view s) const {
        return 0.5f * px_of(f) * static_cast<float>(s.size()); // 0.5*px/byte
    }
    float ascent(FontId f) const { return 0.8f * px_of(f); }
    float line_height(FontId f) const { return 1.25f * px_of(f); }
    float outline_width(FontId f) const {
        const auto it = fonts_.find(f);
        return it == fonts_.end() ? 0.0f : it->second.strokeWidth;
    }

    // ---- Result ------------------------------------------------------
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    // RGBA8, row-major, alpha always 255 (the canvas is opaque).
    const uint8_t* pixels() const { return buf_.data(); }

  private:
    void fill(Rect dst, Color c, Rect clip); // solid, src-over (text cells)
    void blend_px(int x, int y, float r, float g, float b, float a, uint32_t mode);
    void run_cells(Vec2 pen, std::string_view s, FontId f, Color c, Rect clip,
                   float inflate);
    // The mask rule shared by image() and sweep(): alpha of `mask` sampled
    // at the destination fraction (fx, fy); 1 when mask == 0 or, in kReal,
    // when the mask id is unregistered.
    float mask_alpha(TextureId mask, float fx, float fy) const;
    float px_of(FontId f) const {
        const auto it = fonts_.find(f);
        return it == fonts_.end() ? 0.0f : it->second.px;
    }

    TextureMode mode_;
    ankerl::unordered_dense::map<TextureId, TextureData> textures_;
    ankerl::unordered_dense::map<FontId, FontDesc> fonts_;
    std::vector<uint8_t> buf_;
    uint32_t w_ = 0, h_ = 0;
};

} // namespace ui
