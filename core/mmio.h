/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

#define MMIO_BASE 0xFD000000u
#define MMIO_END  0xFF000000u

extern int g_mmio_trace;
void mmio_reserve(void);   /* claim the device window; call first thing in main */
void mmio_init(void);
void mmio_report_hot(double secs);   /* log the instructions that trap most */
int  mmio_is_device(uint32_t addr);
uint32_t mmio_current_eip(void);
uint64_t xbox_tsc(void);

/* NV2A (nv2a.c) */
void     nv2a_init(void);
uint32_t nv2a_mmio_read(uint32_t off, int size);
void     nv2a_mmio_write(uint32_t off, int size, uint32_t val);

/* MCPX APU (apu.c) */
uint32_t apu_mmio_read(uint32_t off, int size);
void     apu_mmio_write(uint32_t off, int size, uint32_t val);
uint32_t ac97_mmio_read(uint32_t off, int size);
void     ac97_mmio_write(uint32_t off, int size, uint32_t val);
