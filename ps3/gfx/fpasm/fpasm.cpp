// cgcomp's compileFP() (PSL1GHT tools/cgcomp/source/main.cpp, MIT), writing to memory.
#include "fpasm.h"

#include "types.h"
#include "fpparser.h"
#include "compilerfp.h"

#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define SWAP16(v) (v)
#define SWAP32(v) (v)
#else
#define SWAP16(v) (u16)((((v) >> 8) & 0xFF) | (((v) & 0xFF) << 8))
#define SWAP32(v) (u32)((((v) >> 24) & 0xFF) | (((v) & 0xFF) << 24) | (((v) & 0xFF00) << 8) | (((v) & 0xFF0000) >> 8))
#endif

static u32 endian_fp(u32 v) { return ((v >> 16) & 0xffff) | ((v & 0xffff) << 16); }

bool fpasm::assemble(const std::string& source, std::vector<uint8_t>& out) {
    std::string src = source;  // the parser writes into its input
    CFPParser parser;
    CCompilerFP compiler;
    parser.Parse(src.c_str());
    compiler.Compile(&parser);
    if (compiler.GetInstructionCount() <= 0 || compiler.GetUnsupportedCount() > 0) return false;

    std::vector<unsigned char> buf(64 * 1024, 0);
    unsigned char* fragmentprogram = buf.data();
    u16 magic = ('F' << 8) | 'P';
    u32 lastoff = sizeof(rsxFragmentProgram);
    rsxFragmentProgram* fp = (rsxFragmentProgram*)fragmentprogram;
    fp->magic = SWAP16(magic);
    fp->num_regs = SWAP16((u16)compiler.GetNumRegs());
    fp->fp_control = SWAP32((u32)compiler.GetFPControl());
    fp->texcoords = SWAP16((u16)compiler.GetTexcoords());
    fp->texcoord2D = SWAP16((u16)compiler.GetTexcoord2D());
    fp->texcoord3D = SWAP16((u16)compiler.GetTexcoord3D());
    while (lastoff & 3) fragmentprogram[lastoff++] = 0;
    // No attributes or user constants: n64gfx passes everything through interpolants.
    fp->attr_off = SWAP32(lastoff);
    fp->num_attr = SWAP16(0);
    fp->const_off = SWAP32(lastoff);
    fp->num_const = SWAP16(0);
    while (lastoff & 15) fragmentprogram[lastoff++] = 0;
    fp->ucode_off = SWAP32(lastoff);
    fp->num_insn = SWAP16((u16)compiler.GetInstructionCount());
    struct fragment_program_exec* fpi = compiler.GetInstructions();
    u32* dst = (u32*)(fragmentprogram + lastoff);
    for (int i = 0, n = 0; i < compiler.GetInstructionCount(); i++, n += 4, lastoff += 16) {
        dst[n + 0] = endian_fp(SWAP32(fpi[i].data[0]));
        dst[n + 1] = endian_fp(SWAP32(fpi[i].data[1]));
        dst[n + 2] = endian_fp(SWAP32(fpi[i].data[2]));
        dst[n + 3] = endian_fp(SWAP32(fpi[i].data[3]));
    }
    out.assign(fragmentprogram, fragmentprogram + lastoff);
    return true;
}
