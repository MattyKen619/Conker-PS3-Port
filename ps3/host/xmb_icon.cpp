// The release build's XMB icon from the player's ROM: see xmb_icon.h.
#include "xmb_icon.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "paths.h"
#include "miniz.h"

namespace xmb_icon {

#if defined(CONKER_RELEASE)
namespace {
constexpr const char* kIcon = CONKER_GAME_DIR "/ICON0.PNG";
// A copy of the icon made, kept with the saves: installing a package (an update, or the same one again)
// puts its stand-in back, and the copy goes back over it at the next start.
constexpr const char* kKept = CONKER_DATA_DIR "/ICON0.PNG";
constexpr uint32_t kW = 320, kH = 176;  // the XMB's icon size
std::atomic<bool> g_wanted{false};

bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// A box of the picture scaled to fit 320 x 176 (area averaging), centred, see-through around it.
std::vector<uint8_t> fit(const std::vector<uint8_t>& src, uint32_t sw, uint32_t x0, uint32_t y0, uint32_t bw, uint32_t bh) {
    std::vector<uint8_t> out((size_t)kW * kH * 4, 0);
    const float k = std::min((float)kW / bw, (float)kH / bh);
    const uint32_t ow = std::max(1u, (uint32_t)(bw * k)), oh = std::max(1u, (uint32_t)(bh * k));
    const uint32_t ox = (kW - ow) / 2, oy = (kH - oh) / 2;
    for (uint32_t y = 0; y < oh; y++) {
        const float fy0 = y / k, fy1 = (y + 1) / k;
        for (uint32_t x = 0; x < ow; x++) {
            const float fx0 = x / k, fx1 = (x + 1) / k;
            float acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (uint32_t sy = (uint32_t)fy0; sy < std::min<uint32_t>(bh, (uint32_t)std::ceil(fy1)); sy++) {
                const float wy = std::min(fy1, sy + 1.0f) - std::max(fy0, (float)sy);
                for (uint32_t sx = (uint32_t)fx0; sx < std::min<uint32_t>(bw, (uint32_t)std::ceil(fx1)); sx++) {
                    const float wx = std::min(fx1, sx + 1.0f) - std::max(fx0, (float)sx);
                    const uint8_t* p = &src[((size_t)(y0 + sy) * sw + x0 + sx) * 4];
                    const float wgt = wx * wy, a = p[3] / 255.0f;
                    // Colour weighted by alpha, so see-through pixels don't darken the edges.
                    for (int c = 0; c < 3; c++) acc[c] += p[c] * a * wgt;
                    acc[3] += a * wgt;
                    wsum += wgt;
                }
            }
            uint8_t* d = &out[((size_t)(oy + y) * kW + ox + x) * 4];
            if (wsum <= 0 || acc[3] <= 0) continue;
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)std::min(255.0f, acc[c] / acc[3] + 0.5f);
            d[3] = (uint8_t)std::min(255.0f, acc[3] / wsum * 255.0f + 0.5f);
        }
    }
    return out;
}

// Writes a file under a temporary name, then over the one there.
bool write_file(const char* path, const void* data, size_t len) {
    const std::string part = std::string(path) + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    bool ok = f && std::fwrite(data, 1, len, f) == len;
    if (f) std::fclose(f);
    if (ok) {
        if (std::rename(part.c_str(), path) != 0) {
            std::remove(path);
            ok = std::rename(part.c_str(), path) == 0;
        }
    }
    std::remove(part.c_str());
    return ok;
}

// The icon over the stand-in, and its copy with the saves.
bool save_icon(const std::vector<uint8_t>& rgba) {
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), (int)kW, (int)kH, 4, &len, 6, MZ_FALSE);
    if (!png) return false;
    const bool ok = write_file(kIcon, png, len);
    if (ok) write_file(kKept, png, len);
    mz_free(png);
    return ok;
}

bool marked(const std::vector<uint8_t>& data) {
    const size_t n = sizeof(kMark) - 1;
    for (size_t i = 0; i + n <= data.size(); i++)
        if (data[i] == 'c' && std::memcmp(&data[i], kMark, n) == 0) return true;
    return false;
}
}  // namespace

void prepare() {
    std::vector<uint8_t> data;
    if (!read_file(kIcon, data)) return;  // not installed as a package
    if (!marked(data)) return;            // the icon made, or the builder's own
    // The stand-in: the icon made before (kept with the saves) goes back at once; else it's made from
    // the title screen.
    std::vector<uint8_t> kept;
    static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (read_file(kKept, kept) && kept.size() > 8 && std::memcmp(kept.data(), kPng, 8) == 0 && !marked(kept) &&
        write_file(kIcon, kept.data(), kept.size()))
        return;
    g_wanted = true;
}

bool wanted() { return g_wanted.load(std::memory_order_relaxed); }

void after_present(n64gfx::RsxBackend& backend, n64gfx::Interpreter& interp, const n64gfx::RsxBackend::ViInfo& vi) {
    (void)interp; (void)vi;
    if (!wanted()) return;
    if (backend.logo_hashes.empty()) {
        // The title screen's logo: 30 tiles of 32x32 (one texture used twice), found in an R3 capture of
        // the title screen (2026-11-11). The hashes are of the decoded textures (n64gfx's ContentHash).
        backend.logo_hashes = {
            0x0055c7dc45033004ull, 0x07a98d6daa4f9812ull, 0x0dbb8b9356154465ull, 0x11e44bee209011b3ull, 0x3892f9d9f21b9ebbull,
            0x4ed893786858ef7aull, 0x5489b7405210d8ecull, 0x650e809819127d8cull, 0x7e56377bb8db5bdfull, 0x816efa106b0706baull,
            0x863ae85d79313469ull, 0x88c06a6dd6319b6bull, 0x8b5dbe6087661f9dull, 0x8e92002a272ed11bull, 0x8f4e418bdbb4f4c6ull,
            0x930a9a5aecc02739ull, 0xac4af64aa31dc748ull, 0xb95a10581be4b081ull, 0xc526743879e323c8ull, 0xd3da12ecc2fff760ull,
            0xd669e9418028529aull, 0xdceba32fc253797bull, 0xdd25be83e08b5ee8ull, 0xdd96f245365bbe96ull, 0xe09756a0dea8c754ull,
            0xe538bf78c88470ccull, 0xe59c0c72d28dc752ull, 0xf5d16f27cb465a34ull, 0xf8c82ee69e68e85full};
        backend.logo_min_tiles = 30;
        backend.logo_min_lists = 15;  // half a second on the screen, whole
        return;
    }
    std::vector<uint8_t> logo;
    uint32_t w = 0, h = 0;
    if (!backend.logo_image(logo, &w, &h)) return;
    g_wanted = false;  // once, whatever happens
    backend.logo_hashes.clear();
    // The box of what can be seen (the tiles' edges are see-through).
    uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            if (logo[((size_t)y * w + x) * 4 + 3] > 8) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1); }
    if (x1 <= x0 || y1 <= y0) return;
    save_icon(fit(logo, w, x0, y0, x1 - x0, y1 - y0));
}

#else  // the test builds keep their icon

void prepare() {}
bool wanted() { return false; }
void after_present(n64gfx::RsxBackend&, n64gfx::Interpreter&, const n64gfx::RsxBackend::ViInfo&) {}

#endif

}  // namespace xmb_icon
