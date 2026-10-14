/* SPDX-License-Identifier: GPL-2.0-or-later */
/* <stem>.ini: [Video] [Audio] [Controls] [System], one integer per key.
 * Read once at startup; rewritten whole (with a note on each key) whenever a
 * setting changes, through a temporary file so a crash can't truncate it. */
#include "xbox.h"
#include "settings.h"
#include "title.h"
#include <stdio.h>
#include <string.h>

Settings g_set;
static char g_ini[MAX_PATH];

typedef struct { const char *section, *key; int *val; int lo, hi; const char *note; } Field;
static const Field g_fields[] = {
    { "Video",    "Scale",       &g_set.scale,      0, 8,    "internal resolution: 0 auto (from the monitor), 1-8 for 640x480 times N" },
    { "Video",    "Fullscreen",  &g_set.fullscreen, 0, 1,    "0 window, 1 borderless fullscreen" },
    { "Video",    "Aspect",      &g_set.aspect,     0, 1,    "0 keep 4:3, 1 stretch to the window" },
    { "Video",    "Filter",      &g_set.filter,     0, 1,    "scaling to the window: 0 smooth, 1 sharp" },
    { "Video",    "VSync",       &g_set.vsync,      0, 1,    "0 off, 1 on" },
    { "Video",    "Brightness",  &g_set.brightness, 50, 200, "percent, 50-200" },
    { "Video",    "Anisotropy",  &g_set.aniso,      1, 16,   "texture filtering: 1 standard, 2/4/8/16 anisotropic" },
    { "Video",    "ShowFPS",     &g_set.show_fps,   0, 1,    "frame counter: 0 off, 1 on" },
    { "Audio",    "Volume",      &g_set.volume,     0, 100,  "percent, 0-100" },
    { "Controls", "Vibration",   &g_set.rumble,     0, 1,    "0 off, 1 on" },
    { "Controls", "DeadZone",    &g_set.deadzone,   0, 30,   "stick dead zone, percent 0-30" },
    { "System",   "LogWindow",   &g_set.log_window, 0, 1,    "window with the live log: 0 hidden, 1 shown" },
};
#define NFIELDS (int)(sizeof g_fields / sizeof *g_fields)

void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->scale = 0;          /* from the monitor */
    s->aspect = 0;
    s->filter = 0;
    s->vsync = 0;
    s->brightness = 100;
    s->aniso = 8;
    s->volume = 100;
    s->rumble = 1;
    s->deadzone = 12;
}

void settings_load(const char *ini_path)
{
    snprintf(g_ini, sizeof g_ini, "%s", ini_path);
    settings_defaults(&g_set);
    for (int i = 0; i < NFIELDS; i++) {
        const Field *f = &g_fields[i];
        int v = (int)GetPrivateProfileIntA(f->section, f->key, *f->val, g_ini);
        if (v < f->lo) v = f->lo;
        if (v > f->hi) v = f->hi;
        *f->val = v;
    }
    settings_save();       /* create it, add new keys, put back out-of-range values */
}

void settings_save(void)
{
    static SRWLOCK lock = SRWLOCK_INIT;       /* the menu, the window and the log window all save */
    if (!g_ini[0]) return;
    AcquireSRWLockExclusive(&lock);
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_ini);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "; %s settings. The in-game menu (F1, or Back + Start on a\n"
                   "; controller) changes these as you play; edits made here apply at the next start.\n", g_title.game);
        const char *sec = NULL;
        for (int i = 0; i < NFIELDS; i++) {
            const Field *fl = &g_fields[i];
            if (!sec || strcmp(sec, fl->section)) { fprintf(f, "\n[%s]\n", fl->section); sec = fl->section; }
            fprintf(f, "; %s\n%s=%d\n", fl->note, fl->key, *fl->val);
        }
        int ok = !ferror(f);
        if (fclose(f) != 0) ok = 0;
        if (!ok || !MoveFileExA(tmp, g_ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD e = GetLastError();
            DeleteFileA(tmp);
            static int warned;
            if (!warned++) xlog("settings: could not write %s (error %lu)", g_ini, e);
        }
    } else {
        static int warned;
        if (!warned++) xlog("settings: could not create %s (error %lu)", tmp, GetLastError());
    }
    ReleaseSRWLockExclusive(&lock);
}
