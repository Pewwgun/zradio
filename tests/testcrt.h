// testcrt.h - tiny platform layer for the test programs (no C++ standard library on Windows).
#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
extern "C" {
 __declspec(dllimport) int __cdecl printf(const char*, ...);
 __declspec(dllimport) void* __cdecl malloc(size_t);
 __declspec(dllimport) void __cdecl free(void*);
 __declspec(dllimport) int __cdecl strcmp(const char*, const char*);
 __declspec(dllimport) size_t __cdecl strlen(const char*);
 __declspec(dllimport) void __cdecl exit(int);
 __declspec(dllimport) double __cdecl sin(double);
 __declspec(dllimport) void* __stdcall LoadLibraryA(const char*);
 __declspec(dllimport) void* __stdcall GetProcAddress(void*, const char*);
 __declspec(dllimport) void __stdcall Sleep(unsigned long);
 __declspec(dllimport) unsigned long __stdcall GetLastError(void);
 void* __cdecl memcpy(void*, const void*, size_t);
 void* __cdecl memset(void*, int, size_t);
}
inline void* operator new(size_t, void* p) noexcept { return p; }
inline void operator delete(void* p) noexcept { free(p); }
inline void operator delete(void* p, size_t) noexcept { free(p); }
#define TEST_EXPORT extern "C" __declspec(dllexport)
#define TEST_NOINLINE __declspec(noinline)
#define ENGINE_LIB "engine.dll"
static inline void* tc_load(const char* n) { return LoadLibraryA(n); }
static inline void* tc_sym(void* h, const char* n) { return GetProcAddress(h, n); }
static inline void tc_sleep(unsigned ms) { Sleep(ms); }
static inline void tc_err(const char* what) { printf("%s failed (error %lu)\n", what, GetLastError()); }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <unistd.h>
#include <new>
#define TEST_EXPORT extern "C" __attribute__((visibility("default")))
#define TEST_NOINLINE __attribute__((noinline))
#define ENGINE_LIB "./engine.so"
static inline void* tc_load(const char* n) { return dlopen(n, RTLD_NOW | RTLD_GLOBAL); }
static inline void* tc_sym(void* h, const char* n) { return dlsym(h, n); }
static inline void tc_sleep(unsigned ms) { usleep(ms * 1000); }
static inline void tc_err(const char* what) { printf("%s failed: %s\n", what, dlerror()); }
#endif
