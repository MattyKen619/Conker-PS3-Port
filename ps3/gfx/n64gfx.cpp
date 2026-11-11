// n64gfx: display list interpreter (see n64gfx.h).
#include "n64gfx.h"

#include <algorithm>
#include <type_traits>
#include <cmath>
#include <cstdio>
#include <unordered_map>


// Square root and arc cosine. The SPU's library versions work in double precision (slow there), so
// on an SPU these use its reciprocal square root estimate and a polynomial (Abramowitz & Stegun
// 4.4.46, error below 2e-8) instead.
#if defined(__SPU__)
#include <spu_intrinsics.h>
namespace {
inline float n_sqrt(float x) {
    if (!(x > 0.0f)) return 0.0f;
    float r = spu_extract(spu_rsqrte(spu_splats(x)), 0);
    r = r * (1.5f - 0.5f * x * r * r);
    r = r * (1.5f - 0.5f * x * r * r);
    return x * r;
}
inline float n_acos(float x) {
    const bool neg = x < 0.0f;
    float a = neg ? -x : x;
    if (a > 1.0f) a = 1.0f;
    const float p = 1.5707963050f + a * (-0.2145988016f + a * (0.0889789874f + a * (-0.0501743046f + a * (0.0308918810f +
                    a * (-0.0170881256f + a * (0.0066700901f + a * -0.0012624911f))))));
    const float r = n_sqrt(1.0f - a) * p;
    return neg ? 3.14159265f - r : r;
}
}  // namespace
#else
namespace {
inline float n_sqrt(float x) { return std::sqrt(x); }
inline float n_acos(float x) { return std::acos(x); }
}  // namespace
#endif

// The PS3's PPU pays a long stall whenever a value moves between its integer, floating-point and vector
// registers through memory right after being stored ("load-hit-store"), which every int <-> float
// conversion does. The vertex path therefore converts in batches, with the vector unit when available.
#if defined(__ALTIVEC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#include <altivec.h>
#undef vector
#undef pixel
#undef bool
#define N64GFX_VMX 1
typedef __vector float vf;
typedef __vector signed int vsi;
typedef __vector unsigned int vui;
typedef __vector signed short vss;
typedef __vector unsigned short vus;
typedef __vector unsigned char vuc;
#else
#define N64GFX_VMX 0
#endif


namespace n64gfx {

namespace {
    inline uint32_t bits(uint32_t w, int pos, int n) { return (w >> pos) & ((1u << n) - 1); }
    inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // Geometry mode bits (F3DEX2).
    constexpr uint32_t G_ZBUFFER = 0x00000001;
    constexpr uint32_t G_CULL_FRONT = 0x00000200;
    constexpr uint32_t G_CULL_BACK = 0x00000400;
    constexpr uint32_t G_FOG = 0x00010000;
    constexpr uint32_t G_LIGHTING = 0x00020000;
    constexpr uint32_t G_TEXTURE_GEN = 0x00040000;
    constexpr uint32_t G_TEXTURE_GEN_LINEAR = 0x00080000;
    constexpr uint32_t G_SHADING_SMOOTH = 0x00200000;
    constexpr uint32_t G_POINT_LIGHTING = 0x00400000;

    void mat_mul(const float a[4][4], const float b[4][4], float out[4][4]) {
        float r[4][4];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        std::memcpy(out, r, sizeof r);
    }
    void mat_identity(float m[4][4]) {
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m[i][j] = i == j ? 1.0f : 0.0f;
    }
    // Byte -> float tables (a table load needs no integer -> float conversion).
    struct ByteTables {
        float u8[256], s8n[256];
        ByteTables() {
            for (int i = 0; i < 256; i++) { u8[i] = (float)i; s8n[i] = (int8_t)(uint8_t)i / 127.0f; }
        }
    };
    const ByteTables kTables;
    const float* const kU8f = kTables.u8;    // b -> (float)b
    const float* const kS8n = kTables.s8n;   // b -> (int8_t)b / 127, a normal component

    // Truncates every value of rows[0..n) toward zero, as (float)(int32_t)v (|v| < 2^31).
    void trunc_rows(float (*rows)[4], uint32_t n) {
#if N64GFX_VMX
        for (uint32_t i = 0; i < n; i++) vec_st(vec_ctf(vec_cts(vec_ld(0, rows[i]), 0), 0), 0, rows[i]);
#else
        for (uint32_t i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) rows[i][k] = (float)(int32_t)rows[i][k];
#endif
    }
    // Converts rows of 4 values (truncated, clamped to 0..255) to 4 bytes each. Both arrays hold 64
    // rows and are 16-byte aligned (the vector code converts 4 rows at a time).
    void rows_to_bytes(const float (*rows)[4], uint8_t (*out)[4], uint32_t n) {
#if N64GFX_VMX
        for (uint32_t i = 0; i < n; i += 4) {
            vui a = vec_ctu(vec_ld(0, rows[i]), 0), b = vec_ctu(vec_ld(0, rows[i + 1]), 0);
            vui c = vec_ctu(vec_ld(0, rows[i + 2]), 0), d = vec_ctu(vec_ld(0, rows[i + 3]), 0);
            vec_st(vec_packsu(vec_packsu(a, b), vec_packsu(c, d)), 0, &out[i][0]);
        }
#else
        for (uint32_t i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) {
                float v = rows[i][k];
                out[i][k] = v > 0 ? (v < 255.0f ? (uint8_t)v : 255) : 0;
            }
#endif
    }
}

void rgba5551(uint16_t c, float out[4]) {
    out[0] = ((c >> 11) & 31) / 31.0f;
    out[1] = ((c >> 6) & 31) / 31.0f;
    out[2] = ((c >> 1) & 31) / 31.0f;
    out[3] = (c & 1) ? 1.0f : 0.0f;
}

void decode_combiner(uint32_t hi, uint32_t lo, CombinerCycle out[2]) {
    out[0].a = bits(hi, 20, 4); out[0].c = bits(hi, 15, 5);
    out[0].aa = bits(hi, 12, 3); out[0].ac = bits(hi, 9, 3);
    out[1].a = bits(hi, 5, 4); out[1].c = bits(hi, 0, 5);
    out[0].b = bits(lo, 28, 4); out[1].b = bits(lo, 24, 4);
    out[1].aa = bits(lo, 21, 3); out[1].ac = bits(lo, 18, 3);
    out[0].d = bits(lo, 15, 3); out[0].ab = bits(lo, 12, 3); out[0].ad = bits(lo, 9, 3);
    out[1].d = bits(lo, 6, 3); out[1].ab = bits(lo, 3, 3); out[1].ad = bits(lo, 0, 3);
}

// =================================================================================== textures
namespace {
    // Reads TMEM the way the RDP addresses it: odd rows have their 32-bit words swapped.
    inline uint8_t tmem8(const uint8_t* tmem, uint32_t addr, uint32_t row) {
        return tmem[(addr ^ ((row & 1) ? 4 : 0)) & 0xFFF];
    }
    inline uint16_t tmem16(const uint8_t* tmem, uint32_t addr, uint32_t row) {
        uint32_t a = (addr ^ ((row & 1) ? 4 : 0)) & 0xFFE;
        return (uint16_t)(tmem[a] << 8 | tmem[a + 1]);
    }
    void color16(uint16_t c, uint8_t* o) {
        o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
        o[1] = (uint8_t)(((c >> 6) & 31) * 255 / 31);
        o[2] = (uint8_t)(((c >> 1) & 31) * 255 / 31);
        o[3] = (c & 1) ? 255 : 0;
    }
    void tlut_color(const uint8_t* tmem, uint32_t index, uint32_t tlut_type, uint8_t* o) {
        // LOADTLUT stores entry i at 0x800 + i * 8 (n64gfx's layout, see load_tlut).
        uint32_t a = (0x800 + index * 8) & 0xFFE;
        uint16_t c = (uint16_t)(tmem[a] << 8 | tmem[a + 1]);
        if (tlut_type == 3) {  // IA16
            o[0] = o[1] = o[2] = (uint8_t)(c >> 8);
            o[3] = (uint8_t)c;
        } else {
            color16(c, o);
        }
    }
}

void tile_size(const Tile& tile, uint32_t* wp, uint32_t* hp) {
    uint32_t w = tile.masks ? (1u << tile.masks) : ((tile.lrs - tile.uls) >> 2) + 1;
    uint32_t h = tile.maskt ? (1u << tile.maskt) : ((tile.lrt - tile.ult) >> 2) + 1;
    if (tile.lrs < tile.uls) w = tile.masks ? w : 1;
    if (tile.lrt < tile.ult) h = tile.maskt ? h : 1;
    *wp = std::min<uint32_t>(std::max<uint32_t>(w, 1), 1024);
    *hp = std::min<uint32_t>(std::max<uint32_t>(h, 1), 1024);
}

namespace {
    // With TLUT enabled, the RDP looks every 4- and 8-bit texel up in the palette, whatever the tile's
    // format says (Conker's models use "RGBA" 4/8-bit tiles with a palette).
    inline bool uses_palette(const Tile& tile, uint32_t tlut_type) { return tile.siz <= 1 && (tile.fmt == 2 || tlut_type != 0); }
    // The TMEM bytes the image covers (from, length); 32-bit tiles also cover the same range + 0x800.
    inline void tile_range(const Tile& tile, uint32_t h, uint32_t* start, uint32_t* len) {
        *start = tile.tmem * 8;
        *len = std::min<uint32_t>(tile.line * 8 * h, tile.siz == 3 ? 2048 : 4096);
    }
}

// The content hash: four 32-bit lanes, each word of a 16-byte block (big-endian) mixed into its lane
// as XXH32 does, then the lanes folded into 64 bits. Cheap on every processor here: the SPU does a
// block in a handful of vector instructions (it has no 64-bit multiply, which XXH64 needs).
namespace {
constexpr uint32_t kP1 = 2654435761u, kP2 = 2246822519u, kP3 = 3266489917u, kP4 = 668265263u, kP5 = 374761393u;
inline uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
inline uint32_t load_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
struct ContentHash {
#if defined(__SPU__)
    vec_uint4 v = {kP1 + kP2, kP2, 0, 0u - kP1};
    static vec_uint4 mul(vec_uint4 a, vec_uint4 b) {
        const vec_uint4 hi = (vec_uint4)spu_add(spu_mulh((vec_short8)a, (vec_short8)b), spu_mulh((vec_short8)b, (vec_short8)a));
        return spu_add(hi, spu_mulo((vec_ushort8)a, (vec_ushort8)b));
    }
    void block(vec_uint4 w) { v = mul(spu_rl(spu_add(v, mul(w, spu_splats(kP2))), 13), spu_splats(kP1)); }
#else
    uint32_t v[4] = {kP1 + kP2, kP2, 0, 0u - kP1};
    void block(const uint8_t* p) {
        for (int i = 0; i < 4; i++) v[i] = rotl32(v[i] + load_be32(p + 4 * i) * kP2, 13) * kP1;
    }
#endif
    uint32_t bytes = 0;
    // n bytes, a multiple of 8, at an 8-byte-aligned address; an 8-byte tail is padded with zeros.
    void add(const uint8_t* p, uint32_t n) {
        bytes += n;
#if defined(__SPU__)
        const vec_uint4* q = (const vec_uint4*)((uintptr_t)p & ~(uintptr_t)15);
        const vec_uint4 first_half = spu_maskw(0xC);  // the first 8 bytes
        const uint32_t blocks = n >> 4;
        if (((uintptr_t)p & 15) == 0) {
            for (uint32_t i = 0; i < blocks; i++) block(q[i]);
            if (n & 8) block(spu_and(q[blocks], first_half));
        } else {  // 8 bytes into a quadword: each block is the second half of one and the first of the next
            const vec_uchar16 join = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
            vec_uint4 prev = q[0];
            for (uint32_t i = 0; i < blocks; i++) {
                const vec_uint4 next = q[i + 1];
                block(spu_shuffle(prev, next, join));
                prev = next;
            }
            if (n & 8) block(spu_and(spu_slqwbyte(prev, 8), first_half));
        }
#else
        uint32_t i = 0;
        for (; i + 16 <= n; i += 16) block(p + i);
        if (n & 8) {
            uint8_t last[16] = {};
            for (int k = 0; k < 8; k++) last[k] = p[i + k];
            block(last);
        }
#endif
    }
    uint64_t result() const {
#if defined(__SPU__)
        const uint32_t v0 = spu_extract(v, 0), v1 = spu_extract(v, 1), v2 = spu_extract(v, 2), v3 = spu_extract(v, 3);
#else
        const uint32_t v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];
#endif
        auto avalanche = [](uint32_t h) {
            h ^= h >> 15; h *= kP2; h ^= h >> 13; h *= kP3; h ^= h >> 16;
            return h;
        };
        const uint32_t a = avalanche(rotl32(v0, 1) + rotl32(v1, 7) + rotl32(v2, 12) + rotl32(v3, 18) + bytes);
        const uint32_t b = avalanche((v0 * kP4) ^ rotl32(v1 * kP5, 9) ^ rotl32(v2 * kP1, 17) ^ rotl32(v3 * kP3, 25) ^ (bytes * kP2));
        return (uint64_t)a << 32 | b;
    }
};
}  // namespace

uint64_t tile_hash(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type) {
    uint32_t w, h;
    tile_size(tile, &w, &h);
    // Identify the content: tile parameters, the TMEM bytes the image covers, and the palette.
    alignas(16) uint8_t key[32];
    const uint32_t kw[8] = {tile.fmt, tile.siz, tile.line, tile.tmem, tile.palette, w, h, tlut_type};
    for (int i = 0; i < 8; i++) { key[4 * i] = (uint8_t)(kw[i] >> 24); key[4 * i + 1] = (uint8_t)(kw[i] >> 16); key[4 * i + 2] = (uint8_t)(kw[i] >> 8); key[4 * i + 3] = (uint8_t)kw[i]; }
    ContentHash hsh;
    hsh.add(key, 32);
    uint32_t start, len;
    tile_range(tile, h, &start, &len);
    // The bytes the image covers (TMEM wraps at 4 KB).
    auto hash_range = [&](uint32_t from, uint32_t n) {
        from &= 0xFFF;
        uint32_t first = std::min<uint32_t>(n, 0x1000 - from);
        hsh.add(tmem + from, first);
        if (n > first) hsh.add(tmem, n - first);
    };
    hash_range(start, len);
    if (tile.siz == 3) hash_range(start + 0x800, len);
    if (uses_palette(tile, tlut_type)) {
        // The palette entries this tile can use: 16 for 4-bit texels (its palette number picks them).
        if (tile.siz == 0) hsh.add(tmem + 0x800 + ((tile.palette & 15) << 7), 128);
        else hsh.add(tmem + 0x800, 0x800);
    }
    return hsh.result();
}

void decode_tile(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type, uint8_t* px) {
    uint32_t w, h, start, len;
    tile_size(tile, &w, &h);
    tile_range(tile, h, &start, &len);
    const bool palette = uses_palette(tile, tlut_type);
    const uint32_t line = tile.line * 8;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = start + y * line;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* o = px + ((size_t)y * w + x) * 4;
            switch (tile.siz) {
            case 0: {  // 4-bit
                uint8_t b = tmem8(tmem, row + (x >> 1), y);
                uint8_t n = (x & 1) ? (b & 15) : (b >> 4);
                if (palette) tlut_color(tmem, (tile.palette << 4) | n, tlut_type, o);
                else if (tile.fmt == 3) {  // IA4: 3-bit intensity, 1-bit alpha
                    uint8_t i = (uint8_t)((n >> 1) * 255 / 7);
                    o[0] = o[1] = o[2] = i; o[3] = (n & 1) ? 255 : 0;
                } else {  // I4
                    o[0] = o[1] = o[2] = o[3] = (uint8_t)(n * 17);
                }
                break;
            }
            case 1: {  // 8-bit
                uint8_t b = tmem8(tmem, row + x, y);
                if (palette) tlut_color(tmem, b, tlut_type, o);
                else if (tile.fmt == 3) { o[0] = o[1] = o[2] = (uint8_t)((b >> 4) * 17); o[3] = (uint8_t)((b & 15) * 17); }
                else { o[0] = o[1] = o[2] = o[3] = b; }
                break;
            }
            case 2: {  // 16-bit
                uint16_t c = tmem16(tmem, row + x * 2, y);
                if (tile.fmt == 3) { o[0] = o[1] = o[2] = (uint8_t)(c >> 8); o[3] = (uint8_t)c; }
                else if (tile.fmt == 2) tlut_color(tmem, c >> 8, tlut_type, o);  // unusual
                else color16(c, o);
                break;
            }
            default: {  // 32-bit RGBA: red/green in the low half of TMEM, blue/alpha in the high half
                uint32_t a = (row + x * 2) & 0x7FF;
                uint32_t sw = (y & 1) ? 4 : 0;
                uint32_t lo = (a ^ sw) & 0x7FE;
                o[0] = tmem[lo]; o[1] = tmem[lo + 1];
                o[2] = tmem[0x800 + lo]; o[3] = tmem[0x800 + lo + 1];
                break;
            }
            }
        }
    }
}

#if !defined(__SPU__)
const Texture* TextureCache::get(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type, bool copy_mode) {
    (void)copy_mode;
    return get_hashed(tile_hash(tmem, tile, tlut_type), tmem, tile, tlut_type);
}

const Texture* TextureCache::find(uint64_t hsh) {
    auto found = entries_.find(hsh);
    if (found == entries_.end()) return nullptr;
    found->second->last_used = frame_;
    return &found->second->tex;
}

const Texture* TextureCache::get_hashed(uint64_t hsh, const uint8_t* tmem, const Tile& tile, uint32_t tlut_type) {
    if (const Texture* t = find(hsh)) return t;
    uint32_t w, h;
    tile_size(tile, &w, &h);
    Entry* e = new Entry();
    decoded++;
    e->pixels.resize((size_t)w * h * 4);
    e->last_used = frame_;
    decode_tile(tmem, tile, tlut_type, e->pixels.data());
    Texture& t = e->tex;
    t.rgba = e->pixels.data(); t.width = w; t.height = h; t.hash = hsh;
    t.fmt = (uint8_t)tile.fmt; t.siz = (uint8_t)tile.siz; t.palette = (uint8_t)tile.palette; t.tlut = (uint8_t)tlut_type;
    entries_[hsh] = e;
    return &t;
}

void TextureCache::touch(const Texture* t) {
    // Texture is the first member of Entry.
    reinterpret_cast<Entry*>(const_cast<Texture*>(t))->last_used = frame_;
}

void TextureCache::end_frame() {
    frame_++;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (frame_ - it->second->last_used > 120) {
            if (on_evict) on_evict(it->second->tex);
            delete it->second;
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}
#endif

// ================================================================================ interpreter
Interpreter::Interpreter() {
    for (auto& m : model_) mat_identity(m);
    mat_identity(proj_);
    mat_identity(mvp_);
    std::memset(vtx_, 0, sizeof vtx_);
}

void Interpreter::load_matrix(const Memory& mem, uint32_t addr, Mat out) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int k = i * 4 + j;
            int16_t ip = (int16_t)mem.r16(addr + k * 2);
            uint16_t fp = mem.r16(addr + 32 + k * 2);
            out[i][j] = (float)((int32_t)((uint32_t)(uint16_t)ip << 16 | fp)) / 65536.0f;
        }
    }
}

void Interpreter::update_mvp() {
    if (!mvp_dirty_) return;
    mat_mul(model_[model_depth_ - 1], proj_, mvp_);
    if (interp_) mat_mul(model_mid_[model_depth_ - 1], proj_mid_, mvp_mid_);
    mvp_dirty_ = false;
}

// Frame interpolation: this matrix and its counterpart in the last display list, halfway. Not blended
// when there's none, or when they're far apart (a camera cut, or not the same thing after all).
void Interpreter::blend_matrix(uint64_t key, const Mat m, Mat out) {
    stats.matrices++;
    history_->store(key, m);
    alignas(16) Mat prev;
    if (history_->find(key, prev)) {
        float d = 0, n = 0;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) { const float e = m[i][j] - prev[i][j]; d += e * e; n += m[i][j] * m[i][j]; }
#if !defined(__SPU__)
        if (debug_blend) N64GFX_LOG("[blend] dl %u key %08x:%08x rel %.4f\n", stats.dls, (unsigned)(key >> 32), (unsigned)key, n > 0 ? std::sqrt(d / n) : 0.0f);
#endif
        // A projection that changed (the game's are steady while a shot lasts; a cut changes the field of
        // view): the in-between frame would show a camera that never was.
        // (A zoom changes it a little every frame: only a sudden change counts, see run().)
        if ((key & 0x80000000u) && (uint32_t)(key >> 32) == 0x554E4752u && n > 0) proj_change_ = std::max(proj_change_, d / n);
        if (d <= 0.04f * n) {  // within a fifth of its size
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) out[i][j] = (m[i][j] + prev[i][j]) * 0.5f;
            stats.matrices_blended++;
            return;
        }
    }
    std::memcpy(out, m, sizeof(Mat));
}

void Interpreter::cmd_matrix(const Memory& mem, uint32_t w0, uint32_t w1) {
    uint32_t p = (w0 & 0xFF) ^ 1;  // F3DEX2 stores the push flag inverted
    alignas(16) Mat m;
    load_matrix(mem, seg(w1), m);
    alignas(16) Mat mm;  // the in-between frame's
    if (interp_) {
        // Its key: the object's group (the hooks' ids) and its place in it, or its place among the
        // matrices outside groups (modelview and projection counted apart).
        const uint32_t kind = (p & 4) ? 1 : 0;
        uint64_t key;
        if (group_depth_ > 0) key = (uint64_t)group_ids_[group_depth_ - 1] << 32 | kind << 31 | group_counts_[group_depth_ - 1]++;
        else if (kind) { key = 0x554E4752ull << 32 | 1ull << 31 | ungrouped_[1]++; ungrouped_[0] = 0; }
        // A modelview outside groups: its place after the last projection outside groups (the HUD's
        // chocolate pieces come right after its own projection; counted from the list's start, a matrix
        // more or fewer earlier in the scene paired each piece with its neighbour's last frame).
        else key = 0x554E4752ull << 32 | (uint64_t)ungrouped_[1] << 16 | ungrouped_[0]++;
        blend_matrix(key, m, mm);
    }
    if (p & 4) {  // projection
        if (p & 2) std::memcpy(proj_, m, sizeof m);
        else mat_mul(m, proj_, proj_);
        if (interp_) {
            if (p & 2) std::memcpy(proj_mid_, mm, sizeof mm);
            else mat_mul(mm, proj_mid_, proj_mid_);
        }
    } else {
        if ((p & 1) && model_depth_ < 32) {
            std::memcpy(model_[model_depth_], model_[model_depth_ - 1], sizeof(Mat));
            if (interp_) std::memcpy(model_mid_[model_depth_], model_mid_[model_depth_ - 1], sizeof(Mat));
            model_depth_++;
        }
        if (p & 2) std::memcpy(model_[model_depth_ - 1], m, sizeof m);
        else mat_mul(m, model_[model_depth_ - 1], model_[model_depth_ - 1]);
        if (interp_) {
            if (p & 2) std::memcpy(model_mid_[model_depth_ - 1], mm, sizeof mm);
            else mat_mul(mm, model_mid_[model_depth_ - 1], model_mid_[model_depth_ - 1]);
        }
    }
    mvp_dirty_ = true;
}

// Reads `count` N64 vertices (16 bytes each: s16 x, y, z, flag, s, t; u8 r, g, b, a) into vtx_[first..].
void Interpreter::load_vertices(const Memory& mem, uint32_t addr, uint32_t first, uint32_t count) {
#if defined(__SPU__)
    // A vertex is one quadword: s16 x, y, z, flag, s, t; u8 r, g, b, a. Converted with the vector unit
    // straight into the Vtx's second and third quadwords (s, t, color, - / ox, oy, oz, flag).
    static_assert(sizeof(Vtx) == 64, "Vtx is four quadwords");
    alignas(16) uint8_t raw[64 * 16];
    mem.read(addr, raw, count * 16);
    const vec_uchar16 xyzf = {0, 1, 0x80, 0x80, 2, 3, 0x80, 0x80, 4, 5, 0x80, 0x80, 6, 7, 0x80, 0x80};
    const vec_uchar16 st = {8, 9, 0x80, 0x80, 10, 11, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    // (ox, oy, oz as floats, then the raw flag) and (s, t as floats, the color bytes, 0).
    const vec_uchar16 q2 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 22, 23, 0x80, 0x80};
    const vec_uchar16 q1 = {0, 1, 2, 3, 4, 5, 6, 7, 28, 29, 30, 31, 0x80, 0x80, 0x80, 0x80};
    for (uint32_t i = 0; i < count; i++) {
        const vec_uchar16 q = ((const vec_uchar16*)raw)[i];
        const vec_float4 o = spu_convtf(spu_rlmaska((vec_int4)spu_shuffle(q, q, xyzf), -16), 0);
        const vec_float4 t = spu_convtf(spu_rlmaska((vec_int4)spu_shuffle(q, q, st), -16), 0);
        vec_uchar16* v = (vec_uchar16*)&vtx_[first + i];
        v[1] = spu_shuffle((vec_uchar16)t, q, q1);
        v[2] = spu_shuffle((vec_uchar16)o, q, q2);
    }
    return;
#endif
    alignas(16) float in[64][8];  // x, y, z, flag, s, t, -, -
#if N64GFX_VMX
    // The vector unit converts the 16-bit values to floats without a round trip through memory.
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* p = mem.rdram + ((addr + i * 16) & Memory::kMask);
        vuc lo = vec_ld(0, p), hi = vec_ld(15, p);
        vss h = (vss)vec_perm(lo, hi, vec_lvsl(0, p));
        vec_st(vec_ctf(vec_unpackh(h), 0), 0, in[i]);
        vec_st(vec_ctf(vec_unpackl(h), 0), 16, in[i]);
    }
#else
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = addr + i * 16;
        for (int k = 0; k < 6; k++) in[i][k] = (float)(int16_t)mem.r16(a + k * 2);
    }
#endif
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t a = addr + i * 16;
        Vtx& v = vtx_[first + i];
        v.ox = in[i][0]; v.oy = in[i][1]; v.oz = in[i][2];
        v.s = in[i][4]; v.t = in[i][5];
        v.flag = (int16_t)mem.r16(a + 6);
        const uint32_t c = mem.r32(a + 12);
        v.c[0] = (uint8_t)(c >> 24); v.c[1] = (uint8_t)(c >> 16); v.c[2] = (uint8_t)(c >> 8); v.c[3] = (uint8_t)c;
    }
}

void Interpreter::cmd_vertex(const Memory& mem, uint32_t w0, uint32_t w1) {
    const uint64_t t0 = ticks();
    struct Add { uint64_t& to; uint64_t t0; ~Add() { to += ticks() - t0; } } add{stats.t_vertex, t0};
    uint32_t n = bits(w0, 12, 8);
    uint32_t end = bits(w0, 1, 7);
    if (n == 0 || n > end || end > 64) return;
    uint32_t first = end - n;
    uint32_t addr = seg(w1);
    update_mvp();
    load_vertices(mem, addr, first, n);
    const bool lighting = (geometry_mode_ & G_LIGHTING) != 0, fog = (geometry_mode_ & G_FOG) != 0;
    if (lighting && (geometry_mode_ & G_TEXTURE_GEN)) texgen_vertices(mem, first, n);
    // Lighting and fog compute colors as floats (0..255, truncated at the end), converted in one batch.
    alignas(16) Rows col;
    bool recolor = lighting || fog;
    if (recolor) {
#if defined(__SPU__)
        const vec_uchar16 bytes_to_words = {0x80, 0x80, 0x80, 8, 0x80, 0x80, 0x80, 9, 0x80, 0x80, 0x80, 10, 0x80, 0x80, 0x80, 11};
        for (uint32_t k = 0; k < n; k++) {
            const vec_uchar16 q = ((const vec_uchar16*)&vtx_[first + k])[1];  // s, t, color, -
            *(vec_float4*)col[k] = spu_convtf((vec_uint4)spu_shuffle(q, q, bytes_to_words), 0);
        }
#else
        for (uint32_t k = 0; k < n; k++) {
            const uint8_t* c = vtx_[first + k].c;
            col[k][0] = kU8f[c[0]]; col[k][1] = kU8f[c[1]]; col[k][2] = kU8f[c[2]]; col[k][3] = kU8f[c[3]];
        }
#endif
    }
    if (lighting) light_vertices(mem, first, n, col);
    const float (*m)[4] = mvp_;
    const float ssc = texture_.sc / 32.0f, tsc = texture_.tc / 32.0f;
#if defined(__SPU__)
    if (!fog) {
        // One vertex per vector: clip position = ox * row 0 + oy * row 1 + oz * row 2 + row 3.
        const vec_float4* mr = (const vec_float4*)mvp_;
        const vec_float4 m0 = mr[0], m1 = mr[1], m2 = mr[2], m3 = mr[3];
        const vec_float4 sc = {ssc, tsc, 1.0f, 1.0f};
        const vec_uint4 st_lanes = spu_maskw(0xC);
        for (uint32_t k = 0; k < n; k++) {
            vec_float4* v = (vec_float4*)&vtx_[first + k];
            const vec_float4 o = v[2];
            v[0] = spu_madd(spu_splats(spu_extract(o, 2)), m2,
                            spu_madd(spu_splats(spu_extract(o, 1)), m1, spu_madd(spu_splats(spu_extract(o, 0)), m0, m3)));
            v[1] = spu_sel(v[1], spu_mul(v[1], sc), st_lanes);  // s, t scaled; the color untouched
        }
    } else
#endif
    for (uint32_t k = 0; k < n; k++) {
        Vtx& v = vtx_[first + k];
        float x = v.ox, y = v.oy, z = v.oz;
        v.x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        v.y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        v.z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        v.w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        // S10.5 texture coordinates, scaled by G_TEXTURE.
        v.s = v.s * ssc;
        v.t = v.t * tsc;
        if (fog) {
            float zw = v.w != 0 ? v.z / v.w : 0;
            if (v.w < 0) zw = -zw;
            col[k][3] = clampf(zw * fog_mul_ + fog_off_, 0, 255) + 0.5f;  // rounded
        }
    }
    if (interp_) {
        // The vertices' positions in the in-between frame: halfway from last frame's when this is an
        // object's vertex load that was there last frame too (the same count, nothing far apart).
        mid_obj_used_ = false;
        // Not a shadow's (its group id starts "SH"): the ground under it is clipped anew every frame, so
        // last frame's vertices are another patch of ground (the PC version doesn't blend them either).
        if (group_depth_ > 0 && (group_ids_[group_depth_ - 1] >> 16) != 0x5348u) {
            alignas(16) int16_t cur[64][4], prev[64][4];
            for (uint32_t k = 0; k < n; k++) {
                const uint32_t a = addr + k * 16;
                cur[k][0] = (int16_t)mem.r16(a); cur[k][1] = (int16_t)mem.r16(a + 2); cur[k][2] = (int16_t)mem.r16(a + 4); cur[k][3] = 0;
            }
            const uint64_t key = (uint64_t)group_ids_[group_depth_ - 1] << 32 | 0x40000000u | group_vtx_[group_depth_ - 1]++;
            stats.vertex_loads++;
            history_->store_vertices(key, cur, n);
            if (history_->find_vertices(key, prev, n)) {
                int far = 0;
                for (uint32_t k = 0; k < n; k++)
                    for (int c = 0; c < 3; c++) { const int d = cur[k][c] - prev[k][c]; if (d > 256 || d < -256) far = 1; }
#if !defined(__SPU__)
                if (debug_blend) {
                    int dmax = 0;
                    for (uint32_t k = 0; k < n; k++) for (int c = 0; c < 3; c++) dmax = std::max(dmax, std::abs(cur[k][c] - prev[k][c]));
                    N64GFX_LOG("[blend] dl %u vertices %08x:%08x n %u first %u max move %d%s\n", stats.dls, (unsigned)(key >> 32), (unsigned)key, n, first, dmax, far ? " (far)" : "");
                }
#endif
                if (!far) {
                    bool moved = false;
                    for (uint32_t k = 0; k < n; k++)
                        for (int c = 0; c < 3; c++) {
                            mid_obj_[k][c] = ((float)cur[k][c] + (float)prev[k][c]) * 0.5f;
                            moved |= cur[k][c] != prev[k][c];
                        }
                    mid_obj_used_ = moved;
                    stats.vertex_loads_blended++;
                }
            }
        }
        // The in-between frame's clip-space positions.
#if defined(__SPU__)
        const vec_float4* mr = (const vec_float4*)mvp_mid_;
        const vec_float4 m0 = mr[0], m1 = mr[1], m2 = mr[2], m3 = mr[3];
        for (uint32_t k = 0; k < n; k++) {
            vec_float4* v = (vec_float4*)&vtx_[first + k];
            const vec_float4 o = mid_obj_used_ ? *(const vec_float4*)mid_obj_[k] : v[2];
            v[3] = spu_madd(spu_splats(spu_extract(o, 2)), m2,
                            spu_madd(spu_splats(spu_extract(o, 1)), m1, spu_madd(spu_splats(spu_extract(o, 0)), m0, m3)));
        }
#else
        const float (*mm)[4] = mvp_mid_;
        for (uint32_t k = 0; k < n; k++) {
            Vtx& v = vtx_[first + k];
            const float x = mid_obj_used_ ? mid_obj_[k][0] : v.ox, y = mid_obj_used_ ? mid_obj_[k][1] : v.oy, z = mid_obj_used_ ? mid_obj_[k][2] : v.oz;
            v.mx = x * mm[0][0] + y * mm[1][0] + z * mm[2][0] + mm[3][0];
            v.my = x * mm[0][1] + y * mm[1][1] + z * mm[2][1] + mm[3][1];
            v.mz = x * mm[0][2] + y * mm[1][2] + z * mm[2][2] + mm[3][2];
            v.mw = x * mm[0][3] + y * mm[1][3] + z * mm[2][3] + mm[3][3];
        }
#endif
    }
#if defined(__SPU__)
    if (recolor) {
        // Truncated to 0..255 and put straight back into each vertex's color bytes.
        const vec_uchar16 put = {0, 1, 2, 3, 4, 5, 6, 7, 19, 23, 27, 31, 12, 13, 14, 15};
        const vec_uint4 max = spu_splats(255u);
        for (uint32_t k = 0; k < n; k++) {
            vec_uint4 u = spu_convtu(*(const vec_float4*)col[k], 0);
            u = spu_sel(u, max, spu_cmpgt(u, max));
            vec_uchar16* q = (vec_uchar16*)&vtx_[first + k] + 1;
            *q = spu_shuffle(*q, (vec_uchar16)u, put);
        }
        recolor = false;
    }
#endif
    if (recolor) {
        alignas(16) uint8_t bytes[64][4];
        rows_to_bytes(col, bytes, n);
        for (uint32_t k = 0; k < n; k++) std::memcpy(vtx_[first + k].c, bytes[k], 4);
    }
    stats.vertices += n;
}

// Conker's texture generation (environment mapping), as in CBFD-Recompiled's RT64 patch.
void Interpreter::texgen_vertices(const Memory& mem, uint32_t first, uint32_t count) {
    const float (*mv)[4] = model_[model_depth_ - 1];
    float la[2][3];
    for (int k = 0; k < 2; k++) {
        const float* d = cbfd_.lookat[k];
        float x = mv[0][0] * d[0] + mv[0][1] * d[1] + mv[0][2] * d[2];
        float y = mv[1][0] * d[0] + mv[1][1] * d[1] + mv[1][2] * d[2];
        float z = mv[2][0] * d[0] + mv[2][1] * d[1] + mv[2][2] * d[2];
        float len = n_sqrt(x * x + y * y + z * z);
        if (len > 0) { x /= len; y /= len; z /= len; }
        la[k][0] = x; la[k][1] = y; la[k][2] = z;
    }
    alignas(16) Rows tg;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t i = first + k;
        const Vtx& v = vtx_[i];
        float n[3];
        if (cbfd_.advanced) {
            uint32_t na = cbfd_.normal_base + (i << 1);
            n[0] = kS8n[mem.r8(na)];
            n[1] = kS8n[mem.r8(na + 1)];
            n[2] = kS8n[v.flag & 0xFF];
        } else {
            n[0] = kS8n[v.c[0]]; n[1] = kS8n[v.c[1]]; n[2] = kS8n[v.c[2]];
        }
        float u = clampf(n[0] * la[0][0] + n[1] * la[0][1] + n[2] * la[0][2], -1, 1);
        float w = clampf(n[0] * la[1][0] + n[1] * la[1][1] + n[2] * la[1][2], -1, 1);
        if (geometry_mode_ & G_TEXTURE_GEN_LINEAR) {
            u = n_acos(-u) * 325.94932f;
            w = n_acos(-w) * 325.94932f;
        } else {
            u = (u + 1.0f) * 512.0f;
            w = (w + 1.0f) * 512.0f;
        }
        tg[k][0] = clampf(u * 32.0f, -32768.0f, 32767.0f);
        tg[k][1] = clampf(w * 32.0f, -32768.0f, 32767.0f);
        tg[k][2] = tg[k][3] = 0;
    }
    trunc_rows(tg, count);  // as the RSP's 16-bit result
    for (uint32_t k = 0; k < count; k++) { vtx_[first + k].s = tg[k][0]; vtx_[first + k].t = tg[k][1]; }
}

// Conker's vertex lighting (point lights, separate normals), as in CBFD-Recompiled's RT64 patch.
// Writes the lit colors (0..255, to be truncated) into col's red, green and blue.
void Interpreter::light_vertices(const Memory& mem, uint32_t first, uint32_t count, Rows& col) {
    int num = (int)std::min<uint32_t>(cbfd_.light_count, 11);
    const float* mod = cbfd_.coord_mod;
    const bool point = (geometry_mode_ & G_POINT_LIGHTING) != 0;
    const float (*mv)[4] = model_[model_depth_ - 1];
    float dirs[12][3];
    for (int l = 0; l < num; l++) {
        const float* d = cbfd_.lights[l].dir;
        float x = mv[0][0] * d[0] + mv[0][1] * d[1] + mv[0][2] * d[2];
        float y = mv[1][0] * d[0] + mv[1][1] * d[1] + mv[1][2] * d[2];
        float z = mv[2][0] * d[0] + mv[2][1] * d[1] + mv[2][2] * d[2];
        float len = n_sqrt(x * x + y * y + z * z);
        if (len > 0) { x /= len; y /= len; z /= len; } else { x = y = z = 0; }
        dirs[l][0] = x; dirs[l][1] = y; dirs[l][2] = z;
    }
    const float (*m)[4] = mvp_;
    const Light& amb = cbfd_.lights[num];
#if defined(__SPU__)
    {
        // Four vertices at a time, one per lane.
        const vec_float4 one = spu_splats(1.0f), zero = spu_splats(0.0f);
        auto vmin1 = [&](vec_float4 a) { return spu_sel(a, one, spu_cmpgt(a, one)); };
        auto recip = [&](vec_float4 a) {  // 1 / a to full precision (one Newton step)
            const vec_float4 e = spu_re(a);
            return spu_madd(spu_nmsub(a, e, one), e, e);
        };
        const vec_float4 m00 = spu_splats(m[0][0]), m10 = spu_splats(m[1][0]), m20 = spu_splats(m[2][0]), m30 = spu_splats(m[3][0]);
        const vec_float4 m01 = spu_splats(m[0][1]), m11 = spu_splats(m[1][1]), m21 = spu_splats(m[2][1]), m31 = spu_splats(m[3][1]);
        const vec_float4 m02 = spu_splats(m[0][2]), m12 = spu_splats(m[1][2]), m22 = spu_splats(m[2][2]), m32 = spu_splats(m[3][2]);
        const bool adv = cbfd_.advanced, adv_point = adv && point;
        const vec_uchar16 lo2 = {0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23};
        const vec_uchar16 hi2 = {8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31};
        const vec_uchar16 lo4 = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
        const vec_uchar16 hi4 = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31};
        for (uint32_t k = 0; k < count; k += 4) {
            // Transpose the four vertices' (ox, oy, oz, flag) quadwords.
            vec_float4 q[4];
            for (int l = 0; l < 4; l++) q[l] = ((const vec_float4*)&vtx_[(first + k + l) & 63])[2];
            const vec_float4 t0 = spu_shuffle(q[0], q[1], lo2), t1 = spu_shuffle(q[2], q[3], lo2);
            const vec_float4 t2 = spu_shuffle(q[0], q[1], hi2), t3 = spu_shuffle(q[2], q[3], hi2);
            const vec_float4 ox = spu_shuffle(t0, t1, lo4), oy = spu_shuffle(t0, t1, hi4), oz = spu_shuffle(t2, t3, lo4);
            const vec_int4 flag = (vec_int4)spu_shuffle(t2, t3, hi4);  // the flag in each word's top half
            const vec_float4 cx = spu_madd(oz, m20, spu_madd(oy, m10, spu_madd(ox, m00, m30)));
            const vec_float4 cy = spu_madd(oz, m21, spu_madd(oy, m11, spu_madd(ox, m01, m31)));
            const vec_float4 cz = spu_madd(oz, m22, spu_madd(oy, m12, spu_madd(ox, m02, m32)));
            const vec_float4 px = spu_mul(spu_add(cx, spu_splats(mod[8])), spu_splats(mod[12]));
            const vec_float4 py = spu_mul(spu_add(cy, spu_splats(mod[9])), spu_splats(mod[13]));
            const vec_float4 pz = spu_mul(spu_add(cz, spu_splats(mod[10])), spu_splats(mod[14]));
            vec_float4 r = spu_splats(amb.r), g = spu_splats(amb.g), b = spu_splats(amb.b);
            vec_float4 n0 = zero, n1 = zero, n2 = zero;
            int l = num - 1;
            if (adv) {
                float a0[4], a1[4], a2[4];
                for (int j = 0; j < 4; j++) {
                    const uint32_t i = first + k + j;
                    const uint32_t na = cbfd_.normal_base + (i << 1);
                    a0[j] = kS8n[mem.r8(na)];
                    a1[j] = kS8n[mem.r8(na + 1)];
                    a2[j] = kS8n[(spu_extract(flag, j) >> 16) & 0xFF];
                }
                n0 = (vec_float4){a0[0], a0[1], a0[2], a0[3]};
                n1 = (vec_float4){a1[0], a1[1], a1[2], a1[3]};
                n2 = (vec_float4){a2[0], a2[1], a2[2], a2[3]};
                if (l >= 0) {
                    vec_float4 in = vmin1(spu_madd(n2, spu_splats(dirs[l][2]), spu_madd(n1, spu_splats(dirs[l][1]), spu_mul(n0, spu_splats(dirs[l][0])))));
                    in = spu_sel(zero, in, spu_cmpgt(in, zero));
                    const Light& lt = cbfd_.lights[l];
                    r = spu_madd(spu_splats(lt.r), in, r); g = spu_madd(spu_splats(lt.g), in, g); b = spu_madd(spu_splats(lt.b), in, b);
                }
            }
            // The point lights (two loops, so the inner one has no branch on the mode).
            auto point_lights = [&](auto with_normals) {
                for (int pl = num - 2; pl >= 0; pl--) {
                    const Light& lt = cbfd_.lights[pl];
                    const vec_float4 vx = spu_sub(px, spu_splats(lt.pos[0])), vy = spu_sub(py, spu_splats(lt.pos[1])), vz = spu_sub(pz, spu_splats(lt.pos[2]));
                    const vec_float4 len = spu_mul(spu_madd(vz, vz, spu_madd(vy, vy, spu_mul(vx, vx))), spu_splats(2.0f / 65536.0f));
                    const vec_uint4 pos_len = spu_cmpgt(len, zero);
                    vec_float4 in = spu_sel(one, vmin1(spu_mul(spu_splats(lt.ca), recip(spu_sel(one, len, pos_len)))), pos_len);
                    if (decltype(with_normals)::value)
                        in = spu_mul(in, vmin1(spu_madd(n2, spu_splats(dirs[pl][2]), spu_madd(n1, spu_splats(dirs[pl][1]), spu_mul(n0, spu_splats(dirs[pl][0]))))));
                    in = spu_sel(zero, in, spu_cmpgt(in, zero));
                    r = spu_madd(spu_splats(lt.r), in, r); g = spu_madd(spu_splats(lt.g), in, g); b = spu_madd(spu_splats(lt.b), in, b);
                }
            };
            if (adv_point) point_lights(std::true_type{});
            else point_lights(std::false_type{});
            r = vmin1(r); g = vmin1(g); b = vmin1(b);
            for (uint32_t j = 0; j < 4 && k + j < count; j++) {
                if (spu_extract(flag, j) < 0) continue;  // as the scalar version: flag < 0 keeps its color
                col[k + j][0] = spu_extract(r, j) * col[k + j][0];
                col[k + j][1] = spu_extract(g, j) * col[k + j][1];
                col[k + j][2] = spu_extract(b, j) * col[k + j][2];
            }
        }
        return;
    }
#endif
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t i = first + k;
        const Vtx& v = vtx_[i];
        if (v.flag < 0) continue;
        float ox = v.ox, oy = v.oy, oz = v.oz;
        float cx = ox * m[0][0] + oy * m[1][0] + oz * m[2][0] + m[3][0];
        float cy = ox * m[0][1] + oy * m[1][1] + oz * m[2][1] + m[3][1];
        float cz = ox * m[0][2] + oy * m[1][2] + oz * m[2][2] + m[3][2];
        float px = (cx + mod[8]) * mod[12];
        float py = (cy + mod[9]) * mod[13];
        float pz = (cz + mod[10]) * mod[14];
        float r = amb.r, g = amb.g, b = amb.b;
        float n[3] = {0, 0, 0};
        int l = num - 1;
        if (cbfd_.advanced) {
            uint32_t na = cbfd_.normal_base + (i << 1);
            n[0] = kS8n[mem.r8(na)];
            n[1] = kS8n[mem.r8(na + 1)];
            n[2] = kS8n[v.flag & 0xFF];
            if (l >= 0) {
                float in = std::min(1.0f, n[0] * dirs[l][0] + n[1] * dirs[l][1] + n[2] * dirs[l][2]);
                if (in > 0) { r += cbfd_.lights[l].r * in; g += cbfd_.lights[l].g * in; b += cbfd_.lights[l].b * in; }
            }
        }
        for (l = num - 2; l >= 0; l--) {
            const Light& lt = cbfd_.lights[l];
            float vx = px - lt.pos[0], vy = py - lt.pos[1], vz = pz - lt.pos[2];
            float len = (vx * vx + vy * vy + vz * vz) * (2.0f / 65536.0f);
            float in = len > 0 ? std::min(1.0f, lt.ca / len) : 1.0f;
            if (cbfd_.advanced && point)
                in *= std::min(1.0f, n[0] * dirs[l][0] + n[1] * dirs[l][1] + n[2] * dirs[l][2]);
            if (in > 0) { r += lt.r * in; g += lt.g * in; b += lt.b * in; }
        }
        // As the RSP stores it: 8-bit color (light times vertex color, truncated in cmd_vertex).
        col[k][0] = std::min(1.0f, r) * col[k][0];
        col[k][1] = std::min(1.0f, g) * col[k][1];
        col[k][2] = std::min(1.0f, b) * col[k][2];
    }
}

void Interpreter::cmd_movemem(const Memory& mem, uint32_t w0, uint32_t w1) {
    uint32_t index = w0 & 0xFF;
    uint32_t addr = seg(w1);
    switch (index) {
    case 8: {  // G_MV_VIEWPORT
        int16_t s[4], t[4];
        for (int i = 0; i < 4; i++) { s[i] = (int16_t)mem.r16(addr + i * 2); t[i] = (int16_t)mem.r16(addr + 8 + i * 2); }
        vp_scale_[0] = s[0] / 4.0f; vp_scale_[1] = s[1] / 4.0f; vp_scale_[2] = (float)s[2];
        vp_trans_[0] = t[0] / 4.0f; vp_trans_[1] = t[1] / 4.0f; vp_trans_[2] = (float)t[2];
        break;
    }
    case 10: {  // G_MV_LIGHT: Conker's lights are 48 bytes; the first two slots are the look-at vectors
        uint32_t offset = bits(w0, 5, 14);
        uint32_t n = offset / 48;
        if (n < 2) {
            float d[3] = {(float)(int8_t)mem.r8(addr + 8), (float)(int8_t)mem.r8(addr + 9), (float)(int8_t)mem.r8(addr + 10)};
            float len = n_sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int k = 0; k < 3; k++) cbfd_.lookat[n][k] = len > 0 ? d[k] / len : 0;
        } else if (n - 2 < 12) {
            Light& l = cbfd_.lights[n - 2];
            l.r = mem.r8(addr) / 255.0f; l.g = mem.r8(addr + 1) / 255.0f; l.b = mem.r8(addr + 2) / 255.0f;
            float d[3] = {(float)(int8_t)mem.r8(addr + 8), (float)(int8_t)mem.r8(addr + 9), (float)(int8_t)mem.r8(addr + 10)};
            float len = n_sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int k = 0; k < 3; k++) l.dir[k] = len > 0 ? d[k] / len : 0;
            l.pos[0] = (int16_t)mem.r16(addr + 32);
            l.pos[1] = (int16_t)mem.r16(addr + 34);
            l.pos[2] = (int16_t)mem.r16(addr + 36);
            l.ca = mem.r8(addr + 12) / 16.0f;
        }
        break;
    }
    case 14:  // Conker: per-vertex normals for the advanced lighting
        cbfd_.normal_base = addr;
        break;
    default:
        break;
    }
}

void Interpreter::cmd_moveword(const Memory& mem, uint32_t w0, uint32_t w1) {
    (void)mem;
    uint32_t type = bits(w0, 16, 8);
    uint32_t offset = bits(w0, 0, 16);
    switch (type) {
    case 0x02: cbfd_.light_count = w1 / 48; break;  // G_MW_NUMLIGHT (Conker: bytes)
    case 0x06: segments_[(offset >> 2) & 15] = w1 & 0xFFFFFF; break;  // G_MW_SEGMENT
    case 0x08:  // G_MW_FOG
        fog_mul_ = (float)(int16_t)(w1 >> 16);
        fog_off_ = (float)(int16_t)(w1 & 0xFFFF);
        break;
    case 0x10: {  // Conker: coordinate modifier for the point lights
        if (w0 & 8) break;
        uint32_t idx = (w0 >> 1) & 3, pos = w0 & 0x30;
        float* m = cbfd_.coord_mod;
        if (pos == 0x00) { m[0 + idx] = (float)(int16_t)(w1 >> 16); m[1 + idx] = (float)(int16_t)(w1 & 0xFFFF); }
        else if (pos == 0x10) {
            m[4 + idx] = (float)(w1 >> 16) / 65536.0f; m[5 + idx] = (float)(w1 & 0xFFFF) / 65536.0f;
            m[12 + idx] = m[0 + idx] + m[4 + idx]; m[13 + idx] = m[1 + idx] + m[5 + idx];
        } else if (pos == 0x20) { m[8 + idx] = (float)(int16_t)(w1 >> 16); m[9 + idx] = (float)(int16_t)(w1 & 0xFFFF); }
        break;
    }
    default: break;
    }
}

void Interpreter::viewport_to_state() {
    st_.vp_scale[0] = vp_scale_[0]; st_.vp_scale[1] = vp_scale_[1];
    st_.vp_trans[0] = vp_trans_[0]; st_.vp_trans[1] = vp_trans_[1];
    // Depth: the viewport's z range (0..0x3FF in the usual setup) as 0..1.
    st_.vp_scale[2] = vp_scale_[2] / 1023.0f;
    st_.vp_trans[2] = vp_trans_[2] / 1023.0f;
}

void Interpreter::prepare_textures(uint32_t base, bool for_rect) {
    st_.tex[0] = st_.tex[1] = nullptr;
    const CycleType cyc = st_.cycle();
    if (cyc == CYC_FILL) return;
    if (!for_rect && !texture_.on) return;
    CombinerCycle cc[2];
    decode_combiner(st_.combine_hi, st_.combine_lo, cc);
    bool use[2] = {cyc == CYC_COPY, false};
    int ncyc = cyc == CYC_2 ? 2 : 1;
    for (int c = 0; c < ncyc && cyc != CYC_COPY; c++) {
        auto uses = [&](int t) {
            int col = 1 + t;  // TEXEL0 = 1, TEXEL1 = 2 in A, B, C, D; 8 + t as C's alpha inputs
            return cc[c].a == col || cc[c].b == col || cc[c].c == col || cc[c].d == col ||
                   cc[c].c == 8 + t || cc[c].aa == col || cc[c].ab == col || cc[c].ac == col || cc[c].ad == col;
        };
        use[0] |= uses(0);
        use[1] |= uses(1);
    }
    uint32_t tlut = (st_.om_h >> 14) & 3;
    st_.mip_levels = 0;
    // Texture LOD: the tiles from the draw's tile on are a mipmap chain (TEXEL0 the level the pixel's
    // texel density picks, TEXEL1 the next, blended by the LOD fraction). In detail mode the draw's
    // tile is a detail texture for close up, and the chain starts at the next tile.
    const bool lod = !for_rect && cyc == CYC_2 && (st_.om_h & (1u << 16)) && texture_.level > 0;
    if (lod && ((st_.om_h >> 17) & 3) == 2) base = (base + 1) & 7;
    auto shift = [](uint32_t s) { return s == 0 ? 1.0f : (s <= 10 ? 1.0f / (float)(1 << s) : (float)(1 << (16 - s))); };
    for (int t = 0; t < 2; t++) {
        if (!use[t]) continue;
        const uint32_t slot = (base + t) & 7;
        const Tile& tile = tiles_[slot];
        TileLookup& lk = lookup_[slot];
        stats.texture_lookups++;
        if (frame_textures && fb_load_.gen == tmem_gen_ && tile.tmem == fb_load_.tmem && tile.siz == fb_load_.siz &&
            (!fb_load_.block || tile.line * 8 == fb_load_.row_bytes)) {
            // TMEM holds a piece of an image the game drew: the backend reads it from its copy.
            Texture& ft = fb_tex_[t];
            ft = Texture{};
            ft.fb_addr = fb_load_.addr; ft.fb_x = fb_load_.x; ft.fb_y = fb_load_.y;
            ft.width = fb_load_.w; ft.height = fb_load_.h;
            ft.fmt = (uint8_t)tile.fmt; ft.siz = (uint8_t)tile.siz;
            st_.tex[t] = &ft;
            stats.fb_textures++;
        } else if (frame_textures && fb_load_.gen == tmem_gen_ && tile.tmem == fb_load_.tmem && fb_logged_ < 60) {
            fb_logged_++;
            N64GFX_LOG("[gfx] a tile reads TMEM loaded from drawn image 0x%06X but doesn't match it (tile fmt %u siz %u line %u; "
                        "image siz %u, row %u bytes): read from game memory\n", fb_load_.addr, tile.fmt, tile.siz, tile.line,
                        fb_load_.siz, fb_load_.row_bytes);
            st_.tex[t] = texsrc_->get(tmem_, tile, tlut, cyc == CYC_COPY);
        } else if (lk.tex && lk.gen == tmem_gen_ && lk.tlut == tlut && std::memcmp(&lk.tile, &tile, sizeof tile) == 0) {
            st_.tex[t] = lk.tex;
            texsrc_->touch(lk.tex);
        } else {
            stats.texture_hashes++;
            st_.tex[t] = texsrc_->get(tmem_, tile, tlut, cyc == CYC_COPY);
            lk.tile = tile; lk.tlut = tlut; lk.gen = tmem_gen_; lk.tex = st_.tex[t];
        }
        TexMap& m = st_.tmap[t];
        m.shift_s = shift(tile.shifts); m.shift_t = shift(tile.shiftt);
        m.offset_s = tile.uls / 4.0f; m.offset_t = tile.ult / 4.0f;
        m.clamp_s = (tile.cms & 2) || tile.masks == 0;
        m.clamp_t = (tile.cmt & 2) || tile.maskt == 0;
        m.mirror_s = (tile.cms & 1) && tile.masks;
        m.mirror_t = (tile.cmt & 1) && tile.maskt;
        m.clamp_max_s = (float)((int)(tile.lrs - tile.uls) >> 2);
        m.clamp_max_t = (float)((int)(tile.lrt - tile.ult) >> 2);
    }
    const Texture* prev = st_.tex[0];
    if (lod && prev && !prev->fb_addr && (prev->width & (prev->width - 1)) == 0 && (prev->height & (prev->height - 1)) == 0) {
        for (uint32_t k = 1; k <= texture_.level && k <= DrawState::kMaxMips; k++) {
            const uint32_t slot = (base + k) & 7;
            const Tile& tile = tiles_[slot];
            uint32_t w, h;
            tile_size(tile, &w, &h);
            if (w != (prev->width > 1 ? prev->width / 2 : 1) || h != (prev->height > 1 ? prev->height / 2 : 1)) break;
            if (prev->width == 1 && prev->height == 1) break;
            TileLookup& lk = lookup_[slot];
            const Texture* t;
            if (lk.tex && lk.gen == tmem_gen_ && lk.tlut == tlut && std::memcmp(&lk.tile, &tile, sizeof tile) == 0) {
                t = lk.tex;
                texsrc_->touch(t);
            } else {
                stats.texture_hashes++;
                t = texsrc_->get(tmem_, tile, tlut, false);
                lk.tile = tile; lk.tlut = tlut; lk.gen = tmem_gen_; lk.tex = t;
            }
            if (!t || t->width != w || t->height != h) break;
            st_.mip[st_.mip_levels++] = t;
            prev = t;
        }
    }
}

void Interpreter::note_color_image_drawn() {
    if (!st_.cimg_addr) return;
    FbImage* f = nullptr;
    for (uint32_t i = 0; i < fb_count_; i++) if (fb_images_[i].addr == st_.cimg_addr) { f = &fb_images_[i]; break; }
    if (!f) {  // a new one (the oldest gives way when the table is full)
        if (fb_count_ < kFbImages) f = &fb_images_[fb_count_++];
        else { f = &fb_images_[fb_next_]; fb_next_ = (fb_next_ + 1) % kFbImages; }
        *f = FbImage{};
        f->addr = st_.cimg_addr;
    }
    if (f->width != st_.cimg_width || f->siz != st_.cimg_siz) f->rows = 0;
    f->width = st_.cimg_width;
    f->siz = st_.cimg_siz;
    const float lry = st_.sc_lry > 0 ? st_.sc_lry : 0;
    const uint32_t rows = std::min<uint32_t>((uint32_t)std::ceil(lry), 1024);
    if (rows > f->rows) f->rows = rows;
}

void Interpreter::log_fb_load(const char* what, bool used, uint32_t src, uint32_t bytes, uint32_t img, const Tile& t) {
    if (fb_logged_ >= 60) return;
    fb_logged_++;
    const FbImage* f = nullptr;
    for (uint32_t i = 0; i < fb_count_; i++) if (fb_images_[i].addr == img) f = &fb_images_[i];
    N64GFX_LOG("[gfx] %s from drawn image 0x%06X (width %u siz %u): src 0x%06X, %u bytes, image width %u siz %u, tile line %u tmem %u; "
               "drawing to 0x%06X; %s\n", what, img, f ? f->width : 0, f ? f->siz : 0, src, bytes, timg_.width, timg_.siz, t.line, t.tmem,
               st_.cimg_addr, used ? "used as a frame texture" : "NOT understood (read from game memory)");
    (void)f;
}

const Interpreter::FbImage* Interpreter::find_fb_image(uint32_t addr, uint32_t bytes) const {
    const FbImage* best = nullptr;  // the image starting closest at or below addr
    for (uint32_t i = 0; i < fb_count_; i++) {
        const FbImage& f = fb_images_[i];
        if (f.addr <= addr && (!best || f.addr > best->addr)) best = &f;
    }
    if (!best || best->siz < 1 || best->siz > 3) return nullptr;
    const FbImage& f = *best;
    // As tall as the game has drawn into it (its scissor), at most as tall as the backends' targets.
    const uint32_t target_rows = f.width <= 128 ? 128 : (f.width > 320 ? 480 : 240);
    const uint32_t rows = std::min(f.rows ? f.rows : target_rows, target_rows);
    const uint32_t size = (f.width << (f.siz - 1)) * rows;
    if (addr - f.addr >= size || bytes > size - (addr - f.addr)) return nullptr;
    return best;
}

void Interpreter::flush_tris(Backend& be) {
    if (!batch_n_) return;
    note_color_image_drawn();
    viewport_to_state();
    uint64_t t0 = ticks();
    prepare_textures(texture_.tile, false);
    uint64_t t1 = ticks();
    if (interp_) be.draw_triangles_mid(st_, batch_, batch_mid_, batch_n_);
    else be.draw_triangles(st_, batch_, batch_n_);
    stats.t_texture += t1 - t0;
    stats.t_backend += ticks() - t1;
    batch_n_ = 0;
}

// The guard band triangles are clipped at, in multiples of w: past the widest picture (32:9 is 2.4
// times 4:3), so clipping there changes nothing that's seen.
static constexpr float kGuard = 4.0f;

void Interpreter::triangle(Backend& be, uint32_t a, uint32_t b, uint32_t c) {
    (void)be;
    if (a >= 64 || b >= 64 || c >= 64) return;
    const Vtx* v[3] = {&vtx_[a], &vtx_[b], &vtx_[c]};
    const uint32_t cull = geometry_mode_ & (G_CULL_FRONT | G_CULL_BACK);
    if (cull == (G_CULL_FRONT | G_CULL_BACK)) return;
    if (cull && v[0]->w > 0 && v[1]->w > 0 && v[2]->w > 0) {
        // Signed area on screen (y down), without dividing by w: with every w > 0, the projected area
        // has the sign of -det(x y w). The N64's front faces are counter-clockwise on screen.
        const Vtx &p = *v[0], &q = *v[1], &r = *v[2];
        float area = -(p.x * (q.y * r.w - r.y * q.w) - p.y * (q.x * r.w - r.x * q.w) + p.w * (q.x * r.y - r.x * q.y));
        if (vp_scale_[1] < 0) area = -area;
        if ((cull & G_CULL_BACK) && area > 0) return;
        if ((cull & G_CULL_FRONT) && area < 0) return;
    }
    const bool smooth = (geometry_mode_ & G_SHADING_SMOOTH) != 0;
    // Behind the camera (w > 0) and a guard band (|x|, |y| <= kGuard * w), as the RSP clips: the RSX
    // draws triangles with a vertex behind the camera, or nearly on its plane, as huge garbage polygons.
    // Not the near plane (z > -w): Conker's microcode is F3DEXBG.NoN, "no near clipping", so a model
    // closer than it is drawn, its depth held at the nearest (the RSX clamps depth, reset_state; RT64,
    // the PC version's renderer, does the same). The far plane (z < w) still clips, here, since the
    // RSX's own far clip is off with the clamp.
    {
        const float kEps = 1e-5f;
        int in_w = 0;
        bool guard = true;
        for (int k = 0; k < 3; k++) {
            const float w = v[k]->w, gw = kGuard * w;
            in_w += w > kEps;
            guard = guard && v[k]->x <= gw && v[k]->x >= -gw && v[k]->y <= gw && v[k]->y >= -gw && v[k]->z <= w;
        }
        if (in_w == 0) { stats.clipped_away++; return; }  // all behind the camera
        if (in_w < 3 || !guard) { clip_triangle(be, v, smooth); return; }
    }
    if (batch_n_ + 3 > kBatchMax) flush_tris(be);
    OutVertex* o = &batch_[batch_n_];
    batch_n_ += 3;
#if defined(__SPU__)
    for (int k = 0; k < 3; k++) {  // two quadwords each
        ((vec_float4*)&o[k])[0] = ((const vec_float4*)v[k])[0];
        ((vec_float4*)&o[k])[1] = ((const vec_float4*)v[k])[1];
    }
    if (interp_)
        for (int k = 0; k < 3; k++) *(vec_float4*)batch_mid_[batch_n_ - 3 + k] = ((const vec_float4*)v[k])[3];
#else
    for (int k = 0; k < 3; k++) std::memcpy(&o[k], v[k], sizeof(OutVertex));
    if (interp_)
        for (int k = 0; k < 3; k++) std::memcpy(batch_mid_[batch_n_ - 3 + k], &v[k]->mx, 16);
#endif
    if (!smooth) { std::memcpy(o[1].c, v[0]->c, 4); std::memcpy(o[2].c, v[0]->c, 4); }  // flat: the first vertex's color
    // Depth closer than the near plane held at the nearest per vertex, as F3DEX2 clamps each vertex's
    // screen Z at 0 (then the RDP interpolates between them).
    for (int k = 0; k < 3; k++) if (o[k].z < -o[k].w) o[k].z = -o[k].w;
    stats.triangles++;
}

// A triangle that crosses the camera's plane or the guard band: clipped (Sutherland-Hodgman) against
// w = eps, x, y = +-kGuard * w and the far plane z = w, with every attribute (and the in-between frame's position) interpolated, then drawn as a fan.
void Interpreter::clip_triangle(Backend& be, const Vtx* const v[3], bool smooth) {
    struct CV { float x, y, z, w, s, t, r, g, b, a, mx, my, mz, mw; };
    CV poly[9], tmp[9];
    int n = 3;
    for (int k = 0; k < 3; k++) {
        const Vtx& p = *v[k];
        const uint8_t* c = smooth ? p.c : v[0]->c;
        poly[k] = {p.x, p.y, p.z, p.w, p.s, p.t, (float)c[0], (float)c[1], (float)c[2], (float)c[3],
                   p.mx, p.my, p.mz, p.mw};
    }
    for (int plane = 0; plane < 6; plane++) {  // no near plane (NoN microcode)
        int m = 0;
        auto dist = [plane](const CV& p) {
            switch (plane) {
            case 0: return p.w - 1e-5f;
            case 1: return kGuard * p.w - p.x;
            case 2: return kGuard * p.w + p.x;
            case 3: return kGuard * p.w - p.y;
            case 4: return kGuard * p.w + p.y;
            default: return p.w - p.z;
            }
        };
        for (int k = 0; k < n; k++) {
            const CV& a = poly[k];
            const CV& b = poly[k + 1 < n ? k + 1 : 0];
            float da = dist(a);
            float db = dist(b);
            if (da >= 0) tmp[m++] = a;
            if ((da >= 0) != (db >= 0)) {
                float f = da / (da - db);
                const float* pa = &a.x; const float* pb = &b.x;
                float* pc = &tmp[m].x;
                for (int q = 0; q < 14; q++) pc[q] = pa[q] + (pb[q] - pa[q]) * f;
                m++;
            }
        }
        n = m;
        for (int k = 0; k < n; k++) poly[k] = tmp[k];
        if (n < 3) { stats.clipped_away++; return; }
    }
    // Culled after clipping, as the RSP does: a triangle reaching behind the camera can't be culled
    // before (triangle() culls only when every w > 0), and a wall the camera is behind, crossing its
    // plane, would be drawn over the picture (the professor's dark room after the tutorial).
    const uint32_t cull = geometry_mode_ & (G_CULL_FRONT | G_CULL_BACK);
    if (cull) {
        float twice = 0;  // the clipped polygon's signed area on screen (y up), every w > 0 now
        for (int k = 0; k < n; k++) {
            const CV& a = poly[k];
            const CV& b = poly[k + 1 < n ? k + 1 : 0];
            twice += (a.x / a.w) * (b.y / b.w) - (b.x / b.w) * (a.y / a.w);
        }
        float area = -twice;  // as triangle()'s -det(x y w)
        if (vp_scale_[1] < 0) area = -area;
        if (((cull & G_CULL_BACK) && area > 0) || ((cull & G_CULL_FRONT) && area < 0)) { stats.clipped_away++; return; }
    }
    stats.clipped++;
    for (int k = 1; k + 1 < n; k++) {
        if (batch_n_ + 3 > kBatchMax) flush_tris(be);
        OutVertex* o = &batch_[batch_n_];
        const int idx[3] = {0, k, k + 1};
        for (int j = 0; j < 3; j++) {
            const CV& p = poly[idx[j]];
            o[j].x = p.x; o[j].y = p.y; o[j].z = p.z < -p.w ? -p.w : p.z; o[j].w = p.w;
            o[j].s = p.s; o[j].t = p.t;
            const float col[4] = {p.r, p.g, p.b, p.a};
            for (int q = 0; q < 4; q++) {
                float x = col[q] + 0.5f;
                o[j].c[q] = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
            }
            o[j].pad = 0;
            if (interp_) {
                float* mp = batch_mid_[batch_n_ + j];
                mp[0] = p.mx; mp[1] = p.my; mp[2] = p.mz; mp[3] = p.mw;
            }
        }
        batch_n_ += 3;
        stats.triangles++;
    }
}

// ------------------------------------------------------------------------------------- RDP
void Interpreter::load_block(const Memory& mem, uint32_t w0, uint32_t w1) {
    tmem_gen_++;
    uint32_t tile = bits(w1, 24, 3);
    uint32_t uls = bits(w0, 12, 12), ult = bits(w0, 0, 12);
    uint32_t lrs = bits(w1, 12, 12), dxt = bits(w1, 0, 12);
    Tile& t = tiles_[tile];
    t.uls = uls << 2; t.ult = ult << 2; t.lrs = lrs << 2; t.lrt = ult << 2;  // tile size as the RDP leaves it
    if (lrs < uls) return;
    uint32_t texels = std::min<uint32_t>(lrs - uls + 1, 4096);  // TMEM holds at most 4 KB
    uint32_t siz = timg_.siz;
    uint32_t bytes = siz == 0 ? (texels + 1) / 2 : texels << (siz - 1);
    uint32_t src = timg_.addr + ((ult * timg_.width + uls) << siz >> 1);
    uint32_t dst = t.tmem * 8;
    // A block from a color image the game drew into (a render-to-texture image, such as Conker's
    // shadow): whole rows of it, which the render tile reads with the image's own row length.
    fb_load_.gen = 0;
    if (fb_count_) {
        const auto* fb = find_fb_image(src, bytes);
        if (fb) {
            const uint32_t isiz = fb->siz, row_bytes = fb->width << (isiz - 1);
            const uint32_t off = src - fb->addr;
            if (off % row_bytes == 0 && bytes >= row_bytes) {
                fb_load_ = FbLoad{};
                fb_load_.gen = tmem_gen_;
                fb_load_.tmem = t.tmem;
                fb_load_.addr = fb->addr;
                fb_load_.siz = isiz;
                fb_load_.row_bytes = row_bytes;
                fb_load_.x = 0;
                fb_load_.y = (int32_t)(off / row_bytes);
                fb_load_.w = fb->width;
                fb_load_.h = bytes / row_bytes;
                fb_load_.block = true;
                log_fb_load("LOADBLOCK", true, src, bytes, fb->addr, t);
            } else {
                log_fb_load("LOADBLOCK", false, src, bytes, fb->addr, t);
            }
        }
    }
    // The source bytes, read in one go (a cache line at a time on an SPU).
    const uint32_t words = std::min<uint32_t>((bytes + 7) / 8, 512);
    alignas(16) uint8_t in[4096];
    mem.read(src, in, words * 8);
    if (siz == 3) {
        // 32-bit: red/green to the low half, blue/alpha to the high half.
#if defined(__SPU__)
        // Per 8-byte unit (two texels), the four red/green bytes and the four blue/alpha bytes are each
        // a word (4-byte aligned in TMEM): gathered with a shuffle, stored as words.
        const vec_uchar16 rg[2] = {{0, 1, 4, 5, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
                                   {8, 9, 12, 13, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}};
        const vec_uchar16 ba[2] = {{2, 3, 6, 7, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
                                   {10, 11, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}};
        uint32_t acc = 0;
        for (uint32_t wdx = 0; wdx < words; wdx++, acc += dxt) {
            const uint32_t sw = ((acc >> 11) & 1) ? 4 : 0;
            const vec_uchar16 q = ((const vec_uchar16*)in)[wdx >> 1];
            const uint32_t da = ((dst / 2 + wdx * 4) ^ sw) & 0x7FC;
            *(uint32_t*)(tmem_ + da) = spu_extract((vec_uint4)spu_shuffle(q, q, rg[wdx & 1]), 0);
            *(uint32_t*)(tmem_ + 0x800 + da) = spu_extract((vec_uint4)spu_shuffle(q, q, ba[wdx & 1]), 0);
        }
        return;
#endif
        for (uint32_t wdx = 0; wdx < words; wdx++) {
            uint32_t line = dxt ? (wdx * dxt) >> 11 : 0;
            uint32_t sw = (line & 1) ? 4 : 0;
            for (uint32_t k = 0; k < 2; k++) {  // two texels per 8 source bytes
                const uint8_t* sp = in + wdx * 8 + k * 4;
                uint32_t da = ((dst / 2 + wdx * 4 + k * 2) ^ sw) & 0x7FE;
                tmem_[da] = sp[0]; tmem_[da + 1] = sp[1];
                tmem_[0x800 + da] = sp[2]; tmem_[0x800 + da + 1] = sp[3];
            }
        }
        return;
    }
#if defined(__SPU__)
    // A quadword (two 8-byte units, each swapped on odd lines) at a time into a linear buffer, then
    // copied into TMEM at dst (8-byte aligned), wrapping at 4 KB.
    {
        static const vec_uchar16 kSwap[4] = {
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 8, 9, 10, 11},
            {4, 5, 6, 7, 0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15}, {4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11}};
        alignas(16) uint8_t lin[4096];
        const vec_uchar16* src_q = (const vec_uchar16*)in;
        vec_uchar16* dst_q = (vec_uchar16*)lin;
        const uint32_t quads = (words + 1) / 2;
        uint32_t acc = 0;  // wdx * dxt for the quadword's first unit
        for (uint32_t q = 0; q < quads; q++, acc += 2 * dxt) {
            const uint32_t sel = ((acc >> 11) & 1) << 1 | (((acc + dxt) >> 11) & 1);
            dst_q[q] = spu_shuffle(src_q[q], src_q[q], kSwap[sel]);
        }
        const uint32_t n = words * 8, at = dst & 0xFF8, first = std::min<uint32_t>(n, 0x1000 - at);
        spu_copy(tmem_ + at, lin, first);
        if (n > first) spu_copy(tmem_, lin + first, n - first);
        return;
    }
#endif
    // 8 bytes at a time: dst is 8-byte aligned, so each lands in one TMEM word pair (swapped on odd lines).
    for (uint32_t wdx = 0; wdx < words; wdx++) {
        uint32_t line = dxt ? (wdx * dxt) >> 11 : 0;
        uint8_t* d = tmem_ + ((dst + wdx * 8) & 0xFF8);
        const uint8_t* sp = in + wdx * 8;
        if (line & 1) { std::memcpy(d, sp + 4, 4); std::memcpy(d + 4, sp, 4); }
        else std::memcpy(d, sp, 8);
    }
}

void Interpreter::load_tile(const Memory& mem, uint32_t w0, uint32_t w1) {
    tmem_gen_++;
    uint32_t tile = bits(w1, 24, 3);
    Tile& t = tiles_[tile];
    t.uls = bits(w0, 12, 12); t.ult = bits(w0, 0, 12); t.lrs = bits(w1, 12, 12); t.lrt = bits(w1, 0, 12);
    uint32_t uls = t.uls >> 2, ult = t.ult >> 2, lrs = t.lrs >> 2, lrt = t.lrt >> 2;
    if (lrs < uls || lrt < ult) return;
    uint32_t siz = timg_.siz;
    // Loading from a color image the game drew into: note where in it (see prepare_textures).
    fb_load_.gen = 0;
    if (siz >= 1 && fb_count_) {
        const auto* fb = find_fb_image(timg_.addr, 1);
        if (fb && fb->siz == siz && fb->width == timg_.width) {
            const uint32_t row_bytes = fb->width << (siz - 1);
            const uint32_t off = timg_.addr - fb->addr;
            fb_load_ = FbLoad{};
            fb_load_.gen = tmem_gen_;
            fb_load_.tmem = t.tmem;
            fb_load_.addr = fb->addr;
            fb_load_.siz = siz;
            fb_load_.row_bytes = row_bytes;
            fb_load_.x = (int32_t)(uls + ((off % row_bytes) >> (siz - 1)));
            fb_load_.y = (int32_t)(ult + off / row_bytes);
            fb_load_.w = lrs - uls + 1;
            fb_load_.h = std::min<uint32_t>(lrt - ult + 1, 1024);
            log_fb_load("LOADTILE", true, timg_.addr, 0, fb->addr, t);
        } else if (fb) {
            log_fb_load("LOADTILE", false, timg_.addr, 0, fb->addr, t);
        }
    }
    uint32_t w = lrs - uls + 1;
    uint32_t line_bytes = t.line * 8;
    const uint32_t rows = std::min<uint32_t>(lrt - ult + 1, 1024);
    alignas(16) uint8_t in[4096];
    for (uint32_t y = 0; y < rows; y++) {
        uint32_t src = timg_.addr + ((((ult + y) * timg_.width) + uls) << siz >> 1);
        uint32_t sw = (y & 1) ? 4 : 0;
        if (siz == 3) {
            uint32_t base = t.tmem * 8 / 2 + y * line_bytes / 2;  // 32-bit tiles use half the TMEM per bank
            const uint32_t n = std::min<uint32_t>(w, 1024);
            mem.read(src, in, n * 4);
            for (uint32_t x = 0; x < n; x++) {
                uint32_t da = ((base + x * 2) ^ sw) & 0x7FE;
                const uint8_t* sp = in + x * 4;
                tmem_[da] = sp[0]; tmem_[da + 1] = sp[1];
                tmem_[0x800 + da] = sp[2]; tmem_[0x800 + da + 1] = sp[3];
            }
            continue;
        }
        uint32_t bytes = std::min<uint32_t>(siz == 0 ? (w + 1) / 2 : w << (siz - 1), 4096);
        uint32_t dst = t.tmem * 8 + y * line_bytes;  // a multiple of 8
        mem.read(src, in, bytes);
        uint32_t k = 0;
#if defined(__SPU__)
        {
            // Whole 8-byte units: swap the words of each on odd rows (in place, a quadword at a time),
            // then copy them into TMEM (wrapping at 4 KB); the bytes of a last partial unit below.
            const uint32_t units = bytes & ~7u;
            if (sw) {
                const vec_uchar16 swap = {4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11};
                for (uint32_t q = 0; q < (units + 15) / 16; q++) ((vec_uchar16*)in)[q] = spu_shuffle(((vec_uchar16*)in)[q], ((vec_uchar16*)in)[q], swap);
            }
            const uint32_t at = dst & 0xFF8, first = std::min<uint32_t>(units, 0x1000 - at);
            spu_copy(tmem_ + at, in, first);
            if (units > first) spu_copy(tmem_, in + first, units - first);
            k = units;
            if (sw && k < bytes) {
                // The partial unit was swapped too if it shares a quadword with the last whole one: undo that.
                const vec_uchar16 swap = {4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11};
                vec_uchar16* q = (vec_uchar16*)(in + (k & ~15u));
                if (k & 15) *q = spu_shuffle(*q, *q, swap);
            }
        }
#endif
        for (; k + 8 <= bytes; k += 8) {
            uint8_t* d = tmem_ + ((dst + k) & 0xFF8);
            if (sw) { std::memcpy(d, in + k + 4, 4); std::memcpy(d + 4, in + k, 4); }
            else std::memcpy(d, in + k, 8);
        }
        for (; k < bytes; k++) tmem_[((dst + k) ^ sw) & 0xFFF] = in[k];
    }
}

void Interpreter::load_tlut(const Memory& mem, uint32_t w0, uint32_t w1) {
    tmem_gen_++;
    uint32_t tile = bits(w1, 24, 3);
    Tile& t = tiles_[tile];
    uint32_t uls = bits(w0, 12, 12) >> 2, lrs = bits(w1, 12, 12) >> 2;
    uint32_t count = lrs >= uls ? lrs - uls + 1 : 0;
    uint32_t src = timg_.addr + uls * 2;
    // n64gfx keeps one copy of each entry at 0x800 + index * 8 (the hardware writes four).
    uint32_t first = (t.tmem * 8 - 0x800) / 8;
    const uint32_t n = first < 256 ? std::min<uint32_t>(count, 256 - first) : 0;
    alignas(16) uint8_t in[512 + 16];
    mem.read(src, in, n * 2);
#if defined(__SPU__)
    // Two entries per TMEM quadword (at bytes 0 and 8 of it): each quadword's new bytes are shuffled
    // out of the source and merged in.
    if (n) {
        const vec_uchar16 base = {0, 1, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 2, 3, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
        const vec_uchar16 m0 = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const vec_uchar16 m1 = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
        const vec_uchar16* inq = (const vec_uchar16*)in;
        const uint32_t e_end = first + n;
        for (uint32_t e = first & ~1u; e < e_end; e += 2) {
            // Source bytes of entry e (maybe before `first`: then masked out) start at 2 * (e - first).
            const int32_t o = 2 * ((int32_t)e - (int32_t)first);
            const uint32_t ob = (uint32_t)(o + 32) & 15;
            const int32_t qi = ((o + 32) >> 4) - 2;  // -1 for an entry before `first` (its bytes unused)
            const vec_uchar16 pat = (vec_uchar16)spu_add((vec_ushort8)base, spu_splats((unsigned short)(ob * 0x101)));
            const vec_uchar16 v = spu_shuffle(inq[qi], inq[qi + 1], pat);
            vec_uchar16 m = spu_splats((unsigned char)0);
            if (e >= first) m = spu_or(m, m0);
            if (e + 1 < e_end) m = spu_or(m, m1);
            vec_uchar16* d = (vec_uchar16*)(tmem_ + 0x800 + e * 8);
            *d = spu_sel(*d, v, m);
        }
        return;
    }
#endif
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = (0x800 + (first + i) * 8) & 0xFFF;
        tmem_[a] = in[i * 2];
        tmem_[a + 1] = in[i * 2 + 1];
    }
}

void Interpreter::fill_rect(Backend& be, uint32_t w0, uint32_t w1) {
    fill_rect_at(be, bits(w1, 12, 12) / 4.0f, bits(w1, 0, 12) / 4.0f, bits(w0, 12, 12) / 4.0f, bits(w0, 0, 12) / 4.0f, false);
}

void Interpreter::fill_rect_at(Backend& be, float ulx, float uly, float lrx, float lry, bool explicit_coords) {
    CycleType cyc = st_.cycle();
    if (cyc == CYC_FILL || cyc == CYC_COPY) { lrx += 1; lry += 1; }
    if (st_.cimg_addr != 0 && st_.cimg_addr == st_.zimg_addr && cyc == CYC_FILL) {
        be.clear_depth(st_.zimg_addr, ulx, uly, lrx, lry);
        return;
    }
    viewport_to_state();
    st_.tex[0] = st_.tex[1] = nullptr;
    note_color_image_drawn();
    uint64_t t0 = ticks();
    st_.rect_explicit = explicit_coords;
    be.fill_rect(st_, ulx, uly, lrx, lry);
    st_.rect_explicit = 0;
    stats.t_backend += ticks() - t0;
    stats.fills++;
}

void Interpreter::tex_rect(Backend& be, uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, bool flip) {
    tex_rect_at(be, bits(w1, 12, 12) / 4.0f, bits(w1, 0, 12) / 4.0f, bits(w0, 12, 12) / 4.0f, bits(w0, 0, 12) / 4.0f,
                bits(w1, 24, 3), half1, half2, flip, false);
}

void Interpreter::tex_rect_at(Backend& be, float ulx, float uly, float lrx, float lry, uint32_t tile, uint32_t half1,
                              uint32_t half2, bool flip, bool explicit_coords) {
    float s = (int16_t)(half1 >> 16) / 32.0f, t = (int16_t)(half1 & 0xFFFF) / 32.0f;
    float dsdx = (int16_t)(half2 >> 16) / 1024.0f, dtdy = (int16_t)(half2 & 0xFFFF) / 1024.0f;
    CycleType cyc = st_.cycle();
    if (cyc == CYC_COPY) { dsdx /= 4.0f; lrx += 1; lry += 1; }
    if (cyc == CYC_FILL) { lrx += 1; lry += 1; }
    viewport_to_state();
    note_color_image_drawn();
    uint64_t t0 = ticks();
    prepare_textures(tile, true);
    uint64_t t1 = ticks();
    st_.rect_explicit = explicit_coords;
    be.tex_rect(st_, ulx, uly, lrx, lry, s, t, dsdx, dtdy, flip);
    st_.rect_explicit = 0;
    stats.t_texture += t1 - t0;
    stats.t_backend += ticks() - t1;
    stats.rects++;
}

// ------------------------------------------------------------------------------ main loop
#if defined(N64GFX_CMD_HOOK)
void (*g_cmd_hook)(uint32_t pc, uint32_t w0, uint32_t w1, const uint8_t* tmem, int depth) = nullptr;
#endif

void Interpreter::run(const Memory& mem, uint32_t dl_addr, Backend& be) {
    stats.dls++;
    cbfd_.advanced = false;  // each graphics task starts in the basic lighting mode (as RT64's reset)
    ex_gbi_ = false;         // RT64's extended GBI is enabled per display list
    interp_ = history_ != nullptr;
    last_proj_change_ = proj_change_;
    proj_change_ = 0;
    if (interp_) {
        // The in-between frame starts where the last display list left the matrices.
        std::memcpy(model_mid_, model_, sizeof model_);
        std::memcpy(proj_mid_, proj_, sizeof proj_);
        mvp_dirty_ = true;
        group_depth_ = 0;
        ungrouped_[0] = ungrouped_[1] = 0;
    }
    const uint64_t t0 = ticks();
    for (auto& lk : lookup_) lk.tex = nullptr;  // the texture cache may have dropped images since
    be.begin_dl();
    exec(mem, dl_addr & 0xFFFFFF, be);
    flush_tris(be);
    // A camera cut: a projection changed by over 3% (squared 0.0009) and much more than last frame.
    if (interp_ && proj_change_ > 0.0009f && proj_change_ > 9.0f * last_proj_change_ + 0.0001f) stats.camera_cut = 1;
    const uint64_t t1 = ticks();
    be.end_dl();
    stats.t_backend += ticks() - t1;
    stats.t_total += ticks() - t0;
}

void Interpreter::exec(const Memory& mem, uint32_t addr, Backend& be) {
    uint32_t stack[32];
    int depth = 0;
    uint32_t pc = addr;
    for (uint32_t guard = 0; guard < 1000000; guard++) {
        uint32_t w0 = mem.r32(pc), w1 = mem.r32(pc + 4);
        current_w0 = w0; current_w1 = w1; current_pc = pc;
#if defined(N64GFX_CMD_HOOK)  // workstation tools only: see every command and the TMEM it leaves
        if (g_cmd_hook) g_cmd_hook(pc, w0, w1, tmem_, depth);
#endif
        pc += 8;
        stats.commands++;
        uint8_t op = (uint8_t)(w0 >> 24);
        // Commands that keep the batch going.
        switch (op) {
        case 0x01: cmd_vertex(mem, w0, w1); continue;                          // G_VTX
        case 0x05: {                                                           // G_TRI1
            const uint64_t t0 = ticks();
            triangle(be, bits(w0, 17, 7), bits(w0, 9, 7), bits(w0, 1, 7));
            stats.t_tris += ticks() - t0;
            continue;
        }
        case 0x06: case 0x07: {                                                 // G_TRI2, G_QUAD
            const uint64_t t0 = ticks();
            triangle(be, bits(w0, 17, 7), bits(w0, 9, 7), bits(w0, 1, 7));
            triangle(be, bits(w1, 17, 7), bits(w1, 9, 7), bits(w1, 1, 7));
            stats.t_tris += ticks() - t0;
            continue;
        }
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: {  // Conker's TRI4
            uint32_t v[12] = {
                bits(w0, 23, 5), bits(w0, 18, 5), (bits(w0, 15, 3) << 2) | bits(w1, 30, 2),
                bits(w0, 10, 5), bits(w0, 5, 5), bits(w0, 0, 5),
                bits(w1, 25, 5), bits(w1, 20, 5), bits(w1, 15, 5),
                bits(w1, 10, 5), bits(w1, 5, 5), bits(w1, 0, 5),
            };
            const uint64_t t0 = ticks();
            for (int t = 0; t < 4; t++) {
                uint32_t a = v[t * 3], b = v[t * 3 + 1], c = v[t * 3 + 2];
                if (a != b && b != c && a != c) triangle(be, a, b, c);
            }
            stats.t_tris += ticks() - t0;
            continue;
        }
        case 0xDA: cmd_matrix(mem, w0, w1); continue;                          // G_MTX
        case 0xD8: {                                                           // G_POPMTX
            uint32_t n = w1 >> 6;
            while (n-- && model_depth_ > 1) model_depth_--;
            mvp_dirty_ = true;
            continue;
        }
        case 0xDE:                                                             // G_DL
            if (bits(w0, 16, 8) == 0 && depth < 32) stack[depth++] = pc;
            pc = seg(w1);
            continue;
        case 0xDF:                                                             // G_ENDDL
            if (depth == 0) return;
            pc = stack[--depth];
            continue;
        case 0xE1: rdphalf1_ = w1; continue;                                   // G_RDPHALF_1
        case 0xE0:  // G_NOOP; RT64's hook enabling its extended GBI on opcode 0x64
            if ((w0 & 0xFFFFFF) == 0x525464 && (w1 >> 28) == 1 && (w1 & 0xFF) == 0x64) ex_gbi_ = true;
            continue;
        case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xF1: continue;  // syncs
        case 0x00: continue;                                                   // G_NOOP
        case 0x03: continue;                                                   // G_CULLDL: draw everything
        case 0xDB:                                                             // G_MOVEWORD
            if (bits(w0, 16, 8) == 0x08) flush_tris(be);
            cmd_moveword(mem, w0, w1);
            continue;
        default: break;
        }
        // Everything else changes the draw state.
        flush_tris(be);
        switch (op) {
        case 0x02: {  // G_MODIFYVTX
            uint32_t where = bits(w0, 16, 8), v = bits(w0, 0, 16) / 2;
            if (v >= 64) break;
            Vtx& x = vtx_[v];
            if (where == 0x10) { x.c[0] = (uint8_t)(w1 >> 24); x.c[1] = (uint8_t)(w1 >> 16); x.c[2] = (uint8_t)(w1 >> 8); x.c[3] = (uint8_t)w1; }
            else if (where == 0x14) { x.s = (int16_t)(w1 >> 16) * texture_.sc / 32.0f; x.t = (int16_t)(w1 & 0xFFFF) * texture_.tc / 32.0f; }
            else if (where == 0x18) {
                float sx = (int16_t)(w1 >> 16) / 4.0f, sy = (int16_t)(w1 & 0xFFFF) / 4.0f;
                if (vp_scale_[0] != 0 && vp_scale_[1] != 0) {
                    x.x = (sx - vp_trans_[0]) / vp_scale_[0] * x.w;
                    x.y = -(sy - vp_trans_[1]) / vp_scale_[1] * x.w;
                    // The in-between frame: the same place on screen (Conker's chocolate bar is placed so;
                    // left where its vertex load put it, it was elsewhere every other frame).
                    if (interp_) {
                        x.mx = (sx - vp_trans_[0]) / vp_scale_[0] * x.mw;
                        x.my = -(sy - vp_trans_[1]) / vp_scale_[1] * x.mw;
                    }
                }
            }
            break;
        }
        case 0x04: {  // G_BRANCH_Z: branch to the RDPHALF_1 address when the vertex is closer than w1
            uint32_t v = bits(w0, 1, 11) / 5;
            if (v < 64) {
                const Vtx& x = vtx_[v];
                bool behind = x.w <= 0;
                float depth01 = behind ? 0 : (x.z / x.w) * (vp_scale_[2] / 1023.0f) + vp_trans_[2] / 1023.0f;
                if (behind || depth01 * 1023.0f < (float)w1 / 65536.0f) pc = seg(rdphalf1_);
            }
            break;
        }
        case 0xD7:  // G_TEXTURE
            texture_.tile = bits(w0, 8, 3);
            texture_.level = bits(w0, 11, 3);
            texture_.on = bits(w0, 1, 7) != 0;
            texture_.sc = (w1 >> 16) / 65536.0f;
            texture_.tc = (w1 & 0xFFFF) / 65536.0f;
            if ((w1 >> 16) == 0xFFFF) texture_.sc = 1.0f;
            if ((w1 & 0xFFFF) == 0xFFFF) texture_.tc = 1.0f;
            break;
        case 0xD9:  // G_GEOMETRYMODE
            geometry_mode_ = (geometry_mode_ & (w0 & 0xFFFFFF)) | w1;
            st_.geometry_mode = geometry_mode_;
            break;
        case 0xDC: cmd_movemem(mem, w0, w1); break;                            // G_MOVEMEM
        case 0xDD: cbfd_.advanced = true; break;                              // Conker: G_LOAD_UCODE = advanced lighting
        case 0xE2: case 0xE3: {                                                // G_SETOTHERMODE_L / _H
            uint32_t len = bits(w0, 0, 8) + 1;
            int shift = 32 - (int)bits(w0, 8, 8) - (int)len;
            if (shift < 0) shift = 0;
            uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
            uint32_t& om = op == 0xE2 ? st_.om_l : st_.om_h;
            om = (om & ~mask) | (w1 & mask);
            break;
        }
        case 0xEF: st_.om_h = w0 & 0xFFFFFF; st_.om_l = w1; break;             // G_RDPSETOTHERMODE
        case 0x64: {  // RT64's extended GBI (only what the widescreen hooks write)
            if (!ex_gbi_) break;
            const uint32_t sub = w0 & 0xFFFFFF;
            auto corner = [](uint32_t v) { return (int16_t)(v & 0xFFFF) / 4.0f; };
            if (sub == 0x000002) {  // G_EX_TEXRECT_V1: rectangle, then s/t and steps (signed corners)
                const uint32_t c0 = mem.r32(pc), c1 = mem.r32(pc + 4), h1 = mem.r32(pc + 8), h2 = mem.r32(pc + 12);
                pc += 16;
                flush_tris(be);
                tex_rect_at(be, corner(c0 >> 16), corner(c0), corner(c1 >> 16), corner(c1), w1 & 7, h1, h2, false, true);
            } else if (sub == 0x000003) {  // G_EX_FILLRECT_V1: rectangle (signed corners)
                const uint32_t c0 = mem.r32(pc), c1 = mem.r32(pc + 4);
                pc += 8;
                flush_tris(be);
                fill_rect_at(be, corner(c0 >> 16), corner(c0), corner(c1 >> 16), corner(c1), true);
            }
            else if (sub == 0x00000C) {  // G_EX_MATRIXGROUP_V1: id, then its parameters
                pc += 8;
                if (group_depth_ < 8) { group_ids_[group_depth_] = w1; group_counts_[group_depth_] = 0; group_vtx_[group_depth_] = 0; group_depth_++; }
            } else if (sub == 0x00000D) {  // G_EX_POPMATRIXGROUP_V1: how many
                for (uint32_t k = 0; k < w1 && group_depth_ > 0; k++) group_depth_--;
            }
            // G_EX_SETRECTASPECT_V1 (0x33) and the rest: ignored.
            break;
        }
        case 0xE4: case 0xE5: {                                                // G_TEXRECT(FLIP): followed by RDPHALF_1, RDPHALF_2
            uint32_t h1 = 0, h2 = 0;
            if ((mem.r32(pc) >> 24) == 0xE1) { h1 = mem.r32(pc + 4); pc += 8; }
            if ((mem.r32(pc) >> 24) == 0xF1) { h2 = mem.r32(pc + 4); pc += 8; }
            tex_rect(be, w0, w1, h1, h2, op == 0xE5);
            break;
        }
        case 0xED:  // G_SETSCISSOR
            st_.sc_ulx = bits(w0, 12, 12) / 4.0f; st_.sc_uly = bits(w0, 0, 12) / 4.0f;
            st_.sc_lrx = bits(w1, 12, 12) / 4.0f; st_.sc_lry = bits(w1, 0, 12) / 4.0f;
            break;
        case 0xEE: st_.prim_depth = (w1 >> 16) / 32768.0f; break;              // G_SETPRIMDEPTH
        case 0xEC: {                                                           // G_SETCONVERT: K5 is a combiner input
            int32_t k5 = (int32_t)(w1 & 0x1FF);
            if (k5 & 0x100) k5 -= 0x200;
            st_.k5 = k5 / 255.0f;
            break;
        }
        case 0xEA: case 0xEB: break;                                           // keys
        case 0xF0: load_tlut(mem, w0, w1); break;                              // G_LOADTLUT
        case 0xF2: {                                                           // G_SETTILESIZE
            Tile& t = tiles_[bits(w1, 24, 3)];
            t.uls = bits(w0, 12, 12); t.ult = bits(w0, 0, 12); t.lrs = bits(w1, 12, 12); t.lrt = bits(w1, 0, 12);
            break;
        }
        case 0xF3: load_block(mem, w0, w1); break;                             // G_LOADBLOCK
        case 0xF4: load_tile(mem, w0, w1); break;                              // G_LOADTILE
        case 0xF5: {                                                           // G_SETTILE
            Tile& t = tiles_[bits(w1, 24, 3)];
            t.fmt = bits(w0, 21, 3); t.siz = bits(w0, 19, 2); t.line = bits(w0, 9, 9); t.tmem = bits(w0, 0, 9);
            t.palette = bits(w1, 20, 4); t.cmt = bits(w1, 18, 2); t.maskt = bits(w1, 14, 4); t.shiftt = bits(w1, 10, 4);
            t.cms = bits(w1, 8, 2); t.masks = bits(w1, 4, 4); t.shifts = bits(w1, 0, 4);
            break;
        }
        case 0xF6: fill_rect(be, w0, w1); break;                               // G_FILLRECT
        case 0xF7: st_.fill_color = w1; break;                                 // G_SETFILLCOLOR
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: {                          // fog, blend, prim, env colors
            float c[4] = {(w1 >> 24) / 255.0f, ((w1 >> 16) & 255) / 255.0f, ((w1 >> 8) & 255) / 255.0f, (w1 & 255) / 255.0f};
            float* dst = op == 0xF8 ? st_.fog : op == 0xF9 ? st_.blend : op == 0xFA ? st_.prim : st_.env;
            std::memcpy(dst, c, sizeof c);
            if (op == 0xFA) st_.prim_lod_frac = (w0 & 0xFF) / 255.0f;
            break;
        }
        case 0xFC: st_.combine_hi = w0 & 0xFFFFFF; st_.combine_lo = w1; break; // G_SETCOMBINE
        case 0xFD:                                                             // G_SETTIMG
            timg_.fmt = bits(w0, 21, 3); timg_.siz = bits(w0, 19, 2); timg_.width = bits(w0, 0, 12) + 1; timg_.addr = seg(w1);
            break;
        case 0xFE: st_.zimg_addr = seg(w1); break;                             // G_SETZIMG
        case 0xFF:                                                             // G_SETCIMG
            st_.cimg_fmt = bits(w0, 21, 3); st_.cimg_siz = bits(w0, 19, 2);
            st_.cimg_width = bits(w0, 0, 12) + 1; st_.cimg_addr = seg(w1);
            break;
        default:
            stats.unknown++;
            last_unknown_opcode = op;
            break;
        }
    }
}

}  // namespace n64gfx
