#!/usr/bin/env bash
# Builds gmsv_zradio for all four Garry's Mod server targets:
#   gmsv_zradio_win32.dll, gmsv_zradio_win64.dll   (clang targeting the MSVC ABI + lld-link)
#   gmsv_zradio_linux.dll, gmsv_zradio_linux64.dll (gcc/g++ with multilib)
#
# Requirements (Debian/Ubuntu): clang lld llvm g++-multilib mingw-w64
#   (mingw-w64 is only used for its Windows C headers and msvcrt/kernel32 import libraries;
#    the module itself uses the MSVC ABI, so its C++ vtables match the engine's)
#
# Usage: ./build.sh [win32|win64|linux|linux64 ...]   (default: all)
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
OUT=$ROOT/bin
mkdir -p "$OUT" build
TARGETS=${*:-"win32 win64 linux linux64"}

OPUS=$ROOT/third_party/opus
MH=$ROOT/third_party/minhook
OPUS_SRC=$(sed "s#^#$OPUS/#" third_party/opus_sources.txt)
OPUS_DEF="-DOPUS_BUILD -DNONTHREADSAFE_PSEUDOSTACK -DPACKAGE_VERSION=\"1.5-zradio\""
OPUS_INC="-I$OPUS/include -I$OPUS/celt -I$OPUS/silk -I$OPUS/silk/float -I$OPUS"
MOD_INC="-I$ROOT/src -I$ROOT/include -I$OPUS/include -I$MH/include"

build_linux() {   # $1 = 32|64, $2 = suffix
  local B=build/linux$1 objs=()
  mkdir -p $B
  local M="-m$1 -O2 -fPIC -fvisibility=hidden -fno-strict-aliasing -fno-stack-protector -U_FORTIFY_SOURCE -include $ROOT/src/glibc_compat.h"
  for f in $OPUS_SRC; do
    o=$B/opus_$(basename ${f%.c}).o
    [ -f $o ] || gcc $M -std=gnu99 $OPUS_DEF -DHAVE_LRINTF -DHAVE_LRINT $OPUS_INC -c $f -o $o
    objs+=($o)
  done
  local hde=hde64; [ $1 = 32 ] && hde=hde32
  for f in $MH/src/buffer.c $MH/src/hook.c $MH/src/trampoline.c $MH/src/hde/$hde.c; do
    o=$B/mh_$(basename ${f%.c}).o
    gcc $M -std=gnu99 -I$MH/include -c $f -o $o
    objs+=($o)
  done
  for f in audiofile dec_mp3 dec_ogg; do
    gcc $M -std=gnu99 -O2 -I$ROOT/third_party/decoders -w -c src/$f.c -o $B/$f.o
    objs+=($B/$f.o)
  done
  for f in src/zradio.cpp src/platform.cpp; do
    o=$B/$(basename ${f%.cpp}).o
    g++ $M -std=c++11 -fno-exceptions -fno-rtti -Wall -Wno-unused-function $MOD_INC -c $f -o $o
    objs+=($o)
  done
  g++ -m$1 -shared -o $OUT/gmsv_zradio_$2.dll "${objs[@]}" -static-libgcc -static-libstdc++ -lm -Wl,--no-as-needed -ldl -Wl,--no-undefined
  echo "built $OUT/gmsv_zradio_$2.dll"
}

build_win() {     # $1 = i686|x86_64, $2 = suffix
  local A=$1 B=build/win_$1 objs=()
  mkdir -p $B
  local TGT=$A-pc-windows-msvc
  local MINGW=/usr/$A-w64-mingw32
  # C dependencies (Opus, MinHook) are plain C: build them with the GNU target against MinGW's headers.
  # The C ABI is identical to MSVC's; only the C++ module needs the MSVC ABI for its vtables.
  local CF="--target=$A-w64-windows-gnu -O2 -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables -Wno-everything"
  for f in $OPUS_SRC; do
    o=$B/opus_$(basename ${f%.c}).obj
    [ -f $o ] || clang $CF $OPUS_DEF $OPUS_INC -c $f -o $o
    objs+=($o)
  done
  local hde=hde64; [ $A = i686 ] && hde=hde32
  # MinHook is plain C: build it against MinGW's headers with the GNU target (same C ABI)
  for f in $MH/src/buffer.c $MH/src/hook.c $MH/src/trampoline.c $MH/src/hde/$hde.c; do
    o=$B/mh_$(basename ${f%.c}).obj
    clang $CF -I$MH/include -c $f -o $o
    objs+=($o)
  done
  for f in audiofile dec_mp3 dec_ogg; do
    clang $CF -I$ROOT/third_party/decoders -c src/$f.c -o $B/$f.obj
    objs+=($B/$f.obj)
  done
  # the module: freestanding C++ (no C runtime headers), MSVC ABI
  for f in src/zradio.cpp src/platform.cpp src/crt_win.cpp; do
    o=$B/$(basename ${f%.cpp}).obj
    clang++ --target=$TGT -std=c++14 -O2 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector \
      -mstack-probe-size=2147483647 -Wall -Wno-unused-function \
      -D_WIN32 -I$ROOT/include/freestanding $MOD_INC -c $f -o $o
    objs+=($o)
  done
  local MACH=x64; [ $A = i686 ] && MACH=x86
  # import library for the system msvcrt.dll, generated from a .def file; kernel32 comes from MinGW
  local DT="-m i386:x86-64"; [ $A = i686 ] && DT="-m i386"
  local DEF=src/msvcrt.def
  if [ $A = i686 ]; then cat src/msvcrt.def <(tail -n +3 src/msvcrt_x86_extra.def) > $B/msvcrt.def; DEF=$B/msvcrt.def; fi
  llvm-dlltool $DT -d $DEF -l $B/msvcrt.lib
  local SEH=""; [ $A = i686 ] && SEH="/safeseh:no"
  lld-link /nologo /errorlimit:0 $SEH /dll /noentry /nodefaultlib /machine:$MACH /opt:ref /out:$OUT/gmsv_zradio_$2.dll \
    "${objs[@]}" $B/msvcrt.lib $MINGW/lib/libkernel32.a
  rm -f $OUT/gmsv_zradio_$2.lib $OUT/gmsv_zradio_$2.exp
  echo "built $OUT/gmsv_zradio_$2.dll"
}

for t in $TARGETS; do
  case $t in
    linux)   build_linux 32 linux ;;
    linux64) build_linux 64 linux64 ;;
    win32)   build_win i686 win32 ;;
    win64)   build_win x86_64 win64 ;;
    *) echo "unknown target $t"; exit 1 ;;
  esac
done
