/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MCPX APU registers (0xFE800000).
 *   0x01000  front end / interrupt registers
 *   0x02000  setup engine (SE)
 *   0x20000  voice processor PIO: DirectSound submits FE methods here
 *   0x30000  GP DSP (memories + reset at 0x3FFFC)
 *   0x50000  EP DSP (AC3 encoder)
 * Stage 1: the chip behaves as present, idle and ready; audio output is a
 * later stage (methods written to the PIO are where voices get programmed). */
#include "xbox.h"
#include "mmio.h"

static uint32_t g_apu_regs[0x80000 / 4];
static uint8_t g_seen[0x80000 / 4 / 8];
int g_apu_trace;

static int first(uint32_t off)
{
    uint32_t i = off >> 2;
    if (g_seen[i >> 3] & (1 << (i & 7))) return 0;
    g_seen[i >> 3] |= 1 << (i & 7);
    return 1;
}

uint32_t apu_mmio_read(uint32_t off, int size)
{
    uint32_t a = off & 0x7FFFC, v;
    switch (a) {
    case 0x20010: v = 0x80; break;            /* NV1BA0_PIO_FREE: queue always empty */
    default: v = g_apu_regs[a >> 2]; break;
    }
    if (g_apu_trace > 1 || (g_apu_trace && first(a)))
        xlog("apu: R%d +%05x = %08x eip=%08x", size * 8, off, v, mmio_current_eip());
    {   static uint32_t la, le, n; uint32_t e = mmio_current_eip();
        if (a == la && e == le) { if (++n == 100000) xlog("apu: SPIN polling +%05x (=%08x) at %08x", a, v, e); }
        else { la = a; le = e; n = 0; } }
    if (size == 4) return v;
    v >>= (off & 3) * 8;
    return size == 1 ? (v & 0xFF) : (v & 0xFFFF);
}

void apu_mmio_write(uint32_t off, int size, uint32_t val)
{
    uint32_t a = off & 0x7FFFC;
    if (g_apu_trace > 1 || (g_apu_trace && first(a)))
        xlog("apu: W%d +%05x <- %08x eip=%08x", size * 8, off, val, mmio_current_eip());
    g_apu_regs[a >> 2] = val;
}

/* AC97 controller (0xFEC00000): codec mixer regs at 0x000, bus master at 0x100.
 * Report both codecs ready; DMA engines idle. */
static uint32_t g_ac97[0x1000 / 4];
uint32_t ac97_mmio_read(uint32_t off, int size)
{
    uint32_t a = off & 0xFFC, v;
    if (a == 0x130) v = 0x00000300;                 /* GLOB_STA: primary+secondary codec ready */
    else if (a == 0x07C) v = 0x83847650;            /* vendor id (SigmaTel-like) */
    else v = g_ac97[a >> 2];
    if (g_apu_trace && first(0x7F000 + a)) xlog("ac97: R%d +%03x = %08x eip=%08x", size * 8, off, v, mmio_current_eip());
    if (size == 4) return v;
    v >>= (off & 3) * 8;
    return size == 1 ? (v & 0xFF) : (v & 0xFFFF);
}
void ac97_mmio_write(uint32_t off, int size, uint32_t val)
{
    uint32_t a = off & 0xFFC;
    if (g_apu_trace && first(0x7F000 + a)) xlog("ac97: W%d +%03x <- %08x eip=%08x", size * 8, off, val, mmio_current_eip());
    if (size == 4) g_ac97[a >> 2] = val;
    else {
        uint32_t sh = (off & 3) * 8, m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        g_ac97[a >> 2] = (g_ac97[a >> 2] & ~m) | ((val << sh) & m);
    }
    /* bus-master channels at 0x100/0x110/0x120/0x170: CR is byte +0x0B.
     * Reset (RR, bit 1) completes immediately: clear CIV/LVI/PICB, SR = DCH. */
    if (a >= 0x100 && a < 0x180 && (a & 0xF) == 0x8) {
        uint32_t d = g_ac97[a >> 2];
        if (d & (2u << 24)) {
            g_ac97[(a - 8) >> 2] = 0;                    /* BDBAR stays? cleared on reset */
            g_ac97[(a - 4) >> 2] = 0x00010000;           /* CIV=0 LVI=0 SR=DCH */
            g_ac97[a >> 2] = 0;                          /* PICB=0 PIV=0 CR=0 */
        }
    }
}
