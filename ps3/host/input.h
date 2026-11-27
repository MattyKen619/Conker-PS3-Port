// input.h - PS3 controller -> N64 controller (input.cpp).
#pragma once
#include <cstdint>

namespace ps3input {
    void init();
    void poll();
    bool get(int controller, uint16_t* buttons, float* x, float* y);
    void set_rumble(int controller, bool on);
    bool right_stick(int controller, float* x, float* y);  // x right, y down, -1 to 1 (the free camera)
    bool is_connected(int controller);  // a PS3 controller is on that port (1 to 4) right now
    // Presses so far of the buttons the game doesn't use (Select: resolution, L3: frame rate).
    uint32_t select_count();
    bool quit_combo_held();  // Select + Start held for a second on the first controller: quit to the XMB
    uint32_t l3_count();
    uint64_t last_read_tb();  // the timebase when the game last read the controllers (input lag check)
    uint32_t r3_count();  // R3: snapshots for checking a picture problem on the workstation
    void report();  // logs how often the game read the controller (every 10 s, by the renderer)
}

// free_camera.cpp: this player's right stick turns their camera now (so it isn't the C-buttons too).
bool free_camera_owns_stick(int player);
