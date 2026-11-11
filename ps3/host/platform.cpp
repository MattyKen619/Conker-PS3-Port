// platform.cpp - the per-platform pieces of ultramodern for the PS3.
#include <string>
#include <sys/thread.h>
#include <lv2/thread.h>
#include "ultramodern/ultramodern.hpp"

extern "C" void ps3prof_set_name(const char* name);

void ultramodern::set_native_thread_name(const std::string& name) {
    ps3prof_set_name(name.c_str());
    sys_ppu_thread_t id;
    sysThreadGetId(&id);
    sysThreadRename(id, name.c_str());
}

// lv2 priorities: 0 is the highest, 3071 the lowest. ps3compat::thread starts threads at 1000.
void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    s32 prio = 1000;
    switch (pri) {
        case ThreadPriority::Low:      prio = 1500; break;
        case ThreadPriority::Normal:   prio = 1000; break;
        case ThreadPriority::High:     prio = 800;  break;
        case ThreadPriority::VeryHigh: prio = 600;  break;
        case ThreadPriority::Critical: prio = 400;  break;
    }
    sys_ppu_thread_t id;
    sysThreadGetId(&id);
    sysThreadSetPriority(id, prio);
}
