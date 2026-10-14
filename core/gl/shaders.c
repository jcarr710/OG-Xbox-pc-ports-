/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GLSL generation for the NV2A pipeline.
 *
 * Vertex: either a translation of the NV2A vertex-program microcode (MAC +
 *         ILU paired ops, 192 constants, A0 relative addressing) or a
 *         fixed-function T&L emulation; both read one constant array c[].
 *         NV2A outputs screen-space positions, which are mapped back to GL
 *         clip space using the surface size (row 0 = Xbox top line).
 * Fragment: texture-shader stages + up to 8 register-combiner stages and the
 *         final combiner, alpha test, fog.
 * Programs are cached by a key describing all state that changes the code. */
#include "gl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ---------------- string builder ---------------- */
typedef struct { char *p; size_t n, cap; } Str;
static void sp(Str *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char tmp[2048]; int k = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (k < 0) return;
    if (s->n + k + 1 > s->cap) { s->cap = (s->n + k + 1) * 2 + 4096; s->p = realloc(s->p, s->cap); }
    memcpy(s->p + s->n, tmp, k + 1); s->n += k;
}

/* ---------------- shader key ---------------- */
typedef struct {
    uint32_t vp, vp_hash, vp_len;
    uint32_t ff[12];                /* fixed-function state words */
    uint32_t comb_ctl, shader_prog, other_input, final0, final1, spec_en;
    uint32_t rgb_in[8], rgb_out[8], a_in[8], a_out[8];
    uint8_t  tex_type[4];           /* 0 none, 1 2D, 2 2D rect, 3 cube */
    uint8_t  alpha_test, alpha_func, fog_en, fog_mode;
    uint32_t ctl0[4];               /* alpha kill */
    uint8_t  blend_signed;          /* 1 FUNC_ADD_SIGNED, 2 FUNC_REVERSE_SUBTRACT_SIGNED (done here, see draw.c) */
} ShaderKey;

typedef struct CacheEnt { ShaderKey key; ShaderProg prog; struct CacheEnt *next; } CacheEnt;
static CacheEnt *g_cache[512];

void shaders_init(void) {}

static uint32_t hash32(const void *p, size_t n)
{
    const uint8_t *b = p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* ---------------- vertex program translation ---------------- */
static uint32_t F(const uint32_t *t, int dw, int start, int len) { return (t[dw] >> start) & ((1u << len) - 1); }
static const char *g_out_names[16] = { "oPos", "oUnk1", "oUnk2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "oUnk13", "oUnk14", "oA0" };
static const char *g_mask[16] = { "", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw" };

static void vp_input(Str *s, const uint32_t *t, int which, int scalar_c)
{
    int neg, sx, sy, sz, sw, reg, mux;
    if (which == 0) { neg = F(t,1,8,1); sx = F(t,1,6,2); sy = F(t,1,4,2); sz = F(t,1,2,2); sw = F(t,1,0,2); reg = F(t,2,28,4); mux = F(t,2,26,2); }
    else if (which == 1) { neg = F(t,2,25,1); sx = F(t,2,23,2); sy = F(t,2,21,2); sz = F(t,2,19,2); sw = F(t,2,17,2); reg = F(t,2,13,4); mux = F(t,2,11,2); }
    else { neg = F(t,2,10,1); sx = F(t,2,8,2); sy = F(t,2,6,2); sz = F(t,2,4,2); sw = F(t,2,2,2); reg = (F(t,2,0,2) << 2) | F(t,3,30,2); mux = F(t,3,28,2); }
    if (scalar_c) sy = sz = sw = sx;
    const char *c = "xyzw";
    sp(s, "%s", neg ? "-" : "");
    if (mux == 1) sp(s, "R%d", reg);
    else if (mux == 2) sp(s, "v%d", F(t,1,9,4));
    else if (mux == 3) {
        int ci = F(t,1,13,8);
        if (F(t,3,1,1)) sp(s, "c[clamp(A0 + %d, 0, 191)]", ci); else sp(s, "c[%d]", ci);
    } else sp(s, "vec4(0.0)");
    sp(s, ".%c%c%c%c", c[sx], c[sy], c[sz], c[sw]);
}

static void gen_vertex_program(Str *s, const uint32_t (*prog)[4], int start, int maxlen)
{
    static const char *mac_fn[] = { "", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT", "SGE", "ARL" };
    static const int mac_args[][3] = { {0,0,0},{1,0,0},{1,1,0},{1,0,1},{1,1,1},{1,1,0},{1,1,0},{1,1,0},{1,1,0},{1,1,0},{1,1,0},{1,1,0},{1,1,0},{1,0,0} };
    static const char *ilu_fn[] = { "", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT" };
    for (int slot = start; slot < KV_PROGRAM_LEN && slot - start < maxlen; slot++) {
        const uint32_t *t = prog[slot];
        int mac = F(t,1,21,4), ilu = F(t,1,25,3);
        int out_r = F(t,3,20,4), mac_mask = F(t,3,24,4), ilu_mask = F(t,3,16,4), o_mask = F(t,3,12,4);
        int orb = F(t,3,11,1), oaddr = F(t,3,3,8), omux = F(t,3,2,1);
        sp(s, "  // %02d: %08x %08x %08x %08x\n", slot, t[0], t[1], t[2], t[3]);
        if (mac > 13) mac = 0;
        int paired = mac && ilu;
        /* MAC */
        if (mac) {
            Str args = { 0 };
            for (int a = 0; a < 3; a++) if (mac_args[mac][a]) { sp(&args, ", _in("); vp_input(&args, t, a, 0); sp(&args, ")"); }
            if (mac == 13) {        /* ARL */
                sp(s, "  { int _a = _ARL(_in("); vp_input(s, t, 0, 0); sp(s, ").x);");
                if (paired) sp(s, " _tmp_addr = _a; _has_tmp_addr = true; }\n"); else sp(s, " A0 = _a; }\n");
            } else {
                sp(s, "  _mac = _%s(%s);\n", mac_fn[mac], args.p + 2);
                if (mac_mask && !(paired && out_r == 1)) {
                    if (paired) sp(s, "  _macR = _mac; _macMask%d = true;\n", out_r);
                    else sp(s, "  R%d.%s = _mac.%s;\n", out_r, g_mask[mac_mask], g_mask[mac_mask]);
                }
                if (omux == 0 && o_mask) {
                    if (orb == 1) sp(s, "  %s.%s = _mac.%s;\n", g_out_names[oaddr & 0xF], g_mask[o_mask], g_mask[o_mask]);
                    else sp(s, "  /* write to c[%d] not supported */\n", oaddr);
                }
            }
            free(args.p);
        }
        /* ILU (uses input C, scalar for most ops) */
        if (ilu) {
            int scalar = ilu >= 2 && ilu <= 6;
            sp(s, "  _ilu = _%s(_in(", ilu_fn[ilu]); vp_input(s, t, 2, scalar); sp(s, "));\n");
            int dst = paired ? 1 : out_r;
            if (ilu_mask) sp(s, "  R%d.%s = _ilu.%s;\n", dst, g_mask[ilu_mask], g_mask[ilu_mask]);
            if (omux == 1 && o_mask) {
                if (orb == 1) sp(s, "  %s.%s = _ilu.%s;\n", g_out_names[oaddr & 0xF], g_mask[o_mask], g_mask[o_mask]);
                else sp(s, "  /* write to c[%d] not supported */\n", oaddr);
            }
        }
        /* paired MAC result lands after the ILU read its inputs */
        if (paired && mac != 13 && mac_mask && out_r != 1) sp(s, "  R%d.%s = _macR.%s;\n", out_r, g_mask[mac_mask], g_mask[mac_mask]);
        if (paired && mac == 13) sp(s, "  if (_has_tmp_addr) A0 = _tmp_addr;\n");
        if (F(t,3,0,1)) break;   /* FINAL */
    }
}

static const char *g_vp_header =
    "vec4 _in(float v) { return vec4(v); }\n"
    "vec4 _in(vec4 v) { return v; }\n"
    "vec4 _MOV(vec4 a) { return a; }\n"
    "vec4 _MUL(vec4 a, vec4 b) { vec4 r = a * b; vec4 z = vec4(equal(a, vec4(0.0))) + vec4(equal(b, vec4(0.0)));\n"
    "  return mix(r, vec4(0.0), greaterThan(z, vec4(0.0))); }\n"
    "vec4 _ADD(vec4 a, vec4 c) { return a + c; }\n"
    "vec4 _MAD(vec4 a, vec4 b, vec4 c) { return _MUL(a, b) + c; }\n"
    "vec4 _DP3(vec4 a, vec4 b) { return vec4(dot(a.xyz, b.xyz)); }\n"
    "vec4 _DPH(vec4 a, vec4 b) { return vec4(dot(vec4(a.xyz, 1.0), b)); }\n"
    "vec4 _DP4(vec4 a, vec4 b) { return vec4(dot(a, b)); }\n"
    "vec4 _DST(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }\n"
    "vec4 _MIN(vec4 a, vec4 b) { return min(a, b); }\n"
    "vec4 _MAX(vec4 a, vec4 b) { return max(a, b); }\n"
    "vec4 _SLT(vec4 a, vec4 b) { return vec4(lessThan(a, b)); }\n"
    "vec4 _SGE(vec4 a, vec4 b) { return vec4(greaterThanEqual(a, b)); }\n"
    "int  _ARL(float a) { return int(floor(a + 0.001)); }\n"
    "vec4 _RCP(vec4 a) { return vec4(1.0 / a.x); }\n"
    "float _clampAway(float t) { if (t > 0.0 || floatBitsToUint(t) == 0u) return clamp(t, 5.42101086e-20, 1.84467441e19);\n"
    "  return clamp(t, -1.84467441e19, -5.42101086e-20); }\n"
    "vec4 _RCC(vec4 a) { return vec4(_clampAway(1.0 / a.x)); }\n"
    "vec4 _RSQ(vec4 a) { float s = a.x; if (s == 0.0) return vec4(1.0/0.0); return vec4(inversesqrt(abs(s))); }\n"
    "vec4 _EXP(vec4 a) { float s = a.x; return vec4(exp2(floor(s)), s - floor(s), exp2(s), 1.0); }\n"
    "vec4 _LOG(vec4 a) { float t = abs(a.x); if (t == 0.0) return vec4(-1.0/0.0, 1.0, -1.0/0.0, 1.0);\n"
    "  float e = floor(log2(t)); return vec4(e, t / exp2(e), log2(t), 1.0); }\n"
    "vec4 _LIT(vec4 s) { float w = clamp(s.w, -127.996, 127.996); vec4 t = vec4(1.0, max(s.x, 0.0), 0.0, 1.0);\n"
    "  t.z = (s.x > 0.0) ? exp2(w * log2(max(s.y, 0.0))) : 0.0; return t; }\n";

/* ---------------- fixed-function vertex ---------------- */
static void gen_fixed_function(Str *s, const ShaderKey *k)
{
    /* k->ff: [0] skin mode, [1] lighting, [2] light enable mask (2 bits/light),
     *        [3..6] texgen S|T<<4|R<<8|Q<<12 per stage, [7] texmat enable bits,
     *        [8] normalize|localeye<<1|sep_spec<<2|alpha_mat_spec<<3, [9] color material,
     *        [10] fog gen mode */
    sp(s, "  vec4 tPos = position * mat4(c[8], c[9], c[10], c[11]);\n");   /* modelview0 rows */
    sp(s, "  vec3 tNorm = (vec4(normal.xyz, 0.0) * mat4(c[12], c[13], c[14], c[15])).xyz;\n");
    if (k->ff[8] & 1) sp(s, "  tNorm = normalize(tNorm);\n");
    for (int i = 0; i < 4; i++) {
        uint32_t tg = k->ff[3 + i];
        for (int j = 0; j < 4; j++) {
            int mode = (tg >> (j * 4)) & 0xF; char cc = "xyzw"[j];
            int plane = 0x40 + i * 8 + j;
            switch (mode) {
            case 0: sp(s, "  oT%d.%c = texture%d.%c;\n", i, cc, i, cc); break;
            case 1: sp(s, "  oT%d.%c = dot(c[%d], tPos);\n", i, cc, plane); break;     /* eye linear */
            case 2: sp(s, "  oT%d.%c = dot(c[%d], position);\n", i, cc, plane); break; /* object linear */
            case 3: sp(s, "  { vec3 u = normalize(tPos.xyz); vec3 r = reflect(u, tNorm); float m = 1.0/(2.0*length(r + vec3(0,0,1)));\n"
                          "    oT%d.%c = r.%c * m + 0.5; }\n", i, cc, cc); break;                 /* sphere map */
            case 4: sp(s, "  { vec3 u = normalize(tPos.xyz); oT%d.%c = reflect(u, tNorm).%c; }\n", i, cc, cc); break;
            case 5: sp(s, "  oT%d.%c = tNorm.%c;\n", i, cc, cc); break;
            default: sp(s, "  oT%d.%c = texture%d.%c;\n", i, cc, i, cc); break;
            }
        }
        if (k->ff[7] & (1 << i)) sp(s, "  oT%d = oT%d * mat4(c[%d], c[%d], c[%d], c[%d]);\n", i, i, 0x44 + i * 8, 0x45 + i * 8, 0x46 + i * 8, 0x47 + i * 8);
    }
    if (!k->ff[1]) {
        sp(s, "  oD0 = diffuse; oD1 = specular; oB0 = backDiffuse; oB1 = backSpecular;\n");
    } else {
        int cm = k->ff[9];
        int emis = cm & 3, amb = (cm >> 2) & 3, dif = (cm >> 4) & 3, spc = (cm >> 6) & 3;
        const char *src[] = { "MAT", "diffuse", "specular" };
        const char *alpha = dif == 1 ? "diffuse.a" : dif == 2 ? "specular.a" : "materialAlpha";
        if (amb == 0) sp(s, "  oD0 = vec4(ltctxa[17].xyz, %s);\n", alpha);
        else sp(s, "  oD0 = vec4(%s.rgb, %s);\n", src[amb], alpha);
        sp(s, "  oD0.rgb *= ltctxa[19].rgb;\n");
        if (emis == 0) sp(s, "  oD0.rgb += ltctxa[17].rgb;\n"); else sp(s, "  oD0.rgb += %s.rgb;\n", src[emis]);
        sp(s, "  oD1 = vec4(0.0, 0.0, 0.0, specular.a);\n");
        for (int i = 0; i < 8; i++) {
            int lt = (k->ff[2] >> (i * 2)) & 3;   /* 0 off, 1 infinite, 2 local, 3 spot */
            if (!lt) continue;
            sp(s, "  { float att = 1.0; vec3 L; float nDotHV;\n");
            if (lt == 1) {
                sp(s, "    L = normalize(lightDir[%d]); nDotHV = max(0.0, dot(tNorm, lightHalf[%d]));\n", i, i);
            } else {
                sp(s, "    vec3 VP = lightPos[%d] - tPos.xyz / tPos.w; float d = length(VP); L = normalize(VP);\n"
                      "    att = (d <= ltc1[%d].x) ? 1.0 / (lightAtt[%d].x + lightAtt[%d].y * d + lightAtt[%d].z * d * d) : 0.0;\n"
                      "    nDotHV = max(0.0, dot(tNorm, normalize(L + vec3(0.0, 0.0, 1.0))));\n", i, i, i, i, i);
                if (lt == 3) sp(s, "    { vec4 sd = ltctxa[%d]; float inv = 1.0 / length(sd.xyz); float cphi = -inv * sd.w; float cth = inv + cphi;\n"
                                   "      float rho = inv * dot(sd.xyz, L); if (rho <= cphi) att = 0.0; else if (rho <= cth) att *= dot(sd.xyz, L) + sd.w; }\n",
                                   1 + i * 2);
            }
            sp(s, "    float nDotL = max(0.0, dot(tNorm, L));\n"
                  "    float pf = (nDotL == 0.0 || nDotHV == 0.0) ? 0.0 : pow(nDotHV, specularPower);\n"
                  "    oD0.rgb += ltctxb[%d].rgb * att;\n", i * 6);
            if (dif == 0) sp(s, "    oD0.rgb += ltctxb[%d].rgb * att * nDotL;\n", i * 6 + 1);
            else sp(s, "    oD0.rgb += %s.rgb * ltctxb[%d].rgb * att * nDotL;\n", src[dif], i * 6 + 1);
            if (spc == 0) sp(s, "    oD1.rgb += ltctxb[%d].rgb * att * pf;\n", i * 6 + 2);
            else sp(s, "    oD1.rgb += %s.rgb * ltctxb[%d].rgb * att * pf;\n", src[spc], i * 6 + 2);
            sp(s, "  }\n");
        }
        sp(s, "  oB0 = backDiffuse; oB1 = backSpecular;\n");
        if (!(k->ff[8] & 4)) sp(s, "  oD0.rgb += oD1.rgb;\n");
    }
    if (k->fog_en) {
        switch (k->ff[10]) {
        case 0: sp(s, "  oFog.x = clamp(specular.a, 0.0, 1.0);\n"); break;
        case 1: sp(s, "  oFog.x = length(tPos.xyz);\n"); break;
        case 2: sp(s, "  oFog.x = dot(c[0x39].xyz, tPos.xyz) + c[0x39].w;\n"); break;
        case 3: sp(s, "  oFog.x = abs(dot(c[0x39].xyz, tPos.xyz) + c[0x39].w);\n"); break;
        default: sp(s, "  oFog.x = fogCoord;\n"); break;
        }
    }
    sp(s, "  oPos = position * mat4(c[0], c[1], c[2], c[3]);\n"
          "  oPos.w = _clampAway(oPos.w);\n"
          "  oPos.xy = oPos.xy / oPos.w + c[0x3b].xy;\n");
}

static char *gen_vertex_shader(const ShaderKey *k)
{
    Str s = { 0 };
    sp(&s, "#version 330\n"
           "uniform vec4 c[192];\n"
           "uniform vec2 surfaceSize;\n"
           "uniform vec2 clipRange;\n"
           "uniform vec4 ltctxa[26]; uniform vec4 ltctxb[52]; uniform vec4 ltc1[20];\n"
           "uniform vec3 lightHalf[8]; uniform vec3 lightDir[8]; uniform vec3 lightPos[8]; uniform vec3 lightAtt[8];\n"
           "uniform float materialAlpha; uniform float specularPower;\n"
           "uniform vec2 fogParam;\n");
    for (int i = 0; i < 16; i++) sp(&s, "in vec4 v%d;\n", i);
    sp(&s, "out vec4 vD0, vD1, vB0, vB1, vT0, vT1, vT2, vT3; out float vFog;\n");
    sp(&s, "%s", g_vp_header);
    sp(&s, "#define position v0\n#define weight v1\n#define normal v2\n#define diffuse v3\n#define specular v4\n"
           "#define fogCoord v5.x\n#define backDiffuse v7\n#define backSpecular v8\n"
           "#define texture0 v9\n#define texture1 v10\n#define texture2 v11\n#define texture3 v12\n");
    sp(&s, "void main() {\n"
           "  vec4 oPos = vec4(0,0,0,1), oD0 = vec4(0,0,0,1), oD1 = vec4(0,0,0,1), oB0 = vec4(0,0,0,1), oB1 = vec4(0,0,0,1);\n"
           "  vec4 oFog = vec4(0,0,0,1), oPts = vec4(0,0,0,1), oT0 = vec4(0,0,0,1), oT1 = vec4(0,0,0,1), oT2 = vec4(0,0,0,1), oT3 = vec4(0,0,0,1);\n"
           "  vec4 oUnk1, oUnk2, oUnk13, oUnk14, oA0;\n"
           "  vec4 R0 = vec4(0), R1 = vec4(0), R2 = vec4(0), R3 = vec4(0), R4 = vec4(0), R5 = vec4(0), R6 = vec4(0), R7 = vec4(0),\n"
           "       R8 = vec4(0), R9 = vec4(0), R10 = vec4(0), R11 = vec4(0);\n"
           "  #define R12 oPos\n"
           "  int A0 = 0; int _tmp_addr = 0; bool _has_tmp_addr = false; vec4 _mac, _ilu, _macR;\n"
           "  bool _macMask0, _macMask1, _macMask2, _macMask3, _macMask4, _macMask5, _macMask6, _macMask7, _macMask8, _macMask9, _macMask10, _macMask11, _macMask12;\n");
    if (k->vp) {
        gen_vertex_program(&s, (const uint32_t (*)[4])g_kv.program, KREG(NV097_SET_TRANSFORM_PROGRAM_START), KV_PROGRAM_LEN);
        sp(&s, "  oPos.w = _clampAway(oPos.w);\n");
    } else {
        gen_fixed_function(&s, k);
    }
    /* screen space -> GL clip space (vertex programs output post-divide xyz;
     * fixed function keeps z in clip space) */
    sp(&s, "  vec4 p = oPos;\n"
           "  p.xy = (2.0 * p.xy - surfaceSize) / surfaceSize;\n"
           "  p.z = p.z / clipRange.y;\n"
           "  %s\n", k->vp ? "p.xyz *= p.w;" : "p.xy *= p.w;");
    sp(&s, ""
           "  gl_Position = vec4(p.xy, 2.0 * p.z - p.w, p.w);\n"
           "  vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0); vB0 = clamp(oB0, 0.0, 1.0); vB1 = clamp(oB1, 0.0, 1.0);\n"
           "  vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3;\n");
    if (!k->spec_en) sp(&s, "  vD1 = vec4(0.0, 0.0, 0.0, 1.0); vB1 = vec4(0.0, 0.0, 0.0, 1.0);\n");
    if (k->fog_en) {
        switch (k->fog_mode) {
        case 0: case 4: sp(&s, "  float f = fogParam.x + oFog.x * fogParam.y - 1.0;\n"); break;   /* linear */
        case 1: case 5: sp(&s, "  float f = fogParam.x + exp2(oFog.x * fogParam.y * 16.0) - 1.5;\n"); break;
        default: sp(&s, "  float f = fogParam.x + exp2(-oFog.x * oFog.x * fogParam.y * fogParam.y * 32.0) - 1.5;\n"); break;
        }
        if (k->fog_mode >= 4) sp(&s, "  f = abs(f);\n");
        sp(&s, "  vFog = f;\n");
    } else sp(&s, "  vFog = 1.0;\n");
    sp(&s, "}\n");
    return s.p;
}

/* ---------------- combiners ---------------- */
static int g_stage;          /* current combiner stage (8 = final) */
static uint32_t g_comb_flags;

static void reg_name(Str *s, int reg, const ShaderKey *k)
{
    switch (reg) {
    case 0x0: sp(s, "vec4(0.0)"); break;
    case 0x1: sp(s, "consts[%d]", (g_comb_flags & 0x10 || g_stage == 8) ? g_stage * 2 : 0); break;
    case 0x2: sp(s, "consts[%d]", (g_comb_flags & 0x100 || g_stage == 8) ? g_stage * 2 + 1 : 1); break;
    case 0x3: sp(s, "pFog"); break;
    case 0x4: sp(s, "v0"); break;
    case 0x5: sp(s, "v1"); break;
    case 0x8: sp(s, "t0"); break;
    case 0x9: sp(s, "t1"); break;
    case 0xA: sp(s, "t2"); break;
    case 0xB: sp(s, "t3"); break;
    case 0xC: sp(s, "r0"); break;
    case 0xD: sp(s, "r1"); break;
    case 0xE: {
        uint32_t fl = k->final1 & 0xFF;
        sp(s, "%svec4(%s.rgb + %s.rgb, 0.0)%s", (fl & 0x80) ? "clamp(" : "", (fl & 0x40) ? "(1.0 - v1)" : "v1",
           (fl & 0x20) ? "(1.0 - r0)" : "r0", (fl & 0x80) ? ", 0.0, 1.0)" : "");
        break; }
    case 0xF: sp(s, "vec4(_E * _F, 0.0)"); break;
    default: sp(s, "vec4(0.0)"); break;
    }
}

static void comb_input(Str *s, int v, int alpha, const ShaderKey *k)
{
    int reg = v & 0xF, chan = v & 0x10, map = v & 0xE0;
    Str r = { 0 };
    reg_name(&r, reg, k);
    if (!alpha) sp(&r, chan ? ".aaa" : ".rgb"); else sp(&r, chan ? ".a" : ".b");
    switch (map) {
    case 0x00: sp(s, "max(%s, 0.0)", r.p); break;
    case 0x20: sp(s, "(1.0 - clamp(%s, 0.0, 1.0))", r.p); break;
    case 0x40: sp(s, "(2.0 * max(%s, 0.0) - 1.0)", r.p); break;
    case 0x60: sp(s, "(-2.0 * max(%s, 0.0) + 1.0)", r.p); break;
    case 0x80: sp(s, "(max(%s, 0.0) - 0.5)", r.p); break;
    case 0xA0: sp(s, "(-max(%s, 0.0) + 0.5)", r.p); break;
    case 0xC0: sp(s, "(%s)", r.p); break;
    default: sp(s, "(-%s)", r.p); break;
    }
    free(r.p);
}

static const char *dst_name(int reg)
{
    switch (reg) {
    case 0x4: return "v0"; case 0x5: return "v1"; case 0x8: return "t0"; case 0x9: return "t1";
    case 0xA: return "t2"; case 0xB: return "t3"; case 0xC: return "r0"; case 0xD: return "r1";
    default: return NULL;
    }
}

static void out_map(Str *s, const char *x, int mapping)
{
    switch (mapping) {
    case 0x08: sp(s, "(%s - 0.5)", x); break;
    case 0x10: sp(s, "(%s * 2.0)", x); break;
    case 0x18: sp(s, "((%s - 0.5) * 2.0)", x); break;
    case 0x20: sp(s, "(%s * 4.0)", x); break;
    case 0x30: sp(s, "(%s / 2.0)", x); break;
    default: sp(s, "(%s)", x); break;
    }
}

static void comb_stage(Str *s, uint32_t in, uint32_t out, int alpha, const ShaderKey *k)
{
    Str a = { 0 }, b = { 0 }, c = { 0 }, d = { 0 };
    comb_input(&a, (in >> 24) & 0xFF, alpha, k); comb_input(&b, (in >> 16) & 0xFF, alpha, k);
    comb_input(&c, (in >> 8) & 0xFF, alpha, k); comb_input(&d, in & 0xFF, alpha, k);
    int cd_dst = out & 0xF, ab_dst = (out >> 4) & 0xF, sum_dst = (out >> 8) & 0xF;
    int flags = out >> 12;
    int cd_dot = flags & 1, ab_dot = flags & 2, mux = flags & 4, mapping = flags & 0x38;
    int ab_b2a = flags & 0x80, cd_b2a = flags & 0x40;
    const char *ty = alpha ? "float" : "vec3", *m = alpha ? "a" : "rgb";
    sp(s, "  { %s A = %s, B = %s, C = %s, D = %s;\n", ty, a.p, b.p, c.p, d.p);
    if (ab_dot && !alpha) sp(s, "    %s ab = vec3(dot(A, B));\n", ty); else sp(s, "    %s ab = A * B;\n", ty);
    if (cd_dot && !alpha) sp(s, "    %s cd = vec3(dot(C, D));\n", ty); else sp(s, "    %s cd = C * D;\n", ty);
    if (mux) sp(s, "    %s ms = %s ? cd : ab;\n", ty, (g_comb_flags & 1) ? "(r0.a >= 0.5)" : "((uint(r0.a * 255.0) & 1u) == 1u)");
    else sp(s, "    %s ms = ab + cd;\n", ty);
    Str o = { 0 };
    const char *dn;
    if ((dn = dst_name(ab_dst))) {
        free(o.p); o.p = NULL; o.n = o.cap = 0; out_map(&o, "ab", mapping);
        sp(s, "    _ab = clamp(%s, -1.0, 1.0);\n", o.p);
    }
    if ((dn = dst_name(cd_dst))) {
        free(o.p); o.p = NULL; o.n = o.cap = 0; out_map(&o, "cd", mapping);
        sp(s, "    _cd = clamp(%s, -1.0, 1.0);\n", o.p);
    }
    if ((dn = dst_name(sum_dst))) {
        free(o.p); o.p = NULL; o.n = o.cap = 0; out_map(&o, "ms", mapping);
        sp(s, "    _ms = clamp(%s, -1.0, 1.0);\n", o.p);
    }
    sp(s, "  }\n");
    /* writes happen after all reads of this half-stage */
    if ((dn = dst_name(ab_dst))) { sp(s, "  %s.%s = _ab;\n", dn, m); if (!alpha && ab_b2a) sp(s, "  %s.a = _ab.b;\n", dn); }
    if ((dn = dst_name(cd_dst))) { sp(s, "  %s.%s = _cd;\n", dn, m); if (!alpha && cd_b2a) sp(s, "  %s.a = _cd.b;\n", dn); }
    if ((dn = dst_name(sum_dst))) sp(s, "  %s.%s = _ms;\n", dn, m);
    free(a.p); free(b.p); free(c.p); free(d.p); free(o.p);
}

static char *gen_fragment_shader(const ShaderKey *k)
{
    Str s = { 0 };
    sp(&s, "#version 330\n"
           "in vec4 vD0, vD1, vB0, vB1, vT0, vT1, vT2, vT3; in float vFog;\n"
           "out vec4 fragColor;\n"
           "uniform vec4 consts[18];\n"
           "uniform vec4 fogColor;\n"
           "uniform float alphaRef;\n"
           "uniform vec4 texScale[4];\n"
           "uniform vec4 bumpMat[4]; uniform vec4 bumpScaleOffset[4];\n");
    for (int i = 0; i < 4; i++) {
        if (k->tex_type[i] == 3) sp(&s, "uniform samplerCube tex%d;\n", i);
        else if (k->tex_type[i]) sp(&s, "uniform sampler2D tex%d;\n", i);
    }
    if (k->blend_signed) sp(&s, "uniform sampler2D dstTex;\n");
    sp(&s, "void main() {\n"
           "  vec4 v0 = vD0, v1 = vD1;\n"
           "  vec4 pFog = vec4(fogColor.rgb, clamp(vFog, 0.0, 1.0));\n"
           "  vec4 t0 = vec4(0), t1 = vec4(0), t2 = vec4(0), t3 = vec4(0);\n"
           "  vec3 _ab3, _cd3, _ms3; float _abA, _cdA, _msA;\n");
    /* texture shader stages */
    const char *tc[4] = { "vT0", "vT1", "vT2", "vT3" };
    for (int i = 0; i < 4; i++) {
        int mode = (k->shader_prog >> (i * 5)) & 0x1F;
        int ty = k->tex_type[i];
        if (!mode) continue;
        if (!ty && mode != 4 && mode != 5) { sp(&s, "  t%d = vec4(0.0);\n", i); continue; }
        switch (mode) {
        case 1: /* PROJECT2D */
        case 2: /* PROJECT3D (approximated as 2D) */
            if (ty == 3) sp(&s, "  t%d = texture(tex%d, %s.xyz);\n", i, i, tc[i]);
            else if (ty == 2) sp(&s, "  t%d = texture(tex%d, (%s.xy / %s.w) / texScale[%d].xy);\n", i, i, tc[i], tc[i], i);
            else sp(&s, "  t%d = textureProj(tex%d, %s.xyw);\n", i, i, tc[i]);
            break;
        case 3: /* CUBEMAP */
            if (ty == 3) sp(&s, "  t%d = texture(tex%d, %s.xyz);\n", i, i, tc[i]);
            else sp(&s, "  t%d = texture(tex%d, %s.xy);\n", i, i, tc[i]);
            break;
        case 4: sp(&s, "  t%d = clamp(%s, 0.0, 1.0);\n", i, tc[i]); break;             /* PASSTHRU */
        case 5: sp(&s, "  if (any(lessThan(%s, vec4(0.0)))) discard;\n", tc[i]); break; /* CLIPPLANE (approx) */
        case 6: case 7: { /* BUMPENVMAP(_LUM): offset by previous stage's du/dv */
            int src = i == 1 ? 0 : (int)((k->other_input >> (16 + (i - 2) * 4)) & 0xF);
            if (i == 1) src = 0;
            sp(&s, "  { vec2 dd = t%d.rg * 2.0 - 1.0; vec2 uv = %s.xy + vec2(dot(dd, bumpMat[%d].xz), dot(dd, bumpMat[%d].yw));\n",
               src, tc[i], i, i);
            if (ty == 2) sp(&s, "    t%d = texture(tex%d, uv / texScale[%d].xy);\n", i, i, i);
            else sp(&s, "    t%d = texture(tex%d, uv);\n", i, i);
            if (mode == 7) sp(&s, "    t%d.rgb *= clamp(t%d.b * bumpScaleOffset[%d].x + bumpScaleOffset[%d].y, 0.0, 1.0);\n", i, src, i, i);
            sp(&s, "  }\n");
            break; }
        default:
            { static int warned[32]; if (!warned[mode]++) xlog("shaders: texture mode %d not implemented (stage %d)", mode, i); }
            if (ty == 3) sp(&s, "  t%d = texture(tex%d, %s.xyz);\n", i, i, tc[i]);
            else if (ty == 2) sp(&s, "  t%d = texture(tex%d, %s.xy / texScale[%d].xy);\n", i, i, tc[i], i);
            else sp(&s, "  t%d = texture(tex%d, %s.xy);\n", i, i, tc[i]);
            break;
        }
        if (k->ctl0[i] & 4) sp(&s, "  if (t%d.a == 0.0) discard;\n", i);   /* alpha kill */
    }
    int mode0 = k->shader_prog & 0x1F;
    sp(&s, "  vec4 r0 = vec4(0.0, 0.0, 0.0, %s), r1 = vec4(0.0);\n", mode0 ? "t0.a" : "1.0");
    sp(&s, "  vec3 _E = vec3(0.0), _F = vec3(0.0);\n");
    int nst = k->comb_ctl & 0xFF;
    g_comb_flags = k->comb_ctl >> 8;
    for (int i = 0; i < nst && i < 8; i++) {
        g_stage = i;
        sp(&s, "  // combiner stage %d\n  { vec3 _ab, _cd, _ms;\n", i);
        comb_stage(&s, k->rgb_in[i], k->rgb_out[i], 0, k);
        sp(&s, "  }\n  { float _ab, _cd, _ms;\n");
        comb_stage(&s, k->a_in[i], k->a_out[i], 1, k);
        sp(&s, "  }\n");
    }
    g_stage = 8;
    if (k->final0 || k->final1) {
        Str A = { 0 }, B = { 0 }, C = { 0 }, D = { 0 }, E = { 0 }, Fv = { 0 }, G = { 0 };
        comb_input(&E, (k->final1 >> 24) & 0xFF, 0, k); comb_input(&Fv, (k->final1 >> 16) & 0xFF, 0, k);
        sp(&s, "  _E = %s; _F = %s;\n", E.p, Fv.p);
        comb_input(&A, (k->final0 >> 24) & 0xFF, 0, k); comb_input(&B, (k->final0 >> 16) & 0xFF, 0, k);
        comb_input(&C, (k->final0 >> 8) & 0xFF, 0, k); comb_input(&D, k->final0 & 0xFF, 0, k);
        comb_input(&G, (k->final1 >> 8) & 0xFF, 1, k);
        sp(&s, "  fragColor.rgb = %s + mix(vec3(%s), vec3(%s), vec3(%s));\n", D.p, C.p, B.p, A.p);
        sp(&s, "  fragColor.a = %s;\n", G.p);
        free(A.p); free(B.p); free(C.p); free(D.p); free(E.p); free(Fv.p); free(G.p);
    } else {
        sp(&s, "  fragColor = r0;\n");
    }
    sp(&s, "  fragColor = clamp(fragColor, 0.0, 1.0);\n");
    if (k->alpha_test && k->alpha_func != 7) {
        static const char *ops[] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
        if (k->alpha_func == 0) sp(&s, "  discard;\n");
        else sp(&s, "  if (!(round(fragColor.a * 255.0) %s alphaRef)) discard;\n", ops[k->alpha_func]);
    }
    if (k->blend_signed)   /* 8-bit add/subtract with wrap-around; the blend factors play no part */
        sp(&s, "  { vec4 d8 = floor(texelFetch(dstTex, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5), s8 = floor(fragColor * 255.0 + 0.5);\n"
               "    fragColor = mod(d8 %c s8, 256.0) / 255.0; }\n", k->blend_signed == 1 ? '+' : '-');
    sp(&s, "}\n");
    return s.p;
}

/* ---------------- key + cache ---------------- */
static void build_key(ShaderKey *k, const BoundTex tex[4])
{
    memset(k, 0, sizeof *k);
    k->vp = (KREG(NV097_SET_TRANSFORM_EXECUTION_MODE) & 3) == 2;
    if (k->vp) {
        int start = KREG(NV097_SET_TRANSFORM_PROGRAM_START), len = 0;
        for (int i = start; i < KV_PROGRAM_LEN; i++) { len++; if (g_kv.program[i][3] & 1) break; }
        k->vp_len = len;
        k->vp_hash = hash32(g_kv.program[start], len * 16) ^ start;
    } else {
        k->ff[0] = KREG(NV097_SET_SKIN_MODE);
        k->ff[1] = KREG(NV097_SET_LIGHTING_ENABLE) & 1;
        k->ff[2] = k->ff[1] ? KREG(NV097_SET_LIGHT_ENABLE_MASK) : 0;
        for (int i = 0; i < 4; i++) {
            static const int remap[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            (void)remap;
            uint32_t S = KREG(NV097_SET_TEXGEN_S + i * 16), T = KREG(NV097_SET_TEXGEN_T + i * 16);
            uint32_t Rr = KREG(NV097_SET_TEXGEN_R + i * 16), Q = KREG(NV097_SET_TEXGEN_Q + i * 16);
            /* GL enum values -> small codes */
            #define TG(v) ((v) == 0x2400 ? 1 : (v) == 0x2401 ? 2 : (v) == 0x2402 ? 3 : (v) == 0x8512 ? 4 : (v) == 0x8511 ? 5 : 0)
            k->ff[3 + i] = TG(S) | (TG(T) << 4) | (TG(Rr) << 8) | (TG(Q) << 12);
            #undef TG
            if (g_kv.texture_matrix_enable[i]) k->ff[7] |= 1 << i;
        }
        uint32_t lc = KREG(NV097_SET_LIGHT_CONTROL);
        k->ff[8] = (KREG(NV097_SET_NORMALIZATION_ENABLE) & 1) | ((lc >> 16 & 1) << 1) | ((lc & 2) ? 4 : 0) | ((lc >> 17 & 1) << 3);
        k->ff[9] = KREG(NV097_SET_COLOR_MATERIAL);
        uint32_t fg = KREG(NV097_SET_FOG_GEN_MODE);
        k->ff[10] = fg == 0x8452 ? 4 : fg == 1 ? 1 : fg == 2 ? 2 : fg == 3 ? 3 : 0;
    }
    k->spec_en = KREG(NV097_SET_SPECULAR_ENABLE) & 1;
    k->fog_en = KREG(NV097_SET_FOG_ENABLE) & 1;
    if (k->fog_en) {
        uint32_t fm = KREG(NV097_SET_FOG_MODE);
        k->fog_mode = fm == 0x2601 ? 0 : fm == 0x800 ? 1 : fm == 0x801 ? 2 : fm == 0x802 ? 5 : fm == 0x803 ? 6 : fm == 0x804 ? 4 : 0;
    }
    k->comb_ctl = KREG(NV097_SET_COMBINER_CONTROL);
    k->shader_prog = KREG(NV097_SET_SHADER_STAGE_PROGRAM);
    k->other_input = KREG(NV097_SET_SHADER_OTHER_STAGE_INPUT);
    k->final0 = KREG(NV097_SET_COMBINER_SPECULAR_FOG_CW0);
    k->final1 = KREG(NV097_SET_COMBINER_SPECULAR_FOG_CW1);
    int nst = k->comb_ctl & 0xFF;
    for (int i = 0; i < nst && i < 8; i++) {
        k->rgb_in[i] = KREG(NV097_SET_COMBINER_COLOR_ICW + i * 4);
        k->rgb_out[i] = KREG(NV097_SET_COMBINER_COLOR_OCW + i * 4);
        k->a_in[i] = KREG(NV097_SET_COMBINER_ALPHA_ICW + i * 4);
        k->a_out[i] = KREG(NV097_SET_COMBINER_ALPHA_OCW + i * 4);
    }
    for (int i = 0; i < 4; i++) {
        if (tex[i].tex) k->tex_type[i] = tex[i].target == GL_TEXTURE_CUBE_MAP ? 3 : tex[i].rect ? 2 : 1;
        k->ctl0[i] = KREG(NV097_SET_TEXTURE_OFFSET + i * 0x40 + 0x0C) & 4;
    }
    k->alpha_test = KREG(NV097_SET_ALPHA_TEST_ENABLE) & 1;
    if (k->alpha_test) { uint32_t f = KREG(NV097_SET_ALPHA_FUNC); k->alpha_func = (f >= 0x200 && f <= 0x207) ? f - 0x200 : 7; }
    if (KREG(NV097_SET_BLEND_ENABLE) & 1) {
        uint32_t eq = KREG(NV097_SET_BLEND_EQUATION);
        k->blend_signed = eq == 0xF006 ? 1 : eq == 0xF005 ? 2 : 0;
    }
}

ShaderProg *shaders_get(const BoundTex tex[4], uint32_t compressed_attrs)
{
    ShaderKey k; build_key(&k, tex);
    uint32_t h = hash32(&k, sizeof k) & 511;
    for (CacheEnt *e = g_cache[h]; e; e = e->next) if (!memcmp(&e->key, &k, sizeof k)) return e->prog.prog ? &e->prog : NULL;
    CacheEnt *e = calloc(1, sizeof *e);
    e->key = k; e->next = g_cache[h]; g_cache[h] = e;
    char *vs = gen_vertex_shader(&k), *fs = gen_fragment_shader(&k);
    GLuint p = gl_compile_program(vs, fs);
    static int nprog;
    if (p) {
        ShaderProg *sp_ = &e->prog;
        sp_->prog = p;
        glUseProgram(p);
        sp_->u_c = glGetUniformLocation(p, "c");
        sp_->u_surface = glGetUniformLocation(p, "surfaceSize");
        sp_->u_clip = glGetUniformLocation(p, "clipRange");
        sp_->u_texscale = glGetUniformLocation(p, "texScale");
        sp_->u_consts = glGetUniformLocation(p, "consts");
        sp_->u_alpharef = glGetUniformLocation(p, "alphaRef");
        sp_->u_fogcolor = glGetUniformLocation(p, "fogColor");
        sp_->u_bumpmat = glGetUniformLocation(p, "bumpMat");
        sp_->u_bumpscale = glGetUniformLocation(p, "bumpScaleOffset");
        sp_->u_material_alpha = glGetUniformLocation(p, "materialAlpha");
        sp_->u_specpow = glGetUniformLocation(p, "specularPower");
        sp_->u_ltctxa = glGetUniformLocation(p, "ltctxa");
        sp_->u_ltctxb = glGetUniformLocation(p, "ltctxb");
        sp_->u_ltc1 = glGetUniformLocation(p, "ltc1");
        sp_->u_lhalf = glGetUniformLocation(p, "lightHalf");
        sp_->u_ldir = glGetUniformLocation(p, "lightDir");
        sp_->u_lpos = glGetUniformLocation(p, "lightPos");
        sp_->u_latt = glGetUniformLocation(p, "lightAtt");
        sp_->u_fogparam = glGetUniformLocation(p, "fogParam");
        for (int i = 0; i < 4; i++) { char n[8]; sprintf(n, "tex%d", i); GLint l = glGetUniformLocation(p, n); if (l >= 0) glUniform1i(l, i); }
        { GLint l = glGetUniformLocation(p, "dstTex"); if (l >= 0) glUniform1i(l, 4); }
        if (++nprog <= 200 || nprog % 100 == 0) xlog("shaders: compiled program #%d (%s)", nprog, k.vp ? "vertex program" : "fixed function");
    }
    free(vs); free(fs);
    return p ? &e->prog : NULL;
}
