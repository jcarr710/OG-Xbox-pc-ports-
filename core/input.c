/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Controller input: XDK XInput/XPP API -> Windows XInput (+ keyboard).
 *
 * The XDK USB stack (XPP) drives an OHCI host controller directly; instead
 * of emulating USB we replace the public device API. Up to four Windows
 * XInput controllers drive Xbox ports 1-4 in the order they are first used;
 * the keyboard always drives port 1. */
#include "hle.h"
#include "settings.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Xbox structures ---- */
typedef struct { ULONG CurrentConnected, ChangeConnected, PreviousConnected; } XPP_DEVICE_TYPE;
#pragma pack(push, 2)
typedef struct { WORD wButtons; BYTE bAnalogButtons[8]; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } X_XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; X_XINPUT_GAMEPAD Gamepad; } X_XINPUT_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } X_XINPUT_RUMBLE;
typedef struct { BYTE SubType; WORD Reserved; X_XINPUT_GAMEPAD In; X_XINPUT_RUMBLE Out; } X_XINPUT_CAPABILITIES;
typedef struct { DWORD dwStatus; HANDLE hEvent; BYTE Reserved[58]; X_XINPUT_RUMBLE Rumble; } X_XINPUT_FEEDBACK;
#pragma pack(pop)

enum { XB_A, XB_B, XB_X, XB_Y, XB_BLACK, XB_WHITE, XB_LT, XB_RT };
#define XB_DPAD_UP 0x01
#define XB_DPAD_DOWN 0x02
#define XB_DPAD_LEFT 0x04
#define XB_DPAD_RIGHT 0x08
#define XB_START 0x10
#define XB_BACK 0x20
#define XB_LTHUMB 0x40
#define XB_RTHUMB 0x80

/* ---- Windows XInput (loaded dynamically) ---- */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } W_GAMEPAD;
typedef struct { DWORD dwPacketNumber; W_GAMEPAD Gamepad; } W_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } W_VIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; W_GAMEPAD Gamepad; W_VIBRATION Vibration; } W_CAPS;
static DWORD (WINAPI *pXInputGetState)(DWORD, W_STATE *);
static DWORD (WINAPI *pXInputSetState)(DWORD, W_VIBRATION *);
static DWORD (WINAPI *pXInputGetCapabilities)(DWORD, DWORD, W_CAPS *);

static LARGE_INTEGER g_in_qpf, g_in_t0;   /* scripted-input clock */
typedef struct { int port; DWORD packet; X_XINPUT_GAMEPAD last; } Pad;
static Pad g_pads[4];
static ULONG g_prev_mask = 0xFFFFFFFF;
int g_keyboard_window_focus = 1;   /* set by the window code; keyboard only when focused */
static CRITICAL_SECTION g_in_lock;

void input_init(void)
{
    InitializeCriticalSection(&g_in_lock);
    QueryPerformanceFrequency(&g_in_qpf); QueryPerformanceCounter(&g_in_t0);
    const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !pXInputGetState; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) {
            pXInputGetState = (void *)GetProcAddress(m, "XInputGetState");
            pXInputSetState = (void *)GetProcAddress(m, "XInputSetState");
            pXInputGetCapabilities = (void *)GetProcAddress(m, "XInputGetCapabilities");
            char path[MAX_PATH] = ""; GetModuleFileNameA(m, path, sizeof path);
            if (pXInputGetState) xlog("input: using %s", path);
        }
    }
    if (!pXInputGetState) xlog("input: no XInput DLL; keyboard only");
}

/* ---- Windows controllers -> Xbox ports ----
 * XInput slot numbers say little about which controller is in use: slot 0
 * can be a virtual or idle device while the pad in your hands sits in slot
 * 1. So controllers are attached to Xbox ports in the order they are first
 * used: the first one to see a button press (or a stick or trigger pushed
 * most of the way) becomes player 1, the next player 2, and so on. The
 * keyboard always drives player 1 too. */
typedef struct {
    int connected, port;     /* port: Xbox port it drives, -1 = not used yet */
    W_GAMEPAD prev;          /* state at the previous poll */
    W_STATE st;              /* latest state */
    int logged;              /* state changes written to the log so far */
} Slot;
static Slot g_slot[4];
static int g_port_slot[4] = { -1, -1, -1, -1 };

/* newly pressed button, or trigger/stick newly pushed past a threshold */
static int became_active(const W_GAMEPAD *o, const W_GAMEPAD *n)
{
    if (n->wButtons & ~o->wButtons & 0xF3FF) return 1;
    if (n->bLeftTrigger > 100 && o->bLeftTrigger <= 100) return 1;
    if (n->bRightTrigger > 100 && o->bRightTrigger <= 100) return 1;
    const SHORT *a = &o->sThumbLX, *b = &n->sThumbLX;
    for (int i = 0; i < 4; i++) if (abs(b[i]) > 20000 && abs(a[i]) <= 20000) return 1;
    return 0;
}

static void describe_slot(int i)
{
    W_CAPS c;
    if (pXInputGetCapabilities && pXInputGetCapabilities(i, 0, &c) == 0)
        xlog("input: controller connected in XInput slot %d (type %u, subtype %u, flags %04x)", i, c.Type, c.SubType, c.Flags);
    else
        xlog("input: controller connected in XInput slot %d", i);
}

/* Read every connected XInput slot (empty ones only every 2 s: probing an
 * empty slot is slow). Called from the game's polling, at most ~250 times/s. */
static void poll_slots(void)
{
    static DWORD last_poll, last_scan; static int scanned;
    if (!pXInputGetState) return;
    EnterCriticalSection(&g_in_lock);
    DWORD now = GetTickCount();
    if (scanned && now - last_poll < 4) { LeaveCriticalSection(&g_in_lock); return; }
    last_poll = now;
    int full = !scanned || now - last_scan >= 2000;
    if (full) { last_scan = now; scanned = 1; }
    for (int i = 0; i < 4; i++) {
        Slot *s = &g_slot[i];
        if (!s->connected && !full) continue;
        W_STATE st;
        DWORD r = pXInputGetState(i, &st);
        if (r != 0) {
            if (s->connected) {
                xlog("input: controller in XInput slot %d disconnected (error %lu)", i, r);
                if (s->port >= 0) {
                    g_port_slot[s->port] = -1;
                    if (s->port == 0) xlog("input: player 1 is on the keyboard only until a controller is pressed");
                    else xlog("input: player %d unplugged", s->port + 1);
                }
                s->connected = 0; s->port = -1;
            }
            continue;
        }
        if (!s->connected) {
            s->connected = 1; s->port = -1; s->logged = 0;
            s->prev = st.Gamepad;            /* buttons already held don't count as a press */
            s->st = st;
            describe_slot(i);
        }
        if (memcmp(&st.Gamepad, &s->st.Gamepad, sizeof st.Gamepad) && s->logged < 40) {
            s->logged++;
            const W_GAMEPAD *g = &st.Gamepad;
            xlog("input: slot %d: buttons %04x  LT %3u RT %3u  L %6d %6d  R %6d %6d", i, g->wButtons, g->bLeftTrigger,
                 g->bRightTrigger, g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY);
        }
        if (s->port < 0 && became_active(&s->prev, &st.Gamepad)) {
            for (int p = 0; p < 4; p++) if (g_port_slot[p] < 0) {
                g_port_slot[p] = i; s->port = p;
                xlog("input: the controller in XInput slot %d is player %d", i, p + 1);
                break;
            }
        }
        s->prev = st.Gamepad;
        s->st = st;
    }
    LeaveCriticalSection(&g_in_lock);
}

/* the XInput state of the controller driving an Xbox port, if any */
static int port_xinput(int port, W_GAMEPAD *out)
{
    poll_slots();
    EnterCriticalSection(&g_in_lock);
    int slot = g_port_slot[port], ok = slot >= 0 && g_slot[slot].connected;
    if (ok) *out = g_slot[slot].st.Gamepad;
    LeaveCriticalSection(&g_in_lock);
    return ok;
}

static ULONG connected_mask(void)
{
    poll_slots();
    ULONG m = 1;                     /* port 1 always present (keyboard) */
    for (int p = 1; p < 4; p++) if (g_port_slot[p] >= 0) m |= 1u << p;
    return m;
}

/* Radial dead zone with rescaling, so worn sticks don't drift but full
 * deflection still reads full scale. Integer math: this runs inside game
 * calls, where the x87 state belongs to the game. */
static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return (uint32_t)r;
}
static void deadzone(SHORT *x, SHORT *y, int32_t dz)
{
    int64_t fx = *x, fy = *y;
    int32_t m = (int32_t)isqrt64((uint64_t)(fx * fx + fy * fy));
    if (m <= dz) { *x = *y = 0; return; }
    int32_t mc = m > 32767 ? 32767 : m;
    int64_t scaled = (int64_t)(mc - dz) * 32767 / (32767 - dz);     /* new magnitude */
    int64_t nx = fx * scaled / m, ny = fy * scaled / m;
    *x = (SHORT)(nx > 32767 ? 32767 : nx < -32768 ? -32768 : nx);
    *y = (SHORT)(ny > 32767 ? 32767 : ny < -32768 ? -32768 : ny);
}
#define STICK_DEADZONE (g_set.deadzone * 32767 / 100)   /* default 12%: modern pads rest within about +-3000 */

static int key(int vk) { return g_keyboard_window_focus && (GetAsyncKeyState(vk) & 0x8000); }

static SHORT axis(int neg, int pos) { return (SHORT)((pos ? 32767 : 0) - (neg ? 32767 : 0)); }

/* Scripted input for automated testing: --press=BUTTON@SECONDS[:DURATION],
 * or "press BUTTON [DURATION]" in the control file (main.c, --control) */
#define MAX_PRESS 256
typedef struct { char name[8]; double at, dur; } Press;
static Press g_press[MAX_PRESS]; static volatile LONG g_npress;
static double input_now(void);
void input_press_now(const char *name, double dur)
{
    if (g_npress >= MAX_PRESS) return;
    Press *p = &g_press[g_npress];
    snprintf(p->name, sizeof p->name, "%s", name);
    p->at = input_now() + 0.05; p->dur = dur > 0 ? dur : 0.25;
    MemoryBarrier();
    InterlockedIncrement(&g_npress);
}
void input_add_press(const char *spec)
{
    if (g_npress >= MAX_PRESS) return;
    Press *p = &g_press[g_npress];
    char name[16] = { 0 }; double at = 0, dur = 0.25;
    if (sscanf(spec, "%15[^@]@%lf:%lf", name, &at, &dur) < 2) { xlog("input: bad --press spec '%s'", spec); return; }
    snprintf(p->name, sizeof p->name, "%s", name); p->at = at; p->dur = dur;
    g_npress++;
}
static double input_now(void)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (!g_in_qpf.QuadPart) { QueryPerformanceFrequency(&g_in_qpf); }
    return (double)(t.QuadPart - g_in_t0.QuadPart) / (double)g_in_qpf.QuadPart;
}
static void apply_script(X_XINPUT_GAMEPAD *g)
{
    double now = input_now();
    for (int i = 0; i < g_npress; i++) {
        Press *p = &g_press[i];
        static int logged[MAX_PRESS];
        if (now >= p->at + p->dur && !logged[i]) { logged[i] = 1; xlog("input: scripted press %s@%.1f missed (no input poll during it)", p->name, p->at); }
        if (now < p->at || now >= p->at + p->dur) continue;
        if (!logged[i]++) xlog("input: scripted press %s at %.1fs", p->name, now);
        const char *n = p->name;
        if (!_stricmp(n, "START")) g->wButtons |= XB_START;
        else if (!_stricmp(n, "BACK")) g->wButtons |= XB_BACK;
        else if (!_stricmp(n, "UP")) g->wButtons |= XB_DPAD_UP;
        else if (!_stricmp(n, "DOWN")) g->wButtons |= XB_DPAD_DOWN;
        else if (!_stricmp(n, "LEFT")) g->wButtons |= XB_DPAD_LEFT;
        else if (!_stricmp(n, "RIGHT")) g->wButtons |= XB_DPAD_RIGHT;
        else if (!_stricmp(n, "A")) g->bAnalogButtons[XB_A] = 255;
        else if (!_stricmp(n, "B")) g->bAnalogButtons[XB_B] = 255;
        else if (!_stricmp(n, "X")) g->bAnalogButtons[XB_X] = 255;
        else if (!_stricmp(n, "Y")) g->bAnalogButtons[XB_Y] = 255;
        else if (!_stricmp(n, "BLACK")) g->bAnalogButtons[XB_BLACK] = 255;
        else if (!_stricmp(n, "WHITE")) g->bAnalogButtons[XB_WHITE] = 255;
        else if (!_stricmp(n, "LT")) g->bAnalogButtons[XB_LT] = 255;
        else if (!_stricmp(n, "RT")) g->bAnalogButtons[XB_RT] = 255;
        else if (!_stricmp(n, "LUP")) g->sThumbLY = 32767;
        else if (!_stricmp(n, "LDOWN")) g->sThumbLY = -32767;
        else if (!_stricmp(n, "LLEFT")) g->sThumbLX = -32767;
        else if (!_stricmp(n, "LRIGHT")) g->sThumbLX = 32767;
    }
}

/* ---- the settings menu borrows the controls ----
 * While it is open the game sees released controls; after it closes they stay
 * released until everything has been let go, so the button that closed the
 * menu doesn't also reach the game. */
volatile int g_menu_open;
static volatile int g_input_hold;
void input_menu_closed(void) { g_input_hold = 1; }

static int pad_neutral(const X_XINPUT_GAMEPAD *g)
{
    if (g->wButtons) return 0;
    for (int i = 0; i < 8; i++) if (g->bAnalogButtons[i] > 30) return 0;
    return abs(g->sThumbLX) < 8000 && abs(g->sThumbLY) < 8000 && abs(g->sThumbRX) < 8000 && abs(g->sThumbRY) < 8000;
}

/* Key presses for the menu arrive as window messages (polling alone can miss
 * a quick tap between two frames). */
static volatile LONG g_menu_key_events;
static int menu_bits_for_key(int vk)
{
    switch (vk) {
    case VK_UP: case 'W': return MENU_UP;
    case VK_DOWN: case 'S': return MENU_DOWN;
    case VK_LEFT: case 'A': return MENU_LEFT;
    case VK_RIGHT: case 'D': return MENU_RIGHT;
    case VK_RETURN: case VK_SPACE: return MENU_OK;
    case VK_ESCAPE: case VK_BACK: return MENU_BACK;
    case VK_F1: return MENU_TOGGLE;
    }
    return 0;
}
void input_menu_key(int vk) { int b = menu_bits_for_key(vk); if (b) InterlockedOr(&g_menu_key_events, b); }
int  input_menu_events(void) { return (int)InterlockedExchange(&g_menu_key_events, 0); }

/* Controls as the menu sees them: any keyboard key or any controller. */
int input_menu_buttons(void)
{
    int b = 0;
    if (key(VK_UP) || key('W')) b |= MENU_UP;
    if (key(VK_DOWN) || key('S')) b |= MENU_DOWN;
    if (key(VK_LEFT) || key('A')) b |= MENU_LEFT;
    if (key(VK_RIGHT) || key('D')) b |= MENU_RIGHT;
    if (key(VK_RETURN) || key(VK_SPACE)) b |= MENU_OK;
    if (key(VK_ESCAPE) || key(VK_BACK)) b |= MENU_BACK;
    if (key(VK_F1)) b |= MENU_TOGGLE;
    poll_slots();
    EnterCriticalSection(&g_in_lock);
    for (int i = 0; i < 4; i++) {
        if (!g_slot[i].connected) continue;
        const W_GAMEPAD *g = &g_slot[i].st.Gamepad;
        if ((g->wButtons & 1) || g->sThumbLY > 16000) b |= MENU_UP;
        if ((g->wButtons & 2) || g->sThumbLY < -16000) b |= MENU_DOWN;
        if ((g->wButtons & 4) || g->sThumbLX < -16000) b |= MENU_LEFT;
        if ((g->wButtons & 8) || g->sThumbLX > 16000) b |= MENU_RIGHT;
        if (g->wButtons & 0x1000) b |= MENU_OK;
        if (g->wButtons & 0x2000) b |= MENU_BACK;
        if ((g->wButtons & 0x30) == 0x30) b |= MENU_TOGGLE;        /* Back + Start */
    }
    LeaveCriticalSection(&g_in_lock);
    return b;
}

static void read_pad_raw(int port, X_XINPUT_GAMEPAD *g);
static void read_pad(int port, X_XINPUT_GAMEPAD *g)
{
    read_pad_raw(port, g);
    if ((g->wButtons & (XB_BACK | XB_START)) == (XB_BACK | XB_START)) memset(g, 0, sizeof *g);  /* menu combo */
    if (g_menu_open) memset(g, 0, sizeof *g);
    else if (g_input_hold) {
        if (pad_neutral(g)) g_input_hold = 0;
        memset(g, 0, sizeof *g);
    }
}

static void read_pad_raw(int port, X_XINPUT_GAMEPAD *g)
{
    memset(g, 0, sizeof *g);
    if (port == 0 && g_npress) apply_script(g);
    W_STATE s;
    if (port_xinput(port, &s.Gamepad)) {
        deadzone(&s.Gamepad.sThumbLX, &s.Gamepad.sThumbLY, STICK_DEADZONE);
        deadzone(&s.Gamepad.sThumbRX, &s.Gamepad.sThumbRY, STICK_DEADZONE);
        WORD b = s.Gamepad.wButtons;
        if (b & 0x0001) g->wButtons |= XB_DPAD_UP;
        if (b & 0x0002) g->wButtons |= XB_DPAD_DOWN;
        if (b & 0x0004) g->wButtons |= XB_DPAD_LEFT;
        if (b & 0x0008) g->wButtons |= XB_DPAD_RIGHT;
        if (b & 0x0010) g->wButtons |= XB_START;
        if (b & 0x0020) g->wButtons |= XB_BACK;
        if (b & 0x0040) g->wButtons |= XB_LTHUMB;
        if (b & 0x0080) g->wButtons |= XB_RTHUMB;
        if (b & 0x1000) g->bAnalogButtons[XB_A] = 255;
        if (b & 0x2000) g->bAnalogButtons[XB_B] = 255;
        if (b & 0x4000) g->bAnalogButtons[XB_X] = 255;
        if (b & 0x8000) g->bAnalogButtons[XB_Y] = 255;
        if (b & 0x0100) g->bAnalogButtons[XB_WHITE] = 255;  /* LB -> White */
        if (b & 0x0200) g->bAnalogButtons[XB_BLACK] = 255;  /* RB -> Black */
        if (s.Gamepad.bLeftTrigger > g->bAnalogButtons[XB_LT]) g->bAnalogButtons[XB_LT] = s.Gamepad.bLeftTrigger;
        if (s.Gamepad.bRightTrigger > g->bAnalogButtons[XB_RT]) g->bAnalogButtons[XB_RT] = s.Gamepad.bRightTrigger;
        if (s.Gamepad.sThumbLX || s.Gamepad.sThumbLY) { g->sThumbLX = s.Gamepad.sThumbLX; g->sThumbLY = s.Gamepad.sThumbLY; }
        if (s.Gamepad.sThumbRX || s.Gamepad.sThumbRY) { g->sThumbRX = s.Gamepad.sThumbRX; g->sThumbRY = s.Gamepad.sThumbRY; }
    }
    if (port == 0) { /* keyboard */
        if (key('W') || key('A') || key('S') || key('D')) { g->sThumbLX = axis(key('A'), key('D')); g->sThumbLY = axis(key('S'), key('W')); }
        if (key('J') || key('L') || key('I') || key('K')) { g->sThumbRX = axis(key('J'), key('L')); g->sThumbRY = axis(key('K'), key('I')); }
        if (key(VK_UP)) g->wButtons |= XB_DPAD_UP;
        if (key(VK_DOWN)) g->wButtons |= XB_DPAD_DOWN;
        if (key(VK_LEFT)) g->wButtons |= XB_DPAD_LEFT;
        if (key(VK_RIGHT)) g->wButtons |= XB_DPAD_RIGHT;
        if (key(VK_RETURN)) g->wButtons |= XB_START;
        if (key(VK_BACK) || key(VK_ESCAPE)) g->wButtons |= XB_BACK;
        if (key(VK_SPACE)) g->bAnalogButtons[XB_A] = 255;
        if (key(VK_LSHIFT) || key(VK_SHIFT)) g->bAnalogButtons[XB_B] = 255;
        if (key('E')) g->bAnalogButtons[XB_X] = 255;
        if (key('Q')) g->bAnalogButtons[XB_Y] = 255;
        if (key('R')) g->bAnalogButtons[XB_BLACK] = 255;
        if (key('F')) g->bAnalogButtons[XB_WHITE] = 255;
        if (key('Z') || key(VK_LBUTTON)) g->bAnalogButtons[XB_LT] = 255;
        if (key('C') || key(VK_RBUTTON)) g->bAnalogButtons[XB_RT] = 255;
    }
}

/* ---- replacements (stdcall, same signatures as XAPI) ---- */
static uint32_t g_type_gamepad;       /* XAPI's XDEVICE_TYPE_GAMEPAD descriptor */
static void __stdcall hle_XInitDevices(DWORD n, PVOID types)
{
    /* Devices present at init are "known": not reported as insertions. */
    g_prev_mask = connected_mask();
    XPP_DEVICE_TYPE *t = (XPP_DEVICE_TYPE *)g_type_gamepad;
    if (t) { t->CurrentConnected = t->PreviousConnected = g_prev_mask; t->ChangeConnected = 0; }
    xlog("XInitDevices(%lu) [hle]: gamepads 0x%lx", n, g_prev_mask);
}

static DWORD __stdcall hle_XGetDevices(XPP_DEVICE_TYPE *t)
{
    if ((uint32_t)t != g_type_gamepad) return 0;
    ULONG m = connected_mask();
    t->CurrentConnected = m; t->ChangeConnected = 0; t->PreviousConnected = m;
    g_prev_mask = m;
    return m;
}

static BOOL __stdcall hle_XGetDeviceChanges(XPP_DEVICE_TYPE *t, PDWORD ins, PDWORD rem)
{
    *ins = 0; *rem = 0;
    if ((uint32_t)t != g_type_gamepad) return FALSE;
    ULONG m = connected_mask();
    if (g_prev_mask == 0xFFFFFFFF) g_prev_mask = 0;
    *ins = m & ~g_prev_mask; *rem = g_prev_mask & ~m;
    g_prev_mask = m;
    return *ins || *rem;
}

static HANDLE __stdcall hle_XInputOpen(XPP_DEVICE_TYPE *t, DWORD port, DWORD slot, PVOID poll)
{
    if ((uint32_t)t != g_type_gamepad || port > 3 || !(connected_mask() & (1u << port))) {
        hle_set_last_error(1167 /* ERROR_DEVICE_NOT_CONNECTED */);
        return NULL;
    }
    Pad *p = &g_pads[port]; p->port = (int)port;
    xlog("XInputOpen(port %lu) [hle]", port);
    return (HANDLE)p;
}
static void __stdcall hle_XInputClose(HANDLE h) {}

static DWORD __stdcall hle_XInputGetCapabilities(HANDLE h, X_XINPUT_CAPABILITIES *c)
{
    memset(c, 0, sizeof *c);
    c->SubType = 1; /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    c->In.wButtons = 0xFF; memset(c->In.bAnalogButtons, 0xFF, 8);
    c->In.sThumbLX = c->In.sThumbLY = c->In.sThumbRX = c->In.sThumbRY = (SHORT)0xFFFF;
    c->Out.wLeftMotorSpeed = c->Out.wRightMotorSpeed = 0xFFFF;
    return 0;
}

volatile LONG g_input_polls;
static DWORD __stdcall hle_XInputGetState(HANDLE h, X_XINPUT_STATE *s)
{
    Pad *p = (Pad *)h;
    if (!p) return 6; /* ERROR_INVALID_HANDLE */
    g_input_polls++;
    X_XINPUT_GAMEPAD g; read_pad(p->port, &g);
    { static int n; if (g.wButtons && n++ < 8) xlog("XInputGetState(port %d) buttons %04x from %p", p->port, g.wButtons, __builtin_return_address(0)); }
    if (memcmp(&g, &p->last, sizeof g)) { p->packet++; p->last = g; }
    s->dwPacketNumber = p->packet;
    s->Gamepad = g;
    return 0;
}

static DWORD __stdcall hle_XInputSetState(HANDLE h, X_XINPUT_FEEDBACK *f)
{
    Pad *p = (Pad *)h;
    int slot = p ? g_port_slot[p->port] : -1;
    if (slot >= 0 && pXInputSetState) {
        W_VIBRATION v = { f->Rumble.wLeftMotorSpeed, f->Rumble.wRightMotorSpeed };
        if (!g_set.rumble) v.wLeftMotorSpeed = v.wRightMotorSpeed = 0;
        pXInputSetState(slot, &v);
    }
    f->dwStatus = 0;
    if (f->hEvent) SetEvent(f->hEvent);
    return 997; /* ERROR_IO_PENDING: completion already signalled */
}

static DWORD __stdcall hle_XInputPoll(HANDLE h) { return 0; }

void input_stop_rumble(void)
{
    W_VIBRATION v = { 0, 0 };
    if (pXInputSetState) for (int p = 0; p < 4; p++) if (g_port_slot[p] >= 0) pXInputSetState(g_port_slot[p], &v);
}
static DWORD __stdcall hle_XMountMUA(DWORD port, DWORD slot, PCHAR drive) { return 1167; }
static DWORD __stdcall hle_XUnmountMU(DWORD port, DWORD slot) { return 1167; }

void input_install(void)
{
    input_init();
    g_type_gamepad = sym("XAPILIB__g_DeviceType_Gamepad");
    if (!g_type_gamepad) xlog("input: WARNING the gamepad device type wasn't found; controllers won't work");
    hle_patch_sym("XAPILIB__XInitDevices",          hle_XInitDevices, 1);
    hle_patch_sym("XAPILIB__XGetDevices",           hle_XGetDevices, 1);
    hle_patch_sym("XAPILIB__XGetDeviceChanges",     hle_XGetDeviceChanges, 1);
    hle_patch_sym("XAPILIB__XInputOpen",            hle_XInputOpen, 1);
    hle_patch_sym("XAPILIB__XInputClose",           hle_XInputClose, 1);
    hle_patch_sym("XAPILIB__XInputGetCapabilities", hle_XInputGetCapabilities, 1);
    hle_patch_sym("XAPILIB__XInputGetState",        hle_XInputGetState, 1);
    hle_patch_sym("XAPILIB__XInputSetState",        hle_XInputSetState, 1);
    hle_patch_sym("XAPILIB__XInputPoll",            hle_XInputPoll, 0);
    hle_patch_sym("XAPILIB__XMountMUA",             hle_XMountMUA, 0);
    hle_patch_sym("XAPILIB__XUnmountMU",            hle_XUnmountMU, 0);
    xlog("input: XInput/XPP API replaced");
}
