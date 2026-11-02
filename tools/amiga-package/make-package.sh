#!/bin/bash
# Builds build/amiga/amatrix-test.lha for a real Amiga: amx-cli, amxdiag,
# the amatrix-test start script and LiesMich.txt (scripts/readme as ISO-8859-1).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
make -C "$top" -j2 TARGET=amiga build/amiga/tools/amx-cli build/amiga/tools/amxdiag >/dev/null
tmp=$(mktemp -d); mkdir "$tmp/amatrix"
cp "$top/build/amiga/tools/amx-cli" "$top/build/amiga/tools/amxdiag" "$tmp/amatrix/"
iconv -f utf-8 -t latin1 "$here/amatrix-test" > "$tmp/amatrix/amatrix-test"
iconv -f utf-8 -t latin1 "$here/LiesMich.utf8.txt" > "$tmp/amatrix/LiesMich.txt"
out="$top/build/amiga/amatrix-test.lha"
rm -f "$out"
(cd "$tmp" && /opt/amiga/bin/lha aq "$out" amatrix >/dev/null)
rm -rf "$tmp"; ls -la "$out"
