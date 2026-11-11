// audio_out.cpp - sound output on the PS3.
//
// The game's audio thread hands over each buffer the audio microcode mixed (osAiSetNextBuffer ->
// queue_samples), at the game's rate (22020 Hz for Conker), and paces itself by how much is still
// queued (osAiGetLength -> get_frames_remaining). Those samples go into a ring buffer. A
// high-priority thread feeds the PS3 audio port: 2 channels of 32-bit float at a fixed 48 kHz, in
// blocks of 256 frames, resampling with linear interpolation.
//
// RDRAM is big-endian on the PS3, exactly as on the N64, so each stereo frame is [left, right] (the
// PC build has to swap them).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sys/thread.h>

#include "audio_out.h"

extern "C" void ps3prof_block(int blocked);
extern "C" void ps3_log(const char* fmt, ...);

namespace {
    constexpr uint32_t kOutRate = 48000;
    constexpr uint32_t kRingFrames = 32768;  // ~1.5 s at 22 kHz

    std::mutex ring_mutex;
    float ring[kRingFrames * 2];
    uint64_t ring_write = 0;      // frames written (total)
    double ring_read = 0.0;       // fractional read position (total frames)
    std::atomic<uint32_t> in_rate{22020};

    u32 port_num = 0;
    audioPortConfig port_config{};
    sys_event_queue_t notify_queue;
    u64 notify_key;
    bool started = false;

    std::atomic<uint32_t> underruns{0};
    std::atomic<uint64_t> frames_in{0};

    void fill_block(float* out) {
        std::lock_guard lock{ring_mutex};
        const double step = (double)in_rate.load() / (double)kOutRate;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            uint64_t i0 = (uint64_t)ring_read;
            if (i0 + 1 >= ring_write) {
                // Underrun: play silence for the rest of the block.
                for (; i < AUDIO_BLOCK_SAMPLES; i++) { out[i * 2] = 0.0f; out[i * 2 + 1] = 0.0f; }
                underruns++;
                return;
            }
            float frac = (float)(ring_read - (double)i0);
            const float* a = &ring[(i0 % kRingFrames) * 2];
            const float* b = &ring[((i0 + 1) % kRingFrames) * 2];
            out[i * 2 + 0] = a[0] + (b[0] - a[0]) * frac;
            out[i * 2 + 1] = a[1] + (b[1] - a[1]) * frac;
            ring_read += step;
        }
    }

    void audio_thread(void*) {
        ps3prof_set_name_c("Audio Output Thread");
        float* data = (float*)(uintptr_t)port_config.audioDataStart;
        const u32 block_floats = port_config.channelCount * AUDIO_BLOCK_SAMPLES;
        u64 last_block = *(volatile u64*)(uintptr_t)port_config.readIndex;
        for (;;) {
            sys_event_t ev;
            ps3prof_block(1);
            sysEventQueueReceive(notify_queue, &ev, 20 * 1000);
            ps3prof_block(0);
            // Fill every block between the one being played and the one after it, so the hardware
            // always has one queued.
            u64 current = *(volatile u64*)(uintptr_t)port_config.readIndex;
            u64 target = (current + 1) % port_config.numBlocks;
            while (last_block != target) {
                last_block = (last_block + 1) % port_config.numBlocks;
                fill_block(data + block_floats * last_block);
            }
        }
    }
}

extern "C" void ps3prof_set_name(const char* name);
void ps3prof_set_name_c(const char* name) { ps3prof_set_name(name); }

bool ps3audio::init() {
    s32 r = audioInit();
    audioPortParam params{};
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = AUDIO_PORT_INITLEVEL;
    params.level = 1.0f;
    s32 r2 = audioPortOpen(&params, &port_num);
    s32 r3 = audioGetPortConfig(port_num, &port_config);
    s32 r4 = audioCreateNotifyEventQueue(&notify_queue, &notify_key);
    s32 r5 = audioSetNotifyEventQueue(notify_key);
    sysEventQueueDrain(notify_queue);
    // Start with silence in every block.
    std::memset((void*)(uintptr_t)port_config.audioDataStart, 0, port_config.portSize);
    s32 r6 = audioPortStart(port_num);
    ps3_log("[audio] init %d, port open %d (port %u), config %d, notify %d/%d, start %d\n",
            r, r2, port_num, r3, r4, r5, r6);
    if (r2 || r3 || r6) return false;
    sys_ppu_thread_t tid;
    static char name[] = "audio out";
    // High priority (lv2: 0 is highest): the port must be fed every 5.3 ms.
    sysThreadCreate(&tid, audio_thread, nullptr, 100, 64 * 1024, 0, name);
    started = true;
    return true;
}

void ps3audio::set_frequency(uint32_t hz) {
    if (hz == 0) return;
    in_rate = hz;
    ps3_log("[audio] game audio rate %u Hz\n", hz);
}

void ps3audio::queue_samples(int16_t* samples, size_t sample_count) {
    if (!started) return;
    std::lock_guard lock{ring_mutex};
    size_t frames = sample_count / 2;
    // Drop the oldest audio if the game ever gets more than the ring ahead.
    if (ring_write + frames - (uint64_t)ring_read > kRingFrames - 16) {
        ring_read = (double)(ring_write + frames - (kRingFrames - 16));
    }
    constexpr float scale = 1.0f / 32768.0f;
    for (size_t f = 0; f < frames; f++) {
        float* dst = &ring[(ring_write % kRingFrames) * 2];
        dst[0] = samples[f * 2 + 0] * scale;
        dst[1] = samples[f * 2 + 1] * scale;
        ring_write++;
    }
    frames_in += frames;
}

// As in CBFD-Recompiled's SDL output: report what is queued less one game buffer (736 frames), so
// Conker's audio thread keeps about a buffer and a half queued (see host/src/audio_output.cpp).
size_t ps3audio::get_frames_remaining() {
    if (!started) return 0;
    std::lock_guard lock{ring_mutex};
    constexpr uint64_t one_buffer = 736;
    uint64_t queued = ring_write > (uint64_t)ring_read ? ring_write - (uint64_t)ring_read : 0;
    return queued > one_buffer ? (size_t)(queued - one_buffer) : 0;
}

uint32_t ps3audio::take_underruns() { return underruns.exchange(0); }
uint64_t ps3audio::frames_received() { return frames_in.load(); }
