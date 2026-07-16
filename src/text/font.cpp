#include "text/font.hpp"

#include <slughorn/freetype.hpp> // decomposeGlyph

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include FT_TRUETYPE_TABLES_H

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <algorithm>

using slughorn::cv;
using slughorn::slug_t;
using namespace slughorn::literals;

namespace ui {

Font::~Font() {
    if (shape_buf_) hb_buffer_destroy(shape_buf_);
    for (ExtraFace& f : extra_faces_) {
        if (f.hb)   hb_font_destroy(f.hb);
        if (f.face) FT_Done_Face(f.face);
    }
    if (hb_font_) hb_font_destroy(hb_font_);
    if (face_)    FT_Done_Face(face_);
    if (ft_lib_)  FT_Done_FreeType(ft_lib_);
}

int Font::add_face(const std::string& path) {
    if (!ft_lib_ || !face_) return -1; // primary must load first
    FT_FaceRec_* face = nullptr;
    if (FT_New_Face(ft_lib_, path.c_str(), 0, &face)) return -1;
    return init_extra_face(face);
}

int Font::add_face_from_memory(const void* data, size_t size) {
    if (!ft_lib_ || !face_) return -1; // primary must load first
    // Same no-copy contract as load_from_memory: the blob backs the face.
    FT_FaceRec_* face = nullptr;
    if (FT_New_Memory_Face(ft_lib_, static_cast<const FT_Byte*>(data),
                           static_cast<FT_Long>(size), 0, &face))
        return -1;
    return init_extra_face(face);
}

int Font::init_extra_face(FT_FaceRec_* face) {
    // Refuse BEFORE taking ownership: a face whose tag would not fit above the
    // gid bits must not linger in extra_faces_ (it would shift later indices
    // and leak into teardown paths that assume every entry is addressable).
    const int index = static_cast<int>(extra_faces_.size()) + 1; // 1-based
    if (static_cast<uint32_t>(index) >= (1u << (32 - kFaceShift))) {
        FT_Done_Face(face);
        return -1;
    }
    ExtraFace f;
    f.face = face;
    const float upem = static_cast<float>(f.face->units_per_EM);
    f.to_em = 1.0f / (upem * 64.0f);
    if (FT_Set_Char_Size(f.face, 0, static_cast<FT_F26Dot6>(upem * 64.0f),
                         72, 72)) {
        FT_Done_Face(face);
        return -1; // face refuses the ppem==upem handoff (Hazard)
    }
    f.hb = hb_ft_font_create_referenced(f.face);
    extra_faces_.push_back(f);
    return index;
}

bool Font::load(const std::string& path) {
    if (FT_Init_FreeType(&ft_lib_)) return false;
    if (FT_New_Face(ft_lib_, path.c_str(), 0, &face_)) return false;
    return init_loaded_face();
}

bool Font::load_from_memory(const void* data, size_t size) {
    if (FT_Init_FreeType(&ft_lib_)) return false;
    // FT_New_Memory_Face does NOT copy: the caller's blob (an mmap'd archive
    // payload) backs the face for its whole lifetime.
    if (FT_New_Memory_Face(ft_lib_, static_cast<const FT_Byte*>(data),
                           static_cast<FT_Long>(size), 0, &face_))
        return false;
    return init_loaded_face();
}

bool Font::init_loaded_face() {
    upem_  = static_cast<float>(face_->units_per_EM);
    to_em_ = 1.0f / (upem_ * 64.0f);

    // ppem == upem: HarfBuzz returns 26.6 units, one em == upem*64 (Hazard).
    if (FT_Set_Char_Size(face_, 0, static_cast<FT_F26Dot6>(upem_ * 64.0f),
                         72, 72))
        return false; // the whole coordinate handoff depends on this size
    hb_font_ = hb_ft_font_create_referenced(face_);

    hb_font_extents_t ext{};
    hb_font_get_h_extents(hb_font_, &ext);
    ascent_   = static_cast<float>(ext.ascender)  * to_em_;
    descent_  = static_cast<float>(ext.descender) * to_em_; // negative
    line_gap_ = static_cast<float>(ext.line_gap)  * to_em_;

    // Cap height: OS/2 sCapHeight (version >= 2) if present, else the 'H' outline
    // top bearing. In em (units / upem), not the *64 shaping scale.
    const float units_to_em = 1.0f / upem_;
    if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face_, FT_SFNT_OS2));
        os2 && os2->version >= 2 && os2->sCapHeight > 0) {
        cap_height_ = static_cast<float>(os2->sCapHeight) * units_to_em;
    } else if (FT_UInt h = FT_Get_Char_Index(face_, 'H');
               h && FT_Load_Glyph(face_, h, FT_LOAD_NO_SCALE) == 0) {
        cap_height_ = static_cast<float>(face_->glyph->metrics.horiBearingY) * units_to_em;
    }

    atlas_ = std::make_unique<slughorn::Atlas>();
    return true;
}

void Font::shape(std::string_view utf8, std::vector<ShapedGlyph>& out,
                 int face) const {
    ++shape_calls_;
    out.clear();
    hb_font_t* hb = hb_font_;
    float to_em = to_em_;
    uint32_t tag = 0;
    if (face > 0 && face <= static_cast<int>(extra_faces_.size())) {
        hb    = extra_faces_[face - 1].hb;
        to_em = extra_faces_[face - 1].to_em;
        tag   = static_cast<uint32_t>(face) << kFaceShift;
    }
    // Pooled buffer: reset keeps HarfBuzz's internal storage, so steady-state
    // shaping reuses it instead of allocating per call.
    if (!shape_buf_) shape_buf_ = hb_buffer_create();
    hb_buffer_t* buf = shape_buf_;
    hb_buffer_reset(buf);
    hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), 0,
                       static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(buf);
    hb_shape(hb, buf, nullptr, 0);

    unsigned n = 0;
    const hb_glyph_info_t*     gi = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* gp = hb_buffer_get_glyph_positions(buf, &n);
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        out.push_back(ShapedGlyph{
            gi[i].codepoint | tag,
            static_cast<float>(gp[i].x_advance) * to_em,
            static_cast<float>(gp[i].x_offset)  * to_em,
            static_cast<float>(gp[i].y_offset)  * to_em,
            static_cast<int>(gi[i].cluster),
        });
    }
}

uint32_t Font::glyph_index(uint32_t codepoint) const {
    return face_ == nullptr ? 0u : FT_Get_Char_Index(face_, codepoint);
}

float Font::advance_em(uint32_t gid) const {
    if (face_ == nullptr ||
        FT_Load_Glyph(face_, gid, FT_LOAD_NO_SCALE) != 0) {
        return 0;
    }
    return static_cast<float>(face_->glyph->metrics.horiAdvance) / upem_;
}

bool Font::ensure_glyphs(const std::vector<uint32_t>& gids) {
    bool grew = false;
    for (uint32_t g : gids) {
        if (!std::binary_search(registered_.begin(), registered_.end(), g)) {
            registered_.insert(std::upper_bound(registered_.begin(), registered_.end(), g), g);
            grew = true;
        }
    }
    if (grew || !atlas_->isBuilt()) {
        rebuild_atlas();
        return true;
    }
    return false;
}

void Font::rebuild_atlas() {
    // Fresh atlas from the full registered set (Hazard: build() is one-shot, so
    // reconstruct rather than mutate). Cheap; for a directory listing it fires
    // once at startup and never again.
    atlas_ = std::make_unique<slughorn::Atlas>();
    const slug_t emScale = 1_cv / cv(static_cast<double>(upem_));

    for (uint32_t g : registered_) {
        // tagged ids resolve to their secondary face; the atlas keys on the
        // tagged id, so faces never collide
        const bool stroked = (g & kStrokedBit) != 0;
        const uint32_t untagged = g & ~kStrokedBit;
        const uint32_t faceIdx = untagged >> kFaceShift;
        FT_FaceRec_* face = face_;
        slug_t scale = emScale;
        float upem = upem_;
        if (faceIdx > 0 && faceIdx <= extra_faces_.size()) {
            face = extra_faces_[faceIdx - 1].face;
            upem = static_cast<float>(face->units_per_EM);
            scale = 1_cv / cv(static_cast<double>(upem));
        }
        const uint32_t raw = untagged & ((1u << kFaceShift) - 1u);
        if (FT_Load_Glyph(face, raw, FT_LOAD_NO_SCALE)) continue;
        slughorn::Atlas::ShapeInfo info; // autoMetrics = true
        const slug_t adv = cv(static_cast<double>(face->glyph->metrics.horiAdvance)) * scale;
        if (stroked && stroke_radius_em_ > 0) {
            // The stroked variant: expand the outline outward by the stroke
            // radius (round joins/caps) and register THAT shape under the
            // tagged id — same advance, fatter coverage. The FT glyph is
            // stroked as a standalone FT_Glyph; its outline is swapped into
            // the slot around decomposeGlyph (which reads face->glyph) and
            // restored after.
            FT_Glyph fg = nullptr;
            if (FT_Get_Glyph(face->glyph, &fg) != 0) continue;
            FT_Stroker stroker = nullptr;
            FT_Stroker_New(ft_lib_, &stroker);
            // The stroker radius is in the OUTLINE's units — with
            // FT_LOAD_NO_SCALE that is plain font units, not 26.6.
            const FT_Fixed radius = (FT_Fixed)(stroke_radius_em_ * upem);
            FT_Stroker_Set(stroker, radius, FT_STROKER_LINECAP_ROUND,
                           FT_STROKER_LINEJOIN_ROUND, 0);
            if (FT_Glyph_StrokeBorder(&fg, stroker, 0, 1) == 0 &&
                fg->format == FT_GLYPH_FORMAT_OUTLINE) {
                const FT_Outline saved = face->glyph->outline;
                face->glyph->outline =
                    reinterpret_cast<FT_OutlineGlyph>(fg)->outline;
                slughorn::freetype::decomposeGlyph(face, scale, adv, nullptr,
                                                   info);
                face->glyph->outline = saved;
            }
            FT_Stroker_Done(stroker);
            FT_Done_Glyph(fg);
        } else {
            slughorn::freetype::decomposeGlyph(face, scale, adv, nullptr,
                                               info);
        }
        if (!info.curves.empty()) atlas_->addShape(g, info);
    }
    atlas_->build();
    snapshot_table();
    atlas_dirty_ = true;
}

void Font::snapshot_table() {
    table_.clear();
    index_.clear();
    table_.reserve(registered_.size());
    for (uint32_t g : registered_) {
        GlyphInfo gi;
        if (const auto shape = atlas_->getShape(g)) {
            gi.bandTexX = shape->bandTexX; gi.bandTexY = shape->bandTexY;
            gi.bandMaxX = shape->bandMaxX; gi.bandMaxY = shape->bandMaxY;
            gi.bandScaleX  = static_cast<float>(shape->bandScaleX);
            gi.bandScaleY  = static_cast<float>(shape->bandScaleY);
            gi.bandOffsetX = static_cast<float>(shape->bandOffsetX);
            gi.bandOffsetY = static_cast<float>(shape->bandOffsetY);
            gi.bearingX = static_cast<float>(shape->bearingX);
            gi.bearingY = static_cast<float>(shape->bearingY);
            gi.width    = static_cast<float>(shape->width);
            gi.height   = static_cast<float>(shape->height);
            gi.advance  = static_cast<float>(shape->advance);
            gi.drawable = gi.width > 1e-6f && gi.height > 1e-6f;
        }
        index_[g] = static_cast<int>(table_.size());
        table_.push_back(gi);
    }
}

} // namespace ui
