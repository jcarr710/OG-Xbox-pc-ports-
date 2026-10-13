# SPDX-License-Identifier: GPL-2.0-or-later
# Shared setup for the Wine test scripts (sourced, not run).
#
# JSRF_GAME  the extracted game folder (default.xbe + Media); required
# JSRF_RUN   scratch folder the game runs in (default: build/wine-run in the
#            repository); it gets links to the game files, a copy of
#            loader/jsrf.exe, and everything the run writes (jsrf.log,
#            jsrf.ini, _hdd\ saves, screenshots converted to shots/*.png)
# WINEPREFIX a 32-bit prefix (default: ~/.wine32, created on first use)
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
: "${JSRF_GAME:?set JSRF_GAME to the extracted game folder (default.xbe + Media)}"
RUN=${JSRF_RUN:-$ROOT/build/wine-run}
export WINEDEBUG=-all WINEARCH=win32 WINEPREFIX=${WINEPREFIX:-$HOME/.wine32}

[ -f "$ROOT/loader/jsrf.exe" ] || { echo "build loader/jsrf.exe first (make)"; exit 1; }
[ -f "$JSRF_GAME/default.xbe" ] || { echo "no default.xbe in $JSRF_GAME"; exit 1; }
mkdir -p "$RUN/shots"
ln -sfn "$JSRF_GAME/default.xbe" "$RUN/default.xbe"
ln -sfn "$JSRF_GAME/Media" "$RUN/Media"
cp "$ROOT/loader/jsrf.exe" "$RUN/"
rm -f "$RUN"/shot_*.bmp "$RUN"/shots/*.png "$RUN/xinput1_4.dll"
cd "$RUN"

# xvfb_run SECONDS COMMAND...: run headless on a fresh X server
xvfb_run() {
    local secs=$1; shift
    timeout -s KILL $((secs + 20)) xvfb-run -a -s "-screen 0 1600x1200x24" "$@"
}

# shots_to_png: convert shot_*.bmp to shots/*.png (needs Pillow)
shots_to_png() {
    python3 - <<'PY'
import glob, os
from PIL import Image
for f in sorted(glob.glob('shot_*.bmp')):
    Image.open(f).save('shots/' + os.path.basename(f)[:-4] + '.png')
    os.remove(f)
print(len(glob.glob('shots/*.png')), 'screenshots in', os.path.abspath('shots'))
PY
}

# ini_values: jsrf.ini without the comments, on one line
ini_values() { grep -av '^;' jsrf.ini | tr -d '\r' | grep -v '^$' | tr '\n' ' '; echo; }
