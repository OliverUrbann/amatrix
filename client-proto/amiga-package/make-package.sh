#!/bin/bash
# Builds amatrix-test3.lha for the Amiga: binary, start script, readme (Latin-1).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
"$here/../build-amiga.sh" >/dev/null
tmp=$(mktemp -d); mkdir "$tmp/amatrix"
cp "$here/../build-amiga/amatrix-recv" "$here/../build-amiga/amxdiag" "$tmp/amatrix/"
# scripts are UTF-8 here, the Amiga shell wants ISO-8859-1
for f in amatrix-test amxdiag-run; do iconv -f utf-8 -t latin1 "$here/$f" > "$tmp/amatrix/$f"; done
iconv -f utf-8 -t latin1 "$here/LiesMich.utf8.txt" > "$tmp/amatrix/LiesMich.txt"
rm -f "$here/../build-amiga/amatrix-test3.lha"
(cd "$tmp" && /opt/amiga/bin/lha aq "$here/../build-amiga/amatrix-test3.lha" amatrix >/dev/null)
rm -rf "$tmp"; ls -la "$here/../build-amiga/amatrix-test3.lha"
