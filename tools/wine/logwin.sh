#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# logwin.sh: starts with LogWindow=1, closes the log window (it should hide
# and turn the setting off, not quit), then turns it back on from the menu.
# Needs xdotool and ImageMagick (import) for the desktop screenshots.
. "$(dirname "$0")/common.sh"
${CROSS:-i686-w64-mingw32-}gcc -O2 -o closewin.exe "$ROOT/tools/wine/closewin.c"
printf '[System]\r\nLogWindow=1\r\n' > $INI
xvfb_run 60 bash -c '
  wine $EXE --window=800x600 > wine.txt 2>&1 &
  sleep 14
  import -window root shots/desktop1.png
  wine closewin.exe JSRFLog; sleep 2
  echo "after closing it: $(grep -a ^LogWindow= $INI)"
  W=$(xdotool search --name "Jet Set Radio" | head -1)
  xdotool windowfocus --sync $W 2>/dev/null; sleep 1
  xdotool key F1; sleep 1.5
  for k in Up Up Up Right; do xdotool key $k; sleep 0.4; done; sleep 2
  import -window root shots/desktop2.png
  xdotool key Escape; sleep 2
  echo "after the menu: $(grep -a ^LogWindow= $INI)"
  kill %1; sleep 1'
rm -f closewin.exe
grep -aq "log window closed" $LOG && grep -aq "^LogWindow=1" $INI && echo "logwin: OK" || { echo "logwin: FAILED"; exit 1; }
