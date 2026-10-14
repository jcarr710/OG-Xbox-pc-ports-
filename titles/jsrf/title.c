/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Jet Set Radio Future (NA Xbox release, XDK 4134). */
#include "hle.h"
#include "version.h"
#include <string.h>

static const TitleSym syms[] = {
#include "symbols.inc"
    /* found by hand (not in XbSymbolDatabase for XDK 4134) */
    { "D3D8__DacLoadGammaRamp",         0x0018e584 },   /* thiscall (regs, ramp[3][256]) */
    { "DSOUND__DirectSoundUseFullHRTF", 0x0019e523 },
    { NULL, 0 }
};

/* CDevice::KickOff (0x1912a0) runs a few dozen times per frame and costs
 * four device traps each: a write-combine flush handshake on PFB_WBC (three
 * accesses; meaningless here, RAM is coherent) and the DMA_PUT store. The
 * handshake is jumped over and the store becomes a direct call. */
void hle_kick_put(void);
__asm__(
    ".globl _hle_kick_put\n"
    "_hle_kick_put:\n"                 /* replaces: mov edx,ecx; and edx,03ffffffh; mov [eax+40h],edx */
    "  pushl %eax\n  pushl %ecx\n"
    "  movl %ecx, %edx\n  andl $0x3ffffff, %edx\n"
    "  pushl %edx\n  pushl $0x40\n  call _pfifo_user_write\n  addl $8, %esp\n"
    "  popl %ecx\n  popl %eax\n"
    "  movl %ecx, %edx\n  andl $0x3ffffff, %edx\n"
    "  ret\n");

static void patch_kickoff(void)
{
    uint8_t *wbc = (uint8_t *)0x1912c6, *put = (uint8_t *)0x1912f6;
    static const uint8_t wbc_orig[] = { 0x0f, 0xae, 0xf8, 0x8b, 0x90, 0x10, 0x04, 0x10, 0x00 };   /* sfence; mov edx,[eax+100410h] */
    static const uint8_t put_orig[] = { 0x8b, 0xd1, 0x81, 0xe2, 0xff, 0xff, 0xff, 0x03, 0x89, 0x50, 0x40 };
    if (memcmp(wbc, wbc_orig, sizeof wbc_orig) || memcmp(put, put_orig, sizeof put_orig)) {
        xlog("hle: WARNING KickOff bytes differ, not patched");
        return;
    }
    wbc[0] = 0xEB; wbc[1] = 0x24;                         /* jmp 0x1912ec (after the poll loop) */
    put[0] = 0xE8; *(int32_t *)(put + 1) = (int32_t)((uintptr_t)hle_kick_put - (uintptr_t)(put + 5));
    memset(put + 5, 0x90, sizeof put_orig - 5);
}

/* D3D's BlockOnTime (0x191440) spin loop -> gpu_time_wait (see core/nv2a/nv2a.c). */
void hle_blockontime_spin(void);
__asm__(
    ".globl _hle_blockontime_spin\n"
    "_hle_blockontime_spin:\n"            /* eax = distance to target, edx = &gpu_time, edi = current put time */
    "1: movl (%edx), %ecx\n"
    "  movl %edi, %esi\n"
    "  subl %ecx, %esi\n"
    "  cmpl %esi, %eax\n"
    "  jae 2f\n"
    "  pushl %eax\n  pushl %edx\n"
    "  call _gpu_time_wait@0\n"
    "  popl %edx\n  popl %eax\n"
    "  jmp 1b\n"
    "2: pushl $0x1914fa\n"
    "  ret\n");

static void patch_blockontime(void)
{
    uint8_t *p = (uint8_t *)0x1914f0;
    static const uint8_t orig[] = { 0x8b, 0x0a, 0x8b, 0xf7, 0x2b, 0xf1, 0x3b, 0xc6, 0x72, 0xf6 };
    if (memcmp(p, orig, sizeof orig)) { xlog("hle: WARNING BlockOnTime bytes differ, not patched"); return; }
    nv2a_gpu_time_enable();
    p[0] = 0xE9; *(int32_t *)(p + 1) = (int32_t)((uintptr_t)hle_blockontime_spin - (uintptr_t)(p + 5));
    memset(p + 5, 0x90, sizeof orig - 5);
}

extern char hle_rdtsc[];
static void jsrf_patch(void)
{
    /* the game's own `rdtsc; ret` helper (followed directly by code, so the
     * generic scan leaves it alone) -> Xbox-rate counter */
    if (!memcmp((void *)0x15fa20, "\x0f\x31\xc3\xeb\x0b", 5)) {
        hle_patch(0x15fa20, hle_rdtsc, "game rdtsc helper", NULL, 0);
        xlog("patched game rdtsc helper @ 0015fa20");
    } else xlog("WARNING: patch 'game rdtsc helper' signature mismatch, skipped");
    if (!title_env("NO_KICK_PATCH")) patch_kickoff();
    if (!title_env("SPIN_BLOCKONTIME")) patch_blockontime();
}

const Title g_title = {
    .product  = TITLE_PRODUCT,
    .version  = TITLE_VERSION_STR,
    .game     = TITLE_GAME,
    .stem     = TITLE_STEM,
    .title_id = TITLE_XBE_TITLE_ID,
    .region   = TITLE_XBE_REGION,
    .xbe_sha1 = TITLE_XBE_SHA1,
    .release  = TITLE_RELEASE,
    .syms     = syms,
    .patch    = jsrf_patch,
};
