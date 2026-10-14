/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Midnight Club 3: DUB Edition Remix (NA Xbox release, XDK 5849). */
#include "hle.h"
#include "version.h"
#include <string.h>

static const TitleSym syms[] = {
#include "symbols.inc"
    /* found by hand (not in XbSymbolDatabase) */
    { "D3D8__DacLoadGammaRamp", 0x004189da },   /* thiscall (regs, ramp[3][256]); called by SetGammaRamp */
    { NULL, 0 }
};

/* CDevice::KickOff (0x4163c0) as in JSRF (see titles/jsrf/title.c): the
 * write-combine flush handshake on PFB_WBC is jumped over and the DMA_PUT
 * store becomes a direct call. XDK 5849 keeps the USER area pointer in the
 * device (+0x1C20); it points at RAM instead while D3D records a push buffer,
 * so the store only goes to the GPU when it is in the device window. */
void hle_kick_put_5849(void);
__asm__(
    ".globl _hle_kick_put_5849\n"
    "_hle_kick_put_5849:\n"            /* replaces: mov ecx,[esi+1C20h]; and edx,0FFFFFFFh; mov [ecx+40h],edx */
    "  movl 0x1c20(%esi), %ecx\n"
    "  andl $0x0fffffff, %edx\n"
    "  cmpl $0xfd000000, %ecx\n"
    "  jb 1f\n"
    "  pushl %eax\n  pushl %ecx\n  pushl %edx\n"
    "  pushl %edx\n"
    "  leal -0xfd7fffc0(%ecx), %eax\n"  /* ecx + 0x40 - 0xFD800000: offset in the USER area */
    "  pushl %eax\n"
    "  call _pfifo_user_write\n"
    "  addl $8, %esp\n"
    "  popl %edx\n  popl %ecx\n  popl %eax\n"
    "  ret\n"
    "1: movl %edx, 0x40(%ecx)\n"
    "  ret\n");

static void patch_kickoff(void)
{
    uint8_t *wbc = (uint8_t *)0x4163e4, *put = (uint8_t *)0x41640c;
    static const uint8_t wbc_orig[] = { 0x0f, 0xae, 0xf8, 0x8b, 0x88, 0x10, 0x04, 0x10, 0x00 };   /* sfence; mov ecx,[eax+100410h] */
    static const uint8_t put_orig[] = { 0x8b, 0x8e, 0x20, 0x1c, 0x00, 0x00, 0x81, 0xe2, 0xff, 0xff, 0xff, 0x0f, 0x89, 0x51, 0x40 };
    if (memcmp(wbc, wbc_orig, sizeof wbc_orig) || memcmp(put, put_orig, sizeof put_orig)) {
        xlog("hle: WARNING KickOff bytes differ, not patched");
        return;
    }
    wbc[0] = 0xEB; wbc[1] = 0x26;                         /* jmp 0x41640c (after the poll loop) */
    put[0] = 0xE8; *(int32_t *)(put + 1) = (int32_t)((uintptr_t)hle_kick_put_5849 - (uintptr_t)(put + 5));
    memset(put + 5, 0x90, sizeof put_orig - 5);
}

/* D3D's BlockOnTime (0x4165a0) spin loop -> gpu_time_wait (core/nv2a/nv2a.c). */
void hle_blockontime_spin_5849(void);
__asm__(
    ".globl _hle_blockontime_spin_5849\n"
    "_hle_blockontime_spin_5849:\n"       /* eax = distance to target, edx = &gpu_time, esi = current put time */
    "1: movl (%edx), %ecx\n"
    "  movl %esi, %edi\n"
    "  subl %ecx, %edi\n"
    "  cmpl %edi, %eax\n"
    "  jae 2f\n"
    "  pushl %eax\n  pushl %edx\n"
    "  call _gpu_time_wait@0\n"
    "  popl %edx\n  popl %eax\n"
    "  jmp 1b\n"
    "2: pushl $0x4166e1\n"
    "  ret\n");

static void patch_blockontime(void)
{
    uint8_t *p = (uint8_t *)0x4166d7;
    static const uint8_t orig[] = { 0x8b, 0x0a, 0x8b, 0xfe, 0x2b, 0xf9, 0x3b, 0xc7, 0x72, 0xf6 };
    if (memcmp(p, orig, sizeof orig)) { xlog("hle: WARNING BlockOnTime bytes differ, not patched"); return; }
    nv2a_gpu_time_enable();
    p[0] = 0xE9; *(int32_t *)(p + 1) = (int32_t)((uintptr_t)hle_blockontime_spin_5849 - (uintptr_t)(p + 5));
    memset(p + 5, 0x90, sizeof orig - 5);
}

static void mc3_patch(void)
{
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
    .patch    = mc3_patch,
};
