/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OpenGL renderer for the NV2A (Kelvin) 3D engine. */
#pragma once
#include "../nv2a/kelvin.h"
#include <GL/gl.h>
#include <GL/glext.h>

#define GLF(ret, name, args) extern ret (APIENTRY *p_##name) args;
#include "glfuncs.inc"
#undef GLF
#define glGenFramebuffers p_glGenFramebuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glBlitFramebuffer p_glBlitFramebuffer
#define glDrawBuffers p_glDrawBuffers
#define glGenBuffers p_glGenBuffers
#define glDeleteBuffers p_glDeleteBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glVertexAttrib4fv p_glVertexAttrib4fv
#define glCreateShader p_glCreateShader
#define glDeleteShader p_glDeleteShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glBindFragDataLocation p_glBindFragDataLocation
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform2f p_glUniform2f
#define glUniform4f p_glUniform4f
#define glUniform2fv p_glUniform2fv
#define glUniform3fv p_glUniform3fv
#define glUniform4fv p_glUniform4fv
#define glUniform4iv p_glUniform4iv
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glActiveTexture p_glActiveTexture
#define glCompressedTexImage2D p_glCompressedTexImage2D
#define glGenerateMipmap p_glGenerateMipmap
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlendEquation p_glBlendEquation
#define glBlendColor p_glBlendColor
#define glStencilOpSeparate p_glStencilOpSeparate
#define glStencilFuncSeparate p_glStencilFuncSeparate
#define glClearBufferfv p_glClearBufferfv
#define glClearBufferfi p_glClearBufferfi
#define glClearBufferiv p_glClearBufferiv
#define glGenSamplers p_glGenSamplers
#define glBindSampler p_glBindSampler
#define glSamplerParameteri p_glSamplerParameteri
#define glSamplerParameterfv p_glSamplerParameterfv
#define glDrawRangeElements p_glDrawRangeElements
#define glTexImage3D p_glTexImage3D
#define glMapBufferRange p_glMapBufferRange
#define glUnmapBuffer p_glUnmapBuffer
#define glFlushMappedBufferRange p_glFlushMappedBufferRange
#define glDebugMessageCallback p_glDebugMessageCallback

/* ---- configuration ---- */
extern int g_scale;            /* internal resolution multiplier */
extern int g_window_w, g_window_h;
extern char g_screenshot_dir[MAX_PATH];
extern int g_screenshot_every; /* frames; 0 = off */

/* ---- glctx.c ---- */
void gl_check(const char *where);
GLuint gl_compile_program(const char *vs, const char *fs);
void gl_present_texture(GLuint tex, int w, int h, int flip_y);

/* ---- surface.c ---- */
typedef struct Surface {
    uint32_t pa, pitch, w, h;        /* unscaled */
    uint32_t fmt;                    /* NV097 surface color format, or 0x100|zeta format */
    int      zeta, swizzled;
    GLuint   tex;
    uint64_t frame_used;
    int      has_alpha;
    uint32_t serial;                 /* GPU write order (see surface_written) */
    GLuint   view; uint32_t view_serial;   /* zeta: its memory read as A8R8G8B8, for sampling */
    struct Surface *next;
} Surface;
typedef struct {
    Surface *color, *zeta;
    GLuint   fbo;
    uint32_t w, h;                   /* clip size (unscaled) */
    uint32_t clip_x, clip_y;
} RenderTarget;
extern RenderTarget g_rt;
void surface_init(void);
void surface_bind_current(void);     /* make Kelvin's current surface the GL draw target */
Surface *surface_find(uint32_t pa);  /* color surface containing pa (exact start) */
Surface *surface_find_texture(uint32_t pa, uint32_t w, uint32_t h);
GLuint surface_texture_view(Surface *s);   /* GL texture holding what a texture fetch of s's memory sees */
void surface_written(Surface *s);    /* s was just rendered to */
void surface_upload_from_ram(Surface *s);
void surface_download_to_ram(Surface *s);
extern uint64_t g_frame_no;

/* ---- texture.c ---- */
typedef struct {
    GLenum   target;                 /* GL_TEXTURE_2D / GL_TEXTURE_CUBE_MAP / GL_TEXTURE_3D */
    GLuint   tex;
    int      rect;                   /* linear (non-normalized coords) */
    float    width, height;          /* for rect scaling */
    int      signed_rgb;
} BoundTex;
void texture_init(void);
int  texture_bind_stage(int stage, BoundTex *out);   /* returns 0 if stage disabled */
void texture_frame_end(void);

/* ---- shaders.c ---- */
typedef struct {
    GLuint prog;
    GLint  u_c, u_surface, u_clip, u_texscale, u_consts, u_alpharef, u_fogcolor, u_clipregion;
    GLint  u_bumpmat, u_bumpscale, u_bumpoffset, u_material_alpha, u_specpow;
    GLint  u_ltctxa, u_ltctxb, u_ltc1, u_lhalf, u_ldir, u_lpos, u_latt, u_fogparam, u_pointparams;
} ShaderProg;
void shaders_init(void);
ShaderProg *shaders_get(const BoundTex tex[4], uint32_t compressed_attrs);

/* ---- draw.c ---- */
void draw_init(void);

/* ---- capture.c (renderer debugging) ---- */
void capture_arm(void);
int  capture_active(void);
void capture_frame_marker(void);
void capture_before_draw(void);
void capture_after_draw(const KvDraw *d, const BoundTex tex[4], int vp);
void capture_note(const char *fmt, ...);
