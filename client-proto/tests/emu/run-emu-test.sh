#!/bin/bash
# run-emu-test.sh <amigados-script> [peer command ...]
# Runs an emulator test: Extra: = a fresh drawer with the Amiga binaries and
# package files, the script as SYS:autotest, the peer (if any) on the host.
set -u
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
TESTENV=${TESTENV:-$HOME/amiga/testenv/amiga-testenv}
MAX=${MAX:-300}
script=$1; shift
"$top/../testserver/ensure-up.sh" >/dev/null
extra=$(mktemp -d)
cp "$top/build-amiga/amatrix-recv" "$top/build-amiga/amxdiag" "$extra/" 2>/dev/null
cp "$top/amiga-package/amatrix-test" "$top/amiga-package/amxdiag-run" "$extra/" 2>/dev/null
iconv -f utf-8 -t latin1 "$script" > "$extra/.test"
rm -f "$TESTENV/system/out.txt"
"$TESTENV/run-amiga.sh" "$extra/.test" "$MAX" "$extra" > "$extra/.run.log" 2>&1 &
emu=$!
if [ $# -gt 0 ]; then sleep 6; "$@"; fi
wait $emu
tail -3 "$extra/.run.log" | grep -E "done after|TIMEOUT"
iconv -f latin1 -t utf-8 "$TESTENV/system/out.txt"
rm -rf "$extra"
