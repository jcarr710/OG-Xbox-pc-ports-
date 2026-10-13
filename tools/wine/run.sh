#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# run.sh SECONDS [jsrf.exe options...]
# Runs the game headless for SECONDS; the log is jsrf.log in the run folder.
# Example: run.sh 60 --window=800x600 --screenshots=120 --perf
. "$(dirname "$0")/common.sh"
secs=${1:-30}; shift || true
xvfb_run "$secs" bash -c "timeout $secs wine jsrf.exe $* > wine.txt 2>&1"
shots_to_png
grep -a "FATAL\|GL error" jsrf.log && exit 1 || true
