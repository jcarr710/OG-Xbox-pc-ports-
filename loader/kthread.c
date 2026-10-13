/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Xbox-style threads on Win32.
 *
 * Xbox titles find their TLS block at  [StackBase - TlsDataSize]  via fs:[4],
 * i.e. the TLS lives at the very top of each thread's stack.  Windows owns the
 * top of a normal thread stack, so every game thread runs on a stack we
 * allocate ourselves, with the TEB stack bounds pointed at it.  This keeps
 * fs:[4]-relative TLS, SEH stack-range checks and __chkstk all consistent. */
#include "xbox.h"
#include <setjmp.h>

typedef struct {
    void *ExceptionList; void *StackBase; void *StackLimit; void *SubSystemTib;
    void *FiberData; void *ArbitraryUserPointer; void *Self;
} TIB32;
#define TEB_DEALLOCATION_STACK 0xE0C

static inline TIB32 *tib(void) { TIB32 *t; __asm__("movl %%fs:0x18, %0" : "=r"(t)); return t; }

XThread *xthread_current(void)
{
    X_FAKE_KPCR *p = (X_FAKE_KPCR *)tib()->ArbitraryUserPointer;
    return (XThread *)p; /* pcr is the first member */
}

/* call a stdcall/cdecl game function with N dword args from a C context,
 * restoring esp afterwards regardless of the callee's convention. */
uint32_t xcall(void *fn, int n, const uint32_t *args);
__asm__(
    ".globl _xcall\n"
    "_xcall:\n"
    "  pushl %ebp\n  movl %esp, %ebp\n  pushl %esi\n  pushl %edi\n  pushl %ebx\n"
    "  movl 12(%ebp), %ecx\n  movl 16(%ebp), %esi\n"
    "1: testl %ecx, %ecx\n  jz 2f\n  pushl -4(%esi,%ecx,4)\n  decl %ecx\n  jmp 1b\n"
    "2: movl %esp, %ebx\n" /* ebx is callee-saved; not used to restore, ebp is */
    "  call *8(%ebp)\n"
    "  leal -12(%ebp), %esp\n  popl %ebx\n  popl %edi\n  popl %esi\n  popl %ebp\n  ret\n");

typedef struct { void *next; void *handler; } SEH_REC;

void xbacktrace(void);
/* Last-resort SEH handler at the bottom of every game stack. */
static EXCEPTION_DISPOSITION __cdecl final_seh(EXCEPTION_RECORD *r, void *frame, CONTEXT *c, void *dc)
{
    if (r->ExceptionFlags & 6) return ExceptionContinueSearch; /* unwinding */
    EXCEPTION_POINTERS ep = { r, c };
    LPTOP_LEVEL_EXCEPTION_FILTER f = SetUnhandledExceptionFilter(NULL);
    SetUnhandledExceptionFilter(f);
    if (f) f(&ep);
    ExitProcess(1);
}

static __thread jmp_buf *t_exit_jmp;

void xthread_exit_current(void)
{
    if (t_exit_jmp) __builtin_longjmp(*t_exit_jmp, 1);
    ExitThread(0);
}

/* runs on the game stack */
static void __cdecl xthread_body(XThread *t, SEH_REC *final_rec)
{
    jmp_buf jb;
    tib()->ExceptionList = final_rec;
    if (__builtin_setjmp(jb) == 0) {
        t_exit_jmp = &jb;
        if (t->system_routine) {
            uint32_t a[2] = { (uint32_t)t->start, (uint32_t)t->context };
            xcall(t->system_routine, 2, a);
        } else {
            uint32_t a[1] = { (uint32_t)t->context };
            xcall(t->start, 1, a);
        }
    }
    t_exit_jmp = NULL;
    tib()->ExceptionList = final_rec;
}

static void switch_and_run(void *sp, XThread *t, SEH_REC *rec)
{
    __asm__ volatile(
        "movl %%esp, %%edi\n\t"
        "movl %0, %%esp\n\t"
        "pushl %3\n\t"
        "pushl %2\n\t"
        "call *%1\n\t"
        "movl %%edi, %%esp\n\t"
        :: "a"(sp), "b"(xthread_body), "c"(t), "d"(rec)
        : "edi", "esi", "memory", "cc");
}

static DWORD WINAPI host_entry(LPVOID param)
{
    XThread *t = param;
    TIB32 *tb = tib();
    tb->ArbitraryUserPointer = &t->pcr;

    /* original values to restore */
    void *old_list = tb->ExceptionList, *old_base = tb->StackBase, *old_limit = tb->StackLimit;
    void *old_dealloc = *(void **)((char *)tb + TEB_DEALLOCATION_STACK);

    /* SEHOP wants the chain to terminate in ntdll's final handler: copy it. */
    SEH_REC *last = old_list == (void *)-1 ? NULL : old_list;
    for (int i = 0; last && last->next != (void *)-1 && i < 64; i++) last = last->next;
    if (last && last->next != (void *)-1) last = NULL;

    uint32_t tls = (t->tls_size + 15) & ~15u;
    SIZE_T total = t->stack_size + tls + 0x1000;
    total = (total + 0xFFFF) & ~(SIZE_T)0xFFFF;
    char *mem = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) xfatal("thread stack alloc failed (%lu bytes)", (unsigned long)total);
    t->stack_mem = mem;
    char *top = mem + total;
    char *tls_base = top - t->tls_size;
    t->ethread.TlsData = t->tls_size ? tls_base : NULL;

    /* Exception chain on the new stack: [final_seh] -> [copy of the host's
     * terminal record] -> end. Windows' SEHOP (on by default in Windows 10/11)
     * only dispatches to frame handlers if the chain ends in ntdll's final
     * handler; final_seh sits in front of it so it handles every unhandled
     * exception and the host handler is only ever reached while unwinding
     * (Wine's handler cannot run from a copied frame). */
    char *sp = (char *)(((uintptr_t)tls_base - 64) & ~(uintptr_t)15);
    sp -= 2 * sizeof(SEH_REC);
    SEH_REC *rec = (SEH_REC *)sp, *term = rec + 1;
    term->next = (void *)-1;
    term->handler = last ? last->handler : (void *)final_seh;
    rec->next = term;
    rec->handler = (void *)final_seh;
    sp = (char *)((uintptr_t)sp & ~(uintptr_t)15);

    tb->StackBase = top;
    tb->StackLimit = mem;
    *(void **)((char *)tb + TEB_DEALLOCATION_STACK) = mem;

    switch_and_run(sp, t, rec);

    tb->ExceptionList = old_list;
    tb->StackBase = old_base;
    tb->StackLimit = old_limit;
    *(void **)((char *)tb + TEB_DEALLOCATION_STACK) = old_dealloc;
    VirtualFree(mem, 0, MEM_RELEASE);
    return 0;
}

XThread *g_threads[64]; volatile LONG g_nthreads;

/* Threads that run title/XDK code share one core (the Xbox is uniprocessor);
 * emulated-hardware and renderer threads run wherever Windows likes. */
DWORD_PTR g_game_affinity = 1;
void xthread_pin_game(XThread *t)
{
    if (g_game_affinity && t && t->host_handle) SetThreadAffinityMask(t->host_handle, g_game_affinity);
}

XThread *xthread_create(void (__stdcall *start)(void *), void *ctx,
                        void (__stdcall *sysroutine)(void *, void *),
                        SIZE_T stack, uint32_t tls, BOOL suspended)
{
    XThread *t = VirtualAlloc(NULL, sizeof(XThread), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    t->pcr.Prcb = &t->pcr.CurrentThread;
    t->pcr.CurrentThread = &t->ethread;
    t->pcr.Irql = 0;
    t->start = start; t->context = ctx; t->system_routine = sysroutine;
    if (stack < 0x100000) stack = 0x100000; /* host drivers need real stack */
    t->stack_size = stack;
    t->tls_size = tls;
    /* small native stack: we immediately switch off it */
    t->host_handle = host_create_thread(0x10000, host_entry, t, CREATE_SUSPENDED, &t->host_tid);
    if (!t->host_handle) xfatal("CreateThread failed: %lu", GetLastError());
    t->ethread.UniqueThread = t->host_tid;
    LONG n = InterlockedIncrement(&g_nthreads);
    if (n <= 64) g_threads[n - 1] = t;
    if (!suspended) ResumeThread(t->host_handle);
    return t;
}

/* Give a host thread (not created by the game) a fake KPCR so that kernel
 * HLE code and patched fs: reads work if it ever runs game code. */
void xthread_adopt_current(void)
{
    if (tib()->ArbitraryUserPointer) return;
    XThread *t = VirtualAlloc(NULL, sizeof(XThread), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    t->pcr.Prcb = &t->pcr.CurrentThread;
    t->pcr.CurrentThread = &t->ethread;
    t->host_tid = GetCurrentThreadId();
    t->ethread.UniqueThread = t->host_tid;
    tib()->ArbitraryUserPointer = &t->pcr;
}
