/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xboxkrnl.exe HLE: the 120 kernel exports imported by JSRF, on Win32. */
#include "xbox.h"
#include "xmem.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <ctype.h>

#define KAPI __stdcall
#define FASTCALL __fastcall
#define TRACE(...) do { if (g_trace) xlog(__VA_ARGS__); } while (0)
int g_trace = 0;
void xbacktrace(void);

uint32_t xcall(void *fn, int n, const uint32_t *args);
void xthread_exit_current(void);

/* ------------------------------------------------------------------ */
/* Data exports                                                        */
/* ------------------------------------------------------------------ */
volatile ULONG x_KeTickCount;
PVOID          x_LaunchDataPage;
struct { ULONG Flags; UCHAR GpuRevision, McpRevision, Unk3, Unk4; } x_XboxHardwareInfo = { 0x20, 0xD2, 0xD4, 0, 0 };
UCHAR x_XboxHDKey[16], x_XboxSignatureKey[16], x_XboxAlternateSignatureKeys[16][16];
ULONG x_HalDiskCachePartitionCount = 3;
ULONG x_HalBootSMCVideoMode = 0;
static uint8_t x_ExEventObjectType[0x40], x_PsThreadObjectType[0x40];

/* ------------------------------------------------------------------ */
/* Global "dispatch lock": Xbox is single-core, so raising IRQL to     */
/* DISPATCH_LEVEL means exclusive access.  Emulate with a host lock.   */
/* ------------------------------------------------------------------ */
static CRITICAL_SECTION g_dispatch_lock, g_device_lock, g_obj_lock, g_file_lock;

/* IRQL emulation. Xbox is uniprocessor: code at DISPATCH_LEVEL excludes other
 * threads and DPCs; code above it (device IRQL) also excludes interrupt
 * service routines.  Two host locks model those two levels.  ISRs take only
 * the device lock, so they can still "preempt" DISPATCH_LEVEL code. */
static UCHAR irql_set(UCHAR n)
{
    XThread *t = xthread_current();
    if (!t) return 0;
    UCHAR old = t->pcr.Irql;
    if (old < 2 && n >= 2) EnterCriticalSection(&g_dispatch_lock);
    if (old <= 2 && n > 2) EnterCriticalSection(&g_device_lock);
    if (old > 2 && n <= 2) LeaveCriticalSection(&g_device_lock);
    if (old >= 2 && n < 2) LeaveCriticalSection(&g_dispatch_lock);
    t->pcr.Irql = n;
    return old;
}
UCHAR FASTCALL x_KfRaiseIrql(UCHAR n) { return irql_set(n); }
void  FASTCALL x_KfLowerIrql(UCHAR n) { irql_set(n); }
UCHAR KAPI x_KeRaiseIrqlToDpcLevel(void) { return irql_set(2); }

/* ------------------------------------------------------------------ */
/* Kernel-object -> host event mapping (KEVENT, KTIMER, CS events)     */
/* ------------------------------------------------------------------ */
typedef struct KObj { void *addr; HANDLE ev; UCHAR type; struct KObj *next; } KObj;
static KObj *g_kobj[1024];

static HANDLE kobj_event(void *obj)
{
    unsigned h = ((uintptr_t)obj >> 3) & 1023;
    X_DISPATCHER_HEADER *hd = obj;
    EnterCriticalSection(&g_obj_lock);
    KObj *k = g_kobj[h];
    while (k && k->addr != obj) k = k->next;
    if (k && k->type != hd->Type) {
        /* KeInitializeEvent is inline on the Xbox: the same address (often a
         * stack slot) can be reinitialised as a different kind of object. */
        CloseHandle(k->ev);
        k->ev = CreateEventA(NULL, hd->Type == 0 || hd->Type == 8, hd->SignalState != 0, NULL);
        k->type = hd->Type;
    }
    if (!k) {
        BOOL manual = (hd->Type == 0 || hd->Type == 8); /* Notification event/timer */
        k = malloc(sizeof *k);
        k->addr = obj;
        k->type = hd->Type;
        k->ev = CreateEventA(NULL, manual, hd->SignalState != 0, NULL);
        k->next = g_kobj[h]; g_kobj[h] = k;
    }
    LeaveCriticalSection(&g_obj_lock);
    return k->ev;
}

/* Xbox code clears notification events by storing SignalState = 0 directly
 * (KeClearEvent is a macro), e.g. D3D's BlockUntilVerticalBlank. Bring the
 * host event in line before waiting, or the wait returns at once forever. */
static void sync_notification_event(X_DISPATCHER_HEADER *h, HANDLE ev)
{
    if (h->Type != 0) return;
    if (h->SignalState) { SetEvent(ev); return; }
    ResetEvent(ev);
    MemoryBarrier();
    if (h->SignalState) SetEvent(ev);    /* KeSetEvent ran between the check and the reset */
}

/* Objects returned by ObReferenceObjectByHandle wrap a host handle. */
#define HOSTOBJ_MAGIC 0xFE
typedef struct { X_DISPATCHER_HEADER hdr; HANDLE h; volatile LONG refs; } HostObj;

static HANDLE obj_waitable(void *obj)
{
    HostObj *ho = obj;
    if (ho->hdr.Type == HOSTOBJ_MAGIC) return ho->h;
    return kobj_event(obj);
}

/* ------------------------------------------------------------------ */
/* Timeouts and waits                                                  */
/* ------------------------------------------------------------------ */
static DWORD timeout_ms(const LARGE_INTEGER *t)
{
    if (!t) return INFINITE;
    LONGLONG v = t->QuadPart;
    if (v < 0) return (DWORD)((-v + 9999) / 10000);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    LONGLONG now = ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return v <= now ? 0 : (DWORD)((v - now + 9999) / 10000);
}

static NTSTATUS_X wait_result(DWORD r)
{
    if (r == WAIT_TIMEOUT) return X_STATUS_TIMEOUT;
    if (r == WAIT_IO_COMPLETION) return X_STATUS_USER_APC;
    if (r == WAIT_FAILED) { xlog("wait failed: %lu", GetLastError()); return X_STATUS_INVALID_HANDLE; }
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + 64) return 0x80 + (r - WAIT_ABANDONED_0);
    return (NTSTATUS_X)(r - WAIT_OBJECT_0);
}

NTSTATUS_X KAPI x_NtWaitForSingleObject(HANDLE h, BOOLEAN alertable, PLARGE_INTEGER to)
{ return wait_result(WaitForSingleObjectEx(h, timeout_ms(to), alertable)); }
NTSTATUS_X KAPI x_NtWaitForSingleObjectEx(HANDLE h, CHAR mode, BOOLEAN alertable, PLARGE_INTEGER to)
{ return wait_result(WaitForSingleObjectEx(h, timeout_ms(to), alertable)); }
NTSTATUS_X KAPI x_NtWaitForMultipleObjectsEx(ULONG n, HANDLE *hs, ULONG type, CHAR mode, BOOLEAN alertable, PLARGE_INTEGER to)
{ return wait_result(WaitForMultipleObjectsEx(n, hs, type == 0, timeout_ms(to), alertable)); }

NTSTATUS_X KAPI x_KeWaitForSingleObject(PVOID obj, ULONG reason, CHAR mode, BOOLEAN alertable, PLARGE_INTEGER to)
{
    HANDLE w = obj_waitable(obj);
    if (((X_DISPATCHER_HEADER *)obj)->Type != HOSTOBJ_MAGIC) sync_notification_event(obj, w);
    NTSTATUS_X r = wait_result(WaitForSingleObjectEx(w, timeout_ms(to), alertable));
    if (r == 0 && ((X_DISPATCHER_HEADER *)obj)->Type != HOSTOBJ_MAGIC) {
        X_DISPATCHER_HEADER *h = obj;
        if (h->Type == 1 || h->Type == 9) h->SignalState = 0;
    }
    return r;
}

NTSTATUS_X KAPI x_KeDelayExecutionThread(CHAR mode, BOOLEAN alertable, PLARGE_INTEGER interval)
{
    DWORD ms = timeout_ms(interval);
    if (ms == 0) {
        /* SleepEx(0, TRUE) is how titles (and CRI middleware) let pending
         * I/O-completion APCs run; it must enter an alertable wait. */
        if (alertable) return SleepEx(0, TRUE) == WAIT_IO_COMPLETION ? X_STATUS_USER_APC : 0;
        SwitchToThread();
        return 0;
    }
    return SleepEx(ms, alertable) == WAIT_IO_COMPLETION ? X_STATUS_USER_APC : 0;
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */
NTSTATUS_X KAPI x_NtCreateEvent(PHANDLE out, X_OBJECT_ATTRIBUTES *oa, ULONG type, BOOLEAN initial)
{
    char name[260] = {0};
    if (oa && oa->ObjectName && oa->ObjectName->Length) {
        snprintf(name, sizeof name, "jsrf_%.*s", oa->ObjectName->Length, oa->ObjectName->Buffer);
        for (char *p = name; *p; p++) if (*p == '\\') *p = '_';
    }
    HANDLE h = CreateEventA(NULL, type == 0, initial, name[0] ? name : NULL);
    if (!h) return X_STATUS_UNSUCCESSFUL;
    *out = h;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0x40000000 /*OBJECT_NAME_EXISTS*/ : 0;
}
NTSTATUS_X KAPI x_NtSetEvent(HANDLE h, PLONG prev) { if (prev) *prev = 0; return SetEvent(h) ? 0 : X_STATUS_INVALID_HANDLE; }
NTSTATUS_X KAPI x_NtClearEvent(HANDLE h) { return ResetEvent(h) ? 0 : X_STATUS_INVALID_HANDLE; }

LONG KAPI x_KeSetEvent(X_KEVENT *ev, LONG incr, BOOLEAN wait)
{
    LONG old = ev->Header.SignalState;
    HANDLE h = kobj_event(ev);
    ev->Header.SignalState = 1;
    SetEvent(h);
    if (ev->Header.Type == 1) ev->Header.SignalState = 0; /* auto-reset: consumed by host */
    return old;
}

/* ------------------------------------------------------------------ */
/* Critical sections (Xbox layout, NT algorithm)                       */
/* ------------------------------------------------------------------ */
void KAPI x_RtlInitializeCriticalSection(X_CRITICAL_SECTION *cs)
{
    memset(cs, 0, sizeof *cs);
    cs->Event.Type = 1; /* synchronization event */
    cs->Event.Size = 4;
    cs->LockCount = -1;
}
void KAPI x_RtlEnterCriticalSection(X_CRITICAL_SECTION *cs)
{
    HANDLE me = (HANDLE)xthread_current()->pcr.CurrentThread;
    if (InterlockedIncrement(&cs->LockCount) == 0) {
        cs->OwningThread = me; cs->RecursionCount = 1; return;
    }
    if (cs->OwningThread == me) { cs->RecursionCount++; return; }
    WaitForSingleObject(kobj_event(&cs->Event), INFINITE);
    cs->OwningThread = me; cs->RecursionCount = 1;
}
void KAPI x_RtlLeaveCriticalSection(X_CRITICAL_SECTION *cs)
{
    if (--cs->RecursionCount != 0) { InterlockedDecrement(&cs->LockCount); return; }
    cs->OwningThread = 0;
    if (InterlockedDecrement(&cs->LockCount) >= 0) SetEvent(kobj_event(&cs->Event));
}

/* ------------------------------------------------------------------ */
/* DPCs and timers: one worker thread, run under the dispatch lock     */
/* ------------------------------------------------------------------ */
#define MAX_TIMERS 256
static X_KDPC *g_dpcq[512]; static int g_dpcq_n;
static X_KTIMER *g_timers[MAX_TIMERS]; static LONGLONG g_timer_due[MAX_TIMERS];
static HANDLE g_kwork_ev;
static LARGE_INTEGER g_qpf, g_qpc0;

/* Clocks use integer math only: they run inside game calls, where the x87
 * state belongs to the game (it may hold MMX data or run at 24-bit
 * precision, which would make a double-based clock jitter or read NaN). */
static inline uint64_t qpc_scale(uint64_t d, uint64_t f, uint64_t hz) { return (d / f) * hz + (d % f) * hz / f; }
static LONGLONG now_100ns(void)
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (LONGLONG)qpc_scale((uint64_t)(c.QuadPart - g_qpc0.QuadPart), (uint64_t)g_qpf.QuadPart, 10000000ull);
}

void KAPI x_KeInitializeDpc(X_KDPC *d, void *routine, PVOID ctx)
{
    memset(d, 0, sizeof *d); d->Type = 19; d->DeferredRoutine = routine; d->DeferredContext = ctx;
}
BOOLEAN KAPI x_KeInsertQueueDpc(X_KDPC *d, PVOID a1, PVOID a2)
{
    BOOLEAN r = FALSE;
    EnterCriticalSection(&g_obj_lock);
    if (!d->Inserted && g_dpcq_n < 512) {
        d->Inserted = 1; d->SystemArgument1 = a1; d->SystemArgument2 = a2;
        g_dpcq[g_dpcq_n++] = d; r = TRUE;
    }
    LeaveCriticalSection(&g_obj_lock);
    if (r) SetEvent(g_kwork_ev);
    return r;
}
BOOLEAN KAPI x_KeRemoveQueueDpc(X_KDPC *d)
{
    BOOLEAN r = FALSE;
    EnterCriticalSection(&g_obj_lock);
    for (int i = 0; i < g_dpcq_n; i++) if (g_dpcq[i] == d) {
        memmove(&g_dpcq[i], &g_dpcq[i + 1], (g_dpcq_n - i - 1) * sizeof(void *));
        g_dpcq_n--; d->Inserted = 0; r = TRUE; break;
    }
    LeaveCriticalSection(&g_obj_lock);
    return r;
}

void KAPI x_KeInitializeTimerEx(X_KTIMER *t, ULONG type)
{
    memset(t, 0, sizeof *t); t->Header.Type = 8 + (UCHAR)type; t->Header.Size = sizeof *t / 4;
}
static BOOLEAN timer_cancel_locked(X_KTIMER *t)
{
    for (int i = 0; i < MAX_TIMERS; i++) if (g_timers[i] == t) { g_timers[i] = NULL; t->Header.Inserted = 0; return TRUE; }
    return FALSE;
}
BOOLEAN KAPI x_KeCancelTimer(X_KTIMER *t)
{
    EnterCriticalSection(&g_obj_lock); BOOLEAN r = timer_cancel_locked(t); LeaveCriticalSection(&g_obj_lock);
    return r;
}
BOOLEAN KAPI x_KeSetTimer(X_KTIMER *t, LARGE_INTEGER due, X_KDPC *dpc)
{
    EnterCriticalSection(&g_obj_lock);
    BOOLEAN was = timer_cancel_locked(t);
    t->Dpc = dpc; t->Header.SignalState = 0;
    ResetEvent(kobj_event(t));
    LONGLONG at = due.QuadPart < 0 ? now_100ns() - due.QuadPart
                                    : now_100ns() + (LONGLONG)timeout_ms(&due) * 10000;
    for (int i = 0; i < MAX_TIMERS; i++) if (!g_timers[i]) { g_timers[i] = t; g_timer_due[i] = at; t->Header.Inserted = 1; break; }
    LeaveCriticalSection(&g_obj_lock);
    SetEvent(g_kwork_ev);
    return was;
}

static void __stdcall kernel_worker(void *unused)
{
    for (;;) {
        LONGLONG now = now_100ns(), next = now + 100000; /* timers and DPCs signal g_kwork_ev; poll every 10 ms anyway */
        X_KTIMER *fired[MAX_TIMERS]; int nf = 0;
        EnterCriticalSection(&g_obj_lock);
        for (int i = 0; i < MAX_TIMERS; i++) if (g_timers[i]) {
            if (g_timer_due[i] <= now) { fired[nf++] = g_timers[i]; g_timers[i]->Header.Inserted = 0; g_timers[i] = NULL; }
            else if (g_timer_due[i] < next) next = g_timer_due[i];
        }
        LeaveCriticalSection(&g_obj_lock);
        for (int i = 0; i < nf; i++) {
            fired[i]->Header.SignalState = 1;
            SetEvent(kobj_event(fired[i]));
            if (fired[i]->Dpc) x_KeInsertQueueDpc(fired[i]->Dpc, (PVOID)(ULONG)now, (PVOID)(ULONG)(now >> 32));
        }
        UCHAR old = irql_set(2);
        for (;;) {
            EnterCriticalSection(&g_obj_lock);
            X_KDPC *d = NULL;
            if (g_dpcq_n) { d = g_dpcq[0]; memmove(g_dpcq, g_dpcq + 1, --g_dpcq_n * sizeof(void *)); d->Inserted = 0; }
            LeaveCriticalSection(&g_obj_lock);
            if (!d) break;
            uint32_t a[4] = { (uint32_t)d, (uint32_t)d->DeferredContext, (uint32_t)d->SystemArgument1, (uint32_t)d->SystemArgument2 };
            xcall(d->DeferredRoutine, 4, a);
        }
        irql_set(old);
        LONGLONG wait = (next - now_100ns()) / 10000;
        WaitForSingleObject(g_kwork_ev, wait < 0 ? 0 : wait > 1 ? (DWORD)wait : 1);
    }
}

/* Interrupts.  Xbox KINTERRUPT layout (only the fields we use). */
typedef struct {
    void   *ServiceRoutine;      /* BOOLEAN (stdcall *)(KINTERRUPT *, PVOID) */
    PVOID   ServiceContext;
    ULONG   BusInterruptLevel;
    ULONG   Irql;
    BOOLEAN Connected, ShareVector; UCHAR Mode; UCHAR pad;
    ULONG   ServiceCount;
} X_KINTERRUPT;
static X_KINTERRUPT *g_irq[32];

void KAPI x_KeInitializeInterrupt(X_KINTERRUPT *i, void *routine, PVOID ctx, ULONG vector, UCHAR irql, ULONG mode, BOOLEAN share)
{
    memset(i, 0, sizeof *i);
    i->ServiceRoutine = routine; i->ServiceContext = ctx;
    i->BusInterruptLevel = vector - 0x30; i->Irql = irql; i->Mode = (UCHAR)mode; i->ShareVector = share;
}
BOOLEAN KAPI x_KeConnectInterrupt(X_KINTERRUPT *i)
{
    if (i->BusInterruptLevel >= 32 || i->Connected) return FALSE;
    i->Connected = TRUE; g_irq[i->BusInterruptLevel] = i;
    xlog("KeConnectInterrupt: level %lu irql %lu isr %p ctx %p", i->BusInterruptLevel, i->Irql, i->ServiceRoutine, i->ServiceContext);
    return TRUE;
}
BOOLEAN KAPI x_KeDisconnectInterrupt(X_KINTERRUPT *i)
{
    if (!i->Connected) return FALSE;
    i->Connected = FALSE; if (i->BusInterruptLevel < 32) g_irq[i->BusInterruptLevel] = NULL;
    return TRUE;
}
/* Xbox: vector = 0x30 + level, IRQL = 26 - level (PROFILE_LEVEL-relative). */
ULONG KAPI x_HalGetInterruptVector(ULONG level, UCHAR *irql) { if (irql) *irql = (UCHAR)(26 - level); return 0x30 + level; }

BOOLEAN KAPI x_KeSynchronizeExecution(X_KINTERRUPT *intr, void *routine, PVOID ctx)
{
    UCHAR old = irql_set(intr && intr->Irql > 2 ? (UCHAR)intr->Irql : 3);
    uint32_t a[1] = { (uint32_t)ctx };
    BOOLEAN r = (BOOLEAN)xcall(routine, 1, a);
    irql_set(old);
    return r;
}

/* Called by emulated devices (from an xthread) to assert a bus interrupt.
 * Runs the ISR under the device lock only. Returns the ISR's result. */
int kernel_raise_interrupt(ULONG level)
{
    X_KINTERRUPT *i = level < 32 ? g_irq[level] : NULL;
    if (!i || !i->Connected) return 0;
    XThread *t = xthread_current();
    EnterCriticalSection(&g_device_lock);
    UCHAR old = t->pcr.Irql; t->pcr.Irql = (UCHAR)i->Irql;
    uint32_t a[2] = { (uint32_t)i, (uint32_t)i->ServiceContext };
    int r = (BOOLEAN)xcall(i->ServiceRoutine, 2, a);
    i->ServiceCount++;
    t->pcr.Irql = old;
    LeaveCriticalSection(&g_device_lock);
    return r;
}

/* ------------------------------------------------------------------ */
/* Threads and objects                                                 */
/* ------------------------------------------------------------------ */
NTSTATUS_X KAPI x_PsCreateSystemThreadEx(PHANDLE h, ULONG ext, ULONG kstack, ULONG tls, PHANDLE tid,
    void *start, PVOID ctx, BOOLEAN suspended, BOOLEAN dbg, void *sysroutine)
{
    XThread *t = xthread_create(start, ctx, sysroutine, kstack, tls, TRUE);
    xthread_pin_game(t);
    if (!suspended) ResumeThread(t->host_handle);
    xlog("PsCreateSystemThreadEx start=%p ctx=%p tls=%lu stack=%lu -> tid %lu", start, ctx, tls, kstack, t->host_tid);
    if (h) DuplicateHandle(GetCurrentProcess(), t->host_handle, GetCurrentProcess(), h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (tid) *tid = (HANDLE)(uintptr_t)t->host_tid;
    return 0;
}
void KAPI x_PsTerminateSystemThread(NTSTATUS_X st) { xthread_exit_current(); }
NTSTATUS_X KAPI x_NtResumeThread(HANDLE h, PULONG prev) { DWORD r = ResumeThread(h); if (prev) *prev = r; return r == (DWORD)-1 ? X_STATUS_INVALID_HANDLE : 0; }
NTSTATUS_X KAPI x_NtSuspendThread(HANDLE h, PULONG prev) { DWORD r = SuspendThread(h); if (prev) *prev = r; return r == (DWORD)-1 ? X_STATUS_INVALID_HANDLE : 0; }

NTSTATUS_X KAPI x_ObReferenceObjectByHandle(HANDLE h, PVOID type, PVOID *out)
{
    HostObj *o = calloc(1, sizeof *o);
    o->hdr.Type = HOSTOBJ_MAGIC;
    o->refs = 1;
    if (h == (HANDLE)-2) h = GetCurrentThread(); /* NtCurrentThread() */
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &o->h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        free(o);
        return X_STATUS_INVALID_HANDLE;
    }
    *out = o;
    return 0;
}
void FASTCALL x_ObfDereferenceObject(PVOID p)
{
    HostObj *o = p;
    if (!o || o->hdr.Type != HOSTOBJ_MAGIC) return;      /* a kernel object in title memory */
    if (InterlockedDecrement(&o->refs) == 0) { CloseHandle(o->h); o->hdr.Type = 0; free(o); }
}

static HANDLE thread_of(PVOID kthread)
{
    HostObj *o = kthread;
    if (o && o->hdr.Type == HOSTOBJ_MAGIC) return o->h;
    return GetCurrentThread();
}
int g_prio_trace;
LONG KAPI x_KeQueryBasePriorityThread(PVOID t)
{
    LONG r = GetThreadPriority(thread_of(t));
    if (g_prio_trace) { static LONG n; if (InterlockedIncrement(&n) < 400) xlog("KeQueryBasePriorityThread(%p) -> %ld  ret=%p", t, r, __builtin_return_address(0)); }
    return r;
}
LONG KAPI x_KeSetBasePriorityThread(PVOID t, LONG incr)
{
    HANDLE h = thread_of(t); LONG old = GetThreadPriority(h), want = incr;
    if (incr > 2) incr = THREAD_PRIORITY_TIME_CRITICAL;
    if (incr < -2) incr = THREAD_PRIORITY_IDLE;
    BOOL ok = SetThreadPriority(h, incr);
    if (g_prio_trace) { static LONG n; if (InterlockedIncrement(&n) < 400)
        xlog("KeSetBasePriorityThread(%p, %ld) old=%ld ok=%d now=%d", t, want, old, ok, GetThreadPriority(h)); }
    return old;
}
LONG KAPI x_KeSetDisableBoostThread(PVOID t, LONG disable) { SetThreadPriorityBoost(thread_of(t), disable); return 0; }

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
static DWORD host_protect(ULONG p)
{
    p &= 0xFF; /* strip PAGE_NOCACHE / PAGE_WRITECOMBINE (0x200/0x400) */
    return p ? p : PAGE_READWRITE;
}
NTSTATUS_X KAPI x_NtAllocateVirtualMemory(PVOID *base, ULONG zero, PULONG size, ULONG type, ULONG prot)
{
    type &= (MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN);
    PVOID p = VirtualAlloc(*base, *size, type, host_protect(prot));
    if (!p) { TRACE("NtAllocateVirtualMemory(%p,%lx) failed %lu", *base, *size, GetLastError()); return X_STATUS_NO_MEMORY; }
    MEMORY_BASIC_INFORMATION mbi; VirtualQuery(p, &mbi, sizeof mbi);
    *base = p; *size = (ULONG)((*size + 0xFFF) & ~0xFFF);
    return 0;
}
NTSTATUS_X KAPI x_NtFreeVirtualMemory(PVOID *base, PULONG size, ULONG type)
{
    if (type & MEM_RELEASE) return VirtualFree(*base, 0, MEM_RELEASE) ? 0 : X_STATUS_INVALID_PARAMETER;
    return VirtualFree(*base, *size, type) ? 0 : X_STATUS_INVALID_PARAMETER;
}
NTSTATUS_X KAPI x_NtQueryVirtualMemory(PVOID base, MEMORY_BASIC_INFORMATION *mbi)
{ return VirtualQuery(base, mbi, sizeof *mbi) ? 0 : X_STATUS_INVALID_PARAMETER; }
NTSTATUS_X KAPI x_NtProtectVirtualMemory(PVOID *base, PULONG size, ULONG prot, PULONG old)
{ DWORD o; BOOL r = VirtualProtect(*base, *size, host_protect(prot), &o); if (old) *old = o; return r ? 0 : X_STATUS_INVALID_PARAMETER; }

PVOID KAPI x_MmAllocateContiguousMemoryEx(ULONG size, ULONG lo, ULONG hi, ULONG align, ULONG prot)
{
    PVOID p = xmem_alloc_contiguous(size, lo, hi, align);
    TRACE("MmAllocateContiguousMemoryEx(%lx, %lx-%lx, align %lx, prot %lx) -> %p", size, lo, hi, align, prot, p);
    return p;
}
PVOID KAPI x_MmAllocateContiguousMemory(ULONG size) { return x_MmAllocateContiguousMemoryEx(size, 0, 0xFFFFFFFF, 0, 4); }
void  KAPI x_MmFreeContiguousMemory(PVOID p) { xmem_free(p); }
PVOID KAPI x_MmAllocateSystemMemory(ULONG size, ULONG prot) { return xmem_alloc_contiguous(size, 0, 0xFFFFFFFF, 0); }
ULONG KAPI x_MmFreeSystemMemory(PVOID p, ULONG size) { xmem_free(p); return 0; }
ULONG KAPI x_MmQueryAllocationSize(PVOID p)
{
    if (xmem_is_ram(p)) return xmem_size(p);
    MEMORY_BASIC_INFORMATION m; char *q; ULONG total = 0;
    if (!VirtualQuery(p, &m, sizeof m)) return 0;
    PVOID ab = m.AllocationBase; q = ab;
    while (VirtualQuery(q, &m, sizeof m) && m.AllocationBase == ab && m.State != MEM_FREE) { total += m.RegionSize; q += m.RegionSize; }
    return total;
}
ULONG KAPI x_MmGetPhysicalAddress(PVOID p)
{
    uint32_t pa = xmem_pa(p);
    if (pa == XMEM_BAD_PA) { xlog("MmGetPhysicalAddress(%p): not in Xbox RAM", p); return (ULONG)p & (XBOX_RAM_SIZE - 1); }
    return pa;
}
void  KAPI x_MmPersistContiguousMemory(PVOID p, ULONG n, BOOLEAN persist) {}
void  KAPI x_MmLockUnlockBufferPages(PVOID p, ULONG n, BOOLEAN unlock) {}
void  KAPI x_MmLockUnlockPhysicalPage(ULONG pa, BOOLEAN unlock) {}
void  KAPI x_MmSetAddressProtect(PVOID p, ULONG n, ULONG prot) { if (!xmem_is_ram(p)) { DWORD o; VirtualProtect(p, n, host_protect(prot), &o); } }
ULONG KAPI x_MmQueryAddressProtect(PVOID p)
{
    if (xmem_is_ram(p)) return PAGE_READWRITE;
    MEMORY_BASIC_INFORMATION m; return VirtualQuery(p, &m, sizeof m) ? m.Protect : 0;
}
/* GPU instance memory = the top 64 KB of physical RAM (reserved in xmem). */
PVOID KAPI x_MmClaimGpuInstanceMemory(ULONG size, PULONG pad)
{
    if (pad) *pad = 0;
    xlog("MmClaimGpuInstanceMemory(%lx)", size);
    return (PVOID)(uintptr_t)(XBOX_RAM_VA + XBOX_RAM_SIZE);
}

PVOID KAPI x_ExAllocatePoolWithTag(ULONG n, ULONG tag) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
PVOID KAPI x_ExAllocatePool(ULONG n) { return x_ExAllocatePoolWithTag(n, 0); }
void  KAPI x_ExFreePool(PVOID p) { HeapFree(GetProcessHeap(), 0, p); }
ULONG KAPI x_ExQueryPoolBlockSize(PVOID p) { return (ULONG)HeapSize(GetProcessHeap(), 0, p); }

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
void KAPI x_KeQuerySystemTime(PLARGE_INTEGER t) { FILETIME f; GetSystemTimeAsFileTime(&f); t->LowPart = f.dwLowDateTime; t->HighPart = f.dwHighDateTime; }
ULONGLONG KAPI x_KeQueryInterruptTime(void) { return (ULONGLONG)now_100ns(); }
void KAPI x_KeStallExecutionProcessor(ULONG us)
{
    LONGLONG end = now_100ns() + (LONGLONG)us * 10;
    while (now_100ns() < end) YieldProcessor();
}
NTSTATUS_X KAPI x_KeSaveFloatingPointState(PVOID s) { return 0; }
NTSTATUS_X KAPI x_KeRestoreFloatingPointState(PVOID s) { return 0; }

/* Xbox CPU timestamp counter is 733.333 MHz; game code assumes it. */
#define XBOX_TSC_HZ 733333333ull
uint64_t xbox_tsc(void)
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return qpc_scale((uint64_t)(c.QuadPart - g_qpc0.QuadPart), (uint64_t)g_qpf.QuadPart, XBOX_TSC_HZ);
}
BOOL KAPI hle_QueryPerformanceCounter(LARGE_INTEGER *out) { out->QuadPart = (LONGLONG)xbox_tsc(); return TRUE; }

static DWORD WINAPI tick_thread(LPVOID p)
{
    for (;;) { x_KeTickCount = (ULONG)(now_100ns() / 10000); Sleep(1); }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rtl                                                                 */
/* ------------------------------------------------------------------ */
void KAPI x_RtlInitAnsiString(X_STRING *s, const char *src)
{
    s->Buffer = (char *)src;
    if (src) { size_t n = strlen(src); s->Length = (USHORT)n; s->MaximumLength = (USHORT)(n + 1); }
    else s->Length = s->MaximumLength = 0;
}
BOOLEAN KAPI x_RtlEqualString(X_STRING *a, X_STRING *b, BOOLEAN ci)
{
    if (a->Length != b->Length) return FALSE;
    return (ci ? _strnicmp(a->Buffer, b->Buffer, a->Length) : memcmp(a->Buffer, b->Buffer, a->Length)) == 0;
}
ULONG KAPI x_RtlCompareMemoryUlong(PVOID src, ULONG len, ULONG pat)
{
    ULONG *p = src, n = 0;
    for (ULONG i = 0; i < len / 4; i++, n += 4) if (p[i] != pat) break;
    return n;
}
static ULONG (WINAPI *p_RtlNtStatusToDosError)(LONG);
static void (WINAPI *p_RtlTimeToTimeFields)(PLARGE_INTEGER, PVOID);
static BOOLEAN (WINAPI *p_RtlTimeFieldsToTime)(PVOID, PLARGE_INTEGER);
ULONG   KAPI x_RtlNtStatusToDosError(NTSTATUS_X s) { return p_RtlNtStatusToDosError(s); }
void    KAPI x_RtlTimeToTimeFields(PLARGE_INTEGER t, PVOID f) { p_RtlTimeToTimeFields(t, f); }
BOOLEAN KAPI x_RtlTimeFieldsToTime(PVOID f, PLARGE_INTEGER t) { return p_RtlTimeFieldsToTime(f, t); }
/* RtlRaiseException / RtlUnwind: x86 SEH is identical; forwarded to ntdll in kernel_resolve. */

/* ------------------------------------------------------------------ */
/* Hal / Av / misc                                                     */
/* ------------------------------------------------------------------ */
ULONG __cdecl x_DbgPrint(const char *fmt, ...)
{
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    size_t n = strlen(buf); while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    xlog("[DbgPrint] %s", buf);
    return 0;
}
void KAPI x_KeBugCheck(ULONG code) { xfatal("KeBugCheck(0x%08lx)", code); }
void KAPI x_HalReturnToFirmware(ULONG how) { xlog("HalReturnToFirmware(%lu) from %p: exiting", how, __builtin_return_address(0)); xbacktrace(); ExitProcess(0); }
void KAPI x_HalInitiateShutdown(void) { xlog("HalInitiateShutdown"); ExitProcess(0); }
BOOLEAN KAPI x_HalIsResetOrShutdownPending(void) { return FALSE; }
void KAPI x_HalRegisterShutdownNotification(PVOID r, BOOLEAN reg) {}
void KAPI x_HalReadWritePCISpace(ULONG bus, ULONG slot, ULONG reg, PVOID buf, ULONG len, BOOLEAN write)
{ if (!write) memset(buf, 0, len); }

PVOID KAPI x_AvGetSavedDataAddress(void) { return NULL; }
void  KAPI x_AvSetSavedDataAddress(PVOID p) {}
ULONG KAPI x_AvSetDisplayMode(PVOID regs, ULONG step, ULONG mode, ULONG fmt, ULONG pitch, ULONG fb) { return 0; }
void  KAPI x_AvSendTVEncoderOption(PVOID regs, ULONG opt, ULONG param, PULONG result)
{
    if (result) *result = (opt == 6) ? 0x00400106 /* HDTV pack | NTSC-M | 60Hz */ : 0;
}

/* EEPROM settings (XC_* indices) */
NTSTATUS_X KAPI x_ExQueryNonVolatileSetting(ULONG idx, PULONG type, PVOID val, ULONG len, PULONG outlen)
{
    ULONG v = 0;
    switch (idx) {
    case 0x7:   v = 1; break;          /* XC_LANGUAGE = English */
    case 0x103: v = 0x00400100; break; /* XC_FACTORY_AV_REGION = NTSC-M */
    case 0x104: v = 1; break;          /* XC_FACTORY_GAME_REGION = NA */
    case 0x8:   v = 0; break;          /* XC_VIDEO_FLAGS */
    case 0x9:   v = 0; break;          /* XC_AUDIO_FLAGS */
    default:    TRACE("ExQueryNonVolatileSetting(0x%lx) -> 0", idx); break;
    }
    if (type) *type = 4; /* REG_DWORD */
    if (val && len) { memset(val, 0, len); memcpy(val, &v, len < 4 ? len : 4); }
    if (outlen) *outlen = 4;
    return 0;
}

/* Xbox section loading: everything is mapped up front, just refcount. */
NTSTATUS_X KAPI x_XeLoadSection(XBE_SECTION *s) { s->SectionReferenceCount++; return 0; }
NTSTATUS_X KAPI x_XeUnloadSection(XBE_SECTION *s) { if (s->SectionReferenceCount) s->SectionReferenceCount--; return 0; }

/* Io* driver-model calls: only used by XDK USB/disk drivers we replace. */
NTSTATUS_X KAPI x_IoCreateDevice(PVOID drv, ULONG ext, PVOID name, ULONG type, BOOLEAN excl, PVOID *dev)
{
    /* Xbox DEVICE_OBJECT: Type, Size, RefCount, DriverObject(+8) ... Flags(+0x14),
     * DeviceExtension(+0x18), DeviceType(+0x1C), StackSize(+0x1E) ... */
    uint8_t *d = calloc(1, 0x80 + ext);
    *(USHORT *)(d + 0) = 3; *(USHORT *)(d + 2) = 0x80;
    *(PVOID *)(d + 0x08) = drv;
    *(PVOID *)(d + 0x0C) = d;
    *(PVOID *)(d + 0x18) = ext ? d + 0x80 : NULL;
    d[0x1C] = (UCHAR)type; d[0x1E] = 1;
    *dev = d;
    return 0;
}
NTSTATUS_X KAPI x_IoInvalidDeviceRequest(PVOID dev, PVOID irp) { return X_STATUS_INVALID_DEVICE_REQUEST; }
void KAPI x_IoStartNextPacket(PVOID dev) {}
void KAPI x_IoStartPacket(PVOID dev, PVOID irp, PULONG key) {}
void FASTCALL x_IofCompleteRequest(PVOID irp, CHAR boost) {}
void KAPI x_IoMarkIrpMustComplete(PVOID irp) {}
NTSTATUS_X KAPI x_IoDismountVolumeByName(X_STRING *n) { return 0; }

/* SHA-1 (XcSHA*): used for save-game signatures, and by main.c to identify
 * default.xbe. SHA1C is declared in xbox.h. */
static void sha1_block(SHA1C *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, t, cc;
    for (int i = 0; i < 16; i++) w[i] = (p[4*i] << 24) | (p[4*i+1] << 16) | (p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 80; i++) { t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]; w[i] = (t << 1) | (t >> 31); }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (int i = 0; i < 80; i++) {
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = cc; cc = (b << 30) | (b >> 2); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
void KAPI x_XcSHAInit(SHA1C *c) { c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE; c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0; c->n=0; }
void KAPI x_XcSHAUpdate(SHA1C *c, const uint8_t *p, ULONG len)
{
    while (len--) { c->buf[c->n++ & 63] = *p++; if ((c->n & 63) == 0) sha1_block(c, c->buf); }
}
void KAPI x_XcSHAFinal(SHA1C *c, uint8_t *out)
{
    uint64_t bits = c->n * 8; uint8_t pad = 0x80, z = 0;
    x_XcSHAUpdate(c, &pad, 1);
    while ((c->n & 63) != 56) x_XcSHAUpdate(c, &z, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (i * 8)); x_XcSHAUpdate(c, &b, 1); }
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* file I/O lives in kfile.c */
#include "kfile.inc"

/* ------------------------------------------------------------------ */
/* Export table                                                        */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t ord; void *addr; } KExport;
#define E(o, f) { o, (void *)&f }
static const KExport g_exports[] = {
    E(1, x_AvGetSavedDataAddress), E(2, x_AvSendTVEncoderOption), E(3, x_AvSetDisplayMode), E(4, x_AvSetSavedDataAddress),
    E(8, x_DbgPrint), E(14, x_ExAllocatePool), E(15, x_ExAllocatePoolWithTag), E(16, x_ExEventObjectType),
    E(17, x_ExFreePool), E(23, x_ExQueryPoolBlockSize), E(24, x_ExQueryNonVolatileSetting),
    E(40, x_HalDiskCachePartitionCount), E(44, x_HalGetInterruptVector), E(46, x_HalReadWritePCISpace),
    E(47, x_HalRegisterShutdownNotification), E(49, x_HalReturnToFirmware),
    E(65, x_IoCreateDevice), E(67, x_IoCreateSymbolicLink), E(69, x_IoDeleteSymbolicLink), E(74, x_IoInvalidDeviceRequest),
    E(81, x_IoStartNextPacket), E(83, x_IoStartPacket), E(87, x_IofCompleteRequest), E(91, x_IoDismountVolumeByName),
    E(95, x_KeBugCheck), E(97, x_KeCancelTimer), E(98, x_KeConnectInterrupt), E(99, x_KeDelayExecutionThread),
    E(100, x_KeDisconnectInterrupt), E(107, x_KeInitializeDpc), E(109, x_KeInitializeInterrupt), E(113, x_KeInitializeTimerEx),
    E(119, x_KeInsertQueueDpc), E(124, x_KeQueryBasePriorityThread), E(125, x_KeQueryInterruptTime), E(128, x_KeQuerySystemTime),
    E(129, x_KeRaiseIrqlToDpcLevel), E(137, x_KeRemoveQueueDpc), E(139, x_KeRestoreFloatingPointState),
    E(142, x_KeSaveFloatingPointState), E(143, x_KeSetBasePriorityThread), E(144, x_KeSetDisableBoostThread),
    E(145, x_KeSetEvent), E(149, x_KeSetTimer), E(151, x_KeStallExecutionProcessor), E(153, x_KeSynchronizeExecution),
    E(156, x_KeTickCount), E(159, x_KeWaitForSingleObject), E(160, x_KfRaiseIrql), E(161, x_KfLowerIrql),
    E(164, x_LaunchDataPage), E(165, x_MmAllocateContiguousMemory), E(166, x_MmAllocateContiguousMemoryEx),
    E(167, x_MmAllocateSystemMemory), E(168, x_MmClaimGpuInstanceMemory), E(171, x_MmFreeContiguousMemory),
    E(172, x_MmFreeSystemMemory), E(173, x_MmGetPhysicalAddress), E(175, x_MmLockUnlockBufferPages),
    E(176, x_MmLockUnlockPhysicalPage), E(178, x_MmPersistContiguousMemory), E(179, x_MmQueryAddressProtect),
    E(180, x_MmQueryAllocationSize), E(182, x_MmSetAddressProtect), E(184, x_NtAllocateVirtualMemory),
    E(186, x_NtClearEvent), E(187, x_NtClose), E(189, x_NtCreateEvent), E(190, x_NtCreateFile),
    E(196, x_NtDeviceIoControlFile), E(198, x_NtFlushBuffersFile), E(199, x_NtFreeVirtualMemory), E(200, x_NtFsControlFile),
    E(202, x_NtOpenFile), E(203, x_NtOpenSymbolicLinkObject), E(204, x_NtProtectVirtualMemory),
    E(207, x_NtQueryDirectoryFile), E(211, x_NtQueryInformationFile), E(215, x_NtQuerySymbolicLinkObject),
    E(217, x_NtQueryVirtualMemory), E(218, x_NtQueryVolumeInformationFile), E(219, x_NtReadFile),
    E(224, x_NtResumeThread), E(225, x_NtSetEvent), E(226, x_NtSetInformationFile), E(231, x_NtSuspendThread),
    E(232, x_NtUserIoApcDispatcher), E(233, x_NtWaitForSingleObject), E(234, x_NtWaitForSingleObjectEx),
    E(235, x_NtWaitForMultipleObjectsEx), E(236, x_NtWriteFile), E(246, x_ObReferenceObjectByHandle),
    E(250, x_ObfDereferenceObject), E(255, x_PsCreateSystemThreadEx), E(258, x_PsTerminateSystemThread),
    E(259, x_PsThreadObjectType), E(269, x_RtlCompareMemoryUlong), E(277, x_RtlEnterCriticalSection),
    E(279, x_RtlEqualString), E(289, x_RtlInitAnsiString), E(291, x_RtlInitializeCriticalSection),
    E(294, x_RtlLeaveCriticalSection), E(301, x_RtlNtStatusToDosError), E(304, x_RtlTimeFieldsToTime),
    E(305, x_RtlTimeToTimeFields), E(322, x_XboxHardwareInfo), E(323, x_XboxHDKey), E(325, x_XboxSignatureKey),
    E(327, x_XeLoadSection), E(328, x_XeUnloadSection), E(335, x_XcSHAInit), E(336, x_XcSHAUpdate),
    E(337, x_XcSHAFinal), E(354, x_XboxAlternateSignatureKeys), E(356, x_HalBootSMCVideoMode),
    E(358, x_HalIsResetOrShutdownPending), E(359, x_IoMarkIrpMustComplete), E(360, x_HalInitiateShutdown),
};

uintptr_t kernel_resolve(uint32_t ord)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (ord == 302) return (uintptr_t)GetProcAddress(nt, "RtlRaiseException");
    if (ord == 312) return (uintptr_t)GetProcAddress(nt, "RtlUnwind");
    for (size_t i = 0; i < sizeof g_exports / sizeof *g_exports; i++)
        if (g_exports[i].ord == ord) return (uintptr_t)g_exports[i].addr;
    return 0;
}

void kernel_init(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    p_RtlNtStatusToDosError = (void *)GetProcAddress(nt, "RtlNtStatusToDosError");
    p_RtlTimeToTimeFields = (void *)GetProcAddress(nt, "RtlTimeToTimeFields");
    p_RtlTimeFieldsToTime = (void *)GetProcAddress(nt, "RtlTimeFieldsToTime");
    InitializeCriticalSection(&g_dispatch_lock);
    InitializeCriticalSection(&g_device_lock);
    InitializeCriticalSection(&g_obj_lock);
    InitializeCriticalSection(&g_file_lock);
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_qpc0);
    MMRESULT tr = timeBeginPeriod(1);
    {   /* report the real sleep granularity: every Xbox wait maps onto it */
        LARGE_INTEGER a, b, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        for (int i = 0; i < 10; i++) Sleep(1);
        QueryPerformanceCounter(&b);
        xlog("timer: timeBeginPeriod(1) -> %u, Sleep(1) takes %.2f ms", tr, (double)(b.QuadPart - a.QuadPart) * 100.0 / (double)f.QuadPart);
    }
    g_kwork_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    host_create_thread(0x10000, tick_thread, NULL, 0, NULL);
    kfile_init();
    XThread *w = xthread_create(kernel_worker, NULL, NULL, 0x100000, 0, FALSE);
    xthread_pin_game(w);
    /* DPCs preempt every thread on the Xbox; TIME_CRITICAL is the closest
     * a normal-class process gets (Windows schedules the shared core by strict priority). */
    SetThreadPriority(w->host_handle, THREAD_PRIORITY_TIME_CRITICAL);
}
