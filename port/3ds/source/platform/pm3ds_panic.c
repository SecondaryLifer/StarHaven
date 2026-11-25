/*
 * pm3ds_panic.c — halt-and-display on shim or assertion failures.
 * Fidelity tier: N/A (3DS-side primitive)
 *
 * The top screen shows the panic reason, file:line, pc/lr and the return
 * addresses found on the stack; the bottom one the same report as a QR code
 * for the Discord (crash_text). Loop on hidScanInput; KEY_START exits cleanly
 * so the player can go back to the Home Menu without yanking power.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PM3DS_HOST_TEST
#include <3ds.h>
#endif

#include "pm3ds_brand.h"
#include "pm3ds_log.h"
#include "pm3ds_panic.h"
#ifndef PM3DS_HOST_TEST
#include "qrcodegen.h"
#endif

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

#ifndef PM3DS_HOST_TEST
/* ---- the crash report: on the screen and in a QR code ------------------------------ */
/* The QR code holds the report as text, to scan with a phone and paste into the
 * Discord. It is written the way the support bot reads crash reports
 * (starhaven-discord bot/crash.py): the version as vX.Y.Z, code addresses as
 * 8 hex digits, "pc"/"lr" and "Fault address:" as in a Luma3DS dump, and like
 * the PANIC screen it reads from photos, "PANIC file:line", the message, "stack:". */
extern char __start__[], __exidx_start[];   /* the program's code (3dsx.ld) */

#if defined(VERSION_IQUE) && VERSION_IQUE
#define CRASH_ROM "iQue"
#elif defined(VERSION_US) && VERSION_US
#define CRASH_ROM "US"
#elif defined(VERSION_JP) && VERSION_JP
#define CRASH_ROM "JP"
#else
#define CRASH_ROM "EU"
#endif
#define CRASH_BT_MAX 8

/* a return address: in the code, after a BL or BLX (the port is ARM code, no Thumb) */
static bool is_return_address(u32 a) {
    if ((a & 3) || a < (u32)__start__ + 4 || a >= (u32)__exidx_start) return false;
    u32 insn = *(const u32 *)(a - 4);
    return (insn & 0x0F000000u) == 0x0B000000u      /* BL */
        || (insn & 0xFE000000u) == 0xFA000000u      /* BLX to a label */
        || (insn & 0x0FFFFFF0u) == 0x012FFF30u;     /* BLX to a register */
}

/* the return addresses left on the stack from sp up, within its memory block:
 * roughly the calls that led here (the build keeps no frame pointers). They are
 * taken from above the first that is after, ret: what lies below it is the
 * panicking code's own frame, whose unset words hold older calls' addresses. */
static int stack_return_addresses(const u32 *sp, u32 after, u32 *out, int max) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, (u32)sp))) return 0;
    const u32 *end = (const u32 *)(mi.base_addr + mi.size);
    if (end > sp + 4096) end = sp + 4096;
    for (const u32 *w = sp; w < end; w++) {
        if (*w == after) {
            sp = w + 1;
            break;
        }
    }
    int n = 0;
    for (const u32 *w = sp; w < end && n < max; w++) {
        if (!is_return_address(*w)) continue;
        bool seen = false;
        for (int i = 0; i < n && !seen; i++) seen = out[i] == *w;
        if (!seen) out[n++] = *w;
    }
    return n;
}

static const char *base_name(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* the report's text; has_far: a CPU fault, at far */
static void crash_text(char *t, size_t size, const char *file, int line, const char *msg,
                       u32 pc, u32 lr, bool has_far, u32 far, const u32 *bt, int n) {
    int k = snprintf(t, size, PM3DS_APP_NAME " v" PM3DS_VERSION_STR " " CRASH_ROM " crash\n"
                     "PANIC %s:%d\n%.100s\npc %08lx lr %08lx\n", base_name(file), line, msg,
                     (unsigned long)pc, (unsigned long)lr);
    if (k > 0 && (size_t)k < size)
        k += has_far ? snprintf(t + k, size - k, "Fault address: %08lx\n", (unsigned long)far)
                     : snprintf(t + k, size - k, "Fault address: none\n");
    if (k > 0 && (size_t)k < size) k += snprintf(t + k, size - k, "stack:");
    for (int i = 0; i < n && k > 0 && (size_t)k < size; i++)
        k += snprintf(t + k, size - k, " %08lx", (unsigned long)bt[i]);
}

/* the QR code of text on the bottom screen: fb is its console's framebuffer (RGB565,
 * rotated: 240 rows per screen column); as big as fits under the console's first two lines */
static void draw_qr(u16 *fb, const char *text) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(13)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(13)];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, 1, 13, qrcodegen_Mask_AUTO, true)) return;
    int n = qrcodegen_getSize(qr), quiet = 4;
    int top = 18, avail = 240 - top - 2;
    int m = avail / (n + 2 * quiet);
    if (m < 1) return;
    int side = (n + 2 * quiet) * m, x0 = (320 - side) / 2, y0 = top + (avail - side) / 2;
    if (!fb) return;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            int mx = x / m - quiet, my = y / m - quiet;
            bool dark = mx >= 0 && my >= 0 && mx < n && my < n && qrcodegen_getModule(qr, mx, my);
            int sx = x0 + x, sy = y0 + y;
            fb[sx * 240 + (239 - sy)] = dark ? 0x0000 : 0xFFFF;
        }
    }
}

/* the crash screen: the report on the top screen, its QR code on the bottom; START exits */
static void crash_screen(const char *file, int line, const char *msg, u32 pc, u32 lr,
                         bool has_far, u32 far, const u32 *bt, int n) {
    static char report[512];
    crash_text(report, sizeof report, file, line, msg, pc, lr, has_far, far, bt, n);
    pm3ds_log(PM3DS_LOG_ERROR, "crash report:\n%s", report);

    /* the last frame's copies to the screens are still under way: let them land
     * first, or they write over the consoles' text (citro3d sends no new ones) */
    for (int i = 0; i < 3; i++) gspWaitForVBlank();
    gfxSet3D(false);
    static PrintConsole top, bottom;   /* one each: consoleInit(.., NULL) shares one */
    consoleInit(GFX_BOTTOM, &bottom);
    printf(" Scan this for the Star Haven Discord:\n the crash report from the top screen.");
    consoleInit(GFX_TOP, &top);   /* printf goes to the top screen from here */
    printf("\n\x1b[31;1mPANIC\x1b[0m %s:%d\n%s\n", file, line, msg);
    printf("\n" PM3DS_APP_NAME " v" PM3DS_VERSION_STR " (" CRASH_ROM " ROM)\n");
    printf("pc %08lx  lr %08lx\n", (unsigned long)pc, (unsigned long)lr);
    if (has_far) printf("Fault address: %08lx\n", (unsigned long)far);
    printf("\nstack:\n");
    for (int i = 0; i < n; i++) printf("  #%-2d  %08lx\n", i, (unsigned long)bt[i]);
    printf("\nScan the QR code below for the Discord.\nPress START to exit.\n");
    draw_qr(bottom.frameBuffer, report);
    gfxFlushBuffers();

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        /* shown again each frame: the game's last frame can still swap the screens'
         * buffers after the consoles took them (citro3d presents when the GPU is done) */
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_TOP, false);
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        gspWaitForVBlank();
    }
    gfxExit();
    exit(1);   /* the app ends */
}
#endif

__attribute__((noreturn, noinline))
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
    /* pc: the call that panicked; lr: where its function returns to, the first
     * return address on the stack above this call's own */
    u32 ret = (u32)__builtin_return_address(0), bt[CRASH_BT_MAX], sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    int n = stack_return_addresses((const u32 *)sp, ret, bt, CRASH_BT_MAX);
    crash_screen(file, line, msg, ret - 4, n > 0 ? bt[0] : 0, false, 0, bt, n);
#else
    /* Host path: just exit. */
    fprintf(stderr, "PANIC %s:%d %s\n", file, line, msg);
    exit(1);
#endif
    __builtin_unreachable();
}
