// diag.c - diagnostics for bring-up on the PS3: one log file for all threads, and crash reports.
//
// Log: newlib on the PS3 gives each thread its own stdout/stderr, which write to the debug TTY
// (only visible on development hardware). Overriding PSL1GHT's __librt_write_r sends every write to
// file descriptors 1 and 2, from any thread, to the log file as well.
//
// Crashes: tries to install an lv2 PPU exception handler (liblv2dbg). Where the firmware allows it, a
// crash writes the faulting address, the registers and a call chain to the log before exiting.
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <net/socket.h>
#include <sys/lv2errno.h>
#include <sys/tty.h>
#include <sys/file.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/process.h>
#include <sys/dbg.h>
#include <lv2/sysfs.h>
#include <sysmodule/sysmodule.h>

static s32 g_log_fd = -1;
static sys_mutex_t g_log_lock;
static int g_lock_ready = 0;
static int g_log_sync = 0;

// With sync on, every log write is flushed to the disk, so the log survives a console freeze
// (a crash on retail firmware) at the cost of speed. Used by the test apps.
void ps3_log_set_sync(int on) { g_log_sync = on; }

static void raw_log_write(const void* p, size_t n) {
    if (g_log_fd < 0) return;
    u64 done = 0;
    if (g_lock_ready) sysMutexLock(g_log_lock, 0);
    sysLv2FsWrite(g_log_fd, p, n, &done);
    if (g_log_sync) sysLv2FsFsync(g_log_fd);
    if (g_lock_ready) sysMutexUnlock(g_log_lock);
}

void ps3_log_open(const char* path) {
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);
    attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
    if (sysMutexCreate(&g_log_lock, &attr) == 0) g_lock_ready = 1;
    s32 fd;
    if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0) == 0) {
        g_log_fd = fd;
    }
}

// The release build has no log; a crash, abort or fatal error writes a crash report instead (a file
// that only exists after something went wrong, for bug reports).
static const char* g_crash_path = 0;
static const char* g_crash_title = "";
void ps3_set_crash_report(const char* path, const char* title) { g_crash_path = path; g_crash_title = title; }
void ps3_crash_begin(void) {
    if (g_log_fd >= 0 || !g_crash_path) return;
    s32 fd;
    if (sysLv2FsOpen(g_crash_path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0) != 0) return;
    g_log_fd = fd;
    g_log_sync = 1;
    char head[256];
    int n = snprintf(head, sizeof head, "%s crash report. Please attach this file to a bug report.\n", g_crash_title);
    if (n > 0) raw_log_write(head, (size_t)(n < (int)sizeof head ? n : (int)sizeof head - 1));
}

// printf-style write straight to the log (safe from any thread, including the crash handler).
void ps3_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    if (n > 0) raw_log_write(buf, (size_t)n);
}

// Replaces PSL1GHT's write (ppu/librt/write.c): identical, except that stdout and stderr also go to
// the log file.
_ssize_t __librt_write_r(struct _reent* r, int fd, const void* ptr, size_t len) {
    if (fd & SOCKET_FD_MASK) return send(fd, ptr, len, 0);
    if (fd == 1 || fd == 2) {
#if !defined(CONKER_RELEASE)  // the release build writes no log: its prints go nowhere
        raw_log_write(ptr, len);
        u32 done = 0;
        sysTtyWrite(fd, ptr, len, &done);
#endif
        return (_ssize_t)len;
    }
    u64 done = 0;
    s32 ret = sysLv2FsWrite(fd, ptr, len, &done);
    if (ret) return (_ssize_t)lv2errno_r(r, ret);
    return (_ssize_t)done;
}

// abort() (also reached from failed assert()s and std::terminate): say so before exiting.
void abort(void) {
    ps3_crash_begin();
    ps3_log("\n[ps3] abort() called: the program stopped itself (failed check or unhandled error).\n");
    sysProcessExit(134);
    for (;;) {}
}

// ---------------------------------------------------------------------------------- crashes
static const char* cause_name(u64 cause) {
    switch (cause) {
        case 0x01: return "trap";
        case 0x02: return "privileged instruction";
        case 0x04: return "illegal instruction";
        case 0x08: return "instruction storage (bad jump target)";
        case 0x10: return "data storage (bad memory access)";
        case 0x20: return "alignment";
        case 0x40: return "floating point";
        case 0x80: return "data segment";
        case 0x100: return "instruction segment";
        default: return "unknown";
    }
}

static void crash_handler(u64 cause, sys_ppu_thread_t tid, u64 dar) {
    ps3_crash_begin();
    char name[32] = {0};
    sysDbgGetPPUThreadName(tid, name);
    ps3_log("\n[ps3] CRASH: %s (cause 0x%llx) in thread '%s', address 0x%08llx\n",
            cause_name(cause), (unsigned long long)cause, name, (unsigned long long)dar);
    sys_dbg_ppu_thread_context_t ctx;
    if (sysDbgReadPPUThreadContext(tid, &ctx) == 0) {
        ps3_log("[ps3]   pc=0x%08x lr=0x%08x ctr=0x%08x\n", ctx.pc, ctx.lr, ctx.ctr);
        for (int i = 0; i < 32; i += 4) {
            ps3_log("[ps3]   r%-2d %016llx %016llx %016llx %016llx\n", i,
                    (unsigned long long)ctx.gpr[i], (unsigned long long)ctx.gpr[i + 1],
                    (unsigned long long)ctx.gpr[i + 2], (unsigned long long)ctx.gpr[i + 3]);
        }
        // Call chain: 64-bit PowerPC frames start with the back chain; the caller's frame holds the
        // saved LR at +16.
        ps3_log("[ps3]   call chain:");
        u64 sp = ctx.gpr[1];
        for (int depth = 0; depth < 24; depth++) {
            if (sp == 0 || (sp & 7) || sp < 0x10000 || sp > 0xFFFFFFFFull) break;
            u64 next = *(volatile u64*)(uintptr_t)sp;
            if (next <= sp || next - sp > 0x100000) break;
            u64 lr = *(volatile u64*)(uintptr_t)(next + 16);
            ps3_log(" 0x%08llx", (unsigned long long)lr);
            sp = next;
        }
        ps3_log("\n");
    } else {
        ps3_log("[ps3]   (could not read the thread's registers)\n");
    }
    ps3_log("[ps3] exiting after crash\n");
    sysProcessExit(139);
}

void ps3_install_crash_handler(void) {
    s32 r1 = sysModuleLoad(SYSMODULE_LV2DBG);
    s32 r2 = sysDbgInitializePPUExceptionHandler(0);
    s32 r3 = sysDbgRegisterPPUExceptionHandler(crash_handler, 1 /* stop the faulting thread */);
    ps3_log("[ps3] crash handler: module %d, init %d, register %d (%s)\n", r1, r2, r3,
            (r2 == 0 && r3 == 0) ? "active" : "not available on this firmware");
}
