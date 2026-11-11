// n64gfx: a compact high-level renderer front end for Conker's Bad Fur Day.
//
// Interprets the game's display lists (F3DEX2 with Rare's changes, "F3DEX2CBFD") the way the RSP
// microcode would, and the RDP commands they contain: vertices are transformed and lit on the CPU,
// textures are loaded into an emulated TMEM and decoded to RGBA8 images, and every draw is handed
// to a Backend with the RDP state it needs (combiner, blender, depth, scissor, textures).
//
// Backends: the RSX on the PS3 (rsx_backend.cpp) and a software rasterizer on the workstation
// (softraster.cpp) that renders frames to images for checking. The front end is portable: it reads
// N64 memory through Memory, which knows the host's byte order.
//
// References: RT64 (MIT) for the F3DEX2 command handling, and CBFD-Recompiled's RT64 patch (based on
// GLideN64's F3DEX2CBFD) for Conker's vertex lighting, texture generation and TRI4 command.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__SPU__)
#include <spu_intrinsics.h>
#endif
#if !defined(__SPU__)
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <vector>
#endif

// Diagnostic messages (the log on the PS3's PPU; none on an SPU).
#if defined(__SPU__)
#define N64GFX_LOG(...) ((void)0)
#else
#define N64GFX_LOG(...) std::printf(__VA_ARGS__)
#endif

namespace n64gfx {
#if defined(N64GFX_CMD_HOOK)
extern void (*g_cmd_hook)(uint32_t pc, uint32_t w0, uint32_t w1, const uint8_t* tmem, int depth);
#endif

// A cheap cycle counter for profiling (timebase on the PS3, TSC elsewhere).
#if defined(__PPU__) || defined(__powerpc__)
inline uint64_t ticks() { uint64_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
#elif defined(__x86_64__)
inline uint64_t ticks() { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
#elif defined(__SPU__)
inline uint64_t ticks() { return 0; }  // the SPU program times whole display lists with its decrementer
#else
inline uint64_t ticks() { return 0; }
#endif

// ------------------------------------------------------------------------------------ memory
// N64 RDRAM as the runtime stores it: plain big-endian bytes on the PS3, 32-bit words in host order
// (bytes at address ^ 3) on little-endian hosts.
#if defined(__SPU__)
// On an SPU, game memory is reached by DMA through a small direct-mapped cache of 128-byte lines
// (the SPU program defines the arrays and miss(), and empties it before each display list).
// A byte copy for the SPU, in whole quadwords (shuffled into place, the ends merged), for any
// alignment: the library's memcpy goes much more slowly for the short and unaligned copies here.
inline void spu_copy(void* dst, const void* src, uint32_t n) {
    if (!n) return;
    const uintptr_t d = (uintptr_t)dst, s = (uintptr_t)src;
    const uint32_t head = d & 15, sh = (uint32_t)(s - d) & 15;
    vec_uchar16* dq = (vec_uchar16*)(d & ~(uintptr_t)15);
    const vec_uchar16* sq = (const vec_uchar16*)((s - head) & ~(uintptr_t)15);  // holds the byte for dq[0][0]
    const vec_uchar16 idx = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const vec_uchar16 pat = (vec_uchar16)spu_add((vec_ushort8)idx, spu_splats((unsigned short)(sh * 0x101)));  // no carries: bytes stay below 31
    const uint32_t quads = (head + n + 15) >> 4, tail = head + n - ((quads - 1) << 4);  // bytes used of the last
    const vec_uchar16 zero = spu_splats((unsigned char)0);
    // The first and last quadwords keep the bytes of dst outside [d, d + n).
    const vec_uchar16 keep_first = spu_cmpgt(spu_splats((unsigned char)head), idx);
    const vec_uchar16 keep_last = spu_cmpgt(idx, spu_splats((unsigned char)(tail - 1)));
    if (quads == 1) {
        dq[0] = spu_sel(spu_shuffle(sq[0], sq[1], pat), dq[0], spu_or(keep_first, keep_last));
        return;
    }
    dq[0] = spu_sel(spu_shuffle(sq[0], sq[1], pat), dq[0], keep_first);
    uint32_t i = 1;
    for (; i + 4 <= quads - 1; i += 4) {
        const vec_uchar16 a = sq[i], b = sq[i + 1], c = sq[i + 2], e = sq[i + 3], f = sq[i + 4];
        dq[i] = spu_shuffle(a, b, pat); dq[i + 1] = spu_shuffle(b, c, pat);
        dq[i + 2] = spu_shuffle(c, e, pat); dq[i + 3] = spu_shuffle(e, f, pat);
    }
    for (; i < quads - 1; i++) dq[i] = spu_shuffle(sq[i], sq[i + 1], pat);
    dq[quads - 1] = spu_sel(spu_shuffle(sq[quads - 1], sq[quads], pat), dq[quads - 1], keep_last);
    (void)zero;
}

namespace spucache {
constexpr uint32_t kLineBits = 9, kLineSize = 1u << kLineBits, kLines = 64;  // 32 KB
extern uint32_t tags[kLines];
extern uint8_t data[kLines][kLineSize];
const uint8_t* miss(uint32_t a);  // fetches the line holding a, returns its data
inline const uint8_t* line(uint32_t a) {
    const uint32_t ln = a >> kLineBits, set = ln & (kLines - 1);
    if (__builtin_expect(tags[set] != ln, 0)) return miss(a);
    return data[set];
}
}  // namespace spucache
struct Memory {
    static constexpr uint32_t kMask = 0xFFFFFF;  // 16 MB: the game's 8 and our own display lists past it
    uint8_t r8(uint32_t a) const { a &= kMask; return spucache::line(a)[a & (spucache::kLineSize - 1)]; }
    uint16_t r16(uint32_t a) const { a &= kMask & ~1u; return *(const uint16_t*)(spucache::line(a) + (a & (spucache::kLineSize - 1))); }
    uint32_t r32(uint32_t a) const { a &= kMask & ~3u; return *(const uint32_t*)(spucache::line(a) + (a & (spucache::kLineSize - 1))); }
    // n bytes from a (in N64 order) to dst.
    void read(uint32_t a, uint8_t* dst, uint32_t n) const {
        while (n) {
            a &= kMask;
            const uint32_t off = a & (spucache::kLineSize - 1), k = n < spucache::kLineSize - off ? n : spucache::kLineSize - off;
            spu_copy(dst, spucache::line(a) + off, k);
            a += k; dst += k; n -= k;
        }
    }
};
#else
struct Memory {
    uint8_t* rdram = nullptr;
    // 16 MB: the game's 8 MB (Expansion Pak) and display lists the widescreen hooks write past it
    // (from 0x00F00000, in memory the PS3 runtime keeps for mods).
    static constexpr uint32_t kMask = 0xFFFFFF;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint8_t r8(uint32_t a) const { return rdram[a & kMask]; }
    uint16_t r16(uint32_t a) const { return *(const uint16_t*)(rdram + (a & (kMask & ~1u))); }
    uint32_t r32(uint32_t a) const { return *(const uint32_t*)(rdram + (a & (kMask & ~3u))); }
    void read(uint32_t a, uint8_t* dst, uint32_t n) const {  // n bytes from a (in N64 order) to dst
        while (n) {
            a &= kMask;
            const uint32_t k = n < kMask + 1 - a ? n : kMask + 1 - a;
            __builtin_memcpy(dst, rdram + a, k);
            a += k; dst += k; n -= k;
        }
    }
#else
    uint8_t r8(uint32_t a) const { return rdram[(a & kMask) ^ 3]; }
    uint16_t r16(uint32_t a) const { return *(const uint16_t*)(rdram + ((a & (kMask & ~1u)) ^ 2)); }
    uint32_t r32(uint32_t a) const { return *(const uint32_t*)(rdram + (a & (kMask & ~3u))); }
    void read(uint32_t a, uint8_t* dst, uint32_t n) const { for (uint32_t i = 0; i < n; i++) dst[i] = r8(a + i); }
#endif
};
#endif

// ------------------------------------------------------------------------------- RDP state
enum CycleType { CYC_1 = 0, CYC_2 = 1, CYC_COPY = 2, CYC_FILL = 3 };

struct Tile {
    uint32_t fmt = 0, siz = 0, line = 0, tmem = 0, palette = 0;
    uint32_t cms = 0, cmt = 0, masks = 0, maskt = 0, shifts = 0, shiftt = 0;
    uint32_t uls = 0, ult = 0, lrs = 0, lrt = 0;  // 10.2 fixed point
};

// A texture ready to draw: a tile decoded from TMEM to RGBA8 (R first in memory).
struct Texture {
    const uint8_t* rgba = nullptr;  // width * height * 4 bytes, owned by the TextureCache
    uint32_t width = 0, height = 0;
    uint64_t hash = 0;              // identifies the decoded contents
    uint8_t fmt = 0, siz = 0, palette = 0, tlut = 0;  // what it was decoded from (diagnostics)
    void* backend_data = nullptr;   // e.g. the RSX copy of the image
    // A piece of a frame the game drew (loaded into TMEM from a color image, as the motion blur and the
    // pause screen's background do): no pixels here (game memory doesn't have what the backend drew);
    // the backend reads its own copy of the color image at fb_addr, from (fb_x, fb_y), width x height.
    uint32_t fb_addr = 0;
    int32_t fb_x = 0, fb_y = 0;
};

// How a vertex's texture coordinate (texels, see OutVertex) maps onto a Texture's image:
//   u = s * shift_s - offset_s, then wrapped / mirrored / clamped by the modes below.
struct TexMap {
    float shift_s = 1, shift_t = 1, offset_s = 0, offset_t = 0;
    bool clamp_s = false, clamp_t = false, mirror_s = false, mirror_t = false;
    float clamp_max_s = 0, clamp_max_t = 0;  // last texel before clamping
};

struct DrawState {
    // Target.
    uint32_t cimg_addr = 0, cimg_width = 320, cimg_fmt = 0, cimg_siz = 2;
    uint32_t zimg_addr = 0;
    // Modes.
    uint32_t geometry_mode = 0;
    uint32_t om_h = 0, om_l = 0;
    uint32_t combine_hi = 0, combine_lo = 0;  // G_SETCOMBINE w0 & 0xFFFFFF, w1
    // Colors (0..1).
    float prim[4] = {}, env[4] = {}, fog[4] = {}, blend[4] = {};
    float prim_lod_frac = 0;
    float k5 = 0;  // G_SETCONVERT's K5 (signed 9-bit / 255), a combiner C input
    uint32_t fill_color = 0;
    float prim_depth = 0;  // 0..1
    // Scissor, in target pixels.
    float sc_ulx = 0, sc_uly = 0, sc_lrx = 320, sc_lry = 240;
    // Viewport: screen = translate + ndc * scale (y: translate - ndc * scale).
    float vp_scale[3] = {160, 120, 0.5f}, vp_trans[3] = {160, 120, 0.5f};
    // A rectangle from RT64's extended GBI: drawn where its coordinates say (widescreen.h).
    uint32_t rect_explicit = 0;
    // Textures for TEXEL0 and TEXEL1 (null when not used).
    const Texture* tex[2] = {nullptr, nullptr};
    TexMap tmap[2];
    // Mipmapping (texture LOD, 2-cycle): TEXEL0's smaller versions, each half the one before (tex[0]
    // is the largest). Backends that can draw a mipmapped texture use them for TEXEL0; the combiner's
    // LOD fraction stays 0, so (TEXEL1 - TEXEL0) * LOD + TEXEL0 is that texture.
    static constexpr uint32_t kMaxMips = 7;
    uint32_t mip_levels = 0;
    const Texture* mip[kMaxMips] = {};

    CycleType cycle() const { return CycleType((om_h >> 20) & 3); }
    bool z_compare() const { return (om_l & 0x10) != 0; }
    bool z_update() const { return (om_l & 0x20) != 0; }
    uint32_t z_mode() const { return (om_l >> 10) & 3; }  // 0 opaque, 1 interpenetrating, 2 translucent, 3 decal
    bool z_source_prim() const { return (om_l & 4) != 0; }
    bool force_blend() const { return (om_l & 0x4000) != 0; }
    uint32_t alpha_compare() const { return om_l & 3; }    // 1 threshold, 3 dither
    bool cvg_x_alpha() const { return (om_l & 0x1000) != 0; }
    bool alpha_cvg_sel() const { return (om_l & 0x2000) != 0; }
    bool z_buffered() const { return (geometry_mode & 0x1) != 0; }  // G_ZBUFFER
    // Whether the blender's last cycle blends. With FORCE_BL it always does; without, the RDP blends only
    // pixels with partial coverage, and with CVG_X_ALPHA (coverage times alpha, and alpha not used as
    // the coverage itself) that's every pixel whose alpha is below 1: alpha blending in effect (Conker's
    // iris transitions draw their mask this way).
    bool blends() const { return force_blend() || (cvg_x_alpha() && !alpha_cvg_sel()); }
};

// A vertex handed to the backend: clip-space position, texture coordinates in texel units (after
// G_TEXTURE's scale, before the tile's offset and shift) and the shade color (RGBA bytes, as the RSP
// stores it).
struct alignas(16) OutVertex {  // 32 bytes, so it copies as two quadwords (SPU, AltiVec)
    float x, y, z, w;
    float s, t;
    uint8_t c[4];
    uint32_t pad;
};

class Backend {
public:
    virtual ~Backend() = default;
    // Triangles (count is a multiple of 3), in clip space, already culled by the interpreter.
    virtual void draw_triangles(const DrawState& st, const OutVertex* v, size_t count) = 0;
    // Rectangles in target pixels (exclusive lower-right corner), already adjusted for the cycle type.
    virtual void fill_rect(const DrawState& st, float ulx, float uly, float lrx, float lry) = 0;
    // s, t: texel coordinates at (ulx, uly); dsdx, dtdy: per pixel. flip swaps s and t.
    virtual void tex_rect(const DrawState& st, float ulx, float uly, float lrx, float lry,
                          float s, float t, float dsdx, float dtdy, bool flip) = 0;
    // The game filled a depth buffer (its usual way of clearing it) with a fill rectangle.
    virtual void clear_depth(uint32_t zimg_addr, float ulx, float uly, float lrx, float lry) = 0;
    // A new display list is starting / has ended.
    virtual void begin_dl() {}
    virtual void end_dl() {}
    // With frame interpolation (Interpreter::set_matrix_history): the same triangles, with also each
    // vertex's clip-space position in the frame halfway from the last one (x, y, z, w each).
    virtual void draw_triangles_mid(const DrawState& st, const OutVertex* v, const float (*mid)[4], size_t count) {
        (void)mid;
        draw_triangles(st, v, count);
    }    // The same triangles with these positions instead of the vertices' own (an in-between frame's).
    virtual void draw_triangles_pos(const DrawState& st, const OutVertex* v, const float (*pos)[4], size_t count) {
        OutVertex tmp[96];
        for (size_t done = 0; done < count;) {
            const size_t m = count - done < 96 ? count - done : 96;
            for (size_t k = 0; k < m; k++) {
                tmp[k] = v[done + k];
                tmp[k].x = pos[done + k][0]; tmp[k].y = pos[done + k][1]; tmp[k].z = pos[done + k][2]; tmp[k].w = pos[done + k][3];
            }
            draw_triangles(st, tmp, m);
            done += m;
        }
    }
};

// Frame interpolation (60 frames a second from the game's 30): the matrices the game loaded in its
// last display list, by key (an object's group and the matrix's place in it, or the place among those
// outside any group), so that each matrix loaded now can be blended with its counterpart halfway.
class MatrixHistory {
public:
    virtual ~MatrixHistory() = default;
    // Last display list's matrix under this key (false if there was none).
    virtual bool find(uint64_t key, float out[4][4]) = 0;
    // This display list's matrix under this key.
    virtual void store(uint64_t key, const float m[4][4]) = 0;
    // The same for a batch of vertices' object-space positions (x, y, z, 0 as the game's 16-bit
    // values; n up to 64), loaded by one command inside an object's group: the game moves some
    // objects' vertices itself (bending joints, and whole objects), not with their matrices.
    virtual bool find_vertices(uint64_t key, int16_t (*out)[4], uint32_t n) = 0;
    virtual void store_vertices(uint64_t key, const int16_t (*v)[4], uint32_t n) = 0;
};

// ----------------------------------------------------------------------------- texture cache
// Where the interpreter gets textures: the decoding cache below, or (on an SPU) a recorder that passes
// the tile and its TMEM on to the PPU (n64gfx_stream.h).
class TextureSource {
public:
    virtual ~TextureSource() = default;
    // The texture of a tile with TMEM as it is now.
    virtual const Texture* get(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type, bool copy_mode) = 0;
    // The texture is used again (found through the interpreter's per-tile lookup).
    virtual void touch(const Texture* t) { (void)t; }
};

// The dimensions of a tile's image (as the texture cache decodes it).
void tile_size(const Tile& tile, uint32_t* w, uint32_t* h);
// The texture cache's key for a tile and TMEM: tile parameters and the TMEM bytes the image covers.
uint64_t tile_hash(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type);
// Decodes a tile from TMEM to RGBA8 (w x h from tile_size).
void decode_tile(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type, uint8_t* rgba);

#if !defined(__SPU__)
// A MatrixHistory in two hash maps (this display list's, the last one's).
class MapMatrixHistory : public MatrixHistory {
public:
    bool find(uint64_t key, float out[4][4]) override {
        auto it = prev_.find(key);
        if (it == prev_.end()) return false;
        std::memcpy(out, it->second.m, sizeof it->second.m);
        return true;
    }
    void store(uint64_t key, const float m[4][4]) override { std::memcpy(cur_[key].m, m, sizeof cur_[key].m); }
    bool find_vertices(uint64_t key, int16_t (*out)[4], uint32_t n) override {
        auto it = vprev_.find(key);
        if (it == vprev_.end() || it->second.size() != n * 4) return false;
        std::memcpy(out, it->second.data(), n * 8);
        return true;
    }
    void store_vertices(uint64_t key, const int16_t (*v)[4], uint32_t n) override {
        auto& d = vcur_[key];
        d.assign(&v[0][0], &v[0][0] + n * 4);
    }
    // The display list is done: its matrices become the last ones.
    void next_frame() { prev_.swap(cur_); cur_.clear(); vprev_.swap(vcur_); vcur_.clear(); }
    // Forget them (a cut, or the in-between frames were off).
    void clear() { prev_.clear(); cur_.clear(); vprev_.clear(); vcur_.clear(); }

private:
    struct M { float m[4][4]; };
    std::unordered_map<uint64_t, M> prev_, cur_;
    std::unordered_map<uint64_t, std::vector<int16_t>> vprev_, vcur_;
};

class TextureCache : public TextureSource {
public:
    // Returns the decoded image of a tile, decoding it if this TMEM content has not been seen.
    const Texture* get(const uint8_t* tmem, const Tile& tile, uint32_t tlut_type, bool copy_mode) override;
    // The same, with the key already known (the stream player has it from the SPU).
    const Texture* get_hashed(uint64_t hash, const uint8_t* tmem, const Tile& tile, uint32_t tlut_type);
    // A texture already decoded under this key, or null.
    const Texture* find(uint64_t hash);
    void end_frame();  // forgets textures not used for a while (calls on_evict for each first)
    void touch(const Texture* t) override;  // marks an image as used this frame
    size_t size() const { return entries_.size(); }
    std::function<void(Texture&)> on_evict;  // backends free their copy of the image here
    uint32_t decoded = 0;
private:
    struct Entry { Texture tex; std::vector<uint8_t> pixels; uint32_t last_used; };
    std::unordered_map<uint64_t, Entry*> entries_;
    uint32_t frame_ = 0;
};
#endif

// ------------------------------------------------------------------------------- interpreter
struct Stats {
    uint32_t dls = 0, commands = 0, triangles = 0, rects = 0, fills = 0, unknown = 0;
    uint32_t vertices = 0, textures_decoded = 0;
    uint32_t texture_lookups = 0, texture_hashes = 0, fb_textures = 0;
    // Time (ticks()) spent in: the whole display list, vertex processing, texture lookup/decoding,
    // and the backend (drawing calls, including any waits inside them).
    uint64_t t_total = 0, t_vertex = 0, t_texture = 0, t_backend = 0;
    uint64_t t_tris = 0;  // triangle commands (culling, batching)
    uint32_t matrices = 0, matrices_blended = 0;  // frame interpolation: loaded, and blended with last frame's
    uint32_t vertex_loads = 0, vertex_loads_blended = 0;  // and objects' vertex loads
    uint32_t camera_cut = 0;  // a projection changed: no in-between frame for this list
    uint32_t clipped = 0, clipped_away = 0;  // triangles crossing the camera's plane, and wholly behind it
};

class Interpreter {
public:
    Interpreter();
    // Runs one graphics task's display list (physical address).
    void run(const Memory& mem, uint32_t dl_addr, Backend& backend);
#if !defined(__SPU__)
    TextureCache& textures() { return texcache_; }
#endif
    // Textures come from here (by default the interpreter's own cache).
    void set_texture_source(TextureSource* s) { texsrc_ = s; }
    Stats stats;
    uint8_t last_unknown_opcode = 0;
    volatile uint32_t current_w0 = 0, current_w1 = 0, current_pc = 0;  // for diagnostics
    bool frame_textures = true;  // false: textures loaded from drawn images come from game memory (old way)
    // Frame interpolation: with a history, every draw also gets the in-between frame's positions
    // (Backend::draw_triangles_mid). Null: off.
    void set_matrix_history(MatrixHistory* h) { history_ = h; }
    bool debug_blend = false;  // log every blend's relative difference (workstation)

private:
    struct alignas(16) Vtx {  // starts like OutVertex (copied as one block)
        float x, y, z, w;     // clip space
        float s, t;           // texels
        uint8_t c[4];         // RGBA (or the normal, with lighting)
        uint32_t pad;
        float ox, oy, oz;     // object space (for lighting)
        int16_t flag;
        float mx, my, mz, mw;  // clip space in the in-between frame (frame interpolation)
    };
    // Frame interpolation: object-space positions of the vertices in the in-between frame (cmd_vertex).
    alignas(16) float mid_obj_[64][4];
    bool mid_obj_used_ = false;
    // Per-vertex scratch rows (4 floats each) for the batched conversions in cmd_vertex.
    using Rows = float[64][4];
    struct Light { float r, g, b; float dir[3]; float pos[3]; float ca; };
    using Mat = float[4][4];

    // Display list.
    void exec(const Memory& mem, uint32_t addr, Backend& be);
    uint32_t seg(uint32_t a) const { return (segments_[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0xFFFFFF; }
    // RSP.
    void load_matrix(const Memory& mem, uint32_t addr, Mat out);
    void cmd_matrix(const Memory& mem, uint32_t w0, uint32_t w1);
    void cmd_vertex(const Memory& mem, uint32_t w0, uint32_t w1);
    void cmd_movemem(const Memory& mem, uint32_t w0, uint32_t w1);
    void cmd_moveword(const Memory& mem, uint32_t w0, uint32_t w1);
    void update_mvp();
    void load_vertices(const Memory& mem, uint32_t addr, uint32_t first, uint32_t count);
    void light_vertices(const Memory& mem, uint32_t first, uint32_t count, Rows& col);
    void texgen_vertices(const Memory& mem, uint32_t first, uint32_t count);
    void triangle(Backend& be, uint32_t a, uint32_t b, uint32_t c);
    void clip_triangle(Backend& be, const Vtx* const v[3], bool smooth);
    void flush_tris(Backend& be);
    // RDP.
    void load_block(const Memory& mem, uint32_t w0, uint32_t w1);
    void load_tile(const Memory& mem, uint32_t w0, uint32_t w1);
    void load_tlut(const Memory& mem, uint32_t w0, uint32_t w1);
    void fill_rect(Backend& be, uint32_t w0, uint32_t w1);
    void tex_rect(Backend& be, uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, bool flip);
    // Corners in pixels (lower right exclusive as the command gives it); explicit: from RT64's extended GBI.
    void fill_rect_at(Backend& be, float ulx, float uly, float lrx, float lry, bool explicit_coords);
    void tex_rect_at(Backend& be, float ulx, float uly, float lrx, float lry, uint32_t tile, uint32_t half1, uint32_t half2,
                     bool flip, bool explicit_coords);
    // RT64's extended GBI (the widescreen hooks write it): enabled by its hook in a G_NOOP (0xE0) and
    // used through opcode 0x64 until the end of the display list.
    bool ex_gbi_ = false;
    void prepare_textures(uint32_t base_tile, bool for_rect);
    void viewport_to_state();

    // RSP state.
    uint32_t segments_[16] = {};
    alignas(16) Mat model_[32];
    int model_depth_ = 1;
    alignas(16) Mat proj_;
    alignas(16) Mat mvp_;
    bool mvp_dirty_ = true;
    Vtx vtx_[64];
    uint32_t geometry_mode_ = 0;
    struct { uint32_t tile = 0, level = 0, on = 0; float sc = 1, tc = 1; } texture_;
    float vp_scale_[3] = {160, 120, 511}, vp_trans_[3] = {160, 120, 511};
    float fog_mul_ = 0, fog_off_ = 0;
    // Conker's lighting.
    struct {
        bool advanced = false;
        uint32_t normal_base = 0;
        uint32_t light_count = 0;
        Light lights[12] = {};
        float coord_mod[20] = {};
        float lookat[2][3] = {{1, 0, 0}, {0, 1, 0}};
    } cbfd_;
    uint32_t rdphalf1_ = 0;
    // RDP state.
    DrawState st_;
    struct { uint32_t fmt = 0, siz = 0, width = 0, addr = 0; } timg_;
    Tile tiles_[8];
    alignas(16) uint8_t tmem_[4096] = {};
    uint32_t tmem_gen_ = 1;  // changes whenever TMEM is written
    // The texture each tile slot resolved to last time, valid while TMEM and the tile are unchanged.
    struct TileLookup { Tile tile; uint32_t tlut = 0, gen = 0; const Texture* tex = nullptr; } lookup_[8];
#if !defined(__SPU__)
    TextureCache texcache_;
    TextureSource* texsrc_ = &texcache_;
#else
    TextureSource* texsrc_ = nullptr;
#endif
    // Color images drawn to (address, width, bytes per pixel), and the last TMEM load taken from one.
    struct FbImage { uint32_t addr = 0, width = 0, siz = 2, rows = 0; };  // rows: the most the game's scissor allowed
    static constexpr uint32_t kFbImages = 16;
    FbImage fb_images_[kFbImages];
    uint32_t fb_count_ = 0, fb_next_ = 0;
    struct FbLoad {
        uint32_t gen = 0, tmem = 0, addr = 0, siz = 0, row_bytes = 0;
        int32_t x = 0, y = 0; uint32_t w = 0, h = 0;
        bool block = false;  // LOADBLOCK: rows as the render tile's line reads them
    } fb_load_;
    // The color image holding [addr, addr + bytes), or null.
    const FbImage* find_fb_image(uint32_t addr, uint32_t bytes) const;
    // Logs the first loads from drawn images (used, or not understood) for diagnosis.
    void log_fb_load(const char* what, bool used, uint32_t src, uint32_t bytes, uint32_t img, const Tile& t);
    uint32_t fb_logged_ = 0;
    Texture fb_tex_[2];
    void note_color_image_drawn();
    // Batched triangles with the same state (a full batch is drawn and a new one started).
#if defined(__SPU__)
    static constexpr uint32_t kBatchMax = 3 * 128;  // (local store)
#else
    static constexpr uint32_t kBatchMax = 3 * 4096;
#endif
    OutVertex batch_[kBatchMax];
    alignas(16) float batch_mid_[kBatchMax][4];  // the in-between frame's positions (frame interpolation)
    uint32_t batch_n_ = 0;
    // Frame interpolation: the in-between frame's matrices (as model_, proj_, mvp_), and the keys.
    MatrixHistory* history_ = nullptr;
    bool interp_ = false;  // on for this display list
    alignas(16) Mat model_mid_[32];
    alignas(16) Mat proj_mid_;
    alignas(16) Mat mvp_mid_;
    uint32_t group_ids_[8] = {}, group_counts_[8] = {}, group_vtx_[8] = {};  // RT64's matrix groups (the hooks name objects)
    int group_depth_ = 0;
    uint32_t ungrouped_[2] = {};  // matrices loaded outside groups: modelview, projection
    void blend_matrix(uint64_t key, const Mat m, Mat out);
    float proj_change_ = 0, last_proj_change_ = 0;  // largest projection change (squared, relative) this list and last
};

// ------------------------------------------------------------------------ combiner helpers
// The N64 color combiner, (A - B) * C + D per cycle, decoded from G_SETCOMBINE.
struct CombinerCycle { uint8_t a, b, c, d, aa, ab, ac, ad; };
void decode_combiner(uint32_t hi, uint32_t lo, CombinerCycle out[2]);

// RGBA from an RGBA5551 fill color (16-bit framebuffers pack two pixels per word).
void rgba5551(uint16_t c, float out[4]);

}  // namespace n64gfx
