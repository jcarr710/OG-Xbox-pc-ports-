/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Frame capture, for renderer debugging.
 *
 * For one game frame every draw call is logged with the state that decides
 * its look and with what it did to the render target (pixels changed, mean
 * brightness change, bounding box). The render target after each draw that
 * changes a noticeable part of it, and the textures that draw samples, are
 * written as BMPs (capNNN_*.bmp) next to the log (capture.txt).
 *
 * Armed by "capture" in the control file (--control) or by
 * <STEM>_CAPTURE_AT=seconds; it starts at the next flip. */
#include "gl.h"
#include "../title.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static volatile int g_cap_req;
static int g_cap_on, g_cap_draw, g_cap_dumps, g_cap_texdumps;
static uint8_t *g_before, *g_after; static size_t g_cap_bytes;
static FILE *g_cap;
static uint32_t g_dumped_tex[256]; static int g_ndumped_tex;

void capture_arm(void) { g_cap_req = 1; }
int capture_active(void) { return g_cap_on; }

static void cap(const char *fmt, ...)
{
    if (!g_cap) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_cap, fmt, ap); va_end(ap);
}

void capture_note(const char *fmt, ...)
{
    if (!g_cap_on || !g_cap) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_cap, fmt, ap); va_end(ap);
    fputc('\n', g_cap);
}

static void write_bmp(const char *name, const uint8_t *rgba, int w, int h)
{
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\%s", g_screenshot_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = { 0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + w * h * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
    BITMAPINFOHEADER ih = { sizeof ih, w, -h, 1, 32, BI_RGB, w * h * 4, 2835, 2835, 0, 0 };
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    uint8_t *row = malloc(w * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = s[x * 4 + 3]; }
        fwrite(row, 1, w * 4, f);
    }
    free(row); fclose(f);
}

static int rt_size(int *w, int *h)
{
    if (!g_rt.color) return 0;
    *w = (int)(g_rt.color->w * g_scale); *h = (int)(g_rt.color->h * g_scale);
    size_t n = (size_t)*w * *h * 4;
    if (n > g_cap_bytes) { g_cap_bytes = n; g_before = realloc(g_before, n); g_after = realloc(g_after, n); }
    return 1;
}
static void read_rt(uint8_t *dst, int w, int h)
{
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
}

void capture_frame_marker(void)
{
    if (g_cap_on) {
        cap("---- end of frame %llu: %d draws, %d render-target dumps ----\n", (unsigned long long)g_frame_no, g_cap_draw, g_cap_dumps);
        fclose(g_cap); g_cap = NULL; g_cap_on = 0;
        xlog("capture: done (%d draws)", g_cap_draw);
        return;
    }
    static int at = -2; static DWORD t0;
    if (at == -2) { const char *e = title_env("CAPTURE_AT"); at = e ? atoi(e) : -1; t0 = GetTickCount(); }
    if (at > 0 && (GetTickCount() - t0) / 1000 >= (DWORD)at) { g_cap_req = 1; at = -1; }
    if (!g_cap_req) return;
    g_cap_req = 0;
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\capture.txt", g_screenshot_dir);
    g_cap = fopen(path, "w");
    if (!g_cap) return;
    g_cap_on = 1; g_cap_draw = 0; g_cap_dumps = 0; g_cap_texdumps = 0; g_ndumped_tex = 0;
    xlog("capture: frame %llu -> %s", (unsigned long long)g_frame_no, path);
    cap("capture of frame %llu (scale %d)\n", (unsigned long long)g_frame_no, g_scale);
}

void capture_before_draw(void)
{
    int w, h;
    if (!g_cap_on || !rt_size(&w, &h)) return;
    read_rt(g_before, w, h);
}

static const char *blend_name(uint32_t f)
{
    switch (f) {
    case 0: return "0"; case 1: return "1";
    case 0x300: return "Sc"; case 0x301: return "1-Sc"; case 0x302: return "Sa"; case 0x303: return "1-Sa";
    case 0x304: return "Da"; case 0x305: return "1-Da"; case 0x306: return "Dc"; case 0x307: return "1-Dc"; case 0x308: return "SaSat";
    case 0x8001: return "Kc"; case 0x8002: return "1-Kc"; case 0x8003: return "Ka"; case 0x8004: return "1-Ka";
    default: { static char b[16]; snprintf(b, sizeof b, "%x", f); return b; }
    }
}

static void dump_texture(int stage, const BoundTex *t, uint32_t pa, uint32_t fmt)
{
    if (!t->tex || t->target != GL_TEXTURE_2D || g_cap_texdumps >= 60) return;
    for (int i = 0; i < g_ndumped_tex; i++) if (g_dumped_tex[i] == (pa ^ fmt)) return;
    if (g_ndumped_tex < 256) g_dumped_tex[g_ndumped_tex++] = pa ^ fmt;
    GLint w = 0, h = 0;
    glActiveTexture(GL_TEXTURE0 + stage);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    uint8_t *px = malloc((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char name[64]; snprintf(name, sizeof name, "cap_tex_%08x_%02x.bmp", pa, (fmt >> 8) & 0xFF);
    write_bmp(name, px, w, h);
    /* alpha as grey, for formats where it matters */
    for (int i = 0; i < w * h; i++) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4 + 3], px[i * 4 + 3] = 255;
    snprintf(name, sizeof name, "cap_tex_%08x_%02x_alpha.bmp", pa, (fmt >> 8) & 0xFF);
    write_bmp(name, px, w, h);
    free(px);
    g_cap_texdumps++;
    cap("    dumped texture stage %d -> cap_tex_%08x_%02x.bmp (%dx%d)\n", stage, pa, (fmt >> 8) & 0xFF, w, h);
}

static void log_vertices(const KvDraw *d)
{
    uint32_t first = 0;
    if (d->nindices) first = d->indices[0];
    else if (d->nranges) first = d->ranges[0];
    else return;                                   /* inline/immediate: not decoded here */
    for (int i = 0; i < KV_ATTRS; i++) {
        const KvAttr *a = &g_kv.attr[i];
        if (!a->size) continue;
        uint32_t dma = (a->offset & 0x80000000) ? g_kv.dma_vertex_b : g_kv.dma_vertex_a;
        const uint8_t *src = (const uint8_t *)xmem_va(nv_dma_load(dma).base + (a->offset & 0x7FFFFFFF));
        cap("    attr %2d type %u size %u stride %u:", i, a->type, a->size, a->stride);
        for (uint32_t v = 0; v < 3; v++) {
            uint32_t idx = d->nindices > (int)v ? d->indices[v] : first + v;
            const uint8_t *p = src + (size_t)idx * a->stride;
            cap(" (");
            for (uint32_t c = 0; c < a->size && c < 4; c++) {
                if (a->type == VT_F) cap("%s%.4g", c ? " " : "", ((const float *)p)[c]);
                else if (a->type == VT_S1 || a->type == VT_S32K) cap("%s%d", c ? " " : "", ((const int16_t *)p)[c]);
                else if (a->type == VT_UB_D3D || a->type == VT_UB_OGL) cap("%s%02x", c ? " " : "", p[c]);
                else cap("%s%08x", c ? " " : "", ((const uint32_t *)p)[0]);
            }
            cap(")");
        }
        cap("\n");
    }
}

void capture_after_draw(const KvDraw *d, const BoundTex tex[4], int vp)
{
    int w, h;
    if (!g_cap_on || !rt_size(&w, &h)) return;
    read_rt(g_after, w, h);
    int n = g_cap_draw++;
    /* what changed */
    long changed = 0, achanged = 0; double dl = 0, da = 0; int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) {
        const uint8_t *b = g_before + (size_t)y * w * 4, *a = g_after + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, a += 4, b += 4) {
            int d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2], d3 = a[3] - b[3];
            int c = abs(d0) > 3 || abs(d1) > 3 || abs(d2) > 3, ca = abs(d3) > 3;
            if (c) { changed++; dl += (d0 * 0.3 + d1 * 0.59 + d2 * 0.11); }
            if (ca) { achanged++; da += d3; }
            if (c || ca) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        }
    }
    double frac = (double)changed / ((double)w * h), afrac = (double)achanged / ((double)w * h);
    uint32_t verts = d->nindices ? (uint32_t)d->nindices : 0;
    if (!verts) for (int r = 0; r < d->nranges; r++) verts += d->ranges[r * 2 + 1];
    if (d->ninl || d->nimm) verts = d->nimm ? (uint32_t)d->nimm : 0;
    cap("#%d rt %08x %ux%u z %08x prim %u verts %u%s | changed %.2f%% dlum %+.1f alpha %.2f%% %+.1f box %d,%d-%d,%d\n", n, g_rt.color->pa,
        g_rt.color->w, g_rt.color->h, g_rt.zeta ? g_rt.zeta->pa : 0, d->prim, verts, d->ninl ? " (inline)" : d->nimm ? " (immediate)" : "",
        100.0 * frac, changed ? dl / changed : 0.0, 100.0 * afrac, achanged ? da / achanged : 0.0, x0, y0, x1, y1);
    /* state */
    uint32_t be = KREG(NV097_SET_BLEND_ENABLE) & 1;
    cap("    blend %s", be ? "on" : "off");
    if (be) cap(" %s,%s eq %x", blend_name(KREG(NV097_SET_BLEND_FUNC_SFACTOR)), blend_name(KREG(NV097_SET_BLEND_FUNC_DFACTOR)), KREG(NV097_SET_BLEND_EQUATION));
    cap(" | z %s func %x mask %u | stencil %u | cull %u | alphatest %u func %x ref %u | cmask %08x | fog %u | %s\n",
        (KREG(NV097_SET_DEPTH_TEST_ENABLE) & 1) && g_rt.zeta ? "on" : "off", KREG(NV097_SET_DEPTH_FUNC), KREG(NV097_SET_DEPTH_MASK) & 1,
        KREG(NV097_SET_STENCIL_TEST_ENABLE) & 1, KREG(NV097_SET_CULL_FACE_ENABLE) & 1, KREG(NV097_SET_ALPHA_TEST_ENABLE) & 1,
        KREG(NV097_SET_ALPHA_FUNC), KREG(NV097_SET_ALPHA_REF), KREG(NV097_SET_COLOR_MASK), KREG(NV097_SET_FOG_ENABLE) & 1,
        vp ? "vertex program" : "fixed function");
    if (!vp) {
        cap("    ff: lighting %u texgen", KREG(NV097_SET_LIGHTING_ENABLE) & 1);
        for (int i = 0; i < 4; i++) cap(" [%x %x %x %x]", KREG(NV097_SET_TEXGEN_S + i * 16), KREG(NV097_SET_TEXGEN_T + i * 16),
                                        KREG(NV097_SET_TEXGEN_R + i * 16), KREG(NV097_SET_TEXGEN_Q + i * 16));
        cap(" texmat %u%u%u%u\n", g_kv.texture_matrix_enable[0], g_kv.texture_matrix_enable[1], g_kv.texture_matrix_enable[2], g_kv.texture_matrix_enable[3]);
    } else cap("    vp start %u\n", KREG(NV097_SET_TRANSFORM_PROGRAM_START));
    uint32_t prog = KREG(NV097_SET_SHADER_STAGE_PROGRAM);
    for (int i = 0; i < 4; i++) {
        uint32_t mode = (prog >> (i * 5)) & 0x1F;
        if (!mode) continue;
        uint32_t base = NV097_SET_TEXTURE_OFFSET + i * 0x40;
        uint32_t fmt = KREG(base + 4), ctl0 = KREG(base + 0xC);
        uint32_t dma = (fmt & 3) == 2 ? g_kv.dma_b : g_kv.dma_a;
        uint32_t pa = (nv_dma_load(dma).base + KREG(base)) & (XBOX_RAM_SIZE - 1);
        Surface *s = surface_find_texture(pa, 1, 1);
        cap("    tex%d mode %u %s fmt %02x %ux%u lv %u pa %08x addr %06x filt %08x ctl0 %08x rect %08x%s%s\n", i, mode,
            (ctl0 & 0x40000000) ? "on" : "OFF", (fmt >> 8) & 0xFF, 1u << ((fmt >> 20) & 0xF), 1u << ((fmt >> 24) & 0xF), (fmt >> 16) & 0xF,
            pa, KREG(base + 8), KREG(base + 0x14), ctl0, KREG(base + 0x1C), s && s->pa == pa ? " [render target]" : "",
            tex[i].tex ? "" : " [not bound]");
    }
    uint32_t cc = KREG(NV097_SET_COMBINER_CONTROL);
    cap("    combiners %u ctl %08x final %08x %08x | stages:", cc & 0xFF, cc, KREG(NV097_SET_COMBINER_SPECULAR_FOG_CW0), KREG(NV097_SET_COMBINER_SPECULAR_FOG_CW1));
    for (uint32_t i = 0; i < (cc & 0xFF) && i < 8; i++)
        cap(" [c %08x>%08x a %08x>%08x k %08x %08x]", KREG(NV097_SET_COMBINER_COLOR_ICW + i * 4), KREG(NV097_SET_COMBINER_COLOR_OCW + i * 4),
            KREG(NV097_SET_COMBINER_ALPHA_ICW + i * 4), KREG(NV097_SET_COMBINER_ALPHA_OCW + i * 4),
            KREG(NV097_SET_COMBINER_FACTOR0 + i * 4), KREG(NV097_SET_COMBINER_FACTOR1 + i * 4));
    cap("\n");
    /* the big ones: vertices, textures, and the render target afterwards */
    if ((frac > 0.01 || afrac > 0.01) && g_cap_dumps < 100) {
        log_vertices(d);
        for (int i = 0; i < 4; i++) {
            uint32_t mode = (prog >> (i * 5)) & 0x1F;
            if (!mode || !tex[i].tex) continue;
            uint32_t base = NV097_SET_TEXTURE_OFFSET + i * 0x40, fmt = KREG(base + 4);
            uint32_t dma = (fmt & 3) == 2 ? g_kv.dma_b : g_kv.dma_a;
            dump_texture(i, &tex[i], (nv_dma_load(dma).base + KREG(base)) & (XBOX_RAM_SIZE - 1), fmt);
        }
        glActiveTexture(GL_TEXTURE0);
        char name[64]; snprintf(name, sizeof name, "cap%04d_rt%08x.bmp", n, g_rt.color->pa);
        write_bmp(name, g_after, w, h);
        if (afrac > 0.01) {          /* the alpha channel as grey */
            for (size_t i = 0; i < (size_t)w * h; i++) g_before[i * 4] = g_before[i * 4 + 1] = g_before[i * 4 + 2] = g_after[i * 4 + 3], g_before[i * 4 + 3] = 255;
            snprintf(name, sizeof name, "cap%04d_rt%08x_alpha.bmp", n, g_rt.color->pa);
            write_bmp(name, g_before, w, h);
        }
        g_cap_dumps++;
    }
}
