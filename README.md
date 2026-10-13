<img src="loader/res/icon.png" width="88" align="right" alt="">

# JSRF PC

Play your own copy of **Jet Set Radio Future** (Xbox, 2002) natively on
Windows: up to 8× the original resolution, a steady 60 fps, Xbox
controllers, and an in-game settings menu.

![The title screen at 3× the Xbox's resolution](docs/screenshots/title.jpg)

`jsrf.exe` loads the game's original Xbox executable and runs its code
directly on your PC's processor. The Xbox used an ordinary x86 CPU, so the
game's code doesn't need to be translated or interpreted. Everything the
game expects from the console around it (the Xbox kernel, the NV2A graphics
chip, the audio hardware, the controller ports) is re-created on top of
Windows and OpenGL. No Xbox BIOS or firmware is needed.

> [!IMPORTANT]
> **You need the game.** Nothing from Jet Set Radio Future is included here.
> JSRF PC runs the files from the **North American** Xbox disc
> (`default.xbe` and the `Media` folder), dumped from a disc you own.
> Please don't ask for game files.

<p align="center">
  <img src="docs/screenshots/garage.jpg" width="49%" alt="Skating in the Garage at the start of the game">
  <img src="docs/screenshots/settings.jpg" width="49%" alt="The settings menu over the game">
</p>
<p align="center"><sub>The Garage, and the settings menu (F1 or Back + Start). Rendered at 1920×1440, shown in 1600×1200 fullscreen.</sub></p>

## Contents

[Status](#status) ·
[Getting started](#getting-started) ·
[Controls](#controls) ·
[Settings](#settings) ·
[Options](#command-line-options) ·
[Troubleshooting](#troubleshooting) ·
[FAQ](#faq) ·
[How it works](#how-it-works) ·
[Building](#building-from-source) ·
[Credits](#credits) ·
[Legal](#legal)

## Status

**Version 0.1.0, the first release.** It has been played for over 45
minutes on Windows 11 with an NVIDIA RTX 5090 and a wireless Xbox
controller, from boot through the intro, title screen, New Game and the
tutorial, at a steady 60 fps with sound in 4K borderless fullscreen. It is
also regression-tested under Wine on Linux with scripted input.

What to expect from an early version:

* **Only the North American release works.** jsrf.exe checks when it starts
  and explains if your `default.xbe` is a different one.
* **Most of the game hasn't been played on it yet.** Graphics features the
  tested parts don't use may be missing (3D textures, for example), so
  later areas may show glitches. [Reports](#reporting-problems) are very
  welcome.
* **4:3 only.** There's a stretch-to-window option, but no real widescreen
  yet.
* **Only tested on NVIDIA.** AMD and Intel GPUs with OpenGL 3.3 should work
  but haven't been tried.
* **Multiplayer is untested.** Up to four controllers are supported, but
  nobody has played a multiplayer match on it yet.

## Getting started

You need 64-bit Windows 10 or 11, a graphics card with OpenGL 3.3 (anything
from the last ten years), and ideally an Xbox controller or another XInput
pad.

### 1. Get the game files

Original Xbox discs can't be read by an ordinary PC drive. The usual ways to
dump your copy are copying the game to a modded Xbox's hard drive and
transferring the folder over FTP, or imaging the disc with a PC drive that
has Kreon firmware.

If you have an ISO image of your disc, unpack it to a folder with an Xbox
image tool such as [xdvdfs](https://github.com/antangelo/xdvdfs)
(`xdvdfs unpack "Jet Set Radio Future.iso" JSRF`; it also runs
[in the browser](https://xiso.antangelo.com/)) or
[extract-xiso](https://github.com/XboxDev/extract-xiso).

Either way you should end up with a folder containing `default.xbe` and a
`Media` folder (about 2.4 GB).

### 2. Add jsrf.exe

Download the latest `jsrf-pc-…-windows.zip` from the
[Releases](../../releases) page (or [build it](#building-from-source)) and
put `jsrf.exe` in the game folder, next to `default.xbe`.

Keep the game folder somewhere you can write to, like Documents or a games
folder (not Program Files): saves, settings and the log are written there.

### 3. Play

Double-click `jsrf.exe`. About a minute of logos and intro plays first; then
press Start (or Enter) at the title screen.

The first time, Windows SmartScreen may say it "protected your PC", because
jsrf.exe isn't code-signed. Choose **More info → Run anyway**, or
[build it yourself](#building-from-source).

To check your dump by hand, run this in PowerShell in the game folder:

```powershell
Get-FileHash default.xbe -Algorithm SHA1
```

The supported version is `86FFB4081FFB1A8A5DA2E03D3838382CE31D7AE8`.

## Controls

| Xbox | Controller (XInput) | Keyboard and mouse |
|---|---|---|
| Left stick | Left stick | W A S D |
| Right stick | Right stick | I J K L |
| D-pad | D-pad | Arrow keys |
| A | A | Space |
| B | B | Shift |
| X | X | E |
| Y | Y | Q |
| L trigger | LT | Z or left mouse button |
| R trigger | RT | C or right mouse button |
| White | LB | F |
| Black | RB | R |
| Start | Start | Enter |
| Back | Back | Backspace or Esc |

| Key | |
|---|---|
| F1 (or Back + Start on a controller) | Settings menu |
| Alt+Enter | Window / borderless fullscreen |
| F12 | Screenshot (`shot_NNNNN.bmp` next to jsrf.exe) |

Controllers join in the order you use them: the first one you press a
button on becomes player 1, whatever slot Windows gave it. (Virtual pads from
Steam, DS4Windows or streaming tools can sit in the first slot without ever
sending input; they don't get in the way.) Up to four controllers; the
keyboard also drives player 1. The keyboard only works while the game window
has focus.

## Settings

Press **F1**, or **Back + Start** together on a controller, to open the
settings menu over the game. The game sees no input while it's open. Pick a
setting with the arrow keys or the D-pad/stick, change it with left and
right, and close the menu with Esc or B. Changes apply immediately.

| Setting | Choices | Default |
|---|---|---|
| Internal resolution | Auto, 1× to 8× (640×480 per step) | Auto: 2× at 1080p, 3× at 1440p, 4× at 4K |
| Display | Window, Fullscreen (borderless) | Window |
| Aspect ratio | 4:3, Stretch to window | 4:3 |
| Scaling filter | Smooth, Sharp | Smooth |
| VSync | Off, On | Off |
| Brightness | 50% to 200% | 100% |
| Texture filtering | Standard, 2× to 16× anisotropic | 8× |
| Frame counter | Off, On | Off |
| Volume | 0% to 100% | 100% |
| Vibration | Off, On | On |
| Stick dead zone | 0% to 30% | 12% |
| Log window | Hidden, Shown | Hidden |

Settings are saved in `jsrf.ini` next to jsrf.exe. It has a note on every
key and can be edited by hand; edits there apply the next time the game
starts.

The log window shows the game's log as it runs. Closing it just turns the
setting off; the game keeps running.

### Where things are saved

| File | |
|---|---|
| `_hdd\` in the game folder | The Xbox hard disk. Your saves are in `_hdd\Partition1\UDATA`; back this folder up. |
| `jsrf.ini` | Settings |
| `jsrf.log` | Log of the last run (replaced each time the game starts) |
| `shot_NNNNN.bmp` | Screenshots |

## Command-line options

Options override the settings for one run, for example `jsrf.exe --scale=3`.

| Option | |
|---|---|
| `--scale=N` | Internal resolution, 1 to 8 |
| `--window=WxH` | Starting window size |
| `--perf` | Log the frame rate and the GPU thread's load every 2 s |
| `--profile` | Log where each game thread spends its time every 10 s (`jsrf_profile.bat` runs this with `--perf`) |
| `--core=N` | The CPU core the game's threads share (default 2; see [How it works](#how-it-works)) |
| `--gl-debug` | Check for OpenGL errors after every call (slow) |
| `--screenshots=N` | Save a screenshot every N frames |
| `--trace` | Log every file and kernel call (very verbose) |
| `--press=BUTTON@SEC[:DUR]` | Scripted button press, for testing |
| `--any-xbe` | Run a `default.xbe` that isn't the supported version (for porting work; expect crashes) |
| `PATH\default.xbe` | Run a game folder somewhere else. Dragging `default.xbe` onto jsrf.exe does the same. Saves go in that folder; settings and the log stay next to jsrf.exe. |

Started from a command prompt, jsrf.exe also logs to that prompt. Setting the
environment variable `JSRF_MULTICORE=1` lets the game's threads run on all
cores instead of one.

## Troubleshooting

`jsrf.log`, next to jsrf.exe, records each step of starting up and anything
that went wrong. If the game can't start, a message box says why. The common
ones:

* **"This is a different (or modified) build of Jet Set Radio Future"** or
  **"…a different region's release…"**: only the North American release
  works for now, and a patched or modified `default.xbe` counts as
  different too.
* **"Could not map the Xbox's RAM" or "Could not reserve the Xbox device
  window"**: something else took the fixed memory addresses the Xbox uses.
  The message lists what. Overlays and injectors (FPS counters, recorders,
  some antivirus tools) are the usual cause; try closing them.
* **"OpenGL function … missing"**: the graphics driver doesn't provide
  OpenGL 3.3. Update it from your GPU maker's site.
* **A controller isn't picked up**: press a button on it; the first pad that
  sends input becomes player 1. `jsrf.log` lists every controller Windows
  reports and its first few button presses.
* **Slow or stuttering**: run `jsrf_profile.bat` for a minute and include
  `jsrf.log` in your report.

### Reporting problems

Open an issue and include:

* what happened, and where in the game;
* `jsrf.log` from the run where it happened (it's replaced the next time the
  game starts);
* an F12 screenshot, for graphics problems;
* your Windows version, GPU and graphics driver version.

## FAQ

**Is this a decompilation or a source port?**
No. It runs the game's original executable, with a few small patches, and
re-implements the console around it. Think of it as an emulator built for
exactly one game, which doesn't need to emulate the processor. For a
matching decompilation of the game, see
[JSRF-Decompilation](https://codeberg.org/KeybadeBlox/JSRF-Decompilation).

**How is it different from xemu?**
[xemu](https://xemu.app) emulates the whole Xbox and runs a large part of
its library; it needs dumps of the Xbox's boot ROM and BIOS. JSRF PC runs
one game, needs only that game's files, and runs the game's code directly
instead of emulating the CPU. The same idea, for Xbox games in general, is
behind [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded).

**Will other versions (PAL, Japanese) work?**
Not yet. jsrf.exe patches the game's code at fixed addresses, and those are
different in other builds. Supporting one means finding the same functions
in it; the tools in `tools/` help with that.

**Widescreen? Higher frame rates?**
Not yet. The game runs at 60 fps like the original, in 4:3, with an option
to stretch it.

**Does it run on Linux or the Steam Deck?**
It runs under Wine; that's how it's regression-tested (32-bit Wine prefix,
software rendering). It hasn't been tried on Linux with real GPU drivers or
on a Steam Deck. Reports are welcome.

**My antivirus doesn't like it.**
jsrf.exe isn't code-signed, and it does things normal programs rarely do: it
reserves memory at fixed addresses, patches the game's code in memory, and
catches the game's hardware accesses with an exception handler. If you'd
rather not trust a downloaded build, [build it yourself](#building-from-source);
release builds come with SHA-256 checksums.

## How it works

The Xbox is a 733 MHz Pentium III with an NVIDIA NV2A GPU, running a small
kernel. Games are linked with Microsoft's XDK libraries (Direct3D,
DirectSound, XInput and so on), which run in kernel mode and program the
hardware directly. JSRF PC keeps the game and its Direct3D library as they
are and emulates the graphics hardware they talk to; the audio and
controller libraries are replaced with Windows equivalents instead, which is
simpler than emulating their hardware:

| Layer | On the Xbox | In JSRF PC |
|---|---|---|
| Executable | `default.xbe` at 0x10000 | `jsrf.exe` is a 32-bit Windows program whose image reserves 0x11000–0x289000; the XBE is mapped there, at its original addresses |
| Kernel (120 functions used) | `xboxkrnl.exe` | Re-implemented on Win32 (`kernel.c`, `kfile.inc`, `kthread.c`) |
| Per-CPU state (`fs:`) | KPCR | 18 instructions patched to read a per-thread copy |
| Thread-local storage | At the top of each thread's stack | Every game thread runs on a stack laid out the same way |
| Timing | `rdtsc` at 733 MHz | Windows' performance counter, scaled to 733.33 MHz |
| RAM | 64 MB shared with the GPU | 64 MB mapped at 0x80000000 and 0xF0000000, where the XDK expects it |
| GPU (NV2A) | Direct3D writes push buffers and registers | The registers at 0xFD000000 are trapped and emulated; push buffers are decoded and the 3D commands drawn with OpenGL 3.3 (`nv2a/`, `gl/`) |
| Interrupts | Vertical blank and GPU interrupts | A timer thread raises them and runs Direct3D's own interrupt handler |
| Audio | DirectSound programs the audio processor | The DirectSound API is replaced by a software mixer (`dsound.c`) |
| Controllers | XInput over USB | The XInput API is replaced with Windows XInput plus keyboard and mouse (`input.c`) |

Because the game's Direct3D library runs unmodified against an emulated
NV2A, the renderer sees exactly the commands the real GPU would. Only a few
hot paths are replaced: uploading the gamma ramp and kicking off push
buffers (which would otherwise trap on every register access), and
Direct3D's spin-wait for the GPU, which now sleeps until the GPU signals
progress.

The XBE header and jsrf.exe's own PE header both belong at address 0x10000.
The XBE header wins, because the game's libraries read it there; jsrf.exe's
headers are moved into the unused tail of that page, since Windows keeps
reading them while the program runs.

The game's threads all share one CPU core, as they did on the Xbox's single
CPU; the game and its audio middleware rely on thread priorities meaning
what they mean on one core. Rendering, audio mixing and the window run on
other cores. The default is core 2, which avoids core 0 (busiest with system
interrupts) and is a performance core on Intel's hybrid CPUs.

## Building from source

jsrf.exe is a 32-bit Windows program built with MinGW-w64.

* **Linux or WSL:** `sudo apt install gcc-mingw-w64-i686 make zip`, then
  `make` in this folder.
* **Windows:** install [MSYS2](https://www.msys2.org/), open the *MSYS2
  MINGW32* shell, run `pacman -S mingw-w64-i686-gcc make`, then
  `make CROSS=` in this folder.

That builds `loader/jsrf.exe`. `make dist` also builds the release zips and
their checksums in `dist/`. GitHub Actions does the same for every push
(`.github/workflows/build.yml`) and drafts a release for version tags.

On Linux, `tools/wine/` has the headless test scripts used during
development: boot to gameplay with scripted input, the settings menu, the
log window. See [tools/wine/README.md](tools/wine/README.md).

### Source layout

```
loader/
  main.c        startup: address reservations, XBE loading and version check, patches, options, crash log
  kernel.c      the Xbox kernel: memory, threads, synchronization, timers, interrupts, Rtl, crypto
  kfile.inc     file system: the game disc and the hard disk (_hdd\)
  kthread.c     Xbox-style threads (own stacks, TLS at the top, per-thread kernel state)
  xmem.c        the 64 MB of shared RAM
  mmio.c        device registers: trapped accesses are decoded, emulated and resumed
  nv2a/         NV2A model: registers and interrupts, push-buffer processing, 3D engine state
  gl/           OpenGL renderer: window and context, render targets, textures, shaders,
                draws, settings menu and overlays
  dsound.c      DirectSound replacement and software mixer
  input.c       XInput replacement: controllers, keyboard, mouse
  settings.c    jsrf.ini
  logwin.c      the log window
  prof.c        the --profile sampling profiler
  hle.c         patching helper
  apu.c         audio hardware register stubs
  stubs.S       XBE address reservation, fs: and rdtsc helpers
  xbe.ld        linker script
  jsrf.rc, res/ icon, version information, manifest
docs/
  screenshots/  the pictures in this README
tools/
  xbeinfo.py    dump an XBE's header, certificate, sections, libraries and kernel imports
  scan.py       find fs: and hardware accesses in an XBE's code
  xboxkrnl.def  kernel export ordinals and names
  fakexi/       a stand-in xinput1_4.dll for tests
  wine/         headless test scripts
```

### Contributing

Bug reports, test results on other hardware, and fixes are welcome. Keep in
mind that this is a separate project from
[JSRF-Decompilation](https://codeberg.org/KeybadeBlox/JSRF-Decompilation),
which doesn't accept AI-assisted contributions (see [Credits](#credits)), so
please don't carry code from here over to it.

## Credits

JSRF PC was made by **jcarr710** together with **Claude**, Anthropic's AI
model, which did most of the reverse engineering and wrote most of the
code; jcarr710 directed the work and tested it on real hardware.

It builds on knowledge from the Xbox homebrew and emulation community:

* [xemu](https://github.com/xemu-project/xemu): NV2A register and method
  names (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
* [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase):
  finding the XDK library functions inside the game.
* [nxdk](https://github.com/XboxDev/nxdk): the kernel's export table.
* Cxbx and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded),
  which pioneered running Xbox games' code directly on Windows.

## Legal

Jet Set Radio Future is © SEGA. JSRF PC is an unofficial fan project, not
affiliated with or endorsed by SEGA or Microsoft. It contains none of the
game's code or data; you need your own copy of the game. The screenshots in
`docs/screenshots` show the game running in JSRF PC; the game's artwork in
them is © SEGA.

JSRF PC is free software under the GNU General Public License, version 2 or
(at your option) any later version; see [LICENSE](LICENSE). A few files are
under other licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
It comes with no warranty.
