/*
 * pm3ds_panic.c — halt-and-display on shim or assertion failures.
 * Fidelity tier: N/A (3DS-side primitive)
 *
 * Two screens go full-text; we dump panic reason, file:line, register
 * snapshot, current thread name, and a 32-frame return-address walk.
 * Loop on hidScanInput; KEY_START exits cleanly so the dev can go back
 * to the Homebrew Launcher without yanking power.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PM3DS_HOST_TEST
#include <3ds.h>
#endif

#include "pm3ds_log.h"
#include "pm3ds_panic.h"

void pm3ds_panic_install(void) {
    /* devkitARM/newlib funnels asserts to __assert_func, which itself
     * calls abort(). Hook abort via setjmp would require pthread state;
     * for the skeleton we let abort() take the default path (which on
     * 3DS triggers the system crash handler) — pm3ds_panic is the
     * primary path. */
}

void pm3ds_unimpl_once(const char *name) {
    extern pm3ds_stats_t pm3ds_stats;
    pm3ds_stats.unimpl_calls++;
    pm3ds_log(PM3DS_LOG_INFO, "[UNIMPL] %s", name);
}

__attribute__((noreturn))
void pm3ds_panic_impl(const char *file, int line, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    { extern void pm3ds_bottom_screen_lcd_restore(void); pm3ds_bottom_screen_lcd_restore(); }   /* Settings > Bottom screen */

    pm3ds_stats.panic_count++;
    pm3ds_log(PM3DS_LOG_ERROR, "PANIC %s:%d %s", file, line, msg);

#ifndef PM3DS_HOST_TEST
    /* Bring both screens to console mode and re-print. */
    consoleInit(GFX_TOP, NULL);
    consoleInit(GFX_BOTTOM, NULL);
    printf("\n\x1b[31;1mPANIC\x1b[0m %s:%d\n%s\n", file, line, msg);

    /* Stack walk: print up to 32 return addresses. */
    void *fp = __builtin_frame_address(0);
    printf("\nstack:\n");
    for (int i = 0; i < 32 && fp; i++) {
        void **frame = (void **)fp;
        void  *pc    = frame[-1]; /* ARM AAPCS layout */
        if (!pc) break;
        printf("  #%-2d  %p\n", i, pc);
        fp = frame[-3];
    }

    printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    /* Return from main → app exits. */
    exit(1);
#else
    /* Host path: just exit. */
    fprintf(stderr, "PANIC %s:%d %s\n", file, line, msg);
    exit(1);
#endif
    __builtin_unreachable();
}
