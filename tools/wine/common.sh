# SPDX-License-Identifier: GPL-2.0-or-later
# Shared setup for the Wine test scripts (sourced, not run).
#
# T          which game: jsrf (default) or mc3 -> build/$T.exe, $T.log, $T.ini
# GAME       the extracted game folder (default.xbe and the rest); required
#            (JSRF_GAME is accepted for T=jsrf)
# RUN_DIR    scratch folder the game runs in (default: build/wine-run-$T in
#            the repository); it gets links to the game files, a copy of the
#            exe, and everything the run writes ($T.log, $T.ini, _hdd\ saves,
#            screenshots converted to shots/*.png)
# WINEPREFIX a 32-bit prefix (default: ~/.wine32, created on first use)
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=${T:-jsrf}
GAME=${GAME:-${JSRF_GAME:-}}
: "${GAME:?set GAME to the extracted game folder (default.xbe and the rest)}"
RUN=${RUN_DIR:-$ROOT/build/wine-run-$T}
EXE=$T.exe
LOG=$T.log
INI=$T.ini
export WINEDEBUG=-all WINEARCH=win32 WINEPREFIX=${WINEPREFIX:-$HOME/.wine32}

[ -f "$ROOT/build/$EXE" ] || { echo "build build/$EXE first (make $T)"; exit 1; }
[ -f "$GAME/default.xbe" ] || { echo "no default.xbe in $GAME"; exit 1; }
mkdir -p "$RUN/shots"
for f in "$GAME"/*; do                      # link the game's files and folders
    b=$(basename "$f"); [ "$b" = _hdd ] && continue
    ln -sfn "$f" "$RUN/$b"
done
cp "$ROOT/build/$EXE" "$RUN/"
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

# ini_values: the .ini without the comments, on one line
ini_values() { grep -av '^;' "$INI" | tr -d '\r' | grep -v '^$' | tr '\n' ' '; echo; }
