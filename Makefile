# SPDX-License-Identifier: GPL-2.0-or-later
# One shared runtime (core/), one small directory per game (titles/<name>/).
#   make              build every game's exe into build/  (build/jsrf.exe, build/mc3.exe)
#   make jsrf         just one of them
#   make dist T=jsrf  release zips and checksums for one game in dist/ (needs zip)
#   make clean
# From Linux/WSL this needs i686-w64-mingw32-gcc (apt install gcc-mingw-w64-i686);
# in the MSYS2 MINGW32 shell on Windows, use  make CROSS=
CROSS   ?= i686-w64-mingw32-
CC      = $(CROSS)gcc
WINDRES = $(CROSS)windres
CFLAGS  = -O2 -g -std=gnu11 -Wall -Wno-unused -fno-omit-frame-pointer -fno-strict-aliasing -m32 -Icore
# The image sits at 0x10000 with no relocations so that the XBE can be mapped
# into the reserved range right after it (see core/stubs.S and core/xbe.ld).
LDFLAGS = -Wl,--image-base=0x10000 -Wl,--disable-dynamicbase -Wl,--disable-reloc-section \
          -Wl,--large-address-aware -Wl,-T,core/xbe.ld -mwindows -static -lwinmm -lgdi32 -lopengl32

TITLES  := jsrf mc3
CORE_C  := main kernel kthread xmem mmio apu hle input dsound prof settings logwin \
           nv2a/nv2a nv2a/pfifo nv2a/pgraph \
           gl/glctx gl/surface gl/texture gl/shaders gl/draw gl/overlay gl/capture
CORE_O  := $(CORE_C:%=build/core/%.o) build/core/stubs.o
HDRS    := $(wildcard core/*.h core/*.inc core/nv2a/*.h core/gl/*.h core/gl/*.inc)

all: $(TITLES)
$(TITLES): %: build/%.exe

build/%.exe: $(CORE_O) build/titles/%/title.o build/titles/%/res.o core/xbe.ld
	$(CC) -o $@ $(CORE_O) build/titles/$*/title.o build/titles/$*/res.o $(LDFLAGS)

build/core/%.o: core/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
build/core/stubs.o: core/stubs.S
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@
build/titles/%/title.o: titles/%/title.c titles/%/symbols.inc titles/%/version.h $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ititles/$* -c $< -o $@
build/titles/%/res.o: titles/%/title.rc titles/%/version.h $(wildcard titles/%/res/*)
	@mkdir -p $(dir $@)
	$(WINDRES) -Ititles/$* -O coff $< $@

# ---- release packaging: make dist T=<title> ----
T       ?= jsrf
VERSION  = $(shell sed -n 's/^\#define TITLE_VERSION_STR *"\(.*\)"/\1/p' titles/$(T)/version.h)
PKG      = $(T)-pc-$(VERSION)
SOURCES := README.md CHANGELOG.md THIRD_PARTY_NOTICES.md LICENSE LICENSES Makefile .gitignore .gitattributes \
           .github docs core titles tools

dist: build/$(T).exe
	rm -rf dist/$(PKG)-windows dist/src && mkdir -p dist/$(PKG)-windows dist/src/$(PKG)
	cp build/$(T).exe README.md CHANGELOG.md THIRD_PARTY_NOTICES.md dist/$(PKG)-windows/
	cp titles/$(T)/*.bat dist/$(PKG)-windows/ 2>/dev/null || true
	cp LICENSE dist/$(PKG)-windows/LICENSE.txt
	tar cf - --exclude='*.o' --exclude=tools/fakexi/xinput1_4.dll $(SOURCES) | tar xf - -C dist/src/$(PKG)
	cd dist && rm -f $(PKG)-windows.zip $(PKG)-source.zip && zip -qrX $(PKG)-windows.zip $(PKG)-windows \
	    && cd src && zip -qrX ../$(PKG)-source.zip $(PKG)
	rm -rf dist/src
	cd dist && sha256sum $(PKG)-windows.zip $(PKG)-source.zip $(PKG)-windows/$(T).exe > SHA256SUMS-$(PKG).txt
	@cat dist/SHA256SUMS-$(PKG).txt

clean:
	rm -rf build dist
	$(MAKE) -C tools/fakexi clean

.PHONY: all $(TITLES) dist clean
.SECONDARY:
