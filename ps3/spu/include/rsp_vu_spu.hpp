// SPU SIMD versions of the hottest RSP vector instructions (included by the SPU build's generated
// rsp_vu_impl.hpp; see ps3/spu/audio/gen_vu_impl.py). Bit-identical to the scalar code they replace:
// checked against the x86 SSE build's results for every element mode (tools/spu-sim, vuops).
//
// Elements 0-7 are the SPU's halfwords 0-7 (big-endian, as the N64). The 48-bit accumulator is
// worked on as two sets of four 32-bit lanes, even and odd elements, each lane a (top, low) pair:
// top = ACCH sign-extended, low = ACCM << 16 | ACCL.
#pragma once
#include <spu_intrinsics.h>

namespace spuvu {
typedef vec_uchar16 vb;
typedef vec_short8 vs8;
typedef vec_ushort8 vu8;
typedef vec_int4 vi4;
typedef vec_uint4 vu4;

inline vb ld(const RSP::r128& r) { return *reinterpret_cast<const vb*>(&r); }
inline void st(RSP::r128& r, vb v) { *reinterpret_cast<vb*>(&r) = v; }

// vt(e): which element each element takes.
const vb kVte[16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 0, 1, 4, 5, 4, 5, 8, 9, 8, 9, 12, 13, 12, 13},
    {2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15},
    {0, 1, 0, 1, 0, 1, 0, 1, 8, 9, 8, 9, 8, 9, 8, 9},
    {2, 3, 2, 3, 2, 3, 2, 3, 10, 11, 10, 11, 10, 11, 10, 11},
    {4, 5, 4, 5, 4, 5, 4, 5, 12, 13, 12, 13, 12, 13, 12, 13},
    {6, 7, 6, 7, 6, 7, 6, 7, 14, 15, 14, 15, 14, 15, 14, 15},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
    {2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3},
    {4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5, 4, 5},
    {6, 7, 6, 7, 6, 7, 6, 7, 6, 7, 6, 7, 6, 7, 6, 7},
    {8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9},
    {10, 11, 10, 11, 10, 11, 10, 11, 10, 11, 10, 11, 10, 11, 10, 11},
    {12, 13, 12, 13, 12, 13, 12, 13, 12, 13, 12, 13, 12, 13, 12, 13},
    {14, 15, 14, 15, 14, 15, 14, 15, 14, 15, 14, 15, 14, 15, 14, 15},
};
template <u8 e> inline vb vte(const RSP::r128& vt) { const vb v = ld(vt); return spu_shuffle(v, v, kVte[e]); }

// Halfword vector from the low (high) halves of two word vectors: element 2k from a's word k,
// element 2k+1 from b's word k.
const vb kPackLo = {2, 3, 18, 19, 6, 7, 22, 23, 10, 11, 26, 27, 14, 15, 30, 31};
const vb kPackHi = {0, 1, 16, 17, 4, 5, 20, 21, 8, 9, 24, 25, 12, 13, 28, 29};
inline vb pack_lo(vi4 a, vi4 b) { return spu_shuffle((vb)a, (vb)b, kPackLo); }
inline vb pack_hi(vi4 a, vi4 b) { return spu_shuffle((vb)a, (vb)b, kPackHi); }

// Elements as 32-bit lanes.
inline vi4 even_s(vb v) { return spu_rlmaska((vi4)v, -16); }
inline vi4 odd_s(vb v) { return spu_extend((vs8)v); }
inline vi4 even_u(vb v) { return (vi4)spu_rlmask((vu4)v, -16); }
inline vi4 odd_u(vb v) { return (vi4)spu_and((vu4)v, 0xFFFF); }

// Products of elements, as 32-bit lanes (they always fit).
inline vi4 mul_ss_e(vb a, vb b) { return spu_mule((vs8)a, (vs8)b); }
inline vi4 mul_ss_o(vb a, vb b) { return spu_mulo((vs8)a, (vs8)b); }
inline vi4 mul_uu_e(vb a, vb b) { return (vi4)spu_mule((vu8)a, (vu8)b); }
inline vi4 mul_uu_o(vb a, vb b) { return (vi4)spu_mulo((vu8)a, (vu8)b); }
// Unsigned a times signed b: the signed product, plus b * 65536 where a's top bit is set.
inline vi4 mul_us_e(vb a, vb b) {
    const vi4 neg = (vi4)spu_cmpgt(spu_splats(0), even_s(a));
    return spu_add(mul_ss_e(a, b), spu_and(spu_and((vi4)b, spu_splats((int)0xFFFF0000)), neg));
}
inline vi4 mul_us_o(vb a, vb b) {
    const vi4 neg = (vi4)spu_cmpgt(spu_splats(0), odd_s(a));
    return spu_add(mul_ss_o(a, b), spu_and(spu_sl((vi4)b, 16), neg));
}
inline vi4 mul_su_e(vb a, vb b) { return mul_us_e(b, a); }
inline vi4 mul_su_o(vb a, vb b) { return mul_us_o(b, a); }

struct Acc { vi4 top_e, low_e, top_o, low_o; };
inline Acc acc_load(const RSP& r) {
    const vb h = ld(r.vpu.acch), m = ld(r.vpu.accm), l = ld(r.vpu.accl);
    Acc a;
    a.top_e = even_s(h);
    a.top_o = odd_s(h);
    a.low_e = (vi4)spu_shuffle(m, l, kPackHi);
    a.low_o = (vi4)spu_shuffle(m, l, kPackLo);
    return a;
}
inline void acc_store(RSP& r, const Acc& a) {
    st(r.vpu.acch, pack_lo(a.top_e, a.top_o));
    st(r.vpu.accm, pack_hi(a.low_e, a.low_o));
    st(r.vpu.accl, pack_lo(a.low_e, a.low_o));
}
// Adds a signed 32-bit value (sign-extended to 48 bits) to one parity's lanes.
// The top is kept as ACCH is: 16 bits (wrapping), sign-extended.
inline vi4 wrap16(vi4 top) { return spu_extend((vs8)top); }
inline void add48(vi4& top, vi4& low, vi4 x) {
    const vu4 c = spu_genc((vu4)low, (vu4)x);
    low = spu_add(low, x);
    top = wrap16(spu_add(spu_add(top, spu_rlmaska(x, -31)), (vi4)c));
}
// The accumulator's top 32 bits (ACCH:ACCM), signed.
inline vi4 hm(vi4 top, vi4 low) { return spu_or(spu_sl(top, 16), (vi4)spu_rlmask((vu4)low, -16)); }
inline vi4 clamp16(vi4 x) {
    x = spu_sel(x, spu_splats(32767), spu_cmpgt(x, spu_splats(32767)));
    return spu_sel(x, spu_splats(-32768), spu_cmpgt(spu_splats(-32768), x));
}
// accumulatorSaturate with slice 1 (ACCM, clamped to signed 16 bits) and slice 0 (ACCL, or 0 / 0xFFFF).
inline vb sat_mid(const Acc& a) { return pack_lo(clamp16(hm(a.top_e, a.low_e)), clamp16(hm(a.top_o, a.low_o))); }
inline vi4 sat_low1(vi4 top, vi4 low) {
    const vi4 h = hm(top, low);
    vi4 r = spu_and(low, spu_splats(0xFFFF));
    r = spu_sel(r, spu_splats(0xFFFF), spu_cmpgt(h, spu_splats(32767)));
    return spu_sel(r, spu_splats(0), spu_cmpgt(spu_splats(-32768), h));
}
inline vb sat_low(const Acc& a) { return pack_lo(sat_low1(a.top_e, a.low_e), sat_low1(a.top_o, a.low_o)); }

template <u8 e> inline void VADD(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    const vb c = (vb)spu_andc(spu_splats((unsigned short)1), spu_cmpeq((vu8)ld(r.vpu.vcol), spu_splats((unsigned short)0)));
    const vi4 re = spu_add(spu_add(even_s(s), even_s(t)), even_u(c));
    const vi4 ro = spu_add(spu_add(odd_s(s), odd_s(t)), odd_u(c));
    st(r.vpu.accl, pack_lo(re, ro));
    st(vd, pack_lo(clamp16(re), clamp16(ro)));
    st(r.vpu.vcol, (vb)spu_splats(0));
    st(r.vpu.vcoh, (vb)spu_splats(0));
}
template <u8 e> inline void VSUB(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    const vb c = (vb)spu_andc(spu_splats((unsigned short)1), spu_cmpeq((vu8)ld(r.vpu.vcol), spu_splats((unsigned short)0)));
    const vi4 re = spu_sub(spu_sub(even_s(s), even_s(t)), even_u(c));
    const vi4 ro = spu_sub(spu_sub(odd_s(s), odd_s(t)), odd_u(c));
    st(r.vpu.accl, pack_lo(re, ro));
    st(vd, pack_lo(clamp16(re), clamp16(ro)));
    st(r.vpu.vcol, (vb)spu_splats(0));
    st(r.vpu.vcoh, (vb)spu_splats(0));
}
// VMULF (U = 0): acc = vs * vt * 2 + 0x8000; vd = ACCM clamped.
template <u8 e> inline void VMULF(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a;
    const vi4 pe = mul_ss_e(s, t), po = mul_ss_o(s, t);
    // 2p + 0x8000 is negative (as a 48-bit value) exactly when p < -0x4000.
    a.low_e = spu_add(spu_sl(pe, 1), spu_splats(0x8000));
    a.low_o = spu_add(spu_sl(po, 1), spu_splats(0x8000));
    a.top_e = (vi4)spu_cmpgt(spu_splats(-0x4000), pe);
    a.top_o = (vi4)spu_cmpgt(spu_splats(-0x4000), po);
    acc_store(r, a);
    st(vd, sat_mid(a));
}
// VMACF (U = 0): acc += vs * vt * 2; vd = ACCM clamped.
template <u8 e> inline void VMACF(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a = acc_load(r);
    const vi4 pe = mul_ss_e(s, t), po = mul_ss_o(s, t);
    // 2p as a 48-bit value: low 32 bits p << 1, sign that of p.
    const vi4 xe = spu_sl(pe, 1), xo = spu_sl(po, 1);
    const vu4 ce = spu_genc((vu4)a.low_e, (vu4)xe), co = spu_genc((vu4)a.low_o, (vu4)xo);
    a.low_e = spu_add(a.low_e, xe);
    a.low_o = spu_add(a.low_o, xo);
    a.top_e = wrap16(spu_add(spu_add(a.top_e, spu_rlmaska(pe, -31)), (vi4)ce));
    a.top_o = wrap16(spu_add(spu_add(a.top_o, spu_rlmaska(po, -31)), (vi4)co));
    acc_store(r, a);
    st(vd, sat_mid(a));
}
// VMADH: ACCH:ACCM += vs * vt (32-bit wrap); vd = that clamped.
template <u8 e> inline void VMADH(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a = acc_load(r);
    const vi4 re = spu_add(hm(a.top_e, a.low_e), mul_ss_e(s, t));
    const vi4 ro = spu_add(hm(a.top_o, a.low_o), mul_ss_o(s, t));
    a.top_e = spu_rlmaska(re, -16);
    a.top_o = spu_rlmaska(ro, -16);
    a.low_e = spu_or(spu_sl(re, 16), spu_and(a.low_e, spu_splats(0xFFFF)));
    a.low_o = spu_or(spu_sl(ro, 16), spu_and(a.low_o, spu_splats(0xFFFF)));
    acc_store(r, a);
    st(vd, pack_lo(clamp16(re), clamp16(ro)));
}
// VMADN: acc += vs (unsigned) * vt (signed); vd = ACCL saturated.
template <u8 e> inline void VMADN(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a = acc_load(r);
    add48(a.top_e, a.low_e, mul_us_e(s, t));
    add48(a.top_o, a.low_o, mul_us_o(s, t));
    acc_store(r, a);
    st(vd, sat_low(a));
}
// VMADM: acc += vs (signed) * vt (unsigned); vd = ACCM clamped.
template <u8 e> inline void VMADM(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a = acc_load(r);
    add48(a.top_e, a.low_e, mul_su_e(s, t));
    add48(a.top_o, a.low_o, mul_su_o(s, t));
    acc_store(r, a);
    st(vd, sat_mid(a));
}
// VMADL: acc += (vs * vt (both unsigned)) >> 16; vd = ACCL saturated.
template <u8 e> inline void VMADL(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a = acc_load(r);
    add48(a.top_e, a.low_e, (vi4)spu_rlmask((vu4)mul_uu_e(s, t), -16));
    add48(a.top_o, a.low_o, (vi4)spu_rlmask((vu4)mul_uu_o(s, t), -16));
    acc_store(r, a);
    st(vd, sat_low(a));
}
// VMUDH: acc = (vs * vt) << 16; vd = vs * vt clamped.
template <u8 e> inline void VMUDH(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    const vi4 pe = mul_ss_e(s, t), po = mul_ss_o(s, t);
    Acc a;
    a.top_e = spu_rlmaska(pe, -16);
    a.top_o = spu_rlmaska(po, -16);
    a.low_e = spu_sl(pe, 16);
    a.low_o = spu_sl(po, 16);
    acc_store(r, a);
    st(vd, pack_lo(clamp16(pe), clamp16(po)));
}
// VMUDM: acc = vs (signed) * vt (unsigned), sign-extended; vd = ACCM.
template <u8 e> inline void VMUDM(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a;
    a.low_e = mul_su_e(s, t);
    a.low_o = mul_su_o(s, t);
    a.top_e = spu_rlmaska(a.low_e, -31);
    a.top_o = spu_rlmaska(a.low_o, -31);
    acc_store(r, a);
    st(vd, ld(r.vpu.accm));
}
// VMUDN: acc = vs (unsigned) * vt (signed), sign-extended; vd = ACCL.
template <u8 e> inline void VMUDN(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a;
    a.low_e = mul_us_e(s, t);
    a.low_o = mul_us_o(s, t);
    a.top_e = spu_rlmaska(a.low_e, -31);
    a.top_o = spu_rlmaska(a.low_o, -31);
    acc_store(r, a);
    st(vd, ld(r.vpu.accl));
}
// VMUDL: acc = (vs * vt (both unsigned)) >> 16; vd = ACCL.
template <u8 e> inline void VMUDL(RSP& r, RSP::r128& vd, const RSP::r128& vs, const RSP::r128& vt) {
    const vb s = ld(vs), t = vte<e>(vt);
    Acc a;
    a.low_e = (vi4)spu_rlmask((vu4)mul_uu_e(s, t), -16);
    a.low_o = (vi4)spu_rlmask((vu4)mul_uu_o(s, t), -16);
    a.top_e = spu_splats(0);
    a.top_o = spu_splats(0);
    acc_store(r, a);
    st(vd, ld(r.vpu.accl));
}
}  // namespace spuvu
