#!/usr/bin/env bash
# Builds the test program + fake engine for a target and runs it against bin/gmsv_zradio_<target>.dll.
# Windows targets run under Wine. Run ./build.sh first.
# Usage: tests/run_tests.sh [linux|linux64|win32|win64 ...]   (default: all)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
TARGETS=${*:-"linux64 linux win64 win32"}
INC="-I$ROOT/src -I$ROOT/include -I$ROOT/third_party/opus/include -I$ROOT/tests"
fail=0
for t in $TARGETS; do
  O=build/test_$t; rm -rf $O; mkdir -p $O
  case $t in
    linux|linux64)
      m=64; ob=build/linux64; [ $t = linux ] && { m=32; ob=build/linux32; }
      g++ -m$m -O0 -fPIC -shared -std=c++11 $INC tests/fake_engine.cpp -o $O/engine.so
      g++ -m$m -O1 -std=c++14 -fno-exceptions -w $INC tests/test_module.cpp $ob/opus_*.o -ldl -lm -o $O/test_module
      cp bin/gmsv_zradio_$t.dll $O/
      echo "=== $t"
      (cd $O && ./test_module ./gmsv_zradio_$t.dll) || fail=1
      ;;
    win32|win64)
      A=x86_64; M=x64; DT=i386:x86-64; SEH=""; [ $t = win32 ] && { A=i686; M=x86; DT=i386; SEH="/safeseh:no"; }
      MINGW=/usr/$A-w64-mingw32
      { cat src/msvcrt.def; printf "printf\nstrcmp\nstrlen\nexit\n__getmainargs\n"; [ $A = i686 ] && tail -n +3 src/msvcrt_x86_extra.def; true; } > $O/msvcrt.def
      llvm-dlltool -m $DT -d $O/msvcrt.def -l $O/msvcrt.lib
      CXX="clang++ --target=$A-pc-windows-msvc -std=c++14 -O1 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
           -fno-stack-protector -mstack-probe-size=2147483647 -D_WIN32 -I$ROOT/include/freestanding $INC -w"
      $CXX -c tests/fake_engine.cpp -o $O/fake_engine.obj
      lld-link /nologo $SEH /dll /noentry /nodefaultlib /machine:$M /out:$O/engine.dll $O/fake_engine.obj $O/msvcrt.lib $MINGW/lib/libkernel32.a
      $CXX -c tests/test_module.cpp -o $O/test_module.obj
      $CXX -c src/crt_win.cpp -o $O/crt.obj
      lld-link /nologo $SEH /subsystem:console /nodefaultlib /machine:$M /entry:mainCRTStartup /out:$O/test_module.exe \
        $O/test_module.obj $O/crt.obj build/win_$A/opus_*.obj $O/msvcrt.lib $MINGW/lib/libkernel32.a
      cp bin/gmsv_zradio_$t.dll $O/
      echo "=== $t (wine)"
      WINE=wine; command -v wine >/dev/null || WINE=wine64
      command -v $WINE >/dev/null || { echo "error: wine not found"; fail=1; continue; }
      (cd $O && WINEDEBUG=-all $WINE test_module.exe gmsv_zradio_$t.dll) || { echo "error: $t test exited with $?"; fail=1; }
      ;;
    *) echo "unknown target $t"; exit 2 ;;
  esac
done
exit $fail
