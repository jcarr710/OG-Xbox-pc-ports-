/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Xbox physical memory model.
 *
 * The XDK D3D library assumes GPU-visible ("contiguous") memory lives at
 *   VA = 0x80000000 + PA   (cached)   and   VA = 0xF0000000 + PA (write-combined)
 * and converts with masks such as (VA & 0x03FFFFFF).  We back 64 MB of
 * "physical RAM" with one page-file section mapped at both addresses, and
 * allocate MmAllocateContiguousMemory* from it page by page. */
#include "xbox.h"
#include "xmem.h"
#include <string.h>
#include <stdio.h>

static uint8_t g_page_used[XBOX_RAM_PAGES];   /* 1 = allocated */
static uint32_t g_alloc_pages[XBOX_RAM_PAGES]; /* at first page of a block: its page count */
static CRITICAL_SECTION g_mem_lock;
uint8_t *g_ram = (uint8_t *)XBOX_RAM_VA;

/* List the allocations inside [lo, hi), for error messages. */
void xmem_describe_range(uint32_t lo, uint32_t hi, char *out, unsigned n)
{
    typedef DWORD (WINAPI *PFN_GMFN)(HANDLE, LPVOID, LPSTR, DWORD);
    static PFN_GMFN gmfn;
    if (!gmfn) gmfn = (PFN_GMFN)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetMappedFileNameA");
    unsigned o = 0; int found = 0;
    out[0] = 0;
    for (uint64_t a = lo; a < hi && found < 6 && o + 8 < n;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) {
            o += snprintf(out + o, n - o, "\n  %08x: not addressable in this process", (uint32_t)a);
            found++;
            break;
        }
        uint64_t end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State != MEM_FREE) {
            char name[MAX_PATH] = "";
            if (mbi.Type == MEM_IMAGE) GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof name);
            if (!name[0] && mbi.Type != MEM_PRIVATE && gmfn) gmfn(GetCurrentProcess(), mbi.BaseAddress, name, sizeof name);
            int k = snprintf(out + o, n - o, "\n  %08x-%08x %s %s %s", (uint32_t)(uintptr_t)mbi.BaseAddress, (uint32_t)(end - 1),
                             mbi.State == MEM_RESERVE ? "reserved" : "committed",
                             mbi.Type == MEM_IMAGE ? "module" : mbi.Type == MEM_MAPPED ? "mapping" : "allocation", name);
            if (k > 0) o += (unsigned)k < n - o ? (unsigned)k : n - o - 1;
            found++;
        }
        if (end <= a) break;
        a = end;
    }
    if (!found) snprintf(out, n, " (nothing; the range looks free, so the error code above is the cause)");
}

const char *xmem_address_space_hint(void)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    if ((uintptr_t)si.lpMaximumApplicationAddress < 0xFF000000u)
        return "jsrf.exe needs 64-bit Windows: it runs as a 32-bit program that uses the full 4 GB address space.";
    return "Programs that inject code into other processes (overlays, FPS counters, screen recorders, "
           "some antivirus tools) can take these addresses. If one is named above, disable it for jsrf.exe and try again.";
}

void xmem_init(void)
{
    InitializeCriticalSection(&g_mem_lock);
    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, XBOX_RAM_SIZE, NULL);
    if (!sec) xfatal("Could not allocate 64 MB for the Xbox's RAM (CreateFileMapping error %lu).", GetLastError());
    static const uint32_t at[2] = { XBOX_RAM_VA, XBOX_RAM_WC_VA };
    for (int i = 0; i < 2; i++) {
        void *v = MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, XBOX_RAM_SIZE, (void *)(uintptr_t)at[i]);
        if (v == (void *)(uintptr_t)at[i]) continue;
        DWORD err = GetLastError();
        char occ[1024]; xmem_describe_range(at[i], at[i] + XBOX_RAM_SIZE, occ, sizeof occ);
        xfatal("Could not map the Xbox's RAM at %08x-%08x (error %lu).\n"
               "Already in that range:%s\n\n%s", at[i], at[i] + XBOX_RAM_SIZE, err, occ, xmem_address_space_hint());
    }
    /* Reserve the first 64 KB (null-ish physical addresses) and the top 64 KB (GPU instance memory). */
    for (uint32_t i = 0; i < 16; i++) g_page_used[i] = 1;
    for (uint32_t i = XBOX_RAM_PAGES - 16; i < XBOX_RAM_PAGES; i++) g_page_used[i] = 1;
    xlog("xmem: 64 MB physical RAM at %08x (cached) / %08x (write-combined)", XBOX_RAM_VA, XBOX_RAM_WC_VA);
}

void *xmem_alloc_contiguous(uint32_t size, uint32_t lo, uint32_t hi, uint32_t align)
{
    uint32_t pages = (size + 0xFFF) >> 12;
    if (!pages) pages = 1;
    uint32_t align_pages = align > 0x1000 ? (align >> 12) : 1;
    uint32_t lo_pfn = lo >> 12, hi_pfn = (hi >= XBOX_RAM_SIZE ? XBOX_RAM_SIZE - 1 : hi) >> 12;
    void *result = NULL;
    EnterCriticalSection(&g_mem_lock);
    /* top-down first fit, like the Xbox kernel */
    int64_t start = (int64_t)hi_pfn + 1 - pages;
    if (start >= 0) start -= start % align_pages;
    while (start >= (int64_t)lo_pfn) {
        uint32_t i;
        for (i = pages; i > 0; i--) if (g_page_used[start + i - 1]) break;
        if (i == 0) {
            memset(&g_page_used[start], 1, pages);
            g_alloc_pages[start] = pages;
            result = (void *)(uintptr_t)(XBOX_RAM_VA + ((uint32_t)start << 12));
            break;
        }
        start = start + i - 1 - pages;  /* block must end below the used page */
        if (start < 0) break;
        start -= start % align_pages;
    }
    LeaveCriticalSection(&g_mem_lock);
    if (result) memset(result, 0, pages << 12);
    else xlog("xmem: contiguous alloc of %u bytes FAILED (lo %x hi %x align %x)", size, lo, hi, align);
    return result;
}

void xmem_free(void *p)
{
    uint32_t pa = xmem_pa(p);
    if (pa == XMEM_BAD_PA) return;
    uint32_t pfn = pa >> 12;
    EnterCriticalSection(&g_mem_lock);
    uint32_t n = g_alloc_pages[pfn];
    if (n) { memset(&g_page_used[pfn], 0, n); g_alloc_pages[pfn] = 0; }
    LeaveCriticalSection(&g_mem_lock);
}

uint32_t xmem_size(void *p)
{
    uint32_t pa = xmem_pa(p);
    if (pa == XMEM_BAD_PA) return 0;
    return g_alloc_pages[pa >> 12] << 12;
}

int xmem_is_ram(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (v >= XBOX_RAM_VA && v < XBOX_RAM_VA + XBOX_RAM_SIZE) ||
           (v >= XBOX_RAM_WC_VA && v < XBOX_RAM_WC_VA + XBOX_RAM_SIZE);
}

uint32_t xmem_pa(const void *p)
{
    if (!xmem_is_ram(p)) return XMEM_BAD_PA;
    return (uint32_t)(uintptr_t)p & (XBOX_RAM_SIZE - 1);
}
