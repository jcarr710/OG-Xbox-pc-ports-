/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Device access emulation.
 *
 * On the Xbox the XDK libraries run in kernel mode and talk to hardware
 * directly: memory-mapped registers (GPU at 0xFD000000, APU at 0xFE800000,
 * USB at 0xFED00000, ...), port I/O, cli/sti, wbinvd, MSRs, control registers.
 * Those ranges are reserved PAGE_NOACCESS here, so every access faults into
 * this vectored handler, which decodes the x86 instruction, performs it
 * against the emulated device, fixes up registers/flags and resumes. */
#include "xbox.h"
#include "mmio.h"
#include "xmem.h"
#include <string.h>
#include <stdio.h>

int g_mmio_trace;
static uint32_t g_eip; /* instruction being emulated (for device logging) */
CONTEXT *g_mmio_ctx;   /* register state of the faulting thread (for device diagnostics) */
uint32_t mmio_current_eip(void) { return g_eip; }

/* ------------------------------------------------------------------ */
/* Ethernet controller (nForce MCP NIC at 0xFEF00000). Titles that link  */
/* the Xbox Live libraries initialise it; it is kept as a register file  */
/* that reports itself idle, and the PHY (kernel.c) reports no cable, so */
/* the network stack comes up offline.                                   */
/* ------------------------------------------------------------------ */
static uint32_t g_nic[0x400 / 4];
static uint32_t nic_read(uint32_t off, int sz)
{
    uint32_t v = g_nic[(off & 0x3FC) / 4];
    if ((off & 0x3FC) == 0x144) v |= 0x08;                       /* TxRxControl: idle */
    if ((off & 0x3FC) == 0x000) v = 0;                           /* no interrupts pending */
    return (v >> ((off & 3) * 8)) & (sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu);
}
static void nic_write(uint32_t off, int sz, uint32_t v)
{
    uint32_t *r = &g_nic[(off & 0x3FC) / 4], sh = (off & 3) * 8;
    uint32_t m = (sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu) << sh;
    *r = (*r & ~m) | ((v << sh) & m);
}

/* ------------------------------------------------------------------ */
/* Device dispatch                                                     */
/* ------------------------------------------------------------------ */
static uint32_t dev_read(uint32_t a, int sz)
{
    uint32_t v;
    if (a >= 0xFD000000 && a < 0xFE000000) v = nv2a_mmio_read(a - 0xFD000000, sz);
    else if (a >= 0xFE800000 && a < 0xFE880000) v = apu_mmio_read(a - 0xFE800000, sz);
    else if (a >= 0xFEC00000 && a < 0xFEC01000) v = ac97_mmio_read(a - 0xFEC00000, sz);
    else if (a >= 0xFEF00000 && a < 0xFEF00400) v = nic_read(a - 0xFEF00000, sz);
    else { v = 0; xlog("mmio: read%d %08x (unmapped device) eip=%08x", sz * 8, a, g_eip); }
    if (g_mmio_trace) xlog("mmio R%d [%08x] -> %08x  eip=%08x", sz * 8, a, v, g_eip);
    return v;
}
static void dev_write(uint32_t a, int sz, uint32_t v)
{
    if (g_mmio_trace) xlog("mmio W%d [%08x] <- %08x  eip=%08x", sz * 8, a, v, g_eip);
    if (a >= 0xFD000000 && a < 0xFE000000) nv2a_mmio_write(a - 0xFD000000, sz, v);
    else if (a >= 0xFE800000 && a < 0xFE880000) apu_mmio_write(a - 0xFE800000, sz, v);
    else if (a >= 0xFEC00000 && a < 0xFEC01000) ac97_mmio_write(a - 0xFEC00000, sz, v);
    else if (a >= 0xFEF00000 && a < 0xFEF00400) nic_write(a - 0xFEF00000, sz, v);
    else xlog("mmio: write%d %08x <- %08x (unmapped device) eip=%08x", sz * 8, a, v, g_eip);
}

int mmio_is_device(uint32_t a) { return a >= MMIO_BASE && a < MMIO_END; }

/* memory operand access: device or ordinary memory (for string ops) */
static uint32_t mem_read(uint32_t a, int sz)
{
    if (mmio_is_device(a)) return dev_read(a, sz);
    switch (sz) { case 1: return *(uint8_t *)a; case 2: return *(uint16_t *)a; default: return *(uint32_t *)a; }
}
static void mem_write(uint32_t a, int sz, uint32_t v)
{
    if (mmio_is_device(a)) { dev_write(a, sz, v); return; }
    switch (sz) { case 1: *(uint8_t *)a = (uint8_t)v; break; case 2: *(uint16_t *)a = (uint16_t)v; break; default: *(uint32_t *)a = v; }
}

/* ------------------------------------------------------------------ */
/* Port I/O                                                            */
/* ------------------------------------------------------------------ */
static uint32_t port_in(uint16_t port, int sz)
{
    uint32_t v = 0;
    if (port >= 0x8000 && port < 0x8100) {        /* MCPX ACPI / PM block */
        if (port == 0x8008) v = (uint32_t)(xbox_tsc() / 205); /* PM timer ~3.579545 MHz */
    }
    if (g_mmio_trace || port < 0x8000 || port >= 0x8100)
        xlog("port in%d  %04x -> %08x  eip=%08x", sz * 8, port, v, g_eip);
    return v;
}
static void port_out(uint16_t port, int sz, uint32_t v)
{
    if (g_mmio_trace || port < 0x8000 || port >= 0x8100)
        xlog("port out%d %04x <- %08x  eip=%08x", sz * 8, port, v, g_eip);
}

/* ------------------------------------------------------------------ */
/* x86 decoding helpers                                                */
/* ------------------------------------------------------------------ */
static uint32_t *r32(CONTEXT *c, int r)
{
    switch (r & 7) {
    case 0: return (uint32_t *)&c->Eax; case 1: return (uint32_t *)&c->Ecx;
    case 2: return (uint32_t *)&c->Edx; case 3: return (uint32_t *)&c->Ebx;
    case 4: return (uint32_t *)&c->Esp; case 5: return (uint32_t *)&c->Ebp;
    case 6: return (uint32_t *)&c->Esi; default: return (uint32_t *)&c->Edi;
    }
}
static uint32_t get_reg(CONTEXT *c, int r, int sz)
{
    if (sz == 1) return (r < 4) ? (*r32(c, r) & 0xFF) : ((*r32(c, r - 4) >> 8) & 0xFF);
    if (sz == 2) return *r32(c, r) & 0xFFFF;
    return *r32(c, r);
}
static void set_reg(CONTEXT *c, int r, int sz, uint32_t v)
{
    if (sz == 1) {
        if (r < 4) *r32(c, r) = (*r32(c, r) & ~0xFFu) | (v & 0xFF);
        else *r32(c, r - 4) = (*r32(c, r - 4) & ~0xFF00u) | ((v & 0xFF) << 8);
    } else if (sz == 2) *r32(c, r) = (*r32(c, r) & ~0xFFFFu) | (v & 0xFFFF);
    else *r32(c, r) = v;
}

typedef struct { int mod, reg, rm; uint32_t ea; } ModRM;
static uint8_t *decode_modrm(CONTEXT *c, uint8_t *p, ModRM *m)
{
    uint8_t b = *p++;
    m->mod = b >> 6; m->reg = (b >> 3) & 7; m->rm = b & 7; m->ea = 0;
    if (m->mod == 3) return p;
    if (m->rm == 4) {
        uint8_t sib = *p++;
        int ss = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (idx != 4) m->ea += *r32(c, idx) << ss;
        if (base == 5 && m->mod == 0) { m->ea += *(uint32_t *)p; p += 4; }
        else m->ea += *r32(c, base);
    } else if (m->rm == 5 && m->mod == 0) { m->ea = *(uint32_t *)p; p += 4; return p; }
    else m->ea = *r32(c, m->rm);
    if (m->mod == 1) { m->ea += (int8_t)*p; p++; }
    else if (m->mod == 2) { m->ea += *(int32_t *)p; p += 4; }
    return p;
}

#define FL_CF 0x001
#define FL_PF 0x004
#define FL_AF 0x010
#define FL_ZF 0x040
#define FL_SF 0x080
#define FL_OF 0x800
#define FL_ARITH (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF)

static int parity8(uint32_t v) { v &= 0xFF; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

/* op: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp; returns result, updates flags */
static uint32_t alu(int op, uint32_t a, uint32_t b, int sz, DWORD *eflags)
{
    uint32_t mask = sz == 1 ? 0xFF : sz == 2 ? 0xFFFF : 0xFFFFFFFF, msb = (mask >> 1) + 1;
    uint32_t cin = (*eflags & FL_CF) ? 1 : 0, r, f = 0;
    a &= mask; b &= mask;
    switch (op) {
    case 0: case 2: {
        uint64_t s = (uint64_t)a + b + (op == 2 ? cin : 0); r = (uint32_t)s & mask;
        if (s > mask) f |= FL_CF;
        if ((~(a ^ b) & (a ^ r)) & msb) f |= FL_OF;
        if ((a ^ b ^ r) & 0x10) f |= FL_AF;
        break; }
    case 3: case 5: case 7: {
        uint64_t s = (uint64_t)a - b - (op == 3 ? cin : 0); r = (uint32_t)s & mask;
        if ((uint64_t)a < (uint64_t)b + (op == 3 ? cin : 0)) f |= FL_CF;
        if (((a ^ b) & (a ^ r)) & msb) f |= FL_OF;
        if ((a ^ b ^ r) & 0x10) f |= FL_AF;
        break; }
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    default: r = a ^ b; break;
    }
    if (!r) f |= FL_ZF;
    if (r & msb) f |= FL_SF;
    if (parity8(r)) f |= FL_PF;
    *eflags = (*eflags & ~FL_ARITH) | f;
    return r;
}

/* ------------------------------------------------------------------ */
/* Instruction emulation. Returns instruction length or 0 if unknown.  */
/* ------------------------------------------------------------------ */
static int emulate(CONTEXT *c)
{
    uint8_t *start = (uint8_t *)c->Eip, *p = start;
    int opsz = 4, rep = 0;
    for (;;) {
        uint8_t b = *p;
        if (b == 0x66) { opsz = 2; p++; }
        else if (b == 0xF3 || b == 0xF2) { rep = b; p++; }
        else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) p++;
        else break;
    }
    uint8_t op = *p++;
    ModRM m;
    switch (op) {
    /* --- mov --- */
    case 0x88: p = decode_modrm(c, p, &m); mem_write(m.ea, 1, get_reg(c, m.reg, 1)); break;
    case 0x89: p = decode_modrm(c, p, &m); mem_write(m.ea, opsz, get_reg(c, m.reg, opsz)); break;
    case 0x8A: p = decode_modrm(c, p, &m); set_reg(c, m.reg, 1, mem_read(m.ea, 1)); break;
    case 0x8B: p = decode_modrm(c, p, &m); set_reg(c, m.reg, opsz, mem_read(m.ea, opsz)); break;
    case 0xC6: p = decode_modrm(c, p, &m); mem_write(m.ea, 1, *p); p += 1; break;
    case 0xC7: p = decode_modrm(c, p, &m);
        if (opsz == 2) { mem_write(m.ea, 2, *(uint16_t *)p); p += 2; } else { mem_write(m.ea, 4, *(uint32_t *)p); p += 4; }
        break;
    case 0xA0: set_reg(c, 0, 1, mem_read(*(uint32_t *)p, 1)); p += 4; break;
    case 0xA1: set_reg(c, 0, opsz, mem_read(*(uint32_t *)p, opsz)); p += 4; break;
    case 0xA2: mem_write(*(uint32_t *)p, 1, get_reg(c, 0, 1)); p += 4; break;
    case 0xA3: mem_write(*(uint32_t *)p, opsz, get_reg(c, 0, opsz)); p += 4; break;

    /* --- two-operand ALU: op r/m,r / op r,r/m --- */
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: {
        int aop = (op >> 3) & 7, sz = (op & 1) ? opsz : 1, dir = op & 2;
        p = decode_modrm(c, p, &m);
        uint32_t mv = mem_read(m.ea, sz), rv = get_reg(c, m.reg, sz);
        if (dir) { uint32_t r = alu(aop, rv, mv, sz, &c->EFlags); if (aop != 7) set_reg(c, m.reg, sz, r); }
        else     { uint32_t r = alu(aop, mv, rv, sz, &c->EFlags); if (aop != 7) mem_write(m.ea, sz, r); }
        break; }
    case 0x80: case 0x81: case 0x83: {
        int sz = op == 0x80 ? 1 : opsz;
        p = decode_modrm(c, p, &m);
        uint32_t imm;
        if (op == 0x81) { if (opsz == 2) { imm = *(uint16_t *)p; p += 2; } else { imm = *(uint32_t *)p; p += 4; } }
        else { imm = op == 0x83 ? (uint32_t)(int32_t)(int8_t)*p : *p; p++; }
        uint32_t r = alu(m.reg, mem_read(m.ea, sz), imm, sz, &c->EFlags);
        if (m.reg != 7) mem_write(m.ea, sz, r);
        break; }
    case 0x84: case 0x85: {
        int sz = op == 0x84 ? 1 : opsz;
        p = decode_modrm(c, p, &m);
        alu(4, mem_read(m.ea, sz), get_reg(c, m.reg, sz), sz, &c->EFlags);
        break; }
    case 0xF6: case 0xF7: {
        int sz = op == 0xF6 ? 1 : opsz;
        p = decode_modrm(c, p, &m);
        if (m.reg != 0) return 0;
        uint32_t imm;
        if (sz == 1) { imm = *p; p++; } else if (sz == 2) { imm = *(uint16_t *)p; p += 2; } else { imm = *(uint32_t *)p; p += 4; }
        alu(4, mem_read(m.ea, sz), imm, sz, &c->EFlags);
        break; }
    case 0xFE: case 0xFF: {
        int sz = op == 0xFE ? 1 : opsz;
        p = decode_modrm(c, p, &m);
        if (m.reg > 1) return 0;
        DWORD cf = c->EFlags & FL_CF;
        uint32_t r = alu(m.reg == 0 ? 0 : 5, mem_read(m.ea, sz), 1, sz, &c->EFlags);
        c->EFlags = (c->EFlags & ~FL_CF) | cf;
        mem_write(m.ea, sz, r);
        break; }
    case 0x86: case 0x87: {
        int sz = op == 0x86 ? 1 : opsz;
        p = decode_modrm(c, p, &m);
        uint32_t mv = mem_read(m.ea, sz);
        mem_write(m.ea, sz, get_reg(c, m.reg, sz));
        set_reg(c, m.reg, sz, mv);
        break; }

    /* --- string ops (block copies to/from device memory) --- */
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: case 0xAC: case 0xAD: {
        int sz = (op & 1) ? opsz : 1;
        int step = (c->EFlags & 0x400) ? -sz : sz;
        uint32_t n = rep ? c->Ecx : 1;
        for (; n; n--) {
            if (op <= 0xA5) { mem_write(c->Edi, sz, mem_read(c->Esi, sz)); c->Esi += step; c->Edi += step; }
            else if (op <= 0xAB) { mem_write(c->Edi, sz, get_reg(c, 0, sz)); c->Edi += step; }
            else { set_reg(c, 0, sz, mem_read(c->Esi, sz)); c->Esi += step; }
        }
        if (rep) c->Ecx = 0;
        break; }

    /* --- port I/O --- */
    case 0xE4: set_reg(c, 0, 1, port_in(*p, 1)); p++; break;
    case 0xE5: set_reg(c, 0, opsz, port_in(*p, opsz)); p++; break;
    case 0xE6: port_out(*p, 1, get_reg(c, 0, 1)); p++; break;
    case 0xE7: port_out(*p, opsz, get_reg(c, 0, opsz)); p++; break;
    case 0xEC: set_reg(c, 0, 1, port_in((uint16_t)c->Edx, 1)); break;
    case 0xED: set_reg(c, 0, opsz, port_in((uint16_t)c->Edx, opsz)); break;
    case 0xEE: port_out((uint16_t)c->Edx, 1, get_reg(c, 0, 1)); break;
    case 0xEF: port_out((uint16_t)c->Edx, opsz, get_reg(c, 0, opsz)); break;

    /* --- privileged no-ops --- */
    case 0xFA: case 0xFB: break;            /* cli / sti */
    case 0xF4: SwitchToThread(); break;     /* hlt */

    case 0x0F: {
        uint8_t op2 = *p++;
        switch (op2) {
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
            int sz = (op2 & 1) ? 2 : 1;
            p = decode_modrm(c, p, &m);
            uint32_t v = mem_read(m.ea, sz);
            if (op2 >= 0xBE) v = sz == 1 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
            set_reg(c, m.reg, opsz, v);
            break; }
        case 0x08: case 0x09: break;                          /* invd / wbinvd */
        case 0x30: break;                                     /* wrmsr */
        case 0x32: c->Eax = 0; c->Edx = 0; break;             /* rdmsr */
        case 0x20: p = decode_modrm(c, p, &m);                /* mov r32, crN */
            set_reg(c, m.rm, 4, m.reg == 0 ? 0x8001003B : m.reg == 4 ? 0x00000690 : 0); break;
        case 0x22: p = decode_modrm(c, p, &m); break;         /* mov crN, r32 */
        case 0x01: p = decode_modrm(c, p, &m); break;         /* invlpg etc. */
        default: return 0;
        }
        break; }
    default:
        return 0;
    }
    return (int)(p - start);
}

/* Privileged instructions that kernel-mode XDK code executes. */
static int is_priv_insn(const uint8_t *p)
{
    while (*p == 0x66 || *p == 0xF3 || *p == 0xF2) p++;
    switch (p[0]) {
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0xFA: case 0xFB: case 0xF4: return 1;
    case 0x0F: return p[1] == 0x08 || p[1] == 0x09 || p[1] == 0x30 || p[1] == 0x32 || p[1] == 0x20 || p[1] == 0x22 || p[1] == 0x01;
    }
    return 0;
}

volatile LONG g_mmio_traps;

/* Trap counts per instruction address (for --perf). */
static struct { uint32_t eip, addr; volatile LONG n; } g_hot[1024];
static void hot_count(uint32_t eip, uint32_t addr)
{
    for (uint32_t i = (eip * 2654435761u) >> 22, k = 0; k < 16; k++, i = (i + 1) & 1023) {
        if (g_hot[i].eip == eip) { InterlockedIncrement(&g_hot[i].n); return; }
        if (!g_hot[i].eip && InterlockedCompareExchange((volatile LONG *)&g_hot[i].eip, eip, 0) == 0) {
            g_hot[i].addr = addr; InterlockedIncrement(&g_hot[i].n); return;
        }
    }
}
void mmio_report_hot(double secs)
{
    for (int r = 0; r < 16; r++) {
        int best = -1;
        for (int i = 0; i < 1024; i++) if (g_hot[i].n > 0 && (best < 0 || g_hot[i].n > g_hot[best].n)) best = i;
        if (best < 0 || g_hot[best].n < secs * 10) break;
        xlog("perf:   %6.0f traps/s at %08x (address %08x)", g_hot[best].n / secs, g_hot[best].eip, g_hot[best].addr);
        g_hot[best].n = -g_hot[best].n;           /* mark reported */
    }
    for (int i = 0; i < 1024; i++) g_hot[i].n = 0;
}

/* Is eip in the game's XBE image (where kernel-mode XDK code may run privileged instructions)? */
static int in_xbe(uint32_t eip) { return g_xbe && eip >= g_xbe->BaseAddress && eip < g_xbe->BaseAddress + g_xbe->SizeOfImage; }

static LONG CALLBACK mmio_veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    DWORD code = er->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uint32_t fa = (uint32_t)er->ExceptionInformation[1];
        /* Wine reports some privileged instructions as access violations */
        int priv = in_xbe(c->Eip) && is_priv_insn((const uint8_t *)c->Eip);
        if (!mmio_is_device(fa) && !priv) return EXCEPTION_CONTINUE_SEARCH;
    } else if (code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        if (!in_xbe(c->Eip)) return EXCEPTION_CONTINUE_SEARCH;
    } else return EXCEPTION_CONTINUE_SEARCH;

    g_eip = c->Eip;
    g_mmio_ctx = c;
    InterlockedIncrement(&g_mmio_traps);
    hot_count(c->Eip, code == EXCEPTION_ACCESS_VIOLATION ? (uint32_t)er->ExceptionInformation[1] : 0);
    int len = emulate(c);
    if (!len) {
        uint8_t *b = (uint8_t *)c->Eip;
        xlog("mmio: cannot emulate instruction at %08lx: %02x %02x %02x %02x %02x %02x %02x %02x (code %08lx)",
             c->Eip, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], code);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    c->Eip += len;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Device window: 0xFD000000 .. 0xFF000000 (GPU, APU, AC97, USB, NIC).
 * Reserved inaccessible at startup, before anything else can claim it; every
 * access then faults into mmio_veh. */
void mmio_reserve(void)
{
    void *r = VirtualAlloc((void *)MMIO_BASE, MMIO_END - MMIO_BASE, MEM_RESERVE, PAGE_NOACCESS);
    if (r != (void *)MMIO_BASE) {
        DWORD err = GetLastError();
        char occ[1024]; xmem_describe_range(MMIO_BASE, MMIO_END, occ, sizeof occ);
        xfatal("Could not reserve the Xbox device window at %08x-%08x (error %lu).\n"
               "Already in that range:%s\n\n%s", MMIO_BASE, MMIO_END, err, occ, xmem_address_space_hint());
    }
}

void mmio_init(void)
{
    AddVectoredExceptionHandler(1, mmio_veh);
    nv2a_init();
    xlog("mmio: device window %08x-%08x trapped", MMIO_BASE, MMIO_END);
}
