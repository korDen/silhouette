#pragma once
// Font — FreeType + HarfBuzz + slughorn tied together.
//
//  * shape()          UTF-8 -> positioned glyph indices (em units).
//  * ensure_glyphs()  register glyph outlines into the Slug atlas (decomposeGlyph,
//                     keyed by glyph index per Hazard), building/rebuilding once.
//  * glyph()          O(1) lookup of GPU-relevant fields from a flat POD table —
//                     never Atlas::getShape() in a hot loop (Hazard / C2).
//  * curve/band textures for the Vulkan backend.
//
// Coordinate handoff (Hazard "exact and easy to get wrong"): the face is sized
// to ppem == units_per_EM, so HarfBuzz returns 26.6 units where one em is
// upem*64. Outlines load with FT_LOAD_NO_SCALE, so packed geometry is
// size-independent. All positions/metrics this class exposes are in em.
#include <numbers> // slughorn.hpp uses std::numbers::pi_v without including it
                   // (libstdc++/libc++ pull it in transitively; MSVC's STL
                   // does not) — supply it here, before the header.
#include <slughorn/slughorn.hpp>

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Opaque FreeType/HarfBuzz handles kept out of the header.
struct FT_LibraryRec_;
struct FT_FaceRec_;
struct hb_font_t;
struct hb_buffer_t;

namespace ui {

// One shaped glyph. Positions in em. cluster is the byte offset into the source
// string of the character that produced this glyph (monotonic for LTR).
struct ShapedGlyph {
    uint32_t gid;
    float    x_advance;
    float    x_offset;
    float    y_offset;
    int      cluster;
};

// GPU-relevant per-glyph fields, snapshotted flat from the atlas after build().
// Copied once at startup, never per frame.
struct GlyphInfo {
    uint32_t bandTexX = 0, bandTexY = 0, bandMaxX = 0, bandMaxY = 0;
    float bandScaleX = 0, bandScaleY = 0, bandOffsetX = 0, bandOffsetY = 0;
    float bearingX = 0, bearingY = 0, width = 0, height = 0, advance = 0;
    bool  drawable = false; // false for whitespace / empty outlines
};

class Font {
  public:
    Font() = default;
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Open a .ttf/.otf. Returns false on failure. Sets the ppem==upem handoff.
    bool load(const std::string& path);

    // Open a face from an UNOWNED memory blob (e.g. a font entry read in place
    // from a host's mmap'd resource archive). The bytes must stay valid and
    // unchanged for the Font's whole lifetime — no copy is made.
    bool load_from_memory(const void* data, size_t size);

    // Add a secondary face (e.g. the condensed digits face) sharing this
    // Font's atlas. Returns the face index to pass to shape(), or -1 on
    // failure. Glyph ids from secondary faces are tagged with the face index
    // in their top bits, so the atlas/table/emit path is untouched — one
    // atlas, one GPU binding, N faces.
    static constexpr uint32_t kFaceShift = 26;

    // Stroked glyph variants: a gid tagged with kStrokedBit registers the
    // OUTLINE-STROKED expansion of the same glyph (FT stroker at the radius
    // below) — same advances, fatter coverage; the under-pass of outlined
    // text. Set the radius (em units) before ensure_glyphs() of tagged ids.
    static constexpr uint32_t kStrokedBit = 1u << 31;
    void set_stroke_radius(float em) { stroke_radius_em_ = em; }
    float stroke_radius() const { return stroke_radius_em_; }

    // Direct codepoint -> glyph id on the primary face (the metric-table
    // placement path selects glyphs per codepoint, no shaping).
    uint32_t glyph_index(uint32_t codepoint) const;

    // A glyph's face advance in em (primary face), independent of atlas
    // registration — the fallback for codepoints outside a host-registered
    // advance table.
    float advance_em(uint32_t gid) const;
    int add_face(const std::string& path);
    // add_face from an UNOWNED memory blob (same no-copy contract as
    // load_from_memory: the bytes must outlive the Font).
    int add_face_from_memory(const void* data, size_t size);

    // Shape UTF-8 into out (cleared then filled; reuses out's capacity).
    // face 0 is the primary; others come from add_face().
    void shape(std::string_view utf8, std::vector<ShapedGlyph>& out,
               int face = 0) const;

    // Ensure every gid has an atlas entry. Builds the atlas the first time; if
    // later gids are new, rebuilds once (cheap repack — Hazard). Returns true if
    // the atlas textures changed (backend must re-upload); check atlas_dirty().
    bool ensure_glyphs(const std::vector<uint32_t>& gids);

    // O(1) lookup; nullptr if the gid has no drawable geometry (e.g. space).
    const GlyphInfo* glyph(uint32_t gid) const {
        auto it = index_.find(gid);
        if (it == index_.end()) return nullptr;
        const GlyphInfo& gi = table_[it->second];
        return gi.drawable ? &gi : nullptr;
    }

    // Vertical metrics, em units. descent is negative (below the baseline).
    float ascent()      const { return ascent_; }
    float descent()     const { return descent_; }
    float line_gap()    const { return line_gap_; }
    float line_height() const { return ascent_ - descent_ + line_gap_; }
    // Cap height (em, positive): baseline to the top of a flat capital (OS/2
    // sCapHeight, else the 'H' outline). Not used by the FreeType line-box model
    // but the right metric for cap alignment / a DirectWrite target later.
    float cap_height()  const { return cap_height_; }
    float upem()        const { return upem_; }

    // Textures for the backend (valid after ensure_glyphs()).
    const slughorn::Atlas::TextureData& curve_texture() const { return atlas_->getCurveTextureData(); }
    const slughorn::Atlas::TextureData& band_texture()  const { return atlas_->getBandTextureData(); }
    uint32_t atlas_width() const { return atlas_->getTextureWidth(); }

    bool atlas_dirty() const { return atlas_dirty_; }
    void clear_atlas_dirty() { atlas_dirty_ = false; }

    // For the coverage oracle (C8): the built atlas, to feed render::decode().
    const slughorn::Atlas& atlas() const { return *atlas_; }

    // Number of shape() calls so far — used by cost tests to prove text is not
    // re-shaped for hover/scroll/focus/blink/caret movement (C6).
    uint64_t shape_calls() const { return shape_calls_; }

  private:
    bool init_loaded_face();    // shared post-FT_New_*Face setup (metrics, hb, atlas)
    int  init_extra_face(FT_FaceRec_* face); // shared add_face* tail
    void rebuild_atlas();       // fresh Atlas from registered_ set
    void snapshot_table();      // Shape fields -> flat GlyphInfo table

    FT_LibraryRec_* ft_lib_ = nullptr;
    FT_FaceRec_*    face_   = nullptr;
    hb_font_t*      hb_font_ = nullptr;

    struct ExtraFace { // secondary faces (index 1..); see add_face()
        FT_FaceRec_* face = nullptr;
        hb_font_t*   hb   = nullptr;
        float        to_em = 0;
    };
    std::vector<ExtraFace> extra_faces_;

    std::unique_ptr<slughorn::Atlas>      atlas_;
    std::vector<uint32_t>                 registered_; // gids in the atlas (sorted-unique)
    ankerl::unordered_dense::map<uint32_t, int>     index_;      // gid -> table_ index
    std::vector<GlyphInfo>                table_;

    float upem_ = 0, to_em_ = 0;
    float stroke_radius_em_ = 0;
    float ascent_ = 0, descent_ = 0, line_gap_ = 0, cap_height_ = 0;
    bool  atlas_dirty_ = false;
    mutable uint64_t shape_calls_ = 0;
    // Pooled shaping scratch: created lazily, reset per shape() so the shaper's
    // working storage is reused instead of malloc/freed per call (C2 spirit —
    // the alloc counter tracks operator new; this pools the C side).
    mutable hb_buffer_t* shape_buf_ = nullptr;
};

} // namespace ui
