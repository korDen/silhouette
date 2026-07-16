#pragma once
// The emit seam — how UI code draws.
//
// silhouette has NO retained canonical command buffer. Drawing is an
// immediate-mode call stream into a backend (a "sink"); what a backend
// retains — pixels, shaped glyphs, per-pipeline instance arrays — is its
// private choice, made in place as the calls arrive. Two consequences are
// the point of the design:
//
//   - STRING LIFETIME: string arguments are borrowed for the DURATION OF THE
//     CALL only (printf's contract). No sink stores the pointer; a sink that
//     needs text later derives what it needs (shaped glyphs, hashed
//     coverage, pixels) before returning.
//   - ORDER: draw order is the call order (painter's algorithm). How a
//     backend preserves it — immediate rasterization, layer marks, sort
//     keys, depth — is the backend's decision, not this layer's.
//
// The Sink concept (statically bound; no virtuals on the hot path):
//
//   void  frame_begin(uint32_t w, uint32_t h, Color clear);
//   void  quad (Rect dst, Color c, Rect clip);
//   void  image(Rect dst, TextureId t, Rect uv, Color tint, uint32_t flags,
//               TextureId mask, Rect clip);
//   void  sweep(Rect dst, Color c, float a0, float a1, float frac, Rect clip);
//   void  text (Vec2 pen, std::string_view s, FontId f, Color c, Rect clip,
//               bool stroked);
//   void  frame_end();
//   // the font surface — the sink owns its fonts (registered by id, like
//   // textures), so it answers every layout question a producer has:
//   float measure(FontId f, std::string_view s) const; // run advance width
//   float ascent(FontId f) const;                      // baseline from cell top
//   float line_height(FontId f) const;                 // default line cell
//   float outline_width(FontId f) const;               // stroke radius; 0 = none
//
// text() is deliberately minimal: ONE run of glyphs at a baseline-left pen
// position in one color. `stroked` selects the font's pre-stroked glyph
// variants (same pen advances, fatter coverage) — the outline pass, real
// only where outline_width() > 0. Everything else a "label" bundles is a
// producer-side pattern, not a primitive property:
//   - alignment/centering  = arithmetic over measure()/line_height()
//   - a drop shadow        = the same run drawn first at +offset in another color
//   - an outline           = a stroked under-pass, or offset copies where the
//                            font declares no stroker
// Keeping those out of the sink keeps every backend's text path a straight
// line, and keeps host-specific layout rules (baseline conventions, cell
// rounding) in the host where they belong.
//
// Painter<S> is the one shared front-end over a sink: it owns the group
// stacks (offset / alpha / clip), folds them, and forwards fully resolved
// primitives (absolute coordinates, final alpha, absolute clip). Group math
// exists exactly once, here; sinks stay straight-line code.
#include "core/geometry.hpp"

#include <cassert>
#include <cstdint>
#include <string_view>

namespace ui {

// Opaque texture identity, host-assigned. 0 = none.
using TextureId = unsigned int;

// Opaque font identity, host-assigned — one id per (face, size, stroker)
// combination, i.e. per named font resource. Sinks resolve it against their own
// registered font resources; producers never see face data.
using FontId = unsigned int;

// image() flags: blend mode in bits 0..1, modifier bits above. The blend
// vocabulary is the fixed-function set the GPU backends will select
// pipelines by; the CPU renderers implement the same arithmetic.
inline constexpr unsigned int kBlendNormal = 0;   // src-over
inline constexpr unsigned int kBlendAdditive = 1; // dst += src·a (glows, sheens)
inline constexpr unsigned int kBlendOverlay = 2;  // dst = 2·dst·src (multiply-brighten wash; ignores alpha)
inline constexpr unsigned int kBlendModeMask = 0x3;
inline constexpr unsigned int kGrayscale = 0x10; // luma-collapse the texel BEFORE tint
inline constexpr unsigned int kTileU = 0x20;     // wrap-sample U (repeat)
inline constexpr unsigned int kTileV = 0x40;     // wrap-sample V

template <class S>
class Painter {
  public:
    explicit Painter(S& sink) : sink_(sink) {}

    void frame_begin(unsigned int w, unsigned int h, Color clear) {
        offsets_n_ = 0;
        alphas_n_ = 0;
        clips_n_ = 0;
        offset_ = {0, 0};
        alpha_ = 1;
        clip_ = kNoClip;
        sink_.frame_begin(w, h, clear);
    }
    void frame_end() {
        assert(offsets_n_ == 0 && alphas_n_ == 0 && clips_n_ == 0 &&
               "unbalanced push/pop at frame_end");
        sink_.frame_end();
    }

    // Group translate: shifts everything drawn until the matching pop.
    // Nesting adds.
    void push_offset(Vec2 o) {
        assert(offsets_n_ < kDepth && "offset stack overflow");
        offsets_[offsets_n_++] = offset_;
        offset_ = offset_ + o;
    }
    void pop_offset() {
        assert(offsets_n_ > 0 && "pop_offset without push");
        offset_ = offsets_[--offsets_n_];
    }

    // Group alpha: multiplies into everything drawn until the matching pop.
    // Nesting multiplies (0.5 inside 0.5 -> 0.25).
    void push_alpha(float a) {
        assert(alphas_n_ < kDepth && "alpha stack overflow");
        alphas_[alphas_n_++] = alpha_;
        alpha_ *= a;
    }
    void pop_alpha() {
        assert(alphas_n_ > 0 && "pop_alpha without push");
        alpha_ = alphas_[--alphas_n_];
    }

    // Group clip: `r` is in the CURRENT translated space, like everything
    // drawn. Nesting intersects.
    void push_clip(Rect r) {
        assert(clips_n_ < kDepth && "clip stack overflow");
        clips_[clips_n_++] = clip_;
        clip_ = intersect(clip_, r + offset_);
    }
    void pop_clip() {
        assert(clips_n_ > 0 && "pop_clip without push");
        clip_ = clips_[--clips_n_];
    }

    void quad(Rect r, Color c) { sink_.quad(r + offset_, faded(c, alpha_), clip_); }

    void image(Rect r, TextureId t, Rect uv = {0, 0, 1, 1}, Color tint = {},
               unsigned int flags = 0, TextureId mask = 0) {
        sink_.image(r + offset_, t, uv, faded(tint, alpha_), flags, mask, clip_);
    }

    void sweep(Rect r, Color c, float a0, float a1, float frac) {
        sink_.sweep(r + offset_, faded(c, alpha_), a0, a1, frac, clip_);
    }

    // `pen` is the run's baseline-left origin. The string is borrowed for
    // the duration of the call. stroked=true draws the font's pre-stroked
    // glyph variants with the same pen advances (the outline under-pass).
    void text(Vec2 pen, std::string_view s, FontId f, Color c,
              bool stroked = false) {
        sink_.text(pen + offset_, s, f, faded(c, alpha_), clip_, stroked);
    }

    // The font surface passes through to the sink so a producer holding
    // only the painter can place text (centering, right-alignment,
    // fit-to-text, stroked-or-fallback outlines).
    float measure(FontId f, std::string_view s) const { return sink_.measure(f, s); }
    float ascent(FontId f) const { return sink_.ascent(f); }
    float line_height(FontId f) const { return sink_.line_height(f); }
    float outline_width(FontId f) const { return sink_.outline_width(f); }

  private:
    static constexpr int kDepth = 32; // deepest real UI trees sit well under this

    S& sink_;
    Vec2 offset_{};
    float alpha_ = 1;
    Rect clip_ = kNoClip;
    Vec2 offsets_[kDepth];
    float alphas_[kDepth];
    Rect clips_[kDepth];
    int offsets_n_ = 0, alphas_n_ = 0, clips_n_ = 0;
};

} // namespace ui
