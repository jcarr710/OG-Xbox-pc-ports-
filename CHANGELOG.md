# Changelog

## 0.1.0 (2026-10-12)

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
