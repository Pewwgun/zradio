// crt_win.cpp - the few runtime symbols msvcrt.dll does not provide.
// The module links against the system msvcrt.dll (present on every Windows install) instead of
// a Visual C++ runtime, so nothing extra has to be installed on the server.
#if defined(_WIN32)
extern "C" {
	int _fltused = 0;                      // the MSVC code model references this whenever floats are used

	__declspec(dllimport) double __cdecl pow(double, double);
	__declspec(dllimport) double __cdecl floor(double);
	__declspec(dllimport) double __cdecl sqrt(double);

	// C99 functions Opus uses that the classic msvcrt.dll lacks
	double __cdecl exp2(double x) { return pow(2.0, x); }
	float __cdecl floorf(float x) { return (float)floor(x); }
	float __cdecl sqrtf(float x) { return (float)sqrt(x); }

#if !defined(_WIN64)
	// libgcc helper referenced by GNU-target objects on 32-bit; compiled here with the MSVC
	// target, so the division itself becomes msvcrt's _alldiv
	long long __divdi3(long long a, long long b) { return a / b; }
	unsigned long long __udivdi3(unsigned long long a, unsigned long long b) { return a / b; }
	long long __moddi3(long long a, long long b) { return a % b; }
	unsigned long long __umoddi3(unsigned long long a, unsigned long long b) { return a % b; }
#endif
}
#endif
