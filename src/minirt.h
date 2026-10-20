// minirt.h - the handful of C runtime functions the module needs.
// On Windows the module is built freestanding (clang, MSVC ABI) and these come straight from
// msvcrt.dll; on Linux they come from libc/libm.
#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
extern "C" {
	__declspec(dllimport) double __cdecl sin(double);
	__declspec(dllimport) double __cdecl cos(double);
	__declspec(dllimport) double __cdecl sqrt(double);
	__declspec(dllimport) double __cdecl log(double);
	__declspec(dllimport) double __cdecl log10(double);
	__declspec(dllimport) double __cdecl pow(double, double);
	__declspec(dllimport) double __cdecl atan2(double, double);
	__declspec(dllimport) double __cdecl tanh(double);
	__declspec(dllimport) void* __cdecl malloc(size_t);
	__declspec(dllimport) void* __cdecl calloc(size_t, size_t);
	__declspec(dllimport) void __cdecl free(void*);
	void* __cdecl memset(void*, int, size_t);
	void* __cdecl memcpy(void*, const void*, size_t);
	int __cdecl memcmp(const void*, const void*, size_t);
	__declspec(dllimport) int __cdecl _snprintf(char*, size_t, const char*, ...);
}
#define rt_snprintf _snprintf
#else
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define rt_snprintf snprintf
#endif

static inline double rt_sin(double x) { return sin(x); }
static inline double rt_cos(double x) { return cos(x); }
static inline double rt_sqrt(double x) { return sqrt(x); }
static inline double rt_log(double x) { return log(x); }
static inline double rt_log10(double x) { return log10(x); }
static inline double rt_pow(double x, double y) { return pow(x, y); }
static inline double rt_atan2(double y, double x) { return atan2(y, x); }
static inline double rt_tanh(double x) { return tanh(x); }
static inline void* rt_memset(void* d, int v, size_t n) { return memset(d, v, n); }
static inline void* rt_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
static inline void* rt_calloc(size_t n, size_t s) { return calloc(n, s); }
static inline void rt_free(void* p) { free(p); }
