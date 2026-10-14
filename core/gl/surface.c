/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Render targets.
 *
 * Every NV2A color/zeta surface (identified by its physical address) is a GL
 * texture rendered at g_scale x the native resolution. GL row 0 holds the
 * Xbox's top scanline, i.e. the same order as video memory, so uploads and
 * downloads need no flipping; only presentation flips. */
#include "gl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

RenderTarget g_rt;
uint64_t g_frame_no;
static Surface *g_surfaces;
static GLuint g_fbo;
static uint32_t g_last_display_pa;
static uint32_t g_serial;

void surface_init(void)
{
    glGenFramebuffers(1, &g_fbo);
}

void surface_written(Surface *s) { if (s) s->serial = ++g_serial; }

/* ---------------- depth <-> color ----------------
 * A colour and a zeta surface at the same address are one piece of memory on
 * the Xbox but two GL textures here. Games use that: they sample a depth
 * buffer as an A8R8G8B8 texture (24-bit depth in A,R,G and stencil in B), or
 * render colour into memory that is then used as a depth buffer. Midnight
 * Club 3 does both to make a half-size copy of the depth buffer for its
 * headlight volumes. The serials say which of the two is newer; these passes
 * carry the content across, bit-exact for Z24S8 (stencil is not carried) and
 * for Z16 <-> R5G6B5. */
static GLuint g_conv_d2c, g_conv_c2d, g_conv_vao, g_conv_fbo;
static GLint g_conv_d2c_mode, g_conv_c2d_mode;

static void conv_init(void)
{
    if (g_conv_vao) return;
    static const char *vs =
        "#version 330\n"
        "void main() { gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1), 0.0, 1.0); }\n";
    g_conv_d2c = gl_compile_program(vs,
        "#version 330\nuniform sampler2D src; uniform int mode; out vec4 fragColor;\n"
        "void main() {\n"
        "  float d = texelFetch(src, ivec2(gl_FragCoord.xy), 0).r;\n"
        "  if (mode == 0) { uint v = uint(d * 16777215.0 + 0.5);\n"
        "    fragColor = vec4(float((v >> 8u) & 255u), float(v & 255u), 0.0, float(v >> 16u)) / 255.0; }\n"
        "  else { uint v = uint(d * 65535.0 + 0.5);\n"
        "    fragColor = vec4(float(v >> 11u) / 31.0, float((v >> 5u) & 63u) / 63.0, float(v & 31u) / 31.0, 1.0); }\n"
        "}\n");
    g_conv_c2d = gl_compile_program(vs,
        "#version 330\nuniform sampler2D src; uniform int mode;\n"
        "void main() {\n"
        "  vec4 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0);\n"
        "  if (mode == 0) { uvec4 b = uvec4(c * 255.0 + 0.5); gl_FragDepth = float((b.a << 16u) | (b.r << 8u) | b.g) / 16777215.0; }\n"
        "  else gl_FragDepth = float((uint(c.r * 31.0 + 0.5) << 11u) | (uint(c.g * 63.0 + 0.5) << 5u) | uint(c.b * 31.0 + 0.5)) / 65535.0;\n"
        "}\n");
    glUseProgram(g_conv_d2c); glUniform1i(glGetUniformLocation(g_conv_d2c, "src"), 7);
    g_conv_d2c_mode = glGetUniformLocation(g_conv_d2c, "mode");
    glUseProgram(g_conv_c2d); glUniform1i(glGetUniformLocation(g_conv_c2d, "src"), 7);
    g_conv_c2d_mode = glGetUniformLocation(g_conv_c2d, "mode");
    glGenVertexArrays(1, &g_conv_vao);
    glGenFramebuffers(1, &g_conv_fbo);
}

static void conv_state(int w, int h)
{
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glViewport(0, 0, w, h);
    glBindVertexArray(g_conv_vao);
}

/* z's depth -> z->view (RGBA8, what an A8R8G8B8 / R5G6B5 fetch of its memory returns) */
static void depth_to_view(Surface *z)
{
    conv_init();
    int W = z->w * g_scale, H = z->h * g_scale;
    if (!z->view) {
        glGenTextures(1, &z->view);
        glBindTexture(GL_TEXTURE_2D, z->view);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_conv_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, z->view, 0);
    GLenum db = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &db);
    conv_state(W, H);
    glDisable(GL_DEPTH_TEST); glColorMask(1, 1, 1, 1);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, z->tex); glBindSampler(7, 0);
    glUseProgram(g_conv_d2c); glUniform1i(g_conv_d2c_mode, (z->fmt & 0xF) == 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    z->view_serial = z->serial;
    gl_check("depth to color");
}

/* colour surface c's content -> zeta surface z (same memory) */
static void color_to_depth(Surface *c, Surface *z)
{
    conv_init();
    int W = z->w * g_scale, H = z->h * g_scale;
    glBindFramebuffer(GL_FRAMEBUFFER, g_conv_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, z->tex, 0);
    GLenum db = GL_NONE; glDrawBuffers(1, &db);
    conv_state(W, H);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE); glColorMask(0, 0, 0, 0);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, c->tex); glBindSampler(7, 0);
    glUseProgram(g_conv_c2d); glUniform1i(g_conv_c2d_mode, (z->fmt & 0xF) == 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    z->serial = c->serial;
    static int n; if (n++ < 8) xlog("surface: colour at %08x copied into the depth buffer there (%ux%u)", c->pa, z->w, z->h);
    gl_check("color to depth");
}

GLuint surface_texture_view(Surface *s)
{
    if (!s->zeta) return s->tex;
    if (!s->view || s->view_serial != s->serial) {
        depth_to_view(s);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);    /* back to the render target */
        glViewport(0, 0, g_rt.w * g_scale, g_rt.h * g_scale);
        static int n; if (n++ < 8) xlog("surface: depth buffer at %08x sampled as a texture (%ux%u)", s->pa, s->w, s->h);
    }
    return s->view;
}

static void surface_free(Surface *s)
{
    glDeleteTextures(1, &s->tex);
    if (s->view) glDeleteTextures(1, &s->view);
    free(s);
}

static int color_bpp(uint32_t f)
{
    switch (f) {
    case 1: case 2: case 3: return 2;
    case 9: return 1;
    case 0xA: return 2;
    default: return 4;
    }
}

/* RAM (native format) -> RGBA8 */
static void convert_to_rgba8(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt, uint8_t *dst)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + y * pitch;
        uint8_t *o = dst + y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            uint32_t v;
            switch (fmt) {
            case 3: v = ((const uint16_t *)row)[x];
                o[0] = ((v >> 11) & 0x1F) * 255 / 31; o[1] = ((v >> 5) & 0x3F) * 255 / 63; o[2] = (v & 0x1F) * 255 / 31; o[3] = 255; break;
            case 1: case 2: v = ((const uint16_t *)row)[x];
                o[0] = ((v >> 10) & 0x1F) * 255 / 31; o[1] = ((v >> 5) & 0x1F) * 255 / 31; o[2] = (v & 0x1F) * 255 / 31; o[3] = 255; break;
            case 9: o[0] = o[1] = o[2] = row[x]; o[3] = 255; break;
            case 0xA: o[0] = row[x * 2 + 1]; o[1] = row[x * 2]; o[2] = 0; o[3] = 255; break;
            default: v = ((const uint32_t *)row)[x];
                o[0] = (v >> 16) & 0xFF; o[1] = (v >> 8) & 0xFF; o[2] = v & 0xFF; o[3] = fmt == 8 ? (v >> 24) : 255; break;
            }
        }
    }
}

/* RGBA8 -> RAM (native format) */
static void convert_from_rgba8(const uint8_t *src, uint32_t w, uint32_t h, uint32_t fmt, uint8_t *dst, uint32_t pitch)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * w * 4;
        uint8_t *row = dst + y * pitch;
        for (uint32_t x = 0; x < w; x++, s += 4) {
            switch (fmt) {
            case 3: ((uint16_t *)row)[x] = (uint16_t)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3)); break;
            case 1: case 2: ((uint16_t *)row)[x] = (uint16_t)(((s[0] >> 3) << 10) | ((s[1] >> 3) << 5) | (s[2] >> 3)); break;
            case 9: row[x] = s[0]; break;
            case 0xA: row[x * 2] = s[1]; row[x * 2 + 1] = s[0]; break;
            default: ((uint32_t *)row)[x] = ((uint32_t)s[3] << 24) | (s[0] << 16) | (s[1] << 8) | s[2]; break;
            }
        }
    }
}

void surface_upload_from_ram(Surface *s)
{
    if (s->zeta) return;
    uint8_t *tmp = malloc(s->w * s->h * 4);
    convert_to_rgba8(xmem_va(s->pa), s->pitch, s->w, s->h, s->fmt, tmp);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    if (g_scale == 1) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    } else {
        /* nearest upscale on the CPU */
        uint32_t W = s->w * g_scale, H = s->h * g_scale;
        uint32_t *big = malloc(W * H * 4), *src = (uint32_t *)tmp;
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) big[y * W + x] = src[(y / g_scale) * s->w + x / g_scale];
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, big);
        free(big);
    }
    free(tmp);
}

void surface_download_to_ram(Surface *s)
{
    if (s->zeta) return;
    uint32_t W = s->w * g_scale, H = s->h * g_scale;
    uint8_t *big = malloc(W * H * 4), *small = malloc(s->w * s->h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, big);
    for (uint32_t y = 0; y < s->h; y++)
        for (uint32_t x = 0; x < s->w; x++)
            ((uint32_t *)small)[y * s->w + x] = ((uint32_t *)big)[(y * g_scale) * W + x * g_scale];
    convert_from_rgba8(small, s->w, s->h, s->fmt, xmem_va(s->pa), s->pitch);
    free(big); free(small);
    g_rt.color = NULL; g_rt.zeta = NULL;   /* force re-attach */
}

static Surface *surface_get(uint32_t pa, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fmt, int zeta)
{
    Surface *s, **pp;
    for (pp = &g_surfaces; (s = *pp); pp = &s->next) {
        if (s->pa == pa && s->zeta == zeta) {
            if (s->fmt == fmt && s->w >= w && s->h >= h && s->pitch == pitch) { s->frame_used = g_frame_no; return s; }
            /* shape changed: drop it */
            *pp = s->next;
            if (g_rt.color == s) g_rt.color = NULL;
            if (g_rt.zeta == s) g_rt.zeta = NULL;
            surface_free(s);
            break;
        }
    }
    s = calloc(1, sizeof *s);
    s->pa = pa; s->pitch = pitch; s->w = w; s->h = h; s->fmt = fmt; s->zeta = zeta;
    s->has_alpha = !zeta && (fmt == 8 || fmt == 6 || fmt == 7);
    s->frame_used = g_frame_no;
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (zeta) glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w * g_scale, h * g_scale, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * g_scale, h * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        surface_upload_from_ram(s);
    }
    s->next = g_surfaces; g_surfaces = s;
    xlog("surface: new %s %ux%u fmt %u pitch %u at %08x", zeta ? "zeta" : "color", w, h, fmt, pitch, pa);
    return s;
}

/* Change the internal resolution: write every colour surface back to RAM,
 * drop them all, and let them be recreated (from RAM) at the new scale. */
void surface_rescale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    if (scale == g_scale) return;
    for (Surface *s = g_surfaces; s; s = s->next) if (!s->zeta) surface_download_to_ram(s);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    for (Surface *s = g_surfaces, *n; s; s = n) { n = s->next; surface_free(s); }
    g_surfaces = NULL;
    g_rt.color = g_rt.zeta = NULL;
    g_scale = scale;
    xlog("display: internal resolution now %dx%d", 640 * g_scale, 480 * g_scale);
}

Surface *surface_find(uint32_t pa)
{
    for (Surface *s = g_surfaces; s; s = s->next) if (!s->zeta && s->pa == pa) return s;
    return NULL;
}

/* the surface whose content a texture at pa sees: colour or zeta, whichever was written last */
Surface *surface_find_texture(uint32_t pa, uint32_t w, uint32_t h)
{
    Surface *best = NULL;
    for (Surface *s = g_surfaces; s; s = s->next)
        if (s->pa == pa && s->w >= w && s->h >= h && (!best || s->serial > best->serial || (s->serial == best->serial && !s->zeta))) best = s;
    return best;
}

void r_surface_changed(void) { /* resolved lazily at the next draw/clear */ }

void surface_bind_current(void)
{
    uint32_t fmt = KREG(NV097_SET_SURFACE_FORMAT);
    uint32_t cfmt = fmt & 0xF, zfmt = (fmt >> 4) & 0xF, type = (fmt >> 8) & 0xF;
    uint32_t ch = KREG(NV097_SET_SURFACE_CLIP_HORIZONTAL), cv = KREG(NV097_SET_SURFACE_CLIP_VERTICAL);
    uint32_t pitch = KREG(NV097_SET_SURFACE_PITCH);
    uint32_t w = (ch & 0xFFFF) + (ch >> 16), h = (cv & 0xFFFF) + (cv >> 16);
    if (type == 2) { w = 1u << ((fmt >> 16) & 0xFF); h = 1u << ((fmt >> 24) & 0xFF); }
    if (!w) w = 640; if (!h) h = 480;

    Surface *c = NULL, *z = NULL;
    if (cfmt && g_kv.dma_color) {
        uint32_t pa = nv_dma_load(g_kv.dma_color).base + KREG(NV097_SET_SURFACE_COLOR_OFFSET);
        uint32_t cp = pitch & 0xFFFF; if (!cp) cp = w * color_bpp(cfmt);
        c = surface_get(pa & (XBOX_RAM_SIZE - 1), cp, w, h, cfmt, 0);
    }
    if (zfmt && g_kv.dma_zeta) {
        /* physical address 0 is never allocated (xmem keeps the first 64 KB):
         * D3D points the zeta surface there when a render target has none */
        uint32_t pa = (nv_dma_load(g_kv.dma_zeta).base + KREG(NV097_SET_SURFACE_ZETA_OFFSET)) & (XBOX_RAM_SIZE - 1);
        if (pa) z = surface_get(pa, pitch >> 16, w, h, 0x100 | zfmt, 1);
    }
    if (z) {   /* colour rendered into this depth buffer's memory since it was last written as depth */
        for (Surface *a = g_surfaces; a; a = a->next)
            if (!a->zeta && a != c && a->pa == z->pa && a->serial > z->serial && a->w == z->w && a->h == z->h && a->pitch == z->pitch &&
                (((z->fmt & 0xF) == 2 && color_bpp(a->fmt) == 4) || ((z->fmt & 0xF) == 1 && a->fmt == 3))) {
                if (g_rt.zeta == z) { glBindFramebuffer(GL_FRAMEBUFFER, g_fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0); g_rt.zeta = NULL; }
                color_to_depth(a, z);
                break;
            }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    if (c != g_rt.color) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c ? c->tex : 0, 0);
        g_rt.color = c;
    }
    if (z != g_rt.zeta) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, z ? z->tex : 0, 0);
        g_rt.zeta = z;
    }
    GLenum db = c ? GL_COLOR_ATTACHMENT0 : GL_NONE;
    glDrawBuffers(1, &db);
    g_rt.w = w; g_rt.h = h;
    g_rt.clip_x = ch & 0xFFFF; g_rt.clip_y = cv & 0xFFFF;
    glViewport(0, 0, w * g_scale, h * g_scale);
}

/* ---------------- clears ---------------- */
void r_clear(uint32_t flags)
{
    surface_bind_current();
    uint32_t rh = KREG(NV097_SET_CLEAR_RECT_HORIZONTAL), rv = KREG(NV097_SET_CLEAR_RECT_VERTICAL);
    int x0 = rh & 0xFFFF, x1 = (rh >> 16) + 1, y0 = rv & 0xFFFF, y1 = (rv >> 16) + 1;
    capture_note("clear %02x rect %d,%d-%d,%d color %08x zs %08x rt %08x z %08x", flags, x0, y0, x1, y1, KREG(NV097_SET_COLOR_CLEAR_VALUE),
                 KREG(NV097_SET_ZSTENCIL_CLEAR_VALUE), g_rt.color ? g_rt.color->pa : 0, g_rt.zeta ? g_rt.zeta->pa : 0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0 * g_scale, y0 * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
    if ((flags & 0xF0) && g_rt.color) {
        uint32_t cv = KREG(NV097_SET_COLOR_CLEAR_VALUE);
        float col[4];
        if (g_rt.color->fmt == 3) {
            col[0] = ((cv >> 11) & 0x1F) / 31.0f; col[1] = ((cv >> 5) & 0x3F) / 63.0f; col[2] = (cv & 0x1F) / 31.0f; col[3] = 1;
        } else if (g_rt.color->fmt == 1 || g_rt.color->fmt == 2) {
            col[0] = ((cv >> 10) & 0x1F) / 31.0f; col[1] = ((cv >> 5) & 0x1F) / 31.0f; col[2] = (cv & 0x1F) / 31.0f; col[3] = 1;
        } else {
            col[0] = ((cv >> 16) & 0xFF) / 255.0f; col[1] = ((cv >> 8) & 0xFF) / 255.0f; col[2] = (cv & 0xFF) / 255.0f;
            col[3] = g_rt.color->has_alpha ? (cv >> 24) / 255.0f : 1.0f;
        }
        glColorMask((flags >> 4) & 1, (flags >> 5) & 1, (flags >> 6) & 1, g_rt.color->has_alpha ? (flags >> 7) & 1 : 1);
        glClearBufferfv(GL_COLOR, 0, col);
    }
    if ((flags & 3) && g_rt.zeta) {
        uint32_t zv = KREG(NV097_SET_ZSTENCIL_CLEAR_VALUE);
        float depth; int stencil = 0;
        if ((g_rt.zeta->fmt & 0xF) == 2) { depth = (zv >> 8) / 16777215.0f; stencil = zv & 0xFF; }
        else depth = (zv & 0xFFFF) / 65535.0f;
        glDepthMask(flags & 1 ? GL_TRUE : GL_FALSE);
        glStencilMask(flags & 2 ? 0xFF : 0);
        if ((flags & 3) == 3) glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, stencil);
        else if (flags & 1) glClearBufferfv(GL_DEPTH, 0, &depth);
        else { GLint st = stencil; glClearBufferiv(GL_STENCIL, 0, &st); }
    }
    glColorMask(1, 1, 1, 1); glDepthMask(GL_TRUE); glStencilMask(0xFF);
    glDisable(GL_SCISSOR_TEST);
    if (flags & 0xF0) surface_written(g_rt.color);
    if (flags & 3) surface_written(g_rt.zeta);
    gl_check("clear");
}

/* ---------------- presentation ---------------- */
static void dump_surface(Surface *s, const char *tag)
{
    uint32_t W = s->w * g_scale, H = s->h * g_scale;
    uint8_t *px = malloc(W * H * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, px);
    g_rt.color = NULL; g_rt.zeta = NULL;
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\surf_%s_%08x_f%llu.bmp", g_screenshot_dir, tag, s->pa, (unsigned long long)g_frame_no);
    FILE *f = fopen(path, "wb");
    if (f) {
        BITMAPFILEHEADER fh = { 0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + W * H * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
        BITMAPINFOHEADER ih = { sizeof ih, (LONG)W, -(LONG)H, 1, 32, BI_RGB, W * H * 4, 2835, 2835, 0, 0 };
        fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(px, 1, W * H * 4, f); fclose(f);
    }
    free(px);
}
int g_dump_frame = -1;

void gpu_present(void)
{
    uint32_t pa = R(NV_PCRTC_START) & (XBOX_RAM_SIZE - 1);
    Surface *s = pa ? surface_find(pa) : NULL;
    if ((g_frame_no % 600) == 0) xlog("present: frame %llu PCRTC_START=%08x surface=%p", (unsigned long long)g_frame_no, R(NV_PCRTC_START), s);
    if (g_dump_frame >= 0 && g_frame_no == (uint64_t)g_dump_frame)
        for (Surface *q = g_surfaces; q; q = q->next) if (!q->zeta) dump_surface(q, "c");
    if (s) {
        s->frame_used = g_frame_no;          /* the scanned-out buffer is in use */
        gl_present_texture(s->tex, s->w * g_scale, s->h * g_scale, 1);
        if (pa != g_last_display_pa) g_perf.flips++;
        g_last_display_pa = pa;
    } else {
        gl_present_texture(0, 0, 0, 0);
    }
    g_frame_no++;
    /* drop surfaces unused for a long time */
    for (Surface **pp = &g_surfaces, *q; (q = *pp);) {
        if (g_frame_no - q->frame_used > 600 && q != g_rt.color && q != g_rt.zeta) {
            *pp = q->next; surface_free(q);
        } else pp = &q->next;
    }
}

void nv2a_display_start_changed(uint32_t pa) {}

/* ---------------- performance log ---------------- */
int g_perf_log;   /* --perf: report every 2 s instead of every 10, with trap hot spots */

void perf_report(void)
{
    static uint64_t t0, freq, f0; static NvPerf p0; static LONG m0;
    uint64_t now = perf_ticks();
    if (!freq) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = (uint64_t)f.QuadPart;
        t0 = now; p0 = g_perf; m0 = g_mmio_traps; f0 = g_frame_no;
        return;
    }
    double secs = (double)(now - t0) / (double)freq;
    if (secs < (g_perf_log ? 2.0 : 10.0)) return;
    NvPerf d = g_perf; LONG m = g_mmio_traps;
    extern volatile LONG g_input_polls; static LONG i0; LONG ip = g_input_polls;
#define PCT(f) (100.0 * (double)(d.f - p0.f) / (double)(now - t0))
#define RATE(f) ((double)(d.f - p0.f) / secs)
    xlog("perf: %.1f vblanks/s, %.1f game frames/s | GPU thread: commands %.0f%% (draws %.0f%%, texture checks %.0f%%, "
         "uploads %.0f%%), present %.0f%% | %.0f draws/s, checked %.0f MB/s, uploaded %.0f textures/s (%.1f MB/s) | %.0f device traps/s, %.0f pad polls/s",
         (double)(g_frame_no - f0) / secs, RATE(flips), PCT(exec), PCT(draw), PCT(hash), PCT(upload), PCT(present),
         RATE(draws), RATE(hash_bytes) / 1048576.0, RATE(uploads), RATE(upload_bytes) / 1048576.0, (double)(m - m0) / secs,
         (double)(ip - i0) / secs);
    i0 = ip;
#undef PCT
#undef RATE
    if (g_perf_log) mmio_report_hot(secs);
    t0 = now; p0 = d; m0 = m; f0 = g_frame_no;
}
