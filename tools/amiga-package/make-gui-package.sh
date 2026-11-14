#!/bin/bash
# Builds the Aminet upload pair build/amiga/Amatrix.lha + Amatrix.readme:
# the MUI client Amatrix with its icons, the language catalogs and
# Amatrix.readme (English + German, Aminet header), ISO-8859-1.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
make -C "$top" -j4 TARGET=amiga gui >/dev/null
tmp=$(mktemp -d); mkdir "$tmp/Amatrix"
cp "$top/build/amiga/gui/Amatrix" "$tmp/Amatrix/"
python3 "$here/make-icon.py" "$tmp/Amatrix/Amatrix.info" 65536
mkdir -p "$tmp/Amatrix/Icons"
for c in "$top"/build/amiga/catalogs/*/amatrix.catalog; do          # languages (English is built in)
    f=$(basename "$(dirname "$c")")
    l=$(sed -n 's/^## language *//p' "$top/catalogs/$f.ct" | iconv -f utf-8 -t latin1)   # AmigaOS name, e.g. fran\347ais
    mkdir -p "$tmp/Amatrix/Catalogs/$l"; cp "$c" "$tmp/Amatrix/Catalogs/$l/"
done
python3 "$here/make-icon.py" "$tmp/Amatrix/Icons/Amatrix-neu.info" 65536 new   # AppIcon: unread
iconv -f utf-8 -t latin1 "$here/Amatrix.readme.txt" > "$tmp/Amatrix/Amatrix.readme"
python3 "$here/make-icon.py" "$tmp/Amatrix/Amatrix.readme.info" 4096 text     # text icon, opens in MultiView
python3 "$here/make-icon.py" "$tmp/Amatrix.info" 4096 drawer                  # the drawer itself
# Aminet: Short <= 40 characters, lines <= 78
short=$(sed -n 's/^Short: *//p' "$here/Amatrix.readme.txt")
[ ${#short} -le 40 ] || { echo "Short is ${#short} characters (max 40)"; exit 1; }
awk 'length>78{print FILENAME": line "NR" too long"; bad=1} END{exit bad}' "$tmp/Amatrix/Amatrix.readme"
out="$top/build/amiga/Amatrix.lha"
rm -f "$out" "$top/build/amiga/amatrix-gui.lha"
(cd "$tmp" && /opt/amiga/bin/lha aq --system-kanji-code=latin1 --archive-kanji-code=latin1 "$out" Amatrix Amatrix.info >/dev/null)
cp "$tmp/Amatrix/Amatrix.readme" "$top/build/amiga/Amatrix.readme"
rm -rf "$tmp"; ls -la "$out" "$top/build/amiga/Amatrix.readme"
