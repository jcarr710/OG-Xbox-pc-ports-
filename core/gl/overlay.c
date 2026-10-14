/* SPDX-License-Identifier: GPL-2.0-or-later */
/* In-game settings menu, FPS counter and the startup hint, drawn over the
 * presented frame on the GPU thread.
 *
 * Open/close: F1 on the keyboard, or Back + Start together on a controller.
 * Text is drawn with GDI (the system UI font, at the window's real pixel
 * size) into a bitmap that is uploaded as a GL texture whenever the menu
 * changes. While the menu is open the game sees released controls
 * (input.c); every change is applied at once and saved to <stem>.ini. */
#include "gl.h"
#include "../settings.h"
#include "../hle.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

void gl_set_fullscreen(int on);
void gl_set_vsync(int on);
int  gl_auto_scale(void);
void surface_rescale(int scale);
void logwin_show(int on);
void input_stop_rumble(void);
extern float g_master_volume;

/* ---------------- menu model ---------------- */
enum { K_HEADER, K_ENUM, K_NUM, K_SCALE, K_ANISO, K_ACTION };
enum { A_RESET = 1, A_CLOSE };
typedef struct {
    int kind;
    const wchar_t *label;
    int *val; int lo, hi, step;
    const wchar_t *const *names;
    const wchar_t *help;
    int action;
} Item;

static const wchar_t *const k_onoff[] = { L"Off", L"On" };
static const wchar_t *const k_display[] = { L"Window", L"Fullscreen" };
static const wchar_t *const k_aspect[] = { L"4:3", L"Stretch to window" };
static const wchar_t *const k_filter[] = { L"Smooth", L"Sharp" };
static const wchar_t *const k_shown[] = { L"Hidden", L"Shown" };

/* "<stem>.ini" / "<stem>.log" as wide strings */
static const wchar_t *stem_file(const char *ext)
{
    static wchar_t buf[2][64]; static int k;
    wchar_t *b = buf[k++ & 1]; int i = 0;
    for (const char *p = g_title.stem; *p && i < 50; p++) b[i++] = (wchar_t)*p;
    b[i++] = L'.';
    for (const char *p = ext; *p && i < 62; p++) b[i++] = (wchar_t)*p;
    b[i] = 0;
    return b;
}

static const Item g_items[] = {
    { K_HEADER, L"VIDEO" },
    { K_SCALE, L"Internal resolution", &g_set.scale, 0, 8, 1, NULL, L"Rendering resolution. Higher is sharper; Auto matches your monitor." },
    { K_ENUM, L"Display", &g_set.fullscreen, 0, 1, 1, k_display, L"Window or borderless fullscreen (Alt+Enter also switches)." },
    { K_ENUM, L"Aspect ratio", &g_set.aspect, 0, 1, 1, k_aspect, L"4:3 keeps the original picture; Stretch fills the window." },
    { K_ENUM, L"Scaling filter", &g_set.filter, 0, 1, 1, k_filter, L"How the picture is scaled to the window." },
    { K_ENUM, L"VSync", &g_set.vsync, 0, 1, 1, k_onoff, L"Wait for the display's refresh. Removes tearing in fullscreen." },
    { K_NUM, L"Brightness", &g_set.brightness, 50, 200, 5, NULL, L"Picture brightness (gamma). 100% is the original." },
    { K_ANISO, L"Texture filtering", &g_set.aniso, 1, 16, 0, NULL, L"Anisotropic filtering keeps the ground sharp at shallow angles." },
    { K_ENUM, L"Frame counter", &g_set.show_fps, 0, 1, 1, k_onoff, L"Frames per second in the top-left corner." },
    { K_HEADER, L"AUDIO" },
    { K_NUM, L"Volume", &g_set.volume, 0, 100, 5, NULL, L"Master volume." },
    { K_HEADER, L"CONTROLS" },
    { K_ENUM, L"Vibration", &g_set.rumble, 0, 1, 1, k_onoff, L"Controller rumble." },
    { K_NUM, L"Stick dead zone", &g_set.deadzone, 0, 30, 1, NULL, L"Raise it if the character creeps with the stick released." },
    { K_HEADER, L"SYSTEM" },
    { K_ENUM, L"Log window", &g_set.log_window, 0, 1, 1, k_shown, NULL },
    { K_ACTION, L"Reset to defaults", NULL, 0, 0, 0, NULL, L"Put every setting back to its default.", A_RESET },
    { K_ACTION, L"Close", NULL, 0, 0, 0, NULL, NULL, A_CLOSE },
};
#define NITEMS (int)(sizeof g_items / sizeof *g_items)

static int g_sel = 1;
static int g_dirty = 1;
static DWORD g_scale_change_at;      /* debounce: rescaling reallocates every render target */

static void value_text(const Item *it, wchar_t *out, int n)
{
    int v = it->val ? *it->val : 0;
    switch (it->kind) {
    case K_ENUM: swprintf(out, n, L"%ls", it->names[v - it->lo]); break;
    case K_NUM: swprintf(out, n, L"%d%%", v); break;
    case K_ANISO: if (v <= 1) swprintf(out, n, L"Standard"); else swprintf(out, n, L"%dx anisotropic", v); break;
    case K_SCALE: {
        int s = v ? v : gl_auto_scale();
        if (v) swprintf(out, n, L"%dx  (%d \x00d7 %d)", s, 640 * s, 480 * s);
        else swprintf(out, n, L"Auto: %dx  (%d \x00d7 %d)", s, 640 * s, 480 * s);
        break; }
    default: out[0] = 0;
    }
}

static void apply(const Item *it)
{
    if (it->val == &g_set.scale) g_scale_change_at = GetTickCount() | 1;
    else if (it->val == &g_set.fullscreen) gl_set_fullscreen(g_set.fullscreen);
    else if (it->val == &g_set.vsync) gl_set_vsync(g_set.vsync);
    else if (it->val == &g_set.volume) g_master_volume = g_set.volume / 100.0f;
    else if (it->val == &g_set.log_window) logwin_show(g_set.log_window);
    else if (it->val == &g_set.rumble && !g_set.rumble) input_stop_rumble();
    /* saved when the menu closes (and on exit) */
}

static void change(const Item *it, int dir)
{
    if (!it->val) return;
    int v = *it->val, nv = v;
    if (it->kind == K_ANISO) {
        static const int steps[] = { 1, 2, 4, 8, 16 };
        int i = 0; while (i < 4 && steps[i] < v) i++;
        i += dir; if (i < 0) i = 0; if (i > 4) i = 4;
        nv = steps[i];
    } else if (it->kind == K_ENUM) {
        nv = v + dir;
        if (nv > it->hi) nv = it->lo;              /* on/off style values wrap */
        if (nv < it->lo) nv = it->hi;
    } else {
        nv = v + dir * it->step;
        if (nv < it->lo) nv = it->lo;
        if (nv > it->hi) nv = it->hi;
    }
    if (nv != v) { *it->val = nv; apply(it); g_dirty = 1; }
}

static void reset_all(void)
{
    int old_log = g_set.log_window;
    settings_defaults(&g_set);
    g_set.log_window = old_log;                     /* a window popping up or vanishing would surprise */
    gl_set_fullscreen(g_set.fullscreen);
    gl_set_vsync(g_set.vsync);
    g_master_volume = g_set.volume / 100.0f;
    g_scale_change_at = GetTickCount() | 1;
    settings_save();
    g_dirty = 1;
}

void overlay_invalidate(void) { g_dirty = 1; }     /* a setting changed elsewhere */

static void open_menu(int open)
{
    if (open == g_menu_open) return;
    g_menu_open = open;
    if (!open) { input_menu_closed(); settings_save(); }
    g_dirty = 1;
}

/* ---------------- input: edges and auto-repeat ---------------- */
static int g_prev_held;
static DWORD g_repeat_at;

static void menu_input(void)
{
    int held = input_menu_buttons();
    int pressed = (held & ~g_prev_held) | input_menu_events();
    DWORD now = GetTickCount();
    int dirs = held & (MENU_UP | MENU_DOWN | MENU_LEFT | MENU_RIGHT);
    if (pressed & dirs) g_repeat_at = now + 380;
    else if (dirs && g_menu_open && (int)(now - g_repeat_at) >= 0) { pressed |= dirs; g_repeat_at = now + 70; }
    g_prev_held = held;

    if (pressed & MENU_TOGGLE) { open_menu(!g_menu_open); return; }
    if (!g_menu_open) return;
    if (pressed & MENU_BACK) { open_menu(0); return; }
    if (pressed & (MENU_UP | MENU_DOWN)) {
        int d = (pressed & MENU_UP) ? -1 : 1;
        do g_sel = (g_sel + d + NITEMS) % NITEMS; while (g_items[g_sel].kind == K_HEADER);
        g_dirty = 1;
    }
    const Item *it = &g_items[g_sel];
    if (pressed & MENU_LEFT) change(it, -1);
    if (pressed & MENU_RIGHT) change(it, +1);
    if (pressed & MENU_OK) {
        if (it->action == A_CLOSE) open_menu(0);
        else if (it->action == A_RESET) reset_all();
        else if (it->kind == K_ENUM) change(it, +1);
    }
}

/* ---------------- GDI text -> GL texture ---------------- */
typedef struct { GLuint tex; int w, h; } UiTex;

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
}

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
}

static void text(HDC dc, HFONT f, COLORREF c, int x, int y, int h, const wchar_t *s, int right_align)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SIZE sz; GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    if (right_align) x -= sz.cx;
    TextOutW(dc, x, y + (h - sz.cy) / 2, s, (int)wcslen(s));
}

typedef void (*PaintFn)(HDC dc, int w, int h, void *ctx);

static void paint_to_texture(UiTex *t, int w, int h, PaintFn paint, void *ctx)
{
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    paint(dc, w, h, ctx);
    GdiFlush();
    if (!t->tex) glGenTextures(1, &t->tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, bits);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    t->w = w; t->h = h;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---------------- menu painting ---------------- */
#define C_BG      RGB(16, 18, 22)
#define C_SEL     RGB(46, 54, 18)
#define C_ACCENT  RGB(196, 236, 40)
#define C_TEXT    RGB(226, 229, 234)
#define C_DIM     RGB(128, 134, 145)
#define C_VALUE   RGB(255, 255, 255)

typedef struct { int L, w; } MenuGeom;

static int menu_height(int L)
{
    int pad = L / 2, h = pad + L * 13 / 10;
    for (int i = 0; i < NITEMS; i++) h += g_items[i].kind == K_HEADER ? L * 9 / 10 : L;
    return h + L / 3 + L * 8 / 10 + L * 8 / 10 + pad;
}

static void paint_menu(HDC dc, int W, int H, void *ctx)
{
    int L = ((MenuGeom *)ctx)->L, pad = L / 2;
    HFONT f_title = make_font(L * 70 / 100, FW_BOLD), f_item = make_font(L * 52 / 100, FW_NORMAL);
    HFONT f_sect = make_font(L * 40 / 100, FW_SEMIBOLD), f_small = make_font(L * 40 / 100, FW_NORMAL);
    fill(dc, 0, 0, W, H, C_BG);
    fill(dc, 0, 0, W, L / 12 + 1, C_ACCENT);
    int y = pad;
    text(dc, f_title, C_TEXT, pad, y, L * 13 / 10, L"Settings", 0);
    text(dc, f_small, C_DIM, W - pad, y, L * 13 / 10, stem_file("ini"), 1);
    y += L * 13 / 10;
    wchar_t v[96];
    for (int i = 0; i < NITEMS; i++) {
        const Item *it = &g_items[i];
        if (it->kind == K_HEADER) {
            int hh = L * 9 / 10;
            text(dc, f_sect, C_DIM, pad, y + L / 6, hh - L / 6, it->label, 0);
            y += hh;
            continue;
        }
        int sel = i == g_sel;
        if (sel) { fill(dc, pad / 3, y, W - pad / 3, y + L, C_SEL); fill(dc, pad / 3, y, pad / 3 + L / 10, y + L, C_ACCENT); }
        text(dc, f_item, sel ? C_ACCENT : C_TEXT, pad, y, L, it->label, 0);
        if (it->val) {
            value_text(it, v, 96);
            if (sel) {
                wchar_t s[110]; swprintf(s, 110, L"\x2039  %ls  \x203A", v);
                text(dc, f_item, C_VALUE, W - pad, y, L, s, 1);
            } else text(dc, f_item, C_TEXT, W - pad, y, L, v, 1);
        }
        y += L;
    }
    y += L / 3;
    const wchar_t *help = g_items[g_sel].help;
    wchar_t hb[160];
    if (!help) {
        if (g_items[g_sel].val == &g_set.log_window)
            swprintf(hb, 160, L"A separate window with the live log. %ls is written either way.", stem_file("log"));
        else swprintf(hb, 160, L"Settings are saved to %ls as you change them.", stem_file("ini"));
        help = hb;
    }
    text(dc, f_small, C_DIM, pad, y, L * 8 / 10, help, 0);
    y += L * 8 / 10;
    text(dc, f_small, C_DIM, pad, y, L * 8 / 10,
         L"\x2191\x2193 select     \x2190\x2192 change     A / Enter: choose     B / Esc: close", 0);
    DeleteObject(f_title); DeleteObject(f_item); DeleteObject(f_sect); DeleteObject(f_small);
}

typedef struct { const wchar_t *s; int L; } LabelCtx;
static void paint_label(HDC dc, int W, int H, void *ctx)
{
    LabelCtx *c = ctx;
    HFONT f = make_font(c->L * 55 / 100, FW_SEMIBOLD);
    fill(dc, 0, 0, W, H, RGB(10, 10, 12));
    text(dc, f, C_TEXT, c->L / 3, 0, H, c->s, 0);
    DeleteObject(f);
}
static int label_width(const wchar_t *s, int L)
{
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f = make_font(L * 55 / 100, FW_SEMIBOLD);
    SelectObject(dc, f);
    SIZE sz; GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    DeleteObject(f); DeleteDC(dc);
    return sz.cx + L * 2 / 3;
}

/* ---------------- drawing ---------------- */
static GLuint g_prog, g_vao;
static GLint u_rect, u_alpha, u_color, u_mode;
static UiTex g_menu_tex, g_fps_tex, g_hint_tex;
static int g_menu_L, g_menu_ww, g_menu_wh;

static void ui_init(void)
{
    g_prog = gl_compile_program(
        "#version 330\nuniform vec4 rect; out vec2 uv;\n"
        "void main(){ vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
        "  uv = vec2(c.x, 1.0 - c.y); gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0); }\n",
        "#version 330\nin vec2 uv; out vec4 fragColor; uniform sampler2D tex; uniform float alpha; uniform vec4 color; uniform int mode;\n"
        "void main(){ fragColor = mode == 1 ? color : vec4(texture(tex, uv).rgb, alpha); }\n");
    u_rect = glGetUniformLocation(g_prog, "rect"); u_alpha = glGetUniformLocation(g_prog, "alpha");
    u_color = glGetUniformLocation(g_prog, "color"); u_mode = glGetUniformLocation(g_prog, "mode");
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "tex"), 0);
    glGenVertexArrays(1, &g_vao);
}

/* pixel rectangle (origin top-left) */
static void quad(int ww, int wh, int x, int y, int w, int h, GLuint tex, float alpha, const float *color)
{
    float x0 = 2.0f * x / ww - 1, x1 = 2.0f * (x + w) / ww - 1, y1 = 1 - 2.0f * y / wh, y0 = 1 - 2.0f * (y + h) / wh;
    glUniform4f(u_rect, x0, y0, x1, y1);
    glUniform1i(u_mode, color ? 1 : 0);
    if (color) glUniform4fv(u_color, 1, color);
    else { glUniform1f(u_alpha, alpha); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); glBindSampler(0, 0); }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void label_tex(UiTex *t, const wchar_t *s, int L)
{
    LabelCtx c = { s, L };
    paint_to_texture(t, label_width(s, L), L, paint_label, &c);
}

void overlay_frame(int ww, int wh)
{
    if (ww < 64 || wh < 64) return;
    menu_input();
    if (g_scale_change_at && GetTickCount() - g_scale_change_at > 400) {
        g_scale_change_at = 0;
        surface_rescale(g_set.scale ? g_set.scale : gl_auto_scale());
    }

    static DWORD t_start, fps_t; static uint32_t fps_flips; static int fps_val = -1, fps_shown = -1;
    DWORD now = GetTickCount();
    if (!t_start) { t_start = now; fps_t = now; fps_flips = g_perf.flips; }
    if (now - fps_t >= 500) { fps_val = (int)((g_perf.flips - fps_flips) * 1000 / (now - fps_t)); fps_t = now; fps_flips = g_perf.flips; }
    int hint = now - t_start < 8000 && !g_menu_open;
    if (!g_menu_open && !g_set.show_fps && !hint) return;

    if (!g_prog) ui_init();
    int L = wh / 26; if (L < 18) L = 18; if (L > 72) L = 72;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);

    if (g_set.show_fps && fps_val >= 0) {
        int Ls = L * 3 / 4;
        if (fps_val != fps_shown || !g_fps_tex.tex) {
            wchar_t s[32]; swprintf(s, 32, L"%d fps", fps_val);
            label_tex(&g_fps_tex, s, Ls);
            fps_shown = fps_val;
        }
        quad(ww, wh, Ls / 3, Ls / 3, g_fps_tex.w, g_fps_tex.h, g_fps_tex.tex, 0.75f, NULL);
    }
    if (hint) {
        if (!g_hint_tex.tex) label_tex(&g_hint_tex, L"Settings: F1, or Back + Start on the controller", L * 3 / 4);
        float a = now - t_start > 7000 ? (8000 - (now - t_start)) / 1000.0f : 1.0f;
        quad(ww, wh, L / 3, wh - g_hint_tex.h - L / 3, g_hint_tex.w, g_hint_tex.h, g_hint_tex.tex, 0.8f * a, NULL);
    }
    if (g_menu_open) {
        static const float dim[4] = { 0, 0, 0, 0.55f };
        quad(ww, wh, 0, 0, ww, wh, 0, 0, dim);
        int W = L * 22; if (W > ww - L) W = ww - L;
        int H = menu_height(L);
        if (g_dirty || g_menu_L != L || g_menu_ww != ww || g_menu_wh != wh) {
            MenuGeom g = { L, W };
            paint_to_texture(&g_menu_tex, W, H, paint_menu, &g);
            g_dirty = 0; g_menu_L = L; g_menu_ww = ww; g_menu_wh = wh;
        }
        quad(ww, wh, (ww - W) / 2, (wh - H) / 2 > 0 ? (wh - H) / 2 : 0, g_menu_tex.w, g_menu_tex.h, g_menu_tex.tex, 0.96f, NULL);
    }
    glDisable(GL_BLEND);
}
