#!/bin/bash
# Installs AmiSSL 5.27 (OS3 runtime) into the emulated system drawer of the
# FS-UAE test environment.  Downloads the release from GitHub if needed.
set -eu
TESTENV=${TESTENV:-$HOME/amiga/testenv/amiga-testenv}
CACHE=${CACHE:-$HOME/.cache/amatrix}
A=${AMISSL_OS3:-$CACHE/amissl-5.27-os3/AmiSSL}
if [ ! -d "$A" ]; then
  mkdir -p "$CACHE/amissl-5.27-os3"
  curl -sSfL -o "$CACHE/AmiSSL-5.27-OS3.lha" \
    https://github.com/jens-maus/amissl/releases/download/5.27/AmiSSL-5.27-OS3.lha
  (cd "$CACHE/amissl-5.27-os3" && /opt/amiga/bin/lha xq "$CACHE/AmiSSL-5.27-OS3.lha")
fi
S=$TESTENV/system
mkdir -p "$S/AmiSSL/Libs/AmiSSL" "$S/AmiSSL/Certs"
cp "$A/Libs/AmigaOS3/amisslmaster.library" "$S/AmiSSL/Libs/"
cp "$A/Libs/AmigaOS3/AmiSSL/68020-40/"* "$S/AmiSSL/Libs/AmiSSL/"
cp "$A/Certs/"* "$S/AmiSSL/Certs/"
echo "AmiSSL installed in $S/AmiSSL (tests/run.py adds the test CA of each run)"
