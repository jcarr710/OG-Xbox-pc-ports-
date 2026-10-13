# Stand-in xinput1_4.dll for tests under Wine (see fake_xinput.c):
#   make            then run with WINEDLLOVERRIDES=xinput1_4=n and the DLL next to jsrf.exe
CROSS ?= i686-w64-mingw32-
xinput1_4.dll: fake_xinput.c fake_xinput.def
	$(CROSS)gcc -O2 -Wall -shared -static-libgcc -o $@ fake_xinput.c fake_xinput.def
clean:
	rm -f xinput1_4.dll
.PHONY: clean
