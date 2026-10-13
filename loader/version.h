/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Release version (also read by jsrf.rc and the Makefile's dist target). */
#pragma once
#define JSRF_VERSION_MAJOR 0
#define JSRF_VERSION_MINOR 1
#define JSRF_VERSION_PATCH 0
#define JSRF_VERSION_STR   "0.1.0"
#define JSRF_PRODUCT       "JSRF PC"

/* The one build of the game this loader is written for: it patches code at
 * fixed addresses, so other builds would be corrupted rather than run. */
#define JSRF_XBE_SHA1      "86ffb4081ffb1a8a5da2e03d3838382ce31d7ae8"
#define JSRF_XBE_TITLE_ID  0x5345000Au
