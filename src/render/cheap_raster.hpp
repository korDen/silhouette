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
#include "paint/painter.hpp"

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

enum class TextureMode { kReal, kSynthetic };

class CheapRaster {
  public:
    explicit CheapRaster(TextureMode mode = TextureMode::kReal) : mode_(mode) {}

    // Register or replace the pixels for an id. kSynthetic mode never reads
    // them (registration is allowed and ignored).
    void set_texture(TextureId id, TextureData t) { textures_[id] = t; }

    // ---- Sink concept ------------------------------------------------
    void frame_begin(uint32_t w, uint32_t h, Color clear);
    void quad(Rect dst, Color c, Rect clip);
    void image(Rect dst, TextureId t, Rect uv, Color tint, uint32_t flags,
               TextureId mask, Rect clip);
    void sweep(Rect dst, Color c, float a0, float a1, float frac, Rect clip);
    // Synthetic text: one run of per-byte cells at a baseline-left pen.
    // Normative layout, all in pixels (pinned by tests — changing any
    // constant is a breaking change to the equivalence gate):
    //   cell i = { pen.x + i*advance, pen.y - ascent + 0.1*px,
    //              0.85*advance, 0.75*px }
    //   fill   = color with alpha scaled by 0.4 + 0.4*hash01(byte, f.face)
    // Alignment and decorations (shadows, outlines) are producer-side
    // patterns over measure()/ascent(), not primitive properties. The
    // string is borrowed only for this call.
    void text(Vec2 pen, std::string_view s, Font f, Color c, Rect clip);
    void frame_end() {}

    // Synthetic metrics — no font backend, a pure function of the Font.
    // Normative: producers lay out against these, and both sides of any
    // comparison see the same values.
    float measure(Font f, std::string_view s) const {
        return 0.5f * f.px * static_cast<float>(s.size()); // advance = 0.5*px/byte
    }
    float ascent(Font f) const { return 0.8f * f.px; }
    float line_height(Font f) const { return 1.25f * f.px; }

    // ---- Result ------------------------------------------------------
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    // RGBA8, row-major, alpha always 255 (the canvas is opaque).
    const uint8_t* pixels() const { return buf_.data(); }

  private:
    void fill(Rect dst, Color c, Rect clip); // solid, src-over (quad + text cells)
    void blend_px(int x, int y, float r, float g, float b, float a, uint32_t mode);

    TextureMode mode_;
    ankerl::unordered_dense::map<TextureId, TextureData> textures_;
    std::vector<uint8_t> buf_;
    uint32_t w_ = 0, h_ = 0;
};

} // namespace ui
