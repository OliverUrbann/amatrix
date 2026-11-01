#!/bin/bash
# Build libolm (from the python-olm sdist) and its unit tests.
#   build.sh amiga   -> m68k-amigaos, libnix, 68020   (out/amiga)
#   build.sh native  -> host gcc, for reference          (out/native)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
target=${1:-amiga}
OLM=${OLM:-$here/../third_party/libolm}
out=$here/out/$target
mkdir -p "$out/obj" "$out/bin"

case $target in
  amiga)
    CC="m68k-amigaos-gcc"; CXX="m68k-amigaos-g++"
    ARCH="-mcrt=nix20 -m68020 -O2 -fomit-frame-pointer"
    STRIP="m68k-amigaos-strip"
    TESTFLAGS="-DDOCTEST_CONFIG_NO_POSIX_SIGNALS -include $here/compat/doctest_amiga.h"
    TESTLIBS="$out/obj/compat_atomic.o"
    mkdir -p "$out/obj"; $CC $ARCH -c "$here/compat/atomic.c" -o "$out/obj/compat_atomic.o" ;;
  native)
    CC=gcc; CXX=g++; ARCH="-O2"; STRIP=strip; TESTFLAGS=""; TESTLIBS="" ;;
  *) echo "unknown target $target"; exit 1 ;;
esac

CPPFLAGS="-I$OLM/include -I$OLM/lib -I$OLM/tests/include -DOLM_STATIC_DEFINE=1 -DNDEBUG -DOLMLIB_VERSION_MAJOR=3 -DOLMLIB_VERSION_MINOR=2 -DOLMLIB_VERSION_PATCH=16"
CFLAGS="$ARCH -std=c99 -Wall"
CXXFLAGS="$ARCH -std=c++11 -Wall"

SOURCES="$(ls $OLM/src/*.c $OLM/src/*.cpp) \
  $OLM/lib/crypto-algorithms/sha256.c \
  $OLM/lib/crypto-algorithms/aes.c \
  $OLM/lib/curve25519-donna/curve25519-donna.c"

objs=""
for s in $SOURCES; do
  o=$out/obj/$(basename "${s%.*}").o
  case $s in
    *.c)   $CC  $CPPFLAGS $CFLAGS   -c "$s" -o "$o" ;;
    *.cpp) $CXX $CPPFLAGS $CXXFLAGS -c "$s" -o "$o" ;;
  esac
  objs="$objs $o"
done
rm -f "$out/libolm.a"
${CC%gcc}ar rcs "$out/libolm.a" $objs 2>/dev/null || ar rcs "$out/libolm.a" $objs
echo "libolm.a: $(stat -c %s "$out/libolm.a") bytes"

# unit tests (one binary per file, each has its own doctest main)
for t in $OLM/tests/test_*.cpp $here/src/*.cpp; do
  [ -f "$t" ] || continue
  n=$(basename "${t%.*}")
  $CXX $CPPFLAGS $CXXFLAGS $TESTFLAGS -I$here/src "$t" "$out/libolm.a" $TESTLIBS -o "$out/bin/$n"
  $STRIP "$out/bin/$n" 2>/dev/null || true
done
ls -la "$out/bin"
