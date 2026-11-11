// The audio microcode on an SPU: waits for a task in its mailbox, fetches the job block and DMEM, runs
// the recompiled microcode (audio_ucode_spu.cpp, generated from RecompiledFuncs/rsp/audio_ucode.cpp)
// with DMEM in local store and game memory reached over DMA, then writes DMEM and the job block back.
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <string.h>
#include <sys/spu_thread.h>

#include "librecomp/rsp.hpp"
#include "spu_audio_job.h"
#include "rsp_tables.h"  // generated

RspExitReason conker_audio_ucode_initial(uint8_t* rdram, RspContext* ctx);
RspExitReason conker_audio_ucode0(uint8_t* rdram, RspContext* ctx);
RspExitReason conker_audio_ucode1(uint8_t* rdram, RspContext* ctx);

uint8_t dmem[0x1000] __attribute__((aligned(128)));
uint16_t rspReciprocals[512];
uint16_t rspInverseSquareRoots[512];

namespace {
constexpr uint32_t kTag = 1;
uint64_t g_rdram_ea = 0;
uint32_t g_reads = 0, g_writes = 0;
uint8_t g_bounce[0x1000 + 64] __attribute__((aligned(128)));
SpuAudioJob g_job;
RspContext g_ctx;

void wait_dma() {
    mfc_write_tag_mask(1u << kTag);
    mfc_read_tag_status_all();
}

// The tables librecomp's rsp.cpp computes at startup, computed when building (gen_tables.py): on the
// SPU the original loop took ~5 billion instructions (seconds).
void constants_init() {
    memcpy(rspReciprocals, kRspReciprocals, sizeof rspReciprocals);
    memcpy(rspInverseSquareRoots, kRspInverseSquareRoots, sizeof rspInverseSquareRoots);
}

// conker_audio_ucode (RecompiledFuncs/rsp/audio_ucode.cpp) without its std::maps: one overlay slot at
// IMEM 0x1238, overlays at offsets 0x1B8 and 0xF70 of the microcode.
RspExitReason run_ucode(uint32_t ucode_addr) {
    memset(&g_ctx, 0, sizeof g_ctx);
    RspExitReason er = conker_audio_ucode_initial(nullptr, &g_ctx);
    while (er == RspExitReason::SwapOverlay) {
        if (g_ctx.dma_mem_address != 0x1238) return RspExitReason::Unsupported;
        const uint32_t off = g_ctx.dma_dram_address - ucode_addr;
        if (off == 0x01B8) er = conker_audio_ucode0(nullptr, &g_ctx);
        else if (off == 0x0F70) er = conker_audio_ucode1(nullptr, &g_ctx);
        else return RspExitReason::Unsupported;
    }
    return er;
}
}  // namespace

// RDRAM -> DMEM, as librecomp's dma_rdram_to_dmem (len already includes the + 1).
void spu_dma_rdram_to_dmem(uint32_t dmem_addr, uint32_t dram_addr, uint32_t len) {
    dram_addr &= 0xFFFFF8;
    dmem_addr &= 0xFFF;
    if (dmem_addr + len > 0x1000) len = 0x1000 - dmem_addr;
    if (len == 0) return;
    const uint64_t ea = g_rdram_ea + dram_addr;
    const uint32_t lead = (uint32_t)ea & 15;
    const uint32_t total = (lead + len + 15) & ~15u;
    mfc_get(g_bounce, ea & ~15ull, total, kTag, 0, 0);
    wait_dma();
    memcpy(dmem + dmem_addr, g_bounce + lead, len);
    g_reads++;
}

// DMEM -> RDRAM, exactly len bytes: the middle in 16-byte-aligned blocks, the ends in naturally
// aligned 8/4/2/1-byte transfers (no read-modify-write of neighbouring game memory).
void spu_dma_dmem_to_rdram(uint32_t dmem_addr, uint32_t dram_addr, uint32_t len) {
    dram_addr &= 0xFFFFF8;
    dmem_addr &= 0xFFF;
    if (dmem_addr + len > 0x1000) len = 0x1000 - dmem_addr;
    if (len == 0) return;
    uint64_t ea = g_rdram_ea + dram_addr;
    const uint32_t lead = (uint32_t)ea & 15;
    memcpy(g_bounce + lead, dmem + dmem_addr, len);
    uint32_t ls = (uint32_t)(uintptr_t)g_bounce + lead;  // same low 4 bits as ea
    uint32_t left = len;
    while (left) {
        uint32_t n;
        if ((ea & 15) == 0 && left >= 16) n = left & ~15u;
        else if ((ea & 7) == 0 && left >= 8) n = 8;
        else if ((ea & 3) == 0 && left >= 4) n = 4;
        else if ((ea & 1) == 0 && left >= 2) n = 2;
        else n = 1;
        mfc_put((volatile void*)ls, ea, n, kTag, 0, 0);
        ls += n; ea += n; left -= n;
    }
    wait_dma();
    g_writes++;
}

int main(uint64_t job_ea, uint64_t, uint64_t, uint64_t) {
    constants_init();
    spu_write_decrementer(0xFFFFFFFFu);
    for (;;) {
        const uint32_t cmd = spu_read_in_mbox();
        if (cmd != SPU_AUDIO_CMD_RUN) break;
        mfc_get(&g_job, job_ea, sizeof g_job, kTag, 0, 0);
        wait_dma();
        mfc_get(dmem, g_job.dmem_ea, 0x1000, kTag, 0, 0);
        wait_dma();
        g_rdram_ea = g_job.rdram_ea;
        g_reads = g_writes = 0;
        const uint32_t t0 = spu_read_decrementer();
        const RspExitReason er = run_ucode(g_job.ucode_addr);
        const uint32_t t1 = spu_read_decrementer();
        mfc_put(dmem, g_job.dmem_ea, 0x1000, kTag, 0, 0);
        wait_dma();
        g_job.result = (uint32_t)er;
        g_job.ticks = t0 - t1;  // the decrementer counts down
        g_job.dma_reads = g_reads;
        g_job.dma_writes = g_writes;
        g_job.done = g_job.seq;
        mfc_put(&g_job, job_ea, sizeof g_job, kTag, 0, 0);
        wait_dma();
    }
    spu_thread_exit(0);
    return 0;
}
