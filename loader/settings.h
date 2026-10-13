/* SPDX-License-Identifier: GPL-2.0-or-later */
/* User settings: jsrf.ini next to jsrf.exe, edited in game with the F1
 * (or Back + Start) menu. Every field is a plain int read by whichever
 * thread uses it; the menu (GPU thread) is the only writer after startup. */
#pragma once

typedef struct {
    /* video */
    int scale;        /* internal resolution multiplier 1..8; 0 = from the monitor */
    int fullscreen;   /* borderless fullscreen */
    int aspect;       /* 0 = 4:3 with bars, 1 = stretch to the window */
    int filter;       /* screen scaling: 0 = smooth (bilinear), 1 = sharp (nearest) */
    int vsync;        /* wait for the display's refresh when presenting */
    int brightness;   /* percent, 50..200 */
    int aniso;        /* anisotropic texture filtering: 1 (off), 2, 4, 8, 16 */
    int show_fps;
    /* audio */
    int volume;       /* percent, 0..100 */
    /* controls */
    int rumble;
    int deadzone;     /* stick dead zone, percent of full deflection, 0..30 */
    /* system */
    int log_window;   /* show the live log in a window of its own (logwin.c) */
} Settings;

extern Settings g_set;

void settings_load(const char *ini_path);     /* defaults, then the file */
void settings_save(void);
void settings_defaults(Settings *s);
