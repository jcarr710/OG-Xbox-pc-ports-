/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Xbox kernel types used by the JSRF loader/HLE layer. */
#pragma once
#include <windows.h>
#include <stdint.h>

typedef LONG NTSTATUS_X;
#define X_STATUS_SUCCESS                 ((NTSTATUS_X)0x00000000L)
#define X_STATUS_PENDING                 ((NTSTATUS_X)0x00000103L)
#define X_STATUS_TIMEOUT                 ((NTSTATUS_X)0x00000102L)
#define X_STATUS_ALERTED                 ((NTSTATUS_X)0x00000101L)
#define X_STATUS_USER_APC                ((NTSTATUS_X)0x000000C0L)
#define X_STATUS_NO_MORE_FILES           ((NTSTATUS_X)0x80000006L)
#define X_STATUS_UNSUCCESSFUL            ((NTSTATUS_X)0xC0000001L)
#define X_STATUS_NOT_IMPLEMENTED         ((NTSTATUS_X)0xC0000002L)
#define X_STATUS_INVALID_HANDLE          ((NTSTATUS_X)0xC0000008L)
#define X_STATUS_INVALID_PARAMETER       ((NTSTATUS_X)0xC000000DL)
#define X_STATUS_NO_SUCH_FILE            ((NTSTATUS_X)0xC000000FL)
#define X_STATUS_END_OF_FILE             ((NTSTATUS_X)0xC0000011L)
#define X_STATUS_NO_MEMORY               ((NTSTATUS_X)0xC0000017L)
#define X_STATUS_ACCESS_DENIED           ((NTSTATUS_X)0xC0000022L)
#define X_STATUS_BUFFER_TOO_SMALL        ((NTSTATUS_X)0xC0000023L)
#define X_STATUS_OBJECT_NAME_INVALID     ((NTSTATUS_X)0xC0000033L)
#define X_STATUS_OBJECT_NAME_NOT_FOUND   ((NTSTATUS_X)0xC0000034L)
#define X_STATUS_OBJECT_NAME_COLLISION   ((NTSTATUS_X)0xC0000035L)
#define X_STATUS_OBJECT_PATH_NOT_FOUND   ((NTSTATUS_X)0xC000003AL)
#define X_STATUS_INVALID_DEVICE_REQUEST  ((NTSTATUS_X)0xC0000010L)
#define X_STATUS_INVALID_INFO_CLASS      ((NTSTATUS_X)0xC0000003L)
#define X_STATUS_DIRECTORY_NOT_EMPTY     ((NTSTATUS_X)0xC0000101L)
#define X_STATUS_NOT_A_DIRECTORY         ((NTSTATUS_X)0xC0000103L)
#define X_STATUS_FILE_IS_A_DIRECTORY     ((NTSTATUS_X)0xC00000BAL)

/* Xbox strings are ANSI. */
typedef struct { USHORT Length, MaximumLength; char *Buffer; } X_STRING;

typedef struct {
    HANDLE   RootDirectory;
    X_STRING *ObjectName;
    ULONG    Attributes;
} X_OBJECT_ATTRIBUTES;

typedef struct {
    union { NTSTATUS_X Status; PVOID Pointer; };
    ULONG_PTR Information;
} X_IO_STATUS_BLOCK;

typedef struct {
    UCHAR Type, Absolute, Size, Inserted;
    LONG  SignalState;
    LIST_ENTRY WaitListHead;
} X_DISPATCHER_HEADER;

typedef struct { X_DISPATCHER_HEADER Header; } X_KEVENT;

typedef struct {
    X_DISPATCHER_HEADER Event;   /* Xbox embeds a KEVENT */
    LONG   LockCount;
    LONG   RecursionCount;
    HANDLE OwningThread;
} X_CRITICAL_SECTION;

typedef struct X_KDPC {
    SHORT Type; UCHAR Inserted; UCHAR Padding;
    LIST_ENTRY DpcListEntry;
    void (__stdcall *DeferredRoutine)(struct X_KDPC*, PVOID, PVOID, PVOID);
    PVOID DeferredContext, SystemArgument1, SystemArgument2;
} X_KDPC;

typedef struct {
    X_DISPATCHER_HEADER Header;
    ULARGE_INTEGER DueTime;
    LIST_ENTRY TimerListEntry;
    X_KDPC *Dpc;
    LONG Period;
} X_KTIMER;

/* XBE header (subset) */
typedef struct {
    uint32_t Magic;
    uint8_t  Signature[256];
    uint32_t BaseAddress, SizeOfHeaders, SizeOfImage, SizeOfImageHeader, TimeDateStamp;
    uint32_t CertificateAddress, NumberOfSections, SectionHeadersAddress, InitFlags;
    uint32_t EntryPoint, TlsAddress, StackCommit, HeapReserve, HeapCommit;
    uint32_t PeBaseAddress, PeSizeOfImage, PeChecksum, PeTimeDate;
    uint32_t DebugPathName, DebugFileName, DebugUnicodeFileName;
    uint32_t KernelThunkAddress, NonKernelImportDir, NumLibraryVersions, LibraryVersions;
    uint32_t KernelLibraryVersion, XapiLibraryVersion, LogoBitmapAddress, LogoBitmapSize;
} XBE_HEADER;

typedef struct {
    uint32_t Flags, VirtualAddress, VirtualSize, PointerToRawData, SizeOfRawData;
    uint32_t SectionName, SectionReferenceCount, HeadSharedRefCount, TailSharedRefCount;
    uint8_t  Digest[20];
} XBE_SECTION;

#define XBE_RETAIL_ENTRY_KEY 0xA8FC57ABu
#define XBE_RETAIL_THUNK_KEY 0x5B6D40B6u
#define XBE_DEBUG_ENTRY_KEY  0x94859D4Bu
#define XBE_DEBUG_THUNK_KEY  0xEFB1F152u

/* ---- Fake per-thread kernel structures ----
 * Game/XDK code reads fs:[0x20] (KPCR.Prcb), fs:[0x24] (Irql),
 * fs:[0x28] (Prcb.CurrentThread) and fs:[0x58]. The loader patches those
 * instructions to read from this block, located via TEB.ArbitraryUserPointer. */
typedef struct {
    uint8_t  pad0[0x20];
    void    *Prcb;          /* 0x20 -> &CurrentThread below (Prcb at 0x28) */
    uint8_t  Irql;          /* 0x24 */
    uint8_t  pad1[3];
    void    *CurrentThread; /* 0x28 (Prcb + 0) */
    uint8_t  prcb_rest[0x400];
} X_FAKE_KPCR;

typedef struct {
    uint8_t  pad0[0x28];
    void    *TlsData;       /* 0x28 */
    uint8_t  pad1[0x12c - 0x2c];
    uint32_t UniqueThread;  /* 0x12c */
    uint8_t  pad2[0x200 - 0x130];
} X_FAKE_ETHREAD;

/* Per-thread host bookkeeping. */
typedef struct XThread {
    X_FAKE_KPCR    pcr;
    X_FAKE_ETHREAD ethread;
    HANDLE   host_handle;
    DWORD    host_tid;
    void    *stack_mem;
    SIZE_T   stack_size;
    uint32_t tls_size;
    void   (__stdcall *start)(void *);
    void    *context;
    void   (__stdcall *system_routine)(void *, void *);
} XThread;

/* globals provided by main.c */
extern XBE_HEADER *g_xbe;
extern char g_game_dir[MAX_PATH];
void xlog(const char *fmt, ...);
typedef struct { uint32_t h[5]; uint64_t n; uint8_t buf[64]; } SHA1C;   /* kernel.c, XcSHA* */
void __stdcall x_XcSHAInit(SHA1C *c);
void __stdcall x_XcSHAUpdate(SHA1C *c, const uint8_t *p, ULONG len);
void __stdcall x_XcSHAFinal(SHA1C *c, uint8_t *out);
HANDLE host_create_thread(SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid);
void xfatal(const char *fmt, ...);

/* kthread.c */
XThread *xthread_create(void (__stdcall *start)(void *), void *ctx,
                        void (__stdcall *sysroutine)(void *, void *),
                        SIZE_T stack, uint32_t tls, BOOL suspended);
XThread *xthread_current(void);
void xthread_pin_game(XThread *t);
extern DWORD_PTR g_game_affinity;
void xthread_adopt_current(void);

/* kernel.c */
void kernel_init(void);
uintptr_t kernel_resolve(uint32_t ordinal);
