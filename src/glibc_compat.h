/* glibc_compat.h - bind to old glibc symbol versions so the Linux module loads on older servers
   (Ubuntu 18.04+/Debian 10+), not just on the distro it was built on. Force-included by build.sh. */
#if defined(__linux__)
#if defined(__x86_64__)
__asm__(".symver pow,pow@GLIBC_2.2.5");
__asm__(".symver exp,exp@GLIBC_2.2.5");
__asm__(".symver log,log@GLIBC_2.2.5");
__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dladdr,dladdr@GLIBC_2.2.5");
__asm__(".symver dlclose,dlclose@GLIBC_2.2.5");
#else
__asm__(".symver pow,pow@GLIBC_2.0");
__asm__(".symver exp,exp@GLIBC_2.0");
__asm__(".symver log,log@GLIBC_2.0");
__asm__(".symver dlopen,dlopen@GLIBC_2.1");
__asm__(".symver dlsym,dlsym@GLIBC_2.0");
__asm__(".symver dladdr,dladdr@GLIBC_2.0");
__asm__(".symver dlclose,dlclose@GLIBC_2.0");
#endif
#endif
