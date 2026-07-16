# QualityRaster — design (pre-review, pre-implementation)

The render ladder's second rung (DESIGN.md): the fidelity bar-raiser. Its
job is REFERENCE OUTPUT — golden images, visual inspection, and the anchor
GPU backends are judged against (± threshold). It is a drop-in Sink: same
concept, same registration model, same call streams as CheapRaster — a
consumer switches renderers by constructing a different sink.

What it must fix over the cheap rung, concretely: real glyphs instead of
hashed cells, and filtered sampling instead of nearest-neighbour aliasing
under minification.

## Non-goals

- Not the equivalence gate. Producer-vs-producer matching stays on the
  cheap renderer (exact, texture-free, milliseconds). Quality output is
  judged by humans and by small self-baked goldens.
- Not real-time. Correct first, pleasant second, fast third — the GPU
  backends own speed.

## Surface

```cpp
class QualityRaster {
  public:
    // -- registration ---------------------------------------------------
    // Textures build their full mip chain at registration (area-weighted
    // downsampling), so draw-time sampling is trilinear with no caps.
    void set_texture(TextureId id, TextureData base);

    // Fonts register REAL face data — and, optionally, AUTHORITATIVE
    // METRICS. The library does no file IO: the host passes the face
    // bytes (borrowed for the sink's lifetime).
    //
    // THE METRICS RULE (the design's load-bearing decision): metrics are
    // DATA, not typography policy. When the host registers a metrics
    // block, the font surface answers with exactly those numbers AND the
    // renderer steps pens by exactly those advances — glyph INK comes
    // from the face, glyph POSITIONS come from the registration. This is
    // what lets a host whose layout world was solved against its own
    // metrics service (its own rounding, its own kerning rules, its own
    // line-cell definition) render through this sink with the ink landing
    // exactly inside the boxes that service solved — with zero knobs in
    // the library. Without a metrics block, the face is the authority:
    // HarfBuzz-shaped, kerned, unrounded — proper typography for hosts
    // that want it.
    struct FontMetrics {
        float ascent = 0;       // baseline from cell top (host-rounded if it cares)
        float lineHeight = 0;   // the line cell the host centers against
        const float* advances = nullptr; // per-codepoint advance table
        uint32_t firstCodepoint = 32, count = 0; // table coverage; outside
                                                 // -> face advance fallback
    };
    struct QualityFontDesc {
        const uint8_t* faceBytes = nullptr;  // TTF/OTF blob, non-owning
        size_t faceSize = 0;
        float px = 0;            // resolved pixel size
        float strokeWidth = 0;   // outline stroke radius; 0 = no stroked variants
        float gamma = 1.0f;      // coverage gamma (LUT: pow(a, 1/gamma))
        bool hinting = true;
        FontMetrics metrics;     // count == 0: face-derived (typographic)
    };
    void set_font(FontId id, QualityFontDesc d);

    // -- the Sink concept (identical signatures to CheapRaster) ---------
    // frame_begin/quad/image/sweep/text/text_stroked/frame_end +
    // measure/ascent/line_height/outline_width. measure() sums the
    // registered advances when a table is present (so layout and ink
    // cannot disagree), else the shaped-run width.
    //
    // Unregistered ids behave exactly like the cheap rung — a nonzero
    // unregistered texture draws loud magenta, an unregistered font draws
    // loud fixed-size cells, and the font surface answers 0 — so a
    // missing registration looks identical on every rung.

    // -- result ----------------------------------------------------------
    // RGBA8. Internally the canvas accumulates in FLOAT and quantizes
    // once at frame_end — per-operation u8 rounding is a cheap-renderer
    // artifact this rung does not inherit.
};
```

## The four quality decisions

1. **Sampling**: mips built at registration (area filter), trilinear at
   draw (bilinear across the two nearest levels by the pixel's UV
   footprint). Masks sample bilinearly at the destination fraction. No
   tap-count caps; magnification stays bilinear.
2. **Text**: HarfBuzz shapes the borrowed string at call time (the
   string-lifetime contract already demands in-call consumption) for
   glyph selection; PLACEMENT follows the metrics rule above — registered
   advance table when present (pen steps by the host's numbers), shaped
   positions otherwise. slughorn's analytic Bezier coverage rasterizes
   the glyphs (the library's text bet — no bitmap snapping), through the
   registered gamma LUT. `text_stroked` renders the stroked outline
   variants (FreeType stroker at the registered radius) with identical
   pen advances. Glyph coverage caches per (font, glyph) — steady-state
   frames allocate nothing after warmup.
3. **Compositing**: float canvas, the same blend arithmetic as the cheap
   rung (src-over / additive / 2·dst·src overlay — the fixed-function
   vocabulary), quantization exactly once. Determinism is claimed
   PER-TOOLCHAIN: same stream + same registrations + same machine →
   byte-identical output. Cross-machine/cross-libm float drift is real,
   so goldens are platform-scoped, SKIP LOUDLY where unbaked, and re-bake
   deliberately — never tolerance-fudged.
4. **Edges stay pixel-center in v1.** Fractional-edge coverage was
   considered and REJECTED for now: UI chrome abuts (9-slice pieces share
   edges), and fractional coverage double-composites every shared edge —
   visible seams on translucent chrome. Analytic edge AA returns, if
   ever, as a whole-frame coverage rework with seam-aware compositing,
   not as a per-rect feature.
5. **Text encoding is pinned: UTF-8.** The sink's string_view is UTF-8;
   the metrics table is keyed by codepoint. A host whose native text
   world is a single-byte encoding transcodes at emission (byte ->
   codepoint is lossless for Latin-1). The cheap rung's per-byte cells
   are a documented cheapness artifact — comparisons are always
   same-rung, so the divergence is invisible to every gate.

## Dependencies

harfbuzz + freetype join the vcpkg manifest — which is baseline-PINNED:
shaping and stroking output track library versions, so a dependency bump
is a deliberate act that re-bakes the text goldens, never silent drift.
slughorn (already a submodule) supplies the analytic coverage rasterizer.
All three appear in the quality translation units only — the cheap
renderer keeps its zero-dependency footprint.

## Testing (every line, same commit — the standing rule)

- Tiny self-baked goldens per primitive family (few-KB PNGs committed),
  rendered from IN-REPO inputs only: generated textures (checkerboards,
  gradients) and one committed OFL-licensed subset font — the library
  repo carries no consumer assets. Goldens are platform-scoped, SKIP
  LOUDLY where unbaked, and re-bake deliberately (a dependency-baseline
  bump or toolchain change is a re-bake event).
- Coverage: filtered minification, trilinear level selection, a shaped
  run with gamma, a stroked run, registered-metrics placement vs shaped
  placement, each blend mode in float.
- Determinism: render twice in-process, byte-equal.
- Metrics: measure/ascent/line_height against the registered table AND
  against known face constants (no-table mode); the stroked run's
  advances equal the plain run's.
- The cheap suite keeps gating the shared semantics (ids, geometry,
  blend vocabulary) — quality tests pin only what quality adds.
- Mip memory: +~33% over base textures, copied at registration (the
  cheap rung borrows; the reference rung owns) — accepted, it is a
  reference renderer.

## Resolved review outcomes

- Metrics-as-registered-data (the blocker): see "THE METRICS RULE" above.
- Text encoding pinned UTF-8; single-byte hosts transcode at emission.
- Fractional-edge coverage rejected for v1 (abutting-chrome seams).
- Goldens: in-repo inputs, platform-scoped, baseline-pinned deps.
- Unregistered ids fail identically to the cheap rung.
- Gamma-correct linear-space BLENDING stays out of scope v1 (the coverage
  gamma LUT is in); revisit with the GPU backends, which will want one
  answer for both.
