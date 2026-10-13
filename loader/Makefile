# jsrf.exe, a 32-bit Windows program built with MinGW-w64.
#   Linux / WSL:   make            (needs i686-w64-mingw32-gcc: apt install gcc-mingw-w64-i686)
#   Windows:       make CROSS=     (MSYS2 MINGW32 shell: pacman -S mingw-w64-i686-gcc make)
CROSS   ?= i686-w64-mingw32-
CC      = $(CROSS)gcc
WINDRES = $(CROSS)windres
CFLAGS  = -O2 -g -std=gnu11 -Wall -Wno-unused -fno-omit-frame-pointer -fno-strict-aliasing -m32 -I.
# The image sits at 0x10000 with no relocations so that the XBE can be mapped
# into the reserved range right after it (see stubs.S and xbe.ld).
LDFLAGS = -Wl,--image-base=0x10000 -Wl,--disable-dynamicbase -Wl,--disable-reloc-section \
          -Wl,--large-address-aware -Wl,-T,xbe.ld -mwindows -static -lwinmm -lgdi32 -lopengl32
OBJS    = main.o kernel.o kthread.o xmem.o mmio.o apu.o hle.o input.o dsound.o stubs.o prof.o settings.o logwin.o \
          nv2a/nv2a.o nv2a/pfifo.o nv2a/pgraph.o \
          gl/glctx.o gl/surface.o gl/texture.o gl/shaders.o gl/draw.o gl/overlay.o \
          jsrf_res.o
HDRS    = xbox.h xmem.h mmio.h hle.h settings.h version.h nv2a/nv2a.h nv2a/kelvin.h nv2a/nv097.h nv2a/methods_gen.h \
          gl/gl.h gl/glfuncs.inc

jsrf.exe: $(OBJS) xbe.ld
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

kernel.o: kernel.c kfile.inc $(HDRS)
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.S
	$(CC) -m32 -c $< -o $@
jsrf_res.o: jsrf.rc version.h res/jsrf.ico res/jsrf.manifest
	$(WINDRES) -I. -O coff jsrf.rc $@

clean:
	rm -f *.o nv2a/*.o gl/*.o jsrf.exe

.PHONY: clean
