// fenv.h - minimal C floating-point environment for the PS3 PPU.
// newlib for powerpc64-ps3-elf ships no <fenv.h>. The recompiled N64 code only needs rounding-mode
// control (MIPS FCSR RM bits). The PPU's FPSCR RN field (bits 62-63) uses the same encoding as
// MIPS: 0 = nearest, 1 = toward zero, 2 = +infinity, 3 = -infinity.
#ifndef PS3_FENV_H
#define PS3_FENV_H

#define FE_TONEAREST  0
#define FE_TOWARDZERO 1
#define FE_UPWARD     2
#define FE_DOWNWARD   3

#define FE_INEXACT    0x02000000
#define FE_DIVBYZERO  0x04000000
#define FE_UNDERFLOW  0x08000000
#define FE_OVERFLOW   0x10000000
#define FE_INVALID    0x20000000
#define FE_ALL_EXCEPT (FE_INEXACT | FE_DIVBYZERO | FE_UNDERFLOW | FE_OVERFLOW | FE_INVALID)

typedef unsigned int fexcept_t;
typedef double fenv_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline int fegetround(void) {
    union { double d; unsigned long long u; } f;
    __asm__ volatile("mffs %0" : "=f"(f.d));
    return (int)(f.u & 3);
}

static inline int fesetround(int mode) {
    if (mode & ~3) return 1;
    if (mode & 2) __asm__ volatile("mtfsb1 30"); else __asm__ volatile("mtfsb0 30");
    if (mode & 1) __asm__ volatile("mtfsb1 31"); else __asm__ volatile("mtfsb0 31");
    return 0;
}

static inline int fegetenv(fenv_t *env) { __asm__ volatile("mffs %0" : "=f"(*env)); return 0; }
static inline int fesetenv(const fenv_t *env) { __asm__ volatile("mtfsf 0xff,%0" : : "f"(*env)); return 0; }

static inline int feclearexcept(int excepts) {
    union { double d; unsigned long long u; } f;
    __asm__ volatile("mffs %0" : "=f"(f.d));
    f.u &= ~(unsigned long long)(excepts & FE_ALL_EXCEPT);
    __asm__ volatile("mtfsf 0xff,%0" : : "f"(f.d));
    return 0;
}

static inline int fetestexcept(int excepts) {
    union { double d; unsigned long long u; } f;
    __asm__ volatile("mffs %0" : "=f"(f.d));
    return (int)(f.u & (unsigned long long)(excepts & FE_ALL_EXCEPT));
}

#ifdef __cplusplus
}
#endif

#endif
