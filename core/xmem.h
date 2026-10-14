/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

#define XBOX_RAM_SIZE   0x04000000u   /* 64 MB retail */
#define XBOX_RAM_PAGES  (XBOX_RAM_SIZE >> 12)
#define XBOX_RAM_VA     0x80000000u
#define XBOX_RAM_WC_VA  0xF0000000u
#define XMEM_BAD_PA     0xFFFFFFFFu

extern uint8_t *g_ram;

void     xmem_init(void);      /* maps RAM at its fixed addresses; call first thing in main */
void     xmem_describe_range(uint32_t lo, uint32_t hi, char *out, unsigned n);  /* what occupies [lo,hi) */
const char *xmem_address_space_hint(void);
void    *xmem_alloc_contiguous(uint32_t size, uint32_t lo, uint32_t hi, uint32_t align);
void     xmem_free(void *p);
uint32_t xmem_size(void *p);
uint32_t xmem_free_pages(void);   /* unallocated 4 KB pages of the 64 MB */
int      xmem_is_ram(const void *p);
uint32_t xmem_pa(const void *p);
static inline void *xmem_va(uint32_t pa) { return (void *)(uintptr_t)(XBOX_RAM_VA + (pa & (XBOX_RAM_SIZE - 1))); }
