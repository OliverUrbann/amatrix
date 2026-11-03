#!/bin/bash
# Boot the test Amiga headless, run a script in it, wait until it is done,
# taking screenshots on request (a variant of the test environment's
# run-amiga.sh for GUI tests).
#
# usage: run-amiga-gui.sh <amigados-script> [max-seconds] [extra-dir] [shot-dir]
#
# Inside the Amiga, creating the file SYS:shot-<name> asks for a screenshot:
# it is saved as <shot-dir>/<name>.png and the file is deleted again, which
# the Amiga side can wait for.
#
# The script runs as SYS:autotest in the Amiga (an AmigaDOS script, FailAt
# 1000).  It must end with:  Echo "done" >SYS:amiga-done
# Write results to SYS:out.txt and read them in system/out.txt afterwards.
# extra-dir (optional) is mounted as a second drive "Extra:" (UAE directory).
# A screenshot of the end state is saved as screen.png.
set -u
here=${TESTENV:-$HOME/amiga/testenv/amiga-testenv}
script=${1:?usage: run-amiga.sh <script> [max-seconds] [extra-dir]}
max=${2:-120}
extra=${3:-}
shots=${4:-$here/shots}
mkdir -p "$shots"
sys=$here/system

cp "$script" "$sys/autotest"
rm -f "$sys/amiga-done"

cfg=$(mktemp --suffix=.fs-uae)
cat > "$cfg" <<EOF
[fs-uae]
amiga_model = A1200
cpu = 68030
fpu = 68882
zorro_iii_memory = 65536
kickstart_file = $here/kickstart/Kickstart-3.1-40.068-A1200.rom
hard_drive_0 = $sys
hard_drive_0_label = Work
bsdsocket_library = 1
uae_cpu_speed = max
floppy_drive_0_sounds = off
sound_card = none
fullscreen = 0
automatic_input_grab = 0
EOF
[ -n "$extra" ] && printf 'hard_drive_1 = %s\nhard_drive_1_label = Extra\n' "$extra" >> "$cfg"

disp=:${AMIGA_DISPLAY:-42}
Xvfb $disp -screen 0 1024x768x24 >/dev/null 2>&1 & xpid=$!
sleep 1
DISPLAY=$disp fs-uae "$cfg" > "$here/fs-uae.log" 2>&1 & upid=$!

i=0
t0=$(date +%s)
while [ ! -f "$sys/amiga-done" ] && [ $i -lt "$max" ]; do
  for f in "$sys"/shot-*; do
    case "$f" in *.uaem|*'*') continue;; esac
    [ -f "$f" ] || continue
    n=${f##*/shot-}
    sleep 1
    DISPLAY=$disp xwd -root -silent | convert xwd:- -crop 692x540+166+114 +repage "$shots/$n.png" 2>/dev/null
    rm -f "$f" "$f.uaem"
    echo "screenshot $n"
  done
  sleep 1; i=$(( $(date +%s) - t0 ))
done
[ -f "$sys/amiga-done" ] && echo "done after ${i}s" || echo "TIMEOUT after ${i}s"
# give the filesystems time to write their buffers before the hard stop
sleep 8
DISPLAY=$disp xwd -root -silent | convert xwd:- "$here/screen.png" 2>/dev/null
kill $upid 2>/dev/null; sleep 2; kill -9 $upid 2>/dev/null
kill $xpid 2>/dev/null
wait 2>/dev/null
rm -f "$sys/autotest" "$sys/autotest.uaem" "$cfg"
[ -f "$sys/out.txt" ] && cat "$sys/out.txt"
