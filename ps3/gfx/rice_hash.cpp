// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rice texture checksums, worked out live from game memory. This file is under the GNU General Public
// License, version 2 or later, unlike the rest of this project (MIT). It follows CBFD-Recompiled's
// rt64_rice_hash.cpp (recomp/rt64.patch, GPL-2.0-or-later), whose hashing is RT64's texture_hasher
// tool's, "referenced from GlideN64 and RiceVideo to imitate their hashing algorithms":
// - the Rice CRC (RiceCRC32, the palette CRC over cimax + 1 entries) is GlideHQ's, in GLideN64's
//   src/GLideNHQ/TxUtil.cpp, copyright (C) 2007 Hiroshi Morii, GPL version 2 or later;
// - the sizes hashed (ReverseDXT, CalculateMaxCI, the tile and block dimensions) are Rice Video's
//   (Rice1964; mupen64plus-video-rice, GPL version 2 or later), as GLideN64 (Sergey Lipskiy and
//   contributors, GPL version 2) also uses them.
// Differences here: game memory is big-endian as on the N64 (the PC renderers keep it word-swapped and
// read little-endian words, which are the N64's big-endian words), and the result is the two CRCs.
//
// This program is free software; you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation; either version 2 of the License,
// or (at your option) any later version. This program is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License (https://www.gnu.org/licenses/) for
// more details.
#include "rice_hash.h"

#include <algorithm>

#include "n64gfx.h"

namespace n64gfx {
namespace {

uint32_t calculate_dxt(uint32_t txl2words) { return txl2words == 0 ? 1u : (2048 + txl2words - 1) / txl2words; }

uint32_t txl2words(uint32_t width, uint32_t size) {
    static const uint32_t size_bytes[4] = {0, 1, 2, 4};
    return size == 0 ? std::max(1u, width / 16) : std::max(1u, width * size_bytes[size] / 8);
}

int reverse_dxt(uint32_t val, uint32_t width, uint32_t size) {
    if (val == 0x800) return 1;
    int low = 2047 / val;
    if (calculate_dxt(low) > val) low++;
    int high = 2047 / (val - 1);
    if (low == high) return low;
    for (int i = low; i <= high; i++)
        if (txl2words(width, size) == (uint32_t)i) return i;
    return (low + high) / 2;
}

// A word of game memory as the PC renderers read it: an unaligned little-endian read of their
// word-swapped memory, whose byte address a is the N64's a ^ 3. Memory::rdram is in the N64's order on
// the PS3 and word-swapped on a little-endian workstation (n64gfx.h).
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline uint8_t byte_at(const uint8_t* rdram, uint32_t mask, uint32_t a) { return rdram[(a ^ 3) & mask]; }
#else
inline uint8_t byte_at(const uint8_t* rdram, uint32_t mask, uint32_t a) { return rdram[a & mask]; }
#endif
inline uint32_t word_at(const uint8_t* rdram, uint32_t mask, uint32_t a) {
    return (uint32_t)byte_at(rdram, mask, a) | (uint32_t)byte_at(rdram, mask, a + 1) << 8 |
           (uint32_t)byte_at(rdram, mask, a + 2) << 16 | (uint32_t)byte_at(rdram, mask, a + 3) << 24;
}

uint8_t max_ci8(const uint8_t* rdram, uint32_t mask, uint32_t src, uint32_t width, uint32_t height, uint32_t stride) {
    uint8_t val = 0;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            val = std::max(val, byte_at(rdram, mask, src + stride * y + x));
            if (val == 0xFF) return 0xFF;
        }
    return val;
}

uint8_t max_ci4(const uint8_t* rdram, uint32_t mask, uint32_t src, uint32_t width, uint32_t height, uint32_t stride) {
    uint8_t val = 0;
    width >>= 1;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t b = byte_at(rdram, mask, src + stride * y + x);
            val = std::max(val, std::max<uint8_t>(b >> 4, b & 0xF));
            if (val == 0xF) return 0xF;
        }
    return val;
}

uint32_t rice_crc32(const uint8_t* rdram, uint32_t mask, uint32_t src, int width, int height, int size, int stride) {
    uint32_t crc = 0;
    const uint32_t bytes_per_line = (uint32_t)width << size >> 1;
    for (int y = height - 1; y >= 0; y--) {
        uint32_t esi = 0;
        for (int x = (int)bytes_per_line - 4; x >= 0; x -= 4) {
            esi = word_at(rdram, mask, src + (uint32_t)x) ^ (uint32_t)x;
            crc = (crc << 4) + ((crc >> 28) & 15);
            crc += esi;
        }
        esi ^= (uint32_t)y;
        crc += esi;
        src += (uint32_t)stride;
    }
    return crc;
}

}  // namespace

bool rice_key(const uint8_t* rdram, uint32_t rdram_bytes, const RiceLoad& op, const RiceLoad& pal, const Tile& draw,
              uint32_t width, uint32_t height, uint32_t tlut, uint32_t* crc_out, uint32_t* pal_out, bool* has_pal,
              uint32_t* hashed_w, uint32_t* hashed_h) {
    *has_pal = false;
    const uint32_t mask = rdram_bytes - 1;
    uint32_t start, count;
    const uint32_t bytes_offset = (uint32_t)(op.uls >> 2) << op.tsiz >> 1;
    uint32_t bytes_per_row = (uint32_t)op.width << op.tsiz >> 1;
    if (op.type == RiceLoad::Block) {
        const uint32_t words = ((uint32_t)(op.lrs - op.uls) >> (4 - op.siz)) + 1;
        start = op.addr + bytes_offset + bytes_per_row * op.ult;
        count = words << 3;
        bytes_per_row = std::max(bytes_per_row, draw.line << 3);
    } else if (op.type == RiceLoad::TileLoad) {
        const uint32_t rows = 1 + ((op.lrt >> 2) - (op.ult >> 2));
        start = op.addr + bytes_offset + bytes_per_row * (op.ult >> 2);
        count = rows * bytes_per_row;
    } else {
        return false;
    }
    const uint32_t tile_bpr = width << draw.siz >> 1;
    count = std::max(count, std::max(tile_bpr, bytes_per_row) * height);
    if (count == 0 || start >= rdram_bytes || count > rdram_bytes - start) return false;

    int bpl, hash_w, hash_h;
    if (op.type == RiceLoad::TileLoad) {
        uint32_t tw = (uint32_t)(std::max((int)(op.lrs >> 2) - (int)(op.uls >> 2), 0) + 1) & 0x3FF;
        uint32_t th = (uint32_t)(std::max((int)(op.lrt >> 2) - (int)(op.ult >> 2), 0) + 1) & 0x3FF;
        if (op.masks) tw = std::min(tw, 1u << op.masks);
        bpl = (int)((uint32_t)op.width << op.tsiz >> 1);
        hash_w = (int)std::min(tw, (uint32_t)op.width);
        if (op.siz > draw.siz) hash_w <<= op.siz - draw.siz;
        hash_h = (int)(op.maskt ? std::min(th, 1u << op.maskt) : th);
    } else {
        const int tw = std::max((int)(draw.lrs >> 2) - (int)(draw.uls >> 2), 0) + 1;
        const int th = std::max((int)(draw.lrt >> 2) - (int)(draw.ult >> 2), 0) + 1;
        const int mw = draw.masks == 0 ? tw : (1 << draw.masks), mh = draw.maskt == 0 ? th : (1 << draw.maskt);
        const bool clamps = draw.masks == 0 || (draw.cms & 2), clampt = draw.maskt == 0 || (draw.cmt & 2);
        hash_w = (clamps && tw <= 256) ? std::min(mw, tw) : mw;
        hash_h = ((clampt && th <= 256) || mh > 256) ? std::min(mh, th) : mh;
        if (draw.siz == 3) bpl = (int)(draw.line << 4);
        else if (op.lrt == 0) bpl = (int)(draw.line << 3);
        else {
            uint32_t dxt = op.lrt;
            if (dxt > 1) dxt = (uint32_t)reverse_dxt(dxt, op.width, op.tsiz);
            bpl = (int)(dxt << 3);
        }
    }
    if (hash_w <= 0 || hash_h <= 0 || bpl <= 0) return false;
    const uint32_t expected = (uint32_t)(hash_h - 1) * (uint32_t)bpl + ((uint32_t)hash_w << draw.siz >> 1);
    if (expected > count) return false;
    *crc_out = rice_crc32(rdram, mask, start, hash_w, hash_h, (int)draw.siz, bpl);
    if (hashed_w) *hashed_w = (uint32_t)hash_w;
    if (hashed_h) *hashed_h = (uint32_t)hash_h;
    if (tlut == 0) return true;

    // The palette, as loaded last into the TLUT half of TMEM.
    const bool ci4 = draw.siz == 0;
    if (pal.type != RiceLoad::Tlut) return true;
    const uint32_t pal_offset = (uint32_t)(pal.uls >> 2) << pal.tsiz >> 1;
    const uint32_t pal_bpr = (uint32_t)pal.width << pal.tsiz >> 1;
    const uint32_t pal_rows = 1 + ((pal.lrt >> 2) - (pal.ult >> 2));
    const uint32_t pal_words = ((pal.lrs >> 2) - (pal.uls >> 2)) + 1;
    const uint32_t pal_start = pal.addr + pal_offset + pal_bpr * (pal.ult >> 2);
    const uint32_t pal_count = (pal_rows - 1) * pal_bpr + (pal_words << 3);
    if (pal_count == 0 || pal_start >= rdram_bytes || pal_count > rdram_bytes - pal_start) return true;
    const uint8_t cimax = ci4 ? max_ci4(rdram, mask, start, (uint32_t)hash_w, (uint32_t)hash_h, (uint32_t)bpl)
                              : max_ci8(rdram, mask, start, (uint32_t)hash_w, (uint32_t)hash_h, (uint32_t)bpl);
    if (((uint32_t)cimax + 1) * 2 > rdram_bytes - pal_start) return true;
    *pal_out = rice_crc32(rdram, mask, pal_start, cimax + 1, 1, 2, ci4 ? 32 : 512);
    *has_pal = true;
    return true;
}

}  // namespace n64gfx
