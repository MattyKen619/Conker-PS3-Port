// Settings (settings.h): load and save.
#include "settings.h"
#include "paths.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

Settings g_settings;
bool g_texture_pack_found = false;

namespace {
constexpr const char* kFile = CONKER_DATA_DIR "/settings.txt";
}

void load_settings() {
    std::ifstream f(kFile);
    if (!f) {
#if defined(CONKER_RELEASE)
        return;
#endif
        // Earlier tests' files: the resolution, and 30fps.
        std::ifstream r("/dev_hdd0/tmp/conkerps3/resolution");
        uint32_t s = 0;
        if (r >> s && s >= 2 && s <= 5) g_settings.scale = s;
        if (std::filesystem::exists("/dev_hdd0/tmp/conkerps3/30fps")) g_settings.fps60 = false;
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "resolution") { const int s = std::atoi(v.c_str()); if (s >= 2 && s <= (int)kMaxPlayerScale) g_settings.scale = (uint32_t)s; }
        else if (k == "fps") g_settings.fps60 = v != "30";
        else if (k == "cutscenes") g_settings.cutscenes_4x3 = v == "4:3";
        else if (k == "picture") g_settings.fill = v != "borders";
        else if (k == "texture_pack") g_settings.texture_pack = v != "off";
        else if (k == "antialias") g_settings.antialias = v == "on";
        else if (k == "look") g_settings.n64_look = v == "n64";
        else if (k == "b_button") g_settings.b_on_square = v == "square";
        else if (k == "right_stick") g_settings.free_camera = v == "camera";
        else if (k == "stereo3d") {
            g_settings.stereo3d = v != "off";
            if (v == "low") g_settings.stereo_depth = 1;   // v99-v101's single setting
            if (v == "medium") g_settings.stereo_depth = 2;
            if (v == "high") g_settings.stereo_depth = 3;
        }
        else if (k == "stereo_depth") { const int n = std::atoi(v.c_str()); if (n >= 0 && n <= 4) g_settings.stereo_depth = n; }
        else if (k == "stereo_screen") { const int n = std::atoi(v.c_str()); if (n >= 0 && n <= 3) g_settings.stereo_screen = n; }
        else if (k == "stereo_ghost") { const int n = std::atoi(v.c_str()); if (n >= 0 && n <= 6) g_settings.stereo_ghost = n; }
    }
}

void save_settings() {
    std::ofstream f(kFile);
    f << "resolution=" << g_settings.scale << "\n"
      << "fps=" << (g_settings.fps60 ? "60" : "30") << "\n"
      << "cutscenes=" << (g_settings.cutscenes_4x3 ? "4:3" : "wide") << "\n"
      << "picture=" << (g_settings.fill ? "fill" : "borders") << "\n"
      << "texture_pack=" << (g_settings.texture_pack ? "on" : "off") << "\n"
      << "antialias=" << (g_settings.antialias ? "on" : "off") << "\n"
      << "look=" << (g_settings.n64_look ? "n64" : "clean") << "\n"
      << "b_button=" << (g_settings.b_on_square ? "square" : "circle") << "\n"
      << "right_stick=" << (g_settings.free_camera ? "camera" : "c_buttons") << "\n"
      << "stereo3d=" << (g_settings.stereo3d ? "on" : "off") << "\n"
      << "stereo_depth=" << g_settings.stereo_depth << "\n"
      << "stereo_screen=" << g_settings.stereo_screen << "\n"
      << "stereo_ghost=" << g_settings.stereo_ghost << "\n";
}
