#pragma once
// The equivalence harness. Two producers of the same UI are equivalent iff
// their call streams raster to IDENTICAL pixels through the same CheapRaster
// configuration (mode, and in real mode, textures). Pixels — not command
// bytes — are the relation, so it is order-tolerant exactly where order
// cannot matter (non-overlapping draws) and strict exactly where it must
// (overlapping draws composite in call order).
#include "render/cheap_raster.hpp"

#include <cstdint>

namespace ui {

struct PixelDiff {
    bool size_mismatch = false;
    uint64_t differing = 0;              // count of differing pixels
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // their bounding box (valid when differing > 0)

    bool equal() const { return !size_mismatch && differing == 0; }
};

// Compare two same-sized RGBA8 buffers pixel-for-pixel.
inline PixelDiff match_pixels(const uint8_t* a, const uint8_t* b, uint32_t w,
                              uint32_t h) {
    PixelDiff d;
    d.x0 = w;
    d.y0 = h;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2] &&
                a[i + 3] == b[i + 3])
                continue;
            ++d.differing;
            if (x < d.x0) d.x0 = x;
            if (y < d.y0) d.y0 = y;
            if (x + 1 > d.x1) d.x1 = x + 1;
            if (y + 1 > d.y1) d.y1 = y + 1;
        }
    }
    if (d.differing == 0) d.x0 = d.y0 = d.x1 = d.y1 = 0;
    return d;
}

inline PixelDiff match_pixels(const CheapRaster& a, const CheapRaster& b) {
    if (a.width() != b.width() || a.height() != b.height()) {
        PixelDiff d;
        d.size_mismatch = true;
        return d;
    }
    return match_pixels(a.pixels(), b.pixels(), a.width(), a.height());
}

} // namespace ui
