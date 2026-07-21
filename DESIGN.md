# silhouette — design

silhouette is a **production-grade, cross-platform UI rendering library**. Its
job is to raise the bar on code quality and performance. Today it ships a
deliberately **minimal** feature set; the architecture treats the full set
(rich layout, widgets, a markup compiler, GPU backends) as first-class future
work, never as bolt-ons. The library is general-purpose — no application-,
game-, or vendor-specific concept appears anywhere in it.

## Non-negotiables

1. **GPU is first-class.** The emit architecture is designed for
   high-performance GPU rendering (Vulkan, then Metal). CPU rendering exists
   for testing and goldens and must never be a reason to compromise the GPU
   path. The GPU backends are critical deliverables — merely sequenced after
   the CPU renderers, and never required by the automated test suite.
2. **Production-quality text.** High-quality text via analytic glyph coverage
   (slughorn) with HarfBuzz shaping. A permanent library capability, owned by
   the quality renderer and the GPU backends.
3. **Full coverage, every commit.** *Every line a commit changes is exercised
   by at least one test that lands in the same commit.* A change without a
   test for it is not ready. See "Coverage discipline".
4. **Fast tests.** Core unit tests run in milliseconds and need no external
   assets. A test too slow to run on every change gets redesigned or cut.
5. **Clean, general vocabulary.** Primitives and APIs are named for what they
   are in UI terms. No leakage of any consuming application's domain.

## Architecture: immediate-mode sinks, no canonical command buffer

silhouette has **no retained canonical draw list**. Drawing is an
immediate-mode call stream into a backend (a **sink**); what a backend retains
— pixels, shaped glyphs, per-pipeline instance arrays — is its private choice,
made in place as the calls arrive.

Why this shape (both reasons are load-bearing):

- **String lifetime.** A retained command buffer must either store caller
  pointers (dangling: the caller's stack buffer is reused before the flush) or
  copy every string into a canonical arena that all backends must share.
  Instead, string arguments are **borrowed for the duration of the call only**
  — printf's contract. A sink that needs text later derives what it needs
  (shaped glyphs, hashed coverage, pixels) *before returning*.
- **Ordering/batching is a backend decision.** A CPU rasterizer wants strict
  painter order and zero z-machinery. One GPU backend may want per-pipeline
  instanced arrays with layer marks; another may aggregate by shader or sort
  key. Baking any one strategy into a shared wire format forces it on every
  backend — and is exactly where cross-pipeline z-order bugs come from. Sinks
  receive a strict painter-order stream; what each builds from it is private.

The seam is one thing (`src/paint/sink.hpp`): **the Sink concept** — the
emit functions plus the font surface, statically bound, no virtuals on the
hot path. Producers and generated drawing code call sinks directly. There is
**no emit-side state**: no offset/alpha/clip stacks, no shared front-end.
Producers pass positions down and fold alpha into colors themselves —
generated code does this naturally, every call is fully determined by its
arguments, and backend complexity is the metric that matters (emit paths are
generated; backends are hand-maintained). `clip` is plain per-call data.
Group operations (transitions) return as a producer-side layer when that
feature does. A type-erased adapter (function-pointer table) can be added if
a tool ever needs runtime backend switching.

**Producer-side memoization is legal; command retention is not.** A producer
(hand-written or generated) may retain its own *inputs and derived values*
across frames — solved geometry, measured widths, a projection of what it
read — in caller-owned storage, and use them to skip recomputation. What it
may not do is retain and replay *the call stream itself*: the sink receives
a full, freshly-issued immediate stream every frame, every call still fully
determined by its arguments, so the two reasons above keep holding (no
stored strings, no baked ordering). The generated retained context is the
canonical example: it gates the layout solve on the fields that can move
geometry and always re-emits the draws. Correctness bar: a producer's
memoized frame must be byte-identical to its stateless render of the same
inputs — cached-vs-fresh equivalence is a gate, not an aspiration.

## The primitive set (current)

Chosen so the current consumers' full visual vocabulary maps 1:1 onto native
calls — and no larger:

- **quad** — solid color rectangle.
- **image** — textured rectangle: UV sub-rect (negative extent = flip), tint,
  blend mode (normal / additive `dst+=src·a` / overlay `dst=2·dst·src`),
  grayscale (luma-collapse **before** tint), tile-U/V wrap, optional
  **alpha-mask** second texture (sampled across the destination rect — the
  shape the quad is cut to, independent of the color UV).
- **sweep** — radial progress wedge: angle range, covered fraction, flat
  color. Angles in degrees, 0° at 12 o'clock, positive clockwise.
- **text / text_stroked** — ONE run of glyphs at a baseline-left pen
  position: string (call-duration borrowed) + font id + color.
  `text_stroked` draws the font's pre-stroked variants (same layout, same
  pen advances, fatter coverage) — a different glyph source is a different
  method, so call sites read unambiguously and GPU backends map each onto
  its own pipeline. Deliberately minimal: alignment is producer arithmetic
  over the sink's font surface; a shadow is the same run drawn first at an
  offset in another color; an outline is a stroked under-pass where the
  font declares a stroker, offset copies where it does not. Label semantics
  live in producers, not in backends.

Fonts mirror textures: hosts register them by opaque id (`FontId` — one id
per face/size/stroker combination), and the sink answers every layout
question a producer has: `measure(id, s)`, `ascent(id)`, `line_height(id)`,
`outline_width(id)` (0 = no stroked variants). The cheap renderer's font
world is synthetic — normative formulas of the registered pixel size — so
producers take the same layout branches they would against a real backend,
with no font bytes involved.

## The render ladder (build order)

1. **Cheap CPU renderer** (`render/cheap_raster`) — fast and deliberately
   inaccurate: nearest-neighbour, base level only, hard-edge coverage,
   synthetic text. Two texture modes:
   - **real** — samples host-registered RGBA8 textures; for eyeballing real
     frames. Unregistered textures draw magenta.
   - **synthetic** — never touches texture data; texels are hashed from
     (texture id, cell). Alpha maps into a mid-range band so every layer keeps
     influencing the composite: overdraw cannot hide a wrong command
     underneath, and blend saturation corners stay off the common path.
   This renderer is the workhorse of the automated suite.
2. **Quality CPU renderer** — the fidelity bar-raiser: gamma-aware
   compositing, proper filtering, analytic glyph coverage via slughorn with
   HarfBuzz shaping. Purpose: golden images and reference output.
3. **Vulkan backend** — highest performance; judged against the quality CPU
   renderer within a ± threshold.
4. **Metal backend** — same bar, same judge.

Nothing is ever judged against screenshots.

## Equivalence is pixels, not bytes

Two call streams that differ in emission order or representation can produce
the identical image. The equivalence relation between two producers of the
same UI is **"same pixels out of the same cheap-renderer configuration"**
(`render/pixel_match.hpp`), never a byte-compare of command buffers. Because
the cheap renderer draws *everything* — including text, as hashed coverage —
the relation is:

- **order-tolerant** where order cannot matter (non-overlapping draws),
- **strict** where it must (overlapping draws composite in call order),
- **content- and placement-sensitive** (a wrong texture, color, glyph, flag,
  or position changes the pixels).

Determinism underwrites this: same call stream + same configuration →
byte-identical buffers. (Output hashing as a frame-skip optimization is a
possible backend detail, not a library contract.)

## Deferred — scope, not a ceiling

Rich layout (flex), interactive widgets, animation, and the markup compiler
are intentionally outside the current build. They return once the new markup
format is defined; the architecture reserves them first-class seats.

## Coverage discipline — how it is enforced

- Every commit builds and passes the full suite on the working platform
  before it lands; the suite is fast enough that this is never skipped.
- Every line a commit changes is exercised by at least one test in that same
  commit. Tests assert **behavior and pixels**, not internals: primitives are
  verified by rendering tiny frames through the cheap renderer and asserting
  exact pixel values; the quality renderer will be verified against small
  goldens; GPU backends against the quality renderer within threshold.
- New code lands with warnings-as-errors at the highest practical warning
  level.

## Layout

```
src/core/      geometry value types (constexpr)
src/paint/     the emit seam: sink.hpp (the Sink concept + ids + flags)
src/render/    CPU renderers + pixel-match harness
tests/         GTest; most of the value lives here
ext/slughorn/  submodule — analytic glyph coverage (quality renderer, GPU)
```
