// The PS3 renderer: n64gfx's display list interpreter with the RSX backend, as the runtime's
// RendererContext. Runs on the runtime's graphics thread, which also owns the RSX from then on.
#include <atomic>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "../gfx/n64gfx.h"
#include "../gfx/n64gfx_stream.h"
#include "../gfx/rsx_backend.h"
#include "../gfx/softraster.h"
#include "miniz.h"
#include "spu_gfx.h"
#include "input.h"
#include "settings.h"
#include <fstream>
#include "display.h"
#include "rsxutil.h"
#include <sys/systime.h>
#include "paths.h"
#include "xmb_icon.h"
#include "texture_pack_host.h"
#include "../gfx/rice_hash.h"

std::mutex g_display_mutex;
std::atomic<bool> g_renderer_owns_display{false};
extern std::atomic<bool> g_conker_motion_blur;  // widescreen_hooks.cpp
extern "C" void ps3prof_block(int blocked) __attribute__((weak));

extern "C" void ps3_log_memory(void);  // diag.c

namespace {
constexpr const char* kShotDir = CONKER_DATA_DIR;  // the test builds' screenshots and R3 captures

class Ps3Renderer : public ultramodern::renderer::RendererContext {
public:
    explicit Ps3Renderer(uint8_t* rdram) {
        mem_.rdram = rdram;
        {
            std::lock_guard<std::mutex> lk(g_display_mutex);
            g_renderer_owns_display = true;
        }
        ok_ = backend_.init(context, kScale);
        // A 4 MB command buffer in the IO-mapped memory past the system's (see rsx_backend.h).
        if (ok_ && host_mem && host_mem_size >= 8u * 1024 * 1024)
            backend_.use_command_buffer((uint8_t*)host_mem + 2 * 1024 * 1024, 4 * 1024 * 1024);
        // The vertex ring in the IO-mapped memory past the command buffer (before the self-test, which
        // draws from it): 4 MB, for rectangles and what isn't read from the SPU's streams.
        if (ok_ && host_mem && host_mem_size >= 10u * 1024 * 1024)
            backend_.use_vertex_memory((uint8_t*)host_mem + 6 * 1024 * 1024, 4 * 1024 * 1024);
        if (ok_ && !backend_.self_test() && backend_.stats.vertices_in_main && !backend_.hung()) {
            std::printf("[gfx] self-test failed with vertices in main memory; trying again with RSX memory\n");
            backend_.use_rsx_vertex_memory();
            backend_.self_test();
        }
        if (ok_) backend_.show_black();
        // The settings chosen in the startup menu (settings.h).
        if (ok_ && g_settings.scale >= kMinScale && g_settings.scale <= kMaxScale && g_settings.scale != kScale) backend_.set_scale(g_settings.scale);
        if (ok_ && g_settings.antialias) backend_.set_antialias(true);  // after the self-test (drawn without)
        backend_.n64_look = g_settings.n64_look;
        backend_.fill_screen = g_settings.fill;
        if (stereo_on) {
            // 3D: the settings page's depth, screen depth and ghost cancellation (settings.h); nearer things
            // come out by at most 1.5 times the farthest shift (v99's, kept: v100's 0.5 looked flatter).
            const float depth = kStereoDepth[g_settings.stereo_depth], convergence = kStereoScreen[g_settings.stereo_screen];
            backend_.set_stereo(depth, convergence, 1.5f);
            const bool ghost_ok = backend_.set_ghost(kStereoGhost[g_settings.stereo_ghost]);
            std::printf("[gfx] 3D: depth %.3f, screen depth at w %.0f, nearer out by at most 1.5x, ghost cancellation %.3f%s\n", depth,
                        convergence, kStereoGhost[g_settings.stereo_ghost], ghost_ok ? "" : " (FAILED: off)");
        }
        frame_interp_ = g_settings.fps60;
        select_seen_ = ps3input::select_count();
        l3_seen_ = ps3input::l3_count();
        r3_seen_ = ps3input::r3_count();
        interp_.textures().on_evict = [this](n64gfx::Texture& t) { backend_.texture_evicted(t); };
        // A texture pack: each texture newly decoded is looked up in it by its Rice checksum, worked out
        // from the game memory its load read (the loads come with it from the SPU).
        if (texpack::active()) {
            backend_.pack = texpack::active();
            interp_.textures().on_decode = [this](n64gfx::Texture& t, const n64gfx::Tile& tile, uint32_t tlut, const n64gfx::RiceLoad& load,
                                                  const n64gfx::RiceLoad& pal_load) {
                uint32_t crc = 0, pal = 0, hw = 0, hh = 0;
                bool has_pal = false;
                const bool uses_pal = tile.siz <= 1 && (tile.fmt == 2 || tlut != 0);
                if (!n64gfx::rice_key(mem_.rdram, 0x800000, load, pal_load, tile, t.width, t.height, uses_pal ? 1 : 0, &crc, &pal, &has_pal, &hw, &hh))
                    return;
                n64gfx::TexturePack::Job job;
                if (!backend_.pack->find(t.width, t.height, hw, hh, crc, pal, has_pal, &job)) return;
                // What the game's own texture's alpha is (all the game uses of the picture's): solid or a
                // cutout halve the picture's size (DXT1 instead of DXT5).
                {
                    bool solid = true, binary = true;
                    for (size_t i = 3, n = (size_t)t.width * t.height * 4; i < n && binary; i += 4) {
                        const uint8_t a = t.rgba[i];
                        if (a != 255) { solid = false; if (a != 0) binary = false; }
                    }
                    job.alpha = solid ? n64gfx::TexturePack::kAlphaSolid : binary ? n64gfx::TexturePack::kAlphaCutout : n64gfx::TexturePack::kAlphaKeep;
                    backend_.pack->alpha_kinds[job.alpha]++;
                }
                t.hd_state = 1;
                backend_.want_hd(t.hash, job);  // read and prepared in the background, unless it's there already
            };
        }
        setup_result = ok_ ? ultramodern::renderer::SetupResult::Success : ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        start_ = last_report_ = std::chrono::steady_clock::now();
#if !defined(CONKER_RELEASE)
        // Monitor: if the graphics thread stops making progress, say where it is.
        monitor_ = std::thread([this] {
            uint32_t last_cmds = 0, still = 0, reports = 0;
            for (;;) {
                usleep(1000000);
                uint32_t cmds = total_dls_ + backend_.stats.presents + screen_updates_;
                if (cmds != last_cmds) { last_cmds = cmds; still = 0; continue; }
                if (++still < 3 || reports >= 5) continue;
                reports++;
                gcmControlRegister volatile* ctrl = gcmGetControlRegister();
                std::printf("[gfx] STUCK for %u s in: %s, in display list #%u (%s%s); %u commands, %u draws, %u presents; "
                            "RSX put 0x%X get 0x%X ref 0x%X\n",
                            still, (const char*)backend_.phase, total_dls_, in_dl_ ? "running it" : "between lists", in_present_ ? ", presenting" : "",
                            interp_.stats.commands, backend_.stats.draws, backend_.stats.presents,
                            (unsigned)ctrl->put, (unsigned)ctrl->get, (unsigned)ctrl->ref);
                std::printf("[gfx] current command: 0x%08X 0x%08X at 0x%06X; texture cache %lu images\n",
                            (unsigned)interp_.current_w0, (unsigned)interp_.current_w1, (unsigned)interp_.current_pc,
                            (unsigned long)interp_.textures().size());
                std::printf("[gfx] last operations:\n");
                backend_.dump_history(12);
                std::fflush(stdout);
            }
        });
        monitor_.detach();
#endif
        std::printf("[gfx] renderer ready (%s), %u lines (render scale %u), %s\n", ok_ ? "ok" : backend_.last_error.c_str(), 216 * backend_.scale(), backend_.scale(),
                    frame_interp_ ? "60 frames a second" : "30 frames a second");
    }

    bool valid() override { return ok_; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dummy_workload(uint32_t) override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return (float)backend_.scale(); }

    void send_dl(const OSTask* task) override {
        if (!ok_) return;
        if (backend_.hung() && !hang_reported_) {
            hang_reported_ = true;
            std::printf("[gfx] (display list #%u)\n", total_dls_);
        }
        total_dls_++;
        if (total_dls_ <= 5) std::printf("[gfx] display list #%u at 0x%06X\n", total_dls_, (uint32_t)task->t.data_ptr & 0xFFFFFF);
        controls();
        bool capture = false;
#if !defined(CONKER_RELEASE)
        if (snapshots_left_) { snapshots_left_--; capture = true; snapshot((uint32_t)task->t.data_ptr); }
        backend_.debug_mips = capture;
#endif
        auto t0 = std::chrono::steady_clock::now();
#if !defined(CONKER_RELEASE)
        const uint64_t pace_t0 = __builtin_ppc_get_timebase();
        pace_.list_read = ps3input::last_read_tb();
#endif
        const uint32_t failures0 = backend_.stats.alloc_failures;
        uint32_t dl = (uint32_t)task->t.data_ptr & 0x1FFFFFFF;
        in_dl_ = true;
        // On the SPU when it's running (it records a draw stream, played here), else on the PPU.
        bool drawn = false;
        SpuGfx* spu = SpuGfx::instance;
        // Frame interpolation (60 frames a second), except in frames with the motion blur (it blends each
        // frame with its copy of the last: an in-between frame would blend it twice).
        const bool blur = g_conker_motion_blur.exchange(false);
        const bool want_mid = frame_interp_ && !blur;
        // The stream buffer this list gets: the RSX done reading it (the list before last's).
        if (spu && spu->running()) backend_.wait_stream(spu->buffer() ^ 1);
        if (spu && spu->running() && spu->begin(mem_.rdram, dl, interp_.frame_textures, want_mid)) {
            // Play the stream as it arrives (the SPU sends it in 8 KB pieces), sleeping while there's
            // nothing new.
            const uint32_t* stream = spu->stream();
            const char* err = nullptr;
            player_.begin();
            n64gfx::StreamPlayer::Result r = n64gfx::StreamPlayer::kMore;
            uint32_t words = 0, seen = 0;
            bool done = false, alive = true;
            while (r == n64gfx::StreamPlayer::kMore) {
                const uint64_t a = __builtin_ppc_get_timebase();
                alive = spu->poll(&words, &done);
                if (!alive) break;
                if (words > seen) {  // more has arrived (play_more plays the whole records in it)
                    seen = words;
                    r = player_.play_more(stream, words, backend_, &err);
                    spu_.play += __builtin_ppc_get_timebase() - a;
                    continue;
                }
                if (done) { r = n64gfx::StreamPlayer::kFailed; err = "the stream ended without END"; break; }
                if (ps3prof_block) ps3prof_block(1);
                usleep(30);
                if (ps3prof_block) ps3prof_block(0);
                spu_.wait += __builtin_ppc_get_timebase() - a;
            }
            if (!alive) {
                std::printf("[gfx] display list #%u: the SPU stopped; drawing on the PPU from now on\n", total_dls_);
            } else {
                // Wait for the SPU's figures (it finishes just after writing END).
                while (!done && spu->poll(&words, &done)) usleep(10);
                if (r == n64gfx::StreamPlayer::kDone) {
                    drawn = true;
                    spu_.dls++;
                    // A camera cut (most matrices too far from last frame's to blend): no in-between frame.
                    const bool cut = spu->last_camera_cut || (spu->last_matrices && spu->last_matrices_blended * 2 < spu->last_matrices);
                    if (want_mid && cut) spu_.cuts++;
                    if (want_mid && !cut) {
                        // The in-between frame: the stream again, with each vertex's second position,
                        // into the frame's in-between copy (shown first, at the next VI).
                        const uint64_t m0 = __builtin_ppc_get_timebase();
                        const char* merr = nullptr;
                        player_.mid_pass = true;
                        backend_.begin_mid();
                        const bool mok = player_.play(stream, words, backend_, &merr);
                        backend_.end_mid();
                        player_.mid_pass = false;
                        spu_.mid += __builtin_ppc_get_timebase() - m0;
                        spu_.mids += mok ? 1 : 0;
                        spu_.matrices += spu->last_matrices; spu_.matrices_blended += spu->last_matrices_blended;
                        spu_.vloads += spu->last_vertex_loads; spu_.vloads_blended += spu->last_vertex_loads_blended;
                    }
                    spu->commit_history(want_mid);
#if !defined(CONKER_RELEASE)
                    if (check_next_) soft_check(stream, words, dl);
                    if (capture) capture_frames(stream, words, want_mid && !cut);
#endif
                } else {
                    spu->commit_history(false);
                    spu_.fallbacks++;
                    if (spu_.fallbacks <= 5)
                        std::printf("[gfx] display list #%u: SPU stream not fully played (%s)%s\n", total_dls_, err,
                                    player_.position() > 64 ? "" : "; drawing it on the PPU");
                    if (player_.position() > 64) drawn = true;  // part of it is drawn already: keep that
                    spu->forget_textures();
                }
                spu_.ticks += spu->last_ticks;
                spu_.words += words;
                spu_.misses += spu->last_misses;
                spu_.tris += spu->last_triangles;
                spu_.clipped += spu->last_clipped;
                spu_.clipped_away += spu->last_clipped_away;
                spu_.commands += spu->last_commands;
                spu_.textures += spu->last_textures_sent;
                spu_.missing += player_.missing_textures;
                player_.missing_textures = 0;
            }
        }
        if (!drawn) {
            interp_.run(mem_, dl, backend_);
            if (spu && spu->running()) spu->forget_textures();  // its idea of the texture cache is out of date
        }
        in_dl_ = false;
        backend_.lists_since_present++;
        if (spu && drawn) backend_.fence_stream(spu->buffer());
        backend_.mark_list_end();
        // Out of RSX memory at this resolution: one step down.
        if (backend_.stats.alloc_failures != failures0 && backend_.scale() > kMinScale) {
            std::printf("[gfx] not enough RSX memory for %u lines: going down a step\n", 216 * backend_.scale());
            backend_.set_scale(backend_.scale() - 1);
        }
        interp_.textures().end_frame();
        dl_time_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        dls_++;
#if !defined(CONKER_RELEASE)
        {   // The game's step for this list (read now: the game sets it just after starting the task).
            int32_t step;
            std::memcpy(&step, mem_.rdram + 0xBE9E4, 4);
            pace_.tb_ms = (double)sysGetTimebaseFrequency() / 1000.0;
            pace_.list(pace_t0, step, (float)((double)(__builtin_ppc_get_timebase() - pace_t0) / pace_.tb_ms));
            pace_.drawn_read = pace_.list_read;  // drawn: the next real frame shown is this list's
        }
#endif
        report();
    }

    void update_screen() override {
        if (!ok_) return;
        const auto* r = ultramodern::renderer::get_vi_regs();
        n64gfx::RsxBackend::ViInfo vi{r->VI_ORIGIN_REG, r->VI_WIDTH_REG, r->VI_H_START_REG, r->VI_V_START_REG,
                                      r->VI_X_SCALE_REG, r->VI_Y_SCALE_REG};
        auto t0 = std::chrono::steady_clock::now();
        screen_updates_++;
        in_present_ = true;
#if !defined(CONKER_RELEASE)
        const uint32_t held0 = backend_.stats.present_older_waited, busy0 = backend_.stats.present_busy;
        const uint32_t mid0 = backend_.stats.mid_presents, drawn0 = backend_.stats.present_drawn;
        const uint64_t flip0 = backend_.stats.t_flip;
        const bool shown = backend_.present(vi);
        if (shown) pace_.shown((float)((double)(backend_.stats.t_flip - flip0) / ((double)sysGetTimebaseFrequency() / 1000.0)),
                               frame_interp_, backend_.stats.mid_presents != mid0, backend_.stats.present_drawn != drawn0);
        else pace_.add(backend_.stats.present_older_waited != held0 ? 'H' : backend_.stats.present_busy != busy0 ? 'B' : '-', 0, 0);
#else
        backend_.present(vi);
#endif
        in_present_ = false;
        present_time_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
#if defined(CONKER_RELEASE)
        (void)capture_unused_;
        xmb_icon::after_present(backend_, interp_, vi);  // the XMB icon, the first time (xmb_icon.h)
        return;
#endif
        // A screenshot every 5 seconds for the first 2 minutes, for checking the picture remotely.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        // And when frames are first used as textures (motion blur, shadows), a few half a second apart.
        if (backend_.stats.fb_draws != last_fb_draws_ && fb_shots_ < 10 && secs >= next_fb_shot_) {
            last_fb_draws_ = backend_.stats.fb_draws;
            next_fb_shot_ = secs + 0.5;
            char path[128];
            std::snprintf(path, sizeof path, "%s/frametex_%03d_%03ds.png", kShotDir, fb_shots_++, (int)secs);
            bool ok = backend_.screenshot(vi, path);
            std::printf("[gfx] screenshot %s: %s (VI origin 0x%06X)\n", path, ok ? "saved" : "not saved", r->VI_ORIGIN_REG & 0xFFFFFF);
        }
        // Every 5 s for the first 30 s only: each takes about 0.3 s (the game stalls), which showed as a
        // hitch every 30 s in the pacing check (v68).
        if (secs >= next_shot_ && next_shot_ <= 30) {
            next_shot_ += 5;
            char path[128];
            std::snprintf(path, sizeof path, "%s/shot_%03d.png", kShotDir, (int)secs);
            bool ok = backend_.screenshot(vi, path);
            std::printf("[gfx] screenshot %s: %s (VI origin 0x%06X)\n", path, ok ? "saved" : "not saved", r->VI_ORIGIN_REG & 0xFFFFFF);
            // At the first screenshot (a still title screen, so the pause doesn't show), the next display
            // list is also drawn in software (check_*.png).
            if (secs < 6) { check_next_ = true; check_secs_ = (int)secs; backend_.describe_targets(); }
        }
    }

private:
    static constexpr uint32_t kScale = 2, kMinScale = 2, kMaxScale = 5;  // 432, 648, 864, 1080 lines
    uint64_t direct_reported_ = 0, vertices_reported_ = 0;
    uint32_t select_seen_ = 0, l3_seen_ = 0, r3_seen_ = 0, snapshots_left_ = 0, snapshot_n_ = 0;

    // R3: the game memory (16 MB, as the SPU reads it) at the start of the next 3 display lists, each
    // saved as a 2048x2048 RGBA PNG (the logs script fetches PNGs): tools/spu-sim/png_to_snap.py
    // turns them into gfx_replay_sim's snapshots, which redraw those frames and their in-between frames.
    // R3, after each captured display list: what the RSX drew (the frame and its in-between frame) and
    // the in-between frame drawn in software from the same stream (capture_N_*.png).
    void capture_frames(const uint32_t* stream, uint32_t words, bool had_mid) {
        const uint32_t n = snapshot_n_ - 1;
        char path[160];
        for (uint32_t addr : backend_.frame_target_addrs()) {
            std::snprintf(path, sizeof path, "%s/capture_%u_rsx_%06X.png", kShotDir, n, addr);
            const bool a = backend_.save_target(addr, false, path);
            std::snprintf(path, sizeof path, "%s/capture_%u_rsx_mid_%06X.png", kShotDir, n, addr);
            const bool b = had_mid && backend_.save_target(addr, true, path);
            std::printf("[gfx] R3: frame 0x%06X: RSX %s, in-between %s\n", addr, a ? "saved" : "not saved", b ? "saved" : had_mid ? "not saved" : "none drawn");
        }
        backend_.dump_mips(kShotDir, n);
        backend_.debug_mips = false;
        if (!had_mid) return;
        auto soft = std::make_unique<n64gfx::SoftRaster>();
        auto player = std::make_unique<n64gfx::StreamPlayer>(interp_.textures());
        player->mid_pass = true;
        const char* err = nullptr;
        player->play(stream, words, *soft, &err);
        const int saved = check_secs_;
        check_secs_ = 10000 + (int)n;  // check_1000N_soft_mid_*.png
        save_soft(*soft, "soft_mid");
        check_secs_ = saved;
    }

    void snapshot(uint32_t dl) {
        const uint64_t t0 = __builtin_ppc_get_timebase();
        size_t len = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(mem_.rdram, 2048, 2048, 4, &len, 1, MZ_FALSE);
        char path[160];
        std::snprintf(path, sizeof path, "%s/snapshot_%u_%06X.png", kShotDir, snapshot_n_++, dl & 0xFFFFFF);
        FILE* f = png ? std::fopen(path, "wb") : nullptr;
        if (f) { std::fwrite(png, 1, len, f); std::fclose(f); }
        if (png) mz_free(png);
        std::printf("[gfx] R3: %s %s (%.0f KB, %.0f ms)\n", path, f ? "saved" : "NOT saved", len / 1024.0,
                    (__builtin_ppc_get_timebase() - t0) * 1000.0 / sysGetTimebaseFrequency());
    }

    // The controller's extra buttons: L3, 60 / 30 frames a second; R3, captures (test builds).
    void controls() {
        const uint32_t sel = ps3input::select_count(), l3 = ps3input::l3_count(), r3 = ps3input::r3_count();
#if defined(CONKER_RELEASE)
        // The release build: no R3 captures, no 60 fps.
        r3_seen_ = r3;
        l3_seen_ = l3;
#else
        if (r3 != r3_seen_) { r3_seen_ = r3; snapshots_left_ = 3; }
#endif
        select_seen_ = sel;  // (Select is half of the quit combo now, Select + Start: see input.cpp)
        if (l3 != l3_seen_ && stereo_on) {
            l3_seen_ = l3;
            std::printf("[gfx] L3: 3D stays at 30 frames a second\n");
        } else if (l3 != l3_seen_) {
            l3_seen_ = l3;
            frame_interp_ = !frame_interp_;
            g_settings.fps60 = frame_interp_;
            save_settings();
            std::printf("[gfx] L3: %s\n", frame_interp_ ? "60 frames a second" : "30 frames a second");
        }
    }

    // A check of the picture: the display list just drawn, drawn again in software twice: from the SPU's
    // stream (check_NNN_spu.png) and by the PPU's own interpreter (check_NNN_ppu.png). Matching the
    // screenshot: the RSX drew what both say. Both right and the screenshot wrong: the RSX side.
    // The SPU's wrong only: its stream.
    void soft_check(const uint32_t* stream, uint32_t words, uint32_t dl) {
        check_next_ = false;
        const uint64_t t0 = __builtin_ppc_get_timebase();
        auto from_spu = std::make_unique<n64gfx::SoftRaster>();
        auto from_ppu = std::make_unique<n64gfx::SoftRaster>();
        auto player = std::make_unique<n64gfx::StreamPlayer>(interp_.textures());
        const char* err = nullptr;
        const bool ok = player->play(stream, words, *from_spu, &err);
        interp_.run(mem_, dl, *from_ppu);
        // The PPU's interpreter changed the texture cache the SPU keeps track of.
        if (SpuGfx::instance) SpuGfx::instance->forget_textures();
        const double ms = (__builtin_ppc_get_timebase() - t0) * 1000.0 / sysGetTimebaseFrequency();
        std::printf("[gfx] check %03d: display list 0x%06X drawn in software from the SPU's stream (%u words, %s) and by the PPU (%.0f ms)\n",
                    check_secs_, dl, words, ok ? "ok" : err, ms);
        save_soft(*from_spu, "spu");
        save_soft(*from_ppu, "ppu");
    }
    void save_soft(const n64gfx::SoftRaster& r, const char* who) {
        for (const auto& [addr, t] : r.targets) {
            if (t.width < 256 || !t.max_y) continue;
            const uint32_t w = t.stride, h = std::min<uint32_t>(t.max_y, t.height);
            uint64_t sum = 0;
            for (size_t i = 0; i < (size_t)w * h * 4; i++) if ((i & 3) != 3) sum += t.rgba[i];
            std::vector<uint8_t> rgba(t.rgba.begin(), t.rgba.begin() + (size_t)w * h * 4);
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            size_t len = 0;
            void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), (int)w, (int)h, 4, &len, 6, MZ_FALSE);
            char path[160];
            std::snprintf(path, sizeof path, "%s/check_%03d_%s_%06X.png", kShotDir, check_secs_, who, addr);
            FILE* f = png ? std::fopen(path, "wb") : nullptr;
            if (f) { std::fwrite(png, 1, len, f); std::fclose(f); }
            if (png) mz_free(png);
            std::printf("[gfx]   %s: image 0x%06X %ux%u (+%u each side), %u rows drawn, brightness %.1f: %s\n", who, addr, t.width, t.height,
                        t.margin, t.max_y, w * h ? sum / (3.0 * w * h) : 0.0, f ? path : "not saved");
        }
    }
    bool check_next_ = false;
    int check_secs_ = 0;
    bool capture_unused_ = false;

    void report() {
#if defined(CONKER_RELEASE)
        dls_ = 0; dl_time_ = 0; present_time_ = 0;  // no reports in the release build
        return;
#endif
        auto now = std::chrono::steady_clock::now();
        if (now - last_report_ < std::chrono::seconds(10)) return;
        double span = std::chrono::duration<double>(now - last_report_).count();
        last_report_ = now;
        const auto& is = interp_.stats;
        auto& bs = backend_.stats;
        ps3input::report();
        std::printf("[gfx]   RSX: %u lines, %s; it finished each display list %.2f ms after the PPU handed over the last of it "
                    "(at most %.2f ms); frame copies used up to %u KB; %.0f%% of vertices read in the stream itself, %u waits for a stream buffer; mipmapped: %u draws, %u textures (totals); render targets freed %u, depth buffers %u, old-film frames in grey %u (totals); tile regions: %u in use, %u bound, %u sizes without one (totals)\n",
                    216 * backend_.scale(), frame_interp_ ? "60 fps" : "30 fps",
                    bs.rsx_lag_n ? bs.rsx_lag_sum_ms / bs.rsx_lag_n : 0.0, bs.rsx_lag_max_ms, bs.scratch_max_kb,
                    bs.vertices ? 100.0 * (double)(bs.direct_vertices - direct_reported_) / (double)(bs.vertices - vertices_reported_) : 0.0, bs.stream_waits, bs.mip_draws, bs.mip_textures, bs.targets_freed, bs.depths_freed, bs.film_frames, backend_.tiles_in_use(), backend_.tiles_bound(), backend_.tiles_refused());
        if (auto* tp = texpack::active())
            std::printf("[gfx]   texture pack: %u of %u textures looked up were in it, %u prepared, %u uploaded, %u freed for room (totals); "
                        "their own alpha: %u solid, %u cutout, %u soft; the pictures take %.1f of %.1f MB; %u draws had it though their decode found no key\n",
                        tp->attached, tp->looked_up, tp->prepared, backend_.stats.hd_textures, backend_.stats.hd_evicted,
                        tp->alpha_kinds[1], tp->alpha_kinds[2], tp->alpha_kinds[0], backend_.hd_bytes() / 1048576.0, backend_.hd_budget() / 1048576.0, backend_.stats.hd_without_key);
        direct_reported_ = bs.direct_vertices; vertices_reported_ = bs.vertices;
        bs.rsx_lag_sum_ms = 0; bs.rsx_lag_max_ms = 0; bs.rsx_lag_n = 0;

        std::printf("[gfx] last %.0f s: %u display lists (%.1f/s), %.2f ms each to process; presents %u; "
                    "totals: %u tris, %u rects, %u fills, %u unknown commands (last 0x%02X), %u draws, "
                    "%u programs (%u failed), %u textures uploaded (%lu cached), %u targets, %u ring waits, RSX memory %u of %u MB used, %u allocation failures, %u command buffer wraps%s%s\n",
                    span, dls_, dls_ / span, dls_ ? 1000.0 * dl_time_ / dls_ : 0.0, bs.presents,
                    is.triangles, is.rects, is.fills, is.unknown, (unsigned)interp_.last_unknown_opcode,
                    bs.draws, bs.programs, bs.program_failures, bs.textures_uploaded, (unsigned long)interp_.textures().size(),
                    bs.targets, bs.segment_waits, bs.vram_used_kb / 1024, bs.vram_mb, bs.alloc_failures, bs.cmd_wraps, backend_.last_error.empty() ? "" : "; error: ",
                    backend_.last_error.c_str());
        std::printf("[gfx]   texture cache: %lu images, %.1f MB (at most %u MB), %u dropped for the cap (total)\n", (unsigned long)interp_.textures().size(),
                    interp_.textures().bytes() / 1048576.0, (unsigned)(n64gfx::TextureCache::kMaxBytes >> 20), interp_.textures().capped);
        ps3_log_memory();
        report_screen_timing();
        // Where the graphics thread's time went (ms per display list, this interval).
        {
            const double tb = (double)sysGetTimebaseFrequency() / 1000.0;
            const double n = dls_ ? (double)dls_ : 1.0;
            auto d = [&](uint64_t now_v, uint64_t& prev) { double v = (double)(now_v - prev) / tb / n; prev = now_v; return v; };
            double total = d(is.t_total, prev_.total), vert = d(is.t_vertex, prev_.vertex), tex = d(is.t_texture, prev_.texture);
            double back = d(is.t_backend, prev_.backend), wait = d(bs.t_wait, prev_.wait), upl = d(bs.t_upload, prev_.upload);
            double flp = d(bs.t_flip, prev_.flip), tris = d(is.t_tris, prev_.tris), fill = d(bs.t_fill, prev_.fill);
            uint32_t looks = is.texture_lookups - prev_.lookups, hashes = is.texture_hashes - prev_.hashes;
            uint32_t draws = bs.draws - prev_.draws, verts = is.vertices - prev_.vertices;
            prev_.lookups = is.texture_lookups; prev_.hashes = is.texture_hashes; prev_.draws = bs.draws; prev_.vertices = is.vertices;
            std::printf("[gfx]   per display list: %.2f ms total = vertices %.2f + triangles %.2f + textures %.2f + backend %.2f (of which "
                        "RSX waits %.2f, texture uploads %.2f, vertex writing %.2f) + other %.2f; flips %.2f ms; %.0f draws, %.0f vertices, "
                        "%.0f texture lookups (%.0f hashed); vertex data in %s memory; frame textures %u drawn, %u not found (totals)\n",
                        total, vert, tris, tex, back, wait, upl, fill, total - vert - tris - tex - back, flp, draws / n, verts / n,
                        looks / n, hashes / n, bs.vertices_in_main ? "main" : "RSX", bs.fb_draws, bs.fb_missing);
        }
        if (spu_.dls || spu_.fallbacks) {
            const double tb = (double)sysGetTimebaseFrequency() / 1000.0;
            const double n = spu_.dls + spu_.fallbacks;
            spu_.missing_total += spu_.missing;
            if (spu_.mids)
                std::printf("[gfx]   60 fps: %u in-between frames drawn (%.2f ms each), %u shown, %u camera cuts; matrices %.0f%% and object vertex loads %.0f%% "
                            "blended with last frame's\n", spu_.mids, spu_.mid / tb / spu_.mids, backend_.stats.mid_presents - mid_presents_reported_, spu_.cuts,
                            spu_.matrices ? 100.0 * spu_.matrices_blended / spu_.matrices : 0.0, spu_.vloads ? 100.0 * spu_.vloads_blended / spu_.vloads : 0.0);
            mid_presents_reported_ = backend_.stats.mid_presents;
            std::printf("[gfx]   SPU: %u display lists (%u drawn on the PPU instead): %.2f ms each on the SPU, the graphics thread "
                        "slept %.2f ms waiting for it and played the stream in %.2f ms (while it worked); %.0f commands, %.0f tris, %.0f KB of stream, %.0f memory "
                        "cache misses, %.1f textures sent per list; %u textures missing (total); behind the camera: %.1f tris clipped, %.1f dropped per list\n",
                        spu_.dls, spu_.fallbacks, spu_.ticks / tb / n, spu_.wait / tb / n, spu_.play / tb / n, spu_.commands / n,
                        spu_.tris / n, spu_.words * 4.0 / 1024.0 / n, spu_.misses / n, spu_.textures / n, spu_.missing_total, spu_.clipped / n, spu_.clipped_away / n);
            const uint32_t total_missing = spu_.missing_total;
            spu_ = SpuStats{};
            spu_.missing_total = total_missing;
        }
        dls_ = 0;
        dl_time_ = 0;
        present_time_ = 0;
    }
    struct { uint64_t total = 0, vertex = 0, texture = 0, backend = 0, wait = 0, upload = 0, flip = 0, tris = 0, fill = 0;
             uint32_t lookups = 0, hashes = 0, draws = 0, vertices = 0; } prev_;

    struct SpuStats { uint64_t wait = 0, play = 0, ticks = 0, words = 0, misses = 0, tris = 0, clipped = 0, clipped_away = 0, commands = 0, textures = 0;
                      uint64_t mid = 0, matrices = 0, matrices_blended = 0, vloads = 0, vloads_blended = 0;
                      uint32_t dls = 0, fallbacks = 0, missing = 0, missing_total = 0, mids = 0, cuts = 0; } spu_;
    uint32_t mid_presents_reported_ = 0;
    // How long each picture stayed on the screen, from the flips' times (rsxutil's flip handler):
    // in refreshes of the display (60 Hz). At 30 fps every picture stays 2.
    uint32_t flips_seen_ = 0;
    uint32_t drawn0_ = 0, older0_ = 0, moved0_ = 0, waited0_ = 0, busy0_ = 0;
    uint64_t last_flip_time_ = 0;
    void report_screen_timing() {
        const uint32_t count = g_flip_count;
        __asm__ volatile("lwsync" ::: "memory");
        uint32_t from = flips_seen_;
        if (count - from > kFlipRing - 1) from = count - (kFlipRing - 1);  // too many to have kept
        const double tb_ms = (double)sysGetTimebaseFrequency() / 1000.0, refresh_ms = 1000.0 / 59.94;
        uint32_t held[5] = {0, 0, 0, 0, 0};  // 1, 2, 3, 4, 5+ refreshes
        double longest = 0;
        uint32_t n = 0;
        for (uint32_t i = from; i != count; i++) {
            const uint64_t t = g_flip_times[i % kFlipRing];
            if (last_flip_time_ && t > last_flip_time_) {
                const double ms = (double)(t - last_flip_time_) / tb_ms;
                int r = (int)(ms / refresh_ms + 0.5);
                if (r < 1) r = 1;
                held[r > 5 ? 4 : r - 1]++;
                if (ms > longest) longest = ms;
                n++;
            }
            last_flip_time_ = t;
        }
        flips_seen_ = count;
        if (!n) return;
        auto& bs = backend_.stats;
        std::printf("[gfx]   screen: %u new pictures; each stayed up 1 refresh: %u, 2: %u, 3: %u, 4: %u, 5 or more: %u "
                    "(longest %.1f ms; at 30 fps all stay 2); shown because: drawn %u, VI moved to an older frame %u, same frame at another row %u; "
                    "older pictures held back %u times; VIs left out with the last picture not yet on screen %u\n",
                    n, held[0], held[1], held[2], held[3], held[4], longest, bs.present_drawn - drawn0_, bs.present_older - older0_, bs.present_moved - moved0_,
                    bs.present_older_waited - waited0_, bs.present_busy - busy0_);
        drawn0_ = bs.present_drawn; older0_ = bs.present_older; moved0_ = bs.present_moved; waited0_ = bs.present_older_waited; busy0_ = bs.present_busy;
#if !defined(CONKER_RELEASE)
        pace_.report();
#endif
    }
#if !defined(CONKER_RELEASE)
    // The pacing check (v68). Two questions, answered in the log every 10 s:
    //  - Does the game's own clock keep up with real time? The game steps its world by the VIs (60ths of
    //    a second) its scheduler counted between one graphics task and the next (D_800BE9E4, used all
    //    over the game for movement and timers; 2 at 30 fps). The steps added up against the time that
    //    really passed between the display lists: 100% = the camera and characters move at the right speed.
    //  - When a picture stays up an uneven time, what happened around it: the last events (display
    //    lists and the VI's screen updates) with their times, for the first few each 10 s.
    struct Pace {
        struct Ev { uint64_t tb; char kind; uint8_t step; float ms; };  // 'L' list (ms to process), 'S' shown (ms in flip), 'H' held back, '-' nothing new
        static constexpr uint32_t kRing = 24;
        Ev ring[kRing];
        uint32_t n = 0, dumps = 0, dump_in = 0;
        double dump_gap = 0;
        uint64_t last_list = 0, last_shown = 0;
        uint32_t steps[5] = {0, 0, 0, 0, 0}, lists = 0, off = 0, capped = 0;
        double sum_steps = 0, sum_real_ms = 0, worst_real = 0;
        int worst_step = 0;
        double tb_ms = 1;
        void add(char kind, uint8_t step, float ms) { ring[n++ % kRing] = Ev{__builtin_ppc_get_timebase(), kind, step, ms}; if (dump_in && --dump_in == 0) dump(); }
        void list(uint64_t t_start, int step, float ms) {
            if (last_list && step > 0 && step < 11) {  // (the game itself steps 2 after a long pause)
                const double real = (double)(t_start - last_list) / tb_ms;
                if (real < 500) {
                    lists++;
                    steps[step > 5 ? 4 : step - 1]++;
                    sum_steps += step; sum_real_ms += real;
                    const double d = real - step * (1000.0 / 60.0);
                    if (d > 8.4 || d < -8.4) { off++; if ((d < 0 ? -d : d) > (worst_real - worst_step * 16.667 < 0 ? worst_step * 16.667 - worst_real : worst_real - worst_step * 16.667)) { worst_real = real; worst_step = step; } }
                } else capped++;
            }
            last_list = t_start;
            ring[n++ % kRing] = Ev{t_start, 'L', (uint8_t)step, ms};
            if (dump_in && --dump_in == 0) dump();
        }
        // Pictures by the time since the one before (1, 2, 3 or more VIs), in-between frames among them;
        // and the input lag: from the game's last controller read before a list to its frame on screen.
        uint64_t list_read = 0, drawn_read = 0;
        uint32_t gaps[3] = {0, 0, 0}, mids = 0, lag_n = 0;
        double lag_sum = 0, lag_max = 0;
        bool fps60 = false;
        void shown(float flip_ms, bool at60, bool mid, bool drawn) {
            const uint64_t now = __builtin_ppc_get_timebase();
            const double gap = last_shown ? (double)(now - last_shown) / tb_ms : 33.3;
            last_shown = now;
            add(mid ? 'M' : 'S', 0, flip_ms);
            if (gap < 400) gaps[gap < 25 ? 0 : gap < 42 ? 1 : 2]++;
            if (mid) mids++;
            fps60 = at60;
            if (drawn && !mid && drawn_read) {  // a frame's first showing: its list's controller read
                const double lag = (double)(now - drawn_read) / tb_ms;
                if (lag < 500) { lag_n++; lag_sum += lag; if (lag > lag_max) lag_max = lag; }
            }
            // A picture out of step: at 30 fps one VI or over 2.5 after the last; at 60 fps over 1.5.
            const bool odd = at60 ? gap > 25 : (gap < 25 || gap > 42);
            if (odd && gap < 400 && dumps < 4 && !dump_in) { dumps++; dump_in = 5; dump_gap = gap; }
        }
        void dump() {
            char line[1400];
            int at = 0;
            const uint32_t count = n < kRing ? n : kRing;
            const uint64_t end = ring[(n - 1) % kRing].tb;
            for (uint32_t i = n - count; i != n && at < (int)sizeof line - 60; i++) {
                const Ev& e = ring[i % kRing];
                const double t = -(double)(end - e.tb) / tb_ms;
                if (e.kind == 'L') at += std::snprintf(line + at, sizeof line - at, " | %.1f list %.1fms step %u", t, e.ms, e.step);
                else if (e.kind == 'S') at += std::snprintf(line + at, sizeof line - at, " | %.1f SHOWN (flip %.1fms)", t, e.ms);
                else if (e.kind == 'M') at += std::snprintf(line + at, sizeof line - at, " | %.1f SHOWN in-between (flip %.1fms)", t, e.ms);
                else at += std::snprintf(line + at, sizeof line - at, " | %.1f vi %s", t, e.kind == 'H' ? "held-back" : e.kind == 'B' ? "left out (last picture not up yet)" : "-");
            }
            std::printf("[pace] a picture came %.1f ms after the one before; around it (ms, list = start of a display list, vi = a screen update):%s\n", dump_gap, line);
        }
        void report() {
            if (lists) {
                std::printf("[pace] last 10 s: the game's clock ran at %.1f%% of real time (it stepped %.0f VIs over %u display lists while %.1f VIs' time passed); "
                            "steps of 1 VI: %u, 2: %u, 3: %u, 4: %u, 5 or more: %u; a step differed from the real time by half a VI or more %u times",
                            sum_steps * (1000.0 / 60.0) / sum_real_ms * 100.0, sum_steps, lists, sum_real_ms * 0.06, steps[0], steps[1], steps[2], steps[3], steps[4], off);
                if (off) std::printf(" (the furthest: a step of %d VIs, %.1f ms)", worst_step, worst_real);
                if (capped) std::printf("; %u pauses over half a second left out", capped);
                std::printf("\n");
            }
            const uint32_t pics = gaps[0] + gaps[1] + gaps[2];
            if (pics)
                std::printf("[pace] last 10 s at %s: %u pictures (%.1f a second), %u of them in-between frames; after the one before: 1 VI %u, 2 VIs %u, "
                            "3 or more %u; input lag (controller read to the frame on screen) %.1f ms on average, at most %.1f ms (%u frames)\n",
                            fps60 ? "60 fps" : "30 fps", pics, pics / 10.0, mids, gaps[0], gaps[1], gaps[2], lag_n ? lag_sum / lag_n : 0.0, lag_max, lag_n);
            gaps[0] = gaps[1] = gaps[2] = mids = lag_n = 0; lag_sum = lag_max = 0;
            for (auto& v : steps) v = 0;
            lists = off = capped = dumps = 0; sum_steps = sum_real_ms = worst_real = 0; worst_step = 0;
        }
    } pace_;
#endif
    // Frame interpolation (60 frames a second), unless the file /dev_hdd0/tmp/conkerps3/30fps exists.
    bool frame_interp_ = true;  // set from the settings

    bool ok_ = false;
    n64gfx::Memory mem_;
    n64gfx::Interpreter interp_;
    n64gfx::StreamPlayer player_{interp_.textures()};
    n64gfx::RsxBackend backend_;
    std::chrono::steady_clock::time_point start_, last_report_;
    double next_shot_ = 5, next_fb_shot_ = 0;
    uint32_t last_fb_draws_ = 0, fb_shots_ = 0;
    uint32_t dls_ = 0, total_dls_ = 0;
    bool hang_reported_ = false;
    volatile bool in_dl_ = false, in_present_ = false;
    volatile uint32_t screen_updates_ = 0;
    std::thread monitor_;
    double dl_time_ = 0, present_time_ = 0;
};
}  // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> ps3_create_renderer(
    uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<Ps3Renderer>(rdram);
}
