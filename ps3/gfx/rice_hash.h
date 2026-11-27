// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rice texture checksums, worked out from game memory as a texture is loaded, so that GLideN64 / Rice
// texture packs (keyed by them) can replace textures. See rice_hash.cpp for where the method comes from
// and its license (GPL version 2 or later, unlike the rest of this project).
#pragma once
#include <cstdint>

namespace n64gfx {

struct Tile;

// A LOADBLOCK, LOADTILE or LOADTLUT, as its command gave it (20 bytes: the SPU keeps a few).
struct RiceLoad {
    enum : uint8_t { None, Block, TileLoad, Tlut };
    uint8_t type = None;
    uint8_t siz = 0, masks = 0, maskt = 0;        // the loaded tile's
    uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;  // the command's fields (LOADBLOCK's lrt is its dxt)
    uint32_t addr = 0;                            // the texture image (SETTIMG) it read
    uint16_t width = 0;
    uint8_t tsiz = 0;
    uint8_t pad = 0;
    uint16_t tmem = 0;                            // the loaded tile's TMEM address (words)
    uint16_t pad2 = 0;
};

// The last loads: of textures (by the TMEM address loaded to; the newest replaces the oldest) and of
// palettes (one per 16 entries of the TLUT half of TMEM).
struct RiceLoads {
    static constexpr uint32_t kTex = 8, kPal = 16;
    RiceLoad tex[kTex], pal[kPal];
    uint32_t next = 0;
    void note(const RiceLoad& r);
    // The load a tile at this TMEM address samples, and the palette's (type None if not known).
    void texture(uint32_t tmem, RiceLoad* out) const;
    void palette(uint32_t siz, uint32_t pal_index, RiceLoad* out) const;
};

// The Rice checksum of the texture a draw's tile samples (width x height, with this TLUT mode) from its
// load, and of its palette when there is one; hashed_w x hashed_h: the texels hashed, from the texture's
// top left, which a pack's picture covers. False if the load isn't one Rice hashes. rdram: game memory
// as Memory::rdram (rdram_bytes a power of two).
bool rice_key(const uint8_t* rdram, uint32_t rdram_bytes, const RiceLoad& load, const RiceLoad& palette_load, const Tile& draw,
              uint32_t width, uint32_t height, uint32_t tlut, uint32_t* crc, uint32_t* palette_crc, bool* has_palette,
              uint32_t* hashed_w = nullptr, uint32_t* hashed_h = nullptr);

}  // namespace n64gfx
