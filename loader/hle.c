/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Patch mechanism for XDK function replacement. */
#include "hle.h"
#include <string.h>

void hle_set_last_error(uint32_t err)
{
    uint32_t a[1] = { err };
    xcall((void *)XAPI_SetLastError, 1, a);
}

/* Overwrite the start of `va` with `jmp fn`. `expect` (optional) are the
 * original first bytes; on mismatch the patch is skipped with a warning. */
int hle_patch(uint32_t va, void *fn, const char *name, const char *expect, int nexpect)
{
    uint8_t *p = (uint8_t *)va;
    if (expect && memcmp(p, expect, nexpect)) {
        xlog("hle: WARNING %s @ %08x: unexpected bytes %02x %02x %02x %02x, not patched", name, va, p[0], p[1], p[2], p[3]);
        return 0;
    }
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)fn - (va + 5));
    return 1;
}

void dsound_install(void);
void nv2a_hle_install(void);
void hle_install(void)
{
    input_install();
    dsound_install();
    nv2a_hle_install();
}
