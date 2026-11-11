// Runtime fragment program assembler (PSL1GHT cgcomp's, see README.md).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fpasm {
// Assembles "!!ARBfp1.0" / "!!FP2.0" assembly into the binary cgcomp writes (an rsxFragmentProgram
// followed by its microcode). Returns false if the program has no instructions.
bool assemble(const std::string& source, std::vector<uint8_t>& out);
}
