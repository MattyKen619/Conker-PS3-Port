// perf.c - performance measurement on the PS3.
//
// 1. Time budget (always on, cheap): every native thread has a per-thread record. The blocking
//    primitives (ps3compat condition variables and sleeps, the lv2 semaphores behind the runtime's
//    LightweightSemaphore) mark the thread as blocked while they wait. A sampler thread checks every
//    record 1000 times a second, so each thread's share of processor time can be reported.
//
// 2. Function profiler (only in builds where the game code is compiled with -finstrument-functions):
//    each game function's entry and exit push and pop its address on a per-thread shadow stack. The
//    sampler also records the function on top of each running thread's stack, which gives a
//    statistical profile of where the game spends its time.
//
// Reports go to the log every 10 seconds.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <lv2/systime.h>
#include <ppu_intrinsics.h>

#define NOINSTR __attribute__((no_instrument_function))

void ps3_log(const char* fmt, ...);

#define MAX_THREADS 48
#define STACK_DEPTH 512

typedef struct {
    volatile int blocked;      // inside a blocking wait
    volatile int depth;        // shadow stack depth (profiler builds)
    void* stack[STACK_DEPTH];
    char name[28];
    uint32_t active_samples;   // samples where the thread was running
    uint32_t waits;            // blocking waits started (this interval)
    uint32_t registered;
} thread_rec;

static thread_rec g_threads[MAX_THREADS];
static volatile int g_thread_count = 0;
static __thread thread_rec* t_rec = 0;
static __thread int t_reg_busy = 0;

static NOINSTR thread_rec* rec(void) {
    thread_rec* r = t_rec;
    if (r || t_reg_busy) return r;
    t_reg_busy = 1;
    int idx = __sync_fetch_and_add(&g_thread_count, 1);
    if (idx < MAX_THREADS) {
        r = &g_threads[idx];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "thread %d", idx);
        r->registered = 1;
        t_rec = r;
    }
    t_reg_busy = 0;
    return r;
}

// Called by the blocking primitives (ps3_threads.h, lightweightsemaphore.h).
NOINSTR void ps3prof_block(int blocked) {
    thread_rec* r = rec();
    if (!r) return;
    r->blocked = blocked;
    if (blocked) r->waits++;
}

// Called by the blocking primitives when the kernel returns an unexpected error.
static volatile uint32_t g_wait_errors = 0;
NOINSTR void ps3prof_wait_error(const char* where, int code) {
    uint32_t n = __sync_add_and_fetch(&g_wait_errors, 1);
    if (n <= 20 || (n & (n - 1)) == 0) {
        thread_rec* r = rec();
        ps3_log("[perf] wait error #%u: %s returned 0x%08x in thread '%s'\n", n, where, (unsigned)code,
                r ? r->name : "?");
    }
}

// Called by ps3compat::thread when its function returns: the thread no longer counts.
NOINSTR void ps3prof_thread_exit(void) {
    thread_rec* r = t_rec;
    if (r) { r->blocked = 1; r->registered = 0; }
}

// Called by ultramodern::set_native_thread_name (platform.cpp).
NOINSTR void ps3prof_set_name(const char* name) {
    thread_rec* r = rec();
    if (r) snprintf(r->name, sizeof(r->name), "%s", name);
}

// ---------------------------------------------------------------------- function profiler
NOINSTR void __cyg_profile_func_enter(void* fn, void* site) {
    (void)site;
    thread_rec* r = t_rec;
    if (!r) { r = rec(); if (!r) return; }
    int d = r->depth;
    if (d < STACK_DEPTH) r->stack[d] = fn;
    r->depth = d + 1;
}

NOINSTR void __cyg_profile_func_exit(void* fn, void* site) {
    (void)fn; (void)site;
    thread_rec* r = t_rec;
    if (r && r->depth > 0) r->depth--;
}

#define HASH_SIZE 16384
static void* g_func_addr[HASH_SIZE];
static uint8_t g_func_thread[HASH_SIZE];
static uint32_t g_func_hits[HASH_SIZE];
static uint32_t g_func_total = 0;
static int g_profiler_seen = 0;

static NOINSTR void count_func(void* fn, int thread) {
    uint32_t h = (uint32_t)((((uintptr_t)fn >> 2) ^ ((uintptr_t)thread << 20)) * 2654435761u) & (HASH_SIZE - 1);
    for (int i = 0; i < HASH_SIZE; i++) {
        uint32_t k = (h + i) & (HASH_SIZE - 1);
        if (g_func_addr[k] == fn && g_func_thread[k] == thread) { g_func_hits[k]++; return; }
        if (!g_func_addr[k]) { g_func_addr[k] = fn; g_func_thread[k] = (uint8_t)thread; g_func_hits[k] = 1; return; }
    }
}

// --------------------------------------------------------------------------- audio timing
static volatile uint64_t g_audio_ticks = 0;
static volatile uint32_t g_audio_count = 0;
static volatile uint64_t g_audio_max = 0;   // longest task this interval
static volatile uint32_t g_audio_late = 0;  // tasks longer than 1/30 s, the game's audio frame
static uint64_t g_tb_freq = 79800000;

NOINSTR uint64_t ps3prof_now(void) { return __mftb(); }
// Audio tasks run on an SPU: the time the SPU itself spent (the task's own time above includes the wait).
static volatile uint64_t g_audio_spu_ticks = 0;
static volatile uint32_t g_audio_spu_count = 0;
NOINSTR void ps3prof_audio_spu(uint64_t ticks) {
    __sync_fetch_and_add(&g_audio_spu_ticks, ticks);
    __sync_fetch_and_add(&g_audio_spu_count, 1);
}
NOINSTR void ps3prof_audio_task(uint64_t ticks) {
    __sync_fetch_and_add(&g_audio_ticks, ticks);
    __sync_fetch_and_add(&g_audio_count, 1);
    if (ticks > g_audio_max) g_audio_max = ticks;
    if (ticks * 30 > g_tb_freq) g_audio_late++;
}

// ---------------------------------------------------------------------------- sampler/report
static NOINSTR void report(uint32_t samples, uint32_t interval_ms) {
    int n = g_thread_count < MAX_THREADS ? g_thread_count : MAX_THREADS;
    ps3_log("[perf] --- last %u s: processor time per thread (100%% = one of the PPU's 2 hardware threads)\n",
            interval_ms / 1000);
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        thread_rec* r = &g_threads[i];
        uint32_t a = r->active_samples, w = r->waits;
        r->active_samples = 0;
        r->waits = 0;
        total += a;
        if (a * 100 >= samples || w >= 1000) {  // at least 1%, or waiting very often
            ps3_log("[perf]   %5.1f%%  %-22s %7.0f waits/s\n", 100.0 * a / samples, r->name,
                    w * 1000.0 / interval_ms);
        }
    }
    ps3_log("[perf]   %5.1f%%  total of 200%% available\n", 100.0 * total / samples);
    uint32_t cnt = g_audio_count, late = g_audio_late; uint64_t ticks = g_audio_ticks, mx = g_audio_max;
    g_audio_count = 0; g_audio_ticks = 0; g_audio_max = 0; g_audio_late = 0;
    if (cnt) {
        double ms = 1000.0 * (double)ticks / (double)g_tb_freq;
        uint32_t spu_cnt = g_audio_spu_count; uint64_t spu_ticks = g_audio_spu_ticks;
        g_audio_spu_count = 0; g_audio_spu_ticks = 0;
        if (spu_cnt)
            ps3_log("[perf]   audio microcode: %u tasks (%u on the SPU, %.2f ms each there), %.2f ms each including the hand-over "
                    "(longest %.2f ms, %u over 33 ms)\n", cnt, spu_cnt, 1000.0 * (double)spu_ticks / (double)g_tb_freq / spu_cnt,
                    ms / cnt, 1000.0 * (double)mx / (double)g_tb_freq, late);
        else
            ps3_log("[perf]   audio microcode: %u tasks, %.2f ms each (longest %.2f ms, %u over 33 ms), %.1f%% of one hardware thread\n",
                    cnt, ms / cnt, 1000.0 * (double)mx / (double)g_tb_freq, late, 100.0 * ms / interval_ms);
    }
    if (g_profiler_seen && g_func_total) {
        ps3_log("[perf]   top game functions (share of running game-code samples, address = function):\n");
        for (int rank = 0; rank < 30; rank++) {
            uint32_t best = 0; int bi = -1;
            for (int k = 0; k < HASH_SIZE; k++) {
                if (g_func_hits[k] > best) { best = g_func_hits[k]; bi = k; }
            }
            if (bi < 0) break;
            int ti = g_func_thread[bi];
            ps3_log("[perf]     %5.1f%%  0x%08lx  in %s\n", 100.0 * best / g_func_total,
                    (unsigned long)(uintptr_t)g_func_addr[bi], ti < MAX_THREADS ? g_threads[ti].name : "?");
            g_func_hits[bi] = 0;
        }
        memset(g_func_addr, 0, sizeof(g_func_addr));
        memset(g_func_thread, 0, sizeof(g_func_thread));
        memset(g_func_hits, 0, sizeof(g_func_hits));
        g_func_total = 0;
    }
}

static NOINSTR void sampler(void* arg) {
    (void)arg;
    t_reg_busy = 1;  // the sampler does not count itself
    const uint32_t interval_ms = 10000;
    uint32_t samples = 0;
    uint64_t start = __mftb();
    for (;;) {
        sysUsleep(1000);
        samples++;
        int n = g_thread_count < MAX_THREADS ? g_thread_count : MAX_THREADS;
        for (int i = 0; i < n; i++) {
            thread_rec* r = &g_threads[i];
            if (!r->registered || r->blocked) continue;
            r->active_samples++;
            int d = r->depth;
            if (d > 0) {
                g_profiler_seen = 1;
                void* fn = r->stack[(d <= STACK_DEPTH ? d : STACK_DEPTH) - 1];
                if (fn) { count_func(fn, i); g_func_total++; }
            }
        }
        uint64_t now = __mftb();
        if (now - start >= (uint64_t)interval_ms * (g_tb_freq / 1000)) {
            report(samples, interval_ms);
            samples = 0;
            start = now;
        }
    }
}

NOINSTR void ps3prof_start(void) {
    u64 f = sysGetTimebaseFrequency();
    if (f) g_tb_freq = f;
    sys_ppu_thread_t tid;
    static char name[] = "perf sampler";
    // Highest priority, so it samples on time even when the game keeps both hardware threads busy.
    sysThreadCreate(&tid, sampler, 0, 0, 64 * 1024, 0, name);
}
