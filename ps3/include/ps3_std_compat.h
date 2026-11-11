// ps3_std_compat.h - force-included (-include) into every C++ file of the runtime.
//
// The PS3 toolchain's libstdc++ is built without gthreads. It still declares a non-functional
// std::thread and std::this_thread, but no std::mutex / std::condition_variable. This header
// suppresses those declarations and puts the working ps3compat versions (lv2 kernel primitives)
// in their place under the standard names.
//
// It also exposes C99 functions that newlib declares but that libstdc++ was configured not to
// import into namespace std (std::snprintf, std::strtoull, ...).
#pragma once
#ifdef __cplusplus

// Keep libstdc++ from declaring its own (unusable) std::thread and std::this_thread.
#define _GLIBCXX_THREAD_H 1
#define _GLIBCXX_THIS_THREAD_SLEEP_H 1
#define _GLIBCXX_THREAD 1

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>
#include "../compat/ps3_threads.h"

namespace std {
using ps3compat::thread;
using ps3compat::mutex;
using ps3compat::recursive_mutex;
using ps3compat::condition_variable;
using ps3compat::cv_status;
using ps3compat::counting_semaphore;
using ps3compat::binary_semaphore;
namespace this_thread {
using ps3compat::this_thread::get_id;
using ps3compat::this_thread::yield;
using ps3compat::this_thread::sleep_for;
using ps3compat::this_thread::sleep_until;
}

template <>
struct hash<ps3compat::thread::id> {
    size_t operator()(const ps3compat::thread::id& i) const noexcept { return (size_t)i.value(); }
};

// C99 functions newlib provides but libstdc++ did not import.
using ::snprintf;
using ::vsnprintf;
using ::vscanf;
using ::vfscanf;
using ::vsscanf;
using ::strtoll;
using ::strtoull;
using ::strtof;
using ::strtold;
using ::llabs;
using ::lldiv;
using ::lldiv_t;
using ::_Exit;
[[noreturn]] inline void quick_exit(int code) noexcept { ::_Exit(code); }
}

#endif
