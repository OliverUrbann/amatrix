#!/bin/sh
# Fetches Herald (Aminet util/cdity/Herald, free for private use) into
# third_party/herald for the GUI test in the emulator - not in git and not
# in our package: users install Herald themselves.
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
d="$top/third_party/herald"
LHA=${LHA:-/opt/amiga/bin/lha}
[ -f "$d/.done" ] && exit 0
rm -rf "$d"; mkdir -p "$d"
curl -fsSL -o "$d/Herald.lha" https://aminet.net/util/cdity/Herald.lha
(cd "$d" && $LHA xq Herald.lha)
touch "$d/.done"
echo "Herald in $d/Herald"
