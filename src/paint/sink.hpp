#pragma once
// The emit seam — how UI code draws.
//
// silhouette has NO retained canonical command buffer and NO emit-side
// state. Drawing is an immediate-mode call stream straight into a backend
// (a "sink"), and every call is fully determined by its arguments. What a
// backend retains — pixels, shaped glyphs, per-pipeline instance arrays —
// is its private choice, made in place as the calls arrive. The rules that
// are the point of the design:
//
//   - STRING LIFETIME: string arguments are borrowed for the DURATION OF
//     THE CALL only (printf's contract). No sink stores the pointer; a sink
//     that needs text later derives what it needs (shaped glyphs, hashed
//     coverage, pixels) before returning.
//   - ORDER: draw order is the call order (painter's algorithm). How a
//     backend preserves it — immediate rasterization, layer marks, sort
//     keys, depth — is the backend's decision, not this layer's.
//   - NO GROUP STATE: there are no offset/alpha/clip stacks. Producers pass
//     positions down and fold alpha into colors themselves — generated
//     drawing code does this naturally, and a call site's meaning never
//     depends on push/pop history. `clip` is plain per-call data (kNoClip
//     when unused). Group operations (transitions) return as a
//     producer-side layer when that feature does.
//
// The Sink concept — a documented convention. Sinks are concrete classes;
// producers and generated code bind them statically (no virtuals):
//
//   void  frame_begin(uint32_t w, uint32_t h, Color clear);
//   void  quad (Rect dst, Color c, uint32_t flags, Rect clip);
//   void  image(Rect dst, TextureId t, Rect uv, Color tint, uint32_t flags,
//               TextureId mask, Rect clip);
//   void  sweep(Rect dst, Color c, float a0, float a1, float frac,
//               TextureId mask, Rect clip);
//   void  text        (Vec2 pen, std::string_view s, FontId f, Color c, Rect clip);
//   void  text_stroked(Vec2 pen, std::string_view s, FontId f, Color c, Rect clip);
//   void  frame_end();
//
// quad() honors the blend-mode bits of `flags` (solid glows and tint washes
// are real); the sampling modifiers (grayscale/tile) are no-ops on a solid
// fill — grayscale of a white texel is the identity. sweep()'s mask is the
// shape the wedge is cut to, sampled across the destination rect exactly
// like image()'s mask (0 = none).
//   // the font surface — the sink owns its fonts (registered by id, like
//   // textures), so it answers every layout question a producer has:
//   float measure(FontId f, std::string_view s) const; // run advance width
//   float ascent(FontId f) const;                      // baseline from cell top
//   float line_height(FontId f) const;                 // default line cell
//   float outline_width(FontId f) const;               // stroke radius; 0 = none
//
// text() draws ONE run of glyphs at a baseline-left pen in one color.
// text_stroked() draws the font's pre-stroked variants — same layout, same
// pen advances, fatter coverage; real only where outline_width() > 0. A
// different glyph source is a different method: call sites read
// unambiguously (no boolean traps), and a GPU backend maps each onto its
// own pipeline with no per-call branch. Everything else a "label" bundles
// is a producer-side pattern, not a primitive property:
//   - alignment/centering  = arithmetic over measure()/line_height()
//   - a drop shadow        = the same run drawn first at +offset in another color
//   - an outline           = a stroked under-pass, or offset copies where the
//                            font declares no stroker
#include "core/geometry.hpp"

#include <cstdint>
#include <string_view>

namespace ui {

// Opaque texture identity, host-assigned. 0 = none.
using TextureId = uint32_t;

// Opaque font identity, host-assigned — one id per (face, size, stroker)
// combination, i.e. per named font resource. Sinks resolve it against their own
// registered font resources; producers never see face data.
using FontId = uint32_t;

// image() flags: blend mode in bits 0..1, modifier bits above. The blend
// vocabulary is the fixed-function set the GPU backends will select
// pipelines by; the CPU renderers implement the same arithmetic.
inline constexpr uint32_t kBlendNormal = 0;   // src-over
inline constexpr uint32_t kBlendAdditive = 1; // dst += src·a (glows, sheens)
inline constexpr uint32_t kBlendOverlay = 2;  // dst = 2·dst·src (multiply-brighten wash; ignores alpha)
inline constexpr uint32_t kBlendModeMask = 0x3;
inline constexpr uint32_t kGrayscale = 0x10; // luma-collapse the texel BEFORE tint
inline constexpr uint32_t kTileU = 0x20;     // wrap-sample U (repeat)
inline constexpr uint32_t kTileV = 0x40;     // wrap-sample V

} // namespace ui
