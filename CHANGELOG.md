# Changelog

Each game has its own versions and releases, tagged `jsrf-v…` and
`mc3-v…`. The runtime in `core/` is shared, so a change listed for one game
reaches the other one's next release too.

## JSRF PC 0.2.0 (2026-10-14)

* JSRF PC now runs on the runtime it shares with MC3 PC, which picked up
  everything listed under MC3 PC 0.1.0. Jet Set Radio Future plays the same
  as on 0.1.0: tested in the game at a steady 60 fps on the same PC.
* New options for testing: `--ds-trace` (DirectSound calls and mixer
  levels) and `--control` (commands while the game runs, including a capture
  of every draw call in a frame).

## MC3 PC 0.1.0 (2026-10-14)

First release.

* Runs the North American Xbox release of Midnight Club 3: DUB Edition
  Remix on 64-bit Windows 10 and 11, from your own copy of the game's files.
  mc3.exe checks the version of `default.xbe` when it starts.
* Same features as JSRF PC 0.1.0: 1× to 8× resolution, anisotropic
  filtering, controllers, keyboard and mouse, the settings menu (saved to
  `mc3.ini`), F12 screenshots, the log window and the diagnostics.
* Tested: the intro videos, menus and profiles (saving and loading), the
  start of Career mode, and driving around San Diego with music, at 30 fps.

What the runtime needed for it:

* XDK 5849 libraries: Direct3D's packed push-buffer commands and fences,
  and its versions of the push-buffer kick-off and GPU wait, which are
  patched like JSRF's.
* 44 more kernel functions, and file objects detailed enough for the
  game's streaming code.
* DirectSound: streams (XACT plays the music through them), mix bins and
  their volumes, 3D voice volumes, envelope releases, synchronized starts,
  and stream and buffer status reported the way XACT expects, so a song
  that stops doesn't keep playing under the next one.
* Graphics: depth buffers read as textures and colour rendered into a depth
  buffer's memory (MC3 makes a half-size copy of the depth buffer that
  way), and NV2A's signed blend equations, which add and subtract with
  wrap-around. Together they make the headlights light the road instead of
  drawing a solid white shape around the car.
* The network adapter is present but has no link.
* For development: `--control` (scripted commands while the game runs) and
  a frame capture that logs every draw call of a frame.

## JSRF PC 0.1.0 (2026-10-12)

First public release.

* Runs the North American Xbox release of Jet Set Radio Future on 64-bit
  Windows 10 and 11, from your own copy of the game's files. jsrf.exe checks
  the version of `default.xbe` when it starts.
* Graphics through OpenGL 3.3, at 1× to 8× the original 640×480 (picked from
  your monitor by default), with anisotropic filtering, 4:3 or stretched,
  windowed or borderless fullscreen.
* Xbox and other XInput controllers, up to four, which join in the order you
  use them; vibration; keyboard and mouse for player 1.
* Sound through a software mixer.
* In-game settings menu (F1, or Back + Start on a controller), saved to
  `jsrf.ini`.
* Saves in `_hdd\` in the game folder; F12 screenshots; an optional live log
  window; `--perf` and `--profile` diagnostics.
