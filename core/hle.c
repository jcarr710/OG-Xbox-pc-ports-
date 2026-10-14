/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Patch mechanism for XDK function replacement, and the generic patches
 * every title gets. */
#include "hle.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

uint32_t sym(const char *name)
{
    for (const TitleSym *s = g_title.syms; s && s->name; s++)
        if (!strcmp(s->name, name)) return s->va;
    return 0;
}

const char *title_env(const char *suffix)
{
    char n[96]; int i = 0;
    for (const char *p = g_title.stem; *p && i < 40; p++) n[i++] = (char)toupper((unsigned char)*p);
    n[i++] = '_';
    snprintf(n + i, sizeof n - i, "%s", suffix);
    return getenv(n);
}

/* Build number of an XDK library linked into the XBE ("D3D8", "DSOUND", ...), 0 if absent. */
int xbe_lib_build(const char *lib)
{
    const uint8_t *v = (const uint8_t *)g_xbe->LibraryVersions;
    for (uint32_t i = 0; v && i < g_xbe->NumLibraryVersions; i++, v += 16)
        if (!strncmp((const char *)v, lib, 8)) return *(const uint16_t *)(v + 12);
    return 0;
}

void hle_set_last_error(uint32_t err)
{
    static uint32_t fn;
    if (!fn) fn = sym("XAPILIB__SetLastError");
    if (!fn) return;
    uint32_t a[1] = { err };
    xcall((void *)fn, 1, a);
}

int hle_patch(uint32_t va, void *fn, const char *name, const char *expect, int nexpect)
{
    uint8_t *p = (uint8_t *)va;
    if (expect && memcmp(p, expect, nexpect)) {
        xlog("hle: WARNING %s @ %08x: unexpected bytes %02x %02x %02x %02x, not patched", name, va, p[0], p[1], p[2], p[3]);
        return 0;
    }
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)fn - (va + 5));
    return 1;
}

int hle_patch_sym(const char *name, void *fn, int required)
{
    uint32_t va = sym(name);
    if (!va) {
        if (required) xlog("hle: WARNING %s not found in this XBE, not replaced", name);
        return 0;
    }
    return hle_patch(va, fn, name, NULL, 0);
}

/* ---- timing ----
 * The Xbox CPU's time-stamp counter runs at 733.33 MHz and XAPI's
 * QueryPerformanceFrequency says so. QueryPerformanceCounter and the small
 * `rdtsc; ret` helpers games and libraries carry are pointed at an
 * Xbox-rate counter (kernel.c), so time measured with them is right. */
BOOL __stdcall hle_QueryPerformanceCounter(LARGE_INTEGER *);
extern char hle_rdtsc[], hle_rdtsc_lo[];

static int is_pad(uint8_t b) { return b == 0xCC || b == 0x90; }

static void patch_timing(void)
{
    uint32_t qpc = sym("XAPILIB__QueryPerformanceCounter");
    if (qpc) hle_patch(qpc, (void *)hle_QueryPerformanceCounter, "XAPI QueryPerformanceCounter", "\x8b\x4c\x24\x04\x0f\x31\x89\x01", 8);

    /* `rdtsc; ret` and `push edx; rdtsc; pop edx; ret` functions, at a
     * function start (after padding) with room for a 5-byte jmp. */
    XBE_SECTION *secs = (XBE_SECTION *)g_xbe->SectionHeadersAddress;
    int n = 0;
    for (uint32_t i = 0; i < g_xbe->NumberOfSections; i++) {
        XBE_SECTION *s = &secs[i];
        const char *nm = (const char *)s->SectionName;
        if (!(s->Flags & 4) || !strcmp(nm, "DOLBY") || !strcmp(nm, ".rdata") || !strcmp(nm, ".data")) continue;
        uint8_t *p = (uint8_t *)s->VirtualAddress, *end = p + s->SizeOfRawData - 8;
        for (p += 1; p < end; p++) {
            if (!is_pad(p[-1]) && p[-1] != 0xC3 && ((uintptr_t)p & 15)) continue;
            if (p[0] == 0x0F && p[1] == 0x31 && p[2] == 0xC3 && is_pad(p[3]) && is_pad(p[4])) {
                hle_patch((uint32_t)p, hle_rdtsc, "rdtsc helper", NULL, 0);
                xlog("hle: rdtsc helper @ %08x -> Xbox-rate counter", (uint32_t)p); n++;
            } else if (p[0] == 0x52 && p[1] == 0x0F && p[2] == 0x31 && p[3] == 0x5A && p[4] == 0xC3) {
                hle_patch((uint32_t)p, hle_rdtsc_lo, "rdtsc helper (low)", NULL, 0);
                xlog("hle: rdtsc helper (low dword) @ %08x -> Xbox-rate counter", (uint32_t)p); n++;
            }
        }
    }
    (void)n;
}

void hle_install(void)
{
    patch_timing();
    input_install();
    dsound_install();
    nv2a_hle_install();
    if (g_title.patch) g_title.patch();
}
