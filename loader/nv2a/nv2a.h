/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../xbox.h"
#include "../xmem.h"
#include "../mmio.h"

#define NV_REG_SPACE 0x01000000u
extern uint32_t *g_nv_regs;
extern uint8_t  *g_ramin;
extern CRITICAL_SECTION g_nv_lock;
extern int g_nv_trace;          /* 0 off, 1 first access per register, 2 everything */
#define R(off) g_nv_regs[(off) >> 2]

/* PMC */
#define NV_PMC_BOOT_0           0x000000
#define NV_PMC_INTR_0           0x000100
#define   NV_PMC_INTR_0_PFIFO     (1u << 8)
#define   NV_PMC_INTR_0_PGRAPH    (1u << 12)
#define   NV_PMC_INTR_0_PTIMER    (1u << 20)
#define   NV_PMC_INTR_0_PCRTC     (1u << 24)
#define NV_PMC_INTR_EN_0        0x000140
#define NV_PMC_ENABLE           0x000200
/* PBUS */
#define NV_PBUS_INTR_0          0x001100
/* PFIFO */
#define NV_PFIFO_INTR_0         0x002100
#define NV_PFIFO_INTR_EN_0      0x002140
#define NV_PFIFO_RAMHT          0x002210
#define NV_PFIFO_RAMFC          0x002214
#define NV_PFIFO_RUNOUT_STATUS  0x002400
#define NV_PFIFO_CACHE1_PUSH1   0x003204
#define NV_PFIFO_CACHE1_STATUS  0x003214
#define NV_PFIFO_CACHE1_DMA_PUSH 0x003220
#define NV_PFIFO_CACHE1_DMA_STATE 0x003228
#define NV_PFIFO_CACHE1_DMA_INSTANCE 0x00322C
#define NV_PFIFO_CACHE1_DMA_PUT 0x003240
#define NV_PFIFO_CACHE1_DMA_GET 0x003244
#define NV_PFIFO_CACHE1_REF     0x003248
#define NV_PFIFO_CACHE1_DMA_SUBROUTINE 0x00324C
/* PTIMER */
#define NV_PTIMER_INTR_0        0x009100
#define NV_PTIMER_INTR_EN_0     0x009140
#define NV_PTIMER_NUMERATOR     0x009200
#define NV_PTIMER_DENOMINATOR   0x009210
#define NV_PTIMER_TIME_0        0x009400
#define NV_PTIMER_TIME_1        0x009410
#define NV_PTIMER_ALARM_0       0x009420
/* PFB */
#define NV_PFB_CFG0             0x100200
#define NV_PFB_CFG1             0x100204
#define NV_PFB_CSTATUS          0x10020C
/* PGRAPH */
#define NV_PGRAPH_INTR          0x400100
#define   NV_PGRAPH_INTR_NOTIFY   (1u << 0)
#define   NV_PGRAPH_INTR_ERROR    (1u << 20)
#define NV_PGRAPH_NSOURCE       0x400108
#define NV_PGRAPH_INTR_EN       0x400140
#define NV_PGRAPH_STATUS        0x400700
#define NV_PGRAPH_TRAPPED_ADDR  0x400704
#define NV_PGRAPH_TRAPPED_DATA_LOW 0x400708
#define NV_PGRAPH_INCREMENT     0x40071C
#define NV_PGRAPH_FIFO          0x400720
/* PCRTC */
#define NV_PCRTC_INTR_0         0x600100
#define   NV_PCRTC_INTR_0_VBLANK  (1u << 0)
#define NV_PCRTC_INTR_EN_0      0x600140
#define NV_PCRTC_START          0x600800
#define NV_PCRTC_CONFIG         0x600804
#define NV_PCRTC_RASTER         0x600808
/* PRAMDAC */
#define NV_PRAMDAC_NVPLL_COEFF  0x680500
#define NV_PRAMDAC_MPLL_COEFF   0x680504
#define NV_PRAMDAC_VPLL_COEFF   0x680508
#define NV_PRAMDAC_PLL_TEST_COUNTER 0x680514
/* windows */
#define NV_PRAMIN               0x700000
#define NV_PRAMIN_SIZE          0x100000
#define NV_USER                 0x800000

const char *nv2a_reg_name(uint32_t off);
void nv2a_update_irq(void);
int  nv2a_irq_pending(void);
uint32_t nv2a_raster_line(void);
void nv2a_display_start_changed(uint32_t pa);

/* pfifo.c */
void     pfifo_init(void);
uint32_t pfifo_user_read(uint32_t off);
void     pfifo_user_write(uint32_t off, uint32_t val);
int      pfifo_busy(void);
int      kernel_raise_interrupt(ULONG level);

/* DMA objects in instance memory */
typedef struct { uint32_t cls, target, base, limit; } NvDma;
NvDma nv_dma_load(uint32_t inst_addr);
void *nv_dma_ptr(uint32_t inst_addr, uint32_t offset, uint32_t len);

/* GPU stall states (pgraph sets, interrupt handlers clear) */
extern volatile int g_wait_flip, g_wait_nop;
extern volatile uint32_t g_vblank_count;
void nv2a_kick(void);                  /* wake the GPU thread */
void nv2a_gpu_time_advanced(void);     /* a semaphore release moved D3D's GPU time */
void nv2a_signal_irq(void);            /* wake the interrupt thread */
#define NV_PGRAPH_SURFACE       0x400710
#define NV_PGRAPH_SEMAPHOREOFFSET 0x4019A0
#define SURF_READ(s)   (((s) >> 24) & 7)
#define SURF_WRITE(s)  (((s) >> 20) & 7)
#define SURF_MODULO(s) (((s) >> 28) & 7)

/* Display gamma ramp (red, green, blue tables), bumped generation on change */
extern uint8_t g_gamma[3][256];
extern volatile LONG g_gamma_gen;

/* Performance counters, reported to the log every few seconds (--perf
 * reports every second). Times are QueryPerformanceCounter ticks. */
typedef struct {
    uint64_t exec, draw, hash, upload, present;
    uint64_t hash_bytes, upload_bytes;
    uint32_t draws, uploads, flips;
} NvPerf;
extern NvPerf g_perf;
extern volatile LONG g_mmio_traps;
extern int g_perf_log;
static inline uint64_t perf_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
void perf_report(void);                /* GPU thread, once per vblank */

/* pgraph.c */
void pgraph_init(void);
void pgraph_bind_object(int subch, uint32_t handle, uint32_t inst, uint32_t cls);
void pgraph_method(int subch, uint32_t method, uint32_t data, uint32_t *rest, int nrest, int *consumed);
