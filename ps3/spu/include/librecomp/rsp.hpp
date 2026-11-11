// SPU build of the RSP microcode support (replaces librecomp/rsp.hpp for ps3/spu/audio): DMEM lives
// in the SPU's local store, and RDRAM is reached with DMA (dma_spu.cpp). Same layout as the PS3
// (big-endian) build of librecomp/rsp.hpp: DMEM bytes in N64 order.
#ifndef __RSP_H__
#define __RSP_H__

#include <stdint.h>
#include <stdio.h>

#include <librecomp/rsp_vu.hpp>

enum class RspExitReason {
    Invalid,
    Broke,
    ImemOverrun,
    UnhandledJumpTarget,
    Unsupported,
    SwapOverlay,
    UnhandledResumeTarget
};

struct RspContext {
    uint32_t      r1,  r2,  r3,  r4,  r5,  r6,  r7,
             r8,  r9,  r10, r11, r12, r13, r14, r15,
             r16, r17, r18, r19, r20, r21, r22, r23,
             r24, r25, r26, r27, r28, r29, r30, r31;
    uint32_t dma_mem_address;
    uint32_t dma_dram_address;
    uint32_t jump_target;
    RSP rsp;
    uint32_t resume_address;
    bool resume_delay;
};

extern uint8_t dmem[];
extern uint16_t rspReciprocals[512];
extern uint16_t rspInverseSquareRoots[512];

#define RSP_DMEM_BYTE_XOR 0
#define RSP_HOST_BYTE(i, n) (i)

#define RSP_MEM_B(offset, addr) \
    (*reinterpret_cast<int8_t*>(dmem + (0xFFF & ((offset) + (addr)))))

#define RSP_MEM_BU(offset, addr) \
    (*reinterpret_cast<uint8_t*>(dmem + (0xFFF & ((offset) + (addr)))))

static inline uint32_t RSP_MEM_W_LOAD(uint32_t offset, uint32_t addr) {
    uint32_t out;
    for (int i = 0; i < 4; i++) reinterpret_cast<uint8_t*>(&out)[i] = RSP_MEM_BU(offset + i, addr);
    return out;
}

static inline void RSP_MEM_W_STORE(uint32_t offset, uint32_t addr, uint32_t val) {
    for (int i = 0; i < 4; i++) RSP_MEM_BU(offset + i, addr) = reinterpret_cast<uint8_t*>(&val)[i];
}

static inline uint32_t RSP_MEM_HU_LOAD(uint32_t offset, uint32_t addr) {
    uint16_t out;
    for (int i = 0; i < 2; i++) reinterpret_cast<uint8_t*>(&out)[i] = RSP_MEM_BU(offset + i, addr);
    return out;
}

static inline uint32_t RSP_MEM_H_LOAD(uint32_t offset, uint32_t addr) {
    int16_t out;
    for (int i = 0; i < 2; i++) reinterpret_cast<uint8_t*>(&out)[i] = RSP_MEM_BU(offset + i, addr);
    return out;
}

static inline void RSP_MEM_H_STORE(uint32_t offset, uint32_t addr, uint32_t val) {
    for (int i = 0; i < 2; i++) RSP_MEM_BU(offset + i, addr) = reinterpret_cast<uint8_t*>(&val)[i + 2];
}

// From recomp.h.
#define S32(val) ((int32_t)(val))
#define U32(val) ((uint32_t)(val))
#define S64(val) ((int64_t)(val))
#define U64(val) ((uint64_t)(val))

#define RSP_ADD32(a, b) ((int32_t)((a) + (b)))
#define RSP_SUB32(a, b) ((int32_t)((a) - (b)))
#define RSP_SIGNED(val) ((int32_t)(val))

// RDRAM <-> DMEM over DMA (dma_spu.cpp). `rdram` (the PPU's pointer) isn't used: the effective
// address of RDRAM comes with each task.
void spu_dma_rdram_to_dmem(uint32_t dmem_addr, uint32_t dram_addr, uint32_t len);
void spu_dma_dmem_to_rdram(uint32_t dmem_addr, uint32_t dram_addr, uint32_t len);

#define SET_DMA_MEM(mem_addr) dma_mem_address = (mem_addr)
#define SET_DMA_DRAM(dram_addr) dma_dram_address = (dram_addr)
#define DO_DMA_READ(rd_len) spu_dma_rdram_to_dmem(dma_mem_address, dma_dram_address, (rd_len) + 1)
#define DO_DMA_WRITE(wr_len) spu_dma_dmem_to_rdram(dma_mem_address, dma_dram_address, (wr_len) + 1)

#endif
