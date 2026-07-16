#pragma once
// QualityRaster — the render ladder's second rung (docs/quality-renderer.md):
// the fidelity bar-raiser. Reference output for golden images, visual
// inspection, and the anchor GPU backends are judged against. A drop-in
// Sink: same concept, same call streams as CheapRaster.
//
// This stage (images): mip chains built at texture registration
// (area-filtered), trilinear sampling at draw, a FLOAT canvas quantized
// once at frame_end, per-operation clamping (a GPU UNORM target clamps per
// draw — the reference must match GPU semantics, not accumulate energy no
// backend could represent), bilinear dst-fraction masks. Edges stay
// pixel-center by design (fractional coverage double-composites abutting
// chrome seams — rejected, see the doc).
//
// Text (faces, shaping, analytic coverage, the registered-metrics rule)
// lands as the next stage; its machinery lives behind the pimpl so this
// header pulls no font dependencies into consumers.
#include "core/geometry.hpp"
#include "paint/sink.hpp"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ui {

class QualityRaster {
  public:
    QualityRaster();
    ~QualityRaster();
    QualityRaster(const QualityRaster&) = delete;
    QualityRaster& operator=(const QualityRaster&) = delete;

    // Copy the base level and build its area-filtered mip chain.
    void set_texture(TextureId id, const uint8_t* rgba, uint32_t width,
                     uint32_t height);

    // ---- fonts (docs/quality-renderer.md, "THE METRICS RULE") ----------
    // Metrics are DATA, not typography policy: when a table is registered,
    // the font surface answers with exactly these numbers AND pens step by
    // exactly these advances — ink from the face, positions from the
    // registration. Without a table the face is the authority (HarfBuzz-
    // shaped, kerned). The advance table is COPIED; faceBytes are BORROWED
    // for the sink's lifetime. faceBytes == nullptr registers metrics-only:
    // the surface answers, text draws the loud missing-ink cells.
    struct FontMetrics {
        float ascent = 0;     // baseline from the line-cell top
        float lineHeight = 0; // the line cell the host centers against
        const float* advances = nullptr; // px, per codepoint
        uint32_t firstCodepoint = 32, count = 0;
    };
    struct QualityFontDesc {
        const uint8_t* faceBytes = nullptr; // TTF/OTF blob, non-owning
        size_t faceSize = 0;
        float px = 0;          // pixels per em
        float strokeWidth = 0; // outline stroke radius (px); 0 = no stroker
        float gamma = 1.0f;    // coverage gamma: fill^(1/gamma)
        FontMetrics metrics;   // count == 0: face-derived (typographic)
    };
    void set_font(FontId id, const QualityFontDesc& d);

    // ---- Sink concept (images stage) -----------------------------------
    void frame_begin(uint32_t w, uint32_t h, Color clear);
    void quad(Rect dst, Color c, uint32_t flags, Rect clip);
    // Texture 0 = the solid white texel (tint is the fill; flags and mask
    // apply); a NONZERO unregistered id draws loud magenta.
    void image(Rect dst, TextureId t, Rect uv, Color tint, uint32_t flags,
               TextureId mask, Rect clip);
    void sweep(Rect dst, Color c, float a0, float a1, float frac,
               TextureId mask, Rect clip);
    // One run at a baseline-left pen (UTF-8, borrowed for the call).
    // text_stroked draws the font's stroked outline variants at the same
    // advances. Unregistered fonts (and metrics-only registrations, which
    // have no ink source) draw the loud fixed-size cells.
    void text(Vec2 pen, std::string_view s, FontId f, Color c, Rect clip);
    void text_stroked(Vec2 pen, std::string_view s, FontId f, Color c,
                      Rect clip);
    void frame_end();

    // ---- the font surface ----------------------------------------------
    // Registered-table numbers when present; face metrics otherwise;
    // zeros when unregistered.
    float measure(FontId f, std::string_view s) const;
    float ascent(FontId f) const;
    float line_height(FontId f) const;
    float outline_width(FontId f) const;

    // ---- Result ---------------------------------------------------------
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    // RGBA8, quantized once at frame_end; alpha always 255.
    const uint8_t* pixels() const { return out_.data(); }

  private:
    struct MipTexture {
        // level 0 = base; each level RGBA stored as float [0,1] for exact
        // area math and filtering without repeated u8 quantization.
        struct Level {
            std::vector<float> rgba;
            uint32_t w = 0, h = 0;
        };
        std::vector<Level> levels;
    };

    void fill_span(Rect dst, Color c, Rect clip, uint32_t mode);
    void blend_px(int x, int y, float r, float g, float b, float a,
                  uint32_t mode);
    float mask_alpha(TextureId mask, float fx, float fy) const;
    void draw_run(Vec2 pen, std::string_view s, FontId f, Color c, Rect clip,
                  bool stroked);
    void loud_cells(Vec2 pen, std::string_view s, Rect clip);

    ankerl::unordered_dense::map<TextureId, MipTexture> textures_;
    std::vector<float> canvas_; // RGB float, per-op clamped
    std::vector<uint8_t> out_;  // RGBA8, filled at frame_end
    uint32_t w_ = 0, h_ = 0;

    struct FontWorld; // faces/shaping/coverage; mutable: the font surface
                      // is a const query that fills shaping scratch and
                      // sampler caches
    mutable std::unique_ptr<FontWorld> fonts_;
};

} // namespace ui
