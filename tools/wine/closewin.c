/* SPDX-License-Identifier: GPL-2.0-or-later */
/* closewin.exe CLASS: posts WM_CLOSE to the first top-level window of CLASS
 * (logwin.sh closes the log window with it, as the title-bar button would). */
#include <windows.h>
int main(int argc, char **argv)
{
    HWND h = FindWindowA(argc > 1 ? argv[1] : "JSRFLog", NULL);
    if (!h) return 1;
    PostMessageA(h, WM_CLOSE, 0, 0);
    return 0;
}
