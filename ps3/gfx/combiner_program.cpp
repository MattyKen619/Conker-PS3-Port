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
    } else {
        CombinerCycle cc[2];
        decode_combiner(key.combine_hi, key.combine_lo, cc);
        int ncyc = key.mode == ProgramMode::Combine2 ? 2 : 1;
        bool want[2] = {false, false};
        for (int c = 0; c < ncyc; c++) { want[0] |= uses_tex(cc[c], 0); want[1] |= uses_tex(cc[c], 1); }
        if (want[1] && !(key.tex_mask & 2)) want[0] = true;  // TEXEL1 falls back to texture 0
        if (want[0]) {
            if (key.tex_mask & 1) emit("TEX R0, f[TEX0], texture[0], 2D;");
            else emit("MOV R0, %s;", kZero);
        }
        if (want[1]) {
            if (key.tex_mask & 2) emit("TEX R1, f[TEX1], texture[1], 2D;");
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
    emit("MOV result.color, R2;");
    emit("END");
    return s;
}

}  // namespace n64gfx
