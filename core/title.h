/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What the shared runtime needs to know about the game it runs.
 *
 * The runtime (core/) is the same for every game: kernel, threads, memory,
 * the emulated GPU, the renderer, audio, input and the settings menu. Each
 * game has a small directory under titles/ whose title.c defines g_title:
 * names, the one default.xbe it supports, the addresses of the XDK library
 * functions statically linked into that XBE (found with XbSymbolDatabase,
 * see tools/gen_symbols.py) and any game-specific patches. */
#pragma once
#include <stdint.h>

typedef struct { const char *name; uint32_t va; } TitleSym;

typedef struct {
    const char *product;    /* "JSRF PC": window, log and message titles */
    const char *version;    /* "0.1.0" */
    const char *game;       /* "Jet Set Radio Future" */
    const char *stem;       /* "jsrf": jsrf.exe, jsrf.log, jsrf.ini, JSRF_* variables */
    uint32_t    title_id;   /* certificate title ID of the supported build */
    uint32_t    region;     /* certificate game region of the supported build */
    const char *xbe_sha1;   /* SHA-1 of the supported default.xbe */
    const char *release;    /* "the North American Xbox release" */
    const TitleSym *syms;   /* XDK functions and data; {NULL, 0}-terminated */
    void (*patch)(void);    /* game-specific patches, after the generic HLE */
} Title;

extern const Title g_title;

/* Address of a symbol in the game's XBE (XbSymbolDatabase naming, e.g.
 * "XAPILIB__XInputGetState"), or 0 if this game doesn't have it. */
uint32_t sym(const char *name);
/* getenv("<STEM>_<suffix>"), e.g. title_env("MULTICORE") reads JSRF_MULTICORE. */
const char *title_env(const char *suffix);
