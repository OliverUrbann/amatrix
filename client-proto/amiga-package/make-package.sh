#!/bin/bash
# Builds amatrix-step2b.lha for the Amiga: binary, start script, readme (Latin-1).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
"$here/../build-amiga.sh" >/dev/null
tmp=$(mktemp -d); mkdir "$tmp/amatrix"
cp "$here/../build-amiga/amatrix-recv" "$here/../build-amiga/amxdiag" "$here/amatrix-test" "$here/amxdiag-run" "$tmp/amatrix/"
iconv -f utf-8 -t latin1 "$here/LiesMich.utf8.txt" > "$tmp/amatrix/LiesMich.txt"
rm -f "$here/../build-amiga/amatrix-step2b.lha"
(cd "$tmp" && /opt/amiga/bin/lha aq "$here/../build-amiga/amatrix-step2b.lha" amatrix >/dev/null)
rm -rf "$tmp"; ls -la "$here/../build-amiga/amatrix-step2b.lha"
