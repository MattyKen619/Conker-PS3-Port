// sys/mman.h - minimal stand-in for the PS3 (lv2 has no mmap/mprotect for game memory).
// mmap() returns zeroed heap memory and ignores protection; mprotect() always succeeds.
// The runtime uses these only to allocate the emulated N64 RDRAM (and for mod code patching,
// which is never used on the PS3).
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/types.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED  ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif

static inline void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)off;
    void* p = memalign(1024 * 1024, len);
    if (!p) return MAP_FAILED;
    memset(p, 0, len);
    return p;
}
static inline int munmap(void* addr, size_t len) { (void)len; free(addr); return 0; }
static inline int mprotect(void* addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return 0; }
int getpagesize(void);  // defined in ps3/compat/posix_stubs.c

#ifdef __cplusplus
}
#endif
