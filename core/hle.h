/* SPDX-License-Identifier: GPL-2.0-or-later */
/* High-level replacements for XDK library functions statically linked into
 * the game's XBE. Addresses come from the title's symbol table (title.h);
 * each patch is skipped with a warning if the symbol is missing or the
 * original bytes don't look as expected. */
#pragma once
#include "xbox.h"
#include "title.h"

void hle_install(void);
int  xbe_lib_build(const char *lib);   /* XDK build of a linked library, e.g. xbe_lib_build("D3D8") == 5849 */
void hle_set_last_error(uint32_t err);
uint32_t xcall(void *fn, int n, const uint32_t *args);

/* Overwrite the start of `va` with `jmp fn`. `expect` (optional) are the
 * original first bytes. Returns 1 if patched. */
int hle_patch(uint32_t va, void *fn, const char *name, const char *expect, int nexpect);
/* Same, by symbol name; returns 0 (quietly unless `required`) if the game
 * doesn't have the symbol. */
int hle_patch_sym(const char *name, void *fn, int required);

/* input.c */
void input_init(void);
void input_install(void);

/* input.c: the settings menu's view of the controls */
enum { MENU_UP = 1, MENU_DOWN = 2, MENU_LEFT = 4, MENU_RIGHT = 8, MENU_OK = 16, MENU_BACK = 32, MENU_TOGGLE = 64 };
int  input_menu_buttons(void);         /* held buttons (keyboard + every controller) */
void input_menu_key(int vk);           /* window thread: a key went down */
int  input_menu_events(void);          /* key presses since the last call */
void input_menu_closed(void);          /* keep game input released until all controls are let go */
extern volatile int g_menu_open;

/* dsound.c / nv2a */
void dsound_install(void);
void nv2a_hle_install(void);
void pfifo_user_write(uint32_t off, uint32_t v);   /* channel 0 USER area register write */
void __stdcall gpu_time_wait(void);                 /* wait (<=1 ms) for the GPU time to advance */
void nv2a_gpu_time_enable(void);                    /* start signalling gpu_time_wait */
