// Software rasterizer backend for n64gfx (see softraster.h). Straightforward and slow: one pixel at
// a time through the N64 color combiner and blender, point-sampled textures.
#include "softraster.h"
#include "widescreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "miniz.h"

namespace n64gfx {

namespace {
    inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

    struct PixelInputs {
        float tex[2][4];
        float shade[4];
        const DrawState* st;
    };

    // Sample a texture with the tile's wrap / mirror / clamp, nearest texel.
    void sample(const Texture* tex, const TexMap& m, float s, float t, float out[4]) {
        if (!tex) { out[0] = out[1] = out[2] = out[3] = 0; return; }
        float u = s * m.shift_s - m.offset_s;
        float v = t * m.shift_t - m.offset_t;
        int x = (int)std::floor(u), y = (int)std::floor(v);
        auto fix = [](int c, bool clamp, bool mirror, float cmax, int size) {
            if (clamp) { if (c < 0) c = 0; if (c > (int)cmax) c = (int)cmax; }
            if (mirror) {
                int period = c / size; if (c < 0) period = (c - size + 1) / size;
                c = ((c % size) + size) % size;
                if (period & 1) c = size - 1 - c;
            } else {
                c = ((c % size) + size) % size;
            }
            return c;
        };
        x = fix(x, m.clamp_s, m.mirror_s, m.clamp_max_s, (int)tex->width);
        y = fix(y, m.clamp_t, m.mirror_t, m.clamp_max_t, (int)tex->height);
        const uint8_t* p = tex->rgba + ((size_t)y * tex->width + x) * 4;
        for (int i = 0; i < 4; i++) out[i] = p[i] / 255.0f;
    }

    // One combiner cycle.
    void combine(const CombinerCycle& c, const PixelInputs& in, const float combined[4], float out[4]) {
        const DrawState& st = *in.st;
        auto color = [&](int sel, int which, int k) -> float {
            // which: 0 = A, 1 = B, 2 = C, 3 = D
            switch (sel) {
            case 0: return combined[k];
            case 1: return in.tex[0][k];
            case 2: return in.tex[1][k];
            case 3: return st.prim[k];
            case 4: return in.shade[k];
            case 5: return st.env[k];
            }
            if (which == 0) return sel == 6 ? 1.0f : 0.0f;  // 7: noise
            if (which == 1) return 0.0f;                     // key center, K4
            if (which == 3) return sel == 6 ? 1.0f : 0.0f;
            switch (sel) {                                   // C
            case 7: return combined[3];
            case 8: return in.tex[0][3];
            case 9: return in.tex[1][3];
            case 10: return st.prim[3];
            case 11: return in.shade[3];
            case 12: return st.env[3];
            case 14: return st.prim_lod_frac;
            case 15: return st.k5;
            default: return 0.0f;
            }
        };
        auto alpha = [&](int sel, bool is_c) -> float {
            switch (sel) {
            case 0: return is_c ? 0.0f : combined[3];  // C: LOD fraction
            case 1: return in.tex[0][3];
            case 2: return in.tex[1][3];
            case 3: return st.prim[3];
            case 4: return in.shade[3];
            case 5: return st.env[3];
            case 6: return is_c ? st.prim_lod_frac : 1.0f;
            default: return 0.0f;
            }
        };
        for (int k = 0; k < 3; k++) {
            float a = color(c.a, 0, k), b = color(c.b, 1, k), cc = color(c.c, 2, k), d = color(c.d, 3, k);
            out[k] = clamp01((a - b) * cc + d);
        }
        out[3] = clamp01((alpha(c.aa, false) - alpha(c.ab, false)) * alpha(c.ac, true) + alpha(c.ad, false));
    }
}

void reference_combine(const DrawState& st, const float tex[2][4], const float shade[4], float out[4]) {
    PixelInputs in;
    in.st = &st;
    std::memcpy(in.tex, tex, sizeof in.tex);
    std::memcpy(in.shade, shade, sizeof in.shade);
    CombinerCycle cc[2];
    decode_combiner(st.combine_hi, st.combine_lo, cc);
    float zero[4] = {0, 0, 0, 0}, c1[4];
    combine(cc[0], in, zero, c1);
    if (st.cycle() == CYC_2) combine(cc[1], in, c1, out);
    else std::memcpy(out, c1, sizeof c1);
}

SoftRaster::Target& SoftRaster::target(const DrawState& st) {
    Target& t = targets[st.cimg_addr];
    const uint32_t width = st.cimg_width ? st.cimg_width : 320;
    if (t.rgba.empty() || t.width != width || t.margin != wide_margin(width)) {
        t.width = width;
        t.height = 480;
        t.bpp = st.cimg_siz == 3 ? 4 : 2;
        t.margin = wide_margin(width);
        t.stride = width + 2 * t.margin;
        t.rgba.assign((size_t)t.stride * t.height * 4, 0);
    }
    return t;
}

std::vector<float>& SoftRaster::depth(const DrawState& st, const Target& t) {
    std::vector<float>& z = depths[st.zimg_addr];
    if (z.size() != (size_t)t.stride * t.height) z.assign((size_t)t.stride * t.height, 1.0f);
    depth_margin_[st.zimg_addr] = t.margin;
    return z;
}

const SoftRaster::Target* SoftRaster::find(uint32_t addr, uint32_t* row_offset) const {
    // The nearest target at or below the address (buffers can sit closer together than 480 rows).
    auto it = targets.upper_bound(addr);
    if (it == targets.begin()) return nullptr;
    --it;
    const Target& t = it->second;
    if (addr >= it->first + t.width * t.height * t.bpp) return nullptr;
    if (row_offset) *row_offset = (addr - it->first) / (t.width * t.bpp);
    return &t;
}

void SoftRaster::clear_depth(uint32_t zimg_addr, float ulx, float uly, float lrx, float lry) {
    if (trace) std::printf("[raster] clear depth 0x%06X %.1f,%.1f-%.1f,%.1f (known: %zu)\n", zimg_addr, ulx, uly, lrx, lry, depths.count(zimg_addr));
    for (auto& [a, z] : depths) {
        if (a != zimg_addr) continue;
        // Depth buffers are laid out as their color target (stride = size / 480; margin noted in depth()).
        const uint32_t w = (uint32_t)(z.size() / 480);
        const int m = (int)depth_margin_[a];
        const int x0 = std::max(0, (m && ulx <= kWideEdge) ? 0 : (int)ulx + m);
        const int x1 = std::min((int)w, (m && lrx >= (float)(w - 2 * m) - kWideEdge) ? (int)w : (int)lrx + m);
        for (int y = std::max(0, (int)uly); y < std::min(480, (int)lry); y++)
            for (int x = x0; x < x1; x++) z[(size_t)y * w + x] = 1.0f;
    }
}

void SoftRaster::shade_pixel(const DrawState& st, Target& t, std::vector<float>* z, int x, int y, float d,
                             const float shade[4], float s, float tt, bool use_z) {
    size_t idx = t.index(x, y);
    const bool zcmp = use_z && st.z_compare() && z;
    float zval = st.z_source_prim() ? st.prim_depth : d;
    if (zcmp) {
        float stored = (*z)[idx];
        if (st.z_mode() == 3) { if (zval > stored + 0.002f) return; }   // decal
        else if (zval > stored) return;
    }
    PixelInputs in;
    in.st = &st;
    std::memcpy(in.shade, shade, sizeof in.shade);
    sample(st.tex[0], st.tmap[0], s, tt, in.tex[0]);
    if (mipmaps && st.mip_levels && st.tex[0] && !st.tex[0]->fb_addr) {
        // Trilinear by levels (each level point sampled), as the RSX draws a mipmapped texture.
        const float lod = std::min(std::max(pix_lod_, 0.0f), (float)st.mip_levels);
        const int l0 = (int)lod, l1 = std::min(l0 + 1, (int)st.mip_levels);
        const float f = lod - l0;
        auto level = [&](int l, float out[4]) {
            if (l == 0) { sample(st.tex[0], st.tmap[0], s, tt, out); return; }
            TexMap m = st.tmap[0];
            const float k = 1.0f / (float)(1 << l);
            m.shift_s *= k; m.shift_t *= k; m.offset_s *= k; m.offset_t *= k; m.clamp_max_s *= k; m.clamp_max_t *= k;
            sample(st.mip[l - 1], m, s, tt, out);
        };
        float a[4], b[4];
        level(l0, a); level(l1, b);
        for (int i = 0; i < 4; i++) in.tex[0][i] = a[i] + (b[i] - a[i]) * f;
    }
    if (st.tex[1]) sample(st.tex[1], st.tmap[1], s, tt, in.tex[1]);
    else std::memcpy(in.tex[1], in.tex[0], sizeof in.tex[0]);

    float col[4];
    CycleType cyc = st.cycle();
    if (cyc == CYC_COPY) {
        std::memcpy(col, in.tex[0], sizeof col);
        if (st.alpha_compare() == 1 && col[3] < 0.5f) return;
    } else {
        CombinerCycle cc[2];
        decode_combiner(st.combine_hi, st.combine_lo, cc);
        float zero[4] = {0, 0, 0, 0}, c1[4];
        combine(cc[0], in, zero, c1);
        if (cyc == CYC_2) combine(cc[1], in, c1, col);
        else std::memcpy(col, c1, sizeof col);
        // Alpha compare and coverage.
        if (st.alpha_compare() == 1) {
            float th = st.blend[3];
            if (col[3] < th || (th == 0 && col[3] == 0)) return;
        }
        if (st.cvg_x_alpha() && st.alpha_cvg_sel() && col[3] < 0.5f) return;
        // ALPHA_CVG_SEL alone: the blender's alpha is the coverage, the combined alpha plays no part, so
        // even alpha 0 is drawn (Conker's bat: black textures with alpha 0 on an opaque surface).
    }

    uint8_t* p = &t.rgba[idx * 4];
    float mem[4] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f};
    if (cyc != CYC_COPY) {
        // Blender: (P * A + M * B) per cycle; the last cycle only when it blends (DrawState::blends).
        const uint32_t l = st.om_l;
        int ncyc = cyc == CYC_2 ? 2 : 1;
        float pix[4] = {col[0], col[1], col[2], col[3]};
        for (int c = 0; c < ncyc; c++) {
            int sh = c == 0 ? 0 : -2;
            uint32_t P = (l >> (30 + sh)) & 3, A = (l >> (26 + sh)) & 3, M = (l >> (22 + sh)) & 3, B = (l >> (18 + sh)) & 3;
            bool last = c == ncyc - 1;
            if (last && !st.blends()) break;
            const float* pc = P == 0 ? pix : P == 1 ? mem : P == 2 ? st.blend : st.fog;
            const float* mc = M == 0 ? pix : M == 1 ? mem : M == 2 ? st.blend : st.fog;
            float a = A == 0 ? col[3] : A == 1 ? st.fog[3] : A == 2 ? shade[3] : 0.0f;
            float b = B == 0 ? 1.0f - a : B == 1 ? 1.0f : B == 2 ? 1.0f : 0.0f;
            float o[3];
            for (int k = 0; k < 3; k++) o[k] = clamp01(pc[k] * a + mc[k] * b);
            pix[0] = o[0]; pix[1] = o[1]; pix[2] = o[2];
        }
        col[0] = pix[0]; col[1] = pix[1]; col[2] = pix[2];
    }
    p[0] = (uint8_t)(clamp01(col[0]) * 255.0f + 0.5f);
    p[1] = (uint8_t)(clamp01(col[1]) * 255.0f + 0.5f);
    p[2] = (uint8_t)(clamp01(col[2]) * 255.0f + 0.5f);
    p[3] = 255;
    if (zcmp || (use_z && z && st.z_update())) {
        if (st.z_update() && z) {
            (*z)[idx] = zval;
            if (trace) { auto& w = zwho_[z]; if (w.size() < z->size()) w.resize(z->size(), 0); w[idx] = draw_id_; }
        }
    }
    if ((uint32_t)y + 1 > t.max_y) t.max_y = y + 1;
}

const DrawState& SoftRaster::with_frame_textures(const DrawState& st) {
    if (!(st.tex[0] && st.tex[0]->fb_addr) && !(st.tex[1] && st.tex[1]->fb_addr)) return st;
    fb_state_ = st;
    for (int u = 0; u < 2; u++) {
        const Texture* src = st.tex[u];
        if (!src || !src->fb_addr) continue;
        Texture& t = fb_tex_[u];
        t = *src;
        t.fb_addr = 0;
        std::vector<uint8_t>& px = fb_pixels_[u];
        px.assign((size_t)t.width * t.height * 4, 0);
        const bool intensity = src->fmt == 4 || src->fmt == 3;
        auto it = targets.find(src->fb_addr);
        if (it != targets.end()) {
            const Target& f = it->second;
            for (uint32_t y = 0; y < t.height; y++) {
                int fy = src->fb_y + (int)y;
                if (fy < 0 || fy >= (int)f.height) continue;
                for (uint32_t x = 0; x < t.width; x++) {
                    int fx = src->fb_x + (int)x;
                    if (fx < -(int)f.margin || fx >= (int)(f.width + f.margin)) continue;
                    const uint8_t* s = &f.rgba[f.index(fx, fy) * 4];
                    uint8_t* d = &px[((size_t)y * t.width + x) * 4];
                    if (intensity) { d[0] = d[1] = d[2] = d[3] = s[0]; }  // the RDP wrote the value as the color
                    else { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
                }
            }
        }
        t.rgba = px.data();
        fb_state_.tex[u] = &t;
    }
    return fb_state_;
}

void SoftRaster::draw_triangles(const DrawState& st_in, const OutVertex* v, size_t count) {
    draw_id_++;
    const DrawState& st = with_frame_textures(st_in);
    if (trace && !traced_tris_) { traced_tris_ = true; std::printf("[raster] tris cimg 0x%06X zimg 0x%06X zbuf %d zcmp %d zupd %d\n", st.cimg_addr, st.zimg_addr, st.z_buffered(), st.z_compare(), st.z_update()); }
    if (!trace) traced_tris_ = false;
    Target& t = target(st);
    const bool use_z = st.z_buffered();
    std::vector<float>* z = use_z ? &depth(st, t) : nullptr;
    float wx0, wx1;
    wide_scissor_3d(st, t.width, t.margin, &wx0, &wx1);
    if (wide_split_view(st, t.width, t.margin)) views_.note(st);
    const float view_shift = wide_view_shift(st, t.width, t.margin);  // a split-screen view's centre
    const float sx0 = std::max(-(float)t.margin, wx0), sy0 = std::max(0.0f, st.sc_uly);
    const float sx1 = std::min((float)(t.width + t.margin), wx1), sy1 = std::min((float)t.height, st.sc_lry);

    // Working copy with float colors (0..1), for clipping and interpolation.
    struct FV { float x, y, z, w, s, t, r, g, b, a; };
    for (size_t i = 0; i + 2 < count; i += 3) {
        // Clip against the camera's plane (w > 0); not the near plane (z > -w): the NoN microcode
        // draws a model closer than it, its depth held at the nearest.
        FV poly[9], tmp[9];
        int n = 3;
        for (int k = 0; k < 3; k++) {
            const OutVertex& o = v[i + k];
            poly[k] = {o.x, o.y, o.z, o.w, o.s, o.t, o.c[0] / 255.0f, o.c[1] / 255.0f, o.c[2] / 255.0f, o.c[3] / 255.0f};
        }
        for (int plane = 0; plane < 5; plane++) {  // as n64gfx: w > 0 and a guard band of 4 w
            auto dist = [&](const FV& p) {
                switch (plane) {
                case 0: return p.w - 1e-5f;
                case 1: return 4.0f * p.w - p.x;
                case 2: return 4.0f * p.w + p.x;
                case 3: return 4.0f * p.w - p.y;
                default: return 4.0f * p.w + p.y;
                }
            };
            int m = 0;
            for (int k = 0; k < n; k++) {
                const FV& a = poly[k];
                const FV& b = poly[(k + 1) % n];
                float da = dist(a), db = dist(b);
                if (da >= 0) tmp[m++] = a;
                if ((da >= 0) != (db >= 0)) {
                    float f = da / (da - db);
                    FV c;
                    const float* pa = &a.x; const float* pb = &b.x; float* pc = &c.x;
                    for (int q = 0; q < 10; q++) pc[q] = pa[q] + (pb[q] - pa[q]) * f;
                    tmp[m++] = c;
                }
            }
            n = m;
            std::memcpy(poly, tmp, sizeof(FV) * m);
            if (n < 3) break;
        }
        if (n < 3) continue;
        // Screen space.
        struct SV { float x, y, z, iw, sw, tw, r, g, b, a; } sv[9];
        for (int k = 0; k < n; k++) {
            const FV& p = poly[k];
            float iw = 1.0f / p.w;
            sv[k].x = st.vp_trans[0] + view_shift + p.x * iw * st.vp_scale[0];
            sv[k].y = st.vp_trans[1] - p.y * iw * st.vp_scale[1];
            sv[k].z = st.vp_trans[2] + p.z * iw * st.vp_scale[2];
            sv[k].iw = iw; sv[k].sw = p.s * iw; sv[k].tw = p.t * iw;
            sv[k].r = p.r; sv[k].g = p.g; sv[k].b = p.b; sv[k].a = p.a;
        }
        for (int k = 1; k + 1 < n; k++) {
            const SV& a = sv[0]; const SV& b = sv[k]; const SV& c = sv[k + 1];
            float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (std::fabs(area) < 1e-6f) continue;
            int x0 = (int)std::floor(std::max(sx0, std::min({a.x, b.x, c.x})));
            int x1 = (int)std::ceil(std::min(sx1, std::max({a.x, b.x, c.x})));
            int y0 = (int)std::floor(std::max(sy0, std::min({a.y, b.y, c.y})));
            int y1 = (int)std::ceil(std::min(sy1, std::max({a.y, b.y, c.y})));
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
                    float w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float iw = w0 * a.iw + w1 * b.iw + w2 * c.iw;
                    float s = (w0 * a.sw + w1 * b.sw + w2 * c.sw) / iw;
                    float tt = (w0 * a.tw + w1 * b.tw + w2 * c.tw) / iw;
                    float d = w0 * a.z + w1 * b.z + w2 * c.z;
                    if (d < 0.0f) d = 0.0f;  // closer than the near plane: held at the nearest (NoN), as the RSX clamps
                    if (mipmaps && st.mip_levels) {
                        auto at = [&](float qx, float qy, float* os, float* ot) {
                            float e0 = ((b.x - qx) * (c.y - qy) - (c.x - qx) * (b.y - qy)) / area;
                            float e1 = ((c.x - qx) * (a.y - qy) - (a.x - qx) * (c.y - qy)) / area;
                            float e2 = 1.0f - e0 - e1;
                            float jw = e0 * a.iw + e1 * b.iw + e2 * c.iw;
                            *os = (e0 * a.sw + e1 * b.sw + e2 * c.sw) / jw * st.tmap[0].shift_s;
                            *ot = (e0 * a.tw + e1 * b.tw + e2 * c.tw) / jw * st.tmap[0].shift_t;
                        };
                        float s0, t0, sx, tx, sy, ty;
                        at(px, py, &s0, &t0); at(px + 1, py, &sx, &tx); at(px, py + 1, &sy, &ty);
                        const float dx = std::sqrt((sx - s0) * (sx - s0) + (tx - t0) * (tx - t0));
                        const float dy = std::sqrt((sy - s0) * (sy - s0) + (ty - t0) * (ty - t0));
                        const float m = std::max(dx, dy);
                        pix_lod_ = m > 0 ? std::log(m) * 1.4426950f : -10.0f;
                    }
                    float shade[4] = {w0 * a.r + w1 * b.r + w2 * c.r, w0 * a.g + w1 * b.g + w2 * c.g,
                                      w0 * a.b + w1 * b.b + w2 * c.b, w0 * a.a + w1 * b.a + w2 * c.a};
                    shade_pixel(st, t, z, x, y, d, shade, s, tt, use_z);
                }
            }
        }
    }
}

static void trace_state(const char* what, const DrawState& st, float ulx, float uly, float lrx, float lry) {
    std::printf("[raster] %s %.1f,%.1f-%.1f,%.1f cimg 0x%06X cyc %d omh 0x%06X oml 0x%08X comb 0x%06X:%08X prim %.2f %.2f %.2f %.2f env %.2f %.2f %.2f %.2f fill 0x%08X tex %p\n",
        what, ulx, uly, lrx, lry, st.cimg_addr, (int)st.cycle(), st.om_h, st.om_l, st.combine_hi, st.combine_lo,
        st.prim[0], st.prim[1], st.prim[2], st.prim[3], st.env[0], st.env[1], st.env[2], st.env[3], st.fill_color, (const void*)st.tex[0]);
}

void SoftRaster::fill_rect(const DrawState& st, float ulx, float uly, float lrx, float lry) {
    if (trace) trace_state("fill", st, ulx, uly, lrx, lry);
    Target& t = target(st);
    wide_rect(st, t.width, t.margin, false, &ulx, &lrx, &views_);
    float wx0, wx1;
    wide_scissor_rect(st, t.width, t.margin, views_, &wx0, &wx1);
    int x0 = (int)std::floor(std::max({-(float)t.margin, ulx, wx0})), y0 = (int)std::max({0.0f, uly, st.sc_uly});
    int x1 = (int)std::min({(float)(t.width + t.margin), lrx, wx1}), y1 = (int)std::min({(float)t.height, lry, st.sc_lry});
    if (st.cycle() == CYC_FILL) {
        float c[4];
        if (t.bpp == 2) rgba5551((uint16_t)(st.fill_color >> 16), c);
        else { c[0] = (st.fill_color >> 24) / 255.0f; c[1] = ((st.fill_color >> 16) & 255) / 255.0f; c[2] = ((st.fill_color >> 8) & 255) / 255.0f; c[3] = 1; }
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                uint8_t* p = &t.rgba[t.index(x, y) * 4];
                p[0] = (uint8_t)(c[0] * 255); p[1] = (uint8_t)(c[1] * 255); p[2] = (uint8_t)(c[2] * 255); p[3] = 255;
            }
            if ((uint32_t)y + 1 > t.max_y) t.max_y = y + 1;
        }
        return;
    }
    std::vector<float>* z = st.z_buffered() ? &depth(st, t) : nullptr;
    float shade[4] = {0, 0, 0, 0};
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) shade_pixel(st, t, z, x, y, st.prim_depth, shade, 0, 0, z != nullptr);
}

void SoftRaster::tex_rect(const DrawState& st_in, float ulx, float uly, float lrx, float lry,
                          float s, float t0, float dsdx, float dtdy, bool flip) {
    const DrawState& st = with_frame_textures(st_in);
    if (trace) { trace_state("texrect", st, ulx, uly, lrx, lry); std::printf("[raster]   s %.2f t %.2f dsdx %.3f dtdy %.3f flip %d\n", s, t0, dsdx, dtdy, flip); }
    Target& t = target(st);
    if (film_.note(st, ulx, uly, lrx, lry) || bullet_hole(st)) wide_film(t.width, t.margin, &ulx, &lrx);
    float wx0, wx1;
    wide_scissor_rect(st, t.width, t.margin, views_, &wx0, &wx1);
    int x0 = (int)std::floor(std::max({-(float)t.margin, ulx, wx0})), y0 = (int)std::max({0.0f, uly, st.sc_uly});
    int x1 = (int)std::min({(float)(t.width + t.margin), lrx, wx1}), y1 = (int)std::min({(float)t.height, lry, st.sc_lry});
    float shade[4] = {0, 0, 0, 0};
    const DrawState& rs = st;
    if (trace && st.z_buffered() && st.z_source_prim() && x1 > x0 && y1 > y0) {
        std::vector<float>& z = depth(st, t);
        float zmin = 2, zmax = -1;
        int pass = 0, n = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) { float v = z[t.index(x, y)]; zmin = std::min(zmin, v); zmax = std::max(zmax, v); n++; pass += st.prim_depth <= v; }
        std::printf("[raster]   prim depth %.5f; z buffer under the rectangle %.5f..%.5f; %d of %d pixels would pass\n", st.prim_depth, zmin, zmax, pass, n);
        std::map<int, int> who;
        auto& w = zwho_[&z];
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) who[w.size() ? w[t.index(x, y)] : -1]++;
        std::printf("[raster]   depth written by triangle draw:");
        for (auto& kv : who) std::printf(" %d (%d px)", kv.first, kv.second);
        std::printf("\n");
    }
    // Hidden by depth with the primitive's depth, as the N64 does (the game's dust and sparks).
    std::vector<float>* zb = st.z_compare() && st.z_source_prim() ? &depth(st, t) : nullptr;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = x - ulx, dy = y - uly;
            float ss = flip ? s + dy * dsdx : s + dx * dsdx;
            float tt = flip ? t0 + dx * dtdy : t0 + dy * dtdy;
            shade_pixel(rs, t, zb, x, y, rs.prim_depth, shade, ss, tt, zb != nullptr);
        }
    }
}

bool write_png(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba, (int)width, (int)height, 4, &len, 6, MZ_FALSE);
    if (!png) return false;
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(png, 1, len, f); std::fclose(f); }
    mz_free(png);
    return f != nullptr;
}

}  // namespace n64gfx
