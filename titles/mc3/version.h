/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Release version and identity (read by title.c, title.rc and `make dist`). */
#pragma once
#define TITLE_VERSION_MAJOR 0
#define TITLE_VERSION_MINOR 1
#define TITLE_VERSION_PATCH 0
#define TITLE_VERSION_STR   "0.1.0"
#define TITLE_PRODUCT       "MC3 PC"
#define TITLE_GAME          "Midnight Club 3: DUB Edition Remix"
#define TITLE_STEM          "mc3"

/* The one build of the game this is written for: it patches code at fixed
 * addresses, so other builds would be corrupted rather than run. */
#define TITLE_XBE_SHA1      "5bca1cf092edaadf876e8f80129900407a4a07b8"
#define TITLE_XBE_TITLE_ID  0x54540079u
#define TITLE_XBE_REGION    1u
#define TITLE_RELEASE       "the North American Xbox release of DUB Edition Remix"
