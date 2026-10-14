/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NV2A register-level model (BAR0 at 0xFD000000, 16 MB).
 *
 * Registers are a plain backing store with side effects for the handful the
 * XDK D3D library depends on: identification, RAM size, clocks, timers,
 * interrupts, the CRTC, instance memory (PRAMIN) and the FIFO channel's
 * DMA_PUT/DMA_GET (which drive the push-buffer processor in pfifo.c). */
#include "nv2a.h"
#include "../hle.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t *g_nv_regs;                 /* 16 MB register space */
uint8_t  *g_ramin;                   /* PRAMIN window (1 MB) */
static uint8_t g_seen_r[NV_REG_SPACE / 4 / 8], g_seen_w[NV_REG_SPACE / 4 / 8];
static LARGE_INTEGER g_qpf, g_t0;
CRITICAL_SECTION g_nv_lock;

/* Integer clocks: register reads are emulated inside game code, where the
 * x87 state is the game's (see kernel.c). */
static uint64_t now_scaled(uint64_t hz)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    uint64_t d = (uint64_t)(t.QuadPart - g_t0.QuadPart), f = (uint64_t)g_qpf.QuadPart;
    return (d / f) * hz + (d % f) * hz / f;
}
static uint64_t now_ns(void) { return now_scaled(1000000000ull); }

typedef struct { uint32_t reg; const char *name; } RegName;
static const RegName g_names[] = {
    { 0x000000, "PMC_BOOT_0" }, { 0x000100, "PMC_INTR_0" }, { 0x000140, "PMC_INTR_EN_0" }, { 0x000200, "PMC_ENABLE" },
    { 0x001100, "PBUS_INTR_0" }, { 0x001140, "PBUS_INTR_EN_0" },
    { 0x002100, "PFIFO_INTR_0" }, { 0x002140, "PFIFO_INTR_EN_0" }, { 0x002210, "PFIFO_RAMHT" }, { 0x002214, "PFIFO_RAMFC" },
    { 0x002218, "PFIFO_RAMRO" }, { 0x002400, "PFIFO_RUNOUT_STATUS" }, { 0x002500, "PFIFO_CACHES" }, { 0x002504, "PFIFO_MODE" },
    { 0x002508, "PFIFO_DMA" }, { 0x003200, "PFIFO_CACHE1_PUSH0" }, { 0x003204, "PFIFO_CACHE1_PUSH1" },
    { 0x003210, "PFIFO_CACHE1_PUT" }, { 0x003214, "PFIFO_CACHE1_STATUS" }, { 0x003220, "PFIFO_CACHE1_DMA_PUSH" },
    { 0x003224, "PFIFO_CACHE1_DMA_FETCH" }, { 0x003228, "PFIFO_CACHE1_DMA_STATE" }, { 0x00322C, "PFIFO_CACHE1_DMA_INSTANCE" },
    { 0x003230, "PFIFO_CACHE1_DMA_CTL" }, { 0x003240, "PFIFO_CACHE1_DMA_PUT" }, { 0x003244, "PFIFO_CACHE1_DMA_GET" },
    { 0x003248, "PFIFO_CACHE1_REF" }, { 0x00324C, "PFIFO_CACHE1_DMA_SUBROUTINE" }, { 0x003250, "PFIFO_CACHE1_PULL0" },
    { 0x003254, "PFIFO_CACHE1_PULL1" }, { 0x003270, "PFIFO_CACHE1_GET" }, { 0x003280, "PFIFO_CACHE1_ENGINE" },
    { 0x009100, "PTIMER_INTR_0" }, { 0x009140, "PTIMER_INTR_EN_0" }, { 0x009200, "PTIMER_NUMERATOR" },
    { 0x009210, "PTIMER_DENOMINATOR" }, { 0x009400, "PTIMER_TIME_0" }, { 0x009410, "PTIMER_TIME_1" }, { 0x009420, "PTIMER_ALARM_0" },
    { 0x100200, "PFB_CFG0" }, { 0x100204, "PFB_CFG1" }, { 0x10020C, "PFB_CSTATUS" }, { 0x100410, "PFB_WBC" },
    { 0x400100, "PGRAPH_INTR" }, { 0x400108, "PGRAPH_NSOURCE" }, { 0x400140, "PGRAPH_INTR_EN" }, { 0x400144, "PGRAPH_CTX_CONTROL" },
    { 0x400148, "PGRAPH_CTX_USER" }, { 0x40014C, "PGRAPH_CTX_SWITCH1" }, { 0x400700, "PGRAPH_STATUS" },
    { 0x400704, "PGRAPH_TRAPPED_ADDR" }, { 0x400708, "PGRAPH_TRAPPED_DATA_LOW" }, { 0x400710, "PGRAPH_SURFACE" },
    { 0x40071C, "PGRAPH_INCREMENT" }, { 0x400720, "PGRAPH_FIFO" }, { 0x400750, "PGRAPH_RDI_INDEX" }, { 0x400754, "PGRAPH_RDI_DATA" },
    { 0x400780, "PGRAPH_CHANNEL_CTX_TABLE" }, { 0x400784, "PGRAPH_CHANNEL_CTX_POINTER" }, { 0x400788, "PGRAPH_CHANNEL_CTX_TRIGGER" },
    { 0x600100, "PCRTC_INTR_0" }, { 0x600140, "PCRTC_INTR_EN_0" }, { 0x600800, "PCRTC_START" }, { 0x600804, "PCRTC_CONFIG" },
    { 0x600808, "PCRTC_RASTER" },
    { 0x680500, "PRAMDAC_NVPLL_COEFF" }, { 0x680504, "PRAMDAC_MPLL_COEFF" }, { 0x680508, "PRAMDAC_VPLL_COEFF" },
    { 0x680514, "PRAMDAC_PLL_TEST_COUNTER" },
    { 0x800040, "USER0_DMA_PUT" }, { 0x800044, "USER0_DMA_GET" }, { 0x800048, "USER0_REF" },
};

const char *nv2a_reg_name(uint32_t off)
{
    static char buf[8][40]; static int bi;
    for (size_t i = 0; i < sizeof g_names / sizeof *g_names; i++) if (g_names[i].reg == off) return g_names[i].name;
    char *b = buf[bi++ & 7];
    const char *blk = off < 0x1000 ? "PMC" : off < 0x2000 ? "PBUS" : off < 0x4000 ? "PFIFO" : off < 0x8000 ? "PRMA" :
                      off < 0x9000 ? "PVIDEO" : off < 0xA000 ? "PTIMER" : off < 0x100000 ? "P?" : off < 0x101000 ? "PFB" :
                      off < 0x400000 ? "P?" : off < 0x402000 ? "PGRAPH" : off < 0x601000 ? "PCRTC" : off < 0x602000 ? "PRMCIO" :
                      off < 0x681000 ? "PRAMDAC" : off < 0x700000 ? "P?" : off < 0x800000 ? "PRAMIN" : "USER";
    snprintf(b, 40, "%s+%05x", blk, off);
    return b;
}

static int first_time(uint8_t *seen, uint32_t off)
{
    uint32_t i = off >> 2;
    if (seen[i >> 3] & (1 << (i & 7))) return 0;
    seen[i >> 3] |= 1 << (i & 7);
    return 1;
}

/* GPU core clock (from NVPLL 0x00011C01: 16.666 MHz * 28 / 1 / 2) = 233.33 MHz */
#define NV_CORE_HZ 233333333ull

static uint64_t ptimer_ticks(void)
{
    uint32_t num = R(NV_PTIMER_NUMERATOR), den = R(NV_PTIMER_DENOMINATOR);
    uint64_t t = now_scaled(NV_CORE_HZ);
    if (num && den) t = t / num * den + (t % num) * den / num;
    return t;
}

#define FRAME_NS 16666667ull            /* 60 Hz */
uint32_t nv2a_raster_line(void)
{
    return (uint32_t)((now_ns() % FRAME_NS) * 525 / FRAME_NS);
}

/* Gamma ramp. D3D loads it into the VGA DAC (PRMDIO 0x3C8/0x3C9) a byte at a
 * time, 769 device traps per update and many updates per second during
 * fades. The loader function is replaced so the whole ramp is taken at once;
 * the renderer applies it when presenting. */
uint8_t g_gamma[3][256];
volatile LONG g_gamma_gen;
static void __attribute__((thiscall)) hle_dac_load_ramp(void *dev, const uint8_t *ramp)
{
    if (!memcmp(g_gamma, ramp, sizeof g_gamma)) return;
    memcpy(g_gamma, ramp, sizeof g_gamma);
    LONG n = InterlockedIncrement(&g_gamma_gen);
    if (n <= 8 || (n % 500) == 0)
        xlog("gamma ramp #%ld: r %u/%u/%u g %u/%u/%u b %u/%u/%u (at 0/128/255)", n, ramp[0], ramp[128], ramp[255],
             ramp[256], ramp[384], ramp[511], ramp[512], ramp[640], ramp[767]);
}

/* D3D's BlockOnTime spins on the GPU-time dword in RAM when the GPU is
 * "nearly" there. Here the GPU is usually waiting for a vblank flip, so that
 * spin burns the title's core for most of every frame. Titles patch the loop
 * (titles/<name>/title.c) to sleep in gpu_time_wait instead, which returns
 * when a semaphore release writes the time (or after 1 ms as a backstop). */
static HANDLE g_gpu_time_ev;
void nv2a_gpu_time_advanced(void) { if (g_gpu_time_ev) SetEvent(g_gpu_time_ev); }
void nv2a_gpu_time_enable(void) { if (!g_gpu_time_ev) g_gpu_time_ev = CreateEventA(NULL, FALSE, FALSE, NULL); }
void __stdcall __attribute__((used)) gpu_time_wait(void) { WaitForSingleObject(g_gpu_time_ev, 1); }

void nv2a_hle_install(void)
{
    extern int g_swm_packed;
    int d3d = xbe_lib_build("D3D8");
    g_swm_packed = d3d > 4134;
    xlog("nv2a: D3D8 build %d (%s software methods)", d3d, g_swm_packed ? "packed" : "register");
    for (int i = 0; i < 256; i++) g_gamma[0][i] = g_gamma[1][i] = g_gamma[2][i] = (uint8_t)i;
    /* D3D's internal DAC loader (thiscall: device register block, ramp) isn't
     * in XbSymbolDatabase; titles name it in their symbol table. */
    uint32_t dac = sym("D3D8__DacLoadGammaRamp");
    if (dac) hle_patch(dac, hle_dac_load_ramp, "D3D DAC gamma load", NULL, 0);
    else xlog("hle: no D3D gamma loader for this title; gamma ramp loads go through the DAC registers");
}

void nv2a_start_irq_thread(void);
void nv2a_init(void)
{
    InitializeCriticalSection(&g_nv_lock);
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_t0);
    g_nv_regs = VirtualAlloc(NULL, NV_REG_SPACE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_nv_regs) xfatal("nv2a: register space alloc failed");
    g_ramin = (uint8_t *)g_nv_regs + NV_PRAMIN;

    R(NV_PMC_BOOT_0) = 0x02A000A1;
    R(NV_PMC_ENABLE) = 0x13111111;
    /* PCI config mirror */
    R(0x001800) = 0x02A010DE; R(0x001804) = 0x02B00007; R(0x001808) = 0x030000A1;
    R(0x00180C) = 0x0000F800; R(0x001810) = 0xFD000000; R(0x001814) = 0xF0000008;
    R(0x00182C) = 0x00000000; R(0x00183C) = 0x01050103;
    R(NV_PFB_CFG0) = 0x03070103;
    R(NV_PFB_CFG1) = 0x11448000;
    R(NV_PFB_CSTATUS) = 0x04000000;
    R(NV_PRAMDAC_NVPLL_COEFF) = 0x00011C01;
    R(NV_PRAMDAC_MPLL_COEFF) = 0x00007702;
    R(NV_PRAMDAC_VPLL_COEFF) = 0x0003C20D;
    R(NV_PFIFO_RUNOUT_STATUS) = 0x10;            /* runout empty */
    R(NV_PFIFO_CACHE1_STATUS) = 0x10;            /* cache empty */
    R(NV_PFIFO_CACHE1_DMA_PUSH) = 0x100;         /* buffer empty */
    pfifo_init();
    nv2a_start_irq_thread();
    xlog("nv2a: registers at %p, PRAMIN %p", g_nv_regs, g_ramin);
}

uint32_t nv2a_mmio_read(uint32_t off, int size)
{
    uint32_t a = off & ~3u, v;
    switch (a) {
    case NV_PTIMER_TIME_0: v = (uint32_t)((ptimer_ticks() & 0x7FFFFFF) << 5); break;
    case NV_PTIMER_TIME_1: v = (uint32_t)((ptimer_ticks() >> 27) & 0x1FFFFFFF); break;
    case NV_PCRTC_RASTER:  v = nv2a_raster_line(); break;
    case NV_PRAMDAC_PLL_TEST_COUNTER: v = 0x05A80000 | (R(a) & 0xFFFF); break;
    case NV_PGRAPH_STATUS: v = pfifo_busy() ? 1 : 0; break;
    case 0x100410: v = R(a) & ~0x00010000u; break;     /* PFB_WBC: flush never pending */
    default:
        if (a >= NV_USER && a < NV_USER + 0x200000) v = pfifo_user_read(a - NV_USER);
        else v = R(a);
        break;
    }
    if (g_nv_trace >= 2 || (g_nv_trace && first_time(g_seen_r, a) && (a < NV_PRAMIN || a >= NV_USER)))
        xlog("nv2a R%d %-28s = %08x  eip=%08x", size * 8, nv2a_reg_name(a), v, mmio_current_eip());
    {   /* spin detection: same register polled from the same place */
        static uint32_t last_a, last_eip, count;
        uint32_t eip = mmio_current_eip();
        if (a == last_a && eip == last_eip) {
            if (++count == 100000) xlog("nv2a: SPIN polling %s (=%08x) at eip=%08x", nv2a_reg_name(a), v, eip);
        } else { last_a = a; last_eip = eip; count = 0; }
    }
    if (size == 4) return v;
    v >>= (off & 3) * 8;
    return size == 1 ? (v & 0xFF) : (v & 0xFFFF);
}

void nv2a_mmio_write(uint32_t off, int size, uint32_t val)
{
    uint32_t a = off & ~3u;
    if (size != 4) {
        uint32_t sh = (off & 3) * 8, m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        val = (R(a) & ~m) | ((val << sh) & m);
    }
    if (g_nv_trace >= 2 || (g_nv_trace && first_time(g_seen_w, a) && (a < NV_PRAMIN || a >= NV_USER)))
        xlog("nv2a W%d %-28s <- %08x  eip=%08x", size * 8, nv2a_reg_name(a), val, mmio_current_eip());

    switch (a) {
    /* interrupt status registers are write-1-to-clear */
    case NV_PGRAPH_INTR:
        R(a) &= ~val;
        nv2a_update_irq();
        return;
    case NV_PGRAPH_FIFO:
        /* D3D's ISR disables PGRAPH FIFO access while it services a trapped
         * method, and re-enables it when done.  The trapped software method
         * reads its parameters from PGRAPH registers, so the GPU must not run
         * ahead until access is restored. */
        R(a) = val;
        if (val & 1) { g_wait_nop = 0; nv2a_kick(); }
        return;
    case NV_PMC_INTR_0: case NV_PBUS_INTR_0: case NV_PFIFO_INTR_0: case NV_PTIMER_INTR_0:
    case NV_PCRTC_INTR_0:
        R(a) &= ~val;
        nv2a_update_irq();
        return;
    case NV_PGRAPH_INCREMENT:
        if (val & 2) {  /* INCREMENT_READ_3D: a flip completed */
            uint32_t s = R(NV_PGRAPH_SURFACE), mod = SURF_MODULO(s) ? SURF_MODULO(s) : 1;
            uint32_t rd = (SURF_READ(s) + 1) % mod;
            R(NV_PGRAPH_SURFACE) = (s & ~(7u << 24)) | (rd << 24);
            nv2a_kick();
        }
        return;
    case NV_PMC_INTR_EN_0: case NV_PCRTC_INTR_EN_0: case NV_PGRAPH_INTR_EN: case NV_PFIFO_INTR_EN_0:
        R(a) = val;
        nv2a_update_irq();
        return;
    case NV_PCRTC_START:
        R(a) = val;
        nv2a_display_start_changed(val);
        return;
    }
    if (a >= NV_USER && a < NV_USER + 0x200000) { pfifo_user_write(a - NV_USER, val); return; }
    if (a == 0x003244 || a == 0x003240) {
        static int n; if (n++ < 20) xlog("nv2a: CPU wrote %s %08x -> %08x eip=%08x", nv2a_reg_name(a), R(a), val, mmio_current_eip());
    }
    R(a) = val;
}

/* PMC_INTR_0 is the OR of the per-block pending&enabled interrupts. */
void nv2a_update_irq(void)
{
    uint32_t pmc = R(NV_PMC_INTR_0) & ~(NV_PMC_INTR_0_PFIFO | NV_PMC_INTR_0_PGRAPH | NV_PMC_INTR_0_PCRTC | NV_PMC_INTR_0_PTIMER);
    if (R(NV_PFIFO_INTR_0) & R(NV_PFIFO_INTR_EN_0)) pmc |= NV_PMC_INTR_0_PFIFO;
    if (R(NV_PGRAPH_INTR) & R(NV_PGRAPH_INTR_EN)) pmc |= NV_PMC_INTR_0_PGRAPH;
    if (R(NV_PCRTC_INTR_0) & R(NV_PCRTC_INTR_EN_0)) pmc |= NV_PMC_INTR_0_PCRTC;
    if (R(NV_PTIMER_INTR_0) & R(NV_PTIMER_INTR_EN_0)) pmc |= NV_PMC_INTR_0_PTIMER;
    R(NV_PMC_INTR_0) = pmc;
    if (pmc && (R(NV_PMC_INTR_EN_0) & 1)) nv2a_signal_irq();
}

int nv2a_irq_pending(void)
{
    return (R(NV_PMC_INTR_0) != 0) && (R(NV_PMC_INTR_EN_0) & 1);
}

/* ------------------------------------------------------------------ */
/* Interrupt / vblank thread                                           */
/* ------------------------------------------------------------------ */
static HANDLE g_irq_ev;
volatile uint32_t g_vblank_count;
void nv2a_signal_irq(void) { if (g_irq_ev) SetEvent(g_irq_ev); }

void gpu_request_present(void);

static void __stdcall irq_thread(void *arg)
{
    uint64_t next_vblank = now_ns() + FRAME_NS;
    for (;;) {
        /* Round the wait up: this thread shares the title's core, so it must
         * not spin for the last fraction of a millisecond before a vblank. */
        uint64_t now = now_ns();
        DWORD wait = now >= next_vblank ? 0 : (DWORD)((next_vblank - now) / 1000000) + 1;
        WaitForSingleObject(g_irq_ev, wait);
        now = now_ns();
        if (now >= next_vblank) {
            next_vblank += FRAME_NS;
            if (now > next_vblank + 100000000ull) next_vblank = now + FRAME_NS; /* fell behind */
            g_vblank_count++;
            R(NV_PCRTC_INTR_0) |= NV_PCRTC_INTR_0_VBLANK;
            gpu_request_present();
        }
        nv2a_update_irq();
        if (nv2a_irq_pending()) {
            kernel_raise_interrupt(3);
            nv2a_update_irq();
            if (nv2a_irq_pending()) Sleep(0);  /* still pending (new event); retry soon */
        }
    }
}

void nv2a_start_irq_thread(void)
{
    g_irq_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    XThread *t = xthread_create(irq_thread, NULL, NULL, 1 << 20, 0, FALSE);
    xthread_pin_game(t);   /* runs the title's ISRs */
    SetThreadPriority(t->host_handle, THREAD_PRIORITY_TIME_CRITICAL);
}
