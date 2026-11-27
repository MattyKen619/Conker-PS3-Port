// The startup menu (settings.h): drawn by the PPU straight into the display buffers (rsxutil), before
// the game and the renderer start. Tabs of settings (L1 / R1), Up/Down choose a line, Left/Right change it,
// Start (or Cross on "Start the game") plays.
#include <algorithm>
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

const char* kResolution[] = {"", "", "432 lines (fastest)", "648 lines", "864 lines", "1080 Lines (Overclock Recommended)"};

// The N64's B button: Circle (Square then C-right), or Square (Circle then C-right).
const char* kBButton[] = {"Circle (Square is C-right)", "Square (Circle is C-right)"};
// The right stick: the C-buttons, or the free camera (free_camera.cpp).
const char* kRightStick[] = {"C-buttons (as on the N64)", "Free camera"};
// 3D (test builds; settings.h's kStereo* hold the numbers).
const char* kStereoDepthName[] = {"Lowest", "Low", "Medium", "High", "Highest"};
const char* kStereoScreenName[] = {"Nearer (more comes out of the screen)", "Normal", "Farther", "Farther still (more goes in)"};
const char* kStereoGhostName[] = {"Off", "1 (weakest)", "2", "3", "4", "5", "6 (strongest, the greyest blacks)"};

// The settings, on tabs: each row a name, its value now, and what Left / Right (dir -1 / 1) do.
enum class Item {
    Resolution, FrameRate, Textures, Antialias,             // Main
    Cutscenes, Picture, Look, BButton, RightStick,          // Extras
    Stereo, StereoDepth, StereoScreen, StereoGhost,         // 3D settings
};
struct Tab { const char* name; std::vector<Item> items; };
const std::vector<Tab>& tabs() {
    static const std::vector<Tab> t = {
        {"Main", {Item::Resolution, Item::FrameRate, Item::Textures, Item::Antialias}},
        {"Extras", {Item::Cutscenes, Item::Picture, Item::Look, Item::BButton, Item::RightStick}},
        {"3D settings", {Item::Stereo, Item::StereoDepth, Item::StereoScreen, Item::StereoGhost}},
    };
    return t;
}

const char* item_name(Item it) {
    switch (it) {
    case Item::Resolution: return "Resolution";
    case Item::FrameRate: return "Frame rate";
    case Item::Textures: return "Textures";
    case Item::Antialias: return "Anti-aliasing";
    case Item::Cutscenes: return "Cutscenes";
    case Item::Picture: return "Picture";
    case Item::Look: return "Look";
    case Item::BButton: return "B button";
    case Item::RightStick: return "Right stick";
    case Item::Stereo: return "3D";
    case Item::StereoDepth: return "Depth";
    case Item::StereoScreen: return "Screen depth";
    case Item::StereoGhost: return "Ghost cancellation";
    }
    return "";
}

std::string item_value(Item it) {
    const bool stereo = g_settings.stereo3d != 0;
    switch (it) {
    case Item::Resolution: return stereo ? std::string("648 lines (3D is on)") : kResolution[g_settings.scale];
    case Item::FrameRate: return stereo ? "30 fps (3D is on)" : g_settings.fps60 ? "60 fps (experimental, interpolation)" : "30 fps (as on the N64)";
    case Item::Textures:
        return !g_texture_pack_found ? "The game's own (no texture pack found)" : g_settings.texture_pack ? "Texture pack" : "The game's own";
    case Item::Antialias:
        return stereo ? "Off (3D is on)" : !g_settings.antialias ? "Off" : g_settings.scale >= 5 ? "On (not at 1080 lines)" :
               g_settings.scale == 4 ? "On (2x at 864 lines)" : "On (4x)";
    case Item::Cutscenes: return g_settings.cutscenes_4x3 ? "4:3, black bars at the sides" : "Widescreen";
    case Item::Picture: return g_settings.fill ? "Fill the screen" : "With the N64's borders";
    case Item::Look: return g_settings.n64_look ? "Like the N64 (filter, dither, soft)" : "Clean (sharper at angles)";
    case Item::BButton: return kBButton[g_settings.b_on_square];
    case Item::RightStick: return kRightStick[g_settings.free_camera];
    case Item::Stereo: return stereo ? "On (3D TVs)" : "Off";
    case Item::StereoDepth: return kStereoDepthName[g_settings.stereo_depth];
    case Item::StereoScreen: return kStereoScreenName[g_settings.stereo_screen];
    case Item::StereoGhost: return kStereoGhostName[g_settings.stereo_ghost];
    }
    return "";
}

void item_change(Item it, int dir) {
    switch (it) {
    case Item::Resolution: {
        const int n = (int)kMaxPlayerScale - 1;  // scales 2..kMaxPlayerScale
        g_settings.scale = (uint32_t)((int)g_settings.scale - 2 + dir + n) % n + 2;
        break;
    }
    case Item::FrameRate: if (!g_settings.stereo3d) g_settings.fps60 = !g_settings.fps60; break;  // 3D: 30 fps
    case Item::Textures: if (g_texture_pack_found) g_settings.texture_pack = !g_settings.texture_pack; break;
    case Item::Antialias: g_settings.antialias = !g_settings.antialias; break;
    case Item::Cutscenes: g_settings.cutscenes_4x3 = !g_settings.cutscenes_4x3; break;
    case Item::Picture: g_settings.fill = !g_settings.fill; break;
    case Item::Look: g_settings.n64_look = !g_settings.n64_look; break;
    case Item::BButton: g_settings.b_on_square = !g_settings.b_on_square; break;
    case Item::RightStick: g_settings.free_camera = !g_settings.free_camera; break;
    case Item::Stereo: g_settings.stereo3d = !g_settings.stereo3d; break;
    case Item::StereoDepth: g_settings.stereo_depth = (g_settings.stereo_depth + dir + 5) % 5; break;
    case Item::StereoScreen: g_settings.stereo_screen = (g_settings.stereo_screen + dir + 4) % 4; break;
    case Item::StereoGhost: g_settings.stereo_ghost = (g_settings.stereo_ghost + dir + 7) % 7; break;
    }
}

// Where the focus is: the tab strip (kOnTabs), a row of the tab, or the "Start the game" button (the row
// after the last).
constexpr int kOnTabs = -1;

// An outlined box.
void frame(Canvas& cv, int x0, int y0, int x1, int y1, int t, uint32_t c) {
    cv.rect(x0, y0, x1, y0 + t, c);
    cv.rect(x0, y1 - t, x1, y1, c);
    cv.rect(x0, y0, x0 + t, y1, c);
    cv.rect(x1 - t, y0, x1, y1, c);
}

void draw(Canvas& cv, int tab, int row) {
    const float k = cv.h / 1080.0f;
    auto X = [&](int v) { return (int)(v * k); };
    const uint32_t kBack = 0x00101820, kPanel = 0x00182434, kTabOff = 0x00121B28, kEdge = 0x00506C90, kText = 0x00A0A8B0;
    cv.fill(kBack);
    cv.rect(0, 0, cv.w, X(150), 0x00203048);
    cv.centered(X(40), "Conker's Bad Fur Day", 0x00FFC040, 1.6f * k);
    // The panel, with its tabs along the top (the chosen one joined to the panel).
    const int left = (int)cv.w / 2 - X(800), right = (int)cv.w / 2 + X(800);
    const int tabs_top = X(200), top = X(280), bottom = X(870), line = std::max(1, X(3));
    const auto& ts = tabs();
    const int n = (int)ts.size();
    const int tab_w = (right - left) / n;
    cv.rect(left, top, right, bottom, kPanel);
    frame(cv, left, top, right, bottom, line, kEdge);
    for (int i = 0; i < n; i++) {
        const int x0 = left + i * tab_w + (i ? X(6) : 0), x1 = left + (i + 1) * tab_w;
        const bool sel = i == tab;
        const int y0 = sel ? tabs_top - X(8) : tabs_top;
        cv.rect(x0, y0, x1, top + (sel ? line : 0), sel ? kPanel : kTabOff);
        frame(cv, x0, y0, x1, top + line, line, kEdge);
        if (sel) cv.rect(x0 + line, top - line, x1 - line, top + line, kPanel);  // open into the panel
        const bool focus = sel && row == kOnTabs;
        const std::string name = focus ? std::string("< ") + ts[i].name + " >" : ts[i].name;
        const int tw = cv.text(0, 0, name, 0, 0.95f * k, true);
        cv.text(x0 + (x1 - x0 - tw) / 2, y0 + X(18), name, sel ? (focus ? 0x00FFE080 : 0x00FFFFFF) : 0x00708090, 0.95f * k);
    }
    // The tab's rows: the name, and the value in a box.
    const auto& items = ts[tab].items;
    const int name_x = left + X(60), box_x0 = left + X(620), box_x1 = right - X(60);
    for (int i = 0; i < (int)items.size(); i++) {
        const int y = top + X(45) + i * X(84);
        const bool sel = i == row;
        if (sel) cv.rect(left + X(20), y - X(14), right - X(20), y + X(70), 0x00253A56);
        cv.text(name_x, y + X(6), item_name(items[i]), sel ? 0x00FFFFFF : kText, k);
        cv.rect(box_x0, y - X(4), box_x1, y + X(60), 0x000C121C);
        frame(cv, box_x0, y - X(4), box_x1, y + X(60), line, sel ? 0x00FFC040 : kEdge);
        cv.text(box_x0 + X(24), y + X(6), item_value(items[i]), sel ? 0x00FFE080 : kText, 0.9f * k);
        if (sel) {
            const int aw = cv.text(0, 0, "<  >", 0, 0.9f * k, true);
            cv.text(box_x1 - aw - X(20), y + X(6), "<  >", 0x00FFC040, 0.9f * k);
        }
    }
    // "Start the game".
    {
        const bool sel = row == (int)items.size();
        const int bw = X(460), x0 = ((int)cv.w - bw) / 2, y0 = bottom - X(120), y1 = bottom - X(40);
        cv.rect(x0, y0, x0 + bw, y1, sel ? 0x00306040 : 0x00203028);
        frame(cv, x0, y0, x0 + bw, y1, line, sel ? 0x0080FF80 : kEdge);
        const int tw = cv.text(0, 0, "Start the game", 0, 1.0f * k, true);
        cv.text(x0 + (bw - tw) / 2, y0 + X(18), "Start the game", sel ? 0x0080FF80 : kText, 1.0f * k);
    }
    cv.centered(X(905), "L1 / R1: tabs     Up / Down: choose     Left / Right: change     Start: play", 0x00808890, 0.7f * k);
#if defined(CONKER_RELEASE)
    if (tab == 2)
        cv.centered(X(960), "Screen depth: what sits at the TV's surface.  Ghost cancellation: takes out the faint copy of the other eye's picture.", 0x00808890, 0.6f * k);
    else
        cv.centered(X(960), "In the game: hold Select + Start to quit", 0x00808890, 0.7f * k);
    cv.centered(X(1020), "Version " CONKER_VERSION, 0x00606870, 0.6f * k);
#else
    if (tab == 2)
        cv.centered(X(960), "Screen depth: what sits at the TV's surface.  Ghost cancellation: takes out the faint copy of the other eye's picture.", 0x00808890, 0.6f * k);
    else
        cv.centered(X(960), "In the game: hold Select + Start to quit, L3 the frame rate (not in 3D)", 0x00808890, 0.7f * k);
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
    int tab = 0;
    int row = (int)tabs()[0].items.size();  // "Start the game"
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
        if (dirty) { draw(cv, tab, row); show(cv); show(cv); dirty = false; }  // both buffers
        else flip();  // both buffers hold the menu
        usleep(4000);
        padInfo info;
        padData pad;
        if (ioPadGetInfo(&info) != 0 || !info.status[0] || ioPadGetData(0, &pad) != 0 || pad.len == 0) continue;
        uint32_t now = 0;
        now |= pad.BTN_UP ? 1u : 0; now |= pad.BTN_DOWN ? 2u : 0; now |= pad.BTN_LEFT ? 4u : 0; now |= pad.BTN_RIGHT ? 8u : 0;
        now |= pad.BTN_CROSS ? 16u : 0; now |= pad.BTN_START ? 32u : 0;
        now |= pad.BTN_L1 ? 64u : 0; now |= pad.BTN_R1 ? 128u : 0;
        // The left stick as the d-pad.
        if (pad.ANA_L_V < 40) now |= 1; if (pad.ANA_L_V > 215) now |= 2; if (pad.ANA_L_H < 40) now |= 4; if (pad.ANA_L_H > 215) now |= 8;
        const uint32_t pressed = now & ~held;
        held = now;
        if (!pressed) continue;
        dirty = true;
        const int ntabs = (int)tabs().size();
        const int nrows = (int)tabs()[tab].items.size();  // then "Start the game"
        const int dir = (pressed & 4) ? -1 : (pressed & 8) ? 1 : 0;
        // Tabs: L1 / R1 anywhere, or Left / Right on the tab strip.
        int tab_dir = (pressed & 64) ? -1 : (pressed & 128) ? 1 : 0;
        if (row == kOnTabs && dir) tab_dir = dir;
        if (tab_dir) {
            tab = (tab + tab_dir + ntabs) % ntabs;
            if (row != kOnTabs) row = 0;
            continue;
        }
        if (pressed & 1) row = row == kOnTabs ? nrows : row - 1;
        if (pressed & 2) row = row == nrows ? kOnTabs : row + 1;
        if (dir && row >= 0 && row < nrows) item_change(tabs()[tab].items[row], dir);
        if ((pressed & 16) && row == kOnTabs) { row = 0; continue; }
        if (!((pressed & 32) || ((pressed & 16) && row == nrows))) continue;
        // Start (or Cross on "Start the game"): play.
        {
            save_settings();
            clear_screen(0);
            std::printf("[ps3] settings: %u lines, %s, cutscenes %s, picture %s, anti-aliasing %s\n", 216 * g_settings.scale, g_settings.fps60 ? "60 fps" : "30 fps",
                        g_settings.cutscenes_4x3 ? "4:3" : "widescreen", g_settings.fill ? "filling the screen" : "with borders", g_settings.antialias ? "on" : "off");
            std::printf("[ps3] look: %s\n", g_settings.n64_look ? "like the N64 (3-point filter, dither, soft video output)" : "clean (anisotropic filtering)");
            std::printf("[ps3] B button: %s\n", g_settings.b_on_square ? "Square (Circle is C-right)" : "Circle (Square is C-right)");
            std::printf("[ps3] right stick: %s\n", g_settings.free_camera ? "free camera" : "C-buttons");
            std::printf("[ps3] 3D: %s, depth %d, screen depth %d, ghost cancellation %d\n", g_settings.stereo3d ? "on" : "off",
                        g_settings.stereo_depth, g_settings.stereo_screen, g_settings.stereo_ghost);
            return true;
        }
    }
}

#ifdef MENU_TEST
// Workstation preview (tools/menu-font/preview.sh): the menu as an image.
void menu_preview(uint32_t* out, uint32_t w, uint32_t h, int row) {
    Canvas cv; cv.w = w; cv.h = h; cv.px.assign((size_t)w * h, 0);
    draw(cv, row / 10, row % 10 == 9 ? kOnTabs : row % 10);
    std::memcpy(out, cv.px.data(), (size_t)w * h * 4);
}
#endif
