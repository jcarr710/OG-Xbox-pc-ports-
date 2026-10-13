#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# menu.sh: opens the settings menu with the stand-in pad (Back + Start),
# changes four settings, closes it, and checks jsrf.ini.
. "$(dirname "$0")/common.sh"
make -s -C "$ROOT/tools/fakexi" && cp "$ROOT/tools/fakexi/xinput1_4.dll" .
rm -f jsrf.ini
S="BACK@20:0.5,START@20:0.5"
S="$S,RIGHT@22:0.15,RIGHT@22.4:0.15,RIGHT@22.8:0.15"                   # internal resolution: Auto -> 3x
S="$S,DOWN@24:0.15,DOWN@24.4:0.15,RIGHT@24.8:0.15"                     # aspect ratio -> Stretch
S="$S,DOWN@26:0.15,DOWN@26.4:0.15,DOWN@26.8:0.15"                      # -> Brightness
S="$S,RIGHT@27.2:0.15,RIGHT@27.6:0.15,RIGHT@28:0.15,RIGHT@28.4:0.15"   # 120%
S="$S,DOWN@29:0.15,DOWN@29.4:0.15,A@29.8:0.15"                         # frame counter on
S="$S,B@31:0.2"                                                        # close
export FAKE_XINPUT_SCRIPT="$S" WINEDLLOVERRIDES="xinput1_4=n" JSRF_SHOT_OVERLAY=1
xvfb_run 40 bash -c "timeout 40 wine jsrf.exe --window=960x720 --screenshots=60 > wine.txt 2>&1"
rm -f xinput1_4.dll
shots_to_png
ini_values
for kv in Scale=3 Aspect=1 Brightness=120 ShowFPS=1; do
    grep -aq "^$kv" jsrf.ini || { echo "expected $kv in jsrf.ini"; exit 1; }
done
echo "menu: OK"
