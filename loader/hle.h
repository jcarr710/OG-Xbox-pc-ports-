/* SPDX-License-Identifier: GPL-2.0-or-later */
/* High-level replacements for XDK library functions statically linked into
 * the JSRF XBE.  Addresses come from XbSymbolDatabase (NA retail XBE,
 * XDK 4134); each patch is guarded by a check of the original bytes. */
#pragma once
#include "xbox.h"

/* XAPI internals we call */
#define XAPI_SetLastError        0x00146429u
#define XAPI_GetLastError        0x00146401u

/* XPP device-type descriptors (XPP_DEVICE_TYPE) */
#define XDEVICE_TYPE_MU          0x001bc7e0u
#define XDEVICE_TYPE_GAMEPAD     0x001bc860u
#define XDEVICE_TYPE_KEYBOARD    0x001bc86cu
#define XDEVICE_TYPE_IR_DONGLE   0x001bc878u

void hle_install(void);
void hle_set_last_error(uint32_t err);
uint32_t xcall(void *fn, int n, const uint32_t *args);

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
