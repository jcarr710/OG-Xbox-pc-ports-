#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# keys.sh: the settings menu from the keyboard (needs xdotool): F1 opens it,
# arrows change two settings, F12 takes a screenshot, Esc closes it.
. "$(dirname "$0")/common.sh"
rm -f $INI
export JSRF_SHOT_OVERLAY=1
xvfb_run 60 bash -c '
  wine $EXE --window=960x720 > wine.txt 2>&1 &
  sleep 12
  W=$(xdotool search --name "Jet Set Radio" | head -1)
  xdotool windowfocus --sync $W 2>/dev/null; sleep 1
  xdotool key F1; sleep 2
  for k in Down Down Down Right; do xdotool key $k; sleep 0.5; done; sleep 0.5        # scaling filter -> Sharp
  for k in Down Down Right Right; do xdotool key $k; sleep 0.5; done; sleep 0.5       # brightness 110%
  xdotool key F12; sleep 2
  xdotool key Escape; sleep 3
  kill %1; sleep 1'
shots_to_png
ini_values
for kv in Filter=1 Brightness=110; do
    grep -aq "^$kv" $INI || { echo "expected $kv in $INI"; exit 1; }
done
echo "keys: OK"
