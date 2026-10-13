/* SPDX-License-Identifier: GPL-2.0-or-later */
/* jsrf.exe — runs Jet Set Radio Future's default.xbe natively on Windows.
 *
 * The Xbox CPU is a Pentium III, so the game's x86 code runs as-is.  This
 * file reserves the Xbox's fixed address ranges, checks and maps the XBE at
 * its original address (0x10000), resolves its kernel imports to the Win32
 * implementations in kernel.c, patches the handful of instructions that read
 * Xbox-only per-CPU state, installs the XDK replacements (audio and input;
 * the GPU is emulated at register level in nv2a/), and starts the title.
 * Also here: command-line options, the log, and crash reporting. */
#include "xbox.h"
#include "version.h"
#include "xmem.h"
#include "mmio.h"
#include "hle.h"
#include "settings.h"
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define XBE_BASE 0x10000u
#define XBE_LIMIT 0x289000u

XBE_HEADER *g_xbe;
char g_game_dir[MAX_PATH];
static FILE *g_log;
static CRITICAL_SECTION g_log_lock;
extern int g_trace, g_nv_trace, g_apu_trace, g_pgraph_log;
extern uint32_t *g_nv_regs;
extern int g_scale, g_window_w, g_window_h, g_screenshot_every, g_scale_user, g_window_user;
extern char g_screenshot_dir[MAX_PATH];
extern int g_dump_frame;
static int g_game_core = -1;
static int g_profile;
void prof_start(void);
void logwin_append(const char *s, int n);
void logwin_show(int on);

void xlog(const char *fmt, ...)
{
    char buf[2048]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    /* integer time: xlog can run inside game code, whose x87 state it must not touch */
    static LARGE_INTEGER f0, qf;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); f0 = now; }
    uint64_t d = (uint64_t)(now.QuadPart - f0.QuadPart), f = (uint64_t)qf.QuadPart;
    unsigned long s = (unsigned long)(d / f), ms = (unsigned long)((d % f) * 1000 / f);
    char line[2100];
    int n = snprintf(line, sizeof line, "%4lu.%03lu [%5lu] %s\n", s, ms, GetCurrentThreadId(), buf);
    if (n < 0) return;
    if (n >= (int)sizeof line) { n = (int)sizeof line - 1; line[n - 1] = '\n'; }
    EnterCriticalSection(&g_log_lock);
    fputs(line, stderr);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    logwin_append(line, n);
    LeaveCriticalSection(&g_log_lock);
}

void xbacktrace(void)
{
    uint32_t *fp = __builtin_frame_address(0); char buf[512]; int o = 0;
    for (int i = 0; i < 16 && fp && !IsBadReadPtr(fp, 8); i++) {
        o += snprintf(buf + o, sizeof buf - o, "%08x ", fp[1]);
        if ((uint32_t *)fp[0] <= fp) break;
        fp = (uint32_t *)fp[0];
    }
    xlog("  backtrace: %s", buf);
}

void xfatal(const char *fmt, ...)
{
    char buf[2048]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    xlog("FATAL: %s", buf);
    MessageBoxA(NULL, buf, JSRF_PRODUCT, MB_ICONERROR);
    ExitProcess(1);
}

void __cdecl unimpl_kernel_call(uint32_t ordinal) /* via _unimpl_entry */
{
    xfatal("Unimplemented xboxkrnl ordinal %u called", ordinal);
}
extern char unimpl_entry[];
extern char fs_read_20[], fs_read_28[], fs_read_58[], fs_read_24b[], hle_rdtsc[];
BOOL __stdcall hle_QueryPerformanceCounter(LARGE_INTEGER *);

static void *map_range(uint32_t va, uint32_t size)
{
    DWORD old;
    if (va < XBE_BASE || va + size > XBE_LIMIT) xfatal("XBE range %08x+%x outside reservation", va, size);
    if (!VirtualProtect((void *)va, size, PAGE_EXECUTE_READWRITE, &old))
        xfatal("VirtualProtect(%08x, %x) failed: %lu", va, size, GetLastError());
    return (void *)va;
}

static void write_jmp(uint32_t at, void *to, uint8_t op /*0xE8 call, 0xE9 jmp*/, int total_len)
{
    uint8_t *p = (uint8_t *)at;
    p[0] = op;
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)to - (at + 5));
    for (int i = 5; i < total_len; i++) p[i] = 0x90;
}

static const char *sec_name(XBE_SECTION *s) { return (const char *)s->SectionName; }

/* Replace reads of Xbox KPCR fields through fs: with calls to helpers. */
static void patch_fs(void)
{
    XBE_SECTION *secs = (XBE_SECTION *)g_xbe->SectionHeadersAddress;
    int n = 0;
    for (uint32_t i = 0; i < g_xbe->NumberOfSections; i++) {
        XBE_SECTION *s = &secs[i];
        if (!(s->Flags & 4) || !strcmp(sec_name(s), "DOLBY")) continue; /* DOLBY = DSP microcode */
        uint8_t *p = (uint8_t *)s->VirtualAddress, *end = p + s->SizeOfRawData - 8;
        for (; p < end; p++) {
            if (p[0] != 0x64) continue;
            if (p[1] == 0xA1 && p[3] == 0 && p[4] == 0 && p[5] == 0) {          /* mov eax, fs:[imm8] */
                void *h = p[2] == 0x20 ? fs_read_20 : p[2] == 0x28 ? fs_read_28 : p[2] == 0x58 ? fs_read_58 : NULL;
                if (!h) continue;
                write_jmp((uint32_t)p, h, 0xE8, 6); n++; p += 5;
            } else if (p[1] == 0x0F && p[2] == 0xB6 && p[3] == 0x05 && p[4] == 0x24 && !p[5] && !p[6] && !p[7]) {
                write_jmp((uint32_t)p, fs_read_24b, 0xE8, 8); n++; p += 7; /* movzx eax, byte fs:[0x24] */
            }
        }
    }
    xlog("patched %d Xbox fs: accesses", n);
}

/* Game-specific patches, each guarded by a byte signature. */
typedef struct { uint32_t va; const char *sig; int siglen; void *target; const char *what; } Patch;
static void apply_patches(void)
{
    static const Patch patches[] = {
        { 0x145560, "\x8b\x4c\x24\x04\x0f\x31\x89\x01", 8, (void *)hle_QueryPerformanceCounter, "XAPI QueryPerformanceCounter" },
        { 0x15fa20, "\x0f\x31\xc3\xeb\x0b", 5, hle_rdtsc, "game rdtsc helper" },
    };
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) {
        const Patch *p = &patches[i];
        if (memcmp((void *)p->va, p->sig, p->siglen)) { xlog("WARNING: patch '%s' signature mismatch, skipped", p->what); continue; }
        write_jmp(p->va, p->target, 0xE9, 5);
        xlog("patched %s @ %08x", p->what, p->va);
    }
}

/* ---- the shared header page at 0x10000 ---- */
static uint8_t g_pe_page[0x1000];          /* our original PE header page */
static CRITICAL_SECTION g_hdr_lock;
static volatile int g_hdr_swap;            /* thread creation needs the original page swapped in */

static DWORD WINAPI nop_thread(LPVOID p) { return 0; }
static DWORD try_thread(void)
{
    HANDLE t = CreateThread(NULL, 0, nop_thread, NULL, 0, NULL);
    if (!t) return GetLastError();
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

/* Copy our NT headers into the free tail of the XBE header page and point
 * e_lfanew at them. Falls back to a copy inside our image if they don't fit. */
static void place_pe_headers(uint8_t *hdr, uint32_t xbe_hdr_size)
{
    static uint8_t far_copy[0x1000] __attribute__((aligned(64)));
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_pe_page;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) xfatal("jsrf.exe's own PE header is missing");
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_pe_page + dos->e_lfanew);
    uint32_t n = (uint32_t)(offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader +
                            nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER));
    uint32_t at = (xbe_hdr_size + 0x3F) & ~0x3Fu;
    uint8_t *dst = at + n <= 0x1000 ? hdr + at : far_copy;
    memcpy(dst, nt, n);
    IMAGE_NT_HEADERS *moved = (IMAGE_NT_HEADERS *)dst;
    if (dst != far_copy && moved->OptionalHeader.SizeOfHeaders < 0x1000) moved->OptionalHeader.SizeOfHeaders = 0x1000;
    hdr[0] = 'M'; hdr[1] = 'Z';
    *(LONG *)(hdr + offsetof(IMAGE_DOS_HEADER, e_lfanew)) = (LONG)((uintptr_t)dst - XBE_BASE);
}

/* Thread creation fails after the header swap: work out what Windows wants. */
static void header_diagnostics(uint8_t *hdr, DWORD err)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    typedef PIMAGE_NT_HEADERS (NTAPI *PFN_RINH)(PVOID);
    typedef LONG (NTAPI *PFN_RINHEX)(ULONG, PVOID, ULONG64, PIMAGE_NT_HEADERS *);
    PFN_RINH rinh = (PFN_RINH)GetProcAddress(ntdll, "RtlImageNtHeader");
    PFN_RINHEX rinhex = (PFN_RINHEX)GetProcAddress(ntdll, "RtlImageNtHeaderEx");
    PIMAGE_NT_HEADERS a = rinh ? rinh(hdr) : NULL, b = NULL, c = NULL;
    LONG sb = rinhex ? rinhex(0, hdr, 0x1000, &b) : -1, sc = rinhex ? rinhex(0, hdr, 0x600000, &c) : -1;
    xlog("header diag: CreateThread error %lu; RtlImageNtHeader -> %p; Ex(page) %08lx %p; Ex(image) %08lx %p",
         err, a, sb, b, sc, c);
    static uint8_t saved[0x1000];
    memcpy(saved, hdr, 0x1000);
    /* (1) original NT headers back at their original offset, XBE data elsewhere intact */
    LONG lf = ((IMAGE_DOS_HEADER *)g_pe_page)->e_lfanew;
    memcpy(hdr + lf, g_pe_page + lf, 0x400);
    *(LONG *)(hdr + 0x3C) = lf;
    DWORD e1 = try_thread();
    memcpy(hdr, saved, 0x1000);
    /* (2) the whole original page */
    memcpy(hdr, g_pe_page, 0x1000);
    DWORD e2 = try_thread();
    memcpy(hdr, saved, 0x1000);
    xlog("header diag: NT headers at the original offset -> %lu; whole original page -> %lu", e1, e2);
    if (!e2) {
        g_hdr_swap = 1;
        xlog("header page: swapping the PE header in for thread creation");
    }
}

/* All host threads are created through here (see header_diagnostics). */
HANDLE host_create_thread(SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid)
{
    if (!g_hdr_swap) return CreateThread(NULL, stack, fn, arg, flags, tid);
    static uint8_t xbe_page[0x1000];
    EnterCriticalSection(&g_hdr_lock);
    memcpy(xbe_page, (void *)XBE_BASE, 0x1000);
    memcpy((void *)XBE_BASE, g_pe_page, 0x1000);
    HANDLE t = CreateThread(NULL, stack, fn, arg, flags, tid);
    DWORD e = GetLastError();
    memcpy((void *)XBE_BASE, xbe_page, 0x1000);
    LeaveCriticalSection(&g_hdr_lock);
    SetLastError(e);
    return t;
}

/* jsrf.exe changes the game's code at fixed addresses, so it only runs the
 * build it was written for; anything else is refused with an explanation
 * instead of crashing somewhere obscure. --any-xbe skips the check. */
static int g_any_xbe;
static void check_version(const uint8_t *file, DWORD size, const char *path)
{
    SHA1C c; uint8_t d[20]; char hex[41];
    x_XcSHAInit(&c); x_XcSHAUpdate(&c, file, size); x_XcSHAFinal(&c, d);
    for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    const XBE_HEADER *h = (const XBE_HEADER *)file;
    uint32_t cert = h->CertificateAddress - h->BaseAddress, title = 0, region = 0, version = 0;
    if (cert < size && size - cert >= 0xB0) {
        title = *(const uint32_t *)(file + cert + 0x08);
        region = *(const uint32_t *)(file + cert + 0xA0);
        version = *(const uint32_t *)(file + cert + 0xAC);
    }
    xlog("default.xbe: SHA-1 %s, title ID %08X, region %X, version %X", hex, title, region, version);
    if (!strcmp(hex, JSRF_XBE_SHA1)) return;
    if (g_any_xbe) { xlog("WARNING: not the supported build of the game; continuing because of --any-xbe"); return; }
    const char *what = title != JSRF_XBE_TITLE_ID ? "This isn't Jet Set Radio Future" :
                       region != 1 ? "This is a different region's release of Jet Set Radio Future" :
                                     "This is a different (or modified) build of Jet Set Radio Future";
    xfatal("%s:\n%s\n(title ID %08X, region %X, version %X, SHA-1 %s)\n\n"
           "%s %s runs exactly one build of the game, the North American Xbox release, "
           "whose default.xbe has the SHA-1\n%s\n\n"
           "It changes code at fixed addresses, which are different in other builds.",
           what, path, title, region, version, hex, JSRF_PRODUCT, JSRF_VERSION_STR, JSRF_XBE_SHA1);
}

static void load_xbe(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) xfatal("Cannot open %s\n\nPut jsrf.exe next to default.xbe (the extracted game folder).", path);
    DWORD size = GetFileSize(f, NULL), got;
    uint8_t *file = malloc(size);
    if (!ReadFile(f, file, size, &got, NULL) || got != size) xfatal("Read failed: %s", path);
    CloseHandle(f);

    XBE_HEADER *h = (XBE_HEADER *)file;
    if (size < 0x1000 || h->Magic != 0x48454258) xfatal("%s is not an Xbox executable (XBE).", path);
    check_version(file, size, path);
    if (h->BaseAddress != XBE_BASE) xfatal("Unexpected XBE base %08x", h->BaseAddress);
    if (h->BaseAddress + h->SizeOfImage > XBE_LIMIT) xfatal("XBE image too large (%x)", h->SizeOfImage);

    /* The XBE header goes to 0x10000, over our own PE header page: XAPI reads
     * it there (certificate, section table, stack/heap sizes). Windows still
     * parses our PE header at runtime though (thread creation reads the stack
     * sizes from it, exception dispatch the SafeSEH table, the loader the TLS
     * directory), so the NT headers are moved into the unused tail of the page
     * and the two DOS fields Windows uses, e_magic and e_lfanew, are written
     * over parts of the XBE header nothing reads (magic and RSA signature). */
    memcpy(g_pe_page, (void *)XBE_BASE, 0x1000);
    DWORD e_before = try_thread();
    uint8_t *hdr = map_range(XBE_BASE, 0x1000);
    memcpy(hdr, file, h->SizeOfHeaders);
    place_pe_headers(hdr, h->SizeOfHeaders);
    DWORD e_after = try_thread();
    xlog("header page: PE headers at +%lx; thread creation %s before, %s after", (unsigned long)((IMAGE_DOS_HEADER *)hdr)->e_lfanew,
         e_before ? "FAILS" : "works", e_after ? "FAILS" : "works");
    if (e_after) header_diagnostics(hdr, e_after);
    g_xbe = (XBE_HEADER *)XBE_BASE;
    map_range(XBE_BASE + 0x1000, XBE_LIMIT - XBE_BASE - 0x1000);

    XBE_SECTION *secs = (XBE_SECTION *)g_xbe->SectionHeadersAddress;
    for (uint32_t i = 0; i < g_xbe->NumberOfSections; i++) {
        XBE_SECTION *s = &secs[i];
        memset((void *)s->VirtualAddress, 0, s->VirtualSize);
        memcpy((void *)s->VirtualAddress, file + s->PointerToRawData, s->SizeOfRawData);
        xlog("section %-10s %08x-%08x flags %x", sec_name(s), s->VirtualAddress, s->VirtualAddress + s->VirtualSize, s->Flags);
    }

    /* Entry point / kernel thunk are XOR-scrambled; retail vs debug keys. */
    uint32_t entry = g_xbe->EntryPoint ^ XBE_RETAIL_ENTRY_KEY;
    uint32_t thunk = g_xbe->KernelThunkAddress ^ XBE_RETAIL_THUNK_KEY;
    if (entry < XBE_BASE || entry >= XBE_LIMIT) {
        entry = g_xbe->EntryPoint ^ XBE_DEBUG_ENTRY_KEY;
        thunk = g_xbe->KernelThunkAddress ^ XBE_DEBUG_THUNK_KEY;
    }
    g_xbe->EntryPoint = entry; g_xbe->KernelThunkAddress = thunk; /* store unscrambled for later */

    uint32_t *t = (uint32_t *)thunk; int nimp = 0, nmiss = 0;
    uint8_t *stubs = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    for (; *t; t++, nimp++) {
        uint32_t ord = *t & 0x7FFFFFFF;
        uintptr_t a = kernel_resolve(ord);
        if (!a) {
            uint8_t *s = stubs + nmiss++ * 10;           /* push imm32; jmp _unimpl_entry */
            s[0] = 0x68; *(uint32_t *)(s + 1) = ord;
            s[5] = 0xE9; *(int32_t *)(s + 6) = (int32_t)((uintptr_t)unimpl_entry - (uintptr_t)(s + 10));
            a = (uintptr_t)s;
            xlog("kernel ordinal %u not implemented (stubbed)", ord);
        }
        *t = (uint32_t)a;
    }
    xlog("resolved %d kernel imports (%d missing)", nimp, nmiss);
    free(file);
}

/* Persistent call tracing (--trace-call=ADDR): int3 at a function entry whose
 * first instruction is `push r32` or `sub esp, imm8`; the handler logs the
 * arguments and emulates that instruction. */
static uint32_t g_tc_addr[16]; static uint8_t g_tc_orig[16][3]; static int g_ntc;
extern uint32_t *g_nv_regs;
static LONG CALLBACK tc_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    for (int i = 0; i < g_ntc; i++) {
        uint32_t a = g_tc_addr[i];
        if (c->Eip != a && c->Eip != a + 1) continue;
        uint32_t *sp = (uint32_t *)c->Esp;
        static LONG n; if (InterlockedIncrement(&n) < 20000) {
            uint32_t dev = *(uint32_t *)0x19dce0;
            xlog("CALL %08x from %08x args %08x %08x %08x | dev put %08x time %08x fence %08x | GET %08x PUT %08x SUB %08x",
                 a, sp[0], sp[1], sp[2], sp[3], dev ? *(uint32_t *)dev : 0, dev ? *(uint32_t *)(dev + 0x30) : 0,
                 dev ? **(uint32_t **)(dev + 0x34) : 0, g_nv_regs[0x3244 / 4], g_nv_regs[0x3240 / 4], g_nv_regs[0x324C / 4]);
        }
        uint8_t *o = g_tc_orig[i];
        if (o[0] >= 0x50 && o[0] <= 0x57) {          /* push r32 */
            uint32_t v = *(&c->Edi + 0);              /* placeholder */
            switch (o[0] & 7) { case 0: v = c->Eax; break; case 1: v = c->Ecx; break; case 2: v = c->Edx; break; case 3: v = c->Ebx; break;
                                case 4: v = c->Esp; break; case 5: v = c->Ebp; break; case 6: v = c->Esi; break; default: v = c->Edi; }
            c->Esp -= 4; *(uint32_t *)c->Esp = v; c->Eip = a + 1;
        } else if (o[0] == 0x83 && o[1] == 0xEC) {    /* sub esp, imm8 */
            c->Esp -= o[2]; c->Eip = a + 3;            /* (flags not updated) */
        } else c->Eip = a;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void tc_arm(void)
{
    for (int i = 0; i < g_ntc; i++) {
        uint8_t *p = (uint8_t *)g_tc_addr[i];
        memcpy(g_tc_orig[i], p, 3);
        if (!((p[0] >= 0x50 && p[0] <= 0x57) || (p[0] == 0x83 && p[1] == 0xEC))) { xlog("trace-call %08x: unsupported first instruction", g_tc_addr[i]); continue; }
        p[0] = 0xCC;
    }
    if (g_ntc) AddVectoredExceptionHandler(1, tc_veh);
}

/* One-shot breakpoints (--bp=ADDR): log registers and stack once. */
static uint32_t g_bp_addr[32]; static uint8_t g_bp_orig[32]; static int g_nbp;
static void bp_add(uint32_t a) { if (g_nbp < 32) g_bp_addr[g_nbp++] = a; }
static void bp_arm(void)
{
    for (int i = 0; i < g_nbp; i++) { uint8_t *p = (uint8_t *)g_bp_addr[i]; g_bp_orig[i] = *p; *p = 0xCC; }
}
static LONG CALLBACK bp_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    for (int i = 0; i < g_nbp; i++) if (c->Eip == g_bp_addr[i] || c->Eip == g_bp_addr[i] + 1) {
        c->Eip = g_bp_addr[i];
        *(uint8_t *)g_bp_addr[i] = g_bp_orig[i];
        uint32_t *sp = (uint32_t *)c->Esp; char st[400]; int o = 0;
        for (int k = 0; k < 12; k++) o += snprintf(st + o, sizeof st - o, "%08x ", sp[k]);
        xlog("BP %08lx: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx | %s",
             c->Eip, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, st);
        if (!IsBadReadPtr((void *)c->Esi, 0x60)) {
            uint32_t *q = (uint32_t *)c->Esi; o = 0;
            for (int k = 0; k < 24; k++) o += snprintf(st + o, sizeof st - o, "%08x ", q[k]);
            xlog("   [esi]: %s", st);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    static volatile LONG count;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && mmio_is_device((uint32_t)ep->ExceptionRecord->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION) {
        if (InterlockedIncrement(&count) <= 50) {
            CONTEXT *c = ep->ContextRecord;
            xlog("exception %08lx at eip=%08lx (%s %08lx)  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx",
                 code, c->Eip,
                 ep->ExceptionRecord->NumberParameters > 1 ? (ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read") : "",
                 ep->ExceptionRecord->NumberParameters > 1 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0,
                 c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp);
            uint32_t *sp = (uint32_t *)c->Esp; char st[512]; int o = 0;
            for (int i = 0; i < 24 && !IsBadReadPtr(sp + i, 4); i++) o += snprintf(st + o, sizeof st - o, "%08x ", sp[i]);
            xlog("  stack: %s", st);
            uint32_t *rec; __asm__("movl %%fs:0, %0" : "=r"(rec)); o = 0;
            for (int i = 0; i < 12 && rec != (uint32_t *)-1 && !IsBadReadPtr(rec, 8); i++, rec = (uint32_t *)rec[0])
                o += snprintf(st + o, sizeof st - o, "[%p h=%08x] ", rec, rec[1]);
            xlog("  seh: %s", st);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    uint32_t *fp = (uint32_t *)c->Ebp; char buf[512]; int o = 0;
    for (int i = 0; i < 16 && fp && !IsBadReadPtr(fp, 8); i++) { o += snprintf(buf + o, sizeof buf - o, "%08x ", fp[1]); if ((uint32_t *)fp[0] <= fp) break; fp = (uint32_t *)fp[0]; }
    xlog("crash backtrace (ebp chain): %08lx %s", c->Eip, buf);
    xfatal("Unhandled exception %08lx at %08lx — see jsrf.log",
           ep->ExceptionRecord->ExceptionCode, ep->ContextRecord->Eip);
    return EXCEPTION_EXECUTE_HANDLER;
}

extern XThread *g_threads[64]; extern volatile LONG g_nthreads;
/* Data watchpoint (--watch=ADDR): DR0 write breakpoint on every game thread. */
static uint32_t g_watch_addr;
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    static int hits;
    if (hits++ < 40) {
        uint32_t *fp = (uint32_t *)c->Ebp; char bt[300]; int o = 0;
        for (int k = 0; k < 8 && fp && !IsBadReadPtr(fp, 8); k++) { o += snprintf(bt + o, sizeof bt - o, "%08x ", fp[1]); if ((uint32_t *)fp[0] <= fp) break; fp = (uint32_t *)fp[0]; }
        xlog("WATCH %08x = %08x  eip=%08lx eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx [esp]=%08x  bt: %s",
             g_watch_addr, *(uint32_t *)g_watch_addr, c->Eip, c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, *(uint32_t *)c->Esp, bt);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI watch_arm_thread(LPVOID p)
{
    static uint8_t armed[64];
    for (;;) {
        for (LONG i = 0; i < g_nthreads && i < 64; i++) {
            XThread *t = g_threads[i];
            if (armed[i] || !t || !t->host_handle || t->host_tid == GetCurrentThreadId()) continue;
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (SuspendThread(t->host_handle) == (DWORD)-1) continue;
            if (GetThreadContext(t->host_handle, &c)) {
                c.Dr0 = g_watch_addr; c.Dr7 = 1 | (1u << 16) | (3u << 18);  /* L0, write, 4 bytes */
                if (SetThreadContext(t->host_handle, &c)) armed[i] = 1;
            }
            ResumeThread(t->host_handle);
        }
        Sleep(50);
    }
    return 0;
}

/* Periodically log where every game thread is (hang diagnosis). */
extern XThread *g_threads[64]; extern volatile LONG g_nthreads;
static int g_watchdog_secs;
static DWORD WINAPI watchdog(LPVOID p)
{
    for (;;) {
        Sleep(g_watchdog_secs * 1000);
        xlog("---- watchdog ----");
        {   uint32_t dev = *(uint32_t *)0x19dce0;
            if (dev) xlog("d3d: dev=%08x time(+30)=%08x fenceptr(+34)=%08x -> %08x  put=%08x get=%08x",
                          dev, *(uint32_t *)(dev + 0x30), *(uint32_t *)(dev + 0x34),
                          *(uint32_t *)(dev + 0x34) ? **(uint32_t **)(dev + 0x34) : 0,
                          g_nv_regs[0x3240 / 4], g_nv_regs[0x3244 / 4]); }
        for (LONG i = 0; i < g_nthreads && i < 64; i++) {
            XThread *t = g_threads[i];
            if (!t || !t->host_handle) continue;
            CONTEXT c; c.ContextFlags = CONTEXT_FULL;
            if (SuspendThread(t->host_handle) == (DWORD)-1) { xlog("thread %lu: suspend failed (%lu)", t->host_tid, GetLastError()); continue; }
            if (!GetThreadContext(t->host_handle, &c)) xlog("thread %lu: no context (%lu)", t->host_tid, GetLastError());
            else {
                char buf[400]; int o = 0;
                uint32_t *fp = (uint32_t *)c.Ebp;
                for (int k = 0; k < 10 && fp && !IsBadReadPtr(fp, 8); k++) {
                    o += snprintf(buf + o, sizeof buf - o, "%08x ", fp[1]);
                    if ((uint32_t *)fp[0] <= fp) break;
                    fp = (uint32_t *)fp[0];
                }
                xlog("thread %lu start=%p eip=%08lx esp=%08lx irql=%u  bt: %s", t->host_tid, t->start, c.Eip, c.Esp, t->pcr.Irql, buf);
            }
            ResumeThread(t->host_handle);
        }
    }
    return 0;
}

/* ---- log output ----
 * jsrf.exe is a GUI program, so double-clicking it opens no console. Started
 * from a command prompt (or jsrf_profile.bat) it logs to that console too;
 * the Log window setting shows the live log in a window of its own
 * (logwin.c). jsrf.log is written regardless. */
static void console_init(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (freopen("CONOUT$", "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "\n");
    }
    if (g_set.log_window) logwin_show(1);
}

int main(int argc, char **argv)
{
    InitializeCriticalSection(&g_log_lock);
    InitializeCriticalSection(&g_hdr_lock);
    SetProcessDPIAware();      /* render at real pixels instead of being bitmap-scaled */
    char exe[MAX_PATH], xbe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    *strrchr(exe, '\\') = 0;

    const char *xbe_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) g_trace = 1;
        else if (!strcmp(argv[i], "--nv-trace")) g_nv_trace = 1;
        else if (!strcmp(argv[i], "--nv-trace-all")) g_nv_trace = 2;
        else if (!strcmp(argv[i], "--mmio-trace")) g_mmio_trace = 1;
        else if (!strcmp(argv[i], "--apu-trace")) g_apu_trace = 1;
        else if (!strcmp(argv[i], "--pgraph-log")) g_pgraph_log = 1;
        else if (!strcmp(argv[i], "--swm-trace")) { extern int g_swm_trace; g_swm_trace = 1; }
        else if (!strcmp(argv[i], "--ds-trace")) { extern int g_ds_trace; g_ds_trace = 1; }
        else if (!strcmp(argv[i], "--prio-trace")) { extern int g_prio_trace; g_prio_trace = 1; }
        else if (!strcmp(argv[i], "--gl-debug")) { extern int g_gl_debug; g_gl_debug = 1; }
        else if (!strcmp(argv[i], "--perf")) { extern int g_perf_log; g_perf_log = 1; }
        else if (!strncmp(argv[i], "--core=", 7)) g_game_core = atoi(argv[i] + 7);
        else if (!strcmp(argv[i], "--profile")) g_profile = 1;
        else if (!strncmp(argv[i], "--watchdog=", 11)) g_watchdog_secs = atoi(argv[i] + 11);
        else if (!strncmp(argv[i], "--bp=", 5)) bp_add(strtoul(argv[i] + 5, NULL, 16));
        else if (!strncmp(argv[i], "--press=", 8)) { void input_add_press(const char *); input_add_press(argv[i] + 8); }
        else if (!strncmp(argv[i], "--watch=", 8)) g_watch_addr = strtoul(argv[i] + 8, NULL, 16);
        else if (!strncmp(argv[i], "--trace-call=", 13)) { if (g_ntc < 16) g_tc_addr[g_ntc++] = strtoul(argv[i] + 13, NULL, 16); }
        else if (!strncmp(argv[i], "--scale=", 8)) { g_scale = atoi(argv[i] + 8); g_scale_user = 1; }
        else if (!strncmp(argv[i], "--window=", 9)) { sscanf(argv[i] + 9, "%dx%d", &g_window_w, &g_window_h); g_window_user = 1; }
        else if (!strncmp(argv[i], "--screenshots=", 14)) g_screenshot_every = atoi(argv[i] + 14);
        else if (!strcmp(argv[i], "--any-xbe")) g_any_xbe = 1;
        else if (!strncmp(argv[i], "--dump-frame=", 13)) g_dump_frame = atoi(argv[i] + 13);
        else xbe_arg = argv[i];
    }
    if (xbe_arg) GetFullPathNameA(xbe_arg, MAX_PATH, xbe, NULL);
    else snprintf(xbe, sizeof xbe, "%s\\default.xbe", exe);
    snprintf(g_game_dir, sizeof g_game_dir, "%s", xbe);
    *strrchr(g_game_dir, '\\') = 0;

    char logp[MAX_PATH]; snprintf(logp, sizeof logp, "%s\\jsrf.log", exe);
    snprintf(g_screenshot_dir, MAX_PATH, "%s", exe);
    if (g_scale < 1) g_scale = 1; if (g_scale > 8) g_scale = 8;
    g_log = fopen(logp, "w");
    xlog("%s %s", JSRF_PRODUCT, JSRF_VERSION_STR);
    /* Claim the Xbox's fixed address ranges (RAM at 0x80000000/0xF0000000,
     * devices at 0xFD000000) before any DLL or driver can allocate there. */
    xmem_init();
    mmio_reserve();
    {
        char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%s\\jsrf.ini", exe);
        settings_load(ini);
        console_init();
        extern float g_master_volume;
        g_master_volume = g_set.volume / 100.0f;
        xlog("settings: %s (scale %d, %s, aspect %s, vsync %d, brightness %d%%, volume %d%%)", ini, g_set.scale,
             g_set.fullscreen ? "fullscreen" : "window", g_set.aspect ? "stretch" : "4:3", g_set.vsync, g_set.brightness, g_set.volume);
    }
    xlog("game dir: %s", g_game_dir);

    /* Title threads share one logical CPU, as on the Xbox. Avoid CPU 0 (it
     * takes most device interrupts) but stay low-numbered: hybrid Intel CPUs
     * number their performance cores first. */
    {
        DWORD_PTR proc_mask = 0, sys_mask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask);
        int want = g_game_core >= 0 ? g_game_core : (__builtin_popcount(proc_mask) >= 4 ? 2 : 0);
        if (want < 0 || want > 31) want = 0;
        DWORD_PTR m = (DWORD_PTR)1 << want;
        if (!(proc_mask & m)) for (int i = 0; i < 32; i++) if (proc_mask & ((DWORD_PTR)1 << i)) { m = (DWORD_PTR)1 << i; break; }
        g_game_affinity = m;
        if (getenv("JSRF_MULTICORE")) g_game_affinity = 0;
        xlog("title threads on CPU mask %08lx (of %08lx)", (unsigned long)g_game_affinity, (unsigned long)proc_mask);
    }
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(unhandled);

    load_xbe(xbe);
    patch_fs();
    apply_patches();
    hle_install();
    bp_arm();
    tc_arm();
    AddVectoredExceptionHandler(1, bp_veh);
    kernel_init();
    mmio_init();
    if (g_profile) prof_start();

    char title[41] = {0};
    uint16_t *wt = (uint16_t *)(g_xbe->CertificateAddress + 0xC);
    for (int i = 0; i < 40 && wt[i]; i++) title[i] = (char)wt[i];
    xlog("starting '%s' (title id %08x) at entry %08x", title, *(uint32_t *)(g_xbe->CertificateAddress + 8), g_xbe->EntryPoint);

    if (g_watchdog_secs) host_create_thread(0, watchdog, NULL, 0, NULL);
    if (g_watch_addr) { AddVectoredExceptionHandler(1, watch_veh); host_create_thread(0, watch_arm_thread, NULL, 0, NULL); }
    XThread *main_t = xthread_create((void *)g_xbe->EntryPoint, NULL, NULL, g_xbe->StackCommit, 0, TRUE);
    xthread_pin_game(main_t);
    ResumeThread(main_t->host_handle);
    WaitForSingleObject(main_t->host_handle, INFINITE);
    xlog("entry thread returned; waiting on title threads");
    Sleep(INFINITE);
    return 0;
}
