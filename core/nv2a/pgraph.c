/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PGRAPH: graphics engine method handling.
 *
 * Methods arrive from the FIFO pusher. Kelvin (class 0x97) state is kept in
 * g_kv: raw method values, the transform-constant file (fixed-function
 * matrices are stored in their hardware XF slots, so fixed-function and
 * vertex-program shaders read one constant array), light contexts, vertex
 * program memory and vertex attribute setup. Draws are collected between
 * SET_BEGIN_END(prim) and SET_BEGIN_END(END) and handed to the renderer. */
#include "kelvin.h"
#include "methods_gen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

Kelvin g_kv;
static uint32_t g_subch_class[8];
static uint64_t g_method_count;
int g_pgraph_log = 0;      /* --pgraph-log: log methods */
int g_swm_trace;
int g_swm_packed;      /* set by nv2a_hle_install for XDK > 4134 */

/* XF slots (transform constant rows used by fixed function) */
#define XF_CMAT0  0x00
#define XF_PMAT0  0x04
#define XF_MMAT0  0x08
#define XF_IMMAT0 0x0c
#define XF_EYEP   0x38
#define XF_FOG    0x39
#define XF_VPSCL  0x3a
#define XF_VPOFF  0x3b
#define XF_TG0MAT 0x40
#define XF_T0MAT  0x44
/* light context rows */
#define LTA_L0_K 0x00
#define LTA_L0_SPT 0x01
#define LTA_FR_AMB 0x11
#define LTA_CM_COL 0x13
#define LTA_FOG_K 0x15
#define LTB_L0_AMB 0x00
#define LTB_L0_DIF 0x01
#define LTB_L0_SPC 0x02
#define LTB_L0_BAMB 0x03
#define LTB_L0_BDIF 0x04
#define LTB_L0_BSPC 0x05
#define LTC1_R0 0x00

static const char *method_name(uint32_t cls, uint32_t m, char *buf)
{
    const NvMethodName *t = cls == 0x97 ? nv097_names : cls == 0x62 ? nv062_names : cls == 0x9F ? nv09f_names :
                            cls == 0x12 ? nv012_names : cls == 0x44 ? nv044_names : NULL;
    if (!t) { sprintf(buf, "cls%02x+%04x", cls, m); return buf; }
    const NvMethodName *best = NULL;
    for (; t->n; t++) if (t->m <= m && (!best || t->m > best->m)) best = t;
    if (!best) { sprintf(buf, "%04x", m); return buf; }
    if (best->m == m) return best->n;
    sprintf(buf, "%s+%x", best->n, m - best->m);
    return buf;
}

void pgraph_init(void)
{
    for (int i = 0; i < KV_ATTRS; i++) {
        g_kv.attr[i].inline_value[3] = 1.0f;
    }
}

void pgraph_bind_object(int subch, uint32_t handle, uint32_t inst, uint32_t cls)
{
    g_subch_class[subch] = cls;
    xlog("pgraph: subch %d <- object %08x (inst %05x) class %02x", subch, handle, inst, cls);
}

/* ------------------------------------------------------------------ */
/* Draw collection                                                     */
/* ------------------------------------------------------------------ */
#define GROW(arr, n, cap, add) do { if ((n) + (add) > (cap)) { (cap) = ((n) + (add)) * 2 + 64; \
    (arr) = realloc((arr), (cap) * sizeof(*(arr))); } } while (0)

static void finish_imm_vertex(void)
{
    KvDraw *d = &g_kv.draw;
    GROW(d->imm, d->nimm * KV_ATTRS * 4, d->cap_imm, KV_ATTRS * 4);
    float *v = d->imm + d->nimm * KV_ATTRS * 4;
    for (int a = 0; a < KV_ATTRS; a++) memcpy(v + a * 4, g_kv.attr[a].inline_value, 16);
    d->nimm++;
}

static void imm_touch(int attr) { g_kv.draw.imm_attr_mask |= 1u << attr; }

static void set_attr_f(int attr, int comp, uint32_t v) { imm_touch(attr); g_kv.attr[attr].inline_value[comp] = u2f(v); }

static void begin_end(uint32_t prim)
{
    KvDraw *d = &g_kv.draw;
    if (prim) {
        d->prim = prim;
        d->nranges = d->nindices = d->ninl = d->nimm = 0;
        d->imm_attr_mask = 0;
        g_kv.in_begin = 1;
    } else {
        if (g_kv.in_begin && (d->nranges || d->nindices || d->ninl || d->nimm)) r_draw(d);
        g_kv.in_begin = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Kelvin                                                              */
/* ------------------------------------------------------------------ */
static void semaphore_release(uint32_t value)
{
    r_flush();
    uint32_t off = R(NV_PGRAPH_SEMAPHOREOFFSET);
    uint32_t *p = (uint32_t *)nv_dma_ptr(g_kv.dma_semaphore, off, 4);
    static int n; if (n++ < 10) { NvDma d = nv_dma_load(g_kv.dma_semaphore);
        xlog("semaphore release %08x -> dma %05x (base %08x limit %08x) + %x = %p", value, g_kv.dma_semaphore, d.base, d.limit, off, p); }
    *p = value;
    nv2a_gpu_time_advanced();       /* wakes D3D's BlockOnTime (see nv2a.c) */
}

static void get_report(uint32_t param)
{
    /* NV097_GET_REPORT: type in bits 24-31, offset in bits 0-23.
     * Writes {timestamp(8), value(4), status(4)} to the report DMA. */
    uint32_t off = param & 0x00FFFFFF;
    uint32_t *p = (uint32_t *)nv_dma_ptr(g_kv.dma_report, off, 16);
    uint64_t t = (uint64_t)GetTickCount() * 1000000ull;
    p[0] = (uint32_t)t; p[1] = (uint32_t)(t >> 32);
    p[2] = 0x7FFFFFFF;  /* zpass pixel count: "visible" */
    p[3] = 0;
}

static void kelvin_method(uint32_t m, uint32_t v, uint32_t *rest, int nrest, int *consumed)
{
    Kelvin *k = &g_kv;
    *consumed = 1;

    /* Bulk data methods first (may consume many words) */
    if (m == NV097_INLINE_ARRAY) {
        KvDraw *d = &k->draw;
        int n = 1 + nrest;
        GROW(d->inl, d->ninl, d->cap_inl, n);
        d->inl[d->ninl++] = v;
        if (nrest) memcpy(d->inl + d->ninl, rest, nrest * 4);
        d->ninl += nrest;
        *consumed = n;
        return;
    }
    if (m == NV097_ARRAY_ELEMENT16) {
        KvDraw *d = &k->draw;
        int n = 1 + nrest;
        GROW(d->indices, d->nindices, d->cap_indices, n * 2);
        for (int i = 0; i < n; i++) {
            uint32_t w = i == 0 ? v : rest[i - 1];
            d->indices[d->nindices++] = w & 0xFFFF;
            d->indices[d->nindices++] = w >> 16;
        }
        *consumed = n;
        return;
    }
    if (m == NV097_ARRAY_ELEMENT32) {
        KvDraw *d = &k->draw;
        int n = 1 + nrest;
        GROW(d->indices, d->nindices, d->cap_indices, n);
        d->indices[d->nindices++] = v;
        if (nrest) memcpy(d->indices + d->nindices, rest, nrest * 4);
        d->nindices += nrest;
        *consumed = n;
        return;
    }

    k->regs[m >> 2] = v;

    /* PGRAPH register mirrors read back by the CPU.  D3D's software methods
     * (NO_OPERATION traps handled by its ISR) receive their parameters
     * through the clear-value registers, e.g. the address to flip to. */
    if (m == NV097_SET_ZSTENCIL_CLEAR_VALUE) { R(0x401A88) = v; return; }
    if (m == NV097_SET_COLOR_CLEAR_VALUE)    { R(0x40186C) = v; return; }

    if (m >= NV097_SET_TEXTURE_OFFSET && m < NV097_SET_TEXTURE_OFFSET + 0x100) return; /* texture state: regs only */

    /* transform constants: fixed-function matrices etc. */
    if (m >= NV097_SET_PROJECTION_MATRIX && m < NV097_SET_PROJECTION_MATRIX + 0x40) {
        int s = (m - NV097_SET_PROJECTION_MATRIX) / 4; k->c[XF_PMAT0 + s / 4][s % 4] = v; return;
    }
    if (m >= NV097_SET_MODEL_VIEW_MATRIX && m < NV097_SET_MODEL_VIEW_MATRIX + 0x100) {
        int s = (m - NV097_SET_MODEL_VIEW_MATRIX) / 4; k->c[XF_MMAT0 + (s / 16) * 8 + (s % 16) / 4][s % 4] = v; return;
    }
    if (m >= NV097_SET_INVERSE_MODEL_VIEW_MATRIX && m < NV097_SET_INVERSE_MODEL_VIEW_MATRIX + 0x100) {
        int s = (m - NV097_SET_INVERSE_MODEL_VIEW_MATRIX) / 4; k->c[XF_IMMAT0 + (s / 16) * 8 + (s % 16) / 4][s % 4] = v; return;
    }
    if (m >= NV097_SET_COMPOSITE_MATRIX && m < NV097_SET_COMPOSITE_MATRIX + 0x40) {
        int s = (m - NV097_SET_COMPOSITE_MATRIX) / 4; k->c[XF_CMAT0 + s / 4][s % 4] = v; return;
    }
    if (m >= NV097_SET_TEXTURE_MATRIX && m < NV097_SET_TEXTURE_MATRIX + 0x100) {
        int s = (m - NV097_SET_TEXTURE_MATRIX) / 4; k->c[XF_T0MAT + (s / 16) * 8 + (s % 16) / 4][s % 4] = v; return;
    }
    if (m >= NV097_SET_TEXGEN_PLANE_S && m < NV097_SET_TEXGEN_PLANE_S + 0x100) {
        int s = (m - NV097_SET_TEXGEN_PLANE_S) / 4; k->c[XF_TG0MAT + (s / 16) * 8 + (s % 16) / 4][s % 4] = v; return;
    }
    if (m >= NV097_SET_FOG_PLANE && m < NV097_SET_FOG_PLANE + 0x10) { k->c[XF_FOG][(m - NV097_SET_FOG_PLANE) / 4] = v; return; }
    if (m >= NV097_SET_VIEWPORT_OFFSET && m < NV097_SET_VIEWPORT_OFFSET + 0x10) { k->c[XF_VPOFF][(m - NV097_SET_VIEWPORT_OFFSET) / 4] = v; return; }
    if (m >= NV097_SET_VIEWPORT_SCALE && m < NV097_SET_VIEWPORT_SCALE + 0x10) { k->c[XF_VPSCL][(m - NV097_SET_VIEWPORT_SCALE) / 4] = v; return; }
    if (m >= NV097_SET_EYE_POSITION && m < NV097_SET_EYE_POSITION + 0x10) { k->c[XF_EYEP][(m - NV097_SET_EYE_POSITION) / 4] = v; return; }
    if (m >= NV097_SET_FOG_PARAMS && m < NV097_SET_FOG_PARAMS + 0xC) { k->ltctxa[LTA_FOG_K][(m - NV097_SET_FOG_PARAMS) / 4] = v; return; }
    if (m >= NV097_SET_SCENE_AMBIENT_COLOR && m < NV097_SET_SCENE_AMBIENT_COLOR + 0xC) { k->ltctxa[LTA_FR_AMB][(m - NV097_SET_SCENE_AMBIENT_COLOR) / 4] = v; return; }
    if (m >= NV097_SET_MATERIAL_EMISSION && m < NV097_SET_MATERIAL_EMISSION + 0xC) { k->ltctxa[LTA_CM_COL][(m - NV097_SET_MATERIAL_EMISSION) / 4] = v; return; }
    if (m == NV097_SET_MATERIAL_ALPHA) { k->material_alpha = u2f(v); return; }
    if (m >= NV097_SET_SPECULAR_PARAMS && m < NV097_SET_SPECULAR_PARAMS + 0x18) { k->specular_params[(m - NV097_SET_SPECULAR_PARAMS) / 4] = u2f(v); return; }
    if (m >= NV097_SET_POINT_PARAMS && m < NV097_SET_POINT_PARAMS + 0x20) { k->point_params[(m - NV097_SET_POINT_PARAMS) / 4] = u2f(v); return; }
    if (m >= NV097_SET_TEXTURE_MATRIX_ENABLE && m < NV097_SET_TEXTURE_MATRIX_ENABLE + 0x10) { k->texture_matrix_enable[(m - NV097_SET_TEXTURE_MATRIX_ENABLE) / 4] = v; return; }

    /* lights: 8 lights * 0x80 bytes from 0x1000; back lights 8 * 0x40 from 0xC00 */
    if (m >= NV097_SET_LIGHT_AMBIENT_COLOR && m < NV097_SET_LIGHT_AMBIENT_COLOR + 0x400) {
        int s = (m - NV097_SET_LIGHT_AMBIENT_COLOR) / 4, li = s / 32, part = s % 32;
        uint32_t mm = NV097_SET_LIGHT_AMBIENT_COLOR + part * 4;
        if (mm < NV097_SET_LIGHT_DIFFUSE_COLOR) k->ltctxb[LTB_L0_AMB + li * 6][(mm - NV097_SET_LIGHT_AMBIENT_COLOR) / 4] = v;
        else if (mm < NV097_SET_LIGHT_SPECULAR_COLOR) k->ltctxb[LTB_L0_DIF + li * 6][(mm - NV097_SET_LIGHT_DIFFUSE_COLOR) / 4] = v;
        else if (mm < NV097_SET_LIGHT_LOCAL_RANGE) k->ltctxb[LTB_L0_SPC + li * 6][(mm - NV097_SET_LIGHT_SPECULAR_COLOR) / 4] = v;
        else if (mm == NV097_SET_LIGHT_LOCAL_RANGE) k->ltc1[LTC1_R0 + li][0] = v;
        else if (mm < NV097_SET_LIGHT_INFINITE_DIRECTION) k->light_infinite_half_vector[li][(mm - NV097_SET_LIGHT_INFINITE_HALF_VECTOR) / 4] = u2f(v);
        else if (mm < NV097_SET_LIGHT_SPOT_FALLOFF) k->light_infinite_direction[li][(mm - NV097_SET_LIGHT_INFINITE_DIRECTION) / 4] = u2f(v);
        else if (mm < NV097_SET_LIGHT_SPOT_DIRECTION) k->ltctxa[LTA_L0_K + li * 2][(mm - NV097_SET_LIGHT_SPOT_FALLOFF) / 4] = v;
        else if (mm < NV097_SET_LIGHT_LOCAL_POSITION) k->ltctxa[LTA_L0_SPT + li * 2][(mm - NV097_SET_LIGHT_SPOT_DIRECTION) / 4] = v;
        else if (mm < NV097_SET_LIGHT_LOCAL_ATTENUATION) k->light_local_position[li][(mm - NV097_SET_LIGHT_LOCAL_POSITION) / 4] = u2f(v);
        else if (mm < NV097_SET_LIGHT_LOCAL_ATTENUATION + 0xC) k->light_local_attenuation[li][(mm - NV097_SET_LIGHT_LOCAL_ATTENUATION) / 4] = u2f(v);
        return;
    }
    if (m >= NV097_SET_BACK_LIGHT_AMBIENT_COLOR && m < NV097_SET_BACK_LIGHT_AMBIENT_COLOR + 0x200) {
        int s = (m - NV097_SET_BACK_LIGHT_AMBIENT_COLOR) / 4, li = s / 16, part = s % 16;
        if (part < 3) k->ltctxb[LTB_L0_BAMB + li * 6][part] = v;
        else if (part < 6) k->ltctxb[LTB_L0_BDIF + li * 6][part - 3] = v;
        else if (part < 9) k->ltctxb[LTB_L0_BSPC + li * 6][part - 6] = v;
        return;
    }

    /* vertex program / constants */
    if (m >= NV097_SET_TRANSFORM_PROGRAM && m < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
        int s = (m - NV097_SET_TRANSFORM_PROGRAM) / 4;
        if (k->program_load < KV_PROGRAM_LEN) k->program[k->program_load][s % 4] = v;
        if (s % 4 == 3) k->program_load++;
        k->program_dirty = 1;
        return;
    }
    if (m >= NV097_SET_TRANSFORM_CONSTANT && m < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        int s = (m - NV097_SET_TRANSFORM_CONSTANT) / 4;
        if (k->const_load < KV_CONSTANTS) k->c[k->const_load][s % 4] = v;
        if (s % 4 == 3) k->const_load++;
        return;
    }

    /* vertex attribute arrays */
    if (m >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT && m < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0x40) {
        KvAttr *a = &k->attr[(m - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
        a->type = v & 0xF; a->size = (v >> 4) & 0xF; a->stride = v >> 8;
        return;
    }
    if (m >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET && m < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 0x40) {
        k->attr[(m - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset = v;
        return;
    }

    /* immediate-mode vertex data */
    if (m >= NV097_SET_VERTEX_DATA2F_M && m < NV097_SET_VERTEX_DATA2F_M + 0x80) {
        int s = (m - NV097_SET_VERTEX_DATA2F_M) / 4, a = s / 2, part = s % 2;
        set_attr_f(a, part, v);
        if (part == 1) { k->attr[a].inline_value[2] = 0; k->attr[a].inline_value[3] = 1; if (a == 0) finish_imm_vertex(); }
        return;
    }
    if (m >= NV097_SET_VERTEX_DATA4F_M && m < NV097_SET_VERTEX_DATA4F_M + 0x100) {
        int s = (m - NV097_SET_VERTEX_DATA4F_M) / 4, a = s / 4, part = s % 4;
        set_attr_f(a, part, v);
        if (part == 3 && a == 0) finish_imm_vertex();
        return;
    }
    if (m >= NV097_SET_VERTEX_DATA2S && m < NV097_SET_VERTEX_DATA2S + 0x40) {
        int a = (m - NV097_SET_VERTEX_DATA2S) / 4; imm_touch(a);
        float *iv = k->attr[a].inline_value;
        iv[0] = (float)(int16_t)(v & 0xFFFF); iv[1] = (float)(int16_t)(v >> 16); iv[2] = 0; iv[3] = 1;
        if (a == 0) finish_imm_vertex();
        return;
    }
    if (m >= NV097_SET_VERTEX_DATA4UB && m < NV097_SET_VERTEX_DATA4UB + 0x40) {
        int a = (m - NV097_SET_VERTEX_DATA4UB) / 4; imm_touch(a);
        float *iv = k->attr[a].inline_value;
        iv[0] = (v & 0xFF) / 255.0f; iv[1] = ((v >> 8) & 0xFF) / 255.0f;
        iv[2] = ((v >> 16) & 0xFF) / 255.0f; iv[3] = (v >> 24) / 255.0f;
        if (a == 0) finish_imm_vertex();
        return;
    }
    if (m >= NV097_SET_VERTEX_DATA4S_M && m < NV097_SET_VERTEX_DATA4S_M + 0x80) {
        int s = (m - NV097_SET_VERTEX_DATA4S_M) / 4, a = s / 2, part = s % 2; imm_touch(a);
        float *iv = k->attr[a].inline_value;
        iv[part * 2] = (float)(int16_t)(v & 0xFFFF); iv[part * 2 + 1] = (float)(int16_t)(v >> 16);
        if (part == 1 && a == 0) finish_imm_vertex();
        return;
    }
    if (m >= NV097_SET_VERTEX3F && m < NV097_SET_VERTEX3F + 0xC) {
        int s = (m - NV097_SET_VERTEX3F) / 4; set_attr_f(ATTR_POSITION, s, v); k->attr[0].inline_value[3] = 1;
        if (s == 2) finish_imm_vertex();
        return;
    }
    if (m >= NV097_SET_VERTEX4F && m < NV097_SET_VERTEX4F + 0x10) {
        int s = (m - NV097_SET_VERTEX4F) / 4; set_attr_f(ATTR_POSITION, s, v);
        if (s == 3) finish_imm_vertex();
        return;
    }
    if (m >= NV097_SET_NORMAL3F && m < NV097_SET_NORMAL3F + 0xC) { set_attr_f(ATTR_NORMAL, (m - NV097_SET_NORMAL3F) / 4, v); return; }
    if (m >= NV097_SET_NORMAL3S && m < NV097_SET_NORMAL3S + 0x8) {
        int part = (m - NV097_SET_NORMAL3S) / 4; imm_touch(ATTR_NORMAL);
        float *iv = k->attr[ATTR_NORMAL].inline_value;
        iv[part * 2] = (float)(int16_t)(v & 0xFFFF) / 32767.0f; iv[part * 2 + 1] = (float)(int16_t)(v >> 16) / 32767.0f;
        return;
    }
    if (m >= NV097_SET_DIFFUSE_COLOR4F && m < NV097_SET_DIFFUSE_COLOR4F + 0x10) { set_attr_f(ATTR_DIFFUSE, (m - NV097_SET_DIFFUSE_COLOR4F) / 4, v); return; }
    if (m >= NV097_SET_DIFFUSE_COLOR3F && m < NV097_SET_DIFFUSE_COLOR3F + 0xC) { set_attr_f(ATTR_DIFFUSE, (m - NV097_SET_DIFFUSE_COLOR3F) / 4, v); k->attr[ATTR_DIFFUSE].inline_value[3] = 1; return; }
    if (m >= NV097_SET_SPECULAR_COLOR4F && m < NV097_SET_SPECULAR_COLOR4F + 0x10) { set_attr_f(ATTR_SPECULAR, (m - NV097_SET_SPECULAR_COLOR4F) / 4, v); return; }
    if (m >= NV097_SET_SPECULAR_COLOR3F && m < NV097_SET_SPECULAR_COLOR3F + 0xC) { set_attr_f(ATTR_SPECULAR, (m - NV097_SET_SPECULAR_COLOR3F) / 4, v); k->attr[ATTR_SPECULAR].inline_value[3] = 1; return; }
    if (m == NV097_SET_DIFFUSE_COLOR4UB || m == NV097_SET_SPECULAR_COLOR4UB) {
        int a = m == NV097_SET_DIFFUSE_COLOR4UB ? ATTR_DIFFUSE : ATTR_SPECULAR; imm_touch(a);
        float *iv = k->attr[a].inline_value;   /* D3DCOLOR (ARGB) */
        iv[0] = ((v >> 16) & 0xFF) / 255.0f; iv[1] = ((v >> 8) & 0xFF) / 255.0f; iv[2] = (v & 0xFF) / 255.0f; iv[3] = (v >> 24) / 255.0f;
        return;
    }
    if (m >= NV097_SET_TEXCOORD0_2F && m < NV097_SET_TEXCOORD3_4S + 0x8) {
        int set = (m - NV097_SET_TEXCOORD0_2F) / 0x28, off = (m - NV097_SET_TEXCOORD0_2F) % 0x28;
        int a = ATTR_TEXTURE0 + set; imm_touch(a);
        float *iv = k->attr[a].inline_value;
        if (off < 0x08) { iv[off / 4] = u2f(v); iv[2] = 0; iv[3] = 1; }                         /* 2F */
        else if (off < 0x10) { iv[0] = (float)(int16_t)(v & 0xFFFF); iv[1] = (float)(int16_t)(v >> 16); iv[2] = 0; iv[3] = 1; } /* 2S */
        else if (off < 0x20) iv[(off - 0x10) / 4] = u2f(v);                                      /* 4F */
        else { int p = (off - 0x20) / 4; iv[p * 2] = (float)(int16_t)(v & 0xFFFF); iv[p * 2 + 1] = (float)(int16_t)(v >> 16); } /* 4S */
        return;
    }
    if (m == NV097_SET_FOG_COORD) { imm_touch(ATTR_FOG); float f = u2f(v); for (int i = 0; i < 4; i++) k->attr[ATTR_FOG].inline_value[i] = f; return; }
    if (m >= NV097_SET_WEIGHT1F && m < NV097_SET_WEIGHT4F + 0x10) {
        imm_touch(ATTR_WEIGHT);
        float *iv = k->attr[ATTR_WEIGHT].inline_value;
        if (m == NV097_SET_WEIGHT1F) { iv[0] = u2f(v); iv[1] = iv[2] = 0; iv[3] = 1; }
        else if (m < NV097_SET_WEIGHT3F) iv[(m - NV097_SET_WEIGHT2F) / 4] = u2f(v);
        else if (m < NV097_SET_WEIGHT4F) iv[(m - NV097_SET_WEIGHT3F) / 4] = u2f(v);
        else iv[(m - NV097_SET_WEIGHT4F) / 4] = u2f(v);
        return;
    }

    switch (m) {
    case NV097_NO_OPERATION:
        /* D3D's software methods: XDK 4134 uses codes 1..9 with the parameters
         * in PGRAPH registers; later XDKs pack (parameter << 5 | code 1..14)
         * into the NOP itself. Anything else is a parse error. */
        if (g_swm_packed ? ((v & 0x1F) == 0 || (v & 0x1F) > 14) && v : v > 9) {
            static int n;
            if (n++ < 20) {
                uint32_t get = R(NV_PFIFO_CACHE1_DMA_GET), base = nv_dma_load(R(NV_PFIFO_CACHE1_DMA_INSTANCE) << 4).base;
                uint32_t *w = (uint32_t *)xmem_va(base + get - 32);
                xlog("pgraph: NOP with data %08x at get=%08x: %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x",
                     v, get, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
            }
            break;
        }
        if (v) {   /* software method: trap to the CPU (D3D's ISR handles it) */
            extern int g_swm_trace;
            if (g_swm_trace) {
                uint32_t zs = R(0x401A88), cc = R(0x40186C);
                xlog("pgraph: software method %u (zs %08x cc %08x) at get %08x", v, zs, cc, R(NV_PFIFO_CACHE1_DMA_GET));
                if ((v == 3 || v == 4) && zs >= 0x10000 && !IsBadReadPtr((void *)zs, 16)) {
                    uint32_t *d = (uint32_t *)zs;
                    xlog("   run-pushbuffer desc: fixups %08x base %08x off %08x target %08x", d[0], d[1], d[2], d[3]);
                }
            }
            R(NV_PGRAPH_TRAPPED_ADDR) = (m & 0x1FFC) | (0 << 16);
            R(NV_PGRAPH_TRAPPED_DATA_LOW) = v;
            R(NV_PGRAPH_NSOURCE) = 1;
            g_wait_nop = 1;
            R(NV_PGRAPH_INTR) |= NV_PGRAPH_INTR_ERROR;
            nv2a_update_irq();
        }
        break;
    case NV097_WAIT_FOR_IDLE: r_flush(); break;
    case NV097_SET_FLIP_READ:   R(NV_PGRAPH_SURFACE) = (R(NV_PGRAPH_SURFACE) & ~(7u << 24)) | ((v & 7) << 24); break;
    case NV097_SET_FLIP_WRITE:  R(NV_PGRAPH_SURFACE) = (R(NV_PGRAPH_SURFACE) & ~(7u << 20)) | ((v & 7) << 20); break;
    case NV097_SET_FLIP_MODULO: R(NV_PGRAPH_SURFACE) = (R(NV_PGRAPH_SURFACE) & ~(7u << 28)) | ((v & 7) << 28); break;
    case NV097_FLIP_INCREMENT_WRITE: {
        uint32_t s = R(NV_PGRAPH_SURFACE), mod = SURF_MODULO(s) ? SURF_MODULO(s) : 1;
        R(NV_PGRAPH_SURFACE) = (s & ~(7u << 20)) | (((SURF_WRITE(s) + 1) % mod) << 20);
        r_frame_marker();
        break; }
    case NV097_FLIP_STALL: r_flush(); g_wait_flip = 1; break;
    case NV097_SET_CONTEXT_DMA_NOTIFIES: k->dma_notifies = v; break;
    case NV097_SET_CONTEXT_DMA_A: k->dma_a = v; break;
    case NV097_SET_CONTEXT_DMA_B: k->dma_b = v; break;
    case NV097_SET_CONTEXT_DMA_STATE: k->dma_state = v; break;
    case NV097_SET_CONTEXT_DMA_COLOR: k->dma_color = v; r_surface_changed(); break;
    case NV097_SET_CONTEXT_DMA_ZETA: k->dma_zeta = v; r_surface_changed(); break;
    case NV097_SET_CONTEXT_DMA_VERTEX_A: k->dma_vertex_a = v; break;
    case NV097_SET_CONTEXT_DMA_VERTEX_B: k->dma_vertex_b = v; break;
    case NV097_SET_CONTEXT_DMA_SEMAPHORE: k->dma_semaphore = v; break;
    case NV097_SET_CONTEXT_DMA_REPORT: k->dma_report = v; break;
    case NV097_SET_SURFACE_CLIP_HORIZONTAL: case NV097_SET_SURFACE_CLIP_VERTICAL:
    case NV097_SET_SURFACE_FORMAT: case NV097_SET_SURFACE_PITCH:
    case NV097_SET_SURFACE_COLOR_OFFSET: case NV097_SET_SURFACE_ZETA_OFFSET:
        r_surface_changed(); break;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD: k->program_load = v; break;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD: k->const_load = v; break;
    case NV097_SET_BEGIN_END: begin_end(v); break;
    case NV097_DRAW_ARRAYS: {
        KvDraw *d = &k->draw;
        GROW(d->ranges, d->nranges * 2, d->cap_ranges, 2);
        d->ranges[d->nranges * 2] = v & 0x00FFFFFF;
        d->ranges[d->nranges * 2 + 1] = (v >> 24) + 1;
        d->nranges++;
        break; }
    case NV097_SET_SEMAPHORE_OFFSET: R(NV_PGRAPH_SEMAPHOREOFFSET) = v; break;
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: semaphore_release(v); break;
    case NV097_CLEAR_SURFACE: r_clear(v); break;
    case NV097_GET_REPORT: get_report(v); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* 2D: surfaces (0x62) + image blit (0x9F)                             */
/* ------------------------------------------------------------------ */
static uint32_t g_s2d[0x400 / 4];     /* class 62 state */
static uint32_t g_blit[0x400 / 4];    /* class 9F state */

static void blit_method(uint32_t m, uint32_t v)
{
    g_blit[(m & 0x3FF) >> 2] = v;
    if (m == 0x308) { /* NV09F_SIZE: perform */
        uint32_t in = g_blit[0x300 >> 2], out = g_blit[0x304 >> 2];
        uint32_t fmt = g_s2d[0x300 >> 2], pitch = g_s2d[0x304 >> 2];
        uint32_t bpp = (fmt == 0x01) ? 1 : (fmt == 0x04) ? 2 : 4;
        uint32_t src = nv_dma_load(g_s2d[0x184 >> 2]).base + g_s2d[0x308 >> 2];
        uint32_t dst = nv_dma_load(g_s2d[0x188 >> 2]).base + g_s2d[0x30C >> 2];
        r_blit_2d(src, dst, in & 0xFFFF, in >> 16, out & 0xFFFF, out >> 16, v & 0xFFFF, v >> 16,
                  pitch & 0xFFFF, pitch >> 16, bpp);
    }
}

void pgraph_method(int subch, uint32_t method, uint32_t data, uint32_t *rest, int nrest, int *consumed)
{
    *consumed = 1;
    uint32_t cls = g_subch_class[subch];
    g_method_count++;
    if (g_pgraph_log) {
        char b[64];
        xlog("pgraph: [%d:%02x] %-40s %08x", subch, cls, method_name(cls, method, b), data);
    }
    switch (cls) {
    case 0x97: kelvin_method(method, data, rest, nrest, consumed); break;
    case 0x62: g_s2d[(method & 0x3FF) >> 2] = data; break;
    case 0x9F: blit_method(method, data); break;
    case 0x44:
        /* NV04 pattern: its colour registers are PGRAPH registers the CPU can
         * read. Later D3D builds (5xxx) write a fence marker
         * ((put << 3 | time) << 2) to SET_MONOCHROME_COLOR0 and poll
         * PATT_COLOR0 (0x400B10) to see how far the GPU has got. */
        if (method == 0x310 || method == 0x314) { R(0x400B10 + (method - 0x310)) = data; break; }
        if (method >= 0x300 && method < 0x400) break;
        goto unhandled;
    default: unhandled: {
        static int n;
        if (n++ < 32) { char b[64]; xlog("pgraph: unhandled [%d:%02x] %s = %08x", subch, cls, method_name(cls, method, b), data); }
        break; }
    }
}
