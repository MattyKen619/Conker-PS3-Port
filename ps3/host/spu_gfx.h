// The display list interpreter on an SPU (ps3/spu/gfx), driven from the PPU: the SPU reads the game's
// display list and writes a draw stream (ps3/gfx/n64gfx_stream.h), which the renderer plays into the
// RSX backend.
#pragma once
#include <cstdint>

#include "spu_audio.h"  // SpuThread

struct SpuGfxJob;
struct SpuGfxProgress;

class SpuGfx {
public:
    bool load(const void* program, uint32_t size, const char* name) { return spu_.load(program, size, name); }
    bool start();
    bool running() const { return spu_.running(); }
    // Starts interpreting the display list at dl_addr on the SPU; false if the SPU isn't running.
    // interp: frame interpolation (the stream also holds the in-between frame's positions).
    bool begin(uint8_t* rdram, uint32_t dl_addr, bool frame_textures, bool interp);
    // After a display list (done): with interpolation, its matrices and vertex loads become the ones the
    // next display list blends with; without, nothing is kept.
    void commit_history(bool interp);
    // The stream being written (valid after begin()).
    const uint32_t* stream() const { return out_[cur_]; }
    // The stream buffer the current display list uses (0 or 1: they alternate).
    uint32_t buffer() const { return cur_; }
    // Two stream buffers of `words` each, in memory the RSX can read (else start() allocates its own).
    void set_out_buffers(uint32_t* a, uint32_t* b, uint32_t words) { out_[0] = a; out_[1] = b; out_words_ = words; }
    // How much of the stream has arrived (words), and whether it's complete. Returns false if the SPU
    // hasn't finished within 2 s of begin() (then it's stopped and running() is false).
    bool poll(uint32_t* words, bool* done);
    // The PPU drew a display list itself or emptied its texture cache: the SPU sends every texture again.
    void forget_textures();
    // About the last display list.
    uint32_t last_ticks = 0, last_misses = 0, last_commands = 0, last_triangles = 0, last_textures_sent = 0;
    uint32_t last_matrices = 0, last_matrices_blended = 0, last_vertex_loads = 0, last_vertex_loads_blended = 0, last_log = 0, last_camera_cut = 0, last_clipped = 0, last_clipped_away = 0;
    static SpuGfx* instance;  // the one main() started (null if none)

private:
    static constexpr uint32_t kOutWords = 1024 * 1024;  // 4 MB of stream
    SpuThread spu_;
    SpuGfxJob* job_ = nullptr;
    SpuGfxProgress* progress_ = nullptr;
    uint32_t* out_[2] = {nullptr, nullptr};
    uint32_t out_words_ = kOutWords, cur_ = 1;
    uint32_t seq_ = 0;
    uint64_t t0_ = 0;
    // Frame interpolation's history: the table (generation-tagged) and two logs, used in turn.
    static constexpr uint32_t kHistSlots = 16384, kLogBytes = 1024 * 1024;
    struct SpuHistEntry* table_ = nullptr;
    uint8_t* log_[2] = {nullptr, nullptr};
    uint32_t log_cur_ = 0, gen_ = 1, prev_gen_ = 0;
};
