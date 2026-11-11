// The audio microcode on an SPU (ps3/spu/audio), driven from the PPU.
#pragma once
#include <cstdint>

#include "librecomp/rsp.hpp"

#include <lv2/spu.h>
#include <sys/spu.h>

struct SpuAudioJob;

// Sets up lv2's SPU management once (6 SPUs for threads).
void ps3_spu_initialize();

// An SPU program on its own SPU thread (one thread group of one thread), given the address of its
// 128-byte job block as its first argument. load() imports the program (do this for every program
// before starting any: the console crashed importing one while another SPU was working), start()
// runs it. Both false (with a message in the log) if they couldn't.
class SpuThread {
public:
    bool load(const void* program, uint32_t size, const char* name);
    bool start(void* job);
    bool running() const { return running_; }
    // Sends a mailbox word; false (logged, and the thread counts as stopped) if that failed.
    bool send(uint32_t word);
    // Stops the thread (it can't write to memory afterwards).
    void stop();
    const char* name() const { return name_; }

private:
    bool loaded_ = false, running_ = false;
    const char* name_ = "";
    uint32_t size_ = 0, group_ = 0, thread_ = 0;
    sysSpuImage image_;  // kept for the thread's lifetime
};

class SpuAudio {
public:
    // Loads the audio SPU program (an SPU ELF embedded with bin2o); start() runs it on its own SPU thread.
    bool load(const void* program, uint32_t size, const char* name) { return spu_.load(program, size, name); }
    bool start();
    bool running() const { return spu_.running(); }
    // Runs one task as conker_audio_ucode does, with the runtime's DMEM (librecomp's `dmem`) as its
    // DMEM. The calling thread sleeps until the SPU is done. If it isn't within 2 s, logs it, stops
    // using the SPU and returns RspExitReason::Invalid.
    RspExitReason run(uint8_t* rdram, uint32_t ucode_addr);
    // The last task's time on the SPU (timebase ticks) and DMA counts.
    uint32_t last_ticks() const { return ticks_; }
    uint32_t last_dma_reads() const { return reads_; }
    uint32_t last_dma_writes() const { return writes_; }

private:
    SpuThread spu_;
    uint32_t seq_ = 0, ticks_ = 0, reads_ = 0, writes_ = 0;
    SpuAudioJob* job_ = nullptr;  // 128-byte aligned
    uint8_t* dmem_ = nullptr;     // 4 KB, 128-byte aligned
};
