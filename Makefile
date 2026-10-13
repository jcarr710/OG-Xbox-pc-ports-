# SPDX-License-Identifier: GPL-2.0-or-later
#   make        build loader/jsrf.exe
#   make dist   release zips and checksums in dist/ (needs zip)
#   make clean
# From Linux/WSL this needs i686-w64-mingw32-gcc (apt install gcc-mingw-w64-i686);
# in the MSYS2 MINGW32 shell on Windows, use  make CROSS=
VERSION := $(shell sed -n 's/^\#define JSRF_VERSION_STR *"\(.*\)"/\1/p' loader/version.h)
NAME    := jsrf-pc-$(VERSION)
SOURCES := README.md CHANGELOG.md THIRD_PARTY_NOTICES.md LICENSE LICENSES Makefile .gitignore .gitattributes \
           .github docs loader tools

all:
	$(MAKE) -C loader

dist: all
	rm -rf dist && mkdir -p dist/$(NAME)-windows dist/src/$(NAME)
	cp loader/jsrf.exe loader/jsrf_profile.bat README.md CHANGELOG.md THIRD_PARTY_NOTICES.md dist/$(NAME)-windows/
	cp LICENSE dist/$(NAME)-windows/LICENSE.txt
	tar cf - --exclude='*.o' --exclude=loader/jsrf.exe --exclude=tools/fakexi/xinput1_4.dll $(SOURCES) \
	    | tar xf - -C dist/src/$(NAME)
	cd dist && zip -qrX $(NAME)-windows.zip $(NAME)-windows && cd src && zip -qrX ../$(NAME)-source.zip $(NAME)
	rm -rf dist/src
	cd dist && sha256sum $(NAME)-windows.zip $(NAME)-source.zip $(NAME)-windows/jsrf.exe > SHA256SUMS.txt
	@cat dist/SHA256SUMS.txt

clean:
	$(MAKE) -C loader clean
	$(MAKE) -C tools/fakexi clean
	rm -rf dist build

.PHONY: all dist clean
