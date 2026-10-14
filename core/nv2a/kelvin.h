/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kelvin (NV097) 3D engine state shared between PGRAPH and the renderer. */
#pragma once
#include "nv2a.h"
#include "nv097.h"

#define KV_ATTRS        16
#define KV_PROGRAM_LEN  136
#define KV_CONSTANTS    192
#define KV_TEXTURES     4

/* Xbox vertex attribute slots (fixed-function meaning) */
enum { ATTR_POSITION = 0, ATTR_WEIGHT, ATTR_NORMAL, ATTR_DIFFUSE, ATTR_SPECULAR, ATTR_FOG,
       ATTR_POINT_SIZE, ATTR_BACK_DIFFUSE, ATTR_BACK_SPECULAR, ATTR_TEXTURE0, ATTR_TEXTURE1,
       ATTR_TEXTURE2, ATTR_TEXTURE3 };

/* NV097_SET_VERTEX_DATA_ARRAY_FORMAT types */
enum { VT_UB_D3D = 0, VT_S1 = 1, VT_F = 2, VT_UB_OGL = 4, VT_S32K = 5, VT_CMP = 6 };

typedef struct {
    uint32_t type, size, stride;   /* size 0 = array disabled (use inline_value) */
    uint32_t offset;               /* bit 31 selects DMA_VERTEX_B */
    float    inline_value[4];
} KvAttr;

/* Primitive collected between SET_BEGIN_END(prim) and SET_BEGIN_END(END). */
typedef struct {
    uint32_t prim;                         /* NV097_SET_BEGIN_END_OP_* */
    /* DRAW_ARRAYS ranges */
    uint32_t *ranges; int nranges, cap_ranges;     /* pairs start,count */
    /* ARRAY_ELEMENT16/32 indices */
    uint32_t *indices; int nindices, cap_indices;
    /* INLINE_ARRAY raw words */
    uint32_t *inl; int ninl, cap_inl;
    /* immediate-mode vertices: [vertex][attr][4] */
    float    *imm; int nimm, cap_imm;
    uint32_t imm_attr_mask;                /* attributes written in immediate mode */
} KvDraw;

typedef struct {
    uint32_t regs[0x2000 / 4];             /* last value of every method (by offset/4) */
    uint32_t c[KV_CONSTANTS][4];           /* transform constants (floats as bits) */
    uint32_t ltctxa[26][4], ltctxb[52][4], ltc1[20][4];
    float    light_infinite_half_vector[8][3], light_infinite_direction[8][3];
    float    light_local_position[8][3], light_local_attenuation[8][3];
    float    specular_params[6], point_params[8], material_alpha;
    uint32_t program[KV_PROGRAM_LEN][4];
    int      program_load, const_load;
    int      program_dirty;
    uint32_t texture_matrix_enable[4];
    KvAttr   attr[KV_ATTRS];
    KvDraw   draw;
    int      in_begin;
    /* DMA object instances (RAMIN addresses) */
    uint32_t dma_notifies, dma_a, dma_b, dma_state, dma_color, dma_zeta, dma_vertex_a, dma_vertex_b,
             dma_semaphore, dma_report;
} Kelvin;

extern Kelvin g_kv;

#define KREG(m) (g_kv.regs[(m) >> 2])
static inline float kfloat(uint32_t m) { union { uint32_t u; float f; } x; x.u = KREG(m); return x.f; }
static inline float u2f(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }

/* ---- renderer entry points (gl/) ---- */
void r_clear(uint32_t flags);
void r_draw(KvDraw *d);
void r_flush(void);                        /* WAIT_FOR_IDLE / before CPU-visible effects */
void r_surface_changed(void);              /* surface format/offset/clip/dma changed */
void r_blit_2d(uint32_t src_pa, uint32_t dst_pa, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
               uint32_t w, uint32_t h, uint32_t src_pitch, uint32_t dst_pitch, uint32_t bpp);
void r_frame_marker(void);
