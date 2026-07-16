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
//   void frame_begin(uint32_t w, uint32_t h, Color clear);
//   void quad (Rect dst, Color c, Rect clip);
//   void image(Rect dst, TextureId t, Rect uv, Color tint, uint32_t flags,
//              TextureId mask, Rect clip);
//   void sweep(Rect dst, Color c, float a0, float a1, float frac, Rect clip);
//   void text (Rect dst, std::string_view s, Font f, const TextStyle& st,
//              Color c, Rect clip);
//   void frame_end();
//
// Painter<S> is the one shared front-end over a sink: it owns the group
// stacks (offset / alpha / clip), folds them, and forwards fully resolved
// primitives (absolute rect, final alpha, absolute clip). Group math exists
// exactly once, here; sinks stay straight-line code.
#include "core/geometry.hpp"

#include <cassert>
#include <cstdint>
#include <string_view>

namespace ui {

// Opaque texture identity, host-assigned. 0 = none.
using TextureId = unsigned int;

// Font identity + pixel size. `face` is host-assigned and opaque; sinks
// resolve it against their own font resources (the cheap rasterizer needs
// none — it derives synthetic metrics from `px` alone).
struct Font {
    unsigned int face = 0;
    float px = 0;
};

enum Align : unsigned char { kAlignLeft = 0, kAlignCenter = 1, kAlignRight = 2 };
enum VAlign : unsigned char { kVAlignTop = 0, kVAlignCenter = 1, kVAlignBottom = 2 };

struct TextStyle {
    unsigned char align = kAlignLeft;
    unsigned char valign = kVAlignTop;
    bool shadow = false;  // one offset copy under the fill
    bool outline = false; // eight offset copies under the fill
    float offset = 1;     // shadow/outline offset, applied to both axes
    Color shadowColor{0, 0, 0, 1};
    float lineHeight = 0; // 0 = derive from the font size
};

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

    void text(Rect r, std::string_view s, Font f, const TextStyle& st, Color c) {
        TextStyle st2 = st;
        st2.shadowColor = faded(st.shadowColor, alpha_);
        sink_.text(r + offset_, s, f, st2, faded(c, alpha_), clip_);
    }

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
