// Turns N64 color combiner modes into RSX fragment programs (assembly for fpasm). Portable, so the
// generator can be checked on the workstation (tools/native-reference/fpgen_check.cpp).
//
// Fragment inputs (written by the backend per vertex, see the RSX vertex program):
//   f[COL0]  shade color          f[TEX0].xy  texture 0 coordinates, .z fog color alpha
//   f[TEX1]  .xy texture 1 coordinates, .z alpha-test threshold (0 = off)
//   f[TEX2]  primitive color      f[TEX3]     environment color
//   f[TEX4]  .x primitive LOD fraction, .yzw fog color
#pragma once
#include <cstdint>
#include <string>

#include "n64gfx.h"

namespace n64gfx {

// Gray: texture 0 in shades of grey (its luma), for showing a frame in black and white.
enum class ProgramMode : uint8_t { Combine1 = 0, Combine2 = 1, Copy = 2, Shade = 3, Gray = 4 };

struct ProgramKey {
    uint32_t combine_hi = 0, combine_lo = 0;  // only the cycles in use
    ProgramMode mode = ProgramMode::Shade;
    uint8_t tex_mask = 0;     // bit 0: texture 0 bound, bit 1: texture 1 bound
    bool alpha_test = false;  // discard when combined alpha < f[TEX1].z
    bool fog = false;         // blend the fog color in by shade alpha (2-cycle fog blender)
    // The 2-cycle blender's first cycle mixing the pixel and the fog color by the fog color's alpha
    // (a constant): 1 = IN * FOG_A + FOG * (1 - FOG_A) (Conker's charred models: fog black, alpha low),
    // 2 = FOG * FOG_A + IN * (1 - FOG_A).
    uint8_t fog_const = 0;
    uint64_t hash() const {
        return ((uint64_t)combine_hi << 40) ^ ((uint64_t)combine_lo << 8) ^ ((uint64_t)mode << 4) ^
               ((uint64_t)tex_mask << 2) ^ ((uint64_t)alpha_test << 1) ^ (uint64_t)fog ^ ((uint64_t)fog_const << 6) ^
               ((uint64_t)combine_lo >> 24 << 60);
    }
    bool operator==(const ProgramKey& o) const {
        return combine_hi == o.combine_hi && combine_lo == o.combine_lo && mode == o.mode &&
               tex_mask == o.tex_mask && alpha_test == o.alpha_test && fog == o.fog && fog_const == o.fog_const;
    }
};

// Builds the key for a draw: which cycles count, which textures are bound, alpha test and fog.
ProgramKey make_program_key(const DrawState& st, bool fill_rect);

// The fragment program's assembly ("!!ARBfp1.0").
std::string generate_program(const ProgramKey& key);

// Alpha-test threshold the backend passes in f[TEX1].z (0 = no test), from the RDP modes.
float alpha_threshold(const DrawState& st);

}  // namespace n64gfx
