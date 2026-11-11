// The startup menu (settings.h): drawn by the PPU straight into the display buffers (rsxutil), before
// the game and the renderer start. Up/Down choose a line, Left/Right change it, Cross or Start plays.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>

#include "settings.h"
#include "rsxutil.h"
#include "menu_font.h"

namespace {

struct Canvas {
    std::vector<uint32_t> px;
    uint32_t w, h;
    void fill(uint32_t c) { std::fill(px.begin(), px.end(), c); }
    void rect(int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = std::max(0, y0); y < std::min((int)h, y1); y++)
            for (int x = std::max(0, x0); x < std::min((int)w, x1); x++) px[(size_t)y * w + x] = c;
    }
    // Text at (x, top y), scaled by s (in 1/40ths of the font's 40 px), in color c; returns its width.
    int text(int x, int y, const std::string& t, uint32_t c, float s = 1.0f, bool measure = false) {
        int pen = 0;
        for (unsigned char ch : t) {
            if (ch < 32 || ch > 126) ch = '?';
            const auto& g = menu_font::kGlyphs[ch - 32];
            if (!measure && g.w > 0) {
                const int gw = (int)(g.w * s + 0.5f), gh = (int)(menu_font::kHeight * s + 0.5f), gx = x + (int)((pen + g.x) * s);
                for (int yy = 0; yy < gh; yy++) {
                    const int py = y + yy;
                    if (py < 0 || py >= (int)h) continue;
                    const int sy = std::min(menu_font::kHeight - 1, (int)(yy / s));
                    for (int xx = 0; xx < gw; xx++) {
                        const int pxx = gx + xx;
                        if (pxx < 0 || pxx >= (int)w) continue;
                        const int sx = std::min(g.w - 1, (int)(xx / s));
                        const uint32_t a = menu_font::kPixels[g.offset + (size_t)sy * g.w + sx];
                        if (!a) continue;
                        uint32_t& d = px[(size_t)py * w + pxx];
                        auto mix = [&](int sh) { return ((((c >> sh) & 255) * a + ((d >> sh) & 255) * (255 - a)) / 255) << sh; };
                        d = mix(16) | mix(8) | mix(0);
                    }
                }
            }
            pen += g.advance;
        }
        return (int)(pen * s);
    }
    void centered(int y, const std::string& t, uint32_t c, float s = 1.0f) { text(((int)w - text(0, 0, t, c, s, true)) / 2, y, t, c, s); }
};

#if defined(CONKER_RELEASE)
// The release: no 1080 lines and no 60 fps (not finished); 648 lines by default.
const char* kResolution[] = {"", "", "432 lines (fastest)", "648 lines", "864 Lines (Overclock Recommended)"};
constexpr int kRows = 4;  // resolution, cutscenes, picture; then "Start the game"
#else
const char* kResolution[] = {"", "", "432 lines (fastest)", "648 lines", "864 lines", "1080 Lines (Overclock Recommended)"};
constexpr int kRows = 5;  // resolution, frame rate, cutscenes, picture; then "Start the game"
#endif

void draw(Canvas& cv, int row) {
    const float k = cv.h / 1080.0f;  // laid out for 1080 lines
    cv.fill(0x00101820);
    cv.rect(0, 0, cv.w, (int)(170 * k), 0x00203048);
    cv.centered((int)(50 * k), "Conker's Bad Fur Day", 0x00FFC040, 1.6f * k);
    cv.centered((int)(200 * k), "Settings", 0x00C0C8D0, 0.9f * k);
#if defined(CONKER_RELEASE)
    const std::string labels[3] = {"Resolution", "Cutscenes", "Picture"};
    const std::string values[3] = {kResolution[g_settings.scale], g_settings.cutscenes_4x3 ? "4:3, black bars at the sides" : "Widescreen",
                                   g_settings.fill ? "Fill the screen" : "With the N64's borders"};
#else
    const std::string labels[4] = {"Resolution", "Frame rate", "Cutscenes", "Picture"};
    const std::string values[4] = {kResolution[g_settings.scale], g_settings.fps60 ? "60 fps (experimental, interpolation)" : "30 fps (as on the N64)",
                                   g_settings.cutscenes_4x3 ? "4:3, black bars at the sides" : "Widescreen",
                                   g_settings.fill ? "Fill the screen" : "With the N64's borders"};
#endif
    const int left = (int)(cv.w / 2 - 760 * k), mid = (int)(cv.w / 2 - 260 * k);
    for (int i = 0; i < kRows; i++) {
        const int y = (int)((300 + i * 110) * k);
        const bool sel = i == row;
        if (sel) cv.rect(left - (int)(30 * k), y - (int)(14 * k), cv.w - left + (int)(30 * k), y + (int)(70 * k), 0x00304868);
        const uint32_t c = sel ? 0x00FFFFFF : 0x00A0A8B0;
        if (i < kRows - 1) {
            cv.text(left, y, labels[i], c, k);
            cv.text(mid, y, std::string(sel ? "<  " : "   ") + values[i] + (sel ? "  >" : ""), sel ? 0x00FFE080 : c, k);
        } else {
            cv.centered(y, "Start the game", sel ? 0x0080FF80 : c, 1.1f * k);
        }
    }
    cv.centered((int)(900 * k), "Up / Down: choose     Left / Right: change     Cross or Start: play", 0x00808890, 0.7f * k);
#if defined(CONKER_RELEASE)
    cv.centered((int)(960 * k), "In the game: Select changes the resolution", 0x00808890, 0.7f * k);
    cv.centered((int)(1020 * k), "Version " CONKER_VERSION, 0x00606870, 0.6f * k);
#else
    cv.centered((int)(960 * k), "In the game: Select changes the resolution, L3 the frame rate", 0x00808890, 0.7f * k);
#endif
}

void show(const Canvas& cv) {
    // Into the buffer being drawn (the one not on screen), then flip.
    uint32_t* dst = color_buffer[curr_fb];
    for (uint32_t y = 0; y < cv.h; y++) std::memcpy((uint8_t*)dst + (size_t)y * color_pitch, &cv.px[(size_t)y * cv.w], cv.w * 4);
    flip();
}

}  // namespace

std::atomic<bool> g_system_menu_open{false};

void show_message_screen(const char* text) {
    Canvas cv;
    cv.w = display_width; cv.h = display_height;
    cv.px.assign((size_t)cv.w * cv.h, 0);
    const float k = cv.h / 1080.0f;
    cv.fill(0x00101820);
    cv.centered((int)(500 * k), text, 0x00C0C8D0, 0.9f * k);
    show(cv);
    show(cv);
}

bool run_startup_menu(const std::atomic<bool>& quit) {
    Canvas cv;
    cv.w = display_width; cv.h = display_height;
    cv.px.assign((size_t)cv.w * cv.h, 0);
    int row = kRows - 1;
    bool dirty = true;
    uint32_t held = 0;  // buttons down at the last read (a press counts once)
    bool was_system = false;
    for (int frames = 0;; frames++) {
        sysUtilCheckCallback();
        if (quit) return false;
        // The display keeps flipping every frame, as the system wants: its own menu (the PS button) is
        // drawn over ours only while the program flips, and it has the controller meanwhile.
        if (g_system_menu_open) { was_system = true; flip(); usleep(4000); continue; }
        if (was_system) { was_system = false; dirty = true; held = ~0u; }  // buttons still down don't count
        if (dirty) { draw(cv, row); show(cv); show(cv); dirty = false; }  // both buffers
        else flip();  // both buffers hold the menu
        usleep(4000);
        padInfo info;
        padData pad;
        if (ioPadGetInfo(&info) != 0 || !info.status[0] || ioPadGetData(0, &pad) != 0 || pad.len == 0) continue;
        uint32_t now = 0;
        now |= pad.BTN_UP ? 1u : 0; now |= pad.BTN_DOWN ? 2u : 0; now |= pad.BTN_LEFT ? 4u : 0; now |= pad.BTN_RIGHT ? 8u : 0;
        now |= pad.BTN_CROSS ? 16u : 0; now |= pad.BTN_START ? 32u : 0;
        // The left stick as the d-pad.
        if (pad.ANA_L_V < 40) now |= 1; if (pad.ANA_L_V > 215) now |= 2; if (pad.ANA_L_H < 40) now |= 4; if (pad.ANA_L_H > 215) now |= 8;
        const uint32_t pressed = now & ~held;
        held = now;
        if (!pressed) continue;
        dirty = true;
        if (pressed & 1) row = (row + kRows - 1) % kRows;
        if (pressed & 2) row = (row + 1) % kRows;
        const int dir = (pressed & 4) ? -1 : (pressed & 8) ? 1 : 0;
        if (dir) {
            const int n = (int)kMaxPlayerScale - 1;  // scales 2..kMaxPlayerScale
            if (row == 0) g_settings.scale = (uint32_t)((int)g_settings.scale - 2 + dir + n) % n + 2;
#if defined(CONKER_RELEASE)
            if (row == 1) g_settings.cutscenes_4x3 = !g_settings.cutscenes_4x3;
            if (row == 2) g_settings.fill = !g_settings.fill;
#else
            if (row == 1) g_settings.fps60 = !g_settings.fps60;
            if (row == 2) g_settings.cutscenes_4x3 = !g_settings.cutscenes_4x3;
            if (row == 3) g_settings.fill = !g_settings.fill;
#endif
        }
        if (pressed & (16 | 32)) {
            save_settings();
            clear_screen(0);
            std::printf("[ps3] settings: %u lines, %s, cutscenes %s, picture %s\n", 216 * g_settings.scale, g_settings.fps60 ? "60 fps" : "30 fps",
                        g_settings.cutscenes_4x3 ? "4:3" : "widescreen", g_settings.fill ? "filling the screen" : "with borders");
            return true;
        }
    }
}

#ifdef MENU_TEST
// Workstation preview (tools/menu-font/preview.sh): the menu as an image.
void menu_preview(uint32_t* out, uint32_t w, uint32_t h, int row) {
    Canvas cv; cv.w = w; cv.h = h; cv.px.assign((size_t)w * h, 0);
    draw(cv, row);
    std::memcpy(out, cv.px.data(), (size_t)w * h * 4);
}
#endif
