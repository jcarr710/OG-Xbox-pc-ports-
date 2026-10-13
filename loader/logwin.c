/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Log window (the "Log window" setting): a read-only text window with the
 * live log. It runs on its own thread so a burst of logging never delays the
 * game window, and closing it only hides it (and turns the setting off).
 *
 * xlog() copies every line into a ring buffer, so opening the window shows
 * the recent history too; a 100 ms timer moves new text into the edit
 * control, which keeps the last megabyte or so. */
#include "xbox.h"
#include "settings.h"
#include <stdlib.h>
#include <string.h>

void overlay_invalidate(void);

#define RING (1u << 18)
static char g_ring[RING];
static uint32_t g_ring_w, g_ring_r;          /* bytes ever written / already shown */
static SRWLOCK g_ring_lock = SRWLOCK_INIT;

static HWND g_lw, g_edit;
static HANDLE g_lw_ready;
static HBRUSH g_bg;
static HFONT g_font;
#define WM_APP_SHOW (WM_APP + 1)
#define C_BG   RGB(16, 18, 22)
#define C_TEXT RGB(214, 218, 224)

void logwin_append(const char *s, int n)
{
    if (n <= 0) return;
    if ((uint32_t)n > RING) { s += n - RING; n = RING; }
    AcquireSRWLockExclusive(&g_ring_lock);
    uint32_t off = g_ring_w % RING, first = RING - off < (uint32_t)n ? RING - off : (uint32_t)n;
    memcpy(g_ring + off, s, first);
    memcpy(g_ring, s + first, n - first);
    g_ring_w += n;
    ReleaseSRWLockExclusive(&g_ring_lock);
}

static void append_text(const wchar_t *ws)
{
    int len = GetWindowTextLengthW(g_edit);
    if (len > (1 << 20)) {                         /* drop the older half, at a line start */
        int line = (int)SendMessageW(g_edit, EM_LINEFROMCHAR, len / 2, 0);
        int cut = (int)SendMessageW(g_edit, EM_LINEINDEX, line + 1, 0);
        if (cut > 0) {
            SendMessageW(g_edit, EM_SETSEL, 0, cut);
            SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
            len = GetWindowTextLengthW(g_edit);
        }
    }
    /* follow the end unless the reader has scrolled up */
    SCROLLINFO si = { sizeof si, SIF_ALL };
    int follow = !GetScrollInfo(g_edit, SB_VERT, &si) || si.nPos + (int)si.nPage > si.nMax;
    DWORD s0 = 0, s1 = 0;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    int top = (int)SendMessageW(g_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(g_edit, EM_SETSEL, len, len);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)ws);
    if (follow) {
        int end = GetWindowTextLengthW(g_edit);
        SendMessageW(g_edit, EM_SETSEL, end, end);
        SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    } else {
        SendMessageW(g_edit, EM_SETSEL, s0, s1);
        int now = (int)SendMessageW(g_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (now != top) SendMessageW(g_edit, EM_LINESCROLL, 0, top - now);
    }
}

/* move new log text into the edit control (window thread) */
static void pump(void)
{
    AcquireSRWLockExclusive(&g_ring_lock);
    uint32_t w = g_ring_w, r = g_ring_r;
    int partial = 0;
    if (w - r > RING) { r = w - RING; partial = 1; }   /* fell behind: start at the oldest line kept */
    uint32_t n = w - r;
    char *buf = n ? malloc(n) : NULL;
    if (buf) for (uint32_t i = 0; i < n; ) {
        uint32_t off = (r + i) % RING, k = RING - off < n - i ? RING - off : n - i;
        memcpy(buf + i, g_ring + off, k);
        i += k;
    }
    g_ring_r = w;
    ReleaseSRWLockExclusive(&g_ring_lock);
    if (!buf) return;
    char *s = buf;
    if (partial) {
        char *nl = memchr(buf, '\n', n);
        if (!nl) { free(buf); return; }
        s = nl + 1; n -= (uint32_t)(s - buf);
    }
    int wn = n ? MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0) : 0;
    wchar_t *tmp = malloc((wn + 1) * sizeof *tmp), *ws = malloc((2 * wn + 1) * sizeof *ws);
    if (tmp && ws && wn) {
        MultiByteToWideChar(CP_UTF8, 0, s, (int)n, tmp, wn);
        int o = 0;
        for (int i = 0; i < wn; i++) { if (tmp[i] == L'\n') ws[o++] = L'\r'; ws[o++] = tmp[i]; }
        ws[o] = 0;
        append_text(ws);
    }
    free(tmp); free(ws); free(buf);
}

static LRESULT CALLBACK lw_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE: if (g_edit) MoveWindow(g_edit, 0, 0, LOWORD(l), HIWORD(l), TRUE); return 0;
    case WM_TIMER: if (IsWindowVisible(h)) pump(); return 0;
    case WM_CTLCOLORSTATIC:                        /* read-only edit controls ask here */
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, C_TEXT); SetBkColor((HDC)w, C_BG);
        return (LRESULT)g_bg;
    case WM_CLOSE:                                 /* hide, never quit: the game keeps running */
        ShowWindow(h, SW_HIDE);
        g_set.log_window = 0;
        settings_save();
        overlay_invalidate();
        xlog("log window closed (Log window setting now off)");
        return 0;
    case WM_APP_SHOW:
        if (w) { pump(); ShowWindow(h, SW_SHOWNOACTIVATE); }
        else ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI lw_thread(LPVOID p)
{
    HDC sdc = GetDC(NULL);
    int dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (dpi < 96) dpi = 96;
    g_bg = CreateSolidBrush(C_BG);
    g_font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = lw_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = g_bg;
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = L"JSRFLog";
    RegisterClassW(&wc);
    g_lw = CreateWindowExW(0, L"JSRFLog", L"JSRF log", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           MulDiv(960, dpi, 96), MulDiv(540, dpi, 96), NULL, NULL, wc.hInstance, NULL);
    g_edit = CreateWindowExW(0, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                             ES_AUTOVSCROLL | ES_NOHIDESEL, 0, 0, 100, 100, g_lw, NULL, wc.hInstance, NULL);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 0, 0);
    RECT rc; GetClientRect(g_lw, &rc);
    MoveWindow(g_edit, 0, 0, rc.right, rc.bottom, FALSE);
    SetTimer(g_lw, 1, 100, NULL);
    SetEvent(g_lw_ready);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

void logwin_show(int on)
{
    if (!g_lw) {
        if (!on) return;
        g_lw_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        host_create_thread(0, lw_thread, NULL, 0, NULL);
        WaitForSingleObject(g_lw_ready, 5000);
        if (!g_lw) return;
    }
    PostMessageW(g_lw, WM_APP_SHOW, (WPARAM)on, 0);
}
