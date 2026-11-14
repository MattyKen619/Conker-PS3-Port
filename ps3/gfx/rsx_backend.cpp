// RSX backend for n64gfx (see rsx_backend.h).
#include "rsx_backend.h"
#include "widescreen.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ppu_intrinsics.h>
#include <sys/systime.h>
#include <unistd.h>
#include <thread>

#include <rsx/rsx.h>

#include "fpasm/fpasm.h"
#include "miniz.h"
#include "rsxutil.h"
#include "shaders/n64gfx_vp.h"

extern "C" void ps3prof_block(int blocked) __attribute__((weak));

namespace n64gfx {

namespace {
    constexpr uint32_t kRingSize = 8 * 1024 * 1024;
    constexpr uint32_t kSegments = 4;
    constexpr uint32_t kLabelSegment = 100;  // 100..103
    constexpr uint32_t kLabelDl = 104;
    constexpr uint32_t kLabelWatch = 105;
    // Frame texture copies: two halves, each room for two whole frames' pieces at the render scale (the
    // motion blur reads most of a frame, in pieces whose rows are padded), at least 4 MB.
    uint32_t scratch_bytes(uint32_t scale) {
        const uint32_t frame = 390u * 240u * 4u * scale * scale;
        return std::max<uint32_t>(8u * 1024 * 1024, (2 * frame + 0xFFFFF) / 0x100000 * 0x100000 * 2);
    }

    uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
    uint8_t to8(float v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return (uint8_t)(v * 255.0f + 0.5f); }
    bool pot(uint32_t v) { return v && !(v & (v - 1)); }
    vu32* label(uint32_t i) { return (vu32*)gcmGetLabelAddress((u8)i); }
}

bool RsxBackend::init(gcmContextData* ctx, uint32_t scale) {
    ctx_ = ctx;
    scale_ = scale;
    ring_ = (uint8_t*)rsxMemalign(128, kRingSize);
    if (!ring_ || rsxAddressToOffset(ring_, &ring_off_) != 0) { last_error = "no RSX memory for vertices"; return false; }
    seg_size_ = kRingSize / kSegments;
    // One big block for everything else, managed by VramArena (bookkeeping in main memory).
    // As much as there is room for (the higher resolutions need about 100 MB), up to 208 MB.
    for (uint32_t mb = 208; mb >= 32; mb -= 16) {
        if (void* p = rsxMemalign(1024 * 1024, mb * 1024 * 1024)) { vram_.init((uint8_t*)p, mb * 1024 * 1024); stats.vram_mb = mb; break; }
    }
    if (!stats.vram_mb) { last_error = "no RSX memory for the arena"; return false; }
    alloc_scratch();
    std::printf("[gfx] RSX memory arena: %u MB\n", stats.vram_mb);
    {
        u32 b = 0, e = 0, c = 0;
        rsxAddressToOffset((void*)ctx_->begin, &b); rsxAddressToOffset((void*)ctx_->end, &e); rsxAddressToOffset((void*)ctx_->current, &c);
        std::printf("[gfx] command buffer: offsets 0x%X-0x%X (%u KB), current 0x%X\n", (unsigned)b, (unsigned)e,
                    (unsigned)((e - b) / 1024), (unsigned)c);
    }
    for (uint32_t i = 0; i < kSegments; i++) *label(kLabelSegment + i) = 0;
    *label(kLabelDl) = 0;
    *label(kLabelWatch) = 0;
    *label(kLabelTs) = 0;
    *label(kLabelStream) = 0;
    *label(kLabelStream + 1) = 0;
    vp_ = (const rsxVertexProgram*)n64gfx_vp_vpo;
    u32 vpsize = 0;
    rsxVertexProgramGetUCode((rsxVertexProgram*)vp_, &vp_ucode_, &vpsize);
    return true;
}

void RsxBackend::wait_label(uint32_t index, uint32_t value) {
    if (*label(index) >= value) return;
    phase = "waiting for a vertex segment";
    struct Add { uint64_t& to; uint64_t t0; ~Add() { to += __mftb() - t0; } } add{stats.t_wait, __mftb()};
    rsxFlushBuffer(ctx_);
    if (ps3prof_block) ps3prof_block(1);
    while (*label(index) < value) usleep(30);
    if (ps3prof_block) ps3prof_block(0);
}

void RsxBackend::load_fp(const rsxFragmentProgram* fp, uint32_t offset) {
    if (cache_.fp_offset == offset) return;
    rsxLoadFragmentProgramLocation(ctx_, (rsxFragmentProgram*)fp, offset, GCM_LOCATION_RSX);
    cache_.fp_offset = offset;
}

void RsxBackend::dump_history(uint32_t count) {
    uint32_t n = history_pos_ < count ? history_pos_ : count;
    if (n > 24) n = 24;
    for (uint32_t i = history_pos_ - n; i < history_pos_; i++) std::printf("[gfx]   %s\n", history_[i % 24]);
}

void RsxBackend::note(const char* fmt, ...) {
    if (!notes_on_) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(history_[history_pos_ % 24], sizeof history_[0], fmt, ap);
    va_end(ap);
    history_pos_++;
}

bool RsxBackend::fence(const char* what) {
    if (hung_) return false;
    const uint32_t v = ++watch_seq_;
    rsxSetWriteBackendLabel(ctx_, kLabelWatch, v);
    rsxFlushBuffer(ctx_);
    const uint64_t tb = sysGetTimebaseFrequency();
    const uint64_t t0 = __mftb();
    phase = "fence";
    struct Add { uint64_t& to; uint64_t t0; ~Add() { to += __mftb() - t0; } } add{stats.t_wait, t0};
    while (*label(kLabelWatch) < v) {
        if (__mftb() - t0 > tb) {  // 1 s
            hung_ = true;
            std::printf("[gfx] RSX STOPPED: no response within 1 s after %s. Last operations, oldest first:\n", what);
            uint32_t n = history_pos_ < 24 ? history_pos_ : 24;
            for (uint32_t i = history_pos_ - n; i < history_pos_; i++) std::printf("[gfx]   %s\n", history_[i % 24]);
            std::printf("[gfx] RSX drawing is off from now on; the game keeps running.\n");
            std::fflush(stdout);
            return false;
        }
        usleep(50);
    }
    return true;
}

// The command buffer is a ring: when it runs out, the context's callback should jump back to the
// start. On the console that callback never returned (graphics tests v1-v5: stuck with the buffer full
// and the RSX idle), so the backend wraps it itself: once less than 128 KB is left, wait until the
// RSX has done everything queued, write a jump to the start, and continue from there.
void RsxBackend::ensure_cmd_space() {
    const uint32_t left = (uint32_t)((uint8_t*)ctx_->end - (uint8_t*)ctx_->current);
    const uint32_t total = (uint32_t)((uint8_t*)ctx_->end - (uint8_t*)ctx_->begin);
    const uint32_t threshold = total / 4 < 128 * 1024 ? total / 4 : 128 * 1024;
    if (left > threshold) return;
    const char* saved = (const char*)phase;
    if (!fence("the command buffer wrap")) return;
    phase = "command buffer wrap";
    u32 begin_off = 0;
    rsxAddressToOffset((void*)ctx_->begin, &begin_off);
    rsxSetJumpCommand(ctx_, begin_off);
    __sync();
    gcmControlRegister volatile* ctrl = gcmGetControlRegister();
    ctrl->put = begin_off;
    const uint64_t tb = sysGetTimebaseFrequency(), t0 = __mftb();
    while (ctrl->get != begin_off) {
        if (__mftb() - t0 > tb) {
            hung_ = true;
            std::printf("[gfx] RSX STOPPED: did not follow the jump to the start of the command buffer "
                        "(get 0x%X, start 0x%X)\n", (unsigned)ctrl->get, (unsigned)begin_off);
            return;
        }
        usleep(20);
    }
    ctx_->current = ctx_->begin;
    stats.cmd_wraps++;
    phase = saved;
}

bool RsxBackend::use_command_buffer(void* buffer, uint32_t bytes) {
    u32 off = 0;
    if (rsxAddressToOffset(buffer, &off) != 0) { std::printf("[gfx] command buffer: memory not mapped for the RSX\n"); return false; }
    if (!fence("switching command buffers")) return false;
    rsxSetJumpCommand(ctx_, off);  // the last command in the old buffer
    __sync();
    gcmControlRegister volatile* ctrl = gcmGetControlRegister();
    ctrl->put = off;
    const uint64_t tb = sysGetTimebaseFrequency(), t0 = __mftb();
    while (ctrl->get != off) {
        if (__mftb() - t0 > tb) { std::printf("[gfx] command buffer: the RSX did not follow the jump\n"); hung_ = true; return false; }
        usleep(20);
    }
    ctx_->begin = (u32*)buffer;
    ctx_->current = (u32*)buffer;
    ctx_->end = (u32*)((uint8_t*)buffer + bytes - 4);
    stats.cmd_kb = bytes / 1024;
    std::printf("[gfx] command buffer: now %u KB at offset 0x%X\n", bytes / 1024, (unsigned)off);
    return fence("the new command buffer");
}

bool RsxBackend::use_vertex_memory(void* buffer, uint32_t bytes) {
    u32 off = 0;
    if (!buffer || bytes < 1024 * 1024 || rsxAddressToOffset(buffer, &off) != 0) {
        std::printf("[gfx] vertex memory: main memory not mapped for the RSX, staying in RSX memory\n");
        return false;
    }
    if (!fence("switching vertex memory")) return false;  // nothing still reads the old ring
    if (ring_location_ == GCM_LOCATION_RSX) { rsx_ring_ = ring_; rsx_ring_off_ = ring_off_; }
    ring_ = (uint8_t*)buffer;
    ring_off_ = off;
    seg_size_ = (bytes / kSegments) & ~127u;
    seg_ = pos_ = 0;
    for (uint32_t i = 0; i < kSegments; i++) seg_fence_[i] = 0;
    ring_location_ = GCM_LOCATION_CELL;
    stats.vertices_in_main = true;
    std::printf("[gfx] vertex memory: %u KB of main memory at offset 0x%X\n", bytes / 1024, (unsigned)off);
    return true;
}

void RsxBackend::use_rsx_vertex_memory() {
    if (ring_location_ == GCM_LOCATION_RSX || !rsx_ring_) return;
    if (!fence("switching vertex memory back")) return;
    ring_ = rsx_ring_;
    ring_off_ = rsx_ring_off_;
    seg_size_ = kRingSize / kSegments;
    seg_ = pos_ = 0;
    for (uint32_t i = 0; i < kSegments; i++) seg_fence_[i] = 0;
    ring_location_ = GCM_LOCATION_RSX;
    stats.vertices_in_main = false;
    std::printf("[gfx] vertex memory: back in RSX memory\n");
}

RsxBackend::Vertex* RsxBackend::alloc_vertices(uint32_t count, uint32_t* offset) {
    uint32_t bytes = count * sizeof(Vertex);
    if (bytes > seg_size_) return nullptr;
    if (pos_ + bytes > seg_size_) {
        // Leave this segment: fence it, then make sure the next one is no longer being read.
        seg_fence_[seg_] = ++seq_;
        rsxSetWriteBackendLabel(ctx_, (u8)(kLabelSegment + seg_), seq_);
        seg_ = (seg_ + 1) % kSegments;
        pos_ = 0;
        if (seg_fence_[seg_]) { stats.segment_waits++; wait_label(kLabelSegment + seg_, seg_fence_[seg_]); }
    }
    uint32_t at = seg_ * seg_size_ + pos_;
    pos_ = align_up(pos_ + bytes, 16);
    *offset = ring_off_ + at;
    return (Vertex*)(ring_ + at);
}

const RsxBackend::Program* RsxBackend::program(const ProgramKey& key) {
    auto it = programs_.find(key);
    if (it != programs_.end()) return it->second.ok ? &it->second : nullptr;
    Program& p = programs_[key];
    phase = "assembling a fragment program";
    std::string src = generate_program(key);
    if (!fpasm::assemble(src, p.blob)) { stats.program_failures++; return nullptr; }
    rsxFragmentProgram* fp = (rsxFragmentProgram*)p.blob.data();
    void* ucode = nullptr;
    u32 size = 0;
    rsxFragmentProgramGetUCode(fp, &ucode, &size);
    phase = "allocating program memory";
    void* mem = vram_.alloc(size, 128);
    if (!mem) { stats.program_failures++; stats.alloc_failures++; return nullptr; }
    std::memcpy(mem, ucode, size);
    rsxAddressToOffset(mem, &p.offset);
    p.ok = true;
    stats.programs++;
    return &p;
}

// ------------------------------------------------------------------------------- targets
RsxBackend::Target* RsxBackend::bind_target(const DrawState& st) {
    // Frame interpolation's pass: frame-sized images into their in-between copies.
    std::map<uint32_t, Target>& targets = (mid_ && st.cimg_width >= 256) ? mid_targets_ : targets_;
    auto it = targets.find(st.cimg_addr);
    Target* t;
    if (it == targets.end() || it->second.width_n64 != st.cimg_width || it->second.margin != wide_margin(st.cimg_width)) {
        if (it != targets.end()) retire_target(targets, it);  // width changed: replace
        Target nt;
        nt.addr = st.cimg_addr;
        nt.width_n64 = st.cimg_width ? st.cimg_width : 320;
        nt.height_n64 = nt.width_n64 <= 128 ? 128 : (nt.width_n64 > 320 ? 480 : 240);
        nt.bpp = st.cimg_siz == 3 ? 4 : 2;
        nt.margin = wide_margin(nt.width_n64);
        nt.xoff = nt.margin * scale_;
        nt.w = (nt.width_n64 + 2 * nt.margin) * scale_;
        nt.h = nt.height_n64 * scale_;
        nt.pitch = align_up(nt.w * 4, 64);
        nt.depth_pitch = align_up(nt.w * 4, 64);
        phase = "allocating a render target";
        nt.color = vram_.alloc(nt.pitch * nt.h, 4096);
        // The depth buffer: one for all targets of this size (the N64 has one depth image, cleared each
        // frame; frame interpolation's copies are drawn after their frames, with the same clears).
        bool fresh_depth = false;
        const DepthBuf* d = nt.color ? depth_for(nt.w, nt.h, &fresh_depth) : nullptr;
        if (!nt.color || !d) {
            vram_.free(nt.color);
            stats.alloc_failures++;
            last_error = "no RSX memory for a render target";
            return nullptr;
        }
        nt.depth = d->mem; nt.depth_off = d->off; nt.depth_pitch = d->pitch;
        rsxAddressToOffset(nt.color, &nt.color_off);
        t = &(targets[st.cimg_addr] = nt);
        stats.targets++;
        note("new target 0x%06X: N64 %ux%u (+%u each side) siz %u -> %ux%u pitch %u, color off 0x%X, depth off 0x%X",
             nt.addr, nt.width_n64, nt.height_n64, nt.margin, st.cimg_siz, nt.w, nt.h, nt.pitch, nt.color_off, nt.depth_off);
        bind_surface(t);
        rsxSetClearColor(ctx_, 0);
        rsxSetClearDepthStencil(ctx_, 0xffffff00);
        rsxSetScissor(ctx_, 0, 0, t->w, t->h);
        cache_.sc[0] = -1;
        rsxClearSurface(ctx_, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | (fresh_depth ? GCM_CLEAR_Z | GCM_CLEAR_S : 0));
    } else {
        t = &it->second;
    }
    if (bound_ != t) bind_surface(t);
    // A depth clear the game asked for (by filling its depth image) applies to the target that uses it.
    if (st.zimg_addr) {
        auto pc = pending_depth_clear_.find(st.zimg_addr);
        if (pc != pending_depth_clear_.end() && st.z_buffered()) {
            rsxSetClearDepthStencil(ctx_, 0xffffff00);
            rsxSetScissor(ctx_, 0, 0, t->w, t->h);
            cache_.sc[0] = -1;
            rsxClearSurface(ctx_, GCM_CLEAR_Z | GCM_CLEAR_S);
            pending_depth_clear_.erase(pc);
        }
    }
    t->dirty = true;
    t->last_used = dl_seq_;
    t->drawn_dl = dl_seq_;
    if (&targets == &targets_ && t->width_n64 >= 256) frame_target_addr_ = t->addr;
    if (mid_ && &targets == &mid_targets_) t->mid_ready = true;
    return t;
}

void RsxBackend::retire_target(std::map<uint32_t, Target>& m, std::map<uint32_t, Target>::iterator it) {
    if (bound_ == &it->second) bound_ = nullptr;
    if (it->second.color) {
        RsxTex* rt = new RsxTex();
        rt->mem = it->second.color;
        free_queue_.push_back({dl_seq_, rt});
    }
    m.erase(it);
    stats.targets_freed++;
}

void RsxBackend::evict_targets() {
    // Unused for 3600 display lists (2 minutes at 30 a second), except what's on the screen.
    for (std::map<uint32_t, Target>* m : {&targets_, &mid_targets_}) {
        for (auto it = m->begin(); it != m->end();) {
            auto next = std::next(it);
            if (dl_seq_ - it->second.last_used > 3600 && it->second.addr != last_present_target_) retire_target(*m, it);
            it = next;
        }
    }
    for (auto it = depths_.begin(); it != depths_.end();) {
        bool used = false;
        for (std::map<uint32_t, Target>* m : {&targets_, &mid_targets_})
            for (auto& kv : *m) used |= kv.second.depth == it->second.mem;
        if (!used) {
            RsxTex* rt = new RsxTex();
            rt->mem = it->second.mem;
            free_queue_.push_back({dl_seq_, rt});
            it = depths_.erase(it);
            stats.depths_freed++;
        } else {
            ++it;
        }
    }
}

void RsxBackend::bind_surface(Target* t) {
    gcmSurface sf;
    std::memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_A8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0] = t->color_off;
    sf.colorPitch[0] = t->pitch;
    for (int i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = t->depth_off;
    sf.depthPitch = t->depth_pitch;
    sf.type = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = (u16)t->w;
    sf.height = (u16)t->h;
    sf.x = 0;
    sf.y = 0;
    rsxSetSurface(ctx_, &sf);
    for (u8 i = 0; i < 8; i++) rsxSetViewportClip(ctx_, i, (u16)t->w, (u16)t->h);
    rsxSetZMinMaxControl(ctx_, GCM_FALSE, GCM_TRUE, GCM_FALSE);
    rsxSetColorMask(ctx_, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetColorMaskMrt(ctx_, 0);
    rsxSetShadeModel(ctx_, GCM_SHADE_MODEL_SMOOTH);
    rsxSetCullFaceEnable(ctx_, GCM_FALSE);
    rsxSetUserClipPlaneControl(ctx_, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
    bound_ = t;
}

void RsxBackend::clear_depth(uint32_t zimg_addr, float, float, float, float) {
    note("depth clear requested for 0x%06X", zimg_addr);
    pending_depth_clear_[zimg_addr] = true;
}

// ------------------------------------------------------------------------------- state
void RsxBackend::set_scissor(const Target* t, const DrawState& st, bool view3d) {
    float sx0, sx1;
    if (view3d) wide_scissor_3d(st, t->width_n64, t->margin, &sx0, &sx1);  // a split-screen view's sides widened
    else wide_scissor_rect(st, t->width_n64, t->margin, views_, &sx0, &sx1);
    int x0 = std::max(0, (int)t->xoff + (int)(sx0 * scale_)), y0 = std::max(0, (int)(st.sc_uly * scale_));
    int x1 = std::min((int)t->w, (int)t->xoff + (int)(sx1 * scale_)), y1 = std::min((int)t->h, (int)(st.sc_lry * scale_));
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    if (cache_.sc[0] == x0 && cache_.sc[1] == y0 && cache_.sc[2] == x1 - x0 && cache_.sc[3] == y1 - y0) return;
    cache_.sc[0] = x0; cache_.sc[1] = y0; cache_.sc[2] = x1 - x0; cache_.sc[3] = y1 - y0;
    rsxSetScissor(ctx_, (u16)x0, (u16)y0, (u16)(x1 - x0), (u16)(y1 - y0));
    note("  scissor %d,%d %dx%d on %ux%u", x0, y0, x1 - x0, y1 - y0, t->w, t->h);
}

void RsxBackend::set_viewport(const Target* t, const float scale[3], const float trans[3], float widen_x, float shift_x) {
    float s[4] = {scale[0] * scale_ * widen_x, -scale[1] * scale_, scale[2], 0};
    float o[4] = {(trans[0] + shift_x) * scale_ + (float)t->xoff, trans[1] * scale_, trans[2], 0};
    const float key[8] = {s[0], s[1], s[2], 0, o[0], o[1], o[2], 0};
    if (cache_.vp_w == t->w && cache_.vp_h == t->h && std::memcmp(cache_.vp, key, sizeof key) == 0) return;
    std::memcpy(cache_.vp, key, sizeof key); cache_.vp_w = t->w; cache_.vp_h = t->h;
    rsxSetViewport(ctx_, 0, 0, (u16)t->w, (u16)t->h, 0.0f, 1.0f, s, o);
}

void RsxBackend::set_blend_depth(const DrawState& st, bool rect, bool fill) {
    // Blending: the last blender cycle, when it blends (DrawState::blends; otherwise the N64 writes the
    // color as is).
    bool blend = false;
    u16 src = GCM_ONE, dst = GCM_ZERO;
    if (!fill && st.cycle() != CYC_COPY && st.blends()) {
        int sh = st.cycle() == CYC_2 ? -2 : 0;
        uint32_t l = st.om_l;
        uint32_t P = (l >> (30 + sh)) & 3, A = (l >> (26 + sh)) & 3, M = (l >> (22 + sh)) & 3, B = (l >> (18 + sh)) & 3;
        if (P == 0 && M == 1) {
            blend = true;
            src = A == 3 ? GCM_ZERO : GCM_SRC_ALPHA;
            dst = B == 0 ? GCM_ONE_MINUS_SRC_ALPHA : B == 2 ? GCM_ONE : B == 3 ? GCM_ZERO : GCM_ONE_MINUS_SRC_ALPHA;
        } else if (P == 1 && M == 0) {
            blend = true;
            dst = A == 3 ? GCM_ZERO : GCM_SRC_ALPHA;  // memory weighted by A
            src = B == 0 ? GCM_ONE_MINUS_SRC_ALPHA : B == 2 ? GCM_ONE : B == 3 ? GCM_ZERO : GCM_ONE_MINUS_SRC_ALPHA;
        }
    }
    if (cache_.blend != (int)blend) { rsxSetBlendEnable(ctx_, blend ? GCM_TRUE : GCM_FALSE); cache_.blend = blend; }
    if (blend && (cache_.bsrc != src || cache_.bdst != dst)) {
        rsxSetBlendFunc(ctx_, src, dst, src, dst);
        rsxSetBlendEquation(ctx_, GCM_FUNC_ADD, GCM_FUNC_ADD);
        cache_.bsrc = src; cache_.bdst = dst;
    }
    // Depth.
    bool test, write;
    if (fill || st.cycle() == CYC_COPY) { test = false; write = false; }
    // Rectangles are hidden by depth with the primitive's depth (the game's dust and sparks: v44
    // drew texture rectangles without the test, and dust showed through walls).
    else if (rect) { test = st.z_compare() && st.z_source_prim(); write = st.z_update() && st.z_source_prim(); }
    else { test = st.z_buffered() && st.z_compare(); write = st.z_buffered() && st.z_update(); }
    if (cache_.dtest != (int)test) {
        rsxSetDepthTestEnable(ctx_, test ? GCM_TRUE : GCM_FALSE);
        if (test) rsxSetDepthFunc(ctx_, GCM_LEQUAL);
        cache_.dtest = test;
    }
    if (cache_.dwrite != (int)write) { rsxSetDepthWriteEnable(ctx_, write ? GCM_TRUE : GCM_FALSE); cache_.dwrite = write; }
    bool decal = test && st.z_mode() == 3;
    if (cache_.poff != (int)decal) {
        rsxSetPolygonOffsetFillEnable(ctx_, decal ? GCM_TRUE : GCM_FALSE);
        if (decal) rsxSetPolygonOffset(ctx_, -1.0f, -2.0f);
        cache_.poff = decal;
    }
}

bool RsxBackend::copy_frame_pieces(const DrawState& st, uint32_t tex_mask) {
    bool any = false;
    for (int u = 0; u < 2; u++) any |= st.tex[u] && st.tex[u]->fb_addr && (tex_mask & (1u << u));
    if (!any) return true;
    if (!scratch_) { stats.fb_missing++; return false; }
    for (int u = 0; u < 2; u++) {
        const Texture* tex = st.tex[u];
        if (!tex || !tex->fb_addr || !(tex_mask & (1u << u))) continue;
        // In frame interpolation's pass, an image drawn in it is read from its in-between copy.
        auto it = targets_.find(tex->fb_addr);
        if (mid_) {
            auto m = mid_targets_.find(tex->fb_addr);
            if (m != mid_targets_.end() && m->second.mid_ready) it = m;
        }
        if (it == targets_.end()) { stats.fb_missing++; return false; }
        Target& f = it->second;
        f.last_used = dl_seq_;
        FbCopy& c = fb_copy_[u];
        // Widescreen: a piece at the frame's side may be read with the margin beside it (fb_ext_).
        // (Up to the widened frame's sides: a piece may start a little inside the frame, or end past it.)
        const int fx = tex->fb_x, fr = (int)f.width_n64 - (fx + (int)tex->width);
        const uint32_t el = (uint32_t)std::max(0, std::min((int)fb_ext_[u][0], (int)f.margin + fx));
        const uint32_t er = (uint32_t)std::max(0, std::min((int)fb_ext_[u][1], (int)f.margin + fr));
        c.ext_l = el; c.texels_w = tex->width + el + er;
        c.w = c.texels_w * scale_;
        c.h = tex->height * scale_;
        c.pitch = align_up(c.w * 4, 64);
        const uint32_t bytes = align_up(c.pitch * c.h, 128);
        const uint32_t half = scratch_size_ / 2;
        if (c.w == 0 || c.h == 0 || c.w > 4096 || c.h > 4096 || scratch_pos_ + bytes > half) { stats.fb_missing++; return false; }
        c.off = scratch_off_ + scratch_half_ * half + scratch_pos_;
        scratch_pos_ += bytes;
        stats.scratch_max_kb = std::max(stats.scratch_max_kb, scratch_pos_ / 1024);
        // Render into the scratch piece: a quad sampling the frame's target.
        ensure_cmd_space();
        phase = "copying a piece of a frame";
        gcmSurface sf;
        std::memset(&sf, 0, sizeof sf);
        sf.colorFormat = GCM_SURFACE_A8R8G8B8;
        sf.colorTarget = GCM_SURFACE_TARGET_0;
        sf.colorLocation[0] = GCM_LOCATION_RSX;
        sf.colorOffset[0] = c.off;
        sf.colorPitch[0] = c.pitch;
        for (int i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
        sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;  // not used (depth off), but the surface needs one
        sf.depthLocation = GCM_LOCATION_RSX;
        sf.depthOffset = f.depth_off;
        sf.depthPitch = f.depth_pitch;
        sf.type = GCM_SURFACE_TYPE_LINEAR;
        sf.antiAlias = GCM_SURFACE_CENTER_1;
        sf.width = (u16)c.w;
        sf.height = (u16)c.h;
        rsxSetSurface(ctx_, &sf);
        bound_ = nullptr;
        for (u8 i = 0; i < 8; i++) rsxSetViewportClip(ctx_, i, (u16)c.w, (u16)c.h);
        rsxSetColorMask(ctx_, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
        rsxSetScissor(ctx_, 0, 0, (u16)c.w, (u16)c.h);
        rsxSetBlendEnable(ctx_, GCM_FALSE);
        rsxSetDepthTestEnable(ctx_, GCM_FALSE);
        rsxSetDepthWriteEnable(ctx_, GCM_FALSE);
        rsxSetPolygonOffsetFillEnable(ctx_, GCM_FALSE);
        {
            float vs[4] = {c.w * 0.5f, c.h * -0.5f, 0.5f, 0}, vo[4] = {c.w * 0.5f, c.h * 0.5f, 0.5f, 0};
            rsxSetViewport(ctx_, 0, 0, (u16)c.w, (u16)c.h, 0.0f, 1.0f, vs, vo);
        }
        ProgramKey key;
        key.mode = ProgramMode::Copy;
        key.tex_mask = 1;
        const Program* p = program(key);
        if (!p) { stats.fb_missing++; return false; }
        load_fp((rsxFragmentProgram*)p->blob.data(), p->offset);
        // The frame's target as texture 0, read as the tile's format reads the image.
        gcmTexture tx;
        std::memset(&tx, 0, sizeof tx);
        tx.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
        tx.mipmap = 1;
        tx.dimension = GCM_TEXTURE_DIMS_2D;
        tx.cubemap = GCM_FALSE;
        const bool intensity = tex->fmt == 4 || tex->fmt == 3;  // I, IA: the RDP wrote the value as the color
        const u32 rc = intensity ? GCM_TEXTURE_REMAP_COLOR_R : 0;
        tx.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
                    (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
                    (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
                    ((intensity ? GCM_TEXTURE_REMAP_TYPE_REMAP : GCM_TEXTURE_REMAP_TYPE_ONE) << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
                    ((intensity ? rc : GCM_TEXTURE_REMAP_COLOR_B) << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
                    ((intensity ? rc : GCM_TEXTURE_REMAP_COLOR_G) << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
                    (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
                    ((intensity ? rc : GCM_TEXTURE_REMAP_COLOR_A) << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
        tx.width = (u16)f.w;
        tx.height = (u16)f.h;
        tx.depth = 1;
        tx.location = GCM_LOCATION_RSX;
        tx.pitch = f.pitch;
        tx.offset = f.color_off;
        rsxInvalidateTextureCache(ctx_, GCM_INVALIDATE_TEXTURE);  // the frame may have just been drawn
        rsxLoadTexture(ctx_, 0, &tx);
        rsxTextureControl(ctx_, 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
        rsxTextureFilter(ctx_, 0, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        rsxTextureWrapMode(ctx_, 0, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0,
                           GCM_TEXTURE_ZFUNC_LESS, 0);
        // The quad: the piece's pixels in the frame's target (2x).
        const float u0 = (float)((int)f.xoff + (tex->fb_x - (int)el) * (int)scale_) / f.w,
                    u1 = (float)((int)f.xoff + (tex->fb_x + (int)tex->width + (int)er) * (int)scale_) / f.w;
        const float v0 = (float)(tex->fb_y * (int)scale_) / f.h, v1 = (float)((tex->fb_y + (int)tex->height) * (int)scale_) / f.h;
        uint32_t voff;
        Vertex* v = alloc_vertices(6, &voff);
        if (!v) { stats.fb_missing++; return false; }
        const float px[4] = {-1, 1, 1, -1}, py[4] = {1, 1, -1, -1};
        const float pu[4] = {u0, u1, u1, u0}, pv[4] = {v0, v0, v1, v1};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++) {
            Vertex d;
            std::memset(&d, 0, sizeof d);
            d.pos[0] = px[order[k]]; d.pos[1] = py[order[k]]; d.pos[2] = 0; d.pos[3] = 1;
            d.uv[0] = pu[order[k]]; d.uv[1] = pv[order[k]];
            d.shade[0] = d.shade[1] = d.shade[2] = d.shade[3] = 255;
            std::memcpy(&v[k], &d, sizeof d);
        }
        identity_consts();
        draw(voff, 6);
        stats.fb_draws++;
        // Everything set here differs from what the state cache remembers.
        const bool vp = cache_.vp_loaded;
        cache_ = StateCache{};
        cache_.vp_loaded = vp;
    }
    rsxInvalidateTextureCache(ctx_, GCM_INVALIDATE_TEXTURE);  // the pieces were just drawn
    return true;
}

// The system (the XMB's in-game menu, from the PS button, and its notices) draws with the RSX too,
// with programs and state of its own: after it, the vertex program, its constants and the rest
// can be the system's (the picture after the PS button was flat, stretched polygons). So every
// display list starts by setting everything again, rather than trusting what was last set.
void RsxBackend::reset_state() {
    cache_ = StateCache{};  // the vertex program too
    vpc_valid_ = false;
    bound_program_ = nullptr;
    bound_ = nullptr;
    rsxSetAlphaTestEnable(ctx_, GCM_FALSE);
    rsxSetStencilTestEnable(ctx_, GCM_FALSE);
    rsxSetTwoSidedStencilTestEnable(ctx_, GCM_FALSE);
    rsxSetLogicOpEnable(ctx_, GCM_FALSE);
    rsxSetDepthBoundsTestEnable(ctx_, GCM_FALSE);
    // No near or far clipping, depth clamped instead: Conker's microcode is F3DEXBG.NoN, which draws a
    // model closer to the camera than the near plane (n64gfx clips only behind the camera).
    rsxSetZMinMaxControl(ctx_, GCM_FALSE, GCM_TRUE, GCM_FALSE);
    rsxSetDitherEnable(ctx_, GCM_FALSE);
    rsxSetFrontPolygonMode(ctx_, GCM_POLYGON_MODE_FILL);
    rsxSetBackPolygonMode(ctx_, GCM_POLYGON_MODE_FILL);
    rsxSetFrontFace(ctx_, GCM_FRONTFACE_CCW);
    rsxSetPolygonSmoothEnable(ctx_, GCM_FALSE);
    rsxSetRestartIndexEnable(ctx_, GCM_FALSE);
    rsxSetFrequencyDividerOperation(ctx_, 0);
    rsxSetFragmentProgramGammaEnable(ctx_, GCM_FALSE);
    rsxSetPointSpriteControl(ctx_, GCM_FALSE, 0, 0);
    rsxSetAntialiasingControl(ctx_, GCM_FALSE, GCM_FALSE, GCM_FALSE, 0xffff);
    rsxSetTwoSideLightEnable(ctx_, GCM_FALSE);
    rsxSetBlendEnableMrt(ctx_, GCM_FALSE, GCM_FALSE, GCM_FALSE);
    rsxSetBlendColor(ctx_, 0, 0);
    rsxSetZCullEnable(ctx_, GCM_FALSE, GCM_FALSE);
    rsxSetTransformBranchBits(ctx_, 0);
    for (u8 a = 0; a < 16; a++)  // every vertex array off; draw() binds the three it uses
        rsxBindVertexArrayAttrib(ctx_, a, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxInvalidateTextureCache(ctx_, GCM_INVALIDATE_TEXTURE);
    rsxInvalidateVertexCache(ctx_);
    stats.state_resets++;
}

void RsxBackend::begin_dl() {
    reset_state();
    film_.next_list();
    views_.next_list();
    // The other half of the scratch memory, once the RSX is done with the display list that used it.
    scratch_half_ ^= 1;
    scratch_pos_ = 0;
    if (scratch_done_[scratch_half_] && *label(kLabelDl) < scratch_done_[scratch_half_]) {
        stats.segment_waits++;
        wait_label(kLabelDl, scratch_done_[scratch_half_]);
    }
    scratch_done_[scratch_half_] = 0;
}

namespace {
// The RSX's swizzled layout: x and y bits interleaved (x first) as far as the smaller side goes, then
// the larger side's remaining bits.
inline uint32_t swizzle(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {
    uint32_t off = 0, bit = 0;
    const uint32_t m = lw < lh ? lw : lh;
    for (uint32_t i = 0; i < m; i++) { off |= ((x >> i) & 1) << bit++; off |= ((y >> i) & 1) << bit++; }
    for (uint32_t i = m; i < lw; i++) off |= ((x >> i) & 1) << bit++;
    for (uint32_t i = m; i < lh; i++) off |= ((y >> i) & 1) << bit++;
    return off;
}
inline uint32_t log2u(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }
}

RsxBackend::RsxTex* RsxBackend::mip_texture(const Texture* base, const DrawState& st) {
    if (!base->rgba || base->fb_addr) return nullptr;
    uint64_t key = base->hash;
    for (uint32_t k = 0; k < st.mip_levels; k++) {
        if (!st.mip[k]->rgba) return nullptr;
        key = (key ^ st.mip[k]->hash) * 0x9E3779B97F4A7C15ull + k;
    }
    MipTex& m = mips_[key];
    m.last_used = dl_seq_;
    last_mip_key_ = key;
    if (m.rt) return m.rt;
    phase = "uploading a mipmapped texture";
    struct Add { uint64_t& to; uint64_t t0; ~Add() { to += __mftb() - t0; } } add{stats.t_upload, __mftb()};
    const uint32_t levels = 1 + st.mip_levels;
    size_t bytes = 0;
    for (uint32_t k = 0; k < levels; k++) {
        const Texture* t = k ? st.mip[k - 1] : base;
        bytes += (size_t)t->width * t->height * 4;
    }
    RsxTex* rt = new RsxTex();
    rt->mem = vram_.alloc(bytes, 128);
    if (!rt->mem) { delete rt; mips_.erase(key); stats.alloc_failures++; return nullptr; }
    rsxAddressToOffset(rt->mem, &rt->off);
    rt->pitch = 0;
    uint8_t* d = (uint8_t*)rt->mem;
    for (uint32_t k = 0; k < levels; k++) {  // each level right after the one before
        const Texture* t = k ? st.mip[k - 1] : base;
        const uint32_t lw = log2u(t->width), lh = log2u(t->height);
        for (uint32_t y = 0; y < t->height; y++) {
            const uint8_t* s = t->rgba + (size_t)y * t->width * 4;
            for (uint32_t x = 0; x < t->width; x++, s += 4) {
                uint8_t* p = d + swizzle(x, y, lw, lh) * 4;
                p[0] = s[3]; p[1] = s[0]; p[2] = s[1]; p[3] = s[2];  // RGBA -> A8R8G8B8
            }
        }
        d += (size_t)t->width * t->height * 4;
    }
    m.rt = rt;
    m.levels = levels;
    m.w = base->width; m.h = base->height;
    stats.mip_textures++;
    stats.textures_uploaded++;
    rsxInvalidateTextureCache(ctx_, GCM_INVALIDATE_TEXTURE);
    return rt;
}

void RsxBackend::evict_mips() {  // the ones unused for a while (their memory is freed once the RSX is past them)
    for (auto it = mips_.begin(); it != mips_.end();) {
        if (dl_seq_ - it->second.last_used > 300) {
            if (it->second.rt) free_queue_.push_back({dl_seq_, it->second.rt});
            it = mips_.erase(it);
        } else {
            ++it;
        }
    }
}

bool RsxBackend::bind_texture(int unit, const Texture* tex, const TexMap& map, bool bilinear, float uvscale[2], float uvoff[2],
                              const DrawState* mips, const uint32_t* crop) {
    uvoff[0] = uvoff[1] = 0;
    RsxTex fb_rt;
    RsxTex* rt = (RsxTex*)tex->backend_data;
    uint32_t levels = 1;
    if (unit == 0) last_mip_levels_ = 0;
    if (mips && mips->mip_levels && unit == 0 && !tex->fb_addr && mips_ok_ && mip_variant != 4) {
        if (RsxTex* m = mip_texture(tex, *mips)) { rt = m; levels = 1 + mips->mip_levels; stats.mip_draws++; last_mip_levels_ = levels; }
    }
    if (tex->fb_addr) {  // copied by copy_frame_pieces: 2x the texture's size
        const FbCopy& c = fb_copy_[unit & 1];
        fb_rt.off = c.off; fb_rt.pitch = c.pitch;
        rt = &fb_rt;
    }
    if (!rt) {
        phase = "uploading a texture";
        struct Add { uint64_t& to; uint64_t t0; ~Add() { to += __mftb() - t0; } } add{stats.t_upload, __mftb()};
        rt = new RsxTex();
        rt->pitch = align_up(tex->width * 4, 64);
        rt->mem = vram_.alloc(rt->pitch * tex->height, 128);
        if (!rt->mem) { delete rt; stats.alloc_failures++; return false; }
        rsxAddressToOffset(rt->mem, &rt->off);
        // RGBA bytes -> A8R8G8B8 (big-endian ARGB words).
        for (uint32_t y = 0; y < tex->height; y++) {
            const uint8_t* s = tex->rgba + (size_t)y * tex->width * 4;
            uint8_t* d = (uint8_t*)rt->mem + (size_t)y * rt->pitch;
            for (uint32_t x = 0; x < tex->width; x++, s += 4, d += 4) { d[0] = s[3]; d[1] = s[0]; d[2] = s[1]; d[3] = s[2]; }
        }
        const_cast<Texture*>(tex)->backend_data = rt;
        stats.textures_uploaded++;
        rsxInvalidateTextureCache(ctx_, GCM_INVALIDATE_TEXTURE);
    }
    gcmTexture tx;
    std::memset(&tx, 0, sizeof tx);
    tx.format = levels > 1 ? GCM_TEXTURE_FORMAT_A8R8G8B8 : GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
    tx.mipmap = (u8)levels;
    tx.dimension = GCM_TEXTURE_DIMS_2D;
    tx.cubemap = GCM_FALSE;
    tx.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
                (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
                (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
                (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
    // A frame piece's copy is twice the size (the render scale); coordinates stay in N64 texels.
    uint32_t tw = tex->fb_addr ? fb_copy_[unit & 1].w : tex->width, th = tex->fb_addr ? fb_copy_[unit & 1].h : tex->height;
    uint32_t crop_off = 0, crop_y0 = 0;
    const bool cropped = crop && !tex->fb_addr && levels == 1 && crop[0] >= 1 && crop[0] <= tw && crop[2] > crop[1] && crop[2] <= th &&
                         (crop[1] * rt->pitch) % 128 == 0 && (crop[0] < tw || crop[1] > 0 || crop[2] < th);
    if (cropped) { tw = crop[0]; crop_y0 = crop[1]; th = crop[2] - crop[1]; crop_off = crop[1] * rt->pitch; stats.cropped_rects++; }
    tx.width = (u16)tw;
    tx.height = (u16)th;
    tx.depth = 1;
    tx.location = GCM_LOCATION_RSX;
    tx.pitch = rt->pitch;
    tx.offset = rt->off + crop_off;
    u8 f = bilinear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
    // Mipmapped: blended between the two nearest levels, as the N64's LOD fraction does.
    u8 fmin = levels > 1 ? (bilinear ? GCM_TEXTURE_LINEAR_MIPMAP_LINEAR : GCM_TEXTURE_NEAREST_MIPMAP_LINEAR) : f;
    if (levels > 1 && mip_variant == 2) fmin = f;
    const u16 maxlod = levels > 1 && mip_variant == 1 ? 0 : (u16)((levels - 1) << 8);
    const u16 bias = levels > 1 && mip_variant == 3 ? (u16)((-4 * 256) & 0x1fff) : 0;
    auto wrap = [](bool clamp, bool mirror, uint32_t size) -> u8 {
        if (clamp || !pot(size)) return GCM_TEXTURE_CLAMP_TO_EDGE;
        return mirror ? GCM_TEXTURE_MIRRORED_REPEAT : GCM_TEXTURE_REPEAT;
    };
    // The RDP clamps at the tile's edge, then wraps by its mask: a tile larger than the texture it masks
    // (the swimming air bubbles' mask: 32 rows, the tile 224) repeats it up to the clamp, so it repeats.
    const bool cs = map.clamp_s && map.clamp_max_s < (float)tex->width + 0.5f;
    const bool ct = map.clamp_t && map.clamp_max_t < (float)tex->height + 0.5f;
    const u8 ws = wrap(cs || cropped, map.mirror_s, tex->width), wt = wrap(ct || cropped, map.mirror_t, tex->height);
    StateCache::Tex& c = cache_.tex[unit & 1];
    if (c.offset != tx.offset || c.w != tw || c.h != th || c.pitch != rt->pitch || c.filter != f ||
        c.ws != ws || c.wt != wt || c.levels != levels || mip_variant) {
        note("  texture %d: %ux%u pitch %u off 0x%X clamp %d/%d mirror %d/%d", unit, tex->width, tex->height, rt->pitch, rt->off,
             map.clamp_s, map.clamp_t, map.mirror_s, map.mirror_t);
        rsxLoadTexture(ctx_, (u8)unit, &tx);
        rsxTextureControl(ctx_, (u8)unit, GCM_TRUE, 0 << 8, maxlod, GCM_TEXTURE_MAX_ANISO_1);
        rsxTextureFilter(ctx_, (u8)unit, bias, fmin, f, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        rsxTextureWrapMode(ctx_, (u8)unit, ws, wt, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
        c.offset = tx.offset; c.w = tw; c.h = th; c.pitch = rt->pitch; c.filter = f; c.ws = ws; c.wt = wt; c.levels = (u8)levels;
    }
    uvscale[0] = 1.0f / tex->width;
    uvscale[1] = 1.0f / tex->height;
    if (cropped) { uvscale[0] = 1.0f / tw; uvscale[1] = 1.0f / th; uvoff[1] = -(float)crop_y0; }
    if (tex->fb_addr) {  // with the margin beside it, if copied so
        const FbCopy& fc = fb_copy_[unit & 1];
        uvscale[0] = 1.0f / fc.texels_w;
        uvoff[0] = (float)fc.ext_l;
    }
    return true;
}

void RsxBackend::set_consts(const VpConsts& k) {
    if (vpc_valid_ && std::memcmp(&k, &vpc_, sizeof k) == 0) return;
    vpc_ = k;
    vpc_valid_ = true;
    rsxLoadVertexProgramParameterBlock(ctx_, 0, 7, &k.c[0][0]);  // 7 constants (SetVertexProgramConstants counts floats)
}

void RsxBackend::identity_consts() {
    VpConsts k{};
    k.c[0][0] = 1; k.c[0][2] = 1;
    k.c[1][0] = 1; k.c[1][2] = 1;
    k.c[6][0] = k.c[6][1] = k.c[6][2] = k.c[6][3] = 1;
    set_consts(k);
}

// A draw's constants: each texture's mapping from texels (u = s * a + b), the combiner's colors, the
// alpha threshold and K5, and the widescreen widening of x.
RsxBackend::VpConsts RsxBackend::draw_consts(const DrawState& st, float thr, const bool have[2], const float uvs[2][2], const float uvo[2][2],
                                             float half, float inv_wf) {
    VpConsts k{};
    for (int u = 0; u < 2; u++) {
        if (!have[u]) continue;
        const TexMap& m = st.tmap[u];
        k.c[u][0] = m.shift_s * uvs[u][0]; k.c[u][1] = (half - m.offset_s + uvo[u][0]) * uvs[u][0];
        k.c[u][2] = m.shift_t * uvs[u][1]; k.c[u][3] = (half - m.offset_t + uvo[u][1]) * uvs[u][1];
    }
    // Colors as the old vertex bytes were (8 bits each).
    for (int i = 0; i < 4; i++) { k.c[2][i] = to8(st.prim[i]) / 255.0f; k.c[3][i] = to8(st.env[i]) / 255.0f; }
    k.c[4][0] = st.prim_lod_frac; k.c[4][1] = st.fog[0]; k.c[4][2] = st.fog[1]; k.c[4][3] = st.fog[2];
    k.c[5][0] = thr; k.c[5][1] = st.k5; k.c[5][2] = st.fog[3];
    k.c[6][0] = inv_wf; k.c[6][1] = k.c[6][2] = k.c[6][3] = 1;
    return k;
}

void RsxBackend::draw(uint32_t offset, uint32_t count, u8 loc, uint32_t pos_offset, u8 pos_stride) {
    const u8 stride = sizeof(Vertex);
    rsxBindVertexArrayAttrib(ctx_, GCM_VERTEX_ATTRIB_POS, 0, pos_offset, pos_stride, 4, GCM_VERTEX_DATA_TYPE_F32, loc);
    rsxBindVertexArrayAttrib(ctx_, GCM_VERTEX_ATTRIB_TEX0, 0, offset + 16, stride, 2, GCM_VERTEX_DATA_TYPE_F32, loc);
    rsxBindVertexArrayAttrib(ctx_, GCM_VERTEX_ATTRIB_COLOR0, 0, offset + 24, stride, 4, GCM_VERTEX_DATA_TYPE_U8, loc);
    if (!cache_.vp_loaded) {
        rsxLoadVertexProgram(ctx_, vp_, vp_ucode_);
        // The old vertex format's other arrays: off.
        for (u8 a : {GCM_VERTEX_ATTRIB_TEX1, GCM_VERTEX_ATTRIB_TEX2, GCM_VERTEX_ATTRIB_TEX3, GCM_VERTEX_ATTRIB_TEX4})
            rsxBindVertexArrayAttrib(ctx_, a, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        cache_.vp_loaded = true;
        // The draw's constants were set before the program was loaded (setup_*): send them again
        // after it (the first draw after reset_state used to get the identity constants instead:
        // texture coordinates in texels, 32 times too dense for a 32-texel texture).
        if (vpc_valid_) rsxLoadVertexProgramParameterBlock(ctx_, 0, 7, &vpc_.c[0][0]);
    }
    if (!vpc_valid_) identity_consts();
    rsxInvalidateVertexCache(ctx_);
    phase = "issuing a draw";
    rsxDrawVertexArray(ctx_, GCM_TYPE_TRIANGLES, 0, count);
    phase = "back in the interpreter";
    stats.draws++;
    stats.vertices += count;
    // Let the RSX start on what is queued instead of waiting for the end of the display list.
    if (++draws_since_flush_ >= 32) { draws_since_flush_ = 0; rsxFlushBuffer(ctx_); }
    if (draws_fenced_ >= fence_draws) notes_on_ = false;
    if (draws_fenced_ < fence_draws) {
        draws_fenced_++;
        char what[48];
        std::snprintf(what, sizeof what, "draw #%u (%u vertices)", stats.draws, count);
        fence(what);
    }
}

void RsxBackend::draw_vertices(const OutVertex* in, const float (*pos)[4], size_t count) {
    static_assert(sizeof(Vertex) == sizeof(OutVertex) && sizeof(OutVertex) == 32, "the RSX reads OutVertex as Vertex");
    // In memory mapped for the RSX (the SPU's stream): read where it is.
    u32 off = 0, poff = 0;
    if (direct_ok_ && ring_location_ == GCM_LOCATION_CELL && rsxAddressToOffset((void*)in, &off) == 0 &&
        (!pos || rsxAddressToOffset((void*)pos, &poff) == 0)) {
        stats.direct_vertices += count;
        draw(off, (uint32_t)count, GCM_LOCATION_CELL, pos ? poff : off, pos ? 16 : sizeof(Vertex));
        return;
    }
    // Else copied into the vertex ring, in segment-sized pieces.
    size_t done = 0;
    while (done < count) {
        uint32_t n = (uint32_t)std::min<size_t>(count - done, (seg_size_ / sizeof(Vertex)) / 3 * 3);
        uint32_t o;
        ensure_cmd_space();
        Vertex* v = alloc_vertices(n, &o);
        if (!v) return;
        const uint64_t tf = __mftb();
        std::memcpy(v, in + done, n * sizeof(Vertex));
        if (pos) for (uint32_t i = 0; i < n; i++) std::memcpy(v[i].pos, pos[done + i], 16);
        stats.t_fill += __mftb() - tf;
        draw(o, n);
        done += n;
    }
}

void RsxBackend::fence_stream(uint32_t buffer) {
    if (hung_ || buffer > 1) return;
    ensure_cmd_space();
    stream_fence_[buffer] = ++stream_seq_;
    rsxSetWriteBackendLabel(ctx_, (u8)(kLabelStream + buffer), stream_seq_);
    rsxFlushBuffer(ctx_);
}

void RsxBackend::wait_stream(uint32_t buffer) {
    if (hung_ || buffer > 1 || !stream_fence_[buffer]) return;
    if (*label(kLabelStream + buffer) >= stream_fence_[buffer]) return;
    stats.stream_waits++;
    wait_label(kLabelStream + buffer, stream_fence_[buffer]);
}

// ------------------------------------------------------------------------------- drawing
void RsxBackend::draw_triangles(const DrawState& st, const OutVertex* in, size_t count) {
    if (count < 3 || hung_) return;
    if (debug_mips && st.mip_levels)
        std::printf("[mip]   %u vertices; first: pos %.2f %.2f %.2f %.2f st %.2f %.2f\n", (unsigned)count, in[0].x, in[0].y, in[0].z, in[0].w, in[0].s, in[0].t);
    if (setup_triangles(st, count)) draw_vertices(in, nullptr, count);
}

void RsxBackend::draw_triangles_pos(const DrawState& st, const OutVertex* in, const float (*pos)[4], size_t count) {
    if (count < 3 || hung_) return;
    if (setup_triangles(st, count)) draw_vertices(in, pos, count);
}

bool RsxBackend::setup_triangles(const DrawState& st, size_t count) {
    ensure_cmd_space();
    phase = "triangles: setup";
    ProgramKey key = make_program_key(st, false);
    if (!copy_frame_pieces(st, key.tex_mask)) return false;  // a frame we don't have: draw nothing rather than garbage
    Target* t = bind_target(st);
    if (!t) return false;
    const Program* p = program(key);
    note("triangles (%u vertices) target 0x%06X: program mode %d comb %06X:%08X tex %d atest %d fog %d %s, om %06X:%08X",
         (unsigned)count, st.cimg_addr, (int)key.mode, key.combine_hi, key.combine_lo, key.tex_mask, key.alpha_test, key.fog,
         p ? "ok" : "FAILED", st.om_h, st.om_l);
    if (!p) return false;
    load_fp((rsxFragmentProgram*)p->blob.data(), p->offset);
    set_scissor(t, st, true);
    if (wide_split_view(st, t->width_n64, t->margin)) views_.note(st);
    // Widescreen: 3D with a viewport covering the frame isn't clipped at the 4:3 edges. Its x is
    // divided by how much wider the picture is and the viewport widened as much, which puts every
    // point where it was but moves the clip planes out to the margins' edges.
    const bool widen = wide_viewport(st, t->width_n64, t->margin);
    const float wf = widen ? (float)(t->width_n64 + 2 * t->margin) / (float)t->width_n64 : 1.0f, inv_wf = 1.0f / wf;
    set_viewport(t, st.vp_scale, st.vp_trans, wf, wide_view_shift(st, t->width_n64, t->margin));
    set_blend_depth(st, false, false);
    const bool bilinear = st.cycle() != CYC_COPY && ((st.om_h >> 12) & 3) >= 2;
    float uvs[2][2] = {{0, 0}, {0, 0}}, uvo[2][2] = {{0, 0}, {0, 0}};
    bool have[2] = {false, false};
    for (int u = 0; u < 2; u++) {
        if (!st.tex[u] || !(key.tex_mask & (1 << u))) continue;
        have[u] = bind_texture(u, st.tex[u], st.tmap[u], bilinear, uvs[u], uvo[u], &st);
        if (!have[u] && st.tex[u]->fb_addr) return false;  // a frame we don't have: draw nothing rather than garbage
    }
    set_consts(draw_consts(st, alpha_threshold(st), have, uvs, uvo, bilinear ? 0.5f : 0.0f, inv_wf));
    if (debug_mips && st.mip_levels && st.tex[0]) {
        const TexMap& m = st.tmap[0];
        std::printf("[mip] draw: tex0 %016llx %ux%u fmt %u siz %u, %u smaller (", (unsigned long long)st.tex[0]->hash, st.tex[0]->width,
                    st.tex[0]->height, st.tex[0]->fmt, st.tex[0]->siz, st.mip_levels);
        for (uint32_t k = 0; k < st.mip_levels; k++) std::printf("%s%ux%u %016llx", k ? ", " : "", st.mip[k]->width, st.mip[k]->height,
                                                                  (unsigned long long)st.mip[k]->hash);
        std::printf("); bound %u levels, key %016llx, have %d/%d; om %06X comb %06X:%08X prog tex %d; map shift %.3f,%.3f off %.1f,%.1f "
                    "clamp %d,%d mirror %d,%d max %.0f,%.0f; consts c0 %.5f %.4f %.5f %.4f; uvs %.5f,%.5f uvo %.1f,%.1f; c5 %.3f %.3f %.3f %.3f\n",
                    last_mip_levels_, (unsigned long long)last_mip_key_, have[0], have[1], st.om_h, st.combine_hi, st.combine_lo, key.tex_mask,
                    m.shift_s, m.shift_t, m.offset_s, m.offset_t, m.clamp_s, m.clamp_t, m.mirror_s, m.mirror_t, m.clamp_max_s, m.clamp_max_t,
                    vpc_.c[0][0], vpc_.c[0][1], vpc_.c[0][2], vpc_.c[0][3], uvs[0][0], uvs[0][1], uvo[0][0], uvo[0][1],
                    vpc_.c[5][0], vpc_.c[5][1], vpc_.c[5][2], vpc_.c[5][3]);
        if (last_mip_levels_) {
            bool seen = false;
            for (uint64_t k : debug_mip_keys_) seen |= k == last_mip_key_;
            if (!seen && debug_mip_keys_.size() < 24) debug_mip_keys_.push_back(last_mip_key_);
        }
    }
    return true;
}

void RsxBackend::dump_mips(const char* dir, uint32_t tag) {
    for (uint64_t key : debug_mip_keys_) {
        auto it = mips_.find(key);
        if (it == mips_.end() || !it->second.rt) continue;
        const MipTex& m = it->second;
        // Levels side by side, deswizzled from what the RSX reads.
        const uint32_t W = m.w * 2 + 2 * m.levels, H = m.h;
        std::vector<uint8_t> rgba((size_t)W * H * 4, 0);
        const uint8_t* src = (const uint8_t*)m.rt->mem;
        uint32_t x0 = 0, w = m.w, h = m.h;
        for (uint32_t l = 0; l < m.levels; l++) {
            const uint32_t lw = log2u(w), lh = log2u(h);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t* p = src + swizzle(x, y, lw, lh) * 4;
                    uint8_t* d = &rgba[((size_t)y * W + x0 + x) * 4];
                    d[0] = p[1]; d[1] = p[2]; d[2] = p[3]; d[3] = 255;  // ARGB -> RGB (alpha shown opaque)
                }
            src += (size_t)w * h * 4;
            x0 += w + 2;
            w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
        }
        size_t len = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), (int)W, (int)H, 4, &len, 6, MZ_FALSE);
        char path[200];
        std::snprintf(path, sizeof path, "%s/mips_%u_%016llx.png", dir, tag, (unsigned long long)key);
        FILE* f = png ? std::fopen(path, "wb") : nullptr;
        if (f) { std::fwrite(png, 1, len, f); std::fclose(f); }
        if (png) mz_free(png);
        std::printf("[mip] %s: %ux%u, %u levels at RSX offset 0x%X: %s\n", path, m.w, m.h, m.levels, m.rt->off, f ? "saved" : "NOT saved");
    }
    debug_mip_keys_.clear();
}

void RsxBackend::rect(const DrawState& st, float ulx, float uly, float lrx, float lry, bool textured,
                      float s, float t0, float dsdx, float dtdy, bool flip, bool fill) {
    if (hung_) return;
    {
        // Widescreen: untextured rectangles reaching the scissor's sides reach into the margins.
        const uint32_t width = st.cimg_width ? st.cimg_width : 320;
        wide_rect(st, width, wide_margin(width), textured, &ulx, &lrx, &views_);
    }
    if (lrx <= ulx || lry <= uly) return;
    struct ResetExt { uint32_t (&e)[2][2]; ~ResetExt() { std::memset(e, 0, sizeof e); } } reset_ext{fb_ext_};
    if (textured && !flip) widen_frame_rect(st, &ulx, &lrx, &s, dsdx);
    ensure_cmd_space();
    phase = "rectangle: setup";
    ProgramKey key = make_program_key(st, fill);
    if (textured && !copy_frame_pieces(st, key.tex_mask)) return;  // a frame we don't have: draw nothing
    Target* t = bind_target(st);
    if (!t) return;
    const Program* p = program(key);
    note("%s %.1f,%.1f-%.1f,%.1f target 0x%06X: program mode %d comb %06X:%08X tex %d %s, om %06X:%08X",
         fill ? "fill rect" : textured ? "texture rect" : "rect", ulx, uly, lrx, lry, st.cimg_addr, (int)key.mode,
         key.combine_hi, key.combine_lo, key.tex_mask, p ? "ok" : "FAILED", st.om_h, st.om_l);
    if (!p) return;
    load_fp((rsxFragmentProgram*)p->blob.data(), p->offset);
    set_scissor(t, st);
    // NDC -1..1 across the whole surface, margins included.
    const float wn = (float)t->width_n64, hn = (float)t->height_n64, half_w = wn / 2 + (float)t->margin;
    const float vs[3] = {half_w, hn / 2, 0.5f}, vt[3] = {wn / 2, hn / 2, 0.5f};
    set_viewport(t, vs, vt);
    set_blend_depth(st, true, fill);
    const bool bilinear = textured && st.cycle() != CYC_COPY && ((st.om_h >> 12) & 3) >= 2;
    float uvs[2][2] = {{0, 0}, {0, 0}}, uvo[2][2] = {{0, 0}, {0, 0}};
    bool have[2] = {false, false};
    if (textured)
        for (int u = 0; u < 2; u++) {
            if (!st.tex[u] || !(key.tex_mask & (1 << u))) continue;
            // Smoothed at a higher resolution, a rectangle's edges would blend in the texels beyond the
            // ones it shows, which the N64 (one texel per pixel) never reads: leftovers in texture memory,
            // or the far side of the texture when it repeats (lines between the strips of a picture).
            // Within the texels it uses, without repeating: the texture cut to those, clamped.
            uint32_t crop[3];
            const uint32_t* cp = nullptr;
            if (bilinear && !st.tex[u]->fb_addr) {
                const TexMap& m = st.tmap[u];
                const float w = lrx - ulx, h = lry - uly;
                float sa = s, sb = s + (flip ? h : w) * dsdx, ta = t0, tb = t0 + (flip ? w : h) * dtdy;
                sa = sa * m.shift_s - m.offset_s; sb = sb * m.shift_s - m.offset_s;
                ta = ta * m.shift_t - m.offset_t; tb = tb * m.shift_t - m.offset_t;
                const float s0 = std::min(sa, sb), s1 = std::max(sa, sb), q0 = std::min(ta, tb), q1 = std::max(ta, tb);
                if (s0 >= -0.01f && q0 >= -0.01f && s1 <= st.tex[u]->width + 0.01f && q1 <= st.tex[u]->height + 0.01f) {
                    crop[0] = (uint32_t)std::ceil(s1 - 0.01f);
                    crop[1] = (uint32_t)std::floor(q0 + 0.01f);
                    crop[2] = (uint32_t)std::ceil(q1 - 0.01f);
                    cp = crop;
                }
            }
            have[u] = bind_texture(u, st.tex[u], st.tmap[u], bilinear, uvs[u], uvo[u], nullptr, cp);
            if (!have[u] && st.tex[u]->fb_addr) return;  // a frame we don't have: draw nothing rather than garbage
        }
    float shade[4] = {0, 0, 0, 0};
    if (fill) {
        if (st.cimg_siz == 3) {
            uint32_t c = st.fill_color;
            shade[0] = (c >> 24) / 255.0f; shade[1] = ((c >> 16) & 255) / 255.0f; shade[2] = ((c >> 8) & 255) / 255.0f; shade[3] = (c & 255) / 255.0f;
        } else {
            rgba5551((uint16_t)(st.fill_color >> 16), shade);
        }
    }
    const float z = st.z_source_prim() ? st.prim_depth * 2.0f - 1.0f : 0.0f;
    set_consts(draw_consts(st, alpha_threshold(st), have, uvs, uvo, bilinear ? 0.5f : 0.0f, 1.0f));
    // Corners: (x, y) and texel coordinates.
    const float xs[4] = {ulx, lrx, lrx, ulx}, ys[4] = {uly, uly, lry, lry};
    uint32_t off;
    Vertex* v = alloc_vertices(6, &off);
    if (!v) return;
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; k++) {
        int c = order[k];
        Vertex& d = v[k];
        d.pos[0] = (xs[c] - wn / 2) / half_w;
        d.pos[1] = 1.0f - ys[c] / (hn / 2);
        d.pos[2] = z;
        d.pos[3] = 1.0f;
        for (int i = 0; i < 4; i++) d.shade[i] = to8(shade[i]);
        d.pad = 0;
        // Texel coordinates: the constants map them for each texture, as for triangles.
        float dx = xs[c] - ulx, dy = ys[c] - uly;
        d.uv[0] = flip ? s + dy * dsdx : s + dx * dsdx;
        d.uv[1] = flip ? t0 + dx * dtdy : t0 + dy * dtdy;
    }
    draw(off, 6);
}

// Widescreen: a frame drawn back as rectangles of its own pieces, one to one (the motion blur, Conker
// drunk: each frame blends in its copy of the last; the pause menu's background), holds only the 4:3
// frame, so the margins beside it would keep what was there. A rectangle at the frame's left or right
// side whose piece starts or ends there reaches into the margin, reading the margin beside the piece
// in its frame (copy_frame_pieces): the whole widened picture goes round.
void RsxBackend::widen_frame_rect(const DrawState& st, float* ulx, float* lrx, float* s, float dsdx) {
    const Texture* tex = st.tex[0];
    if (!tex || !tex->fb_addr) return;
    if (st.tex[1] && st.tex[1]->fb_addr && st.tex[1] != tex) return;
    const uint32_t dw = st.cimg_width ? st.cimg_width : 320, m = wide_margin(dw);
    if (dw < 256 || m == 0) return;
    auto it = targets_.find(tex->fb_addr);
    if (it == targets_.end() || it->second.width_n64 != dw || it->second.margin != m) return;
    const TexMap& tm = st.tmap[0];
    if (std::fabs(dsdx * tm.shift_s - 1.0f) > 0.01f) return;
    const float texel_l = *s * tm.shift_s - tm.offset_s, texel_r = texel_l + (*lrx - *ulx);
    // A piece drawn where it was read from (one to one), starting at most 3 pixels inside the frame's
    // left side (the Haybot's red view starts at 2) or ending at or past its right side (the camera
    // reaches 2 pixels past it), reaches the widened frame's side.
    const bool in_place = std::fabs(*ulx - (float)tex->fb_x) < 0.6f;
    if (*ulx <= 3.5f && in_place && tex->fb_x >= 0 && std::fabs(texel_l) < 0.6f) {
        const uint32_t ext = m + (uint32_t)tex->fb_x;
        *ulx -= (float)ext;
        *s -= (float)ext * dsdx;
        fb_ext_[0][0] = ext;
    }
    if (*lrx >= (float)dw - 0.5f && tex->fb_x + (int)tex->width >= (int)dw && std::fabs(texel_r - (float)tex->width) < 0.6f) {
        const int past = tex->fb_x + (int)tex->width - (int)dw;  // already past the frame's side
        if ((int)m > past) {
            *lrx += (float)((int)m - past);
            fb_ext_[0][1] = (uint32_t)((int)m - past);
        }
    }
}

void RsxBackend::fill_rect(const DrawState& st, float ulx, float uly, float lrx, float lry) {
    rect(st, ulx, uly, lrx, lry, false, 0, 0, 0, 0, false, st.cycle() == CYC_FILL);
}

void RsxBackend::tex_rect(const DrawState& st, float ulx, float uly, float lrx, float lry,
                          float s, float t, float dsdx, float dtdy, bool flip) {
    // The old-film look (widescreen.h WideFilm): its scratches and specks are noted (frames drawn
    // while it's on are shown in grey, end_dl and present()) and spread across a widened picture.
    if (film_.note(st, ulx, uly, lrx, lry) || bullet_hole(st)) {
        const uint32_t width = st.cimg_width ? st.cimg_width : 320;
        wide_film(width, wide_margin(width), &ulx, &lrx);
    }
    // The XMB icon's logo (logo_image): a 32x32 tile drawn 1:1 with one of the logo's textures.
    if (!logo_hashes.empty() && st.tex[0] && st.tex[0]->rgba && st.tex[0]->width == 32 && st.tex[0]->height == 32 && !flip &&
        std::fabs(dsdx - 1.0f) < 0.01f && std::fabs(dtdy - 1.0f) < 0.01f &&
        std::find(logo_hashes.begin(), logo_hashes.end(), st.tex[0]->hash) != logo_hashes.end() && logo_now_.size() < 64) {
        const int x = (int)std::floor(ulx + 0.5f), y = (int)std::floor(uly + 0.5f);
        bool seen = false;
        for (const auto& lt : logo_now_) seen = seen || (lt.x == x && lt.y == y);
        if (!seen) logo_now_.push_back({x, y, std::vector<uint8_t>(st.tex[0]->rgba, st.tex[0]->rgba + 32 * 32 * 4)});
    }
    rect(st, ulx, uly, lrx, lry, true, s, t, dsdx, dtdy, flip, false);
}

void RsxBackend::end_dl() {
    if (hung_) return;
    ensure_cmd_space();
    phase = "end of display list";
    stats.vram_used_kb = vram_.in_use() / 1024;
    // The old-film look on (widescreen.h WideFilm): the frame this list drew is shown in grey.
    if (film_.on()) {
        auto ft = targets_.find(frame_target_addr_);
        if (ft != targets_.end() && ft->second.drawn_dl == dl_seq_) { ft->second.film_dl = dl_seq_; stats.film_frames++; }
    }
    if (!logo_hashes.empty()) {  // the XMB icon's logo: drawn whole in this list?
        if (logo_now_.size() >= logo_min_tiles && logo_min_tiles) { logo_lists_++; logo_last_.swap(logo_now_); }
        else logo_lists_ = 0;
        logo_now_.clear();
    }
    rsxSetWriteBackendLabel(ctx_, kLabelDl, ++dl_seq_);
    if ((dl_seq_ & 63) == 0) { evict_mips(); evict_targets(); }
    if (scratch_pos_) scratch_done_[scratch_half_] = dl_seq_;  // its frame copies are free once this label is written
    rsxFlushBuffer(ctx_);
    if (dls_fenced_ < fence_dls) {
        dls_fenced_++;
        char what[48];
        std::snprintf(what, sizeof what, "the end of display list #%u", dl_seq_);
        fence(what);
    }
    phase = "freeing textures";
    free_textures_done();
    phase = "between display lists";
}

void RsxBackend::texture_evicted(Texture& tex) {
    if (tex.backend_data) {
        free_queue_.push_back({dl_seq_, (RsxTex*)tex.backend_data});
        tex.backend_data = nullptr;
    }
}

void RsxBackend::free_textures_done() {
    uint32_t done = *label(kLabelDl);
    for (size_t i = 0; i < free_queue_.size();) {
        if (free_queue_[i].first <= done) {
            vram_.free(free_queue_[i].second->mem);
            delete free_queue_[i].second;
            free_queue_[i] = free_queue_.back();
            free_queue_.pop_back();
        } else {
            i++;
        }
    }
}

// ------------------------------------------------------------------------------- display
namespace {
    // The visible part of the VI's image: rows and width in N64 pixels, and the fraction of the
    // 4:3 screen it covers.
    void vi_area(const RsxBackend::ViInfo& vi, uint32_t width, float* src_w, float* src_h, float* frac_w, float* frac_h) {
        float hpix = (float)((vi.h_start & 0x3FF) - ((vi.h_start >> 16) & 0x3FF));
        float vlines = (float)((vi.v_start & 0x3FF) - ((vi.v_start >> 16) & 0x3FF)) / 2.0f;
        float xs = (vi.x_scale & 0xFFF) / 1024.0f, ys = (vi.y_scale & 0xFFF) / 1024.0f;
        if (hpix <= 0 || xs <= 0) { hpix = 640; xs = width / 640.0f; }
        if (vlines <= 0 || ys <= 0) { vlines = 240; ys = 1; }
        *src_w = std::min((float)width, hpix * xs);
        *src_h = vlines * ys;
        *frac_w = std::min(1.0f, hpix / 640.0f);
        *frac_h = std::min(1.0f, vlines / 240.0f);
    }
}

bool RsxBackend::present(const ViInfo& vi) {
    if (hung_) return false;
    ensure_cmd_space();
    // The target holding the VI origin: the nearest one at or below the address.
    uint32_t origin = vi.origin & 0xFFFFFF;
    auto it = targets_.upper_bound(origin);
    if (it == targets_.begin()) return false;
    --it;
    Target* tp = &it->second;
    uint32_t row_bytes = tp->width_n64 * tp->bpp;
    if (origin >= tp->addr + row_bytes * tp->height_n64) return false;
    // Frame interpolation: the image's in-between frame first (the image itself stays to be shown next).
    auto mid = mid_targets_.find(tp->addr);
    if (mid != mid_targets_.end() && mid->second.mid_ready && mid->second.width_n64 == tp->width_n64) {
        mid->second.mid_ready = false;
        tp = &mid->second;
        stats.mid_presents++;
    } else {
        if (!tp->dirty && origin == last_present_origin_) return false;
        // The VI moved to a frame not drawn since it was last shown: an older picture. The game swaps
        // to its next frame a moment before that frame's list is drawn here (when the RSX runs late),
        // and showing the frame then put the picture from two frames back up for one refresh before
        // the new one: back and forth every frame (it looked like more than 30 fps). Keep the picture
        // that's up until the frame is drawn; but if the VI stays there (a frame shown again without
        // being drawn), show it after 10 VIs.
        if (!tp->dirty && tp->addr != last_present_target_ && last_present_target_ && ++older_waits_ < 10) {
            stats.present_older_waited++;
            return false;
        }
        older_waits_ = 0;
        // Why (counted for the log): drawn since last shown, or the VI moved to an older picture.
        const bool drawn = tp->dirty;
        if (drawn) stats.present_drawn++;
        else if (tp->addr != last_present_target_) stats.present_older++;
        else stats.present_moved++;
        // A picture right after the last (under 25 ms): what it was, the first 40 times.
        const uint64_t now = __mftb();
        const double since_ms = last_present_tb_ ? (double)(now - last_present_tb_) * 1000.0 / sysGetTimebaseFrequency() : 1e9;
        if (since_ms < 25.0 && quick_logged_ < 40) {
            quick_logged_++;
            std::printf("[gfx] picture %.1f ms after the last: VI origin 0x%06X (last 0x%06X), frame 0x%06X (last 0x%06X), %s, "
                        "%u display lists since\n", since_ms, origin, last_present_origin_, tp->addr, last_present_target_,
                        drawn ? "drawn since last shown" : tp->addr != last_present_target_ ? "NOT drawn since last shown" : "same frame, other row",
                        lists_since_present);
        }
        last_present_tb_ = now;
        lists_since_present = 0;
        tp->dirty = false;
    }
    Target& t = *tp;
    t.last_used = dl_seq_;
    last_present_target_ = t.addr;
    last_present_origin_ = origin;
    float row0 = (float)((origin - t.addr) / row_bytes);
    float sw, sh, fw, fh;
    vi_area(vi, t.width_n64, &sw, &sh, &fw, &fh);

    phase = "present";
    note("present: target 0x%06X (%ux%u), origin row %.0f, area %.0fx%.0f", t.addr, t.w, t.h, row0, sw, sh);
    // Draw onto the display buffer (rsxutil's), then flip.
    begin_display();
    ProgramKey key;
    key.mode = t.film_dl == t.drawn_dl ? ProgramMode::Gray : ProgramMode::Copy;  // the old-film look
    key.tex_mask = 1;
    const Program* p = program(key);
    if (!p) return false;
    load_fp((rsxFragmentProgram*)p->blob.data(), p->offset);
    load_display_texture(t.color_off, t.w, t.h, t.pitch);
    // Widescreen: the margins too, across the whole display.
    display_quad(fw, fh, (sw + 2 * t.margin) / (float)(t.width_n64 + 2 * t.margin), row0 / t.height_n64, (row0 + sh) / t.height_n64,
                 t.margin != 0);
    phase = "flip";
    { uint64_t t0 = __mftb(); flip(); stats.t_flip += __mftb() - t0; }  // waits for the previous flip, queues this one
    phase = "after flip";
    bound_ = nullptr;
    stats.presents++;
    if (stats.presents <= 30) fence("a present");
    return true;
}

void RsxBackend::show_black() {
    if (hung_) return;
    ensure_cmd_space();
    for (int i = 0; i < 2; i++) {  // both display buffers
        begin_display();
        flip();
    }
    bound_ = nullptr;
    fence("a black screen");
}

void RsxBackend::begin_display() {
    reset_state();
    setRenderTarget(curr_fb);
    bound_ = nullptr;
    rsxSetColorMask(ctx_, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetClearColor(ctx_, 0);
    rsxSetScissor(ctx_, 0, 0, (u16)display_width, (u16)display_height);
    rsxClearSurface(ctx_, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    rsxSetBlendEnable(ctx_, GCM_FALSE);
    rsxSetDepthTestEnable(ctx_, GCM_FALSE);
    rsxSetDepthWriteEnable(ctx_, GCM_FALSE);
    rsxSetPolygonOffsetFillEnable(ctx_, GCM_FALSE);
    rsxSetCullFaceEnable(ctx_, GCM_FALSE);
    for (u8 i = 0; i < 8; i++) rsxSetViewportClip(ctx_, i, (u16)display_width, (u16)display_height);
    float s[4] = {display_width * 0.5f, display_height * -0.5f, 0.5f, 0};
    float o[4] = {display_width * 0.5f, display_height * 0.5f, 0.5f, 0};
    rsxSetViewport(ctx_, 0, 0, (u16)display_width, (u16)display_height, 0.0f, 1.0f, s, o);
}

void RsxBackend::load_display_texture(uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch) {
    gcmTexture tx;
    std::memset(&tx, 0, sizeof tx);
    tx.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
    tx.mipmap = 1;
    tx.dimension = GCM_TEXTURE_DIMS_2D;
    tx.cubemap = GCM_FALSE;
    tx.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
                (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
                (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
                (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
                (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
    tx.width = (u16)w;
    tx.height = (u16)h;
    tx.depth = 1;
    tx.location = GCM_LOCATION_RSX;
    tx.pitch = pitch;
    tx.offset = offset;
    rsxInvalidateTextureCache(ctx_, GCM_INVALIDATE_TEXTURE);
    rsxLoadTexture(ctx_, 0, &tx);
    rsxTextureControl(ctx_, 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(ctx_, 0, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(ctx_, 0, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0,
                       GCM_TEXTURE_ZFUNC_LESS, 0);
}

// A quad in the 4:3 area in the middle of the display (wide: the whole display width) (fw, fh: its share of that area), texture
// coordinates u 0..u1, v v0..v1.
void RsxBackend::display_quad(float fw, float fh, float u1, float v0, float v1, bool wide) {
    float area_h = (float)display_height, area_w = wide ? (float)display_width : area_h * 4.0f / 3.0f;
    float qw = area_w * fw, qh = area_h * fh;
    // Filling the screen: as tall as the display, as much wider (a little past the sides may be cut).
    if (fill_screen && qh > 0 && qh < area_h) { qw *= area_h / qh; qh = area_h; }
    float x0 = (display_width - qw) * 0.5f, y0 = (display_height - qh) * 0.5f;
    float u0 = 0;
    uint32_t off;
    Vertex* v = alloc_vertices(6, &off);
    if (!v) return;
    const float px[4] = {x0, x0 + qw, x0 + qw, x0}, py[4] = {y0, y0, y0 + qh, y0 + qh};
    const float pu[4] = {u0, u1, u1, u0}, pv[4] = {v0, v0, v1, v1};
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; k++) {
        int c = order[k];
        Vertex& d = v[k];
        std::memset(&d, 0, sizeof d);
        d.pos[0] = px[c] / (display_width * 0.5f) - 1.0f;
        d.pos[1] = 1.0f - py[c] / (display_height * 0.5f);
        d.pos[2] = 0; d.pos[3] = 1;
        d.uv[0] = pu[c]; d.uv[1] = pv[c];
        d.shade[0] = d.shade[1] = d.shade[2] = d.shade[3] = 255;
    }
    identity_consts();
    draw(off, 6);
}

void RsxBackend::alloc_scratch() {
    scratch_size_ = scratch_bytes(scale_);
    scratch_ = (uint8_t*)vram_.alloc(scratch_size_, 4096);
    if (!scratch_) { scratch_size_ = 8u * 1024 * 1024; scratch_ = (uint8_t*)vram_.alloc(scratch_size_, 4096); }
    if (scratch_) rsxAddressToOffset(scratch_, &scratch_off_);
    else scratch_size_ = 0;
    scratch_pos_ = 0;
    scratch_done_[0] = scratch_done_[1] = 0;
}

const RsxBackend::DepthBuf* RsxBackend::depth_for(uint32_t w, uint32_t h, bool* fresh) {
    const uint64_t key = (uint64_t)w << 32 | h;
    auto it = depths_.find(key);
    if (it != depths_.end()) return &it->second;
    DepthBuf d;
    d.pitch = align_up(w * 4, 64);
    d.mem = vram_.alloc(d.pitch * h, 4096);
    if (!d.mem) return nullptr;
    rsxAddressToOffset(d.mem, &d.off);
    *fresh = true;
    return &(depths_[key] = d);
}

bool RsxBackend::set_scale(uint32_t scale) {
    if (scale == scale_ || hung_) return !hung_;
    // Everything drawn at the old scale goes (the RSX finishes with it first); the next frames are
    // drawn at the new one.
    ensure_cmd_space();
    if (!fence("a resolution change")) return false;
    for (auto* m : {&targets_, &mid_targets_}) {
        for (auto& [a, t] : *m) vram_.free(t.color);
        m->clear();
    }
    for (auto& [k, d] : depths_) vram_.free(d.mem);
    depths_.clear();
    vram_.free(scratch_);
    scratch_ = nullptr;
    bound_ = nullptr;
    pending_depth_clear_.clear();
    last_present_origin_ = 0;
    const bool vp = cache_.vp_loaded;
    cache_ = StateCache{};
    cache_.vp_loaded = vp;
    scale_ = scale;
    alloc_scratch();
    stats.scratch_max_kb = 0;
    std::printf("[gfx] resolution: %u lines (render scale %u); frame texture memory %u MB; RSX memory %u of %u MB in use\n",
                216 * scale, scale, scratch_size_ >> 20, (unsigned)(vram_.in_use() >> 20), stats.vram_mb);
    return true;
}

// The RSX's progress: a label after each display list (both its passes); a thread watching it notes
// when the RSX got there. Compared with when the PPU had handed the list over, that's how far behind the
// PPU the RSX runs. (The RSX's own timestamps stopped it on the console.)
void RsxBackend::mark_list_end() {
    if (hung_) return;
    ensure_cmd_space();
    const uint32_t seq = ts_seq_ + 1;
    rsxSetWriteBackendLabel(ctx_, kLabelTs, seq);
    rsxFlushBuffer(ctx_);
    ts_kick_[seq % kTsSlots] = __mftb();
    ts_seq_ = seq;
    if (!ts_thread_started_) {
        ts_thread_started_ = true;
        std::thread([this] {
            uint32_t seen = 0;
            for (;;) {
                const uint32_t v = *label(kLabelTs);
                if (v != seen) {
                    const uint64_t now = __mftb();
                    for (uint32_t k = seen + 1; k <= v && k > seen; k++) ts_done_[k % kTsSlots] = now;
                    seen = v;
                    __lwsync();  // the times before the count
                    ts_seen_ = v;
                }
                usleep(250);
            }
        }).detach();
    }
    // Lists finished since the last call (still in the slots).
    const uint32_t done = ts_seen_;
    __lwsync();
    if (ts_read_ + kTsSlots < ts_seq_) ts_read_ = ts_seq_ - kTsSlots;
    const double ticks_per_ms = sysGetTimebaseFrequency() / 1000.0;
    while (ts_read_ < done) {
        ts_read_++;
        const uint64_t k = ts_kick_[ts_read_ % kTsSlots], d = ts_done_[ts_read_ % kTsSlots];
        const double lag_ms = d > k ? (d - k) / ticks_per_ms : 0.0;
        stats.rsx_lag_sum_ms += lag_ms;
        stats.rsx_lag_max_ms = std::max(stats.rsx_lag_max_ms, lag_ms);
        stats.rsx_lag_n++;
    }
}

void RsxBackend::describe_targets() {
    std::printf("[gfx]   RSX targets (scale %u, bound %s, viewport cache %ux%u scale %.1f %.1f offset %.1f %.1f):\n", scale_,
                bound_ ? "yes" : "none", cache_.vp_w, cache_.vp_h, cache_.vp[0], cache_.vp[1], cache_.vp[4], cache_.vp[5]);
    for (int m = 0; m < 2; m++)
        for (const auto& [addr, t] : m ? mid_targets_ : targets_)
            std::printf("[gfx]     %s 0x%06X: N64 %ux%u (+%u) -> %ux%u pitch %u, color 0x%X depth 0x%X, xoff %u%s%s\n", m ? "in-between" : "image", addr,
                        t.width_n64, t.height_n64, t.margin, t.w, t.h, t.pitch, t.color_off, t.depth_off, t.xoff,
                        &t == bound_ ? ", bound" : "", t.mid_ready ? ", ready" : "");
}

std::vector<uint32_t> RsxBackend::frame_target_addrs() const {
    std::vector<uint32_t> v;
    for (const auto& [a, t] : targets_) if (t.width_n64 >= 256 && t.addr != 0x7FF000) v.push_back(a);
    return v;
}

bool RsxBackend::save_target(uint32_t addr, bool mid, const char* path) {
    if (hung_) return false;
    const auto& m = mid ? mid_targets_ : targets_;
    auto it = m.find(addr);
    if (it == m.end()) return false;
    const Target& t = it->second;
    if (!fence("saving a target")) return false;
    const uint32_t w = t.width_n64 + 2 * t.margin, h = t.height_n64;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = (const uint8_t*)t.color + (size_t)(y * scale_) * t.pitch;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = src + (size_t)x * scale_ * 4;
            uint8_t* d = &rgba[((size_t)y * w + x) * 4];
            d[0] = p[1]; d[1] = p[2]; d[2] = p[3]; d[3] = 255;
        }
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), (int)w, (int)h, 4, &len, 6, MZ_FALSE);
    if (!png) return false;
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(png, 1, len, f); std::fclose(f); }
    mz_free(png);
    return f != nullptr;
}

bool RsxBackend::logo_image(std::vector<uint8_t>& rgba, uint32_t* w_out, uint32_t* h_out) {
    if (logo_hashes.empty() || logo_lists_ < logo_min_lists || logo_last_.empty()) return false;
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (const auto& t : logo_last_) { x0 = std::min(x0, t.x); y0 = std::min(y0, t.y); x1 = std::max(x1, t.x + 32); y1 = std::max(y1, t.y + 32); }
    const uint32_t w = (uint32_t)(x1 - x0), h = (uint32_t)(y1 - y0);
    if (w > 1024 || h > 1024) return false;
    rgba.assign((size_t)w * h * 4, 0);
    for (const auto& t : logo_last_)
        for (int y = 0; y < 32; y++)
            std::memcpy(&rgba[((size_t)(t.y - y0 + y) * w + (t.x - x0)) * 4], &t.rgba[(size_t)y * 32 * 4], 32 * 4);
    *w_out = w; *h_out = h;
    return true;
}

bool RsxBackend::screenshot(const ViInfo& vi, const char* path) {
    if (hung_) return false;
    uint32_t origin = vi.origin & 0xFFFFFF;
    auto it = targets_.upper_bound(origin);
    if (it == targets_.begin()) return false;
    --it;
    Target& t = it->second;
    uint32_t row_bytes = t.width_n64 * t.bpp;
    if (origin >= t.addr + row_bytes * t.height_n64) return false;
    uint32_t row0 = (origin - t.addr) / row_bytes;
    float sw, sh, fw, fh;
    vi_area(vi, t.width_n64, &sw, &sh, &fw, &fh);
    uint32_t w = (uint32_t)sw + 2 * t.margin, h = std::min<uint32_t>((uint32_t)sh, t.height_n64 - row0);  // with the margins
    // Wait for the RSX to finish drawing (a fence: rsxFinish's reference wait never returned on the console).
    if (!fence("a screenshot")) return false;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = (const uint8_t*)t.color + (size_t)((row0 + y) * scale_) * t.pitch;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = src + (size_t)x * scale_ * 4;
            uint8_t* d = &rgba[((size_t)y * w + x) * 4];
            d[0] = p[1]; d[1] = p[2]; d[2] = p[3]; d[3] = 255;
        }
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), (int)w, (int)h, 4, &len, 6, MZ_FALSE);
    if (!png) return false;
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(png, 1, len, f); std::fclose(f); }
    mz_free(png);
    return f != nullptr;
}

}  // namespace n64gfx

namespace n64gfx {

namespace {
    uint32_t pixel(const void* base, uint32_t pitch, uint32_t x, uint32_t y) {
        return *(volatile const uint32_t*)((const uint8_t*)base + (size_t)y * pitch + x * 4);
    }
}

// Draws two triangles covering the bound target with a hand-written fragment program (self-test).
bool RsxBackend::test_draw(const char* name, const char* fp_source, uint32_t rgba, bool textured) {
    std::vector<uint8_t> blob;
    if (!fpasm::assemble(fp_source, blob)) { std::printf("[gfx] self-test: %s: program did not assemble\n", name); return false; }
    rsxFragmentProgram* fp = (rsxFragmentProgram*)blob.data();
    void* ucode = nullptr; u32 size = 0;
    rsxFragmentProgramGetUCode(fp, &ucode, &size);
    void* mem = vram_.alloc(size, 128);
    if (!mem) return false;
    std::memcpy(mem, ucode, size);
    u32 off = 0;
    rsxAddressToOffset(mem, &off);
    load_fp(fp, off);
    uint32_t voff;
    Vertex* v = alloc_vertices(6, &voff);
    const float xs[6] = {-1, 1, 1, -1, 1, -1}, ys[6] = {-1, -1, 1, -1, 1, 1};
    for (int k = 0; k < 6; k++) {
        std::memset(&v[k], 0, sizeof(Vertex));
        v[k].pos[0] = xs[k]; v[k].pos[1] = ys[k]; v[k].pos[2] = 0; v[k].pos[3] = 1;
        v[k].shade[0] = rgba >> 24; v[k].shade[1] = rgba >> 16; v[k].shade[2] = rgba >> 8; v[k].shade[3] = rgba;
        v[k].uv[0] = (xs[k] + 1) * 0.5f; v[k].uv[1] = (1 - ys[k]) * 0.5f;
    }
    (void)textured;
    identity_consts();
    draw(voff, 6);
    bool ok = fence(name);
    vram_.free(mem);
    cache_.fp_offset = ~0u;  // the next program may be put at the same place: load it again
    return ok;
}

bool RsxBackend::self_test() {
    const uint32_t kAddr = 0x7FF000;  // not a real N64 image address
    fence_draws = 0;                  // the self-test fences its steps itself
    DrawState st;
    st.cimg_addr = kAddr; st.cimg_width = 320; st.cimg_siz = 2;
    st.sc_ulx = 0; st.sc_uly = 0; st.sc_lrx = 320; st.sc_lry = 240;
    Target* t = bind_target(st);
    if (!t) { std::printf("[gfx] self-test: no render target (%s)\n", last_error.c_str()); return false; }
    auto report = [&](const char* name, bool ok) {
        std::printf("[gfx] self-test: %-28s %s  pixels: center %08X  (10,10) %08X  (100,80) %08X\n", name, ok ? "ok     " : "STOPPED",
                    (unsigned)pixel(t->color, t->pitch, t->w / 2, t->h / 2), (unsigned)pixel(t->color, t->pitch, 10, 10),
                    (unsigned)pixel(t->color, t->pitch, 100, 80));
        std::fflush(stdout);
        return ok;
    };
    // 1. Clear to a known color (tests the surface and reading it back).
    rsxSetClearColor(ctx_, 0xFF336699);
    rsxSetScissor(ctx_, 0, 0, t->w, t->h);
    cache_.sc[0] = -1;
    rsxClearSurface(ctx_, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);
    if (!report("clear to FF336699", fence("clear"))) return false;
    // Fixed state for the hand-written programs.
    set_scissor(t, st);
    { const float sc[3] = {160, 120, 0.5f}, tr[3] = {160, 120, 0.5f}; set_viewport(t, sc, tr); }
    rsxSetBlendEnable(ctx_, GCM_FALSE);
    rsxSetDepthTestEnable(ctx_, GCM_FALSE);
    rsxSetDepthWriteEnable(ctx_, GCM_FALSE);
    rsxSetPolygonOffsetFillEnable(ctx_, GCM_FALSE);
    cache_.blend = 0; cache_.dtest = 0; cache_.dwrite = 0; cache_.poff = 0;
    // 2. Constant color (tests vertex positions and the fragment program, no inputs).
    // The center pixel checks that the RSX read the vertices (positions, then colors).
    bool pixels_ok = true;
    auto center_is = [&](uint32_t argb, const char* what) {
        uint32_t got = pixel(t->color, t->pitch, t->w / 2, t->h / 2);
        if (got != argb) { pixels_ok = false; std::printf("[gfx] self-test: WRONG PIXEL after %s: %08X, expected %08X\n", what, (unsigned)got, (unsigned)argb); }
    };
    report("constant red program", test_draw("constant red", "!!ARBfp1.0\n#const c[0] = 1 0 0 1\nMOV result.color, c[0];\nEND\n", 0, false));
    center_is(0xFFFF0000, "constant red");
    // 3. Vertex color in ARB syntax and in NV syntax.
    report("vertex color (fragment.color)", test_draw("vertex color A", "!!ARBfp1.0\nMOV result.color, fragment.color;\nEND\n", 0x00FF00FF, false));
    center_is(0xFF00FF00, "vertex color");
    report("vertex color (f[COL0])", test_draw("vertex color B", "!!ARBfp1.0\nMOV R2, f[COL0];\nMOV result.color, R2;\nEND\n", 0x0000FFFF, false));
    // The grey of the old-film look (ProgramMode::Gray's arithmetic, on the vertex color): pure blue
    // has luma 0.114, about 0x1D in each channel.
    report("grey (luma of blue)", test_draw("grey", "!!ARBfp1.0\n#const c[1] = 0.299 0.587 0.114 0\nMOV R2, f[COL0];\n"
                                                    "DP3 R3.x, R2, c[1];\nMOV R2.xyz, R3.xxxx;\nMOV result.color, R2;\nEND\n", 0x0000FFFF, false));
    {
        const uint32_t got = pixel(t->color, t->pitch, t->w / 2, t->h / 2);
        const uint32_t r = (got >> 16) & 0xFF, g = (got >> 8) & 0xFF, b = got & 0xFF;
        if (r != g || g != b || r < 0x1A || r > 0x20) { pixels_ok = false; std::printf("[gfx] self-test: WRONG PIXEL after grey: %08X, expected about FF1D1D1D\n", (unsigned)got); }
    }
    // 4. The renderer's own paths.
    st.om_h = 3u << 20;  // fill cycle
    st.fill_color = 0x07C107C1;  // green in RGBA5551
    fill_rect(st, 0, 0, 320, 240);
    report("fill rectangle (green)", fence("fill rectangle"));
    OutVertex tri[3] = {
        {-1.0f, -1.0f, 0, 1, 0, 0, {255, 0, 0, 255}},
        {1.0f, -1.0f, 0, 1, 0, 0, {0, 255, 0, 255}},
        {0.0f, 1.0f, 0, 1, 0, 0, {0, 0, 255, 255}},
    };
    draw_triangles(st, tri, 3);
    report("shaded triangle", fence("shaded triangle"));
    {
        // The same triangle read by the RSX where it is (as the SPU's streams are), its positions apart
        // too (as in-between frames are): it must come out the same, else every draw is copied.
        const uint32_t want = pixel(t->color, t->pitch, t->w / 2, t->h / 2);
        uint32_t o1 = 0, o2 = 0;
        Vertex* v = alloc_vertices(3, &o1);
        Vertex* pv = alloc_vertices(2, &o2);  // 3 positions of 16 bytes
        if (v && pv) {
            std::memcpy(v, tri, sizeof tri);
            float (*pos)[4] = reinterpret_cast<float (*)[4]>(pv);
            for (int k = 0; k < 3; k++) { pos[k][0] = tri[k].x; pos[k][1] = tri[k].y; pos[k][2] = tri[k].z; pos[k][3] = tri[k].w; }
            const char* what[2] = {"triangle read in place", "positions read in place"};
            for (int pass = 0; pass < 2 && direct_ok_; pass++) {
                rsxSetClearColor(ctx_, 0xFF000000);
                rsxSetScissor(ctx_, 0, 0, t->w, t->h);
                cache_.sc[0] = -1;
                rsxClearSurface(ctx_, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
                if (setup_triangles(st, 3)) draw_vertices(reinterpret_cast<const OutVertex*>(v), pass ? pos : nullptr, 3);
                const bool ok = fence(what[pass]);
                const uint32_t got = pixel(t->color, t->pitch, t->w / 2, t->h / 2);
                report(what[pass], ok);
                if (got != want || !stats.direct_vertices) {
                    std::printf("[gfx] self-test: %s gave %08X, not %08X: vertices will be copied\n", what[pass], (unsigned)got, (unsigned)want);
                    direct_ok_ = false;
                }
            }
        }
    }
    static uint8_t pixels[8 * 8 * 4];
    for (int i = 0; i < 64; i++) {
        bool on = ((i & 7) ^ (i >> 3)) & 1;
        pixels[i * 4] = on ? 255 : 0; pixels[i * 4 + 1] = 255; pixels[i * 4 + 2] = on ? 0 : 255; pixels[i * 4 + 3] = 255;
    }
    static Texture tex;
    tex.rgba = pixels; tex.width = 8; tex.height = 8;
    st.om_h = 2u << 20;  // copy cycle
    st.tex[0] = &tex;
    st.tmap[0] = TexMap{};
    tex_rect(st, 0, 0, 320, 240, 0, 0, 0.125f, 0.125f, false);
    report("textured rectangle", fence("textured rectangle"));
    st.om_h = 0;
    st.combine_hi = (1u << 20) | (4u << 15) | (1u << 12) | (4u << 9);   // TEXEL0 * SHADE
    st.combine_lo = (15u << 28) | (7u << 15) | (7u << 12) | (7u << 9);
    draw_triangles(st, tri, 3);
    report("combiner triangle", fence("combiner triangle"));
    {
        // A mipmapped texture (texture LOD): an 8x8 base with a color per quarter (checks the
        // swizzled layout), then 4x4 green, 2x2 and 1x1 yellow. Drawn over the target twice: at
        // one texel per 1/8 of the target (level 0, point sampled), then at 4 texels per pixel (level 2).
        static uint8_t l0[8 * 8 * 4], l1[4 * 4 * 4], l2[2 * 2 * 4], l3[4];
        const uint32_t q[4] = {0xFF0000FF, 0x00FF00FF, 0x0000FFFF, 0xFFFFFFFF};  // RGBA: red, green / blue, white
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            const uint32_t c = q[(y >= 4) * 2 + (x >= 4)];
            uint8_t* p = l0 + (y * 8 + x) * 4;
            p[0] = c >> 24; p[1] = c >> 16; p[2] = c >> 8; p[3] = c;
        }
        for (int i = 0; i < 16; i++) { l1[i * 4] = 0; l1[i * 4 + 1] = 255; l1[i * 4 + 2] = 0; l1[i * 4 + 3] = 255; }
        for (int i = 0; i < 4; i++) { l2[i * 4] = 255; l2[i * 4 + 1] = 255; l2[i * 4 + 2] = 0; l2[i * 4 + 3] = 255; }
        l3[0] = l3[1] = 255; l3[2] = 0; l3[3] = 255;  // yellow too (the level 2 check may blend toward it)
        static Texture m0, m1, m2, m3;
        m0.rgba = l0; m0.width = m0.height = 8; m0.hash = 0x6D69703000000001ull;
        m1.rgba = l1; m1.width = m1.height = 4; m1.hash = 0x6D69703000000002ull;
        m2.rgba = l2; m2.width = m2.height = 2; m2.hash = 0x6D69703000000003ull;
        m3.rgba = l3; m3.width = m3.height = 1; m3.hash = 0x6D69703000000004ull;
        DrawState ms = st;
        ms.om_h = (1u << 20) | (1u << 16);  // 2-cycle, texture LOD, point sampled
        ms.om_l = 0;
        // Both cycles: TEXEL0 (color and alpha), then COMBINED.
        ms.combine_hi = (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u;
        ms.combine_lo = (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (1u << 15) | (7u << 12) | (1u << 9) | (0u << 6) | (7u << 3) | 0u;
        ms.tex[0] = &m0; ms.tex[1] = nullptr;
        ms.tmap[0] = TexMap{};
        ms.mip_levels = 3; ms.mip[0] = &m1; ms.mip[1] = &m2; ms.mip[2] = &m3;
        auto quad = [&](float span_s, float span_t) {
            const float xs[6] = {-1, 1, 1, -1, 1, -1}, ys[6] = {-1, -1, 1, -1, 1, 1};
            OutVertex v[6];
            for (int k = 0; k < 6; k++) {
                v[k] = OutVertex{xs[k], ys[k], 0, 1, (xs[k] + 1) * 0.5f * span_s, (1 - ys[k]) * 0.5f * span_t, {255, 255, 255, 255}};
            }
            draw_triangles(ms, v, 6);
        };
        quad(8, 8);
        bool ok = fence("mipmapped texture, level 0");
        const uint32_t a = pixel(t->color, t->pitch, t->w / 4, t->h / 4), b = pixel(t->color, t->pitch, t->w * 3 / 4, t->h / 4);
        const uint32_t c = pixel(t->color, t->pitch, t->w / 4, t->h * 3 / 4), d = pixel(t->color, t->pitch, t->w * 3 / 4, t->h * 3 / 4);
        report("mipmapped texture, level 0", ok);
        // (Upside down is fine: that's the test's mapping, not the layout.)
        const bool l0ok = (a == 0xFFFF0000 && b == 0xFF00FF00 && c == 0xFF0000FF && d == 0xFFFFFFFF) ||
                          (c == 0xFFFF0000 && d == 0xFF00FF00 && a == 0xFF0000FF && b == 0xFFFFFFFF);
        quad(4.0f * t->w, 4.0f * t->h);
        ok = fence("mipmapped texture, level 2");
        const uint32_t e = pixel(t->color, t->pitch, t->w / 2, t->h / 2);
        report("mipmapped texture, level 2", ok);
        const bool l2ok = e == 0xFFFFFF00;
        std::printf("[gfx] self-test: mipmaps: level 0 quarters %08X %08X / %08X %08X (want FFFF0000 FF00FF00 / FF0000FF FFFFFFFF) %s; "
                    "level 2 %08X (want FFFFFF00) %s\n", (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d, l0ok ? "right" : "WRONG",
                    (unsigned)e, l2ok ? "right" : "WRONG");
        if (!l0ok || !l2ok) { mips_ok_ = false; std::printf("[gfx] self-test: mipmapped textures are off: textures drawn full size\n"); }
        // The test's texture goes (its key isn't a game texture's).
        for (auto& kv : mips_) if (kv.second.rt) free_queue_.push_back({dl_seq_, kv.second.rt});
        mips_.clear();
    }
    // 5. Drawing onto the display buffer (not shown: read back before any flip).
    {
        auto disp = [&](const char* name) {
            bool ok = fence(name);
            std::printf("[gfx] self-test: display: %-30s %s  pixels: center %08X  (200,200) %08X\n", name, ok ? "ok     " : "STOPPED",
                        (unsigned)pixel(color_buffer[curr_fb], color_pitch, display_width / 2, display_height / 2),
                        (unsigned)pixel(color_buffer[curr_fb], color_pitch, 200, 200));
            std::fflush(stdout);
        };
        begin_display();
        disp("clear to black");
        // (a) constant red program over the 4:3 area.
        {
            std::vector<uint8_t> blob;
            if (fpasm::assemble("!!ARBfp1.0\n#const c[0] = 1 0 0 1\nMOV result.color, c[0];\nEND\n", blob)) {
                void* ucode = nullptr; u32 size = 0;
                rsxFragmentProgramGetUCode((rsxFragmentProgram*)blob.data(), &ucode, &size);
                void* mem = vram_.alloc(size, 128);
                u32 off = 0;
                std::memcpy(mem, ucode, size);
                rsxAddressToOffset(mem, &off);
                load_fp((rsxFragmentProgram*)blob.data(), off);
                display_quad(1, 1, 1, 0, 1);
                disp("constant red quad");
            }
        }
        ProgramKey ck;
        ck.mode = ProgramMode::Copy;
        ck.tex_mask = 1;
        if (const Program* cp = program(ck)) {
            load_fp((rsxFragmentProgram*)cp->blob.data(), cp->offset);
            // (b) the uploaded 8x8 checker texture.
            if (tex.backend_data) {
                RsxTex* rt = (RsxTex*)tex.backend_data;
                load_display_texture(rt->off, 8, 8, rt->pitch);
                display_quad(1, 1, 1, 0, 1);
                disp("checker texture quad");
            }
            // (c) the test render target as a texture (what present does).
            load_display_texture(t->color_off, t->w, t->h, t->pitch);
            display_quad(1, 1, 1, 0, 1);
            disp("render target texture quad");
        }
    }
    // 6. Copy to the display and flip.
    ViInfo vi{kAddr, 320, (0x6Cu << 16) | 0x2ECu, (0x25u << 16) | 0x1FFu, 0x200, 0x400};
    t->dirty = true;
    bool presented = present(vi);
    bool ok = fence("present");
    uint32_t shown = curr_fb ^ 1;
    std::printf("[gfx] self-test: present %s (%s), display %ux%u, pixels: center %08X (200,200) %08X\n",
                presented ? "done" : "SKIPPED", ok ? "ok" : "STOPPED", display_width, display_height,
                (unsigned)pixel(color_buffer[shown], color_pitch, display_width / 2, display_height / 2),
                (unsigned)pixel(color_buffer[shown], color_pitch, 200, 200));
    // Done: forget the test target and texture.
    auto it = targets_.find(kAddr);
    if (it != targets_.end()) {
        if (bound_ == &it->second) bound_ = nullptr;
        vram_.free(it->second.color);
        targets_.erase(it);
    }
    if (tex.backend_data) { RsxTex* rt = (RsxTex*)tex.backend_data; vram_.free(rt->mem); delete rt; tex.backend_data = nullptr; }
    last_present_origin_ = 0;
    fence_draws = 300;
    std::printf("[gfx] self-test finished%s\n", pixels_ok ? "" : " (wrong pixels)");
    std::fflush(stdout);
    return ok && pixels_ok;
}

}  // namespace n64gfx
