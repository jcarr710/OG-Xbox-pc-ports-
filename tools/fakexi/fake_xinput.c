/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test double for xinput1_4.dll (Wine, WINEDLLOVERRIDES=xinput1_4=n).
 * Slot 0: an idle "virtual" pad that is always connected and never changes.
 * Slot 1: the "real" pad, driven by FAKE_XINPUT_SCRIPT = NAME@SEC[:DUR],...
 * (seconds since the DLL loaded; names START BACK A B X Y LB RB LT RT UP DOWN
 * LEFT RIGHT LUP LDOWN LLEFT LRIGHT). Slots 2-3: not connected. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } GP;
typedef struct { DWORD dwPacketNumber; GP Gamepad; } ST;
typedef struct { BYTE Type, SubType; WORD Flags; GP Gamepad; WORD l, r; } CAPS;

typedef struct { char name[8]; double at, dur; } Press;
static Press g_p[128]; static int g_n; static LARGE_INTEGER g_f, g_t0; static DWORD g_pkt;

static void init(void)
{
    QueryPerformanceFrequency(&g_f); QueryPerformanceCounter(&g_t0);
    const char *s = getenv("FAKE_XINPUT_SCRIPT");
    while (s && *s && g_n < 128) {
        Press *p = &g_p[g_n]; char nm[16] = {0}; double at = 0, dur = 0.25;
        if (sscanf(s, "%15[^@]@%lf:%lf", nm, &at, &dur) >= 2 || sscanf(s, "%15[^@]@%lf", nm, &at) == 2) {
            snprintf(p->name, sizeof p->name, "%s", nm); p->at = at; p->dur = dur; g_n++;
        }
        s = strchr(s, ','); if (s) s++;
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v) { if (r == DLL_PROCESS_ATTACH) init(); return TRUE; }

static void script(GP *g)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    double now = (double)(t.QuadPart - g_t0.QuadPart) / (double)g_f.QuadPart;
    for (int i = 0; i < g_n; i++) {
        Press *p = &g_p[i]; const char *n = p->name;
        if (now < p->at || now >= p->at + p->dur) continue;
        if (!strcmp(n, "START")) g->wButtons |= 0x10; else if (!strcmp(n, "BACK")) g->wButtons |= 0x20;
        else if (!strcmp(n, "UP")) g->wButtons |= 1; else if (!strcmp(n, "DOWN")) g->wButtons |= 2;
        else if (!strcmp(n, "LEFT")) g->wButtons |= 4; else if (!strcmp(n, "RIGHT")) g->wButtons |= 8;
        else if (!strcmp(n, "LB")) g->wButtons |= 0x100; else if (!strcmp(n, "RB")) g->wButtons |= 0x200;
        else if (!strcmp(n, "A")) g->wButtons |= 0x1000; else if (!strcmp(n, "B")) g->wButtons |= 0x2000;
        else if (!strcmp(n, "X")) g->wButtons |= 0x4000; else if (!strcmp(n, "Y")) g->wButtons |= 0x8000;
        else if (!strcmp(n, "LT")) g->bLeftTrigger = 255; else if (!strcmp(n, "RT")) g->bRightTrigger = 255;
        else if (!strcmp(n, "LUP")) g->sThumbLY = 32767; else if (!strcmp(n, "LDOWN")) g->sThumbLY = -32768;
        else if (!strcmp(n, "LLEFT")) g->sThumbLX = -32768; else if (!strcmp(n, "LRIGHT")) g->sThumbLX = 32767;
    }
}

__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD i, ST *s)
{
    if (i > 1) return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    if (i == 0) { s->dwPacketNumber = 1; s->Gamepad.sThumbLX = 1200; s->Gamepad.sThumbLY = -900; return 0; }  /* idle, slight drift */
    script(&s->Gamepad);
    static GP last; if (memcmp(&last, &s->Gamepad, sizeof last)) { g_pkt++; last = s->Gamepad; }
    s->dwPacketNumber = g_pkt;
    return 0;
}
__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD i, void *v) { return i > 1 ? ERROR_DEVICE_NOT_CONNECTED : 0; }
__declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, CAPS *c)
{
    if (i > 1) return ERROR_DEVICE_NOT_CONNECTED;
    memset(c, 0, sizeof *c); c->Type = 1; c->SubType = 1; return 0;
}
__declspec(dllexport) void WINAPI XInputEnable(BOOL e) {}
