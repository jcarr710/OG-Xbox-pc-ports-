/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reserved address space for the XBE image: 0x11000 .. 0x1000000 (main.c's
 * XBE_LIMIT). It is part of the exe image, placed right after the PE header
 * by xbe.ld, so nothing else can take these addresses; being uninitialised
 * it costs no file space. */
        .section .xbeimg,"bwx"
        .globl _xbe_reserve
_xbe_reserve:
        .space 0xFEF000

        .text
/* Replacements for patched `mov eax, fs:[N]` reads (see patch_fs in main.c).
 * TEB.ArbitraryUserPointer (fs:[0x14]) points at this thread's X_FAKE_KPCR.
 * They must not touch flags or any register except eax. */
        .globl _fs_read_20, _fs_read_28, _fs_read_58, _fs_read_24b
_fs_read_20:  movl %fs:0x14, %eax ; movl 0x20(%eax), %eax ; ret
_fs_read_28:  movl %fs:0x14, %eax ; movl 0x28(%eax), %eax ; ret
_fs_read_58:  movl %fs:0x14, %eax ; movl 0x58(%eax), %eax ; ret
_fs_read_24b: movl %fs:0x14, %eax ; movzbl 0x24(%eax), %eax ; ret

/* rdtsc replacement: Xbox-rate (733.33 MHz) timestamp in edx:eax. */
        .globl _hle_rdtsc
_hle_rdtsc:
        pushfl
        pushl %ecx
        call _xbox_tsc
        popl %ecx
        popfl
        ret

/* `push edx; rdtsc; pop edx; ret` replacement: low dword only. */
        .globl _hle_rdtsc_lo
_hle_rdtsc_lo:
        pushfl
        pushl %ecx
        pushl %edx
        call _xbox_tsc
        popl %edx
        popl %ecx
        popfl
        ret

/* Unresolved kernel import: `push ordinal; jmp _unimpl_entry` */
        .globl _unimpl_entry
_unimpl_entry:
        call _unimpl_kernel_call
        int3
