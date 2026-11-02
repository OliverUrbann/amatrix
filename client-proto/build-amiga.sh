#!/bin/bash
# AmigaOS build of amatrix-recv: m68k-amigaos-gcc + libnix, 68020 + FPU,
# bsdsocket.library for the network, AmiSSL 5 (OpenSSL 3) for https and as
# the random source for keys.  Needs AmiSSL 5.x installed on the Amiga.
#   ./build-amiga.sh  -> build-amiga/amatrix-recv
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$here/..
export PATH=/opt/amiga/bin:$PATH
[ -f "$top/libolm-amiga/out/amiga/libolm.a" ] || "$top/libolm-amiga/build.sh" amiga >/dev/null
mkdir -p "$here/build-amiga"
m68k-amigaos-gcc -mcrt=nix20 -m68020 -m68881 -O2 -Wall -std=gnu99 \
  -I"$top/third_party/cjson" -I"$top/third_party/libolm/include" -DOLM_STATIC_DEFINE \
  -DAMX_AMISSL -I"$top/third_party/amissl-sdk/include" \
  "$here/src/recv.c" "$here/src/canonjson.c" "$here/src/http_sock.c" "$here/src/net.c" \
  "$here/src/platform_amiga.c" "$top/third_party/cjson/cJSON.c" \
  "$top/libolm-amiga/out/amiga/libolm.a" -L"$top/third_party/amissl-sdk/lib/AmigaOS3" -lamisslstubs -lstdc++ -lm -o "$here/build-amiga/amatrix-recv"
m68k-amigaos-strip "$here/build-amiga/amatrix-recv"
# amxdiag: step-by-step check of bsdsocket, AmiSSL, TLS and file handling
m68k-amigaos-gcc -mcrt=nix20 -m68020 -m68881 -O2 -Wall -std=gnu99 \
  -I"$top/third_party/libolm/include" -DOLM_STATIC_DEFINE \
  -DAMX_AMISSL -I"$top/third_party/amissl-sdk/include" \
  "$here/src/amxdiag.c" "$here/src/http_sock.c" "$here/src/net.c" "$here/src/platform_amiga.c" \
  "$top/libolm-amiga/out/amiga/libolm.a" \
  -L"$top/third_party/amissl-sdk/lib/AmigaOS3" -lamisslstubs -lstdc++ -lm -o "$here/build-amiga/amxdiag"
m68k-amigaos-strip "$here/build-amiga/amxdiag"
ls -la "$here/build-amiga/amatrix-recv" "$here/build-amiga/amxdiag"
