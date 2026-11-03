#!/bin/sh
# Fetches the MUI 5 SDK (headers, NList/TextEditor/... MCC headers) and the MUI
# 5 user archive for the emulator into third_party/mui (not in git: the MUI
# archives may only be redistributed complete and unmodified).
# Generates clib/inline headers for m68k-amigaos-gcc with sfdc.
set -e
REL=MUI-5.0-20210831
URL=https://github.com/amiga-mui/muidev/releases/download/$REL
top=$(cd "$(dirname "$0")/.." && pwd)
d="$top/third_party/mui"
LHA=${LHA:-/opt/amiga/bin/lha}
SFDC=${SFDC:-/opt/amiga/bin/sfdc}
[ -f "$d/.done-$REL" ] && exit 0
rm -rf "$d"; mkdir -p "$d/dl" "$d/core" "$d/contrib" "$d/include/clib" "$d/include/inline"
for f in $REL-os3.lha $REL-os3-contrib.lha; do
    curl -fsSL -o "$d/dl/$f" "$URL/$f"
done
(cd "$d/core" && $LHA xq "../dl/$REL-os3.lha")
(cd "$d/contrib" && $LHA xq "../dl/$REL-os3-contrib.lha")
cp -r "$d/core/SDK/MUI/C/include/." "$d/include/"
cp -r "$d/contrib/SDK/MUI/C/include/." "$d/include/"
cp "$d/core/SDK/MUI/C/Examples/"SDI_*.h "$d/include/"
$SFDC --mode=clib    --output="$d/include/clib/muimaster_protos.h" "$d/core/SDK/MUI/sfd/muimaster_lib.sfd"
$SFDC --mode=macros --target=m68k-amigaos --output="$d/include/inline/muimaster.h"      "$d/core/SDK/MUI/sfd/muimaster_lib.sfd"
# the SFD marks old functions DEPRECATED; define it for plain gcc
sed -i '0,/^#include/s//#ifndef DEPRECATED\n#define DEPRECATED\n#endif\n&/' "$d/include/clib/muimaster_protos.h"
touch "$d/.done-$REL"
echo "MUI SDK in $d/include"
