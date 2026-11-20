#!/bin/bash
# Installs AmiSSL 5.27 (OS 4 runtime: amisslmaster.library, amissl_v362.library,
# root certificates in DEVS:AmiSSL/Certs = AmiSSL:Certs) on the OS 4 disk image
# of the test environment.  The OS 4.1 FE image ships an older amisslmaster
# (5.25) that refuses the 5.27 API ("OpenAmiSSLTags failed (1)").
# Works through the OS 3 test environment, which can write the SFS disk.
set -eu
OS4ENV=${OS4ENV:-/root/env/nc/claude-env/Emulation/AmigaOS4-Linux}
TESTENV=${TESTENV:-$HOME/amiga/testenv/amiga-testenv}
CACHE=${CACHE:-$HOME/.cache/amatrix}
A=$CACHE/amissl-5.27-os4/AmiSSL
if [ ! -d "$A" ]; then
  mkdir -p "$CACHE/amissl-5.27-os4"
  curl -sSfL -o "$CACHE/AmiSSL-5.27-OS4.lha" \
    https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-OS4.lha
  (cd "$CACHE/amissl-5.27-os4" && /opt/amiga/bin/lha xq "$CACHE/AmiSSL-5.27-OS4.lha")
fi
E=$(mktemp -d)
cp -r "$A/Libs/AmigaOS4" "$E/Libs"; cp -r "$A/Certs" "$E/Certs"
cat > "$E/prep" <<'EOS'
FailAt 1000
Copy Extra:Libs/amisslmaster.library AmigaOS4.1FE:Libs/ CLONE
Copy Extra:Libs/AmiSSL AmigaOS4.1FE:Libs/AmiSSL ALL CLONE
Makedir AmigaOS4.1FE:Devs/AmiSSL/Certs
Copy Extra:Certs AmigaOS4.1FE:Devs/AmiSSL/Certs ALL CLONE QUIET
Version AmigaOS4.1FE:Libs/amisslmaster.library FILE >SYS:out.txt
Echo "done" >SYS:amiga-done
EOS
cp "$OS4ENV/run-os3-hdf.sh" "$TESTENV/run-hdf.sh"
"$TESTENV/run-hdf.sh" "$E/prep" 200 "$OS4ENV/os41fe.hdf" "$E" | grep -v '^done after'
rm -rf "$E"
