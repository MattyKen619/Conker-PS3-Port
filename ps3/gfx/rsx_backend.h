// RSX backend for n64gfx: draws the interpreter's output on the PS3's GPU.
//
// Each N64 color image (by RDRAM address) becomes an RSX render target at `scale` times its size,
// with its own depth buffer. Combiner modes become generated fragment programs (combiner_program.cpp,
// assembled at runtime by fpasm); textures are uploaded once per decoded image. present() draws the
// target the VI is showing onto the display buffer and flips.
#pragma once
#include <unordered_set>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <rsx/rsx.h>

#include "combiner_program.h"
#include "n64gfx.h"
#include "texture_pack.h"
#include "vram_arena.h"
#include "widescreen.h"

namespace n64gfx {

class RsxBackend : public Backend {
public:
    struct Stats {
        uint32_t draws = 0, vertices = 0, programs = 0, program_failures = 0, textures_uploaded = 0;
        uint32_t targets = 0, presents = 0, segment_waits = 0, alloc_failures = 0;
        uint32_t vram_mb = 0, vram_used_kb = 0, cmd_wraps = 0;
        uint64_t t_wait = 0, t_flip = 0, t_upload = 0;  // ticks waiting for the RSX, for flips, uploading textures
        uint64_t t_fill = 0;                              // ticks writing triangle vertices
        bool vertices_in_main = false;                    // vertex ring in main memory (else RSX memory)
        uint32_t cmd_kb = 0;                              // command buffer size
        uint32_t fb_draws = 0, fb_missing = 0;            // draws reading a frame the game drew (found / not)
        uint32_t film_frames = 0;                         // old-film scratches drawn (frames shown in grey)
        uint32_t hd_evicted = 0;                          // and freed again to make room
        uint32_t hd_textures = 0;                         // texture pack textures uploaded
        uint32_t hd_without_key = 0;                      // draws of a picture whose texture's decode found no key
        uint32_t mid_presents = 0;                        // in-between frames shown (frame interpolation)
        // Why each picture was shown: a frame drawn since it was last shown; the VI moved to another
        // frame not drawn since (an older picture); the same frame at another row.
        uint32_t present_drawn = 0, present_older = 0, present_moved = 0;
        uint32_t present_busy = 0;  // VIs left out: the last picture not on screen yet (3 ms waited)
        uint32_t present_older_waited = 0;
        uint32_t targets_freed = 0, depths_freed = 0;     // render targets and depth buffers freed                // VIs an older picture wasn't shown (waiting for its frame to be drawn)
        uint32_t scratch_max_kb = 0;                      // most frame texture copy memory one pass used
        uint64_t direct_vertices = 0;                     // read by the RSX in the stream itself
        uint32_t state_resets = 0;
        uint32_t cropped_rects = 0;                       // rectangle textures cut to the texels used
        uint32_t mip_draws = 0, mip_textures = 0;          // draws with a mipmapped texture; such textures made                        // reset_state calls
        uint32_t stream_waits = 0;                        // a stream buffer still being read when needed
        double rsx_lag_sum_ms = 0, rsx_lag_max_ms = 0;    // how long after the PPU the RSX finished each list
        uint32_t rsx_lag_n = 0;
    };

    bool init(gcmContextData* ctx, uint32_t scale);

    void draw_triangles(const DrawState& st, const OutVertex* v, size_t count) override;
    void draw_triangles_pos(const DrawState& st, const OutVertex* v, const float (*pos)[4], size_t count) override;
    // Stream buffers in RSX-mapped memory: the RSX reads their vertices until it has finished the
    // display list (both passes); before one is written again, wait for that.
    void fence_stream(uint32_t buffer);
    void wait_stream(uint32_t buffer);
    bool setup_triangles(const DrawState& st, size_t count);
    bool direct_ok_ = true;  // the self-test found the RSX reading vertices in place right  // state for a triangle draw; false: skip it
    void fill_rect(const DrawState& st, float ulx, float uly, float lrx, float lry) override;
    void tex_rect(const DrawState& st, float ulx, float uly, float lrx, float lry,
                  float s, float t, float dsdx, float dtdy, bool flip) override;
    void clear_depth(uint32_t zimg_addr, float ulx, float uly, float lrx, float lry) override;
    void begin_dl() override;
    void end_dl() override;

    // The texture cache is dropping a decoded image: free its RSX copy once the RSX is done with it.
    void texture_evicted(Texture& tex);

    struct ViInfo { uint32_t origin, width, h_start, v_start, x_scale, y_scale; };
    // Shows the color image at the VI origin (if it was drawn since the last call or the origin moved).
    // Returns true if it flipped.
    bool present(const ViInfo& vi);

    // Frame interpolation: between begin_mid() and end_mid(), frame-sized color images are drawn into
    // copies of their own (the in-between frame), other images as usual. present() then shows the
    // in-between frame of the image at the VI origin first, if there is one not yet shown.
    void begin_mid() { mid_ = true; }
    void end_mid() { mid_ = false; }
    // Saves the image at the VI origin as a PNG (N64 resolution). Waits for the RSX.
    bool screenshot(const ViInfo& vi, const char* path);

    // Checks the RSX pipeline once (clear, untextured and textured draws, a copy to the display), each
    // step fenced; logs the results. Returns false if the RSX stopped.
    bool self_test();
    // Shows a black screen (after the self-test, so its last picture isn't left up until the game's first).
    void show_black();

    Stats stats;
    std::string last_error;
    bool hung() const { return hung_; }
    // What the backend is doing right now (read by the renderer's monitor thread).
    volatile const char* phase = "idle";
    // Prints the recent operations (no locking: for diagnostics only).
    void dump_history(uint32_t count);
    void describe_targets();  // log the render targets (diagnostics)
    // The render scale (N64 pixels -> RSX pixels each way): drops every render target (waits for the RSX).
    bool set_scale(uint32_t scale);
    // Diagnostics: the frame-sized images there are targets for; one saved as a PNG at N64 resolution
    // (margins included), or its in-between copy (waits for the RSX).
    std::vector<uint32_t> frame_target_addrs() const;
    // The XMB icon (release build, ps3/host/xmb_icon.h): the title screen's logo is drawn as 32x32 texture
    // rectangles, 1:1; with logo_hashes set, the backend notes the ones whose texture is in it. Once a
    // logo of at least logo_min_tiles tiles has been drawn logo_min_lists lists running, logo_image()
    // gives it as one RGBA image (see-through around it), put together from the textures themselves.
    // A texture pack (texture_pack.h): its pictures replace textures once prepared.
    TexturePack* pack = nullptr;
    // Asks for a texture's picture from the pack, unless it's here or asked for already.
    void want_hd(uint64_t hash, const TexturePack::Job& job);
    std::vector<uint64_t> logo_hashes;
    uint32_t logo_min_tiles = 0, logo_min_lists = 15;
    bool logo_image(std::vector<uint8_t>& rgba, uint32_t* w, uint32_t* h);
    bool save_target(uint32_t addr, bool mid, const char* path);
    // The picture fills the display's height (the N64's borders cut off) instead of showing the area
    // the VI would on a TV, borders and all.
    bool fill_screen = true;
    uint32_t scale() const { return scale_; }
    // After a display list's last pass: times the RSX against the PPU (stats.rsx_lag_*).
    void mark_list_end();

    // Diagnostics: the first `fence_draws` draws and `fence_dls` display lists are each followed by
    // a fence with a timeout; if the RSX stops responding, the recent operations are logged and
    // all further RSX work is skipped (the game keeps running).
    uint32_t fence_draws = 300, fence_dls = 60;

    // Replaces the system's small command buffer (27 KB, whose wrap-around callback never returned
    // on the console) with `bytes` of IO-mapped main memory at `buffer`. Returns false if the RSX did
    // not follow.
    bool use_command_buffer(void* buffer, uint32_t bytes);

    // Moves the vertex ring to `bytes` of IO-mapped main memory at `buffer`: the PPU writes cached main
    // memory much faster than RSX memory (which it reaches uncached, one small transaction per store).
    bool use_vertex_memory(void* buffer, uint32_t bytes);
    // Goes back to the vertex ring in RSX memory (if the self-test failed with main memory).
    void use_rsx_vertex_memory();
    // The pack's pictures: bytes held, and the most they may take now.
    size_t hd_bytes() const { return hd_bytes_; }
    size_t hd_budget() const;

private:
    // The pack's pictures uploaded, by their texture's hash: kept while there's room, whatever becomes of
    // the texture (the game's textures come and go with every cut of a cutscene), so one seen again is
    // there at once.
    struct RsxTex;
    struct HdTex { RsxTex* rt = nullptr; uint32_t last_used = 0, bytes = 0; uint16_t w = 0, h = 0; };
    std::unordered_map<uint64_t, HdTex> hd_;
    std::unordered_set<uint64_t> hd_pending_;
    size_t hd_bytes_ = 0;
    // What each texture's picture is made from (want_hd), so one freed for room can be asked for again
    // when its texture is next drawn; and those that didn't fit, not to be tried again at once.
    std::unordered_map<uint64_t, TexturePack::Job> hd_jobs_;
    static uint64_t hd_key(const TexturePack::Job& j);
    std::unordered_map<uint64_t, uint32_t> hd_failed_;
    void take_hd();   // the pictures the background thread has ready
    void evict_hd(size_t need);
    SplitViews views_;  // split-screen views, for rectangles drawn over them (widescreen.h)
    WideFilm film_;
    struct LogoTile { int x, y; std::vector<uint8_t> rgba; };
    std::vector<LogoTile> logo_now_, logo_last_;  // this list's logo tiles, and the last full logo's
    uint32_t logo_lists_ = 0;                     // lists running with the full logo  // the old-film look (widescreen.h): shown in grey, its overlay spread when widened
    uint32_t frame_target_addr_ = 0;  // the frame-sized target last drawn into
    struct Target {
        uint32_t addr = 0, width_n64 = 0, height_n64 = 0, bpp = 2;
        uint32_t margin = 0, xoff = 0;  // widescreen (widescreen.h): N64 columns each side; xoff = margin * scale
        uint32_t w = 0, h = 0, pitch = 0, depth_pitch = 0;
        void* color = nullptr; uint32_t color_off = 0;
        void* depth = nullptr; uint32_t depth_off = 0;
        // Multisampling (anti-aliasing, v77): the RSX's sample layout (GCM_SURFACE_*) and the samples
        // across and down per pixel; the colour and depth images are sx * w by sy * h in memory.
        uint8_t aa = 0, sx = 1, sy = 1;
        // 3D (stereo_on, frame-sized images): 2, the right eye's picture below the left eye's (the surface
        // is h * eyes tall; h is one eye's); each draw goes to both (draw()).
        uint8_t eyes = 1;
        bool dirty = false;  // drawn since last presented
        bool mid_ready = false;  // an in-between frame copy: drawn and not yet shown
        uint32_t last_used = 0;  // the display list (dl_seq_) it was last drawn, read or shown in
        uint32_t drawn_dl = 0;   // the display list it was last drawn in
        uint32_t film_dl = ~0u;  // the display list that drew the old-film look's scratches into it
    };
    struct RsxTex { void* mem = nullptr; uint32_t off = 0, pitch = 0; uint32_t levels = 1; uint8_t dxt = 0; };  // levels > 1: swizzled, pitch 0
    // A vertex as the draw stream holds one (OutVertex): the RSX reads the stream's own vertices where
    // they are; what a draw shares is in the vertex program's constants (VpConsts).
    struct Vertex {
        float pos[4];      // clip space (rectangles: NDC, w 1)
        float uv[2];       // texels (mapped by the constants), or 0..1 with the identity mapping
        uint8_t shade[4];
        uint32_t pad;
    };
    struct VpConsts { float c[8][4]; };  // see shaders/n64gfx_vp.asm
    static constexpr u32 kVpConstCount = sizeof(VpConsts) / sizeof(float[4]);
    VpConsts vpc_{};
    bool vpc_valid_ = false;
    void set_consts(const VpConsts& k);
    void identity_consts();  // texture coordinates as they are, nothing else
    VpConsts draw_consts(const DrawState& st, float thr, const bool have[2], const float uvs[2][2], const float uvo[2][2], float half, float inv_wf);
    // The stream's records, read by the RSX in place (when the memory is mapped for it), else copied.
    void draw_vertices(const OutVertex* v, const float (*pos)[4], size_t count);
    struct Program { std::vector<uint8_t> blob; uint32_t offset = 0; bool ok = false; };
    struct KeyHash { size_t operator()(const ProgramKey& k) const { return (size_t)k.hash(); } };

    Target* bind_target(const DrawState& st);
    void bind_surface(Target* t);
    Vertex* alloc_vertices(uint32_t count, uint32_t* offset);
    const Program* program(const ProgramKey& key);
    // uvscale: texels -> 0..1; uvoff: texels added first (where a frame texture starts in its image).
    // crop (rectangles): the texture's rows y0..y1 and columns 0..x1 only, {x1, y0, y1}, clamped at
    // their edges, so smoothing at higher resolutions doesn't blend in texels the rectangle doesn't use.
    bool bind_texture(int unit, const Texture* tex, const TexMap& map, bool bilinear, float uvscale[2], float uvoff[2],
                      const DrawState* mips = nullptr, const uint32_t* crop = nullptr);
    // Mipmapped textures (TEXEL0 with its smaller versions, DrawState::mip): swizzled, all levels in
    // one allocation, by the levels' keys.
    struct MipTex { RsxTex* rt = nullptr; uint32_t levels = 0, last_used = 0, w = 0, h = 0; };
public:
    // R3: log every mipmapped draw, and dump_mips() saves the mipmapped textures those draws used,
    // read back from RSX memory (mips_<tag>_<key>.png: the levels side by side).
    bool debug_mips = false;
    // R3 experiments on mipmapped draws: 0 as usual, 1 level 0 only (max LOD 0), 2 no mipmap
    // filtering (minification LINEAR), 3 LOD bias -4, 4 no mipmapped textures at all.
    int mip_variant = 0;
    void dump_mips(const char* dir, uint32_t tag);
    // Frame-sized colour images and depth buffers in tile regions (v73); tiles_in_use(): how many now,
    // tiles_bound(): bound in all, tiles_refused(): frame-sized surfaces that got none.
    bool tiling = true;
    // The picture's look (v78): false "clean" (sharper textures on surfaces seen at an angle: anisotropic
    // filtering), true "like the N64" (16-bit colour with the RDP's dither, the video output's softness).
    bool n64_look = false;
    // Anti-aliasing (v77): the RSX's multisampling, 4 samples a pixel at 432 and 648 lines, 2 at 864, none
    // at 1080 (memory and bandwidth). Changing it starts the images again, as a resolution change does.
    bool antialias() const { return antialias_; }
    bool set_antialias(bool on);
    // 3D: depth d (each eye's farthest shift, in -1..1 units), the distance at the screen's depth
    // (convergence, in the game's w), and k: how much nearer things may come out (times d).
    void set_stereo(float depth, float convergence, float k) { stereo_d_ = depth; stereo_conv_ = convergence; stereo_k_ = k; }
    // 3D's ghost cancellation: each eye's picture less `leak` of the other's (what a 3D TV lets through),
    // the black raised to make room (0: off). Returns false if its program couldn't be made.
    bool set_ghost(float leak);
    uint32_t tiles_in_use() const { return (uint32_t)tiles_.size(); }
    uint32_t tiles_bound() const { return tiles_bound_total_; }
    uint32_t tiles_refused() const { return tiles_refused_; }
private:
    uint64_t last_mip_key_ = 0;
    uint32_t last_mip_levels_ = 0;
    std::vector<uint64_t> debug_mip_keys_;
    std::unordered_map<uint64_t, MipTex> mips_;
    bool dxt_ok_ = true;   // the self-test drew DXT1 and DXT5 textures right (a texture pack's pictures are kept so)
    bool mips_ok_ = true;  // the self-test drew them right
    RsxTex* mip_texture(const Texture* base, const DrawState& st);
    RsxTex* hd_texture(const TexturePack::Ready& r);  // a texture pack's picture, prepared in the background
    void evict_mips();
    // Render targets: one replaced (its width changed) or unused for a while (the game's buffers move
    // about between levels and scenes) has its memory freed once the RSX is past it, and a depth
    // buffer no target uses any more too. (They piled up: 20 targets, 135 MB after an hour.)
    void retire_target(std::map<uint32_t, Target>& m, std::map<uint32_t, Target>::iterator it);
    void evict_targets();
    // Frame textures (Texture::fb_addr): before a draw sets its state, each piece of a frame it reads is
    // copied by the RSX from that frame's render target into scratch memory (as the texture format
    // reads it: an intensity image's red in every channel), so it then works as an ordinary texture
    // (exact size, the tile's clamp/wrap/mirror). False if a frame isn't there (skip the draw).
    bool copy_frame_pieces(const DrawState& st, uint32_t tex_mask);
    // off_r: 3D, the same piece of the right eye's picture (0: none); draw() reads it for the right eye.
    struct FbCopy { uint32_t off = 0, off_r = 0, pitch = 0, w = 0, h = 0, ext_l = 0, texels_w = 0; } fb_copy_[2];
    uint32_t fb_right_[2] = {};  // 3D: per texture unit, the bound frame piece's right-eye copy (0: none)
    gcmTexture fb_tx_[2];        // and how it was bound (for the left eye)
    uint32_t fb_ext_[2][2] = {};  // per texture unit: N64 columns of margin read left / right of a frame piece
    void widen_frame_rect(const DrawState& st, float* ulx, float* lrx, float* s, float dsdx);
    uint8_t* scratch_ = nullptr;  // two halves, used by alternate display lists
    uint32_t scratch_size_ = 0;
    void alloc_scratch();
    // Tiled RSX memory for the frame-sized colour images and depth buffers (v73): the RSX reads and
    // writes a tile region's pixels in blocks, much faster than in rows (a "pitch" surface). The PPU and
    // the texture unit see the pixels as rows all the same (no compression). 15 regions; a size that
    // doesn't get one is allocated in rows as before.
    bool antialias_ = false;
    uint32_t dither_off_ = 0;  // the dither patterns (texture 2): magic square, then Bayer, 4 x 4 each
    void apply_look(ProgramKey& key, const DrawState& st) const;
    uint8_t three_point_units(const DrawState& st, uint8_t tex_mask, bool bilinear) const;
    void aa_layout(uint8_t* mode, uint8_t* sx, uint8_t* sy) const;
    bool reset_images(const char* why);
    std::unordered_map<void*, uint8_t> tiles_;
    uint16_t tiles_used_ = 0;
    uint32_t tiles_bound_total_ = 0, tiles_refused_ = 0;
    void* alloc_surface(uint32_t row_bytes, uint32_t rows, uint32_t* pitch);
    void free_vram(void* mem);
    struct DepthBuf { void* mem = nullptr; uint32_t off = 0, pitch = 0; };
    std::map<uint64_t, DepthBuf> depths_;  // one per target size
    const DepthBuf* depth_for(uint32_t w, uint32_t h, uint32_t sx, uint32_t sy, bool* fresh);
    static constexpr uint32_t kTsSlots = 16, kLabelTs = 106, kLabelStream = 107;  // 107, 108
    uint32_t stream_fence_[2] = {0, 0}, stream_seq_ = 0;
    uint64_t ts_kick_[16] = {};
    volatile uint64_t ts_done_[16] = {};
    volatile uint32_t ts_seen_ = 0;
    uint32_t ts_seq_ = 0, ts_read_ = 0;
    bool ts_thread_started_ = false;
    uint32_t scratch_off_ = 0, scratch_half_ = 0, scratch_pos_ = 0, scratch_done_[2] = {0, 0};
    void set_blend_depth(const DrawState& st, bool rect, bool fill);
    // offset: the vertices (Vertex) in location; pos_offset/pos_stride: their positions (the vertices'
    // own, or an in-between frame's array of 4 floats).
    void draw(uint32_t offset, uint32_t count, u8 location, uint32_t pos_offset, u8 pos_stride);
    void draw(uint32_t offset, uint32_t count) { draw(offset, count, ring_location_, offset, sizeof(Vertex)); }
    void set_scissor(const Target* t, const DrawState& st, bool view3d = false);
    void set_viewport(const Target* t, const float scale[3], const float trans[3], float widen_x = 1.0f, float shift_x = 0.0f);
    void rect(const DrawState& st, float ulx, float uly, float lrx, float lry, bool textured,
              float s, float t, float dsdx, float dtdy, bool flip, bool fill);
    void wait_label(uint32_t index, uint32_t value);
    // Restarts the command buffer at its beginning when less than 128 KB is left (see the .cpp).
    void ensure_cmd_space();
    bool test_draw(const char* name, const char* fp_source, uint32_t rgba, bool textured, const float* c7 = nullptr);
    bool three_point_ok_ = true;  // the self-test's 3-point check passed (else the N64 look keeps bilinear)
    void begin_display();
    void load_display_texture(uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch, u8 unit = 0);
    void display_quad(float fw, float fh, float u1, float v0, float v1, bool wide = false);
    bool fence(const char* what);   // false (and hung_) if the RSX did not get there within 1 s
    void note(const char* fmt, ...);  // remember an operation for the hang report
    char history_[24][160] = {};
    uint32_t history_pos_ = 0, watch_seq_ = 0, draws_fenced_ = 0, dls_fenced_ = 0;
    bool hung_ = false;
    bool notes_on_ = true;  // keep the operation history (for hang reports) while the early fences run
    // RSX state last set, to skip commands that would not change anything.
    struct StateCache {
        bool valid = false;
        uint32_t fp_offset = ~0u;
        bool vp_loaded = false;
        struct Tex { uint32_t offset = ~0u, w = 0, h = 0, pitch = 0; uint8_t filter = 0, ws = 0, wt = 0, levels = 0, dxt = 0; } tex[2];
        int sc[4] = {-1, -1, -1, -1};
        float vp[8] = {}; uint32_t vp_w = 0, vp_h = 0;
        int blend = -1; uint16_t bsrc = 0, bdst = 0;
        int dtest = -1, dwrite = -1, poff = -1;
    } cache_;
    void load_fp(const rsxFragmentProgram* fp, uint32_t offset);
    void reset_state();  // set everything again (the system may have drawn since)
    void free_textures_done();

    gcmContextData* ctx_ = nullptr;
    VramArena vram_;  // render targets, textures and fragment programs
    uint32_t scale_ = 2;
    std::map<uint32_t, Target> targets_;
    std::map<uint32_t, Target> mid_targets_;  // frame interpolation: the in-between frames
    bool mid_ = false;
    Target* bound_ = nullptr;
    std::map<uint32_t, bool> pending_depth_clear_;
    // Vertex ring in RSX memory: 4 segments, each fenced with an RSX label when left.
    uint8_t* ring_ = nullptr;
    uint32_t ring_off_ = 0, seg_size_ = 0, seg_ = 0, pos_ = 0, seq_ = 0;
    uint8_t ring_location_ = GCM_LOCATION_RSX;
    uint8_t* rsx_ring_ = nullptr;
    uint32_t rsx_ring_off_ = 0;
    uint32_t draws_since_flush_ = 0;
    // 3D (the vertex program's c[8]).
    float stereo_d_ = 0.02f, stereo_conv_ = 400.0f, stereo_k_ = 1.5f;
    bool draw_3d_ = false;  // this draw is 3D (triangles): each eye from its own side; else at the screen's depth
    float c8_[4] = {0, 0, 0, 0};
    float ghost_ = 0;                  // set_ghost's leak
    std::vector<uint8_t> ghost_blob_;  // its program (and where its code is in RSX memory)
    uint32_t ghost_off_ = 0;
    bool c8_valid_ = false;
    void load_c8(float eye_sign, bool shifted);
    uint32_t seg_fence_[4] = {};
    // Fence for freeing textures: label written at the end of every display list.
    uint32_t dl_seq_ = 0;
    std::vector<std::pair<uint32_t, RsxTex*>> free_queue_;
    std::unordered_map<ProgramKey, Program, KeyHash> programs_;
    const rsxVertexProgram* vp_ = nullptr;
    void* vp_ucode_ = nullptr;
    const Program* bound_program_ = nullptr;
    uint32_t last_present_origin_ = 0;
    uint32_t last_present_target_ = 0;
    uint64_t last_present_tb_ = 0;
    uint32_t quick_logged_ = 0;
    uint32_t older_waits_ = 0;  // VIs the VI has shown a frame not drawn since it was last shown
public:
    uint32_t lists_since_present = 0;  // the renderer counts display lists between pictures
private:
};

}  // namespace n64gfx
