/* SPDX-License-Identifier: GPL-2.0-or-later */
/* More xboxkrnl exports (included by kernel.c): the ones later XDKs import
 * (XAPI's mutexes and semaphores, the Xbox Live / networking libraries, save
 * signatures), first needed by Midnight Club 3 (XDK 5849). */

/* ------------------------------------------------------------------ */
/* Data exports                                                        */
/* ------------------------------------------------------------------ */
static char s_disk_model[] = "WDC WD80EB-28CGH1", s_disk_serial[] = "WD-WMAJ00000000";
X_STRING x_HalDiskModelNumber = { sizeof s_disk_model - 1, sizeof s_disk_model, s_disk_model };
X_STRING x_HalDiskSerialNumber = { sizeof s_disk_serial - 1, sizeof s_disk_serial, s_disk_serial };
static uint8_t x_IoFileObjectType[0x40];
struct { USHORT Major, Minor, Build, Qfe; } x_XboxKrnlVersion = { 1, 0, 5838, 1 };
static char s_image_name[] = "\\Device\\CdRom0\\default.xbe";
X_STRING x_XeImageFileName = { sizeof s_image_name - 1, sizeof s_image_name, s_image_name };
UCHAR x_XboxLANKey[16];
UCHAR x_XePublicKeyData[284];
/* The IDE channel object: XAPI's auto power-down code stores callbacks at
 * +0x10/+0x14, reads a flag at +0x20 and walks a request list at +0x28. */
static uint8_t x_IdexChannelObject[0x100] __attribute__((aligned(16)));

static void kextra_init(void)
{
    LIST_ENTRY *q = (LIST_ENTRY *)(x_IdexChannelObject + 0x28);
    q->Flink = q->Blink = q;
}

/* ------------------------------------------------------------------ */
/* Mutants, semaphores, handles                                        */
/* ------------------------------------------------------------------ */
static void obj_name(X_OBJECT_ATTRIBUTES *oa, char *name, size_t cap)
{
    name[0] = 0;
    if (oa && oa->ObjectName && oa->ObjectName->Length) {
        snprintf(name, cap, "%s_%.*s", g_title.stem, oa->ObjectName->Length, oa->ObjectName->Buffer);
        for (char *p = name; *p; p++) if (*p == '\\') *p = '_';
    }
}
NTSTATUS_X KAPI x_NtCreateMutant(PHANDLE out, X_OBJECT_ATTRIBUTES *oa, BOOLEAN owner)
{
    char name[260]; obj_name(oa, name, sizeof name);
    HANDLE h = CreateMutexA(NULL, owner, name[0] ? name : NULL);
    if (!h) return X_STATUS_UNSUCCESSFUL;
    *out = h;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0x40000000 : 0;
}
NTSTATUS_X KAPI x_NtReleaseMutant(HANDLE h, PLONG prev)
{
    if (prev) *prev = 0;
    return ReleaseMutex(h) ? 0 : 0xC0000046 /* STATUS_MUTANT_NOT_OWNED */;
}
NTSTATUS_X KAPI x_NtCreateSemaphore(PHANDLE out, X_OBJECT_ATTRIBUTES *oa, LONG initial, LONG maximum)
{
    char name[260]; obj_name(oa, name, sizeof name);
    HANDLE h = CreateSemaphoreA(NULL, initial, maximum, name[0] ? name : NULL);
    if (!h) return X_STATUS_UNSUCCESSFUL;
    *out = h;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0x40000000 : 0;
}
NTSTATUS_X KAPI x_NtReleaseSemaphore(HANDLE h, LONG n, PLONG prev)
{
    LONG p = 0;
    if (!ReleaseSemaphore(h, n, &p)) return 0xC0000047 /* STATUS_SEMAPHORE_LIMIT_EXCEEDED */;
    if (prev) *prev = p;
    return 0;
}
NTSTATUS_X KAPI x_NtDuplicateObject(HANDLE src, PHANDLE out, ULONG opts)
{
    if (src == (HANDLE)-2) src = GetCurrentThread();
    HANDLE h;
    if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &h, 0, FALSE,
                         DUPLICATE_SAME_ACCESS | ((opts & 1) ? DUPLICATE_CLOSE_SOURCE : 0)))
        return X_STATUS_INVALID_HANDLE;
    EnterCriticalSection(&g_file_lock);
    FileEnt *f = fent(src);
    if (f) {   /* keep file bookkeeping (paths, raw volumes) for the copy */
        FileEnt copy = *f;
        LeaveCriticalSection(&g_file_lock);
        FileEnt *n = fent_new(h, copy.kind);
        snprintf(n->xpath, MAX_PATH, "%s", copy.xpath); snprintf(n->hpath, MAX_PATH, "%s", copy.hpath);
        n->part = copy.part; n->link = copy.link;
    } else LeaveCriticalSection(&g_file_lock);
    *out = h;
    return 0;
}
NTSTATUS_X KAPI x_NtYieldExecution(void) { return SwitchToThread() ? 0 : 0x40000024 /* STATUS_NO_YIELD_PERFORMED */; }

BOOLEAN KAPI x_RtlTryEnterCriticalSection(X_CRITICAL_SECTION *cs)
{
    HANDLE me = (HANDLE)xthread_current()->pcr.CurrentThread;
    if (InterlockedCompareExchange(&cs->LockCount, 0, -1) == -1) {
        cs->OwningThread = me; cs->RecursionCount = 1; return TRUE;
    }
    if (cs->OwningThread == me) { InterlockedIncrement(&cs->LockCount); cs->RecursionCount++; return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Waits on several kernel objects                                     */
/* ------------------------------------------------------------------ */
NTSTATUS_X KAPI x_KeWaitForMultipleObjects(ULONG n, PVOID *objs, ULONG type, ULONG reason, CHAR mode,
                                           BOOLEAN alertable, PLARGE_INTEGER to, PVOID blocks)
{
    HANDLE hs[64];
    if (n > 64) n = 64;
    for (ULONG i = 0; i < n; i++) {
        hs[i] = obj_waitable(objs[i]);
        if (((X_DISPATCHER_HEADER *)objs[i])->Type != HOSTOBJ_MAGIC) sync_notification_event(objs[i], hs[i]);
    }
    DWORD r = WaitForMultipleObjectsEx(n, hs, type == 0 /* WaitAll */, timeout_ms(to), alertable);
    if (r < WAIT_OBJECT_0 + n) {
        for (ULONG i = 0; i < n; i++) {
            if (type != 0 && i != r - WAIT_OBJECT_0) continue;
            X_DISPATCHER_HEADER *h = objs[i];
            if (h->Type == 1 || h->Type == 9) h->SignalState = 0;
        }
    }
    return wait_result(r);
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
#define XBOX_PERF_HZ 3375000ull       /* KeQueryPerformanceFrequency on the Xbox */
ULONGLONG KAPI x_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return qpc_scale((uint64_t)(c.QuadPart - g_qpc0.QuadPart), (uint64_t)g_qpf.QuadPart, XBOX_PERF_HZ);
}
ULONGLONG KAPI x_KeQueryPerformanceFrequency(void) { return XBOX_PERF_HZ; }
NTSTATUS_X KAPI x_NtSetSystemTime(PLARGE_INTEGER t, PLARGE_INTEGER old)
{
    if (old) x_KeQuerySystemTime(old);    /* the host clock stays as it is */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
/* MmCreateKernelStack returns the top of a new stack; MmDeleteKernelStack
 * gets (top, bottom). */
PVOID KAPI x_MmCreateKernelStack(ULONG size, BOOLEAN debugger)
{
    size = (size + 0xFFF) & ~0xFFFu;
    uint8_t *p = VirtualAlloc(NULL, size + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return NULL;
    DWORD o; VirtualProtect(p, 0x1000, PAGE_NOACCESS, &o);   /* guard page below */
    return p + 0x1000 + size;
}
void KAPI x_MmDeleteKernelStack(PVOID top, PVOID bottom)
{
    VirtualFree((uint8_t *)bottom - 0x1000, 0, MEM_RELEASE);
}
typedef struct {
    ULONG Length, TotalPhysicalPages, AvailablePages, VirtualMemoryBytesCommitted, VirtualMemoryBytesReserved;
    ULONG CachePagesCommitted, PoolPagesCommitted, StackPagesCommitted, ImagePagesCommitted;
} X_MM_STATISTICS;
NTSTATUS_X KAPI x_MmQueryStatistics(X_MM_STATISTICS *s)
{
    if (!s || s->Length != sizeof *s) return X_STATUS_INVALID_PARAMETER;
    memset((uint8_t *)s + 4, 0, sizeof *s - 4);
    s->TotalPhysicalPages = XBOX_RAM_PAGES;
    s->AvailablePages = xmem_free_pages();
    s->ImagePagesCommitted = (g_xbe->SizeOfImage + 0xFFF) >> 12;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } X_UNICODE_STRING;
NTSTATUS_X KAPI x_RtlAnsiStringToUnicodeString(X_UNICODE_STRING *d, X_STRING *s, BOOLEAN alloc)
{
    ULONG need = (ULONG)s->Length * 2 + 2;
    if (need > 0xFFFE) return X_STATUS_INVALID_PARAMETER;
    if (alloc) {
        d->Buffer = x_ExAllocatePoolWithTag(need, 0);
        if (!d->Buffer) return X_STATUS_NO_MEMORY;
        d->MaximumLength = (USHORT)need;
    } else if (d->MaximumLength < need - 2) return 0x80000005 /* STATUS_BUFFER_OVERFLOW */;
    for (USHORT i = 0; i < s->Length; i++) d->Buffer[i] = (WCHAR)(uint8_t)s->Buffer[i];
    d->Length = (USHORT)(s->Length * 2);
    if (d->MaximumLength > d->Length) d->Buffer[s->Length] = 0;
    return 0;
}
NTSTATUS_X KAPI x_RtlUnicodeStringToAnsiString(X_STRING *d, X_UNICODE_STRING *s, BOOLEAN alloc)
{
    ULONG n = s->Length / 2;
    if (alloc) {
        d->Buffer = x_ExAllocatePoolWithTag(n + 1, 0);
        if (!d->Buffer) return X_STATUS_NO_MEMORY;
        d->MaximumLength = (USHORT)(n + 1);
    } else if (d->MaximumLength < n) return 0x80000005;
    for (ULONG i = 0; i < n; i++) d->Buffer[i] = s->Buffer[i] < 0x100 ? (char)s->Buffer[i] : '?';
    d->Length = (USHORT)n;
    if (d->MaximumLength > n) d->Buffer[n] = 0;
    return 0;
}
LONG KAPI x_RtlCompareString(X_STRING *a, X_STRING *b, BOOLEAN ci)
{
    USHORT n = a->Length < b->Length ? a->Length : b->Length;
    for (USHORT i = 0; i < n; i++) {
        int x = (uint8_t)a->Buffer[i], y = (uint8_t)b->Buffer[i];
        if (ci) { x = toupper(x); y = toupper(y); }
        if (x != y) return x - y;
    }
    return (LONG)a->Length - (LONG)b->Length;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */
NTSTATUS_X KAPI x_NtDeleteFile(X_OBJECT_ATTRIBUTES *oa)
{
    char xp[MAX_PATH], hp[MAX_PATH];
    NTSTATUS_X st = oa_resolve(oa, xp, hp);
    if (st) return st;
    DWORD a = GetFileAttributesA(hp);
    if (a == INVALID_FILE_ATTRIBUTES) return err_to_status(GetLastError());
    BOOL ok = (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryA(hp) : DeleteFileA(hp);
    TRACE("NtDeleteFile(%s) -> %d", xp, ok);
    return ok ? 0 : err_to_status(GetLastError());
}
NTSTATUS_X KAPI x_NtQueryFullAttributesFile(X_OBJECT_ATTRIBUTES *oa, PVOID info)
{
    char xp[MAX_PATH], hp[MAX_PATH];
    NTSTATUS_X st = oa_resolve(oa, xp, hp);
    if (st) return st;
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExA(hp, GetFileExInfoStandard, &fd)) return err_to_status(GetLastError());
    /* FILE_NETWORK_OPEN_INFORMATION */
    LONGLONG *q = info; ULONG *u = info;
    LONGLONG size = ((LONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    memset(info, 0, 56);
    q[0] = ft64(fd.ftCreationTime); q[1] = ft64(fd.ftLastAccessTime); q[2] = q[3] = ft64(fd.ftLastWriteTime);
    q[4] = (size + 0x3FFF) & ~0x3FFFLL; q[5] = size; u[12] = fd.dwFileAttributes & 0x37;
    return 0;
}
/* XAPI references the file object behind a handle (ObReferenceObjectByHandle
 * with IoFileObjectType), then asks for volume information about it. */
NTSTATUS_X KAPI x_IoQueryVolumeInformation(PVOID fileobj, ULONG cls, ULONG len, PVOID buf, PULONG retlen)
{
    HostObj *o = fileobj;
    if (!o || o->hdr.Type != HOSTOBJ_MAGIC) return X_STATUS_INVALID_HANDLE;
    X_IO_STATUS_BLOCK iosb = { 0 };
    NTSTATUS_X st = x_NtQueryVolumeInformationFile(o->h, &iosb, buf, len, cls);
    if (retlen) *retlen = (ULONG)iosb.Information;
    return st;
}

/* ------------------------------------------------------------------ */
/* Raw device I/O: XNET reads the network configuration sector of the  */
/* hard disk, XAPI talks to memory units. Neither device exists here.  */
/* ------------------------------------------------------------------ */
NTSTATUS_X KAPI x_ObReferenceObjectByName(X_STRING *name, ULONG attrs, PVOID type, PVOID ctx, PVOID *out)
{
    char n[MAX_PATH]; str_from_x(name, n, sizeof n);
    xlog("ObReferenceObjectByName(%s): no such device object", n);
    if (out) *out = NULL;
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
}
PVOID KAPI x_IoBuildSynchronousFsdRequest(ULONG major, PVOID dev, PVOID buf, ULONG len, PLARGE_INTEGER off,
                                          PVOID ev, X_IO_STATUS_BLOCK *iosb)
{ return NULL; }
NTSTATUS_X FASTCALL x_IofCallDriver(PVOID dev, PVOID irp) { return X_STATUS_INVALID_DEVICE_REQUEST; }
NTSTATUS_X KAPI x_IoSynchronousFsdRequest(ULONG major, PVOID dev, PVOID buf, ULONG len, PLARGE_INTEGER off)
{ return X_STATUS_INVALID_DEVICE_REQUEST; }
NTSTATUS_X KAPI x_IoSynchronousDeviceIoControlRequest(ULONG code, PVOID dev, PVOID in, ULONG inlen, PVOID out,
                                                      ULONG outlen, PULONG ret, BOOLEAN internal)
{ if (ret) *ret = 0; return X_STATUS_INVALID_DEVICE_REQUEST; }

/* Ethernet PHY: no cable plugged in, so the network stack stays offline. */
NTSTATUS_X KAPI x_PhyInitialize(BOOLEAN force, PVOID param) { return 0; }
ULONG KAPI x_PhyGetLinkState(BOOLEAN update) { return 0; }

/* ------------------------------------------------------------------ */
/* Crypto (Xc*). HMAC-SHA1 and RC4 are real (save signatures use HMAC);   */
/* the public-key and DES parts only serve Xbox Live and fail politely.  */
/* ------------------------------------------------------------------ */
void KAPI x_XcHMAC(const uint8_t *key, ULONG keylen, const uint8_t *d1, ULONG n1, const uint8_t *d2, ULONG n2, uint8_t *out)
{
    uint8_t k[64] = { 0 }, pad[64], inner[20];
    SHA1C c;
    if (keylen > 64) { x_XcSHAInit(&c); x_XcSHAUpdate(&c, key, keylen); x_XcSHAFinal(&c, k); }
    else memcpy(k, key, keylen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    x_XcSHAInit(&c); x_XcSHAUpdate(&c, pad, 64);
    if (d1 && n1) x_XcSHAUpdate(&c, d1, n1);
    if (d2 && n2) x_XcSHAUpdate(&c, d2, n2);
    x_XcSHAFinal(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    x_XcSHAInit(&c); x_XcSHAUpdate(&c, pad, 64); x_XcSHAUpdate(&c, inner, 20); x_XcSHAFinal(&c, out);
}
/* RC4 state: S[256], i, j */
void KAPI x_XcRC4Key(uint8_t *st, ULONG keylen, const uint8_t *key)
{
    for (int i = 0; i < 256; i++) st[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + st[i] + key[i % keylen]) & 255;
        uint8_t t = st[i]; st[i] = st[j]; st[j] = t;
    }
    st[256] = st[257] = 0;
}
void KAPI x_XcRC4Crypt(uint8_t *st, ULONG len, uint8_t *data)
{
    uint8_t i = st[256], j = st[257];
    for (ULONG k = 0; k < len; k++) {
        i++; j += st[i];
        uint8_t t = st[i]; st[i] = st[j]; st[j] = t;
        data[k] ^= st[(uint8_t)(st[i] + st[j])];
    }
    st[256] = i; st[257] = j;
}
void KAPI x_XcDESKeyParity(uint8_t *key, ULONG len)
{
    for (ULONG i = 0; i < len; i++) {
        uint8_t b = key[i] & 0xFE; int ones = __builtin_popcount(b);
        key[i] = b | !(ones & 1);
    }
}
BOOLEAN KAPI x_XcVerifyPKCS1Signature(const uint8_t *sig, const uint8_t *pub, const uint8_t *digest) { return FALSE; }
ULONG KAPI x_XcModExp(ULONG *a, const ULONG *b, const ULONG *c, const ULONG *d, ULONG n) { if (a) memset(a, 0, n * 4); return 0; }
void KAPI x_XcKeyTable(ULONG cipher, uint8_t *table, const uint8_t *key) { }
void KAPI x_XcBlockCryptCBC(ULONG cipher, ULONG len, uint8_t *out, const uint8_t *in, const uint8_t *table, ULONG op, uint8_t *fb)
{ if (out != in) memmove(out, in, len); }
