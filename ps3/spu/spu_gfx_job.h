// The job block shared by the PPU and the SPU graphics program (ps3/spu/gfx): the SPU interprets a
// display list and writes a draw stream (ps3/gfx/n64gfx_stream.h) to out_ea.
#pragma once
#include <stdint.h>

#define SPU_GFX_CMD_RUN 1
#define SPU_GFX_CMD_QUIT 2
#define SPU_GFX_CMD_FORGET 3  // the PPU emptied its texture cache: send every texture again

// Read by the SPU at the start of a job (128 bytes, 128-aligned).
struct SpuGfxJob {
    uint64_t rdram_ea;   // game memory (8 MB, big-endian)
    uint64_t out_ea;     // the stream (128-aligned)
    uint64_t progress_ea;  // where the SPU writes SpuGfxProgress (128-aligned)
    uint32_t out_words;  // room at out_ea
    uint32_t dl_addr;
    uint32_t seq;
    uint32_t frame_textures;
    // Frame interpolation (interp != 0): the SPU blends each matrix / object vertex load with last
    // display list's (found in the PPU's table: hist_slots entries of SpuHistEntry, those of generation
    // prev_gen valid) and logs this display list's to log_ea (SpuHistRecord after SpuHistRecord, up
    // to log_bytes), which the PPU turns into the next table.
    uint32_t interp;
    uint32_t prev_gen;
    uint32_t hist_slots;  // a power of 2
    uint64_t hist_ea;
    uint64_t log_ea;
    uint32_t log_bytes;
    uint32_t pad[13];
} __attribute__((aligned(128)));

// The PPU's table of last display list's matrices and vertex loads (open addressing by key).
struct SpuHistEntry {
    uint64_t key;
    uint32_t gen;      // valid when it's the job's prev_gen
    uint32_t bytes;    // of data
    uint64_t data_ea;  // 16-byte aligned
    uint64_t pad;
} __attribute__((aligned(16)));

// A record of the SPU's log: this header, then `bytes` of data (a 4x4 float matrix, or vertices as
// 4 int16 each), padded to 16 bytes.
struct SpuHistRecord {
    uint64_t key;
    uint32_t bytes;
    uint32_t pad;
} __attribute__((aligned(16)));

inline uint32_t spu_hist_hash(uint64_t key) {
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 40);
}

// Written by the SPU while it works (words, so the PPU can play what's there) and when it's done.
struct SpuGfxProgress {
    uint32_t seq;        // the job these numbers are for
    uint32_t words;      // stream words written so far
    uint32_t done;       // 1 when the stream is complete (it ends with END or OVERFLOW)
    uint32_t ticks;      // decrementer ticks (79.8 MHz) the display list took
    uint32_t misses;     // memory cache misses
    uint32_t commands, triangles, textures_sent;
    uint32_t log_used;   // frame interpolation: bytes of log written
    uint32_t matrices, matrices_blended, vertex_loads, vertex_loads_blended;
    uint32_t camera_cut;
    uint32_t clipped, clipped_away;  // triangles crossing a near plane, and wholly behind one
    uint32_t pad[16];
} __attribute__((aligned(128)));
