// The PS3 side of texture packs: see texture_pack_host.h.
#include "texture_pack_host.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "paths.h"
#include "ultramodern/ultramodern.hpp"
#include "settings.h"

namespace fs = std::filesystem;

namespace texpack {
namespace {
constexpr const char* kDir = CONKER_DATA_DIR;
constexpr const char* kPack = CONKER_DATA_DIR "/texture_pack.pack";
constexpr const char* kStamp = CONKER_DATA_DIR "/texture_pack.txt";
n64gfx::TexturePack g_pack;
bool g_active = false;

// The first .htc in the data folder (empty if none).
fs::path find_htc_in(const char* dir) {
    std::error_code ec;
    fs::path best;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string ext = e.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".htc" && (best.empty() || e.path() < best)) best = e.path();
    }
    return best;
}

// The release also takes one left in the test builds' folder (where the Mac's script sends it), unpacked
// into its own data folder.
fs::path find_htc() {
    fs::path best = find_htc_in(kDir);
#if defined(CONKER_RELEASE)
    if (best.empty()) best = find_htc_in(CONKER_TEST_DIR);
#endif
    return best;
}

std::string stamp_of(const fs::path& htc) {
    std::error_code ec;
    return htc.filename().string() + " " + std::to_string((unsigned long long)fs::file_size(htc, ec));
}
}  // namespace

bool find() {
    std::error_code ec;
    g_texture_pack_found = !find_htc().empty() || fs::exists(kPack, ec);
    return g_texture_pack_found;
}

void prepare() {
    g_active = false;
    if (!g_settings.texture_pack) return;
    std::error_code ec;
    const fs::path htc = find_htc();
    if (!htc.empty()) {
        std::string have;
        { std::ifstream in(kStamp); std::getline(in, have); }
        const std::string want = stamp_of(htc);
        if (have != want || !fs::exists(kPack, ec)) {
            std::printf("[texpack] unpacking %s\n", htc.string().c_str());
            std::remove(kStamp);
            int shown = -1;
            std::string error;
            const bool ok = n64gfx::TexturePack::convert(htc.string().c_str(), kPack, [&](float f) {
                const int pct = (int)(f * 100.0f);
                if (pct / 2 == shown / 2) return;
                shown = pct;
                char text[96];
                std::snprintf(text, sizeof text, "Unpacking the texture pack (the first time only): %d%%", pct);
                show_message_screen(text);
            }, &error);
            if (!ok) {
                std::printf("[texpack] not unpacked: %s\n", error.c_str());
                show_message_screen(("The texture pack can't be used: " + error).c_str());
                sleep(6);
                return;
            }
            std::ofstream(kStamp) << want << "\n";
        }
    }
    // Its background thread gives way to the game's and the graphics thread.
    n64gfx::TexturePack::thread_setup = [] { ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Low); };
    if (!g_pack.open(kPack)) { std::printf("[texpack] no usable pack file\n"); return; }
    std::printf("[texpack] texture pack: %u textures\n", (unsigned)g_pack.count());
    g_active = true;
}

n64gfx::TexturePack* active() { return g_active ? &g_pack : nullptr; }

}  // namespace texpack
