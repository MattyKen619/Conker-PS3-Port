// Widescreen: frame-sized color images get extra columns at each side (a margin), and the backends
// show the 3D past the 4:3 edges in them, as RT64 does for the PC version:
//  - 3D drawn with a viewport that covers the frame keeps its projection, but isn't clipped at the
//    4:3 edges (the game culls less with the hooks in ps3/host/widescreen_hooks.cpp);
//  - a scissor side at (or within kWideEdge of) the frame's edge reaches the margin's edge;
//  - an untextured rectangle reaching such a scissor side reaches it too (background fills, black
//    bars); textured ones (pictures, the HUD) stay where they are, in the 4:3 frame;
//  - rectangles from RT64's extended GBI (DrawState::rect_explicit) are drawn where they say, which
//    can be in the margins.
// N64 coordinates stay as they are: x = 0 is the frame's left edge, the margins are x < 0 and
// x >= width.
#pragma once
#include <cstdint>

#include "n64gfx.h"

namespace n64gfx {

// How much wider than 4:3 the picture is shown (1: not widened). Set by the host before drawing.
inline float g_wide_ratio = 1.0f;
constexpr float kWideEdge = 4.0f;

// Columns added at each side of a color image `width` pixels wide.
inline uint32_t wide_margin(uint32_t width) {
    if (g_wide_ratio <= 1.001f || width < 256) return 0;
    return (uint32_t)((float)width * (g_wide_ratio - 1.0f) * 0.5f + 0.5f);
}

// The scissor's left and right sides in a widened image (N64 x, may be negative).
inline void wide_scissor(const DrawState& st, uint32_t width, uint32_t m, float* x0, float* x1) {
    *x0 = st.sc_ulx;
    *x1 = st.sc_lrx;
    if (!m) return;
    if (st.sc_ulx <= kWideEdge) *x0 = -(float)m;
    if (st.sc_lrx >= (float)width - kWideEdge) *x1 = (float)(width + m);
}

// Whether 3D drawn with the state's viewport is widened (the viewport covers the frame).
inline bool wide_viewport(const DrawState& st, uint32_t width, uint32_t m) {
    if (!m) return false;
    const float half = (float)width * 0.5f;
    return st.vp_trans[0] > half - 4.0f && st.vp_trans[0] < half + 4.0f && st.vp_scale[0] > (float)width * 0.4f;
}

// A rectangle's left and right sides in a widened image.
inline void wide_rect(const DrawState& st, uint32_t width, uint32_t m, bool textured, float* ulx, float* lrx) {
    if (!m || textured || st.rect_explicit) return;
    if (st.sc_ulx <= kWideEdge && *ulx <= st.sc_ulx + 0.5f) *ulx = -(float)m;
    if (st.sc_lrx >= (float)width - kWideEdge && *lrx >= st.sc_lrx - 0.5f) *lrx = (float)(width + m);
}

// The old-film look (It's War's newsreel cutscene): scratches, rectangles 1-3 pixels wide down the whole
// frame in the env color, and dust specks, at random places across the 4:3 frame, not in every frame
// (scratches in about 4 of 10). WideFilm tells when the look is on: a scratch within the last
// kFilmHold display lists (specks only count while it's on, being told apart by their state alone).
// The N64 shows those frames in black and white (the backends present them in grey), and in a
// widened picture the scratches and specks are spread across all of it (positions scaled from the
// frame's middle; sizes kept).
inline bool film_scratch(const DrawState& st, float ulx, float uly, float lrx, float lry) {
    return st.combine_hi == 0xFFFFFF && st.combine_lo == 0xFFFEFB7Du && st.om_l == 0x00504840u &&
           uly <= 1.0f && lry >= 200.0f && lrx - ulx <= 8.0f;
}
inline bool film_speck(const DrawState& st) {
    return st.combine_hi == 0xFF9BFF && st.combine_lo == 0xFF377FBFu && st.om_l == 0x00504840u;
}
struct WideFilm {
    static constexpr uint32_t kFilmHold = 30;  // display lists (1 s at 30 fps)
    uint32_t lists_since_scratch = 1000;
    void next_list() { if (lists_since_scratch < 1000) lists_since_scratch++; }
    bool on() const { return lists_since_scratch <= kFilmHold; }
    // A texture rectangle about to be drawn: whether it's part of the film overlay.
    bool note(const DrawState& st, float ulx, float uly, float lrx, float lry) {
        if (film_scratch(st, ulx, uly, lrx, lry)) { lists_since_scratch = 0; return true; }
        return film_speck(st) && on();
    }
};
// The bullet holes shot into the screen (It's War): 64x64 IA8 rectangles in the env color, placed
// across the 4:3 frame; spread across a widened picture like the film overlay.
inline bool bullet_hole(const DrawState& st) {
    return st.combine_hi == 0x129A25 && st.combine_lo == 0xFF37FFFFu && st.om_l == 0x005041C8u;
}
// Moves a film overlay rectangle out from the frame's middle, across a widened picture.
inline void wide_film(uint32_t width, uint32_t m, float* ulx, float* lrx) {
    if (!m) return;
    const float half = (float)width * 0.5f, k = (float)(width + 2 * m) / (float)width;
    const float c = (*ulx + *lrx) * 0.5f, shift = (c - half) * k + half - c;
    *ulx += shift;
    *lrx += shift;
}

}  // namespace n64gfx
