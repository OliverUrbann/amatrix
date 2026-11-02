#!/bin/bash
# Installs AmiSSL 5.27 (OS3 runtime) into the emulated system drawer.  Needs AmiSSL-5.27-OS3.lha unpacked in $AMISSL_OS3.
set -eu
TESTENV=${TESTENV:-$HOME/amiga/testenv/amiga-testenv}
A=${AMISSL_OS3:-$HOME/amiga/dl/amissl/os3/AmiSSL}
S=$TESTENV/system
mkdir -p "$S/AmiSSL/Libs/AmiSSL" "$S/AmiSSL/Certs"
cp "$A/Libs/AmigaOS3/amisslmaster.library" "$S/AmiSSL/Libs/"
cp "$A/Libs/AmigaOS3/AmiSSL/68020-40/"* "$S/AmiSSL/Libs/AmiSSL/"
cp "$A/Certs/"* "$S/AmiSSL/Certs/"
echo "AmiSSL installed in $S/AmiSSL (tests/run.py adds the test CA of each run)"
