// Settings (settings.h): load and save.
#include "settings.h"
#include "paths.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

Settings g_settings;

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
#if !defined(CONKER_RELEASE)
        else if (k == "fps") g_settings.fps60 = v != "30";
#endif
        else if (k == "cutscenes") g_settings.cutscenes_4x3 = v == "4:3";
        else if (k == "picture") g_settings.fill = v != "borders";
    }
}

void save_settings() {
    std::ofstream f(kFile);
    f << "resolution=" << g_settings.scale << "\n"
      << "fps=" << (g_settings.fps60 ? "60" : "30") << "\n"
      << "cutscenes=" << (g_settings.cutscenes_4x3 ? "4:3" : "wide") << "\n"
      << "picture=" << (g_settings.fill ? "fill" : "borders") << "\n";
}
