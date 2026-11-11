// The job block shared by the PPU (ps3/host/spu_audio.cpp) and the SPU audio program
// (ps3/spu/audio): one audio microcode task. 16-byte aligned, a multiple of 16 bytes (DMA).
#pragma once
#include <stdint.h>

#define SPU_AUDIO_CMD_RUN  1u
#define SPU_AUDIO_CMD_QUIT 2u

struct SpuAudioJob {
    uint64_t rdram_ea;    // PPU: game memory (the runtime's rdram pointer)
    uint64_t dmem_ea;     // PPU: 4 KB DMEM image, 128-byte aligned; in before the task, out after
    uint32_t ucode_addr;  // PPU: the task's microcode address (picks the overlays)
    uint32_t seq;         // PPU: task number
    uint32_t result;      // SPU: RspExitReason
    uint32_t done;        // SPU: = seq once DMEM and game memory are written back
    uint32_t ticks;       // SPU: time the task took (decrementer ticks, timebase rate)
    uint32_t dma_reads, dma_writes;  // SPU: DMA transfers made
    uint32_t pad;
} __attribute__((aligned(16)));
