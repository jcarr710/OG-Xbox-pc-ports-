/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Xbox file system HLE (included by kernel.c).
 *   \Device\CdRom0                 -> folder containing default.xbe
 *   \Device\Harddisk0\PartitionN   -> <game dir>\_hdd\PartitionN
 *   \??\X:  drive letters          -> symbolic link table (IoCreateSymbolicLink) */

typedef struct { char link[64]; char target[MAX_PATH]; } SymLink;
static SymLink g_links[64]; static int g_nlinks;

enum { H_FILE = 1, H_DIR, H_LINK, H_RAW };
typedef struct { HANDLE h; int kind; char xpath[MAX_PATH]; char hpath[MAX_PATH]; HANDLE find; int link; int part; } FileEnt;
static FileEnt g_files[1024];
static char g_hdd_dir[MAX_PATH];

static FileEnt *fent(HANDLE h)
{
    for (int i = 0; i < 1024; i++) if (g_files[i].h == h && g_files[i].kind) return &g_files[i];
    return NULL;
}
static FileEnt *fent_new(HANDLE h, int kind)
{
    EnterCriticalSection(&g_file_lock);
    for (int i = 0; i < 1024; i++) if (!g_files[i].kind) {
        memset(&g_files[i], 0, sizeof g_files[i]);
        g_files[i].h = h; g_files[i].kind = kind;
        LeaveCriticalSection(&g_file_lock);
        return &g_files[i];
    }
    LeaveCriticalSection(&g_file_lock);
    xfatal("file handle table full");
    return NULL;
}

static void str_from_x(const X_STRING *s, char *out, size_t cap)
{
    size_t n = s ? s->Length : 0; if (n >= cap) n = cap - 1;
    if (n) memcpy(out, s->Buffer, n);
    out[n] = 0;
}

static SymLink *link_find(const char *name)
{
    for (int i = 0; i < g_nlinks; i++) if (!_stricmp(g_links[i].link, name)) return &g_links[i];
    return NULL;
}
static void link_set(const char *name, const char *target)
{
    SymLink *l = link_find(name);
    if (!l) { if (g_nlinks == 64) return; l = &g_links[g_nlinks++]; }
    snprintf(l->link, sizeof l->link, "%s", name);
    snprintf(l->target, sizeof l->target, "%s", target);
    xlog("symlink %s -> %s", name, target);
}

static int prefix_i(const char *s, const char *p) { return _strnicmp(s, p, strlen(p)) == 0; }

/* Full xbox path -> host path. Returns 0 or an NTSTATUS. */
static int g_resolve_raw; /* set by xpath_resolve: partition number+1 when path names a raw volume */
static NTSTATUS_X xpath_resolve(const char *xp_in, char *host, size_t cap)
{
    g_resolve_raw = 0;
    char xp[MAX_PATH * 2];
    snprintf(xp, sizeof xp, "%s", xp_in);
    for (int depth = 0; depth < 8; depth++) {
        const char *p = xp;
        if (prefix_i(p, "\\??\\")) p += 4;
        if (isalpha((unsigned char)p[0]) && p[1] == ':') {
            char nm[8] = { '\\', '?', '?', '\\', (char)toupper(p[0]), ':', 0 };
            SymLink *l = link_find(nm);
            if (!l) return X_STATUS_OBJECT_PATH_NOT_FOUND;
            char tmp[MAX_PATH * 2]; snprintf(tmp, sizeof tmp, "%s%s", l->target, p + 2);
            snprintf(xp, sizeof xp, "%s", tmp);
            continue;
        }
        const char *rest = NULL; char base[MAX_PATH];
        if (prefix_i(p, "\\Device\\CdRom0")) { rest = p + 14; snprintf(base, sizeof base, "%s", g_game_dir); }
        else if (prefix_i(p, "\\Device\\Harddisk0\\Partition")) {
            int n = atoi(p + 27); rest = p + 28; while (isdigit((unsigned char)*rest)) rest++;
            snprintf(base, sizeof base, "%s\\Partition%d", g_hdd_dir, n);
            if (!*rest) { /* no trailing backslash: raw volume access */
                snprintf(host, cap, "%s\\Partition%d.raw", g_hdd_dir, n);
                g_resolve_raw = n + 1;
                return 0;
            }
            CreateDirectoryA(base, NULL);
        }
        else return X_STATUS_OBJECT_NAME_INVALID;
        snprintf(host, cap, "%s%s", base, rest);
        for (char *c = host; *c; c++) if (*c == '/') *c = '\\';
        size_t n = strlen(host); while (n > 3 && host[n - 1] == '\\') host[--n] = 0;
        return 0;
    }
    return X_STATUS_OBJECT_PATH_NOT_FOUND;
}

static NTSTATUS_X oa_resolve(X_OBJECT_ATTRIBUTES *oa, char *xpath, char *host)
{
    char name[MAX_PATH]; str_from_x(oa->ObjectName, name, sizeof name);
    if (oa->RootDirectory == (HANDLE)-3 || oa->RootDirectory == (HANDLE)-4) {   /* ObDosDevicesDirectory / Win32NamedObjects */
        snprintf(xpath, MAX_PATH, "\\??\\%s", name);
    } else if (oa->RootDirectory) {
        FileEnt *r = fent(oa->RootDirectory);
        if (!r) return X_STATUS_INVALID_HANDLE;
        snprintf(xpath, MAX_PATH, "%s%s%s", r->xpath, name[0] == '\\' ? "" : "\\", name);
    } else snprintf(xpath, MAX_PATH, "%s", name);
    return xpath_resolve(xpath, host, MAX_PATH);
}

static NTSTATUS_X err_to_status(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND: return X_STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND: return X_STATUS_OBJECT_PATH_NOT_FOUND;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return X_STATUS_OBJECT_NAME_COLLISION;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return X_STATUS_ACCESS_DENIED;
    case ERROR_HANDLE_EOF: return X_STATUS_END_OF_FILE;
    case ERROR_DIR_NOT_EMPTY: return X_STATUS_DIRECTORY_NOT_EMPTY;
    case ERROR_INVALID_HANDLE: return X_STATUS_INVALID_HANDLE;
    default: return X_STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS_X KAPI x_IoCreateSymbolicLink(X_STRING *link, X_STRING *target)
{
    char l[64], t[MAX_PATH]; str_from_x(link, l, sizeof l); str_from_x(target, t, sizeof t);
    link_set(l, t);
    return 0;
}
NTSTATUS_X KAPI x_IoDeleteSymbolicLink(X_STRING *link)
{
    char l[64]; str_from_x(link, l, sizeof l);
    SymLink *s = link_find(l);
    if (!s) return X_STATUS_OBJECT_NAME_NOT_FOUND;
    *s = g_links[--g_nlinks];
    return 0;
}
NTSTATUS_X KAPI x_NtOpenSymbolicLinkObject(PHANDLE out, X_OBJECT_ATTRIBUTES *oa)
{
    char name[64]; str_from_x(oa->ObjectName, name, sizeof name);
    SymLink *s = link_find(name);
    if (!s) { TRACE("NtOpenSymbolicLinkObject(%s): not found", name); return X_STATUS_OBJECT_NAME_NOT_FOUND; }
    HANDLE h = CreateEventA(NULL, TRUE, FALSE, NULL);
    FileEnt *f = fent_new(h, H_LINK); f->link = (int)(s - g_links);
    *out = h;
    return 0;
}
NTSTATUS_X KAPI x_NtQuerySymbolicLinkObject(HANDLE h, X_STRING *target, PULONG retlen)
{
    FileEnt *f = fent(h);
    if (!f || f->kind != H_LINK) return X_STATUS_INVALID_HANDLE;
    const char *t = g_links[f->link].target; USHORT n = (USHORT)strlen(t);
    if (retlen) *retlen = n;
    if (target->MaximumLength < n) return X_STATUS_BUFFER_TOO_SMALL;
    memcpy(target->Buffer, t, n); target->Length = n;
    return 0;
}

NTSTATUS_X KAPI x_NtCreateFile(PHANDLE out, ULONG access, X_OBJECT_ATTRIBUTES *oa, X_IO_STATUS_BLOCK *iosb,
    PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disp, ULONG opts)
{
    char xp[MAX_PATH], hp[MAX_PATH];
    NTSTATUS_X st = oa_resolve(oa, xp, hp);
    if (st) { char nm[MAX_PATH]; str_from_x(oa->ObjectName, nm, sizeof nm);
              xlog("NtCreateFile(root=%p name='%s' -> '%s'): cannot resolve (0x%08lx)", oa->RootDirectory, nm, xp, st); iosb->Status = st; return st; }

    int raw = g_resolve_raw; /* not thread safe: callers hold no lock, but resolution is immediate */
    if (raw) {
        HANDLE rh = CreateFileA(hp, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (rh == INVALID_HANDLE_VALUE) { st = err_to_status(GetLastError()); iosb->Status = st; return st; }
        FileEnt *f = fent_new(rh, H_RAW); f->part = raw - 1;
        snprintf(f->xpath, MAX_PATH, "%s", xp); snprintf(f->hpath, MAX_PATH, "%s", hp);
        TRACE("NtCreateFile(%s) -> raw volume %p", xp, rh);
        *out = rh; iosb->Status = 0; iosb->Information = 1;
        return 0;
    }
    DWORD fa = GetFileAttributesA(hp);
    BOOL exists = fa != INVALID_FILE_ATTRIBUTES, isdir = exists && (fa & FILE_ATTRIBUTE_DIRECTORY);
    ULONG info = 1; /* FILE_OPENED */

    if (opts & 0x1) { /* FILE_DIRECTORY_FILE */
        if (exists && !isdir) st = X_STATUS_NOT_A_DIRECTORY;
        else if (!exists) {
            if (disp == 2 || disp == 3) { if (!CreateDirectoryA(hp, NULL)) st = err_to_status(GetLastError()); else info = 2; }
            else st = X_STATUS_OBJECT_NAME_NOT_FOUND;
        } else if (disp == 2) st = X_STATUS_OBJECT_NAME_COLLISION;
        isdir = TRUE;
    } else if ((opts & 0x40) && isdir) st = X_STATUS_FILE_IS_A_DIRECTORY;
    if (st) { TRACE("NtCreateFile(%s) -> 0x%08lx", xp, st); iosb->Status = st; return st; }

    HANDLE h;
    if (isdir) {
        h = CreateFileA(hp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        static const DWORD dmap[] = { CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING, CREATE_ALWAYS };
        DWORD hd = disp < 6 ? dmap[disp] : OPEN_EXISTING;
        DWORD ha = GENERIC_READ;
        if (access & (0x40000000 | 0x10000000 | 0x2 | 0x4 | 0x100)) ha |= GENERIC_WRITE;
        if (access & 0x10000) ha |= DELETE;
        DWORD flags = (opts & 0x1000) ? FILE_FLAG_DELETE_ON_CLOSE : 0;
        h = CreateFileA(hp, ha, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, hd,
                        flags | (attrs ? (attrs & 0x27) : FILE_ATTRIBUTE_NORMAL), NULL);
        if (h != INVALID_HANDLE_VALUE) {
            if (disp == 0) info = 0; else if (disp == 2) info = 2;
            else if (disp == 3 || disp == 5) info = exists ? (disp == 5 ? 3 : 1) : 2;
            else if (disp == 4) info = 3;
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        st = err_to_status(GetLastError());
        TRACE("NtCreateFile(%s) failed: 0x%08lx", xp, st);
        iosb->Status = st; iosb->Information = 5;
        return st;
    }
    FileEnt *f = fent_new(h, isdir ? H_DIR : H_FILE);
    snprintf(f->xpath, MAX_PATH, "%s", xp); snprintf(f->hpath, MAX_PATH, "%s", hp);
    TRACE("NtCreateFile(%s) -> %p", xp, h);
    *out = h; iosb->Status = 0; iosb->Information = info;
    return 0;
}

NTSTATUS_X KAPI x_NtOpenFile(PHANDLE out, ULONG access, X_OBJECT_ATTRIBUTES *oa, X_IO_STATUS_BLOCK *iosb, ULONG share, ULONG opts)
{ return x_NtCreateFile(out, access, oa, iosb, NULL, 0, share, 1, opts); }

NTSTATUS_X KAPI x_NtClose(HANDLE h)
{
    EnterCriticalSection(&g_file_lock);
    FileEnt *f = fent(h);
    if (f) { if (f->find && f->find != INVALID_HANDLE_VALUE) FindClose(f->find); f->kind = 0; f->h = 0; }
    LeaveCriticalSection(&g_file_lock);
    return CloseHandle(h) ? 0 : X_STATUS_INVALID_HANDLE;
}

typedef struct { void *fn; void *ctx; X_IO_STATUS_BLOCK *iosb; } ApcRec;
static void CALLBACK apc_thunk(ULONG_PTR p)
{
    ApcRec *r = (ApcRec *)p;
    uint32_t a[3] = { (uint32_t)r->ctx, (uint32_t)r->iosb, 0 };
    xcall(r->fn, 3, a);
    free(r);
}
static void io_complete(HANDLE ev, void *apc, void *ctx, X_IO_STATUS_BLOCK *iosb)
{
    if (ev) SetEvent(ev);
    if (apc) {
        ApcRec *r = malloc(sizeof *r); r->fn = apc; r->ctx = ctx; r->iosb = iosb;
        QueueUserAPC(apc_thunk, GetCurrentThread(), (ULONG_PTR)r);
    }
}

void KAPI x_NtUserIoApcDispatcher(PVOID ctx, X_IO_STATUS_BLOCK *iosb, ULONG reserved)
{
    uint32_t err = iosb->Status >= 0 ? 0 : x_RtlNtStatusToDosError(iosb->Status);
    uint32_t a[3] = { err, (uint32_t)iosb->Information, (uint32_t)iosb };
    xcall(ctx, 3, a);
}

static void seek_if(HANDLE h, PLARGE_INTEGER off)
{
    if (off && !(off->HighPart == -1 && off->LowPart == 0xFFFFFFFE)) SetFilePointerEx(h, *off, NULL, FILE_BEGIN);
}

NTSTATUS_X KAPI x_NtReadFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, X_IO_STATUS_BLOCK *iosb, PVOID buf, ULONG len, PLARGE_INTEGER off)
{
    DWORD got = 0;
    EnterCriticalSection(&g_file_lock);
    seek_if(h, off);
    BOOL ok = ReadFile(h, buf, len, &got, NULL);
    FileEnt *rf = fent(h);
    if (ok && rf && rf->kind == H_RAW && got < len) { memset((char *)buf + got, 0, len - got); got = len; }
    LeaveCriticalSection(&g_file_lock);
    NTSTATUS_X st = ok ? ((got == 0 && len) ? X_STATUS_END_OF_FILE : 0) : err_to_status(GetLastError());
    if (g_trace || st) { FileEnt *f = fent(h); xlog("NtReadFile(%s, off=%lld, len=%lu) -> 0x%08lx got %lu  ev=%p apc=%p ctx=%p iosb=%p ret=%p", f ? f->xpath : "?", off ? off->QuadPart : -1LL, len, st, got, ev, apc, ctx, iosb, __builtin_return_address(0)); }
    iosb->Status = st; iosb->Information = got;
    io_complete(ev, apc, ctx, iosb);
    return st;
}
NTSTATUS_X KAPI x_NtWriteFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, X_IO_STATUS_BLOCK *iosb, PVOID buf, ULONG len, PLARGE_INTEGER off)
{
    DWORD put = 0;
    EnterCriticalSection(&g_file_lock);
    seek_if(h, off);
    BOOL ok = WriteFile(h, buf, len, &put, NULL);
    LeaveCriticalSection(&g_file_lock);
    NTSTATUS_X st = ok ? 0 : err_to_status(GetLastError());
    iosb->Status = st; iosb->Information = put;
    io_complete(ev, apc, ctx, iosb);
    return st;
}
NTSTATUS_X KAPI x_NtFlushBuffersFile(HANDLE h, X_IO_STATUS_BLOCK *iosb)
{ FlushFileBuffers(h); iosb->Status = 0; iosb->Information = 0; return 0; }

static LONGLONG ft64(FILETIME f) { return ((LONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

NTSTATUS_X KAPI x_NtQueryInformationFile(HANDLE h, X_IO_STATUS_BLOCK *iosb, PVOID buf, ULONG len, ULONG cls)
{
    BY_HANDLE_FILE_INFORMATION bi; LONGLONG *q = buf; ULONG *u = buf;
    if (!GetFileInformationByHandle(h, &bi)) memset(&bi, 0, sizeof bi);
    LONGLONG size = ((LONGLONG)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    LONGLONG alloc = (size + 0x3FFF) & ~0x3FFFLL;
    memset(buf, 0, len);
    switch (cls) {
    case 4:  q[0] = ft64(bi.ftCreationTime); q[1] = ft64(bi.ftLastAccessTime); q[2] = q[3] = ft64(bi.ftLastWriteTime);
             u[8] = bi.dwFileAttributes; iosb->Information = 40; break;
    case 5:  q[0] = alloc; q[1] = size; u[4] = 1; ((BYTE *)buf)[21] = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
             iosb->Information = 24; break;
    case 14: { LARGE_INTEGER z = {0}, p; SetFilePointerEx(h, z, &p, FILE_CURRENT); q[0] = p.QuadPart; iosb->Information = 8; break; }
    case 34: q[0] = ft64(bi.ftCreationTime); q[1] = ft64(bi.ftLastAccessTime); q[2] = q[3] = ft64(bi.ftLastWriteTime);
             q[4] = alloc; q[5] = size; u[12] = bi.dwFileAttributes; iosb->Information = 56; break;
    default: xlog("NtQueryInformationFile: class %lu unimplemented", cls); iosb->Information = 0; break;
    }
    iosb->Status = 0;
    return 0;
}

NTSTATUS_X KAPI x_NtSetInformationFile(HANDLE h, X_IO_STATUS_BLOCK *iosb, PVOID buf, ULONG len, ULONG cls)
{
    NTSTATUS_X st = 0;
    switch (cls) {
    case 14: if (!SetFilePointerEx(h, *(LARGE_INTEGER *)buf, NULL, FILE_BEGIN)) st = err_to_status(GetLastError()); break;
    case 20: { LARGE_INTEGER z = {0}, cur; SetFilePointerEx(h, z, &cur, FILE_CURRENT);
               SetFilePointerEx(h, *(LARGE_INTEGER *)buf, NULL, FILE_BEGIN);
               if (!SetEndOfFile(h)) st = err_to_status(GetLastError());
               SetFilePointerEx(h, cur, NULL, FILE_BEGIN); break; }
    case 13: { FILE_DISPOSITION_INFO di = { *(BOOLEAN *)buf };
               if (!SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di)) st = err_to_status(GetLastError()); break; }
    case 10: { /* FILE_RENAME_INFORMATION { BOOLEAN Replace; HANDLE Root; OBJECT_STRING Name; } */
               struct { BOOLEAN replace; HANDLE root; X_STRING name; } *ri = buf;
               FileEnt *f = fent(h);
               X_OBJECT_ATTRIBUTES oa = { ri->root, &ri->name, 0 };
               char xp[MAX_PATH], hp[MAX_PATH];
               st = oa_resolve(&oa, xp, hp);
               if (!st && f && !MoveFileExA(f->hpath, hp, ri->replace ? MOVEFILE_REPLACE_EXISTING : 0)) st = err_to_status(GetLastError());
               if (!st && f) { snprintf(f->hpath, MAX_PATH, "%s", hp); snprintf(f->xpath, MAX_PATH, "%s", xp); }
               break; }
    case 4: case 19: break;
    default: xlog("NtSetInformationFile: class %lu unimplemented", cls); break;
    }
    iosb->Status = st; iosb->Information = 0;
    return st;
}

NTSTATUS_X KAPI x_NtQueryVolumeInformationFile(HANDLE h, X_IO_STATUS_BLOCK *iosb, PVOID buf, ULONG len, ULONG cls)
{
    LONGLONG *q = buf; ULONG *u = buf;
    const LONGLONG cluster = 16384, total = (8LL << 30) / cluster, avail = (4LL << 30) / cluster;
    memset(buf, 0, len);
    switch (cls) {
    case 1: u[2] = 0x4A535246; iosb->Information = 18; break;                     /* FileFsVolumeInformation */
    case 3: q[0] = total; q[1] = avail; u[4] = 32; u[5] = 512; iosb->Information = 24; break; /* Size */
    case 4: u[0] = 7; iosb->Information = 8; break;                               /* Device: disk */
    case 5: u[0] = 0; u[1] = 42; u[2] = 4; memcpy(&u[3], "FATX", 4); iosb->Information = 16; break;
    case 7: q[0] = total; q[1] = q[2] = avail; u[6] = 32; u[7] = 512; iosb->Information = 32; break;
    default: xlog("NtQueryVolumeInformationFile: class %lu unimplemented", cls); break;
    }
    iosb->Status = 0;
    return 0;
}

static int mask_match(const char *mask, const char *name)
{
    if (!*mask) return !*name;
    if (*mask == '*') { for (;; name++) { if (mask_match(mask + 1, name)) return 1; if (!*name) return 0; } }
    if (*name && (*mask == '?' || toupper((unsigned char)*mask) == toupper((unsigned char)*name))) return mask_match(mask + 1, name + 1);
    return 0;
}

NTSTATUS_X KAPI x_NtQueryDirectoryFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, X_IO_STATUS_BLOCK *iosb,
    PVOID buf, ULONG len, ULONG cls, X_STRING *mask, BOOLEAN restart)
{
    FileEnt *f = fent(h);
    if (!f || f->kind != H_DIR) { iosb->Status = X_STATUS_INVALID_HANDLE; return X_STATUS_INVALID_HANDLE; }
    static char masks[1024][64]; int idx = (int)(f - g_files);
    WIN32_FIND_DATAA fd; BOOL first = FALSE;
    if (restart || !f->find || f->find == INVALID_HANDLE_VALUE) {
        if (f->find && f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
        str_from_x(mask, masks[idx], 64);
        if (!masks[idx][0]) strcpy(masks[idx], "*");
        char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", f->hpath);
        f->find = FindFirstFileA(pat, &fd); first = TRUE;
        if (f->find == INVALID_HANDLE_VALUE) { iosb->Status = X_STATUS_NO_SUCH_FILE; return X_STATUS_NO_SUCH_FILE; }
    } else if (!FindNextFileA(f->find, &fd)) { iosb->Status = X_STATUS_NO_MORE_FILES; return X_STATUS_NO_MORE_FILES; }
    for (;;) {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") && mask_match(masks[idx], fd.cFileName)) break;
        if (!FindNextFileA(f->find, &fd)) {
            NTSTATUS_X st = first ? X_STATUS_NO_SUCH_FILE : X_STATUS_NO_MORE_FILES;
            iosb->Status = st; return st;
        }
    }
    ULONG nlen = (ULONG)strlen(fd.cFileName);
    if (len < 0x40 + nlen) { iosb->Status = X_STATUS_BUFFER_TOO_SMALL; return X_STATUS_BUFFER_TOO_SMALL; }
    memset(buf, 0, 0x40);
    LONGLONG *q = (LONGLONG *)((char *)buf + 8); ULONG *u = buf;
    LONGLONG size = ((LONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    q[0] = ft64(fd.ftCreationTime); q[1] = ft64(fd.ftLastAccessTime); q[2] = q[3] = ft64(fd.ftLastWriteTime);
    q[4] = size; q[5] = (size + 0x3FFF) & ~0x3FFFLL;
    u[14] = fd.dwFileAttributes & 0x37; u[15] = nlen;
    memcpy((char *)buf + 0x40, fd.cFileName, nlen);
    iosb->Status = 0; iosb->Information = 0x40 + nlen;
    io_complete(ev, apc, ctx, iosb);
    return 0;
}

NTSTATUS_X KAPI x_NtDeviceIoControlFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, X_IO_STATUS_BLOCK *iosb,
    ULONG code, PVOID in, ULONG inlen, PVOID out, ULONG outlen)
{
    FileEnt *f = fent(h);
    if (out) memset(out, 0, outlen);
    /* Xbox HDD layout (bytes): 1=E 4.88G, 2=C 500M, 3/4/5 = X/Y/Z cache 750M each */
    static const LONGLONG pstart[] = { 0, 0xABE80000LL, 0x8CA80000LL, 0x80000LL, 0x2EE80000LL, 0x5DC80000LL };
    static const LONGLONG plen[]   = { 0x1D1C10000LL, 0x1312D6000LL, 0x1F400000LL, 0x2EE00000LL, 0x2EE00000LL, 0x2EE00000LL };
    int part = f ? f->part : 0; if (part < 0 || part > 5) part = 0;
    if (code == 0x00074004 && out && outlen >= 28) {         /* IOCTL_DISK_GET_PARTITION_INFO */
        LONGLONG *q = out; q[0] = pstart[part]; q[1] = plen[part];
        ((ULONG *)out)[4] = (ULONG)(pstart[part] / 512); ((ULONG *)out)[5] = part;
        ((BYTE *)out)[24] = 0xE0; ((BYTE *)out)[26] = 1;
    } else if (code == 0x00070000 && out && outlen >= 24) {  /* IOCTL_DISK_GET_DRIVE_GEOMETRY */
        LONGLONG *q = out; ULONG *u = out;
        q[0] = plen[part] / (512LL * 63 * 255); u[2] = 12; u[3] = 255; u[4] = 63; u[5] = 512;
    } else xlog("NtDeviceIoControlFile(%s, 0x%08lx, in=%lu out=%lu) -> zeroed success", f ? f->xpath : "?", code, inlen, outlen);
    TRACE("NtDeviceIoControlFile(%s, 0x%08lx)", f ? f->xpath : "?", code);
    iosb->Status = 0; iosb->Information = outlen;
    io_complete(ev, apc, ctx, iosb);
    return 0;
}
NTSTATUS_X KAPI x_NtFsControlFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, X_IO_STATUS_BLOCK *iosb,
    ULONG code, PVOID in, ULONG inlen, PVOID out, ULONG outlen)
{ return x_NtDeviceIoControlFile(h, ev, apc, ctx, iosb, code, in, inlen, out, outlen); }

static void kfile_init(void)
{
    snprintf(g_hdd_dir, sizeof g_hdd_dir, "%s\\_hdd", g_game_dir);
    CreateDirectoryA(g_hdd_dir, NULL);
    /* The Xbox kernel maps D: to the launch media before the title starts. */
    link_set("\\??\\D:", "\\Device\\CdRom0");
}
