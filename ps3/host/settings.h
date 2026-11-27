// The player's settings, chosen in the startup menu (menu.cpp) and kept in settings.txt in the data
// folder (paths.h). L3 in the game changes the frame rate too (test builds; not in 3D).
#pragma once
#include <atomic>
#include <cstdint>

// The render scales the player can choose: 432 lines up to 1080.
constexpr uint32_t kMaxPlayerScale = 5;

struct Settings {
#if defined(CONKER_RELEASE)
    uint32_t scale = 3;          // render scale: 2..5 = 432, 648, 864, 1080 lines (648 by default)
#else
    uint32_t scale = 4;          // render scale: 2..5 = 432, 648, 864, 1080 lines
#endif
    bool fps60 = false;          // in-between frames (60 a second; experimental: a frame of input lag)
    bool cutscenes_4x3 = false;  // cutscenes with black bars at the sides
    bool fill = true;            // the picture fills the screen's height (else the N64's borders)
    bool texture_pack = true;    // a texture pack's pictures, if one is there (texture_pack_host.h)
    bool antialias = false;
    // 3D TVs: the PS3's stereoscopic 3D (720p frame packing), on its own settings tab.
    int stereo3d = 0;            // 0 off, 1 on
    int stereo_depth = 2;        // 0-4: lowest .. highest (kStereoDepth)
    int stereo_screen = 1;       // 0-3: what's at the screen's depth: nearer .. farthest (kStereoScreen)
    int stereo_ghost = 0;        // 0-6: ghost cancellation off, or how much of the other eye's picture (kStereoGhost)
    bool free_camera = false;    // the right stick turns the camera freely (the PC port's free camera, free_camera.cpp); else it's the C-buttons
    bool b_on_square = false;    // the N64's B button on Square (Circle then C-right), like other PS3 N64 ports; else on Circle
    bool n64_look = false;       // the picture: clean (sharper at angles) or like the N64 (dither, soft video output)      // the RSX's multisampling (4x at 432 and 648 lines, 2x at 864; not at 1080; test builds for now)
};

// 3D's values (the renderer) for the settings above. Depth: each eye's farthest shift in -1..1 units (the
// two pictures of the farthest things that much of the width apart); screen: the distance (the game's w) at
// the screen's depth (v99-v101: 400); ghost: how much of the other eye's picture a 3D TV is taken to leak.
constexpr float kStereoDepth[5] = {0.010f, 0.015f, 0.025f, 0.035f, 0.045f};
constexpr float kStereoScreen[4] = {300.0f, 400.0f, 550.0f, 750.0f};
constexpr float kStereoGhost[7] = {0.0f, 0.025f, 0.05f, 0.075f, 0.10f, 0.125f, 0.15f};
// A texture pack was found in the data folder (main.cpp looks before the menu).
extern bool g_texture_pack_found;
extern Settings g_settings;
void load_settings();
void save_settings();
// The startup menu: returns when the player starts the game (false if they quit to the XMB).
bool run_startup_menu(const std::atomic<bool>& quit);
// The PS button's system menu is open (main.cpp's system event callback sets it).
extern std::atomic<bool> g_system_menu_open;
// A screen with one line of text in the middle (the menu's look), until the next picture.
void show_message_screen(const char* text);
