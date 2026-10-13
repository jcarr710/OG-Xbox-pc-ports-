# Headless tests under Wine

These scripts run `jsrf.exe` on Linux under Wine on a virtual X server, with
scripted input, and leave the log, settings and screenshots in a scratch
folder. They are how the loader was regression-tested during development
(with Mesa's llvmpipe software renderer, so expect 5–15 game frames per
second; the game's own timing stays correct).

Requirements: `wine` with a 32-bit prefix, `xvfb-run`, Python 3 with Pillow,
and the MinGW-w64 cross compiler (for the helper DLL/EXE). `keys.sh` and
`logwin.sh` also need `xdotool`; `logwin.sh` needs ImageMagick's `import`.

```sh
make                                     # loader/jsrf.exe
export JSRF_GAME=~/games/jsrf            # your extracted game: default.xbe + Media/
tools/wine/run.sh 60 --screenshots=120   # boot for a minute
tools/wine/gameplay.sh                   # title -> New Game -> tutorial (5 minutes)
```

| Script | What it checks |
|---|---|
| `run.sh SECONDS [options]` | Boots the game with any `jsrf.exe` options; fails on a fatal error or GL error in the log |
| `gameplay.sh` | Title screen → New Game → tutorial, through the keyboard port (`--press`) |
| `pad.sh` | The same, driven by a stand-in `xinput1_4.dll` (`tools/fakexi`) whose pad sits in XInput slot 1 behind an idle pad in slot 0; the scripted pad must become player 1 |
| `menu.sh` | Settings menu with the stand-in pad (Back + Start), four changes, saved to `jsrf.ini` |
| `keys.sh` | Settings menu from the keyboard (F1, arrows, Esc) |
| `logwin.sh` | The log window: closing it hides it and turns the setting off; the menu brings it back |

The run folder defaults to `build/wine-run/` in the repository (override with
`JSRF_RUN`). It holds links to your game files, so nothing is written to the
game folder itself; screenshots end up in `build/wine-run/shots/`.

The first run creates the Wine prefix (`WINEPREFIX`, default `~/.wine32`). If
Wine asks to install Mono or Gecko, neither is needed.
