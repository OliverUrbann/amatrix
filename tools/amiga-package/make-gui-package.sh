#!/bin/bash
# Builds build/amiga/amatrix-gui.lha for a real Amiga: the MUI client
# Amatrix with its icon and LiesMich.txt (ISO-8859-1).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
make -C "$top" -j4 TARGET=amiga gui >/dev/null
tmp=$(mktemp -d); mkdir "$tmp/Amatrix"
cp "$top/build/amiga/gui/Amatrix" "$tmp/Amatrix/"
python3 "$here/make-icon.py" "$tmp/Amatrix/Amatrix.info" 65536
mkdir -p "$tmp/Amatrix/Icons"
for c in "$top"/build/amiga/catalogs/*/amatrix.catalog; do          # languages (English is built in)
    l=$(basename "$(dirname "$c")"); mkdir -p "$tmp/Amatrix/Catalogs/$l"; cp "$c" "$tmp/Amatrix/Catalogs/$l/"
done
python3 "$here/make-icon.py" "$tmp/Amatrix/Icons/Amatrix-neu.info" 65536 new   # AppIcon: unread
iconv -f utf-8 -t latin1 "$here/LiesMich-GUI.utf8.txt" > "$tmp/Amatrix/LiesMich.txt"
out="$top/build/amiga/amatrix-gui.lha"
rm -f "$out"
(cd "$tmp" && /opt/amiga/bin/lha aq "$out" Amatrix >/dev/null)
rm -rf "$tmp"; ls -la "$out"
