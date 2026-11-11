// dlfcn.h - stand-in for the PS3, which has no dynamic loader for native libraries.
// dlopen() always fails, so native mod libraries are reported as unavailable.
#pragma once
#define RTLD_LAZY  1
#define RTLD_NOW   2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#ifdef __cplusplus
extern "C" {
#endif
static inline void* dlopen(const char* path, int mode) { (void)path; (void)mode; return 0; }
static inline void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return 0; }
static inline int dlclose(void* handle) { (void)handle; return 0; }
static inline const char* dlerror(void) { return "dynamic libraries are not supported on PS3"; }
#ifdef __cplusplus
}
#endif
