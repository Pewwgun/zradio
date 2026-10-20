// platform.cpp - module lookup, scanning ranges, time, pinning. Windows (freestanding) and Linux.
#include "engine.h"

#if defined(_WIN32)
extern "C" {
	__declspec(dllimport) void* __stdcall GetModuleHandleA(const char*);
	__declspec(dllimport) void* __stdcall GetProcAddress(void*, const char*);
	__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
	__declspec(dllimport) int __stdcall GetModuleHandleExA(unsigned long flags, const char* name, void** out);
	__declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
	__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long*);
	__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long*);
}

namespace plat {
	bool find_engine(Module& m) {
		unsigned char* b = (unsigned char*)GetModuleHandleA("engine.dll");
		if (!b) return false;
		m.base = b; m.handle = b;
		uint32_t pe = *(uint32_t*)(b + 0x3C);
		m.size = *(uint32_t*)(b + pe + 24 + 56);     // OptionalHeader.SizeOfImage (same offset for PE32 / PE32+)
		rt_memcpy(m.path, "engine.dll", 11);
		return true;
	}
	void* get_proc(Module& m, const char* name) { return GetProcAddress(m.handle, name); }
	int exec_ranges(Module& m, Range* out, int max) {
		unsigned char* b = m.base;
		uint32_t pe = *(uint32_t*)(b + 0x3C);
		uint16_t nsec = *(uint16_t*)(b + pe + 6);
		uint16_t optsz = *(uint16_t*)(b + pe + 20);
		unsigned char* sec = b + pe + 24 + optsz;
		int n = 0;
		for (int i = 0; i < nsec && n < max; i++, sec += 40) {
			uint32_t vsize = *(uint32_t*)(sec + 8), va = *(uint32_t*)(sec + 12), ch = *(uint32_t*)(sec + 36);
			if (ch & 0x20000000) { out[n].start = b + va; out[n].len = vsize; n++; }
		}
		return n;
	}
	double now() {
		static long long f = 0;
		if (!f) QueryPerformanceFrequency(&f);
		long long c; QueryPerformanceCounter(&c);
		return (double)c / (double)f;
	}
	void pin_self() {
		void* h = 0;
		// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN
		GetModuleHandleExA(0x4 | 0x1, (const char*)(void*)&pin_self, &h);
	}
	void log(const char* msg) { OutputDebugStringA(msg); }
}

#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <link.h>
#include <time.h>
#include <string.h>

namespace plat {
	struct FindCtx { Module* m; bool found; };
	static bool ends_with(const char* s, const char* suf) {
		size_t a = strlen(s), b = strlen(suf);
		return a >= b && strcmp(s + a - b, suf) == 0;
	}
	static int cb(struct dl_phdr_info* info, size_t, void* data) {
		FindCtx* c = (FindCtx*)data;
		const char* n = info->dlpi_name ? info->dlpi_name : "";
		if (ends_with(n, "/engine_srv.so") || ends_with(n, "/engine_client.so") || ends_with(n, "/engine.so")) {
			c->m->base = (unsigned char*)info->dlpi_addr;
			strncpy(c->m->path, n, sizeof(c->m->path) - 1);
			c->found = true;
			return 1;
		}
		return 0;
	}
	bool find_engine(Module& m) {
		memset(&m, 0, sizeof(m));
		FindCtx c = { &m, false };
		dl_iterate_phdr(cb, &c);
		if (!c.found) return false;
		m.handle = dlopen(m.path, RTLD_LAZY | RTLD_NOLOAD);
		return m.handle != 0;
	}
	void* get_proc(Module& m, const char* name) { return dlsym(m.handle, name); }
	struct RangeCtx { Module* m; Range* out; int max; int n; };
	static int cb2(struct dl_phdr_info* info, size_t, void* data) {
		RangeCtx* c = (RangeCtx*)data;
		if (!info->dlpi_name || strcmp(info->dlpi_name, c->m->path) != 0) return 0;
		for (int i = 0; i < info->dlpi_phnum && c->n < c->max; i++) {
			const ElfW(Phdr)& ph = info->dlpi_phdr[i];
			if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
				c->out[c->n].start = (unsigned char*)(info->dlpi_addr + ph.p_vaddr);
				c->out[c->n].len = ph.p_memsz;
				c->n++;
			}
		}
		return 1;
	}
	int exec_ranges(Module& m, Range* out, int max) {
		RangeCtx c = { &m, out, max, 0 };
		dl_iterate_phdr(cb2, &c);
		return c.n;
	}
	double now() {
		struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
		return ts.tv_sec + ts.tv_nsec * 1e-9;
	}
	void pin_self() {
		Dl_info di;
		if (dladdr((void*)&pin_self, &di) && di.dli_fname) dlopen(di.dli_fname, RTLD_NOW | RTLD_NODELETE);
	}
	void log(const char*) {}
}
#endif
