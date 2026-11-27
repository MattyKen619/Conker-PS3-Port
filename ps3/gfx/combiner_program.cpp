// Combiner -> fragment program generator (see combiner_program.h).
#include "combiner_program.h"

#include <cstdio>

namespace n64gfx {

namespace {
    // Operands. Temps: R0 texel 0, R1 texel 1, R2 combined, R3-R6 scratch, R7-R10 copies of the inputs
    // (the RSX reads only one interpolated input per instruction): R7 shade, R8 prim, R9 env, R10 LOD/fog.
    // c[0] = (1, 0, 0.5, 0): ONE = c[0].xxxx, ZERO = c[0].yyyy, NOISE ~ c[0].zzzz.
    const char* kOne = "c[0].xxxx";
    const char* kZero = "c[0].yyyy";

    const char* color_a(int s) {  // A: 0 combined, 1 tex0, 2 tex1, 3 prim, 4 shade, 5 env, 6 one, 7 noise
        switch (s) {
        case 0: return "R2"; case 1: return "R0"; case 2: return "R1"; case 3: return "R8";
        case 4: return "R7"; case 5: return "R9"; case 6: return kOne; case 7: return "c[0].zzzz";
        default: return kZero;
        }
    }
    const char* color_b(int s) {  // B: like A, 6 key center, 7 convert K4 -> 0
        switch (s) {
        case 0: return "R2"; case 1: return "R0"; case 2: return "R1"; case 3: return "R8";
        case 4: return "R7"; case 5: return "R9";
        default: return kZero;
        }
    }
    const char* color_c(int s) {  // C: 0-5 as A, 6 key scale, 7-12 alphas, 13 LOD, 14 prim LOD, 15 K5
        switch (s) {
        case 0: return "R2"; case 1: return "R0"; case 2: return "R1"; case 3: return "R8";
        case 4: return "R7"; case 5: return "R9";
        case 7: return "R2.wwww"; case 8: return "R0.wwww"; case 9: return "R1.wwww";
        case 10: return "R8.wwww"; case 11: return "R7.wwww"; case 12: return "R9.wwww";
        case 14: return "R10.xxxx";
        case 15: return "R11.wwww";  // K5, passed in f[TEX1].w
        default: return kZero;
        }
    }
    const char* color_d(int s) {  // D: 0-5 as A, 6 one, 7 zero
        switch (s) {
        case 0: return "R2"; case 1: return "R0"; case 2: return "R1"; case 3: return "R8";
        case 4: return "R7"; case 5: return "R9"; case 6: return kOne;
        default: return kZero;
        }
    }
    const char* alpha_abd(int s) {  // alpha A, B, D: 0 combined, 1 tex0, 2 tex1, 3 prim, 4 shade, 5 env, 6 one, 7 zero
        switch (s) {
        case 0: return "R2.wwww"; case 1: return "R0.wwww"; case 2: return "R1.wwww"; case 3: return "R8.wwww";
        case 4: return "R7.wwww"; case 5: return "R9.wwww"; case 6: return kOne;
        default: return kZero;
        }
    }
    const char* alpha_c(int s) {  // alpha C: 0 LOD fraction, 1-5 as above, 6 prim LOD fraction, 7 zero
        switch (s) {
        case 1: return "R0.wwww"; case 2: return "R1.wwww"; case 3: return "R8.wwww";
        case 4: return "R7.wwww"; case 5: return "R9.wwww"; case 6: return "R10.xxxx";
        default: return kZero;
        }
    }
    bool uses_tex(const CombinerCycle& c, int t) {
        int v = 1 + t;
        return c.a == v || c.b == v || c.c == v || c.d == v || c.c == 8 + t ||
               c.aa == v || c.ab == v || c.ac == v || c.ad == v;
    }
}

float alpha_threshold(const DrawState& st) {
    float th = 0;
    if (st.cycle() == CYC_COPY) return st.alpha_compare() == 1 ? 0.5f : 0.0f;
    if (st.alpha_compare() == 1) th = st.blend[3] > 0 ? st.blend[3] : 0.5f / 255.0f;
    if (st.cvg_x_alpha() && st.alpha_cvg_sel()) th = th > 0.5f ? th : 0.5f;
    // ALPHA_CVG_SEL alone (no CVG_X_ALPHA): coverage, not the combined alpha, decides; no test.
    return th;
}

ProgramKey make_program_key(const DrawState& st, bool fill_rect) {
    ProgramKey k;
    CycleType cyc = st.cycle();
    if (fill_rect && cyc == CYC_FILL) { k.mode = ProgramMode::Shade; return k; }
    if (cyc == CYC_COPY) {
        k.mode = ProgramMode::Copy;
        k.tex_mask = st.tex[0] ? 1 : 0;
        k.alpha_test = alpha_threshold(st) > 0;
        if (!st.tex[0]) k.mode = ProgramMode::Shade;
        return k;
    }
    if (cyc == CYC_FILL) { k.mode = ProgramMode::Shade; return k; }
    CombinerCycle cc[2];
    decode_combiner(st.combine_hi, st.combine_lo, cc);
    if (cyc == CYC_2) {
        k.mode = ProgramMode::Combine2;
        k.combine_hi = st.combine_hi;
        k.combine_lo = st.combine_lo;
    } else {
        k.mode = ProgramMode::Combine1;
        // Keep only cycle 0's fields.
        k.combine_hi = st.combine_hi & 0xFFFE00;   // a0, c0, Aa0, Ac0
        k.combine_lo = st.combine_lo & 0xF003FE00;  // b0, d0, Ab0, Ad0
    }
    k.tex_mask = (st.tex[0] ? 1 : 0) | (st.tex[1] ? 2 : 0);
    k.alpha_test = alpha_threshold(st) > 0;
    // The 2-cycle fog blender: first blender cycle = (FOG color, SHADE alpha, IN color, 1 - A).
    if (cyc == CYC_2) {
        uint32_t l = st.om_l;
        uint32_t P = (l >> 30) & 3, A = (l >> 26) & 3, M = (l >> 22) & 3, B = (l >> 18) & 3;
        k.fog = P == 3 && A == 2 && M == 0 && B == 0;
        if (A == 1 && B == 0) k.fog_const = (P == 0 && M == 3) ? 1 : (P == 3 && M == 0) ? 2 : 0;
    }
    (void)cc;
    return k;
}

std::string generate_program(const ProgramKey& key) {
    std::string s = "!!ARBfp1.0\n#const c[0] = 1 0 0.5 0\n";
    if (key.mode == ProgramMode::Gray) s += "#const c[1] = 0.299 0.587 0.114 0\n";
    char line[160];
    if (key.dither && key.dscale) {
        // (One constant an instruction: the RSX keeps an instruction's constant inline after it, so a
        // second one in the same instruction would share its slot. v78 had three, and drew black.)
        std::snprintf(line, sizeof line, "#const c[2] = %.9f 0.25 0.5 %.9f\n#const c[3] = 255 7 0.125 31\n#const c[4] = 8 0.25 %.9f 0.5\n",
                      1.0 / key.dscale, key.dither == 3 ? 0.5625 : 0.0625, 1.0 / 255.0);
        s += line;
    }
    if (key.three_point) {
        // The textures' sizes in texels and a texel in 0..1 (c[8], c[9]: texture 0 .xy, 1 .zw).
        std::snprintf(line, sizeof line, "#const c[7] = 0.5 1 0 0\n#const c[8] = %u %u %u %u\n#const c[9] = %.9f %.9f %.9f %.9f\n",
                      key.tp_w[0], key.tp_h[0], key.tp_w[1], key.tp_h[1], key.tp_w[0] ? 1.0 / key.tp_w[0] : 0.0,
                      key.tp_h[0] ? 1.0 / key.tp_h[0] : 0.0, key.tp_w[1] ? 1.0 / key.tp_w[1] : 0.0, key.tp_h[1] ? 1.0 / key.tp_h[1] : 0.0);
        s += line;
    }
    if (key.soft_w) {
        std::snprintf(line, sizeof line, "#const c[5] = %.9f 0 0 0\n#const c[6] = 0.5 0.25 0 0\n", 0.5 / key.soft_w);
        s += line;
    }
    auto emit = [&](const char* fmt, auto... args) { std::snprintf(line, sizeof line, fmt, args...); s += line; s += "\n"; };

    if (key.mode == ProgramMode::Shade) {
        emit("MOV R2, f[COL0];");
    } else if (key.mode == ProgramMode::Gray) {
        emit("TEX R2, f[TEX0], texture[0], 2D;");
        emit("DP3 R3.x, R2, c[1];");
        emit("MOV R2.xyz, R3.xxxx;");
    } else if (key.mode == ProgramMode::Copy) {
        if (key.tex_mask & 1) emit("TEX R2, f[TEX0], texture[0], 2D;");
        else emit("MOV R2, f[COL0];");
        if ((key.tex_mask & 1) && key.soft_w) {
            // Softened sideways, as the N64's video output: half the pixel, a quarter each from half an
            // N64 pixel to the left and to the right.
            emit("ADD R3, f[TEX0], c[5];");
            emit("TEX R4, R3, texture[0], 2D;");
            emit("ADD R3, f[TEX0], -c[5];");
            emit("TEX R5, R3, texture[0], 2D;");
            emit("ADD R4, R4, R5;");
            emit("MUL R4, R4, c[6].yyyy;");
            emit("MAD R2, R2, c[6].xxxx, R4;");
        }
    } else {
        CombinerCycle cc[2];
        decode_combiner(key.combine_hi, key.combine_lo, cc);
        int ncyc = key.mode == ProgramMode::Combine2 ? 2 : 1;
        bool want[2] = {false, false};
        for (int c = 0; c < ncyc; c++) { want[0] |= uses_tex(cc[c], 0); want[1] |= uses_tex(cc[c], 1); }
        if (want[1] && !(key.tex_mask & 2)) want[0] = true;  // TEXEL1 falls back to texture 0
        // The RDP's 3-point filter (RT64's, from emutalk's): the 3 texels nearest of the 4 around the
        // point, blended across the triangle it falls in; the texture unit samples nearest; the sizes are
        // constants (c[8], c[9]; c[7] = 0.5 1 0 0). Same texel centres as the RSX's own bilinear filter
        // (point = uv * size - 0.5). Registers R14-R24 (free again after).
        auto three_point = [&](int u, const char* dst) {
            const char* tc = u ? "f[TEX1]" : "f[TEX0]";
            const char* sz = u ? "c[8].zwzw" : "c[8].xyxy";   // texels
            const char* tx = u ? "c[9].zwzw" : "c[9].xyxy";   // a texel in 0..1
            // Read with a MOV first: fpasm (as PSL1GHT's cgcomp) turns off perspective correction for
            // an input a MUL reads (v80-v84's flat-coloured polygons and Conker's missing pupils; the
            // self-test's square, all at the same depth, couldn't tell).
            emit("MOV R14.xy, %s;", tc);
            emit("MUL R14.xy, R14, %s;", sz);
            emit("ADD R14.xy, R14, -c[7].xxxx;");      // in texels, from texel centres
            emit("FLR R15.xy, R14;");
            emit("ADD R16.xy, R14, -R15;");            // the fraction
            emit("ADD R15.xy, R15, c[7].xxxx;");
            emit("MUL R17.xy, R15, %s;", tx);          // texel (0,0), in 0..1
            emit("MOV R18.y, R17.y;");
            emit("ADD R18.x, R17.x, %s;", u ? "c[9].z" : "c[9].x");  // (1,0)
            emit("MOV R19.x, R17.x;");
            emit("ADD R19.y, R17.y, %s;", u ? "c[9].w" : "c[9].y");  // (0,1)
            emit("ADD R20.xy, R17, %s;", tx);          // (1,1)
            emit("TEX R21, R17, texture[%d], 2D;", u);
            emit("TEX R22, R18, texture[%d], 2D;", u);
            emit("TEX R23, R19, texture[%d], 2D;", u);
            emit("TEX R24, R20, texture[%d], 2D;", u);
            // tri0 = s00 + (s10 - s00) fx + (s01 - s00) fy;  tri1 = s11 + (s01 - s11)(1 - fx) + (s10 - s11)(1 - fy)
            emit("ADD R17, R22, -R21;");
            emit("MAD R17, R17, R16.xxxx, R21;");
            emit("ADD R18, R23, -R21;");
            emit("MAD R17, R18, R16.yyyy, R17;");      // tri0
            emit("ADD R19.xy, c[7].yyyy, -R16;");      // 1 - f
            emit("ADD R18, R23, -R24;");
            emit("MAD R18, R18, R19.xxxx, R24;");
            emit("ADD R20, R22, -R24;");
            emit("MAD R18, R20, R19.yyyy, R18;");      // tri1
            emit("ADD R19.z, R16.x, R16.y;");
            emit("SGE R19.z, R19.z, c[7].y;");         // past the diagonal
            emit("LRP %s, R19.zzzz, R18, R17;", dst);
        };
        if (want[0]) {
            if ((key.tex_mask & 1) && (key.three_point & 1)) three_point(0, "R0");
            else if (key.tex_mask & 1) emit("TEX R0, f[TEX0], texture[0], 2D;");
            else emit("MOV R0, %s;", kZero);
        }
        if (want[1]) {
            if ((key.tex_mask & 2) && (key.three_point & 2)) three_point(1, "R1");
            else if (key.tex_mask & 2) emit("TEX R1, f[TEX1], texture[1], 2D;");
            else if (key.tex_mask & 1) emit("MOV R1, R0;");
            else emit("MOV R1, %s;", kZero);
        }
        emit("MOV R7, f[COL0];");
        emit("MOV R8, f[TEX2];");
        emit("MOV R9, f[TEX3];");
        emit("MOV R10, f[TEX4];");
        emit("MOV R11, f[TEX1];");
        emit("MOV R2, %s;", kZero);
        for (int c = 0; c < ncyc; c++) {
            const CombinerCycle& y = cc[c];
            emit("ADD R3, %s, -%s;", color_a(y.a), color_b(y.b));  // no SUB on the RSX assembler
            emit("MAD R4, R3, %s, %s;", color_c(y.c), color_d(y.d));
            emit("ADD R5, %s, -%s;", alpha_abd(y.aa), alpha_abd(y.ab));
            emit("MAD R4.w, R5, %s, %s;", alpha_c(y.ac), alpha_abd(y.ad));
            emit("MOV_SAT R2, R4;");
        }
        if (key.fog) emit("LRP R2.xyz, R7.wwww, R10.yzww, R2;");
        if (key.fog_const) {
            emit("MOV R12, f[TEX0];");
            if (key.fog_const == 1) emit("LRP R2.xyz, R12.zzzz, R2, R10.yzww;");
            else emit("LRP R2.xyz, R12.zzzz, R10.yzww, R2;");
        }
    }
    if (key.alpha_test) {
        emit("ADDC R6.x, R2.wwww, -f[TEX1].zzzz;");
        emit("KIL LT.x;");
    }
    if (key.dither && key.dscale) {
        // As the RDP writes a 16-bit frame (angrylion's rgb_dither): 5 bits a channel, rounded up when
        // the 3 bits dropped exceed the pattern's value d at this N64 pixel: floor((c + 7 - d) / 8).
        if (key.dither >= 2) {
            emit("MUL R13.xy, f[WPOS], c[2].xxxx;");  // N64 pixels
            emit("FLR R13.xy, R13;");
            emit("MUL R13.xy, R13, c[2].yyyy;");
            emit("FRC R13.xy, R13;");                  // x, y mod 4, over 4
            emit("MAD R13.x, R13.x, c[2].z, c[2].w;");  // the pattern's column (Bayer: the right half)
            emit("ADD R13.y, R13.y, c[3].z;");
            emit("TEX R14, R13, texture[2], 2D;");
            emit("MUL R14.x, R14.x, c[3].x;");
            emit("ADD R14.x, R14.x, c[4].w;");
            emit("FLR R14.x, R14.x;");                 // d
            emit("ADD R14.x, c[3].y, -R14.x;");        // 7 - d
        } else {
            emit("MOV R14.x, %s;", kZero);             // no dither: the 3 bits dropped
        }
        emit("MUL R15.xyz, R2, c[3].xxxx;");
        emit("ADD R15.xyz, R15, c[4].wwww;");
        emit("FLR R15.xyz, R15;");                     // the 8-bit colour
        emit("ADD R15.xyz, R15, R14.xxxx;");
        emit("MUL R15.xyz, R15, c[3].zzzz;");
        emit("FLR R15.xyz, R15;");
        emit("MIN R15.xyz, R15, c[3].wwww;");          // 5 bits
        emit("MUL R16.xyz, R15, c[4].yyyy;");
        emit("FLR R16.xyz, R16;");
        emit("MAD R15.xyz, R15, c[4].xxxx, R16;");     // shown as the VI does: (c << 3) | (c >> 2)
        emit("MUL R2.xyz, R15, c[4].zzzz;");
    }
    emit("MOV result.color, R2;");
    emit("END");
    return s;
}

}  // namespace n64gfx
