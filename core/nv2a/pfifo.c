/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PFIFO: the DMA pusher for channel 0.
 *
 * D3D writes push-buffer commands into a ring in RAM and stores the new end
 * offset into USER DMA_PUT (0xFD800040).  Like the real NV2A, command
 * processing is split in two:
 *
 *   fetch    — the DMA pusher.  Runs as soon as PUT moves (on the writing
 *              thread), parses method headers, follows JUMP/CALL/RETURN and
 *              copies every method + data into a private command queue, then
 *              advances DMA_GET.  DMA_GET is therefore the *fetch* position,
 *              exactly as on hardware, so D3D may reuse ring space as soon as
 *              it has been fetched.
 *   execute  — PGRAPH.  The GPU thread drains the queue at its own pace and
 *              may stall (software-method traps, flip stalls).  Completion is
 *              reported through semaphores/fences written during execution.
 *
 * With a slow renderer the GPU can lag far behind; fetching eagerly keeps D3D's
 * ring bookkeeping (which assumes the GPU fetches promptly) consistent. */
#include "nv2a.h"
#include <string.h>
#include <stdio.h>

static HANDLE g_kick_ev;
static uint32_t g_obj_class[8], g_obj_inst[8];
uint64_t g_pb_words;

int g_nv_trace;
int g_pb_trace;

/* ------------------------------------------------------------------ */
/* Command queue: chunks of [header][data...] using the NV method header  */
/* encoding (count in bits 28:18, subch 15:13, method 12:2, bit 30 = NI).  */
/* ------------------------------------------------------------------ */
#define CQ_WORDS (1u << 20)                 /* 4 MB */
/* How far fetching may run ahead of execution. The real NV2A fetches only a
 * little ahead of PGRAPH, so D3D's push-buffer ring (512 KB in JSRF) is what
 * keeps the CPU from getting far ahead of the GPU: D3D waits for DMA_GET
 * before reusing ring space. With eager fetching that back-pressure has to
 * come from here; uncapped, the CPU ran tens of megabytes (seconds of
 * frames) ahead whenever the renderer stalled, e.g. compiling shaders. */
#define CQ_CAP   (96u << 10)                /* words (384 KB) */
static uint32_t *g_cq;
static volatile uint32_t g_cq_w, g_cq_r;    /* free-running word counters */
static CRITICAL_SECTION g_fetch_lock;
static volatile LONG g_executing;

/* fetcher state (under g_fetch_lock) */
static uint32_t f_mthd, f_subch, f_mcnt, f_noninc;
static uint32_t f_subr_ret; static int f_subr_active;

/* executor state (GPU thread only) */
static uint32_t e_mthd, e_subch, e_cnt, e_noninc;

/* ------------------------------------------------------------------ */
/* Diagnostics: ring buffer of recent fetcher events                     */
/* ------------------------------------------------------------------ */
static struct { uint32_t get, word, kind, put; } g_pbhist[64]; static unsigned g_pbh;
static void pb_hist(uint32_t get, uint32_t word, uint32_t kind)
{
    if (kind == 1 && word == 0 && g_pbh && g_pbhist[(g_pbh - 1) & 63].word == 0) return;
    g_pbhist[g_pbh++ & 63] = (typeof(g_pbhist[0])){ get, word, kind, R(NV_PFIFO_CACHE1_DMA_PUT) };
}
void pfifo_dump_history(const char *why)
{
    static const char *kinds[] = { "", "method", "jump", "oldjump", "call", "return", "BAD" };
    xlog("pfifo: history (%s), oldest first:", why);
    for (unsigned i = 0; i < 64; i++) {
        unsigned k = (g_pbh + i) & 63;
        if (g_pbhist[k].kind) xlog("   @%08x %-7s %08x   put %08x", g_pbhist[k].get, kinds[g_pbhist[k].kind], g_pbhist[k].word, g_pbhist[k].put);
    }
}

/* ------------------------------------------------------------------ */
/* DMA objects / RAMHT                                                   */
/* ------------------------------------------------------------------ */
NvDma nv_dma_load(uint32_t inst)
{
    NvDma d = { 0, 0, 0, 0xFFFFFFFF };
    if (!inst || inst + 16 > NV_PRAMIN_SIZE) return d;
    uint32_t *o = (uint32_t *)(g_ramin + inst);
    d.cls = o[0] & 0xFFF;
    d.target = (o[0] >> 16) & 3;
    d.base = (o[2] & 0xFFFFF000) | (o[0] >> 20);
    d.limit = o[1];
    return d;
}

void *nv_dma_ptr(uint32_t inst, uint32_t offset, uint32_t len)
{
    NvDma d = nv_dma_load(inst);
    return xmem_va(d.base + offset);
}

static uint32_t pb_base(void)
{
    uint32_t inst = R(NV_PFIFO_CACHE1_DMA_INSTANCE) << 4;
    return inst ? nv_dma_load(inst).base : 0;
}

/* RAMHT: hash table of object handles -> instance; scan it linearly. */
static int ramht_find(uint32_t handle, uint32_t *inst)
{
    uint32_t ramht = R(NV_PFIFO_RAMHT);
    uint32_t base = ((ramht >> 4) & 0x1F) << 12;
    uint32_t size = 1u << (((ramht >> 16) & 3) + 12);
    for (uint32_t o = 0; o + 8 <= size && base + o + 8 <= NV_PRAMIN_SIZE; o += 8) {
        uint32_t *e = (uint32_t *)(g_ramin + base + o);
        if (e[0] == handle && (e[1] & 0x80000000)) { *inst = (e[1] & 0xFFFF) << 4; return 1; }
    }
    return 0;
}

static void fifo_method(uint32_t subch, uint32_t mthd, uint32_t data, uint32_t *rest, int nrest, int *consumed)
{
    *consumed = 1;
    if (mthd == 0) {                      /* SET_OBJECT */
        uint32_t inst = 0;
        if (!ramht_find(data, &inst)) { xlog("pfifo: SET_OBJECT handle %08x not in RAMHT", data); return; }
        uint32_t cls = *(uint32_t *)(g_ramin + inst) & 0xFF;
        g_obj_class[subch] = cls; g_obj_inst[subch] = inst;
        pgraph_bind_object(subch, data, inst, cls);
        return;
    }
    if (mthd == 0x50) {                   /* SET_REF */
        R(NV_PFIFO_CACHE1_REF) = data;
        return;
    }
    if (mthd < 0x100) {
        xlog("pfifo: fifo method %03x <- %08x (subch %u)", mthd, data, subch);
        return;
    }
    if (mthd >= 0x180 && mthd < 0x200) {
        /* methods taking object handles: the puller translates them to
         * instance addresses through RAMHT. Process one word at a time. */
        uint32_t inst = 0;
        if (!ramht_find(data, &inst)) xlog("pfifo: object handle %08x (method %03x) not in RAMHT", data, mthd);
        pgraph_method(subch, mthd, inst, NULL, 0, consumed);
        *consumed = 1;
        return;
    }
    pgraph_method(subch, mthd, data, rest, nrest, consumed);
}

volatile int g_wait_flip, g_wait_nop;
static volatile LONG g_present_req;

void nv2a_kick(void) { SetEvent(g_kick_ev); }

/* watchdog: where the GPU side stands */
void nv2a_diag(void)
{
    uint32_t s = R(NV_PGRAPH_SURFACE);
    xlog("gpu: queue %u words, method %03x subch %u left %u, wait_nop %d wait_flip %d (surface read %u write %u modulo %u), "
         "PGRAPH_INTR %08x FIFO %08x, DMA get %08x put %08x",
         g_cq_w - g_cq_r, e_mthd, e_subch, e_cnt, g_wait_nop, g_wait_flip, SURF_READ(s), SURF_WRITE(s), SURF_MODULO(s),
         R(NV_PGRAPH_INTR), R(NV_PGRAPH_FIFO), R(NV_PFIFO_CACHE1_DMA_GET), R(NV_PFIFO_CACHE1_DMA_PUT));
}
void gpu_request_present(void) { InterlockedExchange(&g_present_req, 1); SetEvent(g_kick_ev); }

static int stalled(void)
{
    if (g_wait_nop) return 1;
    if (g_wait_flip) {
        uint32_t s = R(NV_PGRAPH_SURFACE);
        if (SURF_READ(s) != SURF_WRITE(s)) { g_wait_flip = 0; return 0; }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fetch: ring -> command queue                                          */
/* ------------------------------------------------------------------ */
static void bad_command(uint32_t base, uint32_t at, uint32_t word, const char *why)
{
    static int n;
    pb_hist(at, word, 6);
    if (n++ < 4) {
        xlog("pfifo: %s %08x at %08x (put %08x)", why, word, at, R(NV_PFIFO_CACHE1_DMA_PUT));
        pfifo_dump_history(why);
    }
}

/* Copy everything between DMA_GET and DMA_PUT into the queue. Returns the
 * number of words queued. */
static int fetch(void)
{
    EnterCriticalSection(&g_fetch_lock);
    uint32_t base = pb_base();
    uint32_t get = R(NV_PFIFO_CACHE1_DMA_GET), put = R(NV_PFIFO_CACHE1_DMA_PUT);
    uint32_t w = g_cq_w, queued = 0;
    int guard = 0;
    while (get != put) {
        uint32_t depth = w - g_cq_r;
        if (depth >= CQ_CAP) break;                    /* GPU behind: D3D waits for ring space */
        uint32_t space = CQ_WORDS - depth;
        if (space < 4096) {                            /* cannot happen while CQ_CAP << CQ_WORDS */
            static int nf;
            if (nf++ < 5) {
                xlog("pfifo: command queue full (%u words queued); executor: method %03x subch %u left %u, wait_nop %d wait_flip %d; get %08x put %08x",
                     w - g_cq_r, e_mthd, e_subch, e_cnt, g_wait_nop, g_wait_flip, get, put);
                if (nf == 1) pfifo_dump_history("queue full");
            }
            break;
        }
        if (get >= XBOX_RAM_SIZE) { bad_command(base, get, 0, "GET outside RAM"); get = put; break; }
        uint32_t *src = (uint32_t *)xmem_va(base + get);
        if (f_mcnt) {
            /* data words. If PUT is behind GET the CPU has wrapped, so the
             * rest of this method (written in the previous lap) is complete. */
            uint32_t n = put > get ? (put - get) / 4 : f_mcnt;
            if (n > f_mcnt) n = f_mcnt;
            if (n > space - 16) n = space - 16;
            if (!n) break;
            g_cq[w % CQ_WORDS] = (n << 18) | (f_subch << 13) | f_mthd | (f_noninc ? 0x40000000u : 0); w++;
            for (uint32_t i = 0; i < n; i++, w++) g_cq[w % CQ_WORDS] = src[i];
            if (!f_noninc) f_mthd += 4 * n;
            f_mcnt -= n;
            get += 4 * n;
            queued += n + 1;
            continue;
        }
        uint32_t word = *src;
        get += 4;
        if ((word & 0xE0000003) == 0x20000000) {          /* old jump */
            pb_hist(get - 4, word, 3);
            get = word & 0x1FFFFFFC;
        } else if ((word & 3) == 1) {                      /* jump */
            pb_hist(get - 4, word, 2);
            get = word & 0xFFFFFFFC;
        } else if ((word & 3) == 2) {                      /* call */
            pb_hist(get - 4, word, 4);
            if (f_subr_active) bad_command(base, get - 4, word, "nested CALL");
            f_subr_ret = get; f_subr_active = 1;
            /* D3D reads DMA_SUBROUTINE (return offset | STATE) to know where
             * the pusher will continue while GET points into a called buffer. */
            R(NV_PFIFO_CACHE1_DMA_SUBROUTINE) = get | 1;
            get = word & 0xFFFFFFFC;
        } else if (word == 0x00020000) {                   /* return */
            pb_hist(get - 4, word, 5);
            if (!f_subr_active) bad_command(base, get - 4, word, "RETURN without CALL");
            get = f_subr_ret; f_subr_active = 0;
            R(NV_PFIFO_CACHE1_DMA_SUBROUTINE) &= ~1u;
        } else if ((word & 0xE0030003) == 0x00000000 || (word & 0xE0030003) == 0x40000000) {
            pb_hist(get - 4, word, 1);
            f_mthd = word & 0x1FFC;
            f_subch = (word >> 13) & 7;
            f_mcnt = (word >> 18) & 0x7FF;
            f_noninc = (word & 0x40000000) != 0;
            if (f_mthd == 0x100 && f_mcnt && (put > get || put < get - 4)) {   /* NOP: D3D software method? */
                uint32_t op = src[1];
                static int nlog;
                if ((op == 3 || op == 4) && nlog++ < 6) {
                    char buf[256]; int o = 0;
                    for (int i = -6; i < 10; i++) o += snprintf(buf + o, sizeof buf - o, "%s%08x", i == 0 ? " [" : i == 2 ? "] " : " ", src[i]);
                    xlog("pfifo: software method %u at %08x (put %08x):%s", op, get - 4, put, buf);
                }
            }
        } else {
            bad_command(base, get - 4, word, "reserved command");
        }
        if (++guard > (1 << 22)) { xlog("pfifo: fetch runaway; dropping to PUT"); get = put; f_mcnt = 0; break; }
    }
    R(NV_PFIFO_CACHE1_DMA_GET) = get;
    MemoryBarrier();
    g_cq_w = w;
    LeaveCriticalSection(&g_fetch_lock);
    g_pb_words += queued;
    return (int)queued;
}

/* ------------------------------------------------------------------ */
/* Execute: command queue -> PGRAPH                                      */
/* ------------------------------------------------------------------ */
enum { PB_IDLE, PB_STALLED, PB_BUDGET };

static int execute(int budget)
{
    while (budget > 0) {
        if (stalled()) return PB_STALLED;
        uint32_t r = g_cq_r;
        if (!e_cnt) {
            if (r == g_cq_w) return PB_IDLE;
            MemoryBarrier();
            uint32_t h = g_cq[r % CQ_WORDS];
            e_mthd = h & 0x1FFC; e_subch = (h >> 13) & 7; e_cnt = (h >> 18) & 0x7FF; e_noninc = (h & 0x40000000) != 0;
            g_cq_r = r + 1;
            continue;
        }
        uint32_t idx = r % CQ_WORDS;
        uint32_t avail = e_cnt;
        if (avail > CQ_WORDS - idx) avail = CQ_WORDS - idx;     /* chunk wraps the queue end */
        int used = 1;
        g_executing = 1;
        fifo_method(e_subch, e_mthd, g_cq[idx], &g_cq[idx + 1], (int)avail - 1, &used);
        g_executing = 0;
        if (used < 1) used = 1;
        if ((uint32_t)used > avail) used = (int)avail;
        if (!e_noninc) e_mthd += 4 * used;
        e_cnt -= used;
        g_cq_r = r + used;
        budget -= used;
    }
    return PB_BUDGET;
}

void gpu_thread_init(void);   /* renderer: window + context */
void gpu_present(void);       /* renderer: show the current front buffer */

NvPerf g_perf;

static void __stdcall gpu_thread(void *arg)
{
    gpu_thread_init();
    xlog("nv2a: GPU thread running");
    for (;;) {
        if (InterlockedExchange(&g_present_req, 0)) {
            uint64_t t = perf_ticks();
            gpu_present();
            g_perf.present += perf_ticks() - t;
            perf_report();
        }
        fetch();                                   /* catches up if the queue was full */
        uint64_t t = perf_ticks();
        int r = execute(20000);
        g_perf.exec += perf_ticks() - t;
        if (r == PB_IDLE) WaitForSingleObject(g_kick_ev, 5);
        else if (r == PB_STALLED) WaitForSingleObject(g_kick_ev, 2);
    }
}

void pfifo_init(void)
{
    g_kick_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&g_fetch_lock);
    g_cq = VirtualAlloc(NULL, CQ_WORDS * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_cq) xfatal("pfifo: command queue allocation failed");
    pgraph_init();
    xthread_create(gpu_thread, NULL, NULL, 4 << 20, 0, FALSE);
}

uint32_t pfifo_user_read(uint32_t off)
{
    uint32_t chan = off >> 16, reg = off & 0xFFFF;
    if (chan != 0) return 0;
    switch (reg) {
    case 0x40: return R(NV_PFIFO_CACHE1_DMA_PUT);
    case 0x44: return R(NV_PFIFO_CACHE1_DMA_GET);
    case 0x48: return R(NV_PFIFO_CACHE1_REF);
    }
    return 0;
}

void pfifo_user_write(uint32_t off, uint32_t val)
{
    uint32_t chan = off >> 16, reg = off & 0xFFFF;
    if (chan != 0) { xlog("pfifo: write to channel %u user area ignored", chan); return; }
    if (reg == 0x40) {
        R(NV_PFIFO_CACHE1_DMA_PUT) = val;
        fetch();                       /* capture the new commands before the CPU can reuse the space */
        SetEvent(g_kick_ev);
    } else if (reg == 0x44) {
        R(NV_PFIFO_CACHE1_DMA_GET) = val;
    } else xlog("pfifo: user write +%04x <- %08x", reg, val);
}

/* Busy while anything is unfetched, queued or executing. */
int pfifo_busy(void)
{
    return g_cq_r != g_cq_w || e_cnt || g_executing || R(NV_PFIFO_CACHE1_DMA_GET) != R(NV_PFIFO_CACHE1_DMA_PUT);
}
