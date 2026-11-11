// audio_out.h - sound output on the PS3 (audio_out.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

void ps3prof_set_name_c(const char* name);

namespace ps3audio {
    bool init();
    void set_frequency(uint32_t hz);
    void queue_samples(int16_t* samples, size_t sample_count);
    size_t get_frames_remaining();
    uint32_t take_underruns();       // blocks played as silence since the last call
    uint64_t frames_received();      // total frames the game has produced
}
