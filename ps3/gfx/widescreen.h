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

// A split-screen view (multiplayer): a viewport that doesn't cover the frame, with the scissor around it
// (2 players: the viewport 25..267 and the scissor 30..262 of 292; 4 players: quarters). As RT64 does
// for the PC version, such a view is widened about the frame's middle: its sides move out by the
// picture's ratio (one at the middle stays), and its centre with them, the projection kept.
inline bool wide_split_view(const DrawState& st, uint32_t width, uint32_t m) {
    if (!m || st.vp_scale[0] < (float)width * 0.15f) return false;
    if (st.sc_ulx <= kWideEdge && st.sc_lrx >= (float)width - kWideEdge) return false;  // spans the frame
    const float l = st.vp_trans[0] - st.vp_scale[0], r = st.vp_trans[0] + st.vp_scale[0];
    return st.sc_ulx > l - 12.0f && st.sc_ulx < l + 12.0f && st.sc_lrx > r - 12.0f && st.sc_lrx < r + 12.0f;
}
// A split view's side (N64 x) where it's drawn in the widened picture.
inline float wide_split_x(float x, uint32_t width, uint32_t m) {
    const float half = (float)width * 0.5f;
    if (x <= kWideEdge) return -(float)m;
    if (x >= (float)width - kWideEdge) return (float)(width + m);
    return half + (x - half) * (float)(width + 2 * m) / (float)width;
}
// How far a split view's centre moves (added to the viewport's x translation).
inline float wide_view_shift(const DrawState& st, uint32_t width, uint32_t m) {
    if (!wide_split_view(st, width, m)) return 0.0f;
    return (st.vp_trans[0] - (float)width * 0.5f) * (float)(2 * m) / (float)width;
}
// The scissor for 3D (triangles): a split view's sides are widened.
inline void wide_scissor_3d(const DrawState& st, uint32_t width, uint32_t m, float* x0, float* x1) {
    if (wide_split_view(st, width, m)) { *x0 = wide_split_x(st.sc_ulx, width, m); *x1 = wide_split_x(st.sc_lrx, width, m); return; }
    wide_scissor(st, width, m, x0, x1);
}

// Whether 3D drawn with the state's viewport is widened (the viewport covers the frame, or is a
// split-screen view's).
inline bool wide_viewport(const DrawState& st, uint32_t width, uint32_t m) {
    if (!m) return false;
    const float half = (float)width * 0.5f;
    if (st.vp_trans[0] > half - 4.0f && st.vp_trans[0] < half + 4.0f && st.vp_scale[0] > (float)width * 0.4f) return true;
    return wide_split_view(st, width, m);
}

// The split-screen views seen lately (their scissors, noted as their 3D is drawn): rectangles drawn in
// one's scissor (the circle wipe's black and its quarters, the fade after it) are clipped to the view as
// widened, and a plain one reaching the view's side reaches the widened side.
struct SplitViews {
    struct View { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; uint32_t age = 1000; } v[8];
    static constexpr uint32_t kHold = 90;  // display lists a view is remembered (its 3D may not be drawn while wiped out)
    void next_list() { for (auto& e : v) if (e.age < 1000) e.age++; }
    void note(const DrawState& st) {
        View* oldest = &v[0];
        for (auto& e : v) {
            if (e.x0 == st.sc_ulx && e.x1 == st.sc_lrx && e.y0 == st.sc_uly && e.y1 == st.sc_lry) { e.age = 0; return; }
            if (e.age > oldest->age) oldest = &e;
        }
        *oldest = View{st.sc_ulx, st.sc_uly, st.sc_lrx, st.sc_lry, 0};
    }
    bool has(const DrawState& st) const {
        for (const auto& e : v)
            if (e.age < kHold && e.x0 == st.sc_ulx && e.x1 == st.sc_lrx && e.y0 == st.sc_uly && e.y1 == st.sc_lry) return true;
        return false;
    }
};
// The scissor for rectangles: a split-screen view's is widened as its 3D's.
inline void wide_scissor_rect(const DrawState& st, uint32_t width, uint32_t m, const SplitViews& views, float* x0, float* x1) {
    if (m && views.has(st)) { *x0 = wide_split_x(st.sc_ulx, width, m); *x1 = wide_split_x(st.sc_lrx, width, m); return; }
    wide_scissor(st, width, m, x0, x1);
}

// A rectangle's left and right sides in a widened image.
inline void wide_rect(const DrawState& st, uint32_t width, uint32_t m, bool textured, float* ulx, float* lrx, const SplitViews* views = nullptr) {
    if (!m || textured || st.rect_explicit) return;
    if (views && views->has(st)) {  // in a split-screen view: out to its widened sides
        if (*ulx <= st.sc_ulx + 0.5f) *ulx = wide_split_x(st.sc_ulx, width, m);
        if (*lrx >= st.sc_lrx - 0.5f) *lrx = wide_split_x(st.sc_lrx, width, m);
        return;
    }
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
