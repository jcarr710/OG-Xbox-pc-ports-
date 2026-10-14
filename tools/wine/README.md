# Headless tests under Wine

These scripts run a port on Linux under Wine on a virtual X server, with
scripted input, and leave the log, settings and screenshots in a scratch
folder. They are how the ports were regression-tested during development
(with Mesa's llvmpipe software renderer, so expect a few game frames per
second; the games' own timing stays correct).

Requirements: `wine` with a 32-bit prefix, `xvfb-run`, Python 3 with Pillow,
and the MinGW-w64 cross compiler (for the helper DLL/EXE). `keys.sh` and
`logwin.sh` also need `xdotool`; `logwin.sh` needs ImageMagick's `import`.

```sh
make                                          # build/jsrf.exe and build/mc3.exe
export GAME=~/games/jsrf                      # your extracted game: default.xbe + Media/
tools/wine/run.sh 60 --screenshots=120        # boot JSRF for a minute
T=mc3 GAME=~/games/mc3 tools/wine/run.sh 300 --control   # MC3, driven by hand (below)
```

`T` picks the game (`jsrf`, the default, or `mc3`) and `GAME` its folder.

| Script | What it checks |
|---|---|
| `run.sh SECONDS [options]` | Boots the game with any of the exe's options; fails on a fatal error or GL error in the log |
| `gameplay.sh` | JSRF: title screen → New Game → tutorial, through the keyboard port (`--press`) |
| `pad.sh` | JSRF: the same, driven by a stand-in `xinput1_4.dll` (`tools/fakexi`) whose pad sits in XInput slot 1 behind an idle pad in slot 0; the scripted pad must become player 1 |
| `menu.sh` | Settings menu with the stand-in pad (Back + Start), four changes, saved to the .ini |
| `keys.sh` | Settings menu from the keyboard (F1, arrows, Esc) |
| `logwin.sh` | The log window: closing it hides it and turns the setting off; the menu brings it back |

The run folder defaults to `build/wine-run-<game>/` in the repository
(override with `RUN_DIR`). It holds links to your game files, so nothing is
written to the game folder itself; screenshots end up in its `shots/`
folder.

The first run creates the Wine prefix (`WINEPREFIX`, default `~/.wine32`). If
Wine asks to install Mono or Gecko, neither is needed.

## Driving a run by hand

Started with `--control`, the exe reads commands from `control.txt` next to
it, about ten times a second, and deletes the file once it has read it. Write
the file somewhere else and rename it into place so it's never read half
written:

```sh
cd build/wine-run-mc3
echo "press A 0.3" > c.tmp && mv c.tmp control.txt    # hold A for 0.3 s
echo "shot" > c.tmp && mv c.tmp control.txt           # screenshot (shot_NNNNN.bmp)
```

| Command | |
|---|---|
| `press BUTTON [SECONDS]` | Hold a button, as in `--press` (A, B, X, Y, START, BACK, UP, DOWN, LEFT, RIGHT, LT, RT, WHITE, BLACK, LUP, LDOWN, LLEFT, LRIGHT) |
| `shot` | Save a screenshot |
| `capture` | Capture the next frame (below) |

Under software rendering a game may only poll its controllers a few times a
second, so hold buttons for at least that long.

## Frame capture

`capture` (or the environment variable `<GAME>_CAPTURE_AT=SECONDS`) records
the next whole frame, from one flip to the next, in `capture.txt`: every
clear, blit and draw call with the render target it went to, the state that
decides its look (blending, depth and stencil, the texture stages and their
formats, the register combiners), and what it changed in the render target:
the share of pixels changed, the mean change in brightness and in alpha, and
the bounding box. Draws that change more than 1% of the target also get
their first vertices logged, the render target saved afterwards
(`capNNNN_rt<address>.bmp`, plus `_alpha.bmp` when the alpha channel
changed), and their textures saved once (`cap_tex_<address>_<format>.bmp`).
Comparing those lets you find the draw call behind a graphics glitch.
