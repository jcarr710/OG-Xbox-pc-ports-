#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# gameplay.sh [jsrf.exe options...]
# Title screen -> New Game -> the tutorial in the Garage, with scripted
# keyboard-port presses (--press), 300 s, a screenshot every 600 frames.
P="--press=START@128:2 --press=START@140:1 --press=A@150:1"
for t in $(seq 165 6 285); do P="$P --press=A@$t:1"; done
exec "$(dirname "$0")/run.sh" 300 --window=640x480 --scale=1 --screenshots=600 $P "$@"
