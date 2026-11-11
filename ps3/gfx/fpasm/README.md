Fragment program assembler from PSL1GHT's cgcomp (tools/cgcomp; MIT, see LICENSE.PSL1GHT), unchanged
apart from this README, `types.h` (uses PSL1GHT's integer types on the PS3), and `compilerfp.cpp`/`.h` (counts instructions it does
not support, such as SUB, which cgcomp silently dropped; fpasm then fails instead) and `fpasm.cpp`/`fpasm.h`, which wrap cgcomp's `compileFP()` so programs can be
assembled at runtime on the PS3: n64gfx generates one fragment program per N64 combiner mode.
