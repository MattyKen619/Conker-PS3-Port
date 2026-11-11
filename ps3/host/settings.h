// The player's settings, chosen in the startup menu (menu.cpp) and kept in settings.txt in the data
// folder (paths.h). Select and L3 in the game change the first two as well (the release build: Select
// only, between its resolutions).
#pragma once
#include <atomic>
#include <cstdint>

// The render scales the player can choose: 432 lines up to 864 (release) or 1080 (test builds).
#if defined(CONKER_RELEASE)
constexpr uint32_t kMaxPlayerScale = 4;
#else
constexpr uint32_t kMaxPlayerScale = 5;
#endif

struct Settings {
#if defined(CONKER_RELEASE)
    uint32_t scale = 3;          // render scale: 2..4 = 432, 648, 864 lines (1080 and 60 fps aren't finished)
#else
    uint32_t scale = 4;          // render scale: 2..5 = 432, 648, 864, 1080 lines
#endif
    bool fps60 = false;          // in-between frames (60 a second; experimental: a frame of input lag)
    bool cutscenes_4x3 = false;  // cutscenes with black bars at the sides
    bool fill = true;            // the picture fills the screen's height (else the N64's borders)
};
extern Settings g_settings;
void load_settings();
void save_settings();
// The startup menu: returns when the player starts the game (false if they quit to the XMB).
bool run_startup_menu(const std::atomic<bool>& quit);
// The PS button's system menu is open (main.cpp's system event callback sets it).
extern std::atomic<bool> g_system_menu_open;
// A screen with one line of text in the middle (the menu's look), until the next picture.
void show_message_screen(const char* text);
