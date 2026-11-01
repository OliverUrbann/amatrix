#!/bin/sh
# End-to-end test against a local Synapse:
#   peer.py (matrix-nio + vodozemac) sends encrypted messages,
#   amatrix-recv receives and decrypts them.
# Expects Synapse on http://127.0.0.1:8008 with users alice/testalice and
# amiga/testamiga (see README), and matrix-nio[e2e] in $PY's environment.
# Everything can be overridden from the environment, e.g. for the test
# server in ../testserver:  DOMAIN=amatrix.test ALICE_PW=alice-pw AMIGA_PW=amiga-pw
# BIN selects the binary (default build/amatrix-recv; build-sock/... for the
# socket HTTP code the Amiga uses).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
HS=${HS:-http://127.0.0.1:8008}
PY=${PY:-python3}
DOMAIN=${DOMAIN:-localhost}
ALICE_PW=${ALICE_PW:-testalice}
AMIGA_PW=${AMIGA_PW:-testamiga}
BIN=${BIN:-$here/build/amatrix-recv}
STORE=${STORE:-$here/build/test-store}
LOG=${LOG:-$here/build/recv.log}
mkdir -p "$(dirname "$LOG")"
"$BIN" -s "$STORE" -n 2 -t 90 "$HS" amiga "$AMIGA_PW" > "$LOG" 2>&1 &
pid=$!
sleep 3
"$PY" "$here/tools/peer.py" "$HS" "@alice:$DOMAIN" "$ALICE_PW" "@amiga:$DOMAIN" \
    "Hallo Amiga! Grüße aus Python." "Zweite Nachricht – äöü ß 🐄"
wait $pid
cat "$LOG"
grep -q "done, 2 message(s) decrypted" "$LOG" && echo PASS || { echo FAIL; exit 1; }
