/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Release version and identity (read by title.c, title.rc and `make dist`). */
#pragma once
#define TITLE_VERSION_MAJOR 0
#define TITLE_VERSION_MINOR 2
#define TITLE_VERSION_PATCH 0
#define TITLE_VERSION_STR   "0.2.0"
#define TITLE_PRODUCT       "JSRF PC"
#define TITLE_GAME          "Jet Set Radio Future"
#define TITLE_STEM          "jsrf"

/* The one build of the game this is written for: it patches code at fixed
 * addresses, so other builds would be corrupted rather than run. */
#define TITLE_XBE_SHA1      "86ffb4081ffb1a8a5da2e03d3838382ce31d7ae8"
#define TITLE_XBE_TITLE_ID  0x5345000Au
#define TITLE_XBE_REGION    1u
#define TITLE_RELEASE       "the North American Xbox release"
