// input.cpp - PS3 controller -> N64 controller.
//
// PS3 controllers 1 to 4 are the N64's four controller ports (the multiplayer modes).
//   Left stick      analog stick          D-pad          D-pad
//   Cross           A                     Circle         B (or C-right: the menu's "B button" Square)
//   L2 or R2        Z                     R1             R
//   L1              L                     Start          Start
//   Right stick     C buttons (up/down/left/right)
//   Triangle        C-up      Square      C-right   (extra, for quick access; or B: settings.h b_on_square)
//   Select + Start  held for a second: quit to the XMB (2.0; Select stepped through the resolutions before)
//   L3 (left stick pressed)   60 / 30 frames a second
//   R3 (right stick pressed)  saves the game memory of the next 3 frames (snapshot_*.png, for checking)
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <io/pad.h>
#include <sys/systime.h>

#include "input.h"
#include "settings.h"

extern "C" void ps3_log(const char* fmt, ...);

namespace {
    // N64 button bits (libultra)
    constexpr uint16_t A_BUTTON = 0x8000, B_BUTTON = 0x4000, Z_TRIG = 0x2000, START_BUTTON = 0x1000;
    constexpr uint16_t U_JPAD = 0x0800, D_JPAD = 0x0400, L_JPAD = 0x0200, R_JPAD = 0x0100;
    constexpr uint16_t L_TRIG = 0x0020, R_TRIG = 0x0010;
    constexpr uint16_t U_CBUTTONS = 0x0008, D_CBUTTONS = 0x0004, L_CBUTTONS = 0x0002, R_CBUTTONS = 0x0001;

    // The four N64 controller ports: PS3 controllers 1 to 4.
    constexpr int kPorts = 4;
    std::atomic<uint16_t> cur_buttons_[kPorts];
    std::atomic<float> cur_x_[kPorts], cur_y_[kPorts];
    std::atomic<bool> connected_[kPorts];
    // Each controller's buttons without the right stick's C-buttons, and its right stick (x right, y down, -1 to 1):
    // the right stick is the C-buttons unless the free camera turns with it (free_camera.cpp).
    uint16_t base_buttons_[kPorts];
    std::atomic<float> right_x_[kPorts], right_y_[kPorts];
    std::atomic<uint16_t>& cur_buttons = cur_buttons_[0];
    std::atomic<float> &cur_x = cur_x_[0], &cur_y = cur_y_[0];
    std::atomic<bool>& connected = connected_[0];
    uint16_t seen_buttons = 0;  // for logging each button the first time it is pressed
    bool rumble_on[kPorts] = {false, false, false, false};
    std::atomic<uint32_t> select_presses{0}, l3_presses{0}, r3_presses{0}, polls{0}, changes{0};
    bool select_down = false, start_down = false, l3_down = false, r3_down = false;
    // Select + Start held on the first controller: since when (timebase), and whether a second has passed.
    uint64_t combo_since = 0;
    std::atomic<bool> quit_combo{false};

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

namespace {
    // One PS3 controller's state as an N64 controller's.
    void read_pad(const padData& pad, uint16_t* buttons, float* sx, float* sy, float* rsx, float* rsy) {
        uint16_t b = 0;
        if (pad.BTN_CROSS) b |= A_BUTTON;
        const bool b_square = g_settings.b_on_square;  // the menu's "B button": Square (Circle then C-right)
        if (b_square ? pad.BTN_SQUARE : pad.BTN_CIRCLE) b |= B_BUTTON;
        if (pad.BTN_L2 || pad.BTN_R2) b |= Z_TRIG;
        if (pad.BTN_START) b |= START_BUTTON;
        if (pad.BTN_UP) b |= U_JPAD;
        if (pad.BTN_DOWN) b |= D_JPAD;
        if (pad.BTN_LEFT) b |= L_JPAD;
        if (pad.BTN_RIGHT) b |= R_JPAD;
        if (pad.BTN_L1) b |= L_TRIG;
        if (pad.BTN_R1) b |= R_TRIG;
        if (pad.BTN_TRIANGLE) b |= U_CBUTTONS;
        if (b_square ? pad.BTN_CIRCLE : pad.BTN_SQUARE) b |= R_CBUTTONS;
        // The right stick: the C-buttons (stick_buttons) or the free camera.
        *rsx = axis(pad.ANA_R_H, false);
        *rsy = axis(pad.ANA_R_V, false);
        float x = axis(pad.ANA_L_H, false), y = axis(pad.ANA_L_V, true);
        // Small dead zone: PS3 sticks rest a few steps off center.
        if (std::fabs(x) < 0.08f) x = 0.0f;
        if (std::fabs(y) < 0.08f) y = 0.0f;
        *buttons = b; *sx = x; *sy = y;
    }
}

namespace {
    std::atomic<uint64_t> last_read{0};
    // The right stick as C buttons, past half way (y down).
    uint16_t stick_buttons(float rx, float ry) {
        uint16_t b = 0;
        if (ry < -0.5f) b |= U_CBUTTONS;
        if (ry > 0.5f) b |= D_CBUTTONS;
        if (rx < -0.5f) b |= L_CBUTTONS;
        if (rx > 0.5f) b |= R_CBUTTONS;
        return b;
    }
}
uint64_t ps3input::last_read_tb() { return last_read.load(); }

void ps3input::poll() {
    polls++;
    last_read = __builtin_ppc_get_timebase();
    read_index++;
    struct Log { ~Log() { log_input(cur_buttons.load(), cur_x.load(), cur_y.load()); } } log_after;
    padInfo info;
    if (ioPadGetInfo(&info) != 0) {
        for (int i = 0; i < kPorts; i++) connected_[i] = false;
        return;
    }
    for (int i = 0; i < kPorts; i++) {
        connected_[i] = info.status[i] != 0;
        if (!info.status[i]) continue;
        padData pad;
        if (ioPadGetData(i, &pad) != 0 || pad.len == 0) continue;  // no change since the last read
        if (i == 0) {  // the first controller's extra buttons
            if (pad.BTN_SELECT && !select_down) select_presses++;
            select_down = pad.BTN_SELECT;
            start_down = pad.BTN_START;
            if (pad.BTN_L3 && !l3_down) l3_presses++;
            l3_down = pad.BTN_L3;
            if (pad.BTN_R3 && !r3_down) r3_presses++;
            r3_down = pad.BTN_R3;
        }
        uint16_t b;
        float x, y, rx, ry;
        read_pad(pad, &b, &x, &y, &rx, &ry);
        base_buttons_[i] = b;
        right_x_[i] = rx;
        right_y_[i] = ry;
        // The right stick is the C-buttons, unless this player's free camera turns with it now.
        if (!free_camera_owns_stick(i)) b |= stick_buttons(rx, ry);
        if (i == 0) {
            uint16_t newly = b & ~seen_buttons;
            if (newly) {
                seen_buttons |= newly;
                ps3_log("[input] first press of N64 button bits 0x%04X\n", newly);
            }
            changes++;
        }
        cur_buttons_[i] = b;
        cur_x_[i] = x;
        cur_y_[i] = y;
    }
    // A controller unchanged since its last read (no data): the free camera may have taken or let go of its stick since.
    for (int i = 0; i < kPorts; i++) {
        if (!connected_[i]) continue;
        uint16_t b = base_buttons_[i];
        if (!free_camera_owns_stick(i)) b |= stick_buttons(right_x_[i].load(), right_y_[i].load());
        cur_buttons_[i] = b;
    }
    // Select + Start: quit once both have been held for a second (a quick press of both does nothing more
    // than Start does: the game's pause). Read here, as the game reads the controllers: it does every frame,
    // in its pause menu too.
    if (connected_[0] && select_down && start_down) {
        const uint64_t now = __builtin_ppc_get_timebase();
        if (!combo_since) combo_since = now;
        else if (now - combo_since >= sysGetTimebaseFrequency() && !quit_combo.exchange(true))
            ps3_log("[input] Select + Start held for a second: quitting\n");
    } else {
        combo_since = 0;
    }
}

bool ps3input::quit_combo_held() { return quit_combo.load(); }

void ps3input::report() {
    ps3_log("[input] last 10 s: the game read the controller %u times, %u changes; controller %s\n", polls.exchange(0), changes.exchange(0),
            connected ? "connected" : "NOT connected");
}

uint32_t ps3input::select_count() { return select_presses.load(); }
uint32_t ps3input::l3_count() { return l3_presses.load(); }
uint32_t ps3input::r3_count() { return r3_presses.load(); }

bool ps3input::get(int controller, uint16_t* buttons, float* x, float* y) {
    if (controller < 0 || controller >= kPorts || !connected_[controller]) return false;
    *buttons = cur_buttons_[controller].load();
    *x = cur_x_[controller].load();
    *y = cur_y_[controller].load();
    return true;
}

bool ps3input::right_stick(int controller, float* x, float* y) {
    if (controller < 0 || controller >= kPorts || !connected_[controller]) return false;
    *x = right_x_[controller].load();
    *y = right_y_[controller].load();
    return true;
}

bool ps3input::is_connected(int controller) {
    if (controller < 0 || controller >= kPorts) return false;
    padInfo info;
    return ioPadGetInfo(&info) == 0 && info.status[controller] != 0;
}

void ps3input::set_rumble(int controller, bool on) {
    if (controller < 0 || controller >= kPorts || on == rumble_on[controller]) return;
    rumble_on[controller] = on;
    padActParam act{};
    act.small_motor = 0;
    act.large_motor = on ? 200 : 0;
    ioPadSetActDirect(controller, &act);
}
