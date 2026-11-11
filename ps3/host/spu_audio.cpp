// PPU side of the SPU audio program (see spu_audio.h and ps3/spu/audio/spu_audio_main.cpp).
#include "spu_audio.h"

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <ppu_intrinsics.h>
#include <lv2/spu.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include <unistd.h>

#include "../spu/spu_audio_job.h"

extern "C" void ps3prof_block(int blocked) __attribute__((weak));

namespace {
inline uint64_t ea(const volatile void* p) { return (uint64_t)(uintptr_t)p; }
bool g_spus_initialized = false;
}

void ps3_spu_initialize() {
    if (g_spus_initialized) return;
    s32 r = sysSpuInitialize(6, 0);
    if (r != 0) std::printf("[spu] sysSpuInitialize: 0x%08X (may already be set up)\n", (unsigned)r);
    g_spus_initialized = true;
}

bool SpuThread::load(const void* program, uint32_t size, const char* name) {
    if (loaded_) return true;
    ps3_spu_initialize();
    name_ = name;
    size_ = size;
    std::printf("[spu] %s: loading the program (%u bytes at %p)\n", name, (unsigned)size, program);
    std::fflush(stdout);
    s32 r = sysSpuImageImport(&image_, program, 0);
    if (r != 0) { std::printf("[spu] %s: loading the SPU program failed: 0x%08X\n", name, (unsigned)r); return false; }
    std::printf("[spu] %s: loaded (%u segments, entry 0x%X)\n", name, (unsigned)image_.segmentCount, (unsigned)image_.entryPoint);
    {
        const sysSpuSegment* seg = (const sysSpuSegment*)(uintptr_t)image_.segments;
        for (u32 i = 0; seg && i < image_.segmentCount && i < 8; i++)
            std::printf("[spu] %s:   segment %u: type %u, local store 0x%05X, %u bytes\n", name, (unsigned)i, (unsigned)seg[i].type,
                        (unsigned)seg[i].lsStart, (unsigned)seg[i].size);
    }
    std::fflush(stdout);
    loaded_ = true;
    return true;
}

bool SpuThread::start(void* job) {
    if (running_) return true;
    if (!loaded_) return false;
    const char* name = name_;
    sysSpuThreadGroupAttribute gattr;
    std::memset(&gattr, 0, sizeof gattr);
    gattr.nsize = std::strlen(name) + 1;
    gattr.name = name;
    std::printf("[spu] %s: creating the thread group\n", name);
    std::fflush(stdout);
    s32 r = sysSpuThreadGroupCreate(&group_, 1, 100, &gattr);
    if (r != 0) { std::printf("[spu] %s: creating the SPU thread group failed: 0x%08X\n", name, (unsigned)r); return false; }
    sysSpuThreadAttribute tattr;
    tattr.name = name;
    tattr.nsize = std::strlen(name) + 1;
    tattr.option = SPU_THREAD_ATTR_NONE;
    sysSpuThreadArgument arg = {ea(job), 0, 0, 0};
    std::printf("[spu] %s: creating the thread\n", name);
    std::fflush(stdout);
    r = sysSpuThreadInitialize(&thread_, group_, 0, &image_, &tattr, &arg);
    if (r != 0) { std::printf("[spu] %s: creating the SPU thread failed: 0x%08X\n", name, (unsigned)r); return false; }
    std::printf("[spu] %s: starting it\n", name);
    std::fflush(stdout);
    r = sysSpuThreadGroupStart(group_);
    if (r != 0) { std::printf("[spu] %s: starting the SPU thread failed: 0x%08X\n", name, (unsigned)r); return false; }
    running_ = true;
    std::printf("[spu] %s: running on an SPU (program %u bytes)\n", name, (unsigned)size_);
    return true;
}

bool SpuThread::send(uint32_t word) {
    if (!running_) return false;
    s32 r = sysSpuThreadWriteMb(thread_, word);
    if (r != 0) {
        std::printf("[spu] %s: sending to the SPU failed: 0x%08X; not using it\n", name_, (unsigned)r);
        running_ = false;
        return false;
    }
    return true;
}

void SpuThread::stop() {
    if (!running_) return;
    sysSpuThreadGroupTerminate(group_, 1);  // so it can't write to memory later
    running_ = false;
}

bool SpuAudio::start() {
    if (running()) return true;
    if (!job_) {
        job_ = (SpuAudioJob*)memalign(128, sizeof(SpuAudioJob));
        dmem_ = (uint8_t*)memalign(128, 0x1000);
        if (!job_ || !dmem_) { std::printf("[spu] %s: out of memory\n", spu_.name()); return false; }
        std::memset(job_, 0, sizeof *job_);
    }
    return spu_.start(job_);
}

RspExitReason SpuAudio::run(uint8_t* rdram, uint32_t ucode_addr) {
    if (!running()) return RspExitReason::Invalid;
    volatile SpuAudioJob* job = job_;
    std::memcpy(dmem_, dmem, 0x1000);
    job->rdram_ea = ea(rdram);
    job->dmem_ea = ea(dmem_);
    job->ucode_addr = ucode_addr;
    job->seq = ++seq_;
    job->done = 0;
    __sync();  // the job block, DMEM and game memory are visible to the SPU's DMA
    if (!spu_.send(SPU_AUDIO_CMD_RUN)) return RspExitReason::Invalid;
    const uint64_t tb = sysGetTimebaseFrequency(), t0 = __mftb();
    if (ps3prof_block) ps3prof_block(1);
    bool ok = true;
    while (job->done != seq_) {
        if (__mftb() - t0 > 2 * tb) { ok = false; break; }
        usleep(50);
    }
    if (ps3prof_block) ps3prof_block(0);
    if (!ok) {
        std::printf("[spu] %s: the SPU didn't finish task %u within 2 s; stopping it, not using the SPU\n", spu_.name(), (unsigned)seq_);
        spu_.stop();
        return RspExitReason::Invalid;
    }
    __lwsync();
    std::memcpy(dmem, dmem_, 0x1000);
    ticks_ = job->ticks;
    reads_ = job->dma_reads;
    writes_ = job->dma_writes;
    return (RspExitReason)job->result;
}
