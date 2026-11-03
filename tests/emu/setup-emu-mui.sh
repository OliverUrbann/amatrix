#!/bin/bash
# Installs MUI 5 (with the contrib classes NList, TextEditor, BetterString, ...)
# into the emulated system drawer of the FS-UAE test environment, as the MUI
# installer would (SYS:MUI, assigns in S:User-Startup).
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
TESTENV=${TESTENV:-$HOME/amiga/testenv/amiga-testenv}
"$top/third_party/fetch-mui.sh" >/dev/null
M=$top/third_party/mui
S=$TESTENV/system
rm -rf "$S/MUI"
cp -r "$M/core/MUI" "$S/MUI"
cp -r "$M/contrib/MUI/." "$S/MUI/"
rm -rf "$S/MUI/Docs" "$S/MUI/Demos"            # not needed, saves boot time
us=$S/S/User-Startup
touch "$us"
if ! grep -q "assign MUI:" "$us"; then
cat >> "$us" <<'US'
;BEGIN MUI
if exists SYS:MUI
   assign MUI: SYS:MUI
   assign add LIBS: MUI:Libs
   assign add LOCALE: MUI:Locale
endif
;END MUI
US
fi
grep -q "User-Startup" "$S/S/startup-sequence" || \
  sed -i 's#^SYS:System/RexxMast >NIL:#&\nIF EXISTS S:User-Startup\n  Execute S:User-Startup\nEndIF#' "$S/S/startup-sequence"
# Workbench screen for GUI tests: PAL hires interlace 640x512, 256 colours
# (AGA), applied by IPrefs from S:User-Startup
python3 - "$S/Prefs/Env-Archive/Sys/screenmode.prefs" <<'PY'
import struct, sys
prhd = struct.pack(">BBI", 0, 0, 0)
scrm = struct.pack(">4IIHHHH", 0, 0, 0, 0, 0x00029004, 640, 512, 8, 0)
body = b"PREF" + b"PRHD" + struct.pack(">I", len(prhd)) + prhd + b"\0" * (len(prhd) & 1) \
     + b"SCRM" + struct.pack(">I", len(scrm)) + scrm
open(sys.argv[1], "wb").write(b"FORM" + struct.pack(">I", len(body)) + body)
PY
grep -q "^IPrefs" "$us" || echo "IPrefs" >> "$us"
echo "MUI 5 installed in $S/MUI"
