#!/bin/bash
# Builds the Aminet upload pair build/amiga/Amatrix.lha + Amatrix.readme:
# the MUI client Amatrix with its icon, catalogs, ReadMe.txt (English),
# LiesMich.txt (German) and Amatrix.readme (Aminet header), all ISO-8859-1.
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
iconv -f utf-8 -t latin1 "$here/ReadMe-GUI.utf8.txt" > "$tmp/Amatrix/ReadMe.txt"
iconv -f utf-8 -t latin1 "$here/Amatrix.readme.txt" > "$tmp/Amatrix/Amatrix.readme"
# Aminet: Short <= 40 characters, lines <= 78
short=$(sed -n 's/^Short: *//p' "$here/Amatrix.readme.txt")
[ ${#short} -le 40 ] || { echo "Short is ${#short} characters (max 40)"; exit 1; }
awk 'length>78{print FILENAME": line "NR" too long"; bad=1} END{exit bad}' "$here/Amatrix.readme.txt" "$here/ReadMe-GUI.utf8.txt"
out="$top/build/amiga/Amatrix.lha"
rm -f "$out" "$top/build/amiga/amatrix-gui.lha"
(cd "$tmp" && /opt/amiga/bin/lha aq "$out" Amatrix >/dev/null)
cp "$tmp/Amatrix/Amatrix.readme" "$top/build/amiga/Amatrix.readme"
rm -rf "$tmp"; ls -la "$out" "$top/build/amiga/Amatrix.readme"
