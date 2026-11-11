// PPU side of the SPU graphics program (see spu_gfx.h and ps3/spu/gfx/spu_gfx_main.cpp).
#include "spu_gfx.h"

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <ppu_intrinsics.h>
#include <sys/systime.h>
#include <unistd.h>

#include "../spu/spu_gfx_job.h"


SpuGfx* SpuGfx::instance = nullptr;

bool SpuGfx::start() {
    if (running()) return true;
    if (!job_) {
        job_ = (SpuGfxJob*)memalign(128, sizeof(SpuGfxJob));
        progress_ = (SpuGfxProgress*)memalign(128, sizeof(SpuGfxProgress));
        if (!out_[0]) { out_[0] = (uint32_t*)memalign(128, kOutWords * 4); out_[1] = (uint32_t*)memalign(128, kOutWords * 4); out_words_ = kOutWords; }
        if (!job_ || !progress_ || !out_[0] || !out_[1]) { std::printf("[spu] %s: out of memory\n", spu_.name()); return false; }
        std::memset(job_, 0, sizeof *job_);
        std::memset(progress_, 0, sizeof *progress_);
        table_ = (SpuHistEntry*)memalign(128, kHistSlots * sizeof(SpuHistEntry));
        log_[0] = (uint8_t*)memalign(128, kLogBytes);
        log_[1] = (uint8_t*)memalign(128, kLogBytes);
        if (!table_ || !log_[0] || !log_[1]) { std::printf("[spu] %s: no memory for frame interpolation\n", spu_.name()); table_ = nullptr; }
        else std::memset(table_, 0, kHistSlots * sizeof(SpuHistEntry));
    }
    return spu_.start(job_);
}

void SpuGfx::forget_textures() { spu_.send(SPU_GFX_CMD_FORGET); }

bool SpuGfx::begin(uint8_t* rdram, uint32_t dl_addr, bool frame_textures, bool interp) {
    if (!running()) return false;
    volatile SpuGfxJob* job = job_;
    volatile SpuGfxProgress* pr = progress_;
    job->rdram_ea = (uint64_t)(uintptr_t)rdram;
    cur_ ^= 1;  // the buffer the RSX may still read from the list before last's turn (the renderer waited)
    job->out_ea = (uint64_t)(uintptr_t)out_[cur_];
    job->progress_ea = (uint64_t)(uintptr_t)progress_;
    job->out_words = out_words_;
    job->dl_addr = dl_addr;
    job->seq = ++seq_;
    job->frame_textures = frame_textures ? 1 : 0;
    interp = interp && table_;
    job->interp = interp ? 1 : 0;
    job->prev_gen = prev_gen_;
    job->hist_slots = kHistSlots;
    job->hist_ea = (uint64_t)(uintptr_t)table_;
    job->log_ea = interp ? (uint64_t)(uintptr_t)log_[log_cur_] : 0;
    job->log_bytes = kLogBytes;
    pr->done = 0;
    pr->words = 0;
    __sync();  // the job block and game memory are visible to the SPU's DMA
    t0_ = __mftb();
    return spu_.send(SPU_GFX_CMD_RUN);
}

bool SpuGfx::poll(uint32_t* words, bool* done) {
    volatile SpuGfxProgress* pr = progress_;
    // The SPU writes the progress block after the stream data it counts (fenced).
    const bool mine = pr->seq == seq_;
    const uint32_t w = mine ? pr->words : 0;
    const bool d = mine && pr->done;
    __lwsync();
    *words = w;
    *done = d;
    if (d) {
        last_ticks = pr->ticks;
        last_misses = pr->misses;
        last_commands = pr->commands;
        last_triangles = pr->triangles;
        last_textures_sent = pr->textures_sent;
        last_log = pr->log_used;
        last_matrices = pr->matrices;
        last_matrices_blended = pr->matrices_blended;
        last_vertex_loads = pr->vertex_loads;
        last_vertex_loads_blended = pr->vertex_loads_blended;
        last_camera_cut = pr->camera_cut;
        last_clipped = pr->clipped;
        last_clipped_away = pr->clipped_away;
        return true;
    }
    if (__mftb() - t0_ > 2 * sysGetTimebaseFrequency()) {
        std::printf("[spu] %s: the SPU didn't finish display list %u within 2 s; stopping it, drawing on the PPU\n", spu_.name(),
                    (unsigned)seq_);
        spu_.stop();
        return false;
    }
    return true;
}

void SpuGfx::commit_history(bool interp) {
    if (!table_) return;
    if (!interp || !last_log) { prev_gen_ = 0; return; }  // nothing to blend with next time
    // The log the SPU wrote becomes the table's entries for a new generation (older ones count as empty).
    const uint32_t gen = ++gen_;
    const uint8_t* log = log_[log_cur_];
    for (uint32_t pos = 0; pos + sizeof(SpuHistRecord) <= last_log;) {
        const SpuHistRecord* r = (const SpuHistRecord*)(log + pos);
        const uint32_t padded = (r->bytes + 15) & ~15u;
        if (pos + sizeof(SpuHistRecord) + padded > last_log) break;
        uint32_t slot = spu_hist_hash(r->key) & (kHistSlots - 1);
        for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (kHistSlots - 1)) {
            SpuHistEntry& e = table_[slot];
            if (e.gen == gen && e.key != r->key) continue;  // taken this generation
            e.key = r->key;
            e.gen = gen;
            e.bytes = r->bytes;
            e.data_ea = (uint64_t)(uintptr_t)(log + pos + sizeof(SpuHistRecord));
            break;
        }
        pos += sizeof(SpuHistRecord) + padded;
    }
    __lwsync();
    prev_gen_ = gen;
    log_cur_ ^= 1;  // the next display list logs into the other one while this one is read
}
