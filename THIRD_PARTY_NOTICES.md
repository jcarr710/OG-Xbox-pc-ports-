# Third-party notices

The ports are licensed under the GNU General Public License, version 2 or (at
your option) any later version ([LICENSE](LICENSE)). A few files come from,
or are generated from, other projects and keep their own terms:

| File | Origin | License |
|---|---|---|
| `core/nv2a/nv097.h`, `core/nv2a/methods_gen.h` | Generated from `hw/xbox/nv2a/nv2a_regs.h` in [xemu](https://github.com/xemu-project/xemu) ("QEMU Geforce NV2A register definitions", Copyright (c) 2012 espes, Copyright (c) 2015 Jannik Vogel), whose names come from nouveau/envytools | GNU LGPL, version 2 or later ([LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt)) |
| `core/xbe.ld` | The MinGW-w64 i386pe default linker script from GNU ld, Copyright (C) 2014-2024 Free Software Foundation, Inc., with an added address-space reservation | "Copying and distribution of this script, with or without modification, are permitted in any medium without royalty provided the copyright notice and this notice are preserved." |
| `tools/xboxkrnl.def` | [nxdk](https://github.com/XboxDev/nxdk), by Stefan Schmidt | CC0-1.0 |

The exes are linked statically with the MinGW-w64 runtime and libgcc, which
are under permissive licenses and the GCC Runtime Library Exception.

Used during development but not included:

* [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) (MIT)
  located the XDK library functions inside the games' executables; the
  addresses it found are in `titles/*/symbols.inc`, made by
  `tools/gen_symbols.py`.
* [Capstone](https://www.capstone-engine.org/) (BSD) is used by `tools/scan.py`.

The games themselves are not part of this project: Jet Set Radio Future is
© SEGA, and Midnight Club 3: DUB Edition Remix is © Take-Two Interactive
Software. You need your own copies.
