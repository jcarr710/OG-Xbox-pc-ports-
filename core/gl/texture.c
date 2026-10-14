/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Textures: NV2A formats -> GL.
 *
 * Swizzled (Morton order), linear ("image rect") and DXT textures, 8-bit
 * palettes, mipmaps and cube maps. Everything except DXT is converted to
 * RGBA8 on the CPU when uploaded. Uploaded textures are cached by address +
 * format and re-validated once per frame with a content hash. Textures that
 * alias a render target are sampled directly from the GL surface. */
#include "gl.h"
#include "../title.h"
#include "../settings.h"
#include <string.h>
#include <stdlib.h>

typedef struct TexEntry {
    uint32_t pa, format, rect, pitch, palette_pa, palette_hash;
    uint64_t hash, sample; uint32_t size, checks;
    uint64_t checked_frame, used_frame;
    GLuint   tex; GLenum target;
    struct TexEntry *next;
} TexEntry;

static TexEntry *g_tex[1024];
static GLuint g_samplers[4];

void texture_init(void) { glGenSamplers(4, g_samplers); }

/* ---- format table ---- */
enum { K_RGBA, K_DXT1, K_DXT3, K_DXT5, K_PAL8, K_NONE };
typedef struct { uint8_t bpp; uint8_t linear; uint8_t kind; } FmtInfo;
static FmtInfo fmt_info(uint32_t c)
{
    switch (c) {
    case 0x00: case 0x01: case 0x19: return (FmtInfo){ 1, 0, K_RGBA };
    case 0x13: case 0x1B: case 0x1F: return (FmtInfo){ 1, 1, K_RGBA };
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: case 0x27: case 0x28: case 0x29: return (FmtInfo){ 2, 0, K_RGBA };
    case 0x10: case 0x11: case 0x17: case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25: case 0x30: case 0x31: case 0x35: return (FmtInfo){ 2, 1, K_RGBA };
    case 0x06: case 0x07: case 0x3A: case 0x3B: case 0x3C: return (FmtInfo){ 4, 0, K_RGBA };
    case 0x12: case 0x1E: case 0x2E: case 0x2F: case 0x3F: case 0x40: case 0x41: return (FmtInfo){ 4, 1, K_RGBA };
    case 0x0B: return (FmtInfo){ 1, 0, K_PAL8 };
    case 0x0C: return (FmtInfo){ 8, 0, K_DXT1 };   /* bytes per 4x4 block */
    case 0x0E: return (FmtInfo){ 16, 0, K_DXT3 };
    case 0x0F: return (FmtInfo){ 16, 0, K_DXT5 };
    default: return (FmtInfo){ 4, 0, K_NONE };
    }
}

/* one texel of format c at p -> RGBA8 (R,G,B,A bytes) */
static void texel(uint32_t c, const uint8_t *p, uint8_t *o)
{
    uint32_t v;
    switch (c) {
    case 0x00: case 0x13: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;                     /* Y8 */
    case 0x01: case 0x1B: o[0] = o[1] = o[2] = o[3] = p[0]; break;                         /* AY8 */
    case 0x19: case 0x1F: o[0] = o[1] = o[2] = 255; o[3] = p[0]; break;                    /* A8 */
    case 0x1A: case 0x20: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;                   /* A8Y8 */
    case 0x02: case 0x10: v = *(const uint16_t *)p;                                        /* A1R5G5B5 */
        o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; o[3] = (v & 0x8000) ? 255 : 0; break;
    case 0x03: case 0x1C: v = *(const uint16_t *)p;                                        /* X1R5G5B5 */
        o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; o[3] = 255; break;
    case 0x04: case 0x1D: v = *(const uint16_t *)p;                                        /* A4R4G4B4 */
        o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17; o[2] = (v & 15) * 17; o[3] = (v >> 12) * 17; break;
    case 0x05: case 0x11: v = *(const uint16_t *)p;                                        /* R5G6B5 */
        o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255; break;
    case 0x17: case 0x28: o[0] = p[1]; o[1] = p[0]; o[2] = 0; o[3] = 255; break;           /* G8B8 */
    case 0x29: o[0] = p[0]; o[1] = 0; o[2] = p[1]; o[3] = 255; break;                      /* R8B8 */
    case 0x06: case 0x12: v = *(const uint32_t *)p;                                        /* A8R8G8B8 */
        o[0] = (v >> 16) & 255; o[1] = (v >> 8) & 255; o[2] = v & 255; o[3] = v >> 24; break;
    case 0x07: case 0x1E: v = *(const uint32_t *)p;                                        /* X8R8G8B8 */
        o[0] = (v >> 16) & 255; o[1] = (v >> 8) & 255; o[2] = v & 255; o[3] = 255; break;
    case 0x3A: case 0x3F: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;       /* A8B8G8R8 */
    case 0x3B: case 0x40: o[0] = p[1]; o[1] = p[2]; o[2] = p[3]; o[3] = p[0]; break;       /* B8G8R8A8 */
    case 0x3C: case 0x41: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;       /* R8G8B8A8 */
    case 0x30: case 0x35: o[0] = o[1] = o[2] = p[1]; o[3] = 255; break;                    /* 16-bit depth/Y16 */
    default: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255; break;
    }
}

/* Morton (swizzle) masks */
static void swz_masks(uint32_t w, uint32_t h, uint32_t d, uint32_t *mx, uint32_t *my, uint32_t *mz)
{
    uint32_t x = 0, y = 0, z = 0, bit = 1, mb = 1; int done;
    do {
        done = 1;
        if (bit < w) { x |= mb; mb <<= 1; done = 0; }
        if (bit < h) { y |= mb; mb <<= 1; done = 0; }
        if (bit < d) { z |= mb; mb <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mx = x; *my = y; *mz = z;
}

/* decode one mip level of a non-DXT texture into RGBA8 */
static void decode_level(uint32_t c, FmtInfo fi, const uint8_t *src, uint32_t w, uint32_t h, uint32_t pitch,
                         const uint32_t *palette, uint8_t *out)
{
    if (fi.linear) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                texel(c, src + y * pitch + x * fi.bpp, out + (y * w + x) * 4);
        return;
    }
    uint32_t mx, my, mz; swz_masks(w, h, 1, &mx, &my, &mz);
    uint32_t oy = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t ox = 0;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = src + (ox | oy) * fi.bpp;
            uint8_t *o = out + (y * w + x) * 4;
            if (fi.kind == K_PAL8) {
                uint32_t v = palette ? palette[p[0]] : 0xFF000000u | (p[0] * 0x010101u);
                o[0] = (v >> 16) & 255; o[1] = (v >> 8) & 255; o[2] = v & 255; o[3] = v >> 24;
            } else texel(c, p, o);
            ox = (ox - mx) & mx;
        }
        oy = (oy - my) & my;
    }
}

/* Content hash. Four independent lanes so the multiplies pipeline; this is
 * bound by memory bandwidth rather than multiply latency. */
static inline uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint64_t hash_mem(const void *p, uint32_t n)
{
    const uint32_t *q = p;
    uint32_t a = 0x9E3779B1u ^ n, b = 0x85EBCA77u, c = 0xC2B2AE3Du, d = 0x27D4EB2Fu;
    uint32_t i = 0, words = n / 4;
    for (; i + 4 <= words; i += 4) {
        a = rotl32(a + q[i] * 0x85EBCA77u, 13) * 0x9E3779B1u;
        b = rotl32(b + q[i + 1] * 0x85EBCA77u, 13) * 0x9E3779B1u;
        c = rotl32(c + q[i + 2] * 0x85EBCA77u, 13) * 0x9E3779B1u;
        d = rotl32(d + q[i + 3] * 0x85EBCA77u, 13) * 0x9E3779B1u;
    }
    for (; i < words; i++) a = rotl32(a + q[i] * 0x85EBCA77u, 13) * 0x9E3779B1u;
    const uint8_t *t = (const uint8_t *)p + words * 4;
    for (uint32_t k = 0; k < (n & 3); k++) b = (b ^ t[k]) * 0x01000193u;
    uint64_t h = ((uint64_t)(rotl32(a, 1) + rotl32(b, 7)) << 32) | (uint32_t)(rotl32(c, 12) + rotl32(d, 18));
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return h;
}

/* Cheap probe of a large texture: 64 spread-out 32-byte chunks plus both ends. */
static uint64_t hash_sample(const uint8_t *p, uint32_t n)
{
    uint64_t h = hash_mem(p, 256) ^ (hash_mem(p + n - 256, 256) * 31);
    uint32_t step = (n - 32) / 64;
    for (uint32_t k = 0; k < 64; k++) h = (h ^ hash_mem(p + k * step + ((k * 40503u) % (step - 32 + 1)), 32)) * 0x100000001b3ull;
    return h;
}

/* Every texture bound during a frame is checked for CPU changes. Small ones
 * are fully hashed each frame; large ones are probed each frame and fully
 * hashed every TEX_FULL_PERIOD frames (staggered), so a change the probe
 * misses shows up within that many frames. */
#define TEX_SMALL       16384u
#define TEX_FULL_PERIOD 8u
static int g_tex_fullhash = -1;     /* <STEM>_TEX_FULLHASH=1: full hash every frame */

static uint32_t level_bytes(FmtInfo fi, uint32_t w, uint32_t h)
{
    if (fi.kind >= K_DXT1 && fi.kind <= K_DXT5) return ((w + 3) / 4) * ((h + 3) / 4) * fi.bpp;
    return w * h * fi.bpp;
}

static void upload(TexEntry *e, uint32_t c, FmtInfo fi, uint32_t w, uint32_t h, uint32_t levels, uint32_t pitch,
                   int cube, const uint32_t *palette)
{
    const uint8_t *base = xmem_va(e->pa);
    GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glBindTexture(target, e->tex);
    uint8_t *buf = malloc(w * h * 4 + 64);
    uint32_t off = 0;
    for (int face = 0; face < (cube ? 6 : 1); face++) {
        GLenum t = cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
        uint32_t lw = w, lh = h;
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t n = level_bytes(fi, lw, lh);
            if (fi.kind == K_DXT1 || fi.kind == K_DXT3 || fi.kind == K_DXT5) {
                GLenum ifmt = fi.kind == K_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : fi.kind == K_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                                                                                    : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                glCompressedTexImage2D(t, l, ifmt, lw, lh, 0, n, base + off);
            } else {
                decode_level(c, fi, base + off, lw, lh, fi.linear ? pitch : lw * fi.bpp, palette, buf);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(t, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            }
            off += n;
            if (lw > 1) lw >>= 1; if (lh > 1) lh >>= 1;
        }
        if (cube) off = (off + 0x7F) & ~0x7Fu;   /* cube faces are 128-byte aligned */
    }
    glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    free(buf);
    e->target = target;
    gl_check("texture upload");
}

int texture_bind_stage(int i, BoundTex *out)
{
    uint32_t base = NV097_SET_TEXTURE_OFFSET + i * 0x40;
    uint32_t ctl0 = KREG(base + 0x0C);
    memset(out, 0, sizeof *out);
    if (!(ctl0 & 0x40000000)) return 0;
    uint32_t fmt = KREG(base + 0x04), offset = KREG(base + 0x00);
    uint32_t c = (fmt >> 8) & 0xFF, dims = (fmt >> 4) & 0xF, cube = (fmt >> 2) & 1;
    uint32_t levels = (fmt >> 16) & 0xF; if (!levels) levels = 1;
    uint32_t w = 1u << ((fmt >> 20) & 0xF), h = 1u << ((fmt >> 24) & 0xF);
    FmtInfo fi = fmt_info(c);
    if (fi.kind == K_NONE) { static int n; if (n++ < 16) xlog("texture: unsupported format 0x%02x", c); return 0; }
    uint32_t dma = (fmt & 3) == 2 ? g_kv.dma_b : g_kv.dma_a;
    uint32_t pa = (nv_dma_load(dma).base + offset) & (XBOX_RAM_SIZE - 1);
    uint32_t pitch = KREG(base + 0x10) >> 16, rect = KREG(base + 0x1C);
    if (fi.linear) { w = rect >> 16; h = rect & 0xFFFF; levels = 1; if (!w || !h) return 0; }
    if (dims == 3) { static int n; if (n++ < 4) xlog("texture: 3D textures not yet supported"); return 0; }

    GLuint gltex = 0; GLenum target = GL_TEXTURE_2D;
    /* render-target aliasing (a depth buffer is read through its colour view) */
    Surface *s = cube ? NULL : surface_find_texture(pa, w, h);
    if (s) {
        gltex = surface_texture_view(s);
        s->frame_used = g_frame_no;
        out->rect = 1; out->width = (float)w; out->height = (float)h;
        if (!fi.linear) { out->rect = 0; }
    } else {
        uint32_t palette_pa = 0, palette_hash = 0; uint32_t *palette = NULL;
        if (fi.kind == K_PAL8) {
            uint32_t pal = KREG(base + 0x20);
            uint32_t pdma = (pal & 3) == 2 ? g_kv.dma_b : g_kv.dma_a;
            palette_pa = (nv_dma_load(pdma).base + (pal & 0xFFFFFFC0)) & (XBOX_RAM_SIZE - 1);
            uint32_t plen = 256 >> ((pal >> 2) & 3);
            palette = (uint32_t *)xmem_va(palette_pa);
            palette_hash = (uint32_t)hash_mem(palette, plen * 4);
        }
        uint32_t size = 0, lw = w, lh = h;
        if (fi.linear) size = pitch * h;
        else for (uint32_t l = 0; l < levels; l++) { size += level_bytes(fi, lw, lh); if (lw > 1) lw >>= 1; if (lh > 1) lh >>= 1; }
        if (cube) size = ((size + 0x7F) & ~0x7Fu) * 6;
        if (pa + size > XBOX_RAM_SIZE) return 0;

        unsigned slot = (pa >> 6) & 1023;
        TexEntry *e;
        for (e = g_tex[slot]; e; e = e->next)
            if (e->pa == pa && e->format == fmt && e->rect == rect && e->pitch == pitch && e->palette_pa == palette_pa) break;
        int need_upload = 0;
        if (!e) {
            e = calloc(1, sizeof *e);
            e->pa = pa; e->format = fmt; e->rect = rect; e->pitch = pitch; e->palette_pa = palette_pa;
            e->checks = (pa >> 12) % TEX_FULL_PERIOD;      /* stagger full checks across textures */
            glGenTextures(1, &e->tex);
            e->next = g_tex[slot]; g_tex[slot] = e;
            need_upload = 1;
        }
        if (need_upload || e->checked_frame != g_frame_no) {
            uint64_t t0 = perf_ticks();
            const uint8_t *src = xmem_va(pa);
            if (g_tex_fullhash < 0) g_tex_fullhash = title_env("TEX_FULLHASH") != NULL;
            int full = need_upload || size <= TEX_SMALL || g_tex_fullhash || (++e->checks % TEX_FULL_PERIOD) == 0;
            uint64_t smp = size > TEX_SMALL ? hash_sample(src, size) : 0;
            if (smp != e->sample || palette_hash != e->palette_hash) full = 1;
            uint64_t hsh = e->hash;
            if (full) { hsh = hash_mem(src, size); g_perf.hash_bytes += size; }
            else g_perf.hash_bytes += 64 * 32 + 512;
            g_perf.hash += perf_ticks() - t0;
            if (need_upload || hsh != e->hash || palette_hash != e->palette_hash) {
                uint64_t t1 = perf_ticks();
                upload(e, c, fi, w, h, levels, pitch, cube, palette);
                g_perf.upload += perf_ticks() - t1; g_perf.uploads++; g_perf.upload_bytes += size;
                e->hash = hsh; e->palette_hash = palette_hash; e->size = size;
            }
            e->sample = smp;
            e->checked_frame = g_frame_no;
        }
        e->used_frame = g_frame_no;
        gltex = e->tex; target = e->target;
        out->rect = fi.linear; out->width = (float)w; out->height = (float)h;
    }

    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(target, gltex);
    /* sampler state */
    static const GLenum wrap_map[] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER, GL_CLAMP_TO_EDGE, GL_REPEAT, GL_REPEAT };
    uint32_t addr = KREG(base + 0x08), filt = KREG(base + 0x14);
    GLuint smp = g_samplers[i];
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, wrap_map[addr & 7]);
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, wrap_map[(addr >> 8) & 7]);
    glSamplerParameteri(smp, GL_TEXTURE_WRAP_R, wrap_map[(addr >> 16) & 7]);
    uint32_t minf = (filt >> 16) & 0xFF, magf = (filt >> 24) & 0xF;
    static const GLenum min_map[] = { GL_LINEAR, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
                                      GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR };
    GLenum gmin = min_map[minf & 7];
    if (levels == 1 || s) gmin =(gmin == GL_NEAREST || gmin == GL_NEAREST_MIPMAP_NEAREST || gmin == GL_NEAREST_MIPMAP_LINEAR) ? GL_NEAREST : GL_LINEAR;
    glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, gmin);
    glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, magf == 1 ? GL_NEAREST : GL_LINEAR);
    /* anisotropic filtering (settings), where the driver has it */
    static float max_aniso = -1;
    if (max_aniso < 0) {
        max_aniso = 1;
        glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &max_aniso);
        if (glGetError() || max_aniso < 1) max_aniso = 1;
        xlog("gl: anisotropic filtering up to %.0fx", max_aniso);
    }
    if (max_aniso > 1) {
        float an = (gmin != GL_NEAREST && g_set.aniso > 1) ? (float)g_set.aniso : 1.0f;
        if (an > max_aniso) an = max_aniso;
        glSamplerParameterfv(smp, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, &an);
    }
    uint32_t bc = KREG(base + 0x24);
    float border[4] = { ((bc >> 16) & 255) / 255.0f, ((bc >> 8) & 255) / 255.0f, (bc & 255) / 255.0f, (bc >> 24) / 255.0f };
    glSamplerParameterfv(smp, GL_TEXTURE_BORDER_COLOR, border);
    glBindSampler(i, smp);
    out->target = target; out->tex = gltex;
    gl_check("texture bind");
    return 1;
}

void texture_frame_end(void)
{
    /* evict textures unused for ~10 seconds */
    for (int i = 0; i < 1024; i++) {
        for (TexEntry **pp = &g_tex[i], *e; (e = *pp);) {
            if (g_frame_no - e->used_frame > 600) { *pp = e->next; glDeleteTextures(1, &e->tex); free(e); }
            else pp = &e->next;
        }
    }
}
