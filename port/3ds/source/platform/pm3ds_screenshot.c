/*
 * pm3ds_screenshot.c — Select + R saves both screens as PNG files.
 *
 * The press is seen once a frame, after the frame is drawn (n64_shim/gfx_present.c):
 * both screens' render targets are copied out right away (a GPU copy each, about a
 * frame's wait), and a low-priority thread compresses and writes them to
 * /3ds/StarHaven/screenshots/<date>_<time>_top.png and _bottom.png while the game
 * runs on. The bottom screen says when they are saved. While Select is held, R
 * doesn't reach the game (n64_shim/os_cont.c): it would scroll a menu or swap
 * Mario's and the partner's turns.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>

#include "pm3ds_brand.h"
#include "pm3ds_log.h"

#define SHOT_DIR PM3DS_SD_DIR "/screenshots"

typedef struct {
    int w, h;
    u8 *rows;       /* h rows of a filter byte (0) and w RGB pixels: the PNG's raw image data */
} shot_image_t;

static shot_image_t s_img[2];       /* top, bottom */
static char s_name[80];             /* the files' common start */
static volatile int s_busy;         /* the thread is writing */
static volatile int s_result;       /* for the bottom screen: 1 saved, -1 failed, 0 nothing new */

/* ---- PNG ---------------------------------------------------------------------------- */
static void be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static bool chunk(FILE *f, const char *type, const u8 *data, u32 len) {
    u8 head[8], tail[4];
    be32(head, len);
    memcpy(head + 4, type, 4);
    uLong crc = crc32(0, head + 4, 4);
    if (len) crc = crc32(crc, data, len);
    be32(tail, (u32)crc);
    return fwrite(head, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

static bool write_png(const char *path, const shot_image_t *im) {
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uLong raw = (uLong)im->h * (1 + im->w * 3), size = compressBound(raw);
    u8 *z = (u8 *)malloc(size);
    if (!z) return false;
    bool ok = compress2(z, &size, im->rows, raw, Z_BEST_SPEED) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        u8 ihdr[13];
        be32(ihdr, im->w);
        be32(ihdr + 4, im->h);
        ihdr[8] = 8;    /* bits per channel */
        ihdr[9] = 2;    /* RGB */
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, (u32)size) &&
             chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0 && ok;
    } else {
        ok = false;
    }
    free(z);
    return ok;
}

static void writer(void *arg) {
    (void)arg;
    static const char *const side[2] = { "top", "bottom" };
    bool ok = true;
    mkdir(SHOT_DIR, 0777);
    for (int i = 0; i < 2; i++) {
        if (!s_img[i].rows) continue;
        char path[128];
        snprintf(path, sizeof path, SHOT_DIR "/%s_%s.png", s_name, side[i]);
        ok = write_png(path, &s_img[i]) && ok;
        free(s_img[i].rows);
        s_img[i].rows = NULL;
    }
    pm3ds_log(PM3DS_LOG_INFO, "screenshot: %s %s", s_name, ok ? "saved" : "failed");
    s_result = ok ? 1 : -1;
    s_busy = 0;
}

/* ---- capture -------------------------------------------------------------------------- */
/* a render target (w x h on screen, RGBA8, stored as the LCD's columns) as PNG rows */
static bool grab(C3D_RenderTarget *t, int w, int h, shot_image_t *out) {
    out->rows = NULL;
    if (!t) return false;
    u32 *lin = (u32 *)linearAlloc(w * h * 4);
    u8 *rows = (u8 *)malloc(h * (1 + w * 3));
    if (!lin || !rows) {
        if (lin) linearFree(lin);
        free(rows);
        return false;
    }
    C3D_SyncDisplayTransfer((u32 *)t->frameBuf.colorBuf, GX_BUFFER_DIM(h, w), lin, GX_BUFFER_DIM(h, w),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(lin, w * h * 4);
    for (int y = 0; y < h; y++) {
        u8 *r = rows + y * (1 + w * 3);
        *r++ = 0;   /* filter: none */
        for (int x = 0; x < w; x++) {
            u32 p = lin[x * h + (h - 1 - y)];   /* column-major, bottom row first */
            *r++ = p >> 24;
            *r++ = p >> 16;
            *r++ = p >> 8;
        }
    }
    linearFree(lin);
    out->w = w;
    out->h = h;
    out->rows = rows;
    return true;
}

static bool combo_pressed(void) {
    static bool was;
    u32 held = hidKeysHeld();
    bool now = (held & (KEY_SELECT | KEY_R)) == (KEY_SELECT | KEY_R);
    bool pressed = now && !was;
    was = now;
    return pressed;
}

/* after each frame is drawn (C3D_FrameEnd): top, the top screen's target (left eye) */
void pm3ds_screenshot_after_frame(C3D_RenderTarget *top) {
    extern C3D_RenderTarget *pm3ds_bottom_screen_target(void);
    if (!combo_pressed() || s_busy) return;
    time_t now = time(NULL);
    struct tm tm;
    char stamp[48], path[128];
    struct stat st;
    gmtime_r(&now, &tm);   /* the console's clock: its time() is local time */
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(s_name, sizeof s_name, "%s", stamp);
    for (int k = 2; k < 100; k++) {   /* two in the same second: a number after the time */
        snprintf(path, sizeof path, SHOT_DIR "/%s_top.png", s_name);
        if (stat(path, &st) != 0) break;
        snprintf(s_name, sizeof s_name, "%s_%d", stamp, k);
    }
    bool ok = grab(top, 400, 240, &s_img[0]);
    ok = grab(pm3ds_bottom_screen_target(), 320, 240, &s_img[1]) && ok;
    if (!ok) {
        for (int i = 0; i < 2; i++) {
            free(s_img[i].rows);
            s_img[i].rows = NULL;
        }
        pm3ds_log(PM3DS_LOG_WARN, "screenshot: no memory for the copy (linear free %u)", (unsigned)linearSpaceFree());
        s_result = -1;
        return;
    }
    /* compressing and writing take a few hundred ms: below the game's priority */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_busy = 1;
    if (!threadCreate(writer, NULL, 32 * 1024, prio < 0x3F ? prio + 1 : 0x3F, -2, true)) {
        writer(NULL);   /* no thread: write it now */
    }
}

/* for the bottom screen's notice: 1 saved, -1 failed, once; 0 nothing new */
int pm3ds_screenshot_result(void) {
    int r = s_result;
    s_result = 0;
    return r;
}
