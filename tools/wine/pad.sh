#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# pad.sh [SECONDS] [jsrf.exe options...]
# Like gameplay.sh, but the presses come from the stand-in xinput1_4.dll
# (tools/fakexi): an idle virtual pad in XInput slot 0 and the scripted pad in
# slot 1, which has to become player 1 by being the one that's used.
. "$(dirname "$0")/common.sh"
secs=${1:-300}; shift || true
make -s -C "$ROOT/tools/fakexi" && cp "$ROOT/tools/fakexi/xinput1_4.dll" .
S="START@128:2,START@140:1,A@150:1"
for t in $(seq 165 6 285); do S="$S,A@$t:1"; done
export FAKE_XINPUT_SCRIPT="$S" WINEDLLOVERRIDES="xinput1_4=n"
xvfb_run "$secs" bash -c "timeout $secs wine $EXE --window=640x480 --scale=1 --screenshots=600 $* > wine.txt 2>&1"
rm -f xinput1_4.dll
shots_to_png
grep -a "is player 1" $LOG || { echo "the scripted pad never became player 1"; exit 1; }
