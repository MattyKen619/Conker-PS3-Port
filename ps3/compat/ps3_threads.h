// ps3_threads.h - std::thread / std::mutex / std::condition_variable stand-ins for the PS3.
//
// The PS3 toolchain's libstdc++ is built without gthreads, so std::thread, std::mutex and
// std::condition_variable are unavailable. These classes provide the same interface on top of
// lv2 kernel primitives (sys_ppu_thread, sys_mutex, sys_cond) exposed by PSL1GHT.
// std::lock_guard / std::unique_lock / std::scoped_lock work with them unchanged.
#pragma once

#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/systime.h>
#include <lv2/thread.h>
#include <ppu-types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>   // std::unique_lock, std::lock_guard (templates, no gthreads needed)
#include <tuple>
#include <utility>

// Profiler hook (ps3/host/perf.c): marks the calling thread as blocked while it waits. Weak, so
// programs without the profiler (tests/systest) don't need it.
extern "C" void ps3prof_block(int blocked) __attribute__((weak));
extern "C" void ps3prof_wait_error(const char* where, int code) __attribute__((weak));
extern "C" void ps3prof_thread_exit(void) __attribute__((weak));

namespace ps3compat {

struct blocking_scope {
    blocking_scope() { if (ps3prof_block) ps3prof_block(1); }
    ~blocking_scope() { if (ps3prof_block) ps3prof_block(0); }
};

[[noreturn]] inline void fatal(const char* what) {
    (void)what;
    std::abort();
}

// ---------------------------------------------------------------- mutexes
class mutex {
public:
    using native_handle_type = sys_mutex_t;
    mutex() { create(SYS_MUTEX_ATTR_NOT_RECURSIVE); }
    ~mutex() { sysMutexDestroy(handle_); }
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock() { if (sysMutexLock(handle_, 0) != 0) fatal("mutex lock"); }
    bool try_lock() { return sysMutexTryLock(handle_) == 0; }
    void unlock() { sysMutexUnlock(handle_); }
    native_handle_type native_handle() const { return handle_; }

protected:
    explicit mutex(u32 recursive) { create(recursive); }
    void create(u32 recursive) {
        sys_mutex_attr_t attr;
        sysMutexAttrInitialize(attr);
        attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
        attr.attr_recursive = recursive;
        if (sysMutexCreate(&handle_, &attr) != 0) fatal("mutex create");
    }
    sys_mutex_t handle_{};
};

class recursive_mutex : public mutex {
public:
    recursive_mutex() : mutex(SYS_MUTEX_ATTR_RECURSIVE) {}
};

// ------------------------------------------------------- condition variable
// lv2 condition variables are bound to one mutex when created, so this one binds lazily to the
// mutex of its first wait(). Every later wait must use the same mutex (true for all uses in
// N64ModernRuntime). notify_* before any wait has no waiters to wake, so it is a no-op.
enum class cv_status { no_timeout, timeout };

class condition_variable {
public:
    condition_variable() = default;
    ~condition_variable() { if (bound_) sysCondDestroy(cond_); }
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    void notify_one() { if (bound_) sysCondSignal(cond_); }
    void notify_all() { if (bound_) sysCondBroadcast(cond_); }

    void wait(std::unique_lock<mutex>& lk) { wait_usec(lk, 0); }

    template <class Pred>
    void wait(std::unique_lock<mutex>& lk, Pred pred) {
        while (!pred()) wait(lk);
    }

    template <class Rep, class Period>
    cv_status wait_for(std::unique_lock<mutex>& lk, const std::chrono::duration<Rep, Period>& d) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
        return wait_usec(lk, us <= 0 ? 1 : static_cast<u64>(us));
    }

    template <class Rep, class Period, class Pred>
    bool wait_for(std::unique_lock<mutex>& lk, const std::chrono::duration<Rep, Period>& d, Pred pred) {
        auto end = std::chrono::steady_clock::now() + d;
        while (!pred()) {
            auto left = end - std::chrono::steady_clock::now();
            if (left <= decltype(left)::zero()) return pred();
            wait_for(lk, left);
        }
        return true;
    }

    template <class Clock, class Duration, class Pred>
    bool wait_until(std::unique_lock<mutex>& lk, const std::chrono::time_point<Clock, Duration>& t, Pred pred) {
        return wait_for(lk, t - Clock::now(), pred);
    }

private:
    cv_status wait_usec(std::unique_lock<mutex>& lk, u64 usec) {
        bind(*lk.mutex());
        // lv2 returns ETIMEDOUT (0x8001000B) on timeout.
        s32 r;
        {
            blocking_scope blocked;
            r = sysCondWait(cond_, usec);
        }
        // 0x8001000B: ETIMEDOUT, the normal result of a timed wait.
        if (r != 0 && r != (s32)0x8001000B && ps3prof_wait_error) ps3prof_wait_error("sysCondWait", r);
        return r == 0 ? cv_status::no_timeout : cv_status::timeout;
    }
    void bind(mutex& m) {
        if (bound_) {
            if (bound_mutex_ != m.native_handle()) fatal("condition_variable used with two mutexes");
            return;
        }
        sys_cond_attr_t attr;
        sysCondAttrInitialize(attr);
        if (sysCondCreate(&cond_, m.native_handle(), &attr) != 0) fatal("cond create");
        bound_mutex_ = m.native_handle();
        bound_ = true;
    }
    sys_cond_t cond_{};
    sys_mutex_t bound_mutex_{};
    bool bound_ = false;
};

// ---------------------------------------------------------------- threads
class thread {
public:
    // Defaults for new threads. N64ModernRuntime runs game threads with deep call stacks.
    static inline u64 default_stack_size = 1024 * 1024;
    static inline s32 default_priority = 1000;  // lv2: 0 = highest, 3071 = lowest

    class id {
    public:
        id() = default;
        explicit id(sys_ppu_thread_t t) : t_(t) {}
        bool operator==(const id& o) const { return t_ == o.t_; }
        bool operator!=(const id& o) const { return t_ != o.t_; }
        bool operator<(const id& o) const { return t_ < o.t_; }
        sys_ppu_thread_t value() const { return t_; }
    private:
        sys_ppu_thread_t t_ = 0;
    };

    thread() = default;

    template <class F, class... Args>
    explicit thread(F&& f, Args&&... args) {
        auto fn = std::make_unique<std::function<void()>>(
            [f = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                std::apply(f, std::move(tup));
            });
        static char name[] = "ps3compat";
        if (sysThreadCreate(&tid_, &thread::entry, fn.get(), default_priority, default_stack_size,
                            THREAD_JOINABLE, name) != 0)
            fatal("thread create");
        fn.release();  // owned by the new thread now
        joinable_ = true;
    }

    ~thread() { if (joinable_) std::terminate(); }
    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;
    thread(thread&& o) noexcept : tid_(o.tid_), joinable_(o.joinable_) { o.joinable_ = false; }
    thread& operator=(thread&& o) noexcept {
        if (joinable_) std::terminate();
        tid_ = o.tid_; joinable_ = o.joinable_; o.joinable_ = false;
        return *this;
    }

    bool joinable() const { return joinable_; }
    void join() { u64 rv; sysThreadJoin(tid_, &rv); joinable_ = false; }
    void detach() { sysThreadDetach(tid_); joinable_ = false; }
    id get_id() const { return joinable_ ? id(tid_) : id(); }
    sys_ppu_thread_t native_handle() const { return tid_; }

    static unsigned hardware_concurrency() { return 2; }  // PPU: 1 core, 2 hardware threads

private:
    static void entry(void* arg) {
        std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
        try {
            (*fn)();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "\n[ps3compat] unhandled exception in a thread: %s\n", e.what());
            std::abort();
        } catch (...) {
            std::fprintf(stderr, "\n[ps3compat] unhandled non-standard exception in a thread\n");
            std::abort();
        }
        fn.reset();
        if (ps3prof_thread_exit) ps3prof_thread_exit();  // stop counting this thread in the time budget
        sysThreadExit(0);
    }
    sys_ppu_thread_t tid_ = 0;
    bool joinable_ = false;
};

// ----------------------------------------------------------- this_thread
namespace this_thread {
inline thread::id get_id() { sys_ppu_thread_t t; sysThreadGetId(&t); return thread::id(t); }
inline void yield() { sysThreadYield(); }
template <class Rep, class Period>
void sleep_for(const std::chrono::duration<Rep, Period>& d) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    if (us > 0) {
        blocking_scope blocked;
        sysUsleep(static_cast<u64>(us));
    }
}
template <class Clock, class Duration>
void sleep_until(const std::chrono::time_point<Clock, Duration>& t) { sleep_for(t - Clock::now()); }
}  // namespace this_thread

// ------------------------------------------------------- counting semaphore
template <std::ptrdiff_t Max = 0x7fffffff>
class counting_semaphore {
public:
    explicit counting_semaphore(std::ptrdiff_t initial) : count_(initial) {}
    void release(std::ptrdiff_t n = 1) {
        std::unique_lock<mutex> lk(m_);
        count_ += n;
        for (std::ptrdiff_t i = 0; i < n; i++) cv_.notify_one();
    }
    void acquire() {
        std::unique_lock<mutex> lk(m_);
        cv_.wait(lk, [&] { return count_ > 0; });
        --count_;
    }
    bool try_acquire() {
        std::unique_lock<mutex> lk(m_);
        if (count_ <= 0) return false;
        --count_;
        return true;
    }
    template <class Rep, class Period>
    bool try_acquire_for(const std::chrono::duration<Rep, Period>& d) {
        std::unique_lock<mutex> lk(m_);
        if (!cv_.wait_for(lk, d, [&] { return count_ > 0; })) return false;
        --count_;
        return true;
    }
private:
    mutex m_;
    condition_variable cv_;
    std::ptrdiff_t count_;
};
using binary_semaphore = counting_semaphore<1>;

}  // namespace ps3compat
