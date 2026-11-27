// Texture packs: a GLideN64 texture cache (.htc) of high-resolution pictures replaces the game's textures.
// The .htc is a gzip stream that can only be read from its start, so it's converted once into a pack
// file with an index (convert), read in the background as textures are first drawn (request, take). A texture is found by its
// Rice checksum (rice_hash.h), which the pack's pictures are keyed by.
//
// The pack file, big-endian: "CNKRPACK", uint32 version 2, uint32 count, uint64 offset of the index; the
// pictures (RGBA8, R first, top row first); the index, sorted by key: per picture uint64 key (low word the
// texture's Rice CRC, high word its palette's or 0), uint64 offset, uint32 width, uint32 height.
// tools/texturepack/htc_to_pack.py writes the same file on a computer.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "n64gfx.h"

namespace n64gfx {

class TexturePack {
public:
    TexturePack();
    ~TexturePack();
    // Converts a .htc to a pack file; progress(fraction of the .htc read) is called now and then. False
    // (with *error) if it isn't a cache this reads or the disk is full; the pack file is then removed.
    static bool convert(const char* htc_path, const char* pack_path, const std::function<void(float)>& progress, std::string* error);
    // Opens a pack file: reads its index.
    bool open(const char* pack_path);
    bool is_open() const { return !path_.empty(); }
    size_t count() const { return index_.size(); }

    // A picture to prepare: where it is in the file and the size it's drawn at (the texture's size times
    // 2 or 4, so a texture that repeats still can and powers of two stay so). The picture covers the
    // texels the checksum was made of, cover_w x cover_h of the drawn size from the top left: usually all
    // of it, but a clamped tile smaller than its mask is decoded at the mask's size (the weasels' eyes:
    // 40x40 texels hashed of a 64x64 texture); past the picture its edge is held, as the clamp does.
    // alpha: what the game's own texture's alpha is, which is all the game can use of the picture's:
    // kAlphaKeep (varied: the picture's as it is), kAlphaSolid (all 255: the picture made solid, DXT1),
    // kAlphaCutout (only 0 and 255: the picture's cut at half, DXT1 with its one-bit alpha). (v70)
    enum : uint8_t { kAlphaKeep = 0, kAlphaSolid = 1, kAlphaCutout = 2 };
    struct Job { uint64_t offset = 0; uint32_t pack_w = 0, pack_h = 0, w = 0, h = 0, cover_w = 0, cover_h = 0; uint8_t alpha = kAlphaKeep; };
    uint32_t alpha_kinds[3] = {0, 0, 0};  // textures attached, by Job::alpha (counted by the caller)
    // The pack's picture for a texture of this size and checksums (hashed_w x hashed_h texels of it), if
    // it has one worth using.
    bool find(uint32_t tex_w, uint32_t tex_h, uint32_t hashed_w, uint32_t hashed_h, uint32_t crc, uint32_t palette_crc, bool has_palette,
              Job* job);
    // The picture, resampled to its drawn size (RGBA8). f: a file open on the pack.
    static bool read(std::FILE* f, const Job& job, std::vector<uint8_t>& rgba, std::vector<uint8_t>& scratch);
    // A picture as the RSX takes it (A8R8G8B8): with power-of-two sides and mipmaps wanted, swizzled, each
    // level down to 1x1 after the one before (colours averaged by their alpha); otherwise its rows.
    // With compress (and power-of-two sides): DXT1 if the picture has no see-through pixels, else DXT5
    // (dxt 1 or 5), an eighth or a quarter of the bytes, its levels' blocks one after the other.
    struct Ready { uint32_t w = 0, h = 0, levels = 1; bool swizzled = false; uint8_t dxt = 0; std::vector<uint8_t> data; };
    static void prepare(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, bool mipmaps, bool compress, Ready* out);

    // In the background (a thread of its own, started by the first request): request() a picture for a
    // texture (by its content hash), take() it once ready, forget() one no longer wanted. Pictures wait
    // in memory until taken: the thread pauses while more than kMaxWaiting bytes do.
    void request(uint64_t hash, const Job& job, bool mipmaps, bool compress);
    bool take(uint64_t hash, Ready* out);
    bool take_any(uint64_t* hash, Ready* out);
    void forget(uint64_t hash);
    static constexpr size_t kMaxWaiting = 12u << 20;
    // Called in the background thread as it starts (the PS3 host lowers its priority there).
    static inline std::function<void()> thread_setup;
    uint32_t attached = 0, looked_up = 0, prepared = 0;

private:
    struct Entry { uint64_t key, offset; uint32_t width, height; };
    struct Worker;
    const Entry* find_key(uint64_t key) const;
    std::string path_;
    std::vector<Entry> index_;
    Worker* worker_ = nullptr;
};

}  // namespace n64gfx
