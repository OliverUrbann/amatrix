#!/bin/sh
# End-to-end test against a local Synapse:
#   peer.py (matrix-nio + vodozemac) sends encrypted messages,
#   amatrix-recv receives and decrypts them.
# Expects Synapse on http://127.0.0.1:8008 with users alice/testalice and
# amiga/testamiga (see README), and matrix-nio[e2e] in $PY's environment.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
HS=${HS:-http://127.0.0.1:8008}
PY=${PY:-python3}
STORE=${STORE:-$here/build/test-store}
"$here/build/amatrix-recv" -s "$STORE" -n 2 -t 90 "$HS" amiga testamiga > "$here/build/recv.log" 2>&1 &
pid=$!
sleep 3
"$PY" "$here/tools/peer.py" "$HS" @alice:localhost testalice @amiga:localhost \
    "Hallo Amiga! Grüße aus Python." "Zweite Nachricht – äöü ß 🐄"
wait $pid
cat "$here/build/recv.log"
grep -q "done, 2 message(s) decrypted" "$here/build/recv.log" && echo PASS || { echo FAIL; exit 1; }
