# Third-party notices

JSRF PC is licensed under the GNU General Public License, version 2 or (at
your option) any later version ([LICENSE](LICENSE)). A few files come from, or
are generated from, other projects and keep their own terms:

| File | Origin | License |
|---|---|---|
| `loader/nv2a/nv097.h`, `loader/nv2a/methods_gen.h` | Generated from `hw/xbox/nv2a/nv2a_regs.h` in [xemu](https://github.com/xemu-project/xemu) ("QEMU Geforce NV2A register definitions", Copyright (c) 2012 espes, Copyright (c) 2015 Jannik Vogel), whose names come from nouveau/envytools | GNU LGPL, version 2 or later ([LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt)) |
| `loader/xbe.ld` | The MinGW-w64 i386pe default linker script from GNU ld, Copyright (C) 2014-2024 Free Software Foundation, Inc., with an added address-space reservation | "Copying and distribution of this script, with or without modification, are permitted in any medium without royalty provided the copyright notice and this notice are preserved." |
| `tools/xboxkrnl.def` | [nxdk](https://github.com/XboxDev/nxdk), by Stefan Schmidt | CC0-1.0 |

`jsrf.exe` is linked statically with the MinGW-w64 runtime and libgcc, which
are under permissive licenses and the GCC Runtime Library Exception.

Used during development but not included:

* [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) (MIT)
  located the XDK library functions inside the game's executable; the
  addresses it found are what `loader/hle.h`, `input.c` and `dsound.c` patch.
* [Capstone](https://www.capstone-engine.org/) (BSD) is used by `tools/scan.py`.

The game itself is not part of this project: Jet Set Radio Future is
© SEGA. You need your own copy.
