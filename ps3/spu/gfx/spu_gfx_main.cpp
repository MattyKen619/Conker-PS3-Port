// The display list interpreter on an SPU: waits for a job in its mailbox, interprets the display list
// with game memory read over DMA (through spucache), and writes the draw stream (n64gfx_stream.h)
// to main memory, publishing its progress as it goes so the PPU can play it meanwhile.
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <string.h>
#include <sys/spu_thread.h>

#include "n64gfx.h"
#include "n64gfx_stream.h"
#include "spu_gfx_job.h"

namespace n64gfx::spucache {
uint32_t tags[kLines];
uint8_t data[kLines][kLineSize] __attribute__((aligned(128)));
}

namespace {
constexpr uint32_t kTagCache = 1, kTagOut = 2;
uint64_t g_rdram_ea = 0;
uint32_t g_misses = 0;
SpuGfxJob g_job;
SpuGfxProgress g_progress;

void wait(uint32_t tag) {
    mfc_write_tag_mask(1u << tag);
    mfc_read_tag_status_all();
}

void cache_clear() {
    for (uint32_t i = 0; i < n64gfx::spucache::kLines; i++) n64gfx::spucache::tags[i] = ~0u;
}

// The stream goes out in chunks: the recorder fills one half of the buffer while the other is sent.
constexpr uint32_t kChunkWords = 2048;  // 8 KB
uint32_t g_buf[2][kChunkWords] __attribute__((aligned(128)));
struct Out : n64gfx::StreamOut {
    uint32_t written = 0;  // words sent
    bool flush(const uint32_t* b, uint32_t n) override {
        // Whole quadwords (the stream buffer is 128-aligned and every flush but the last is whole).
        const uint32_t bytes = (n * 4 + 15) & ~15u;
        if (written * 4 + bytes > g_job.out_words * 4) return false;
        wait(kTagOut);  // the previous chunk (and progress) are out
        mfc_put((volatile void*)b, g_job.out_ea + (uint64_t)written * 4, bytes, kTagOut, 0, 0);
        written += n;
        g_progress.words = written;
        mfc_putf(&g_progress, g_job.progress_ea, 16, kTagOut, 0, 0);  // after the data (fenced)
        return true;
    }
};
}  // namespace

const uint8_t* n64gfx::spucache::miss(uint32_t a) {
    const uint32_t ln = a >> kLineBits, set = ln & (kLines - 1);
    mfc_get(data[set], g_rdram_ea + ((uint64_t)ln << kLineBits), kLineSize, kTagCache, 0, 0);
    wait(kTagCache);
    tags[set] = ln;
    g_misses++;
    return data[set];
}

// Frame interpolation's history (n64gfx::MatrixHistory) over DMA: lookups in the PPU's table of last
// display list's, and this display list's appended to a log in main memory.
constexpr uint32_t kTagHist = 3;
struct SpuHistory : n64gfx::MatrixHistory {
    uint8_t stage[2048] __attribute__((aligned(128)));  // the log, sent in pieces
    uint32_t stage_pos = 0, log_pos = 0;
    bool overflow = false;
    SpuHistEntry entry __attribute__((aligned(16)));
    uint8_t data[512] __attribute__((aligned(16)));

    void start() { stage_pos = log_pos = 0; overflow = false; }
    void flush() {
        if (!stage_pos) return;
        if (log_pos + stage_pos > g_job.log_bytes) { overflow = true; stage_pos = 0; return; }
        mfc_put(stage, g_job.log_ea + log_pos, stage_pos, kTagHist, 0, 0);
        mfc_write_tag_mask(1u << kTagHist);
        mfc_read_tag_status_all();
        log_pos += stage_pos;
        stage_pos = 0;
    }
    void append(uint64_t key, const void* d, uint32_t bytes) {
        const uint32_t padded = (bytes + 15) & ~15u, need = 16 + padded;
        if (stage_pos + need > sizeof stage) flush();
        SpuHistRecord* r = (SpuHistRecord*)(stage + stage_pos);
        r->key = key; r->bytes = bytes; r->pad = 0;
        memcpy(stage + stage_pos + 16, d, bytes);
        stage_pos += need;
    }
    // Last display list's data under key (bytes of it), into `data`.
    bool lookup(uint64_t key, uint32_t bytes) {
        if (!g_job.hist_ea || !g_job.hist_slots) return false;
        uint32_t slot = spu_hist_hash(key) & (g_job.hist_slots - 1);
        for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (g_job.hist_slots - 1)) {
            mfc_get(&entry, g_job.hist_ea + (uint64_t)slot * sizeof(SpuHistEntry), sizeof entry, kTagHist, 0, 0);
            mfc_write_tag_mask(1u << kTagHist);
            mfc_read_tag_status_all();
            if (entry.gen != g_job.prev_gen) return false;  // an empty slot: not there
            if (entry.key != key) continue;
            if (entry.bytes != bytes || bytes > sizeof data) return false;
            mfc_get(data, entry.data_ea, (bytes + 15) & ~15u, kTagHist, 0, 0);
            mfc_write_tag_mask(1u << kTagHist);
            mfc_read_tag_status_all();
            return true;
        }
        return false;
    }
    bool find(uint64_t key, float out[4][4]) override {
        if (!lookup(key, 64)) return false;
        memcpy(out, data, 64);
        return true;
    }
    void store(uint64_t key, const float m[4][4]) override { append(key, m, 64); }
    bool find_vertices(uint64_t key, int16_t (*out)[4], uint32_t n) override {
        if (!lookup(key, n * 8)) return false;
        memcpy(out, data, n * 8);
        return true;
    }
    void store_vertices(uint64_t key, const int16_t (*v)[4], uint32_t n) override { append(key, v, n * 8); }
};
static SpuHistory g_hist;

// The recorder fills g_buf[0] and g_buf[1] alternately (Out::flush waits for the earlier transfer).
static n64gfx::Interpreter g_interp;
static Out g_out;
static n64gfx::Recorder g_rec_obj(&g_buf[0][0], kChunkWords, &g_out, &g_buf[1][0]);
static n64gfx::Recorder* const g_rec = &g_rec_obj;

int main(uint64_t job_ea, uint64_t, uint64_t, uint64_t) {
    spu_write_decrementer(0xFFFFFFFFu);
    g_interp.set_texture_source(g_rec);
    for (;;) {
        const uint32_t cmd = spu_read_in_mbox();
        if (cmd == SPU_GFX_CMD_FORGET) { g_rec->forget_sent(); continue; }
        if (cmd != SPU_GFX_CMD_RUN) break;
        mfc_get(&g_job, job_ea, sizeof g_job, kTagCache, 0, 0);
        wait(kTagCache);
        g_rdram_ea = g_job.rdram_ea;
        g_misses = 0;
        cache_clear();
        memset(&g_progress, 0, sizeof g_progress);
        g_progress.seq = g_job.seq;
        g_out.written = 0;
        g_rec->start();
        g_interp.frame_textures = g_job.frame_textures != 0;
        g_interp.stats = n64gfx::Stats{};
        const uint32_t t0 = spu_read_decrementer();
        n64gfx::Memory mem;
        g_hist.start();
        g_interp.set_matrix_history(g_job.interp ? &g_hist : nullptr);
        g_interp.run(mem, g_job.dl_addr, *g_rec);
        if (g_job.interp) g_hist.flush();
        g_rec->finish();
        if (g_rec->overflowed()) g_rec->forget_sent();  // the PPU never sees this stream's textures
        const uint32_t t1 = spu_read_decrementer();
        wait(kTagOut);
        g_progress.words = g_out.written;
        g_progress.done = 1;
        g_progress.ticks = t0 - t1;  // the decrementer counts down
        g_progress.misses = g_misses;
        g_progress.commands = g_interp.stats.commands;
        g_progress.triangles = g_interp.stats.triangles;
        g_progress.textures_sent = g_rec->textures_sent;
        g_progress.log_used = g_hist.overflow ? 0 : g_hist.log_pos;
        g_progress.matrices = g_interp.stats.matrices;
        g_progress.matrices_blended = g_interp.stats.matrices_blended;
        g_progress.vertex_loads = g_interp.stats.vertex_loads;
        g_progress.vertex_loads_blended = g_interp.stats.vertex_loads_blended;
        g_progress.camera_cut = g_interp.stats.camera_cut;
        g_progress.clipped = g_interp.stats.clipped;
        g_progress.clipped_away = g_interp.stats.clipped_away;
        mfc_put(&g_progress, g_job.progress_ea, sizeof g_progress, kTagOut, 0, 0);
        wait(kTagOut);
    }
    spu_thread_exit(0);
    return 0;
}
