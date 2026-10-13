/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Draw calls: vertex fetch, raster state, uniforms. */
#include "gl.h"
#include <string.h>
#include <stdlib.h>

static GLuint g_vao, g_vbo, g_ibo;
static uint8_t *g_buf; static size_t g_buf_cap;
static uint32_t *g_idx; static size_t g_idx_cap;
uint32_t g_stat_draws, g_stat_verts;

void draw_init(void)
{
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glGenBuffers(1, &g_ibo);
}

static void *buf_reserve(size_t n) { if (n > g_buf_cap) { g_buf_cap = n * 2; g_buf = realloc(g_buf, g_buf_cap); } return g_buf; }
static uint32_t *idx_reserve(size_t n) { if (n > g_idx_cap) { g_idx_cap = n * 2; g_idx = realloc(g_idx, g_idx_cap * 4); } return g_idx; }

static int comp_bytes(uint32_t t) { return (t == VT_UB_D3D || t == VT_UB_OGL) ? 1 : (t == VT_S1 || t == VT_S32K) ? 2 : 4; }

/* Point GL attribute i at data (already in the VBO at `off`) with Xbox format a. */
static void set_attrib(int i, const KvAttr *a, size_t off, int stride)
{
    GLint n = a->size; GLenum type = GL_FLOAT; GLboolean norm = GL_FALSE;
    switch (a->type) {
    case VT_UB_D3D: type = GL_UNSIGNED_BYTE; norm = GL_TRUE; if (n == 4) n = GL_BGRA; break;
    case VT_UB_OGL: type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
    case VT_S1:     type = GL_SHORT; norm = GL_TRUE; break;
    case VT_S32K:   type = GL_SHORT; norm = GL_FALSE; break;
    case VT_CMP:    type = GL_FLOAT; n = 3; break;     /* converted to float3 by the caller */
    default:        type = GL_FLOAT; break;
    }
    glEnableVertexAttribArray(i);
    glVertexAttribPointer(i, n, type, norm, stride, (void *)off);
}

static void decode_cmp(uint32_t v, float *o)
{
    int x = (int)(v << 21) >> 21, y = (int)(v << 10) >> 21, z = (int)v >> 22;
    o[0] = x / 1023.0f; o[1] = y / 1023.0f; o[2] = z / 511.0f;
}

static const uint8_t *attr_src(const KvAttr *a)
{
    uint32_t dma = (a->offset & 0x80000000) ? g_kv.dma_vertex_b : g_kv.dma_vertex_a;
    return (const uint8_t *)xmem_va(nv_dma_load(dma).base + (a->offset & 0x7FFFFFFF));
}

/* Upload attribute arrays for vertices [vmin, vmax]; returns 0 on failure. */
static int bind_arrays(uint32_t vmin, uint32_t vmax)
{
    uint32_t n = vmax - vmin + 1;
    size_t total = 0;
    for (int i = 0; i < KV_ATTRS; i++) {
        const KvAttr *a = &g_kv.attr[i];
        if (!a->size) continue;
        total += (a->type == VT_CMP ? 12 : (a->stride ? a->stride : 16)) * (size_t)n + 16;
    }
    uint8_t *dst = buf_reserve(total + 16);
    size_t off = 0;
    struct { int i; size_t off; int stride; } binds[KV_ATTRS]; int nb = 0;
    for (int i = 0; i < KV_ATTRS; i++) {
        KvAttr *a = &g_kv.attr[i];
        if (!a->size) { glDisableVertexAttribArray(i); glVertexAttrib4fv(i, a->inline_value); continue; }
        const uint8_t *src = attr_src(a);
        if (!a->stride) {   /* stride 0: first element is a constant */
            float v[4] = { 0, 0, 0, 1 };
            for (uint32_t c = 0; c < a->size && c < 4; c++) {
                if (a->type == VT_F) v[c] = ((const float *)src)[c];
                else if (a->type == VT_S1) v[c] = ((const int16_t *)src)[c] / 32767.0f;
                else if (a->type == VT_S32K) v[c] = ((const int16_t *)src)[c];
                else if (a->type == VT_UB_OGL) v[c] = src[c] / 255.0f;
            }
            if (a->type == VT_UB_D3D) { v[0] = src[2] / 255.0f; v[1] = src[1] / 255.0f; v[2] = src[0] / 255.0f; v[3] = src[3] / 255.0f; }
            glDisableVertexAttribArray(i); glVertexAttrib4fv(i, v);
            continue;
        }
        if (a->type == VT_CMP) {
            float *o = (float *)(dst + off);
            for (uint32_t k = 0; k < n; k++) decode_cmp(*(const uint32_t *)(src + (vmin + k) * a->stride), o + k * 3);
            binds[nb].i = i; binds[nb].off = off; binds[nb].stride = 12; nb++;
            off += n * 12;
        } else {
            memcpy(dst + off, src + (size_t)vmin * a->stride, (size_t)n * a->stride);
            binds[nb].i = i; binds[nb].off = off; binds[nb].stride = a->stride; nb++;
            off += (size_t)n * a->stride;
        }
        off = (off + 15) & ~(size_t)15;
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, off ? off : 16, dst, GL_STREAM_DRAW);
    for (int b = 0; b < nb; b++) set_attrib(binds[b].i, &g_kv.attr[binds[b].i], binds[b].off, binds[b].stride);
    return 1;
}

/* INLINE_ARRAY: attributes packed per vertex in slot order. Returns vertex count. */
static uint32_t bind_inline(const KvDraw *d)
{
    uint32_t offs[KV_ATTRS], vsize = 0;
    for (int i = 0; i < KV_ATTRS; i++) {
        const KvAttr *a = &g_kv.attr[i];
        if (!a->size) continue;
        int cb = comp_bytes(a->type);
        vsize = (vsize + cb - 1) / cb * cb;
        offs[i] = vsize;
        vsize += cb * (a->type == VT_CMP ? 1 : a->size);
        vsize = (vsize + cb - 1) / cb * cb;
    }
    if (!vsize) return 0;
    uint32_t nverts = d->ninl * 4 / vsize;
    int has_cmp = 0;
    for (int i = 0; i < KV_ATTRS; i++) if (g_kv.attr[i].size && g_kv.attr[i].type == VT_CMP) has_cmp = 1;
    const uint8_t *raw = (const uint8_t *)d->inl;
    uint8_t *dst = buf_reserve((size_t)nverts * (vsize + (has_cmp ? 12 * 4 : 0)) + 64);
    memcpy(dst, raw, (size_t)nverts * vsize);
    size_t cmp_off = ((size_t)nverts * vsize + 15) & ~(size_t)15;
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    for (int i = 0; i < KV_ATTRS; i++) {
        const KvAttr *a = &g_kv.attr[i];
        if (!a->size) { glDisableVertexAttribArray(i); glVertexAttrib4fv(i, a->inline_value); continue; }
        if (a->type == VT_CMP) {
            float *o = (float *)(dst + cmp_off);
            for (uint32_t k = 0; k < nverts; k++) decode_cmp(*(const uint32_t *)(raw + k * vsize + offs[i]), o + k * 3);
            set_attrib(i, a, cmp_off, 12);
            cmp_off += nverts * 12;
        } else set_attrib(i, a, offs[i], vsize);
    }
    glBufferData(GL_ARRAY_BUFFER, cmp_off + 16, dst, GL_STREAM_DRAW);
    return nverts;
}

static uint32_t bind_immediate(const KvDraw *d)
{
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)d->nimm * KV_ATTRS * 16, d->imm, GL_STREAM_DRAW);
    for (int i = 0; i < KV_ATTRS; i++) {
        if (d->imm_attr_mask & (1u << i)) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, KV_ATTRS * 16, (void *)(size_t)(i * 16));
        } else { glDisableVertexAttribArray(i); glVertexAttrib4fv(i, g_kv.attr[i].inline_value); }
    }
    return d->nimm;
}

/* ---------------- raster state ---------------- */
static void apply_state(void)
{
    /* blending */
    if (KREG(NV097_SET_BLEND_ENABLE) & 1) {
        glEnable(GL_BLEND);
        uint32_t sf = KREG(NV097_SET_BLEND_FUNC_SFACTOR), df = KREG(NV097_SET_BLEND_FUNC_DFACTOR);
        glBlendFuncSeparate(sf, df, sf, df);
        uint32_t eq = KREG(NV097_SET_BLEND_EQUATION);
        if (eq == 0xF005) eq = GL_FUNC_REVERSE_SUBTRACT; else if (eq == 0xF006) eq = GL_FUNC_ADD;
        if (!eq) eq = GL_FUNC_ADD;
        glBlendEquation(eq);
        uint32_t bc = KREG(NV097_SET_BLEND_COLOR);
        glBlendColor(((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f, (bc & 255) / 255.0f, (bc >> 24) / 255.0f);
    } else glDisable(GL_BLEND);
    /* color mask (no alpha writes to surfaces without alpha: dst alpha reads 1) */
    uint32_t cm = KREG(NV097_SET_COLOR_MASK);
    int has_alpha = g_rt.color && g_rt.color->has_alpha;
    glColorMask((cm >> 16) & 1, (cm >> 8) & 1, cm & 1, has_alpha ? (cm >> 24) & 1 : 0);
    /* depth */
    if (g_rt.zeta && (KREG(NV097_SET_DEPTH_TEST_ENABLE) & 1)) {
        glEnable(GL_DEPTH_TEST);
        uint32_t f = KREG(NV097_SET_DEPTH_FUNC); glDepthFunc(f ? f : GL_LESS);
    } else glDisable(GL_DEPTH_TEST);
    glDepthMask((KREG(NV097_SET_DEPTH_MASK) & 1) && g_rt.zeta ? GL_TRUE : GL_FALSE);
    /* stencil */
    if (g_rt.zeta && (KREG(NV097_SET_STENCIL_TEST_ENABLE) & 1)) {
        glEnable(GL_STENCIL_TEST);
        uint32_t fn = KREG(NV097_SET_STENCIL_FUNC); if (!fn) fn = GL_ALWAYS;
        glStencilFuncSeparate(GL_FRONT_AND_BACK, fn, KREG(NV097_SET_STENCIL_FUNC_REF) & 0xFF, KREG(NV097_SET_STENCIL_FUNC_MASK) & 0xFF);
        uint32_t o1 = KREG(NV097_SET_STENCIL_OP_FAIL), o2 = KREG(NV097_SET_STENCIL_OP_ZFAIL), o3 = KREG(NV097_SET_STENCIL_OP_ZPASS);
        glStencilOpSeparate(GL_FRONT_AND_BACK, o1 ? o1 : GL_KEEP, o2 ? o2 : GL_KEEP, o3 ? o3 : GL_KEEP);
        glStencilMask(KREG(NV097_SET_STENCIL_MASK) & 0xFF);
    } else glDisable(GL_STENCIL_TEST);
    /* culling: GL sees the image vertically flipped, so winding is mirrored */
    if (KREG(NV097_SET_CULL_FACE_ENABLE) & 1) {
        glEnable(GL_CULL_FACE);
        uint32_t cf = KREG(NV097_SET_CULL_FACE); glCullFace(cf ? cf : GL_BACK);
        glFrontFace(KREG(NV097_SET_FRONT_FACE) == 0x900 ? GL_CCW : GL_CW);
    } else glDisable(GL_CULL_FACE);
    /* polygon mode / offset */
    uint32_t pm = KREG(NV097_SET_FRONT_POLYGON_MODE);
    glPolygonMode(GL_FRONT_AND_BACK, pm == 0x1B00 ? GL_POINT : pm == 0x1B01 ? GL_LINE : GL_FILL);
    if (KREG(NV097_SET_POLY_OFFSET_FILL_ENABLE) & 1) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(kfloat(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR), kfloat(NV097_SET_POLYGON_OFFSET_BIAS));
    } else glDisable(GL_POLYGON_OFFSET_FILL);
    /* window clip -> scissor */
    uint32_t wh = KREG(NV097_SET_WINDOW_CLIP_HORIZONTAL), wv = KREG(NV097_SET_WINDOW_CLIP_VERTICAL);
    int x0 = wh & 0xFFF, x1 = ((wh >> 16) & 0xFFF) + 1, y0 = wv & 0xFFF, y1 = ((wv >> 16) & 0xFFF) + 1;
    if (KREG(NV097_SET_WINDOW_CLIP_TYPE) == 0 && x1 > x0 && y1 > y0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0 * g_scale, y0 * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
    } else glDisable(GL_SCISSOR_TEST);
}

static void set_uniforms(ShaderProg *p, const BoundTex tex[4])
{
    static float cf[KV_CONSTANTS * 4];
    memcpy(cf, g_kv.c, sizeof cf);
    glUniform4fv(p->u_c, KV_CONSTANTS, cf);
    glUniform2f(p->u_surface, (float)g_rt.w, (float)g_rt.h);
    float zmax = kfloat(NV097_SET_CLIP_MAX);
    if (zmax <= 0.0f) zmax = (g_rt.zeta && (g_rt.zeta->fmt & 0xF) == 1) ? 65535.0f : 16777215.0f;
    glUniform2f(p->u_clip, kfloat(NV097_SET_CLIP_MIN), zmax);
    /* combiner constants: per-stage factor0/factor1, then final combiner pair */
    float k[18 * 4];
    for (int i = 0; i < 9; i++) {
        uint32_t f0 = i < 8 ? KREG(NV097_SET_COMBINER_FACTOR0 + i * 4) : KREG(NV097_SET_SPECULAR_FOG_FACTOR);
        uint32_t f1 = i < 8 ? KREG(NV097_SET_COMBINER_FACTOR1 + i * 4) : KREG(NV097_SET_SPECULAR_FOG_FACTOR + 4);
        uint32_t f[2] = { f0, f1 };
        for (int j = 0; j < 2; j++) {
            float *o = k + (i * 2 + j) * 4;
            o[0] = ((f[j] >> 16) & 255) / 255.0f; o[1] = ((f[j] >> 8) & 255) / 255.0f; o[2] = (f[j] & 255) / 255.0f; o[3] = (f[j] >> 24) / 255.0f;
        }
    }
    glUniform4fv(p->u_consts, 18, k);
    uint32_t fc = KREG(NV097_SET_FOG_COLOR);
    glUniform4f(p->u_fogcolor, (fc & 255) / 255.0f, ((fc >> 8) & 255) / 255.0f, ((fc >> 16) & 255) / 255.0f, (fc >> 24) / 255.0f);
    glUniform1f(p->u_alpharef, (float)(KREG(NV097_SET_ALPHA_REF) & 0xFF));
    float ts[16] = { 0 }, bm[16] = { 0 }, bso[16] = { 0 };
    for (int i = 0; i < 4; i++) {
        ts[i * 4] = tex[i].width ? tex[i].width : 1; ts[i * 4 + 1] = tex[i].height ? tex[i].height : 1;
        uint32_t base = NV097_SET_TEXTURE_OFFSET + i * 0x40;
        for (int j = 0; j < 4; j++) bm[i * 4 + j] = kfloat(base + 0x28 + j * 4);
        bso[i * 4] = kfloat(base + 0x38); bso[i * 4 + 1] = kfloat(base + 0x3C);
    }
    glUniform4fv(p->u_texscale, 4, ts);
    glUniform4fv(p->u_bumpmat, 4, bm);
    glUniform4fv(p->u_bumpscale, 4, bso);
    if (p->u_fogparam >= 0) glUniform2f(p->u_fogparam, u2f(g_kv.ltctxa[0x15][0]), u2f(g_kv.ltctxa[0x15][1]));
    if (p->u_ltctxa >= 0) {
        static float a[26 * 4], b[52 * 4], c1[20 * 4];
        memcpy(a, g_kv.ltctxa, sizeof a); memcpy(b, g_kv.ltctxb, sizeof b); memcpy(c1, g_kv.ltc1, sizeof c1);
        glUniform4fv(p->u_ltctxa, 26, a); glUniform4fv(p->u_ltctxb, 52, b); glUniform4fv(p->u_ltc1, 20, c1);
        glUniform3fv(p->u_lhalf, 8, &g_kv.light_infinite_half_vector[0][0]);
        glUniform3fv(p->u_ldir, 8, &g_kv.light_infinite_direction[0][0]);
        glUniform3fv(p->u_lpos, 8, &g_kv.light_local_position[0][0]);
        glUniform3fv(p->u_latt, 8, &g_kv.light_local_attenuation[0][0]);
        glUniform1f(p->u_material_alpha, g_kv.material_alpha);
        glUniform1f(p->u_specpow, 16.0f);
    }
}

/* ---------------- primitive assembly ---------------- */
static GLenum gl_prim(uint32_t p)
{
    switch (p) {
    case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_LOOP; case 4: return GL_LINE_STRIP;
    case 5: return GL_TRIANGLES; case 6: return GL_TRIANGLE_STRIP; case 7: return GL_TRIANGLE_FAN;
    case 8: return GL_TRIANGLES;          /* quads -> converted */
    case 9: return GL_TRIANGLE_STRIP;     /* quad strip == triangle strip order */
    case 10: return GL_TRIANGLE_FAN;      /* convex polygon */
    default: return GL_TRIANGLES;
    }
}

static void draw_indexed(uint32_t prim, uint32_t *idx, uint32_t n)
{
    if (prim == 8) {   /* QUADS -> 2 triangles each */
        uint32_t q = n / 4;
        uint32_t *t = malloc(q * 6 * 4);
        for (uint32_t i = 0; i < q; i++) {
            uint32_t *s = idx + i * 4, *o = t + i * 6;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[0]; o[4] = s[2]; o[5] = s[3];
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, q * 6 * 4, t, GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLES, q * 6, GL_UNSIGNED_INT, 0);
        free(t);
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * 4, idx, GL_STREAM_DRAW);
        glDrawElements(gl_prim(prim), n, GL_UNSIGNED_INT, 0);
    }
    g_stat_verts += n;
}

static void r_draw_impl(KvDraw *d);
void r_draw(KvDraw *d)
{
    static int nodraw = -1;
    if (nodraw < 0) nodraw = getenv("JSRF_NODRAW") != NULL;
    if (nodraw) return;
    uint64_t t = perf_ticks();
    r_draw_impl(d);
    g_perf.draw += perf_ticks() - t;
    g_perf.draws++;
}

static void r_draw_impl(KvDraw *d)
{
    surface_bind_current();
    BoundTex tex[4];
    uint32_t prog = KREG(NV097_SET_SHADER_STAGE_PROGRAM);
    for (int i = 0; i < 4; i++) {
        memset(&tex[i], 0, sizeof tex[i]);
        if ((prog >> (i * 5)) & 0x1F) texture_bind_stage(i, &tex[i]);
    }
    ShaderProg *p = shaders_get(tex, 0);
    if (!p) return;
    glUseProgram(p->prog);
    set_uniforms(p, tex);
    apply_state();
    glBindVertexArray(g_vao);
    g_stat_draws++;

    if (d->ninl) {
        uint32_t n = bind_inline(d);
        if (!n) return;
        uint32_t *idx = idx_reserve(n);
        for (uint32_t i = 0; i < n; i++) idx[i] = i;
        draw_indexed(d->prim, idx, n);
    } else if (d->nimm) {
        uint32_t n = bind_immediate(d);
        uint32_t *idx = idx_reserve(n);
        for (uint32_t i = 0; i < n; i++) idx[i] = i;
        draw_indexed(d->prim, idx, n);
    } else {
        /* index list from DRAW_ARRAYS ranges or ARRAY_ELEMENT indices */
        uint32_t n = 0, *idx;
        if (d->nindices) {
            n = d->nindices; idx = idx_reserve(n);
            memcpy(idx, d->indices, n * 4);
        } else {
            for (int r = 0; r < d->nranges; r++) n += d->ranges[r * 2 + 1];
            idx = idx_reserve(n);
            uint32_t k = 0;
            for (int r = 0; r < d->nranges; r++)
                for (uint32_t j = 0; j < d->ranges[r * 2 + 1]; j++) idx[k++] = d->ranges[r * 2] + j;
        }
        if (!n) return;
        uint32_t vmin = 0xFFFFFFFF, vmax = 0;
        for (uint32_t i = 0; i < n; i++) { if (idx[i] < vmin) vmin = idx[i]; if (idx[i] > vmax) vmax = idx[i]; }
        if (vmax - vmin > 1000000) { xlog("draw: suspicious index range %u..%u", vmin, vmax); return; }
        bind_arrays(vmin, vmax);
        for (uint32_t i = 0; i < n; i++) idx[i] -= vmin;
        draw_indexed(d->prim, idx, n);
    }
    gl_check("draw");
}

void r_flush(void) {}
void r_frame_marker(void) { texture_frame_end(); }

void r_blit_2d(uint32_t src, uint32_t dst, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
               uint32_t w, uint32_t h, uint32_t sp_, uint32_t dp, uint32_t bpp)
{
    /* GPU-side copy when both ends are render targets, else a RAM copy */
    Surface *s = surface_find(src), *t = surface_find(dst);
    if (s && t) {
        static GLuint rfb, dfb;
        if (!rfb) { glGenFramebuffers(1, &rfb); glGenFramebuffers(1, &dfb); }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(sx * g_scale, sy * g_scale, (sx + w) * g_scale, (sy + h) * g_scale,
                          dx * g_scale, dy * g_scale, (dx + w) * g_scale, (dy + h) * g_scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g_rt.color = NULL; g_rt.zeta = NULL;
        return;
    }
    if (s) surface_download_to_ram(s);
    uint8_t *S = xmem_va(src), *D = xmem_va(dst);
    for (uint32_t y = 0; y < h; y++) memmove(D + (dy + y) * dp + dx * bpp, S + (sy + y) * sp_ + sx * bpp, w * bpp);
    if (t) surface_upload_from_ram(t);
}
