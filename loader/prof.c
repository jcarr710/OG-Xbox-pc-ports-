/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sampling profiler (--profile): every millisecond each emulator-managed
 * thread is briefly suspended and its instruction pointer recorded. Every ten
 * seconds the log gets, per thread, its CPU use and the addresses it was most
 * often found at ("xbe:" = game/XDK code, "jsrf:" = this program, otherwise a
 * Windows DLL + offset; time inside ntdll is mostly waiting). */
#include "xbox.h"
#include <stdio.h>
#include <string.h>

extern XThread *g_threads[64];
extern volatile LONG g_nthreads;

#define PROF_SLOTS 4096
typedef struct { uint32_t eip, n; } Hit;
typedef struct { Hit h[PROF_SLOTS]; uint32_t samples; ULONGLONG cpu0; } ThreadProf;
static ThreadProf *g_prof[64];

static void add(ThreadProf *p, uint32_t eip)
{
    uint32_t i = (eip * 2654435761u) >> 20;
    for (int k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        if (p->h[i].eip == eip) { p->h[i].n++; return; }
        if (!p->h[i].eip) { p->h[i].eip = eip; p->h[i].n = 1; return; }
    }
}

static void describe(uint32_t eip, char *out, size_t n)
{
    if (eip >= 0x10000 && eip < 0x289000) { snprintf(out, n, "xbe:%08x", eip); return; }
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)eip, &m) && m) {
        if ((uintptr_t)m == 0x10000) { snprintf(out, n, "jsrf:%08x", eip); return; }
        char path[MAX_PATH]; GetModuleFileNameA(m, path, sizeof path);
        const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
        snprintf(out, n, "%s+%x", b, eip - (uint32_t)(uintptr_t)m);
    } else snprintf(out, n, "%08x", eip);
}

static ULONGLONG thread_cpu(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static void report(double secs)
{
    LONG n = g_nthreads; if (n > 64) n = 64;
    for (LONG i = 0; i < n; i++) {
        ThreadProf *p = g_prof[i]; XThread *x = g_threads[i];
        if (!p || !x || !p->samples) continue;
        ULONGLONG cpu = thread_cpu(x->host_handle);
        double pct = (double)(cpu - p->cpu0) / (secs * 1e7) * 100.0;
        p->cpu0 = cpu;
        char line[1024]; int o = 0;
        o += snprintf(line + o, sizeof line - o, "profile: thread %lu (start %p, prio %d) cpu %.0f%%:", x->host_tid, x->start,
                      GetThreadPriority(x->host_handle), pct);
        for (int r = 0; r < 6; r++) {
            int best = -1;
            for (int k = 0; k < PROF_SLOTS; k++) if (p->h[k].n && (best < 0 || p->h[k].n > p->h[best].n)) best = k;
            if (best < 0) break;
            char d[96]; describe(p->h[best].eip, d, sizeof d);
            o += snprintf(line + o, sizeof line - o, " %s %.0f%%", d, 100.0 * p->h[best].n / p->samples);
            p->h[best].n = 0;
        }
        xlog("%s", line);
        memset(p->h, 0, sizeof p->h); p->samples = 0;
    }
}

static DWORD WINAPI prof_thread(LPVOID arg)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(1);
        LONG n = g_nthreads; if (n > 64) n = 64;
        for (LONG i = 0; i < n; i++) {
            XThread *x = g_threads[i];
            if (!x || !x->host_handle) continue;
            if (!g_prof[i]) {
                g_prof[i] = VirtualAlloc(NULL, sizeof(ThreadProf), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (!g_prof[i]) continue;
                g_prof[i]->cpu0 = thread_cpu(x->host_handle);
            }
            /* nothing that can take a lock between suspend and resume */
            if (SuspendThread(x->host_handle) == (DWORD)-1) continue;
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(x->host_handle, &c);
            ResumeThread(x->host_handle);
            if (ok) { add(g_prof[i], c.Eip); g_prof[i]->samples++; }
        }
        QueryPerformanceCounter(&t);
        double secs = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (secs >= 10.0) { report(secs); t0 = t; }
    }
    return 0;
}

void prof_start(void)
{
    host_create_thread(0, prof_thread, NULL, 0, NULL);
    xlog("profile: sampling title threads every 1 ms, report every 10 s");
}
