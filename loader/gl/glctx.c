/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Window, OpenGL context, function loading and presentation.
 *
 * A UI thread owns the window and pumps messages; the GPU thread owns the
 * GL context (created on the window's DC) and does all rendering. */
#include "gl.h"
#include "../settings.h"
#include "../hle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define GLF(ret, name, args) ret (APIENTRY *p_##name) args;
#include "glfuncs.inc"
#undef GLF

int g_scale = 2;
int g_window_w = 1280, g_window_h = 960;
int g_scale_user, g_window_user;     /* set on the command line; otherwise derived from the monitor */
char g_screenshot_dir[MAX_PATH];
int g_screenshot_every;
static int g_auto_scale = 2;          /* internal resolution matching the monitor */
int gl_auto_scale(void) { return g_auto_scale; }
#define WM_APP_FULLSCREEN (WM_APP + 1)
void overlay_frame(int ww, int wh);

static HWND g_hwnd;
static HDC g_hdc;
static HGLRC g_ctx;
static HANDLE g_window_ready;
static volatile int g_shot_req;         /* F12 */
extern int g_keyboard_window_focus;
volatile int g_quit_requested;

/* borderless fullscreen on the window's monitor (UI thread only) */
static void apply_fullscreen(HWND h, int on)
{
    static RECT saved; static LONG style; static int cur;
    if (on == cur) return;
    cur = on;
    if (on) {
        GetWindowRect(h, &saved); style = GetWindowLongA(h, GWL_STYLE);
        MONITORINFO mi = { sizeof mi }; GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(h, GWL_STYLE, style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top, SWP_FRAMECHANGED);
    }
}
void gl_set_fullscreen(int on) { if (g_hwnd) PostMessageA(g_hwnd, WM_APP_FULLSCREEN, (WPARAM)on, 0); }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE: g_quit_requested = 1; settings_save(); ExitProcess(0); return 0;
    case WM_ACTIVATE: g_keyboard_window_focus = LOWORD(w) != WA_INACTIVE; break;
    case WM_SIZE: g_window_w = LOWORD(l); g_window_h = HIWORD(l); break;
    case WM_KEYDOWN:
        if (w == VK_F12) g_shot_req = 1;
        if (!(l & (1 << 30))) input_menu_key((int)w);      /* first press, not auto-repeat */
        break;
    case WM_APP_FULLSCREEN: apply_fullscreen(h, (int)w); return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {   /* Alt+Enter: toggle borderless fullscreen */
            g_set.fullscreen = !g_set.fullscreen;
            apply_fullscreen(h, g_set.fullscreen);
            settings_save();
            return 0;
        }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI ui_thread(LPVOID p)
{
    WNDCLASSA wc = { 0 };
    wc.style = CS_OWNDC; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW); wc.lpszClassName = "JSRFWindow";
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    if (!wc.hIcon) wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    /* Size from the primary monitor unless given on the command line: a 4:3
     * window three quarters of the work area tall, and an internal resolution
     * that matches the monitor (480 lines per step: 1080p -> 2x, 1440p -> 3x,
     * 4K -> 4x) so fullscreen stays sharp. */
    MONITORINFO mi = { sizeof mi };
    if (GetMonitorInfoA(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        int work_w = mi.rcWork.right - mi.rcWork.left, work_h = mi.rcWork.bottom - mi.rcWork.top;
        if (!g_window_user) { g_window_h = work_h * 3 / 4; g_window_w = g_window_h * 4 / 3; }
        g_auto_scale = (mon_h + 240) / 480; if (g_auto_scale < 1) g_auto_scale = 1; if (g_auto_scale > 4) g_auto_scale = 4;
        if (!g_scale_user) g_scale = g_set.scale ? g_set.scale : g_auto_scale;
        int maxh = work_h * 9 / 10 - GetSystemMetrics(SM_CYCAPTION), maxw = work_w * 9 / 10;
        if (g_window_h > maxh) { g_window_h = maxh; g_window_w = maxh * 4 / 3; }
        if (g_window_w > maxw) { g_window_w = maxw; g_window_h = maxw * 3 / 4; }
        xlog("display: monitor %dx%d, window %dx%d, internal resolution %dx%d", (int)(mi.rcMonitor.right - mi.rcMonitor.left), mon_h,
             g_window_w, g_window_h, 640 * g_scale, 480 * g_scale);
    }
    RECT r = { 0, 0, g_window_w, g_window_h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("JSRFWindow", "Jet Set Radio Future  \xB7  F1: Settings", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (g_set.fullscreen) apply_fullscreen(g_hwnd, 1);
    SetEvent(g_window_ready);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    ExitProcess(0);
    return 0;
}

typedef HGLRC (WINAPI *PFN_CreateContextAttribs)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_SwapInterval)(int);
static PFN_SwapInterval g_swap_interval;
void gl_set_vsync(int on) { if (g_swap_interval) g_swap_interval(on ? 1 : 0); }   /* GPU thread */

static void *load(const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        static HMODULE gl32; if (!gl32) gl32 = LoadLibraryA("opengl32.dll");
        p = (void *)GetProcAddress(gl32, name);
    }
    return p;
}

/* glGetError forces a round trip to the driver thread on drivers with
 * threaded optimisation (NVIDIA), so per-call checks are only done with
 * --gl-debug; otherwise errors are collected once per frame. */
int g_gl_debug;
static void gl_check_now(const char *where)
{
    GLenum e = glGetError();
    if (e) { static int n; if (n++ < 50) xlog("GL error 0x%04x at %s", e, where); }
}
void gl_check(const char *where) { if (g_gl_debug) gl_check_now(where); }

void gpu_thread_init(void)
{
    g_window_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    host_create_thread(0, ui_thread, NULL, 0, NULL);
    WaitForSingleObject(g_window_ready, INFINITE);
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA,
                                  32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    SetPixelFormat(g_hdc, ChoosePixelFormat(g_hdc, &pfd), &pfd);
    HGLRC tmp = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, tmp);
    PFN_CreateContextAttribs cca = (PFN_CreateContextAttribs)wglGetProcAddress("wglCreateContextAttribsARB");
    int attrs[] = { 0x2091, 3, 0x2092, 3, 0x9126, 1, 0 };  /* 3.3 core */
    g_ctx = cca ? cca(g_hdc, NULL, attrs) : NULL;
    if (g_ctx) { wglMakeCurrent(g_hdc, g_ctx); wglDeleteContext(tmp); }
    else { g_ctx = tmp; xlog("gl: WARNING no 3.3 core context; using legacy context"); }
    g_swap_interval = (PFN_SwapInterval)wglGetProcAddress("wglSwapIntervalEXT");
    gl_set_vsync(g_set.vsync);   /* off by default: pacing comes from the emulated vblank */
#define GLF(ret, name, args) p_##name = (ret (APIENTRY *) args)load(#name); \
    if (!p_##name && strcmp(#name, "glDebugMessageCallback")) xfatal("OpenGL function %s missing (need OpenGL 3.3)", #name);
#include "glfuncs.inc"
#undef GLF
    xlog("gl: %s / %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER), glGetString(GL_VENDOR));
    surface_init();
    texture_init();
    shaders_init();
    draw_init();
}

GLuint gl_compile_program(const char *vs, const char *fs)
{
    GLuint s[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
    const char *src[2] = { vs, fs };
    for (int i = 0; i < 2; i++) {
        glShaderSource(s[i], 1, &src[i], NULL);
        glCompileShader(s[i]);
        GLint ok; glGetShaderiv(s[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096]; glGetShaderInfoLog(s[i], sizeof log, NULL, log);
            xlog("gl: %s shader compile failed:\n%s\n---- source ----\n%s", i ? "fragment" : "vertex", log, src[i]);
            return 0;
        }
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, s[0]); glAttachShader(p, s[1]);
    for (int i = 0; i < 16; i++) { char n[8]; sprintf(n, "v%d", i); glBindAttribLocation(p, i, n); }
    glBindFragDataLocation(p, 0, "fragColor");
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log); xlog("gl: link failed: %s", log); return 0; }
    glDeleteShader(s[0]); glDeleteShader(s[1]);
    return p;
}

/* ---------------- presentation ---------------- */
static GLuint g_blit_prog, g_blit_vao, g_gamma_tex;
static GLint g_blit_flip, g_blit_gamma_on, g_blit_gamma_exp;
static LONG g_gamma_seen = -1;
static int g_gamma_identity = 1;

static void save_screenshot(int w, int h)
{
    static int n;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\shot_%05d.bmp", g_screenshot_dir, n++);
    uint8_t *px = malloc(w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        int row = (w * 3 + 3) & ~3;
        BITMAPFILEHEADER fh = { 0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + row * h, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER) };
        BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 24, BI_RGB, row * h, 2835, 2835, 0, 0 };
        fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
        uint8_t pad[3] = { 0 };
        for (int y = 0; y < h; y++) { fwrite(px + y * w * 3, 1, w * 3, f); fwrite(pad, 1, row - w * 3, f); }
        fclose(f);
        xlog("screenshot: %s", path);
    }
    free(px);
}

void gl_present_texture(GLuint tex, int w, int h, int flip_y)
{
    if (!g_blit_prog) {
        g_blit_prog = gl_compile_program(
            "#version 330\nout vec2 uv; uniform int flipY;\n"
            "void main(){ vec2 p = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID >> 1) * 2.0);\n"
            "  uv = p; if (flipY != 0) uv.y = 1.0 - uv.y; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n",
            "#version 330\nin vec2 uv; out vec4 fragColor; uniform sampler2D tex; uniform sampler2D gammaLut; uniform int gammaOn;\n"
            "uniform float gammaExp;\n"
            "void main(){ vec3 c = texture(tex, uv).rgb;\n"
            "  if (gammaOn != 0) { ivec3 i = ivec3(clamp(c, 0.0, 1.0) * 255.0 + 0.5);\n"
            "    c = vec3(texelFetch(gammaLut, ivec2(i.r, 0), 0).r, texelFetch(gammaLut, ivec2(i.g, 0), 0).g, texelFetch(gammaLut, ivec2(i.b, 0), 0).b); }\n"
            "  if (gammaExp != 1.0) c = pow(max(c, vec3(0.0)), vec3(gammaExp));\n"
            "  fragColor = vec4(c, 1.0); }\n");
        g_blit_flip = glGetUniformLocation(g_blit_prog, "flipY");
        g_blit_gamma_on = glGetUniformLocation(g_blit_prog, "gammaOn");
        g_blit_gamma_exp = glGetUniformLocation(g_blit_prog, "gammaExp");
        glUseProgram(g_blit_prog);
        glUniform1i(glGetUniformLocation(g_blit_prog, "tex"), 0);
        glUniform1i(glGetUniformLocation(g_blit_prog, "gammaLut"), 1);
        glGenVertexArrays(1, &g_blit_vao);
        glGenTextures(1, &g_gamma_tex);
    }
    LONG gen = g_gamma_gen;
    if (gen != g_gamma_seen) {          /* the title loaded a new gamma ramp into the DAC */
        uint8_t lut[256 * 4];
        g_gamma_identity = 1;
        for (int i = 0; i < 256; i++) {
            lut[i * 4 + 0] = g_gamma[0][i]; lut[i * 4 + 1] = g_gamma[1][i]; lut[i * 4 + 2] = g_gamma[2][i]; lut[i * 4 + 3] = 255;
            if (g_gamma[0][i] != i || g_gamma[1][i] != i || g_gamma[2][i] != i) g_gamma_identity = 0;
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_gamma_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glActiveTexture(GL_TEXTURE0);
        g_gamma_seen = gen;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    int ww = g_window_w, wh = g_window_h;
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (tex) {
        int vw = ww, vh = wh;
        if (g_set.aspect == 0) {                 /* 4:3 with bars */
            vh = ww * 3 / 4;
            if (vh > wh) { vh = wh; vw = wh * 4 / 3; }
        }
        glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
        glUseProgram(g_blit_prog);
        glUniform1i(g_blit_flip, flip_y);
        glUniform1i(g_blit_gamma_on, !g_gamma_identity);
        glUniform1f(g_blit_gamma_exp, 100.0f / (float)(g_set.brightness > 0 ? g_set.brightness : 100));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_gamma_tex);
        glBindSampler(1, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(0, 0);
        GLenum filt = g_set.filter ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glBindVertexArray(g_blit_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    /* screenshots leave the menu and counters out (JSRF_SHOT_OVERLAY=1 keeps them, for testing) */
    static int frames, shot_overlay = -1;
    if (shot_overlay < 0) shot_overlay = getenv("JSRF_SHOT_OVERLAY") != NULL;
    frames++;
    int shot = (g_screenshot_every && frames % g_screenshot_every == 0) || g_shot_req;
    if (shot && !shot_overlay) { g_shot_req = 0; glViewport(0, 0, ww, wh); save_screenshot(ww, wh); }
    overlay_frame(ww, wh);
    if (shot && shot_overlay) { g_shot_req = 0; glViewport(0, 0, ww, wh); save_screenshot(ww, wh); }
    SwapBuffers(g_hdc);
    gl_check_now(g_gl_debug ? "present" : "frame (run with --gl-debug for the exact call)");
}
