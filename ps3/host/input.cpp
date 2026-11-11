// input.cpp - PS3 controller -> N64 controller.
//
//   Left stick      analog stick          D-pad          D-pad
//   Cross           A                     Circle         B
//   L2 or R2        Z                     R1             R
//   L1              L                     Start          Start
//   Right stick     C buttons (up/down/left/right)
//   Triangle        C-up      Square      C-right   (extra, for quick access)
//   Select          next picture resolution (the renderer's, not the game's)
//   L3 (left stick pressed)   60 / 30 frames a second
//   R3 (right stick pressed)  saves the game memory of the next 3 frames (snapshot_*.png, for checking)
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <io/pad.h>

#include "input.h"

extern "C" void ps3_log(const char* fmt, ...);

namespace {
    // N64 button bits (libultra)
    constexpr uint16_t A_BUTTON = 0x8000, B_BUTTON = 0x4000, Z_TRIG = 0x2000, START_BUTTON = 0x1000;
    constexpr uint16_t U_JPAD = 0x0800, D_JPAD = 0x0400, L_JPAD = 0x0200, R_JPAD = 0x0100;
    constexpr uint16_t L_TRIG = 0x0020, R_TRIG = 0x0010;
    constexpr uint16_t U_CBUTTONS = 0x0008, D_CBUTTONS = 0x0004, L_CBUTTONS = 0x0002, R_CBUTTONS = 0x0001;

    std::atomic<uint16_t> cur_buttons{0};
    std::atomic<float> cur_x{0.0f}, cur_y{0.0f};
    std::atomic<bool> connected{false};
    uint16_t seen_buttons = 0;  // for logging each button the first time it is pressed
    bool rumble_on = false;
    std::atomic<uint32_t> select_presses{0}, l3_presses{0}, r3_presses{0}, polls{0}, changes{0};
    bool select_down = false, l3_down = false, r3_down = false;

    float axis(unsigned v, bool invert) {
        float f = ((float)v - 128.0f) / 127.0f;
        if (invert) f = -f;
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return f;
    }
}

void ps3input::init() {
    ioPadInit(7);
}

namespace {
    // Every change of what the game reads, by the number of its controller read, so a session can be
    // played again on the workstation (tools/native-reference: NATIVE_INPUT=input_log.txt).
    std::FILE* input_log = nullptr;
    uint32_t read_index = 0, logged_buttons = 0xFFFFFFFF;
    float logged_x = 2, logged_y = 2;
    void log_input(uint16_t b, float x, float y) {
#if defined(CONKER_RELEASE)
        (void)b; (void)x; (void)y;
        return;  // no log in the release build
#endif
        if (!input_log) {
            input_log = std::fopen("/dev_hdd0/tmp/conkerps3/input_log.txt", "w");
            if (!input_log) return;
            std::fprintf(input_log, "# controller read, N64 buttons, stick x, stick y\n");
        }
        if (read_index % 60 == 0) std::fflush(input_log);
        if (b == logged_buttons && x == logged_x && y == logged_y) return;
        logged_buttons = b; logged_x = x; logged_y = y;
        std::fprintf(input_log, "%u %04X %.3f %.3f\n", read_index, b, x, y);
    }
}

void ps3input::poll() {
    polls++;
    read_index++;
    struct Log { ~Log() { log_input(cur_buttons.load(), cur_x.load(), cur_y.load()); } } log_after;
    padInfo info;
    if (ioPadGetInfo(&info) != 0 || !info.status[0]) {
        connected = false;
        return;
    }
    connected = true;
    padData pad;
    if (ioPadGetData(0, &pad) != 0 || pad.len == 0) {
        return;  // no change since the last read
    }
    if (pad.BTN_SELECT && !select_down) select_presses++;
    select_down = pad.BTN_SELECT;
    if (pad.BTN_L3 && !l3_down) l3_presses++;
    l3_down = pad.BTN_L3;
    if (pad.BTN_R3 && !r3_down) r3_presses++;
    r3_down = pad.BTN_R3;
    uint16_t b = 0;
    if (pad.BTN_CROSS) b |= A_BUTTON;
    if (pad.BTN_CIRCLE) b |= B_BUTTON;
    if (pad.BTN_L2 || pad.BTN_R2) b |= Z_TRIG;
    if (pad.BTN_START) b |= START_BUTTON;
    if (pad.BTN_UP) b |= U_JPAD;
    if (pad.BTN_DOWN) b |= D_JPAD;
    if (pad.BTN_LEFT) b |= L_JPAD;
    if (pad.BTN_RIGHT) b |= R_JPAD;
    if (pad.BTN_L1) b |= L_TRIG;
    if (pad.BTN_R1) b |= R_TRIG;
    if (pad.BTN_TRIANGLE) b |= U_CBUTTONS;
    if (pad.BTN_SQUARE) b |= R_CBUTTONS;
    // Right stick as C buttons, past half way.
    float rx = axis(pad.ANA_R_H, false), ry = axis(pad.ANA_R_V, true);
    if (ry > 0.5f) b |= U_CBUTTONS;
    if (ry < -0.5f) b |= D_CBUTTONS;
    if (rx < -0.5f) b |= L_CBUTTONS;
    if (rx > 0.5f) b |= R_CBUTTONS;

    float x = axis(pad.ANA_L_H, false), y = axis(pad.ANA_L_V, true);
    // Small dead zone: PS3 sticks rest a few steps off center.
    if (std::fabs(x) < 0.08f) x = 0.0f;
    if (std::fabs(y) < 0.08f) y = 0.0f;

    uint16_t newly = b & ~seen_buttons;
    if (newly) {
        seen_buttons |= newly;
        ps3_log("[input] first press of N64 button bits 0x%04X\n", newly);
    }
    changes++;
    cur_buttons = b;
    cur_x = x;
    cur_y = y;
}

void ps3input::report() {
    ps3_log("[input] last 10 s: the game read the controller %u times, %u changes; controller %s\n", polls.exchange(0), changes.exchange(0),
            connected ? "connected" : "NOT connected");
}

uint32_t ps3input::select_count() { return select_presses.load(); }
uint32_t ps3input::l3_count() { return l3_presses.load(); }
uint32_t ps3input::r3_count() { return r3_presses.load(); }

bool ps3input::get(int controller, uint16_t* buttons, float* x, float* y) {
    if (controller != 0 || !connected) return false;
    *buttons = cur_buttons.load();
    *x = cur_x.load();
    *y = cur_y.load();
    return true;
}

void ps3input::set_rumble(int controller, bool on) {
    if (controller != 0 || on == rumble_on) return;
    rumble_on = on;
    padActParam act{};
    act.small_motor = 0;
    act.large_motor = on ? 200 : 0;
    ioPadSetActDirect(0, &act);
}
