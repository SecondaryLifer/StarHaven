/*
 * bottom_screen.c — the bottom-screen menu, drawn with citro3d.
 *
 * Styled after Paper Mario's pause menu and built from the game's own art
 * (status icons, partner portraits, world map, item icons) and its dialogue
 * font, all decoded from the linked N64 data. A rail of tabs down the left,
 * one tall panel beside it:
 *   Stats   location, chapter, star power, play time, HP/FP, level, BP,
 *           star points, coins, star pieces, and what the area and the room
 *           still hide (n64_shim/journal.c)
 *   Map     the pause-menu world map with visited places and Mario
 *   Party   partner portraits (joined / not yet), current partner
 *   Tattle  every enemy Goombario has tattled
 *   Found   the item spots collected, per area
 *   Recipes every dish Tayce T. cooks, which ones she has, how to make them
 * plus three buttons: Options (its own rail: settings, controls, the cheats of
 * a save that allows them, debug page; saved by platform/pm3ds_settings.c),
 * Items and a fast-forward toggle.
 *
 * In battle the rail shows HP/FP/star power/coins and the panel the menu the
 * game is waiting on (commands, moves, items, targets) as buttons to tap, or
 * an A/B pad for action commands when no menu is open. Item and partner lists
 * in the world show the same way (n64_shim/touch_menu.c).
 *
 * Game data comes from n64_shim/hud_info.c through pm3ds_hud_info.h, so this
 * file needs no decomp headers.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pm3ds_brand.h"

#include "pm3ds_fm_touch.h"
#include "pm3ds_cheats.h"
#include "pm3ds_hud_info.h"
#include "pm3ds_rando.h"
#include "pm3ds_settings.h"
#include "pm3ds_langpack.h"
#include "pm3ds_lang.h"
#include "pm3ds_log.h"
#include "pm3ds_touch_menu.h"
#include "pm3ds_save_import.h"
#include "pm3ds_update.h"

extern void pm3ds_set_view_mode(int mode);   /* 0 Original, 1 Fill, 2 Wide, 3 Sharp wide */
extern int pm3ds_view_mode(void);
static const char *const s_view_names[4] = { "Original", "Fill", "Wide", "Sharp wide" };
extern shaderProgram_s *pm3ds_gfx_program(void);
extern int pm3ds_gfx_uloc_screen(void);
extern const char *pm3ds_log_recent(int i);   /* 0 = newest */
extern char pm3ds_perf_text[3][64];

int pm3ds_console_debug;   /* Debug page active (kept for the logger's API) */


/* ---- colours (r, g, b, a) ---------------------------------------------- */
#define RGBA(r, g, b, a) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | ((u32)(a) << 24))
#define COL_BG_TOP     RGBA(250, 196, 110, 255)
#define COL_BG_BOTTOM  RGBA(222, 130, 60, 255)
#define COL_PANEL      RGBA(255, 247, 222, 255)
#define COL_PANEL_EDGE RGBA(116, 72, 40, 255)
#define COL_SHADOW     RGBA(80, 40, 10, 90)
#define COL_TAB        RGBA(205, 160, 105, 255)
#define COL_TAB_ON     RGBA(255, 247, 222, 255)
#define COL_HP         RGBA(232, 56, 48, 255)
#define COL_FP         RGBA(64, 176, 232, 255)
#define COL_BAR_BACK   RGBA(150, 120, 90, 255)
#define COL_WHITE      RGBA(255, 255, 255, 255)
#define COL_BTN        RGBA(116, 72, 40, 230)

/* ---- GPU state ----------------------------------------------------------- */
typedef struct { float x, y, z, w; u8 r, g, b, a; float u0, v0, u1, v1; } vtx_t;
#define UI_MAX_VERTS 12000

static C3D_RenderTarget *s_target;
static C3D_Mtx s_mtx, s_mtx_top;   /* bottom (320x240) and top (400x240) pixel space */
static C3D_AttrInfo s_attr;
static C3D_BufInfo s_buf;
static vtx_t *s_vbuf;
static int s_vcount, s_batch_start;
static C3D_Tex *s_cur_tex;

/* textures: UI atlas (art + white + circle), font (two palettes), world map */
static C3D_Tex s_atlas, s_font, s_map;
static bool s_atlas_ok, s_font_ok, s_map_ok;
#define ATLAS_W 256
#define ATLAS_H 256
typedef struct { short x, y, w, h; } rect_t;
static rect_t s_img[UI_IMG_COUNT];
static rect_t s_white, s_circle;
static u8 s_glyph_w[256];
static int s_glyphs;
static C3D_Tex s_small;      /* libctru's 8x8 console font: crisp small text */
static bool s_small_ok;

/* ---- state --------------------------------------------------------------- */
enum { TAB_STATS, TAB_MAP, TAB_PARTY, TAB_LOG, TAB_FOUND, TAB_COOK, TAB_BADGES, TAB_DEBUG, TAB_OPTIONS, TAB_CONTROLS, TAB_CHEATS };
#define TAB_GAME_LAST TAB_BADGES  /* the game's pages (Badges opens from its button, not the rail) */
#define NUM_GAME_TABS (TAB_COOK + 1)
static int s_tab = TAB_STATS;
static pm3ds_hud_info_t s_info;
static unsigned s_frame;
static pm3ds_touch_menu_t s_tm;   /* the game's open list menu (pm3ds_touch_menu.h) */
static int s_tm_page, s_tm_kind_seen, s_tm_count_seen;
static float s_map_cx = -1, s_map_cy = -1;   /* map view centre in map pixels */
static bool s_map_follow = true;

/* ---- texture building ---------------------------------------------------- */
static u8 s_morton[64];

static void init_morton(void) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int m = 0;
            for (int b = 0; b < 3; b++) {
                m |= ((x >> b) & 1) << (2 * b);
                m |= ((y >> b) & 1) << (2 * b + 1);
            }
            s_morton[y * 8 + x] = (u8)m;
        }
}

static size_t tiled_index(int x, int gy, int w) {
    return (size_t)((gy & ~7) * w + (x & ~7) * 8) + s_morton[(gy & 7) * 8 + (x & 7)];
}

/* linear RGBA8 (R,G,B,A bytes, row 0 = top) -> tiled GPU_RGBA8, same flip
 * convention as the game renderer: texel (x, y) samples at (x/w, y/h) */
static void upload_rgba8(C3D_Tex *tex, const u8 *rgba, int w, int h) {
    u8 *tiled = (u8 *)malloc((size_t)w * h * 4);
    if (!tiled) return;
    for (int gy = 0; gy < h; gy++) {
        int sy = h - 1 - gy;
        for (int x = 0; x < w; x++) {
            const u8 *p = rgba + ((size_t)sy * w + x) * 4;
            u8 *d = tiled + tiled_index(x, gy, w) * 4;
            d[0] = p[3]; d[1] = p[2]; d[2] = p[1]; d[3] = p[0];
        }
    }
    C3D_TexUpload(tex, tiled);
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)tex->data, tex->size);   /* the frame flushes only its vertices (gfx_present.c) */
    free(tiled);
}

/* linear RGBA8 -> tiled GPU_RGB565 (the opaque world map) */
static void upload_rgb565(C3D_Tex *tex, const u8 *rgba, int w, int h) {
    u16 *tiled = (u16 *)malloc((size_t)w * h * 2);
    if (!tiled) return;
    for (int gy = 0; gy < h; gy++) {
        int sy = h - 1 - gy;
        for (int x = 0; x < w; x++) {
            const u8 *p = rgba + ((size_t)sy * w + x) * 4;
            tiled[tiled_index(x, gy, w)] = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    C3D_TexUpload(tex, tiled);
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)tex->data, tex->size);
    free(tiled);
}

static void rgba16(u16 v, u8 *d) {
    u8 r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
    d[0] = (u8)((r << 3) | (r >> 2)); d[1] = (u8)((g << 3) | (g >> 2)); d[2] = (u8)((b << 3) | (b >> 2));
    d[3] = (v & 1) ? 255 : 0;
}

static u16 be16(const void *p) { const u8 *b = (const u8 *)p; return (u16)((b[0] << 8) | b[1]); }

/* decode an N64 image into a block of an RGBA8 canvas cw pixels wide */
static void decode_image(const pm3ds_ui_image_t *im, u8 *canvas, int cw, int ox, int oy) {
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            u8 *d = canvas + ((size_t)(oy + y) * cw + ox + x) * 4;
            int i = y * im->w + x;
            switch (im->fmt) {
                case UI_FMT_RGBA16: rgba16(be16(im->img + i * 2), d); break;
                case UI_FMT_RGBA32: memcpy(d, im->img + i * 4, 4); break;
                case UI_FMT_CI4: {
                    u8 b = im->img[i >> 1];
                    rgba16(be16(&im->pal[(i & 1) ? (b & 15) : (b >> 4)]), d);
                    break;
                }
                case UI_FMT_CI8: rgba16(be16(&im->pal[im->img[i]]), d); break;
                case UI_FMT_IA8: {
                    u8 b = im->img[i];
                    d[0] = d[1] = d[2] = (u8)((b >> 4) * 17);
                    d[3] = (u8)((b & 15) * 17);
                    break;
                }
                case UI_FMT_I4: {
                    u8 b = im->img[i >> 1];
                    u8 v = (u8)(((i & 1) ? (b & 15) : (b >> 4)) * 17);
                    d[0] = d[1] = d[2] = d[3] = v;
                    break;
                }
            }
        }
    }
}

static void build_atlas(void) {
    u8 *canvas = (u8 *)calloc(ATLAS_W * ATLAS_H, 4);
    if (!canvas) return;
    /* shelf packing in id order */
    int x = 0, y = 0, shelf = 0;
    for (int id = 0; id < UI_IMG_COUNT; id++) {
        pm3ds_ui_image_t im;
        s_img[id].w = 0;
        if (id == UI_IMG_WORLD_MAP || !pm3ds_ui_get_image(id, &im)) continue;
        if (x + im.w > ATLAS_W) { x = 0; y += shelf + 1; shelf = 0; }
        if (y + im.h > ATLAS_H - 34) break;
        decode_image(&im, canvas, ATLAS_W, x, y);
        s_img[id] = (rect_t){ (short)x, (short)y, (short)im.w, (short)im.h };
        x += im.w + 1;
        if (im.h > shelf) shelf = im.h;
    }
    /* a white block and an antialiased 32x32 circle (rounded corners) */
    int by = ATLAS_H - 32;
    s_white = (rect_t){ 0, (short)by, 8, 8 };
    for (int j = 0; j < 8; j++) memset(canvas + ((size_t)(by + j) * ATLAS_W) * 4, 255, 8 * 4);
    s_circle = (rect_t){ 16, (short)by, 32, 32 };
    for (int j = 0; j < 32; j++)
        for (int i = 0; i < 32; i++) {
            float dx = i + 0.5f - 16.0f, dy = j + 0.5f - 16.0f;
            float d = 16.0f - __builtin_sqrtf(dx * dx + dy * dy);
            float a = d >= 1.0f ? 1.0f : d <= 0.0f ? 0.0f : d;
            u8 *p = canvas + ((size_t)(by + j) * ATLAS_W + 16 + i) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (u8)(a * 255.0f);
        }
    /* a rebuild uploads into the same texture: the GPU may still be drawing
     * the last frame from it */
    if (s_atlas_ok || C3D_TexInit(&s_atlas, ATLAS_W, ATLAS_H, GPU_RGBA8)) {
        upload_rgba8(&s_atlas, canvas, ATLAS_W, ATLAS_H);
        C3D_TexSetFilter(&s_atlas, GPU_LINEAR, GPU_NEAREST);
        C3D_TexSetWrap(&s_atlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s_atlas_ok = true;
    }
    free(canvas);
}

static void build_map(void) {
    pm3ds_ui_image_t im;
    if (!pm3ds_ui_get_image(UI_IMG_WORLD_MAP, &im)) return;
    u8 *canvas = (u8 *)calloc(512 * 512, 4);
    if (!canvas) return;
    decode_image(&im, canvas, 512, 0, 0);
    if (s_map_ok || C3D_TexInit(&s_map, 512, 512, GPU_RGB565)) {
        upload_rgb565(&s_map, canvas, 512, 512);
        C3D_TexSetFilter(&s_map, GPU_LINEAR, GPU_NEAREST);
        C3D_TexSetWrap(&s_map, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s_map_ok = true;
    }
    free(canvas);
}

/* Release builds start without the game's images (pm3ds_rom.c puts them
 * back once the ROM is loaded), so the menu art is built again then. */
void pm3ds_bottom_screen_reload_art(void) {
    build_atlas();
    build_map();
}

static u32 s_std_ink = 0x303030;   /* build_font: the standard palette's ink (RGB) */

/* font texture 256x512: glyphs 16 per row, standard palette in the top half,
 * white palette in the bottom half */
static void build_font(void) {
    pm3ds_ui_font_t f;
    if (!pm3ds_ui_get_font(&f)) return;
    u8 *canvas = (u8 *)calloc(256 * 512, 4);
    if (!canvas) return;
    s_glyphs = f.glyph_count < 176 ? f.glyph_count : 176;
    for (int half = 0; half < 2; half++) {
        const u16 *pal = f.palettes + (half == 0 ? 0x0A : 0x00) * 8;   /* MSG_PAL_STANDARD / MSG_PAL_WHITE */
        for (int g = 0; g < s_glyphs; g++) {
            pm3ds_ui_image_t im = { f.rasters + g * 128, pal, 16, 16, UI_FMT_CI4 };
            decode_image(&im, canvas, 256, (g % 16) * 16, half * 256 + (g / 16) * 16);
        }
    }
    for (int g = 0; g < s_glyphs; g++) s_glyph_w[g] = f.widths[g];
    {   /* the standard palette's ink (its most used opaque colour), which Chinese and
         * Japanese glyphs take on so a subtitle stays the brown it is in English (GitHub #85) */
        static u32 count[64];
        static u32 rgb[64];
        int used = 0;
        memset(count, 0, sizeof count);
        for (int i = 0; i < 256 * 256; i++) {
            const u8 *p = canvas + i * 4;
            if (p[3] < 200) continue;
            u32 v = p[0] | p[1] << 8 | p[2] << 16;
            int k = 0;
            while (k < used && rgb[k] != v) k++;
            if (k == used) { if (used == 64) continue; rgb[used++] = v; }
            count[k]++;
        }
        /* the most used dark colour (the glyphs' light rim is used about as much) */
        int best = -1;
        u32 total = 0;
        for (int k = 0; k < used; k++) total += count[k];
        for (int k = 0; k < used; k++) {
            u32 v = rgb[k], luma = ((v & 0xFF) * 3 + ((v >> 8) & 0xFF) * 6 + ((v >> 16) & 0xFF)) / 10;
            if (luma < 128 && count[k] * 20 >= total && (best < 0 || count[k] > count[best])) best = k;
        }
        if (best >= 0) s_std_ink = rgb[best];
    }
    if (C3D_TexInit(&s_font, 256, 512, GPU_RGBA8)) {
        upload_rgba8(&s_font, canvas, 256, 512);
        C3D_TexSetFilter(&s_font, GPU_NEAREST, GPU_LINEAR);
        C3D_TexSetWrap(&s_font, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s_font_ok = true;
    }
    free(canvas);
}

/* the 8x8 console font (1 bpp, MSB = left pixel), 16x8 glyphs in 128x64 */
static void build_small_font(void) {
    PrintConsole *pc = consoleGetDefault();
    if (!pc || !pc->font.gfx) return;
    u8 *canvas = (u8 *)calloc(128 * 64, 4);
    if (!canvas) return;
    int n = pc->font.numChars < 128 ? pc->font.numChars : 128;
    for (int c = 0; c < n; c++) {
        const u8 *g = pc->font.gfx + c * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                u8 *p = canvas + ((size_t)((c / 16) * 8 + y) * 128 + (c % 16) * 8 + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = (g[y] & (0x80 >> x)) ? 255 : 0;
            }
    }
    if (C3D_TexInit(&s_small, 128, 64, GPU_RGBA8)) {
        upload_rgba8(&s_small, canvas, 128, 64);
        C3D_TexSetFilter(&s_small, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&s_small, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s_small_ok = true;
    }
    free(canvas);
}

/* ---- primitives ---------------------------------------------------------- */
#if VERSION_IQUE
static void iq_upload(void);
#endif
static void flush(void) {
    if (s_vcount > s_batch_start) {
#if VERSION_IQUE
        iq_upload();
#endif
        C3D_DrawArrays(GPU_TRIANGLES, s_batch_start, s_vcount - s_batch_start);
    }
    s_batch_start = s_vcount;
}

/* ---- the 3DS system font: characters the game's font lacks (Japanese) ---- */
static C3D_Tex *s_sys_sheet;    /* one per glyph sheet (the CTR font has ~1500 small ones) */
static int s_sys_sheets = -1;   /* -1: not loaded yet, 0: none */
static CFNT_s *s_sys_font;

static bool sys_font_load(void) {
    if (s_sys_sheets >= 0) return s_sys_sheets > 0;
    s_sys_sheets = 0;
    Result rc = fontEnsureMapped();
    if (R_FAILED(rc)) {
        pm3ds_log(PM3DS_LOG_WARN, "ui: no system font (rc %08lX), Japanese text not shown", (unsigned long)rc);
        return false;
    }
    s_sys_font = fontGetSystemFont();
    TGLP_s *g = s_sys_font ? fontGetGlyphInfo(s_sys_font) : NULL;
    if (!g) return false;
    pm3ds_log(PM3DS_LOG_INFO, "ui: system font %d sheets %ux%u fmt %d cell %ux%u",
              g->nSheets, g->sheetWidth, g->sheetHeight, g->sheetFmt, g->cellWidth, g->cellHeight);
    int n = g->nSheets;
    s_sys_sheet = (C3D_Tex *)calloc((size_t)n, sizeof *s_sys_sheet);
    if (!s_sys_sheet) return false;
    for (int i = 0; i < n; i++) {   /* the sheets stay where the system put them (as citro2d does) */
        C3D_Tex *t = &s_sys_sheet[i];
        memset(t, 0, sizeof *t);
        t->data = fontGetGlyphSheetTex(s_sys_font, i);
        t->fmt = g->sheetFmt;
        t->size = g->sheetSize;
        t->width = g->sheetWidth;
        t->height = g->sheetHeight;
        t->param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                   GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
    }
    s_sys_sheets = n;
    return n > 0;
}

/* ---- Chinese: the built-in font (pm3ds_cjk_font.inc), and on iQue the ROM's own ---- */
#include "pm3ds_cjk_font.inc"
#define CJK_COLS 32
static C3D_Tex s_cjk;           /* alpha only: both sizes, 16x16 cells, 10 px first */
static int s_cjk_state;         /* 0 not built, 1 ok, -1 failed */

static bool cjk_build(void) {
    if (s_cjk_state) return s_cjk_state > 0;
    s_cjk_state = -1;
    int cells = PM3DS_CJK_CHARS * 2, h = 16;
    while (h < (cells + CJK_COLS - 1) / CJK_COLS * 16) h *= 2;
    if (h > 1024) return false;
    const int w = CJK_COLS * 16;
    u8 *a = (u8 *)calloc((size_t)w * h, 1);
    if (!a) return false;
    for (int c = 0; c < cells; c++) {
        const uint16_t *rows = c < PM3DS_CJK_CHARS ? pm3ds_cjk10_bits[c] : pm3ds_cjk12_bits[c - PM3DS_CJK_CHARS];
        int ox = (c % CJK_COLS) * 16, oy = (c / CJK_COLS) * 16;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                if (rows[y] & (0x8000 >> x)) a[(size_t)(oy + y) * w + ox + x] = 255;
    }
    u8 *tiled = (u8 *)malloc((size_t)w * h);
    if (tiled && C3D_TexInit(&s_cjk, w, h, GPU_A8)) {
        for (int gy = 0; gy < h; gy++)
            for (int x = 0; x < w; x++) tiled[tiled_index(x, gy, w)] = a[(size_t)(h - 1 - gy) * w + x];
        C3D_TexUpload(&s_cjk, tiled);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)s_cjk.data, s_cjk.size);
        C3D_TexSetFilter(&s_cjk, GPU_NEAREST, GPU_NEAREST);   /* pixel font: drawn 1:1 where it can */
        C3D_TexSetWrap(&s_cjk, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s_cjk_state = 1;
    }
    free(tiled);
    free(a);
    return s_cjk_state > 0;
}

static int cjk_index(uint32_t cp) {
    int lo = 0, hi = PM3DS_CJK_CHARS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (pm3ds_cjk_cp[mid] == cp) return mid;
        if (pm3ds_cjk_cp[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

#if VERSION_IQUE
/* The iQue ROM's 16x16 font has every character its messages use (item and
 * enemy names, tattles): those come here as code points (n64_shim/touch_menu.c)
 * and are drawn with it, a character decoded into a cache slot when first seen. */
#include "../n64_shim/ique_charmap.inc"
extern unsigned char charset_standard_OFFSET[];   /* src/charset/charset.c: the whole font */
#define IQ_COLS 16
#define IQ_SLOTS (IQ_COLS * 16)
static C3D_Tex s_iq;            /* RGBA, the white palette, 256 slots */
static u8 *s_iq_canvas;
static bool s_iq_ok, s_iq_dirty;
static int16_t s_iq_glyph[IQ_SLOTS];      /* the glyph in each slot, -1 none */
static uint32_t s_iq_used[IQ_SLOTS];      /* the frame it was last drawn in */
static int s_iq_next;
static uint32_t s_ui_frame;

static int iq_glyph_of(uint32_t cp) {
    int lo = 0, hi = PM3DS_IQUE_CHARS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t k = pm3ds_ique_cp_glyph[mid] >> 12;
        if (k == cp) return (int)(pm3ds_ique_cp_glyph[mid] & 0xFFF);
        if (k < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void decode_image(const pm3ds_ui_image_t *im, u8 *canvas, int cw, int ox, int oy);
static void upload_rgba8(C3D_Tex *tex, const u8 *rgba, int w, int h);

/* the slot holding glyph, decoding it if needed; -1 if the font isn't loaded or
 * every slot is in use this frame (the GPU draws the frame later: a slot drawn
 * in it can't be overwritten). The font's glyphs have an outline (index 6
 * around the fill, index 1), so they take the palette of the text around them
 * like the game font's Latin glyphs (build_font): std_pal MSG_PAL_STANDARD,
 * dark with a light edge, else MSG_PAL_WHITE. A tint over a single colour
 * would merge the two into a blob. */
static int iq_slot(int glyph, bool std_pal) {
    int key = glyph | (std_pal ? 0x1000 : 0);
    pm3ds_ui_font_t f;
    if (!pm3ds_ui_get_font(&f)) return -1;
    if (!s_iq_ok) {
        s_iq_canvas = (u8 *)calloc(IQ_COLS * 16 * IQ_COLS * 16, 4);
        if (!s_iq_canvas || !C3D_TexInit(&s_iq, IQ_COLS * 16, IQ_COLS * 16, GPU_RGBA8)) return -1;
        C3D_TexSetFilter(&s_iq, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&s_iq, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        memset(s_iq_glyph, 0xFF, sizeof s_iq_glyph);
        memset(s_iq_used, 0xFF, sizeof s_iq_used);
        s_iq_ok = true;
    }
    for (int i = 0; i < IQ_SLOTS; i++)
        if (s_iq_glyph[i] == key) { s_iq_used[i] = s_ui_frame; return i; }
    for (int tries = 0; tries < IQ_SLOTS; tries++) {
        int i = s_iq_next;
        s_iq_next = (s_iq_next + 1) % IQ_SLOTS;
        if (s_iq_glyph[i] >= 0 && s_iq_used[i] == s_ui_frame) continue;
        pm3ds_ui_image_t im = { charset_standard_OFFSET + (size_t)glyph * 128,
                                f.palettes + (std_pal ? 0x0A : 0x00) * 8, 16, 16, UI_FMT_CI4 };
        decode_image(&im, s_iq_canvas, IQ_COLS * 16, (i % IQ_COLS) * 16, (i / IQ_COLS) * 16);
        s_iq_glyph[i] = (int16_t)key;
        s_iq_used[i] = s_ui_frame;
        s_iq_dirty = true;
        return i;
    }
    return -1;
}

/* before a draw (flush): slots filled since the last upload were not drawn
 * from in this frame, so the draws queued before keep their glyphs */
static void iq_upload(void) {
    if (s_iq_ok && s_iq_dirty) {
        upload_rgba8(&s_iq, s_iq_canvas, IQ_COLS * 16, IQ_COLS * 16);
        s_iq_dirty = false;
    }
}
#endif

static bool is_sys_sheet(const C3D_Tex *t) {
    if (t == &s_cjk) return true;   /* alpha only, like the system font's sheets */
    return s_sys_sheets > 0 && t >= s_sys_sheet && t < s_sys_sheet + s_sys_sheets;
}

/* one system-font character at x, y (the game font's 16-pixel cell, scaled);
 * returns its advance */
static void quad2(C3D_Tex *t, float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, u32 ct, u32 cb);
static float sys_glyph(float x, float y, uint32_t cp, float scale, u32 c, bool draw, bool std_pal) {
#if VERSION_IQUE
    /* full-size text: the ROM's font, like the game's own text, drawn 1:1 (scaled it
     * blurs); smaller text: the built-in pixel font, sharper there (GitHub #85) */
    int iq = scale >= 0.95f || cjk_index(cp) < 0 ? iq_glyph_of(cp) : -1;
    if (iq >= 0) {
        float k = scale >= 0.95f && scale <= 1.25f ? 1.0f : scale;
        if (draw) {
            int slot = iq_slot(iq, std_pal);
            if (slot >= 0) {
                float gx = (slot % IQ_COLS) * 16.0f, gy = (slot / IQ_COLS) * 16.0f, n = IQ_COLS * 16.0f;
                float x0 = k == 1.0f ? (float)(int)(x + 0.5f) : x, y0 = k == 1.0f ? (float)(int)(y + 0.5f) : y;
                quad2(&s_iq, x0, y0 + 0.5f * k, x0 + 16 * k, y0 + 15.5f * k,
                      gx / n, (gy + 0.5f) / n, (gx + 16) / n, (gy + 15.5f) / n, c, c);
            }
        }
        return 16 * k;
    }
#endif
    /* the alpha-only fonts: game-font text in the standard palette takes its ink, tinted */
    if (std_pal) {
        u32 r = ((c & 0xFF) * (s_std_ink & 0xFF)) / 255, gg = (((c >> 8) & 0xFF) * ((s_std_ink >> 8) & 0xFF)) / 255,
            b = (((c >> 16) & 0xFF) * ((s_std_ink >> 16) & 0xFF)) / 255;
        c = (c & 0xFF000000u) | r | gg << 8 | b << 16;
    }
    /* Japanese: the system font has every kana and kanji; the Chinese pixel font's
     * kanji beside its kana mixed two fonts in one word (GitHub #85) */
    int ci = pm3ds_cfg.language == PM3DS_LANG_JA && sys_font_load() ? -1 : cjk_index(cp);
    if (ci >= 0 && cjk_build()) {
        /* 10 px under 0.9 (small text, list rows), else 12 px; 1:1 near the size
         * each matches beside the game's font, scaled otherwise */
        bool small = scale < 0.9f;
        float k = small ? (scale >= 0.55f ? 1.0f : scale / 0.55f) : (scale <= 1.15f ? 1.0f : scale);
        int cell = small ? ci : PM3DS_CJK_CHARS + ci, asc = small ? PM3DS_CJK10_ASCENT : PM3DS_CJK12_ASCENT;
        float adv = (small ? pm3ds_cjk10_adv[ci] : pm3ds_cjk12_adv[ci]) * k;
        if (draw) {
            float gx = (cell % CJK_COLS) * 16.0f, gy = (cell / CJK_COLS) * 16.0f;
            float tw = (float)s_cjk.width, th = (float)s_cjk.height;
            float x0 = (float)(int)(x + 0.5f), y0 = (float)(int)(y + 13.0f * scale - asc * k + 0.5f);   /* baselines level */
            quad2(&s_cjk, x0, y0, x0 + 16 * k, y0 + 16 * k, gx / tw, gy / th, (gx + 16) / tw, (gy + 16) / th, c, c);
        }
        return adv;
    }
    if (!sys_font_load()) return 8 * scale;
    TGLP_s *g = fontGetGlyphInfo(s_sys_font);
    float k = 14.0f * scale / (float)g->cellHeight;
    fontGlyphPos_s pos;
    fontCalcGlyphPos(&pos, s_sys_font, fontGlyphIndexFromCodePoint(s_sys_font, cp), GLYPH_POS_CALC_VTXCOORD, k, k);
    if (draw && pos.sheetIndex >= 0 && pos.sheetIndex < s_sys_sheets)
        quad2(&s_sys_sheet[pos.sheetIndex], x + pos.vtxcoord.left, y + 1.5f * scale + pos.vtxcoord.top,
              x + pos.vtxcoord.right, y + 1.5f * scale + pos.vtxcoord.bottom,
              pos.texcoord.left, pos.texcoord.top, pos.texcoord.right, pos.texcoord.bottom, c, c);
    return pos.xAdvance;
}

static void use_tex(C3D_Tex *t) {
    if (t != s_cur_tex) {
        flush();
        bool was_sys = s_cur_tex && is_sys_sheet(s_cur_tex), sys = is_sys_sheet(t);
        if (sys != was_sys || !s_cur_tex) {
            /* system-font sheets hold only alpha: colour from the vertex */
            C3D_TexEnv *env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            if (sys) {
                C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, 0, 0);
                C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
                C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
                C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
            } else {
                C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
                C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            }
        }
        s_cur_tex = t;
        C3D_TexBind(0, t);
    }
}

static void vert(float x, float y, u32 c, float u, float v) {
    vtx_t *o = &s_vbuf[s_vcount++];
    o->x = x; o->y = y; o->z = 0.0f; o->w = 1.0f;
    o->r = c & 255; o->g = (c >> 8) & 255; o->b = (c >> 16) & 255; o->a = c >> 24;
    o->u0 = u; o->v0 = v; o->u1 = o->v1 = 0.0f;
}

/* textured quad with a vertical colour gradient (ct top, cb bottom) */
static void quad2(C3D_Tex *t, float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1, u32 ct, u32 cb) {
    if (s_vcount + 6 > UI_MAX_VERTS) return;
    use_tex(t);
    vert(x0, y0, ct, u0, v0); vert(x1, y0, ct, u1, v0); vert(x0, y1, cb, u0, v1);
    vert(x1, y0, ct, u1, v0); vert(x1, y1, cb, u1, v1); vert(x0, y1, cb, u0, v1);
}

static void atlas_quad(rect_t r, float x0, float y0, float x1, float y1, u32 c) {
    quad2(&s_atlas, x0, y0, x1, y1, (float)r.x / ATLAS_W, (float)r.y / ATLAS_H,
          (float)(r.x + r.w) / ATLAS_W, (float)(r.y + r.h) / ATLAS_H, c, c);
}

static void gradient(float x0, float y0, float x1, float y1, u32 ct, u32 cb) {
    float u = (s_white.x + 4.0f) / ATLAS_W, v = (s_white.y + 4.0f) / ATLAS_H;
    quad2(&s_atlas, x0, y0, x1, y1, u, v, u, v, ct, cb);
}

static void fill(float x0, float y0, float x1, float y1, u32 c) { gradient(x0, y0, x1, y1, c, c); }

/* rounded rectangle from the circle's quarters */
static void round_rect(float x0, float y0, float x1, float y1, float r, u32 c) {
    float cu0 = (float)s_circle.x / ATLAS_W, cv0 = (float)s_circle.y / ATLAS_H;
    float cu1 = (float)(s_circle.x + s_circle.w) / ATLAS_W, cv1 = (float)(s_circle.y + s_circle.h) / ATLAS_H;
    float cum = (cu0 + cu1) / 2, cvm = (cv0 + cv1) / 2;
    if (r > (x1 - x0) / 2) r = (x1 - x0) / 2;
    if (r > (y1 - y0) / 2) r = (y1 - y0) / 2;
    quad2(&s_atlas, x0, y0, x0 + r, y0 + r, cu0, cv0, cum, cvm, c, c);
    quad2(&s_atlas, x1 - r, y0, x1, y0 + r, cum, cv0, cu1, cvm, c, c);
    quad2(&s_atlas, x0, y1 - r, x0 + r, y1, cu0, cvm, cum, cv1, c, c);
    quad2(&s_atlas, x1 - r, y1 - r, x1, y1, cum, cvm, cu1, cv1, c, c);
    fill(x0 + r, y0, x1 - r, y1, c);
    fill(x0, y0 + r, x0 + r, y1 - r, c);
    fill(x1 - r, y0 + r, x1, y1 - r, c);
}

/* a pause-menu style panel: shadow, brown rim, cream face */
static void panel(float x0, float y0, float x1, float y1, u32 face) {
    round_rect(x0 + 3, y0 + 3, x1 + 3, y1 + 3, 8, COL_SHADOW);
    round_rect(x0, y0, x1, y1, 8, COL_PANEL_EDGE);
    round_rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 6, face);
}

static void image(int id, float x, float y, float scale, u32 c) {
    rect_t r = s_img[id];
    if (r.w == 0) return;
    atlas_quad(r, x, y, x + r.w * scale, y + r.h * scale, c);
}

/* ---- text (the game's font) ---------------------------------------------- */
static int glyph_of(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c > 0x20 && c - 0x20 < s_glyphs) return c - 0x20;   /* past 0x7F: the game's accented letters */
    return -1;   /* space */
}

/* a phrase with Chinese or Japanese in it is drawn in that font all through, its
 * Latin letters and digits too: one font per phrase (GitHub #85) */
static bool has_cjk(const char *s) {
    for (int len; *s; s += len) {
        uint32_t cp = pm3ds_txt_next(s, &len);
        if ((cp & 0x80000000u) && (cp & 0x1FFFFF) >= 0x2E80) return true;   /* not ♪ ♥ or œ */
    }
    return false;
}

/* s already in the chosen language */
static float text_width_raw(const char *s, float scale) {
    float w = 0;
    bool cjk = has_cjk(s);
    while (*s) {
        int len;
        uint32_t cp = pm3ds_txt_next(s, &len);
        s += len;
        if (cjk && cp >= 0x20 && cp < 0x7F) cp |= 0x80000000u;
        if (cp & 0x80000000u) { w += sys_glyph(0, 0, cp & 0x1FFFFF, scale, 0, false, false); continue; }
        int g = glyph_of((char)cp);
        w += (g >= 0 && g < s_glyphs ? s_glyph_w[g] : 5) * scale;
    }
    return w;
}

static float text_width(const char *s, float scale) {
    return text_width_raw(T(s), scale);   /* the port's own labels in the chosen language (pm3ds_lang.c) */
}

/* white = 1 picks the white palette; c tints the result */
static float text(float x, float y, const char *s, float scale, int white, u32 c) {
    if (!s_font_ok) return x;
    s = T(s);
    bool cjk = has_cjk(s);
    for (int len; *s; s += len) {
        uint32_t cp = pm3ds_txt_next(s, &len);
        if (cjk && cp >= 0x20 && cp < 0x7F) cp |= 0x80000000u;
        if (cp & 0x80000000u) {   /* not in the game's font (Japanese, Chinese) */
            x += sys_glyph(x, y, cp & 0x1FFFFF, scale, c, true, !white);
            continue;
        }
        int g = glyph_of((char)cp);
        if (g >= 0 && g < s_glyphs) {
            /* only the glyph's own columns (and half a texel off the cell's
             * top and bottom): at the cell's edge the GPU can sample the
             * neighbouring glyph and leave a stray line after the text */
            float gx = (g % 16) * 16.0f, gy = (white ? 256.0f : 0.0f) + (g / 16) * 16.0f;
            float cw = s_glyph_w[g] + 2 < 16 ? s_glyph_w[g] + 2 : 16;
            quad2(&s_font, x, y + 0.5f * scale, x + cw * scale, y + 15.5f * scale,
                  gx / 256.0f, (gy + 0.5f) / 512.0f, (gx + cw) / 256.0f, (gy + 15.5f) / 512.0f, c, c);
            x += s_glyph_w[g] * scale;
        } else {
            x += 5 * scale;
        }
    }
    return x;
}

static void text_center(float cx, float y, const char *s, float scale, int white, u32 c) {
    text(cx - text_width(s, scale) / 2, y, s, scale, white, c);
}

/* 8x8 console-font text, 1:1; wraps at max_cols, returns the next line's y */
static float small_text(float x, float y, const char *str, int max_cols, u32 c) {
    if (!s_small_ok) return y + 9;
    str = T(str);
    int col = 0;
    float x0 = x;
    for (int len; *str; str += len) {
        uint32_t cp = pm3ds_txt_next(str, &len);
        if (col >= max_cols) { col = 0; x = x0 + 16; y += 9; }   /* continuation lines indented */
        if (cp & 0x80000000u) {   /* Japanese: the system font, about the 8x8 size */
            sys_glyph(x - 1, y - 2, cp & 0x1FFFFF, 0.72f, c, true, false);
            x += 10; col += 1;
            continue;
        }
        unsigned char ch = (unsigned char)cp;
        if (ch >= 0x80 && ch < 0xB0) ch = (unsigned char)"AAAACEEEEIIIINOOOOUUUUsaaaaceeeeiiiinoooouuuu!?a"[ch - 0x80];   /* accents: the console font has none */
        if (ch > 32 && ch < 128) {
            float gx = (ch % 16) * 8.0f, gy = (ch / 16) * 8.0f;
            quad2(&s_small, x, y, x + 8, y + 8, gx / 128.0f, gy / 64.0f, (gx + 8) / 128.0f, (gy + 8) / 64.0f, c, c);
        }
        x += 8; col++;
    }
    return y + 9;
}

static void bar(float x, float y, float w, float h, int v, int max, u32 col) {
    round_rect(x - 1, y - 1, x + w + 1, y + h + 1, h / 2 + 1, COL_PANEL_EDGE);
    round_rect(x, y, x + w, y + h, h / 2, COL_BAR_BACK);
    if (max > 0 && v > 0) {
        float fw = w * (v > max ? max : v) / (float)max;
        if (fw < h) fw = h;
        round_rect(x, y, x + fw, y + h, h / 2, col);
    }
}

/* ---- layout -------------------------------------------------------------- */
/* A rail of tabs down the left, one tall panel next to it, the button row
 * underneath. Pages draw between CX0 and CX1, a title at HEAD_Y (and a
 * second line under it), their body from BODY_Y. */
#define RAIL_X1 50
#define RAIL_Y0 6
#define RAIL_TAB_H 33
#define PANEL_X0 RAIL_X1
#define PANEL_Y0 4
#define PANEL_Y1 206
#define CX0 58
#define CX1 308
#define HEAD_Y 10
#define SUB_Y (HEAD_Y + 18)
#define BODY_Y 44
#define BTN_Y0 212
#define BTN_Y1 234
static const float s_btn_opt[2] = { 8, 104 };
static const float s_btn_items[2] = { 112, 208 };
/* in the world the middle button is two: Items and Badges (the Badges page) */
static const float s_btn_items_half[2] = { 112, 158 };
static const float s_btn_badges[2] = { 162, 208 };
static const float s_btn_ff[2] = { 216, 312 };

#define COL_CARD      RGBA(250, 236, 204, 255)
#define COL_CARD_RIM  RGBA(200, 160, 110, 255)
#define COL_AREA      RGBA(255, 242, 214, 255)
#define COL_SEL       RGBA(255, 214, 120, 255)
#define COL_OFF       RGBA(200, 185, 165, 255)
#define COL_SUB       RGBA(170, 120, 80, 255)
#define COL_HINT      RGBA(150, 100, 60, 255)
#define COL_GOLD      RGBA(240, 176, 40, 255)
#define COL_DIM       RGBA(110, 80, 50, 70)
#define COL_DONE      RGBA(40, 150, 40, 255)
#define COL_DONE_ROW  RGBA(220, 240, 200, 255)
#define COL_DARK_CHIP RGBA(60, 36, 20, 210)

static void button(const float x[2], const char *label);
static float fit(const char *s, float avail, float scale, float lo);
static bool draw_ui_icon(int id, float x, float y, float size, u32 c);
static bool draw_icon(const void *script, float x, float y, float size, u32 c);
static bool in(int px, int py, float x0, float y0, float x1, float y1);
static void small_text_center(float cx, float y, const char *str, float scale, u32 c);
static void small_text_center_in(float cx, float y, const char *str, float scale, float max_w, u32 c);

/* shortcut for the game (src/world/menus.c): 1 = item menu, 2 = quick save, 3 = pause menu on the
 * Badges page, 0x100 + id = switch partner */
volatile int pm3ds_touch_req;

/* Stats page Save button: a tap arms it, a second one asks the game to save
 * (src/world/menus.c, which answers here: 1 saved, -1 not possible right now) */
volatile int pm3ds_quicksave_result;
enum { QS_IDLE, QS_ARMED, QS_WAIT, QS_DONE };
static int s_qs, s_qs_timer;
#define QS_X0 254
#define QS_X1 308
#define QS_Y0 8
#define QS_Y1 26

static void qs_tick(void) {
    extern volatile int pm3ds_autosave_flash;   /* n64_shim/quicksave.c: an autosave just saved */
    s_qs_timer++;
    if (pm3ds_autosave_flash) {
        pm3ds_autosave_flash = 0;
        pm3ds_quicksave_result = 1;
        s_qs = QS_DONE;
        s_qs_timer = 0;
    }
    if (s_qs == QS_ARMED && s_qs_timer > 150) {
        s_qs = QS_IDLE;
    } else if (s_qs == QS_WAIT) {
        if (pm3ds_quicksave_result == 0 && s_qs_timer > 45) {   /* the world menus didn't run: a cutscene, Peach */
            if (pm3ds_touch_req == 2) pm3ds_touch_req = 0;
            pm3ds_quicksave_result = -1;
        }
        if (pm3ds_quicksave_result != 0) {
            s_qs = QS_DONE;
            s_qs_timer = 0;
        }
    } else if (s_qs == QS_DONE && s_qs_timer > 120) {
        s_qs = QS_IDLE;
    }
}

static void qs_tap(void) {
    if (s_qs == QS_ARMED) {
        pm3ds_quicksave_result = 0;
        pm3ds_touch_req = 2;
        s_qs = QS_WAIT;
    } else if (s_qs != QS_WAIT) {
        s_qs = QS_ARMED;
    }
    s_qs_timer = 0;
}

static void draw_qs_button(void) {
    static const char *const labels[] = { "Save", "Sure?", "...", "" };
    const char *label = labels[s_qs];
    u32 face = s_qs == QS_ARMED ? RGBA(255, 196, 64, 255) : COL_SEL;
    if (s_qs == QS_DONE) {
        label = pm3ds_quicksave_result > 0 ? "Saved!" : "Not now";
        face = pm3ds_quicksave_result > 0 ? RGBA(150, 220, 120, 255) : RGBA(240, 150, 120, 255);
    }
    round_rect(QS_X0, QS_Y0, QS_X1, QS_Y1, 8, COL_PANEL_EDGE);
    round_rect(QS_X0 + 2, QS_Y0 + 2, QS_X1 - 2, QS_Y1 - 2, 6, face);
    text_center((QS_X0 + QS_X1) / 2, QS_Y0 + 2, label, fit(label, QS_X1 - QS_X0 - 8, 0.85f, 0.55f), 0, COL_WHITE);
}

static const char *chapter_label(int ch) {
    static const char *names[] = {
        "Prologue", "Chapter 1", "Chapter 2", "Chapter 3", "Chapter 4",
        "Chapter 5", "Chapter 6", "Chapter 7", "Chapter 8", "Epilogue",
    };
    return ch >= 0 && ch <= 9 ? names[ch] : "";
}

/* ---- building blocks ------------------------------------------------------ */
static void text_right(float x, float y, const char *s, float scale, u32 c) {
    text(x - text_width(s, scale), y, s, scale, 0, c);
}

/* scale that fits s into avail pixels, at most scale, at least lo */
static float fit(const char *s, float avail, float scale, float lo) {
    float w = text_width(s, scale);
    if (w <= avail) return scale;
    float f = scale * avail / w;
    return f < lo ? lo : f;
}

/* text narrowed like fit() and, if even the smallest size is too wide, cut with ".."
 * (a long translation ran into the switch beside it, GitHub #85) */
static float text_in(float x, float y, const char *s, float avail, float scale, float lo, int white, u32 c) {
    static char cut[160];
    s = T(s);
    float sc = fit(s, avail, scale, lo);
    if (text_width_raw(s, sc) <= avail + 0.5f) return text(x, y, s, sc, white, c);
    /* the longest start that fits with ".." after it, measured as text() will draw it */
    int n = 0, best = 0;
    for (int len; s[n]; ) {
        pm3ds_txt_next(s + n, &len);
        if (n + len + 3 > (int)sizeof cut) break;
        n += len;
        memcpy(cut, s, n);
        memcpy(cut + n, "..", 3);
        if (text_width_raw(cut, sc) > avail) break;
        best = n;
    }
    memcpy(cut, s, best);
    memcpy(cut + best, "..", 3);
    return text(x, y, cut, sc, white, c);
}

/* a flat card: thin tan rim, face */
static void card(float x0, float y0, float x1, float y1, u32 face) {
    round_rect(x0, y0, x1, y1, 6, COL_CARD_RIM);
    round_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 5, face);
}

/* a raised tile: shadow, brown rim, face */
static void tile(float x0, float y0, float x1, float y1, float r, u32 face) {
    round_rect(x0 + 2, y0 + 2, x1 + 2, y1 + 2, r, COL_SHADOW);
    round_rect(x0, y0, x1, y1, r, COL_PANEL_EDGE);
    round_rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, r - 2, face);
}

/* a small button with its label centred */
static void chip(float x0, float y0, float x1, float y1, const char *label, u32 face, float scale) {
    round_rect(x0, y0, x1, y1, 7, COL_PANEL_EDGE);
    round_rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 5, face);
    text_center((x0 + x1) / 2, (y0 + y1) / 2 - 8 * scale, label, scale, 0, COL_WHITE);
}

static void page_title(const char *title, const char *sub) {
    text(CX0 + 2, HEAD_Y, title, fit(title, 160, 1.05f, 0.7f), 0, COL_WHITE);
    if (sub) text(CX0 + 2, SUB_Y, sub, 0.8f, 0, COL_SUB);
}

static int pages_of(int n, int per) { return n > 0 ? (n + per - 1) / per : 1; }

/* "<  2/3  >" at the top right of a page; drawn only with more than one page */
static void pager(int page, int pages) {
    if (pages <= 1) return;
    char buf[16];
    chip(226, HEAD_Y, 250, HEAD_Y + 18, "<", COL_SEL, 0.85f);
    snprintf(buf, sizeof buf, "%d/%d", page + 1, pages);
    text_center(268, HEAD_Y + 2, buf, 0.8f, 0, COL_HINT);
    chip(286, HEAD_Y, 310, HEAD_Y + 18, ">", COL_SEL, 0.85f);
}

/* a tap on the pager: the new page, or -1 if the tap missed it */
static int pager_tap(int px, int py, int page, int pages) {
    if (pages <= 1 || py < HEAD_Y - 6 || py >= HEAD_Y + 24) return -1;
    if (px >= 218 && px < 256) return (page + pages - 1) % pages;
    if (px >= 278 && px < 316) return (page + 1) % pages;
    return -1;
}

/* ---- the tab rail ---------------------------------------------------------- */
typedef struct {
    const char *name;
    int img;    /* UI_IMG_* drawn when the icon is not there (yet), -1 none */
    int icon;   /* UI_ICON_*, -1 none */
} rail_tab_t;

static const rail_tab_t s_game_rail[] = {
    { "Stats", UI_IMG_MARIO, -1 },
    { "Map", UI_IMG_MAP_LOCATION, UI_ICON_COMPASS },
    { "Party", UI_IMG_PARTNER_FIRST + 1, -1 },   /* Kooper */
    { "Tattle", UI_IMG_PARTNER_FIRST, -1 },      /* Goombario */
    { "Found", UI_IMG_STAR_PIECE, UI_ICON_STAR_PIECE },
    { "Recipes", -1, UI_ICON_COOK },
};
/* the Options rail; Cheats only for a save that allows them (opt_rail) */
static const rail_tab_t s_opt_rail_all[] = {
    { "Settings", UI_IMG_TIME, UI_ICON_SETTINGS },
    { "Controls", -1, UI_ICON_CONTROLS },
    { "Cheats", -1, UI_ICON_CHEATS },
    { "Debug", -1, UI_ICON_DEBUG },
};
static const int s_opt_rail_tab[] = { TAB_OPTIONS, TAB_CONTROLS, TAB_CHEATS, TAB_DEBUG };

/* after the panel: the active tab runs into the panel's rim */
static void draw_rail(const rail_tab_t *tabs, int n, int active) {
    for (int i = 0; i < n; i++) {
        bool on = i == active;
        float y0 = RAIL_Y0 + i * RAIL_TAB_H, y1 = y0 + RAIL_TAB_H - 1;
        float x0 = on ? 4 : 7, x1 = on ? RAIL_X1 + 4 : RAIL_X1 - 2;
        round_rect(x0 + 2, y0 + 2, x1 + 2, y1 + 2, 7, COL_SHADOW);
        round_rect(x0, y0, x1, y1, 7, COL_PANEL_EDGE);
        round_rect(x0 + 2, y0 + 2, on ? x1 : x1 - 2, y1 - 2, 5, on ? COL_PANEL : COL_TAB);
        float cx = (x0 + RAIL_X1 - 2) / 2 + (on ? 1 : 0);
        if (!(tabs[i].icon >= 0 && draw_ui_icon(tabs[i].icon, cx - 9, y0 + 3, 18, COL_WHITE)) && tabs[i].img >= 0) {
            rect_t r = s_img[tabs[i].img];
            float s = r.w > 16 ? 18.0f / r.w : 1.0f;
            image(tabs[i].img, cx - r.w * s / 2, y0 + 3 + (18 - r.h * s) / 2, s, COL_WHITE);
        }
        text_center(cx, y0 + 20, tabs[i].name, strlen(tabs[i].name) < 7 ? 0.62f : 0.55f, 0, COL_WHITE);
    }
}

/* the rail tab under (px, py), or -1 */
static int rail_hit(int px, int py, int n) {
    if (px >= RAIL_X1 + 2 || py < RAIL_Y0) return -1;
    int i = (py - RAIL_Y0) / RAIL_TAB_H;
    return i < n ? i : -1;
}

static bool cheats_shown(void) { return s_info.in_game && pm3ds_cheats_allowed(); }

/* the Options rail as shown: its tabs and their TAB_* ids; returns how many */
static int opt_rail(rail_tab_t *tabs, int *ids) {
    int n = 0;
    for (int i = 0; i < (int)(sizeof s_opt_rail_tab / sizeof s_opt_rail_tab[0]); i++) {
        if (s_opt_rail_tab[i] == TAB_CHEATS && !cheats_shown()) continue;
        tabs[n] = s_opt_rail_all[i];
        ids[n++] = s_opt_rail_tab[i];
    }
    return n;
}

/* ---- Stats ----------------------------------------------------------------- */
static pm3ds_tracker_t s_found;
static int s_found_page;

/* a ? block, size pixels square; unlit: a faint silhouette */
static void qblock(float x, float y, float size, bool lit) {
    float r = size / 5;
    round_rect(x, y, x + size, y + size, r, lit ? RGBA(140, 80, 20, 255) : COL_DIM);
    if (!lit) return;
    round_rect(x + 1, y + 1, x + size - 1, y + size - 1, r - 1, RGBA(250, 186, 40, 255));
    text_center(x + size / 2 + size / 32, y + size * 0.04f, "?", size / 16, 1, COL_WHITE);
}

/* total icons for one kind of spot, the found ones lit, within width w */
enum { PIP_STAR_PIECE, PIP_BADGE, PIP_HIDDEN };
static void pips(float x, float y, int kind, int found, int total, float w) {
    if (total <= 0) return;
    float step = w / total > 12 ? 12 : w / total, size = step - 2 > 10 ? 10 : step - 2;
    for (int i = 0; i < total; i++) {
        u32 c = i < found ? COL_WHITE : COL_DIM;
        float px = x + i * step;
        if (kind == PIP_STAR_PIECE) {
            if (!draw_ui_icon(UI_ICON_STAR_PIECE, px, y, size, c)) image(UI_IMG_STAR_PIECE, px, y, size / 16, c);
        } else if (kind == PIP_BADGE) {
            image(UI_IMG_BP, px, y, size / 16, c);
        } else {
            qblock(px, y, size, i < found);
        }
    }
}

/* "3/5" centred on cx, or "-" when there are none of these; green when all found */
static void count_cell(float cx, float y, int found, int total, float scale) {
    char buf[12];
    if (total == 0) snprintf(buf, sizeof buf, "-");
    else snprintf(buf, sizeof buf, "%d/%d", found, total);
    text_center(cx, y, buf, scale, 0, total > 0 && found == total ? COL_DONE : COL_WHITE);
}

/* the item spots of Mario's area and room (n64_shim/journal.c) */
static void draw_area_card(float x0, float y0, float x1, float y1) {
    const pm3ds_tracker_t *f = &s_found;
    char buf[48];
    card(x0, y0, x1, y1, COL_AREA);
    if (f->here < 0) {
        text_center((x0 + x1) / 2, (y0 + y1) / 2 - 8, "Nothing to find here", 0.85f, 0, COL_SUB);
        return;
    }
    const pm3ds_tracker_area_t *a = &f->area[f->here];
    text(x0 + 7, y0 + 4, "Found in this area", 0.85f, 0, COL_WHITE);
    snprintf(buf, sizeof buf, "%d/%d", a->found, a->total);
    text_right(x1 - 7, y0 + 3, buf, 1.0f, a->found == a->total ? COL_DONE : COL_WHITE);
    bar(x0 + 8, y0 + 20, x1 - x0 - 16, 6, a->found, a->total, COL_GOLD);
    if (f->rando) {
        extern unsigned int pm3ds_rando_seed(void);
        snprintf(buf, sizeof buf, T("Randomizer seed %08X"), pm3ds_rando_seed());
        text(x0 + 8, y0 + 33, buf, 0.8f, 0, COL_WHITE);
        text(x0 + 8, y0 + 51, "The spots hold shuffled items:", 0.72f, 0, COL_SUB);
        text(x0 + 8, y0 + 64, "only the totals count.", 0.72f, 0, COL_SUB);
    } else {
        static const char *const labels[3] = { "Star Pieces", "Badges", "Hidden" };
        const unsigned char *counts[3][2] = { { &a->sp_found, &a->sp_total }, { &a->badge_found, &a->badge_total },
                                              { &a->hidden_found, &a->hidden_total } };
        for (int k = 0; k < 3; k++) {
            float y = y0 + 30 + k * 16;
            int got = *counts[k][0], total = *counts[k][1];
            if (k == PIP_STAR_PIECE) image(UI_IMG_STAR_PIECE, x0 + 8, y, 1.0f, COL_WHITE);
            else if (k == PIP_BADGE) image(UI_IMG_BP, x0 + 8, y, 1.0f, COL_WHITE);
            else qblock(x0 + 9, y + 1, 14, true);
            text(x0 + 28, y + 1, labels[k], 0.8f, 0, COL_WHITE);
            pips(x0 + 112, y + 3, k, got, total, x1 - x0 - 160);
            count_cell(x1 - 24, y + 1, got, total, 0.8f);
        }
    }
    if (f->room_total == 0) snprintf(buf, sizeof buf, "Nothing to find in this room");
    else if (f->room_found == f->room_total) snprintf(buf, sizeof buf, "This room: all found");
    else snprintf(buf, sizeof buf, T("This room: %d of %d still here"), f->room_total - f->room_found, f->room_total);
    text_center((x0 + x1) / 2, y1 - 16, buf, 0.75f, 0, COL_SUB);
}

static void draw_stats(void) {
    const pm3ds_hud_info_t *h = &s_info;
    char buf[48];
    text(CX0 + 2, HEAD_Y, h->location, fit(h->location, 188, 1.05f, 0.7f), 0, COL_WHITE);
    float x = text(CX0 + 2, SUB_Y, chapter_label(h->chapter), 0.8f, 0, COL_SUB) + 8;
    int full = h->star_power / 256;
    for (int i = 0; i < h->max_star_power && i < 7; i++)
        image(i < full ? UI_IMG_POW_STAR : UI_IMG_POW_EMPTY, x + i * 9, SUB_Y + 4, 1.0f, COL_WHITE);
    unsigned sec = h->play_seconds;
    snprintf(buf, sizeof buf, "%u:%02u:%02u", sec / 3600, (sec / 60) % 60, sec % 60);
    float tw = text_width(buf, 0.8f);
    image(UI_IMG_TIME, CX1 - tw - 18, SUB_Y - 1, 1.0f, COL_WHITE);
    text(CX1 - tw, SUB_Y, buf, 0.8f, 0, COL_WHITE);
    {   /* the save's way to play: GitHub #69, #83 */
        float tx = CX1 - tw - 22;
        if (pm3ds_hard_mode()) {
            text_right(tx, SUB_Y + 2, "Hard", 0.7f, COL_HP);
            tx -= text_width(T("Hard"), 0.7f) + 5;
        }
        if (pm3ds_level99()) text_right(tx, SUB_Y + 2, "Lv 99", 0.7f, COL_GOLD);
    }
    draw_qs_button();

    /* HP and FP */
    for (int k = 0; k < 2; k++) {
        float x0 = CX0 + k * 126;
        int v = k ? h->fp : h->hp, m = k ? h->max_fp : h->max_hp;
        card(x0, 46, x0 + 120, 76, COL_CARD);
        image(k ? UI_IMG_FLOWER : UI_IMG_HEART, x0 + 5, 49, 1.0f, COL_WHITE);
        text(x0 + 23, 49, k ? "FP" : "HP", 0.9f, 0, COL_WHITE);
        snprintf(buf, sizeof buf, "%d/%d", v, m);
        text_right(x0 + 114, 49, buf, 0.9f, COL_WHITE);
        bar(x0 + 6, 66, 108, 5, v, m, k ? COL_FP : COL_HP);
    }
    /* level, BP, star points, coins, star pieces */
    const int icons[5] = { UI_IMG_MARIO, UI_IMG_BP, UI_IMG_STAR_POINT, UI_IMG_COIN, UI_IMG_STAR_PIECE };
    const int vals[5] = { h->level, h->bp, h->star_points, h->coins, h->star_pieces };
    for (int k = 0; k < 5; k++) {
        float x0 = CX0 + k * 50.4f;
        card(x0, 81, x0 + 46, 101, COL_CARD);
        image(icons[k], x0 + 3, 83, 1.0f, COL_WHITE);
        snprintf(buf, sizeof buf, "%d", vals[k]);
        text_right(x0 + 42, 84, buf, fit(buf, 22, 0.8f, 0.6f), COL_WHITE);
    }
    draw_area_card(CX0, 106, CX1, 202);
}

/* ---- Map ------------------------------------------------------------------- */
#define MAP_X0 57
#define MAP_Y0 11
#define MAP_X1 309
#define MAP_Y1 199
#define MAP_CENTRE_X0 (MAP_X1 - 62)
#define MAP_CENTRE_Y0 (MAP_Y1 - 24)

static void draw_map(void) {
    const pm3ds_hud_info_t *h = &s_info;
    const pm3ds_tracker_t *f = &s_found;
    char buf[16];
    float vx0 = MAP_X0, vy0 = MAP_Y0, vx1 = MAP_X1, vy1 = MAP_Y1;
    float vw = vx1 - vx0, vh = vy1 - vy0;
    if (s_map_follow && h->map_current >= 0) {
        s_map_cx = h->map_x[h->map_current];
        s_map_cy = h->map_y[h->map_current];
    }
    if (s_map_cx < 0) { s_map_cx = 160; s_map_cy = 160; }
    /* keep the view inside the 320x320 map */
    if (s_map_cx < vw / 2) s_map_cx = vw / 2;
    if (s_map_cx > 320 - vw / 2) s_map_cx = 320 - vw / 2;
    if (s_map_cy < vh / 2) s_map_cy = vh / 2;
    if (s_map_cy > 320 - vh / 2) s_map_cy = 320 - vh / 2;
    float mx0 = s_map_cx - vw / 2, my0 = s_map_cy - vh / 2;
    round_rect(vx0 - 2, vy0 - 2, vx1 + 2, vy1 + 2, 6, COL_PANEL_EDGE);
    if (s_map_ok)
        quad2(&s_map, vx0, vy0, vx1, vy1, mx0 / 512.0f, my0 / 512.0f,
              (mx0 + vw) / 512.0f, (my0 + vh) / 512.0f, COL_WHITE, COL_WHITE);
    for (int i = 0; i < h->map_count; i++) {
        if (!h->map_visited[i]) continue;
        float x = vx0 + h->map_x[i] - mx0, y = vy0 + h->map_y[i] - my0;
        if (x < vx0 || x > vx1 || y < vy0 || y > vy1) continue;
        image(UI_IMG_MAP_LOCATION, x - 8, y - 8, 1.0f, RGBA(220, 80, 30, 255));
    }
    if (h->map_current >= 0) {
        float x = vx0 + h->map_x[h->map_current] - mx0, y = vy0 + h->map_y[h->map_current] - my0;
        int walk = (s_frame / 8) & 1 ? UI_IMG_MAP_WALK1 : UI_IMG_MAP_WALK0;
        if (x >= vx0 && x <= vx1 && y >= vy0 && y <= vy1) image(walk, x - 12, y - 20, 1.0f, COL_WHITE);
    }
    /* where Mario is, and what the area still hides */
    bool counts = f->here >= 0;
    float cw = text_width(h->location, 0.85f) + 14;
    if (counts && cw < 124) cw = 124;
    round_rect(vx0 + 4, vy0 + 4, vx0 + 4 + cw, vy0 + (counts ? 38 : 22), 7, COL_DARK_CHIP);
    text(vx0 + 10, vy0 + 6, h->location, 0.85f, 1, COL_WHITE);
    if (counts) {
        const pm3ds_tracker_area_t *a = &f->area[f->here];
        float y = vy0 + 21;
        if (f->rando) {
            snprintf(buf, sizeof buf, T("Checked %d/%d"), a->found, a->total);
            text(vx0 + 10, y, buf, 0.7f, 1, COL_WHITE);
        } else {
            image(UI_IMG_STAR_PIECE, vx0 + 9, y, 0.75f, COL_WHITE);
            snprintf(buf, sizeof buf, "%d/%d", a->sp_found, a->sp_total);
            text(vx0 + 22, y, buf, 0.7f, 1, COL_WHITE);
            image(UI_IMG_BP, vx0 + 48, y, 0.75f, COL_WHITE);
            snprintf(buf, sizeof buf, "%d/%d", a->badge_found, a->badge_total);
            text(vx0 + 61, y, buf, 0.7f, 1, COL_WHITE);
            qblock(vx0 + 87, y, 11, true);
            snprintf(buf, sizeof buf, "%d/%d", a->hidden_found, a->hidden_total);
            text(vx0 + 100, y, buf, 0.7f, 1, COL_WHITE);
        }
    }
    if (!s_map_follow) {
        chip(MAP_CENTRE_X0, MAP_CENTRE_Y0, vx1 - 4, vy1 - 4, "Centre", COL_SEL, 0.75f);
    } else if (!pm3ds_cfg.map_dragged) {   /* until the first drag */
        round_rect(vx0 + 4, vy1 - 24, vx0 + 112, vy1 - 4, 7, COL_DARK_CHIP);
        text(vx0 + 10, vy1 - 22, "Drag to look", 0.7f, 1, COL_WHITE);
    }
}

/* ---- Party ----------------------------------------------------------------- */
/* portraits in pause-menu order; partner ids */
static const int s_party_ids[PM3DS_HUD_PARTNERS] = { 1, 2, 3, 4, 9, 6, 7, 8 };

static void party_rect(int i, float *x0, float *y0, float *x1, float *y1) {
    *x0 = CX0 + (i % 4) * 63.5f;
    *y0 = 46 + (i / 4) * 74;
    *x1 = *x0 + 59;
    *y1 = *y0 + 68;
}

static void draw_party(void) {
    const pm3ds_hud_info_t *h = &s_info;
    static const char *names[PM3DS_HUD_PARTNERS] = {
        "Goombario", "Kooper", "Bombette", "Parakarry", "Bow", "Watt", "Sushie", "Lakilester",
    };
    char sub[80];
    if (h->partner) snprintf(sub, sizeof sub, T("%s is with you"), T(h->partner_name));   /* the name too (GitHub #85) */
    else snprintf(sub, sizeof sub, "No partner yet");
    page_title("Party", sub);
    for (int i = 0; i < PM3DS_HUD_PARTNERS; i++) {
        float x0, y0, x1, y1;
        party_rect(i, &x0, &y0, &x1, &y1);
        bool owned = (h->partners_owned >> s_party_ids[i]) & 1;
        tile(x0, y0, x1, y1, 8, h->partner == s_party_ids[i] ? COL_SEL : owned ? COL_TAB_ON : COL_OFF);
        image(owned ? UI_IMG_PARTNER_FIRST + i : UI_IMG_PARTNER_DISABLED_FIRST + i, (x0 + x1) / 2 - 18, y0 + 6, 1.125f,
              COL_WHITE);
        const char *nm = owned ? names[i] : "???";
        text_center((x0 + x1) / 2, y0 + 47, nm, fit(nm, 52, 0.8f, 0.55f), 0,
                    owned ? COL_WHITE : RGBA(255, 255, 255, 140));
    }
    if (h->partners_owned) text_center((CX0 + CX1) / 2, 191, "Tap a partner to switch", 0.7f, 0, COL_HINT);
}

/* Party tab: the partner whose portrait is at (px, py), or 0 */
static int partner_at(int px, int py) {
    for (int i = 0; i < PM3DS_HUD_PARTNERS; i++) {
        float x0, y0, x1, y1;
        party_rect(i, &x0, &y0, &x1, &y1);
        if (in(px, py, x0, y0, x1, y1)) return s_party_ids[i];
    }
    return 0;
}

static void draw_debug(void) {
    fill(0, 0, 320, 240, RGBA(16, 16, 24, 255));
    float y = 3;
    for (int i = 0; i < 3; i++) y = small_text(4, y, pm3ds_perf_text[i], 39, RGBA(255, 230, 120, 255));
    fill(4, y + 1, 316, y + 2, RGBA(90, 90, 110, 255));
    y += 5;
    /* newest lines last; show as many as fit above the Close button */
    int n = 0;
    while (n < 16 && pm3ds_log_recent(n)) n++;
    for (int i = n - 1; i >= 0 && y < 200; i--) {
        const char *l = pm3ds_log_recent(i);
        if (!strncmp(l, "[INFO ] ", 8)) l += 8;   /* drop the level tag for INFO */
        y = small_text(4, y, l, 39, RGBA(220, 220, 220, 255));
    }
    round_rect(250, 214, 316, 236, 8, COL_BTN);
    text_center(283, 217, "Back", 1.0f, 1, COL_WHITE);
}

/* ---- Tattle log ------------------------------------------------------------ */
static pm3ds_tattle_list_t s_log;
static int s_log_page, s_log_open = -1, s_log_text_page;   /* s_log_open: s_log index shown in full */
static char s_log_text[2048];
static int s_log_hp, s_log_atk, s_log_def;                 /* from the open tattle, -1 if it has none */
#define LOG_ROWS 7
#define LOG_PER_PAGE (LOG_ROWS * 2)
#define LOG_TEXT_SCALE 0.8f
#define LOG_LINE_H 15
#define LOG_TEXT_Y1 200

/* word wrap: start/length of each line of str at scale within width */
static int wrap_text(const char *str, float scale, float width, short *start, short *len, int max) {
    int n = 0, i = 0, end_all = (int)strlen(str);
    while (i < end_all && n < max) {
        while (str[i] == ' ') i++;
        if (!str[i]) break;
        int j = i, last_space = -1, cl;
        float w = 0;
        for (; j < end_all; j += cl) {
            uint32_t cp = pm3ds_txt_next(str + j, &cl);
            float cw;
            if (cp & 0x80000000u) {
                /* Chinese and Japanese: a line may break before any character
                 * but closing punctuation */
                cp &= 0x1FFFFF;
                cw = sys_glyph(0, 0, cp, scale, 0, false, false);
                if (!(cp == 0x3001 || cp == 0x3002 || (cp >= 0xFF01 && cp <= 0xFF1F && cp != 0xFF08) ||
                      cp == 0x2026 || cp == 0x300D || cp == 0x300F)) last_space = j;
            } else {
                int g = glyph_of((char)cp);
                cw = (g >= 0 && g < s_glyphs ? s_glyph_w[g] : 5) * scale;
                if (cp == ' ') last_space = j;
            }
            if (w + cw > width && j > i) break;
            w += cw;
        }
        int end = j < end_all && last_space > i ? last_space : j;
        start[n] = (short)i;
        len[n] = (short)(end - i);
        n++;
        i = end;
    }
    return n;
}

/* the number after key in a tattle ("Max HP: 2"), -1 if it isn't there. The
 * tattle is in the game's language, which needn't be the bottom screen's (the
 * iQue ROM is Chinese, the EU ROM any of five): every language's key is tried. */
static int tattle_stat(const char *t, const char *key) {
    for (int lang = 0; lang < PM3DS_LANG_COUNT; lang++) {
        char k[48];
        pm3ds_lang_encode(pm3ds_lang_utf8(lang, key), k, sizeof k);
        const char *p = strstr(t, k);
        if (!p) continue;
        p += strlen(k);
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') return atoi(p);
    }
    return -1;
}

static void log_open(int i) {
    s_log_open = i;
    s_log_text_page = 0;
    pm3ds_tattle_text(s_log.entry[i], s_log_text, sizeof s_log_text);
    s_log_hp = tattle_stat(s_log_text, "Max HP:");
    s_log_atk = tattle_stat(s_log_text, "Attack Power:");
    s_log_def = tattle_stat(s_log_text, "Defense Power:");
}

static bool log_has_stats(void) { return s_log_hp >= 0 && s_log_atk >= 0 && s_log_def >= 0; }

static void log_entry_rect(int slot, float *x0, float *y0, float *x1, float *y1) {
    *x0 = CX0 + (slot / LOG_ROWS) * 126;
    *y0 = 56 + (slot % LOG_ROWS) * 21;
    *x1 = *x0 + 122;
    *y1 = *y0 + 18;
}

static void draw_log(void) {
    char buf[40];
    if (s_log_open >= 0 && s_log_open < s_log.known) {   /* one tattle in full */
        static short starts[48], lens[48];
        float ty0 = log_has_stats() ? 66 : 38;
        int per = (int)((LOG_TEXT_Y1 - ty0 - 8) / LOG_LINE_H);
        int lines = wrap_text(s_log_text, LOG_TEXT_SCALE, CX1 - CX0 - 16, starts, lens, 48);
        int pages = pages_of(lines, per);
        if (s_log_text_page >= pages) s_log_text_page = 0;
        chip(CX0, HEAD_Y - 1, CX0 + 50, HEAD_Y + 19, "Back", COL_SEL, 0.85f);
        const char *name = s_log.name[s_log_open];
        text(CX0 + 58, HEAD_Y, name, fit(name, 190 - (pages > 1 ? 30 : 0), 1.05f, 0.6f), 0, COL_WHITE);
        if (pages > 1) {
            snprintf(buf, sizeof buf, "%d/%d", s_log_text_page + 1, pages);
            text_right(CX1, HEAD_Y + 3, buf, 0.75f, COL_HINT);
        }
        if (log_has_stats()) {
            static const char *const labels[3] = { "Max HP", "Attack", "Defense" };
            const int vals[3] = { s_log_hp, s_log_atk, s_log_def };
            for (int k = 0; k < 3; k++) {
                float x0 = CX0 + k * 84;
                card(x0, 38, x0 + 80, 60, COL_CARD);
                if (k == 0) image(UI_IMG_HEART, x0 + 4, 41, 1.0f, COL_WHITE);
                else draw_ui_icon(k == 1 ? UI_ICON_BOOTS : UI_ICON_HAMMER, x0 + 4, 41, 16, COL_WHITE);
                text(x0 + 23, 42, labels[k], 0.7f, 0, COL_WHITE);
                snprintf(buf, sizeof buf, "%d", vals[k]);
                text_right(x0 + 75, 41, buf, 0.9f, COL_WHITE);
            }
        }
        card(CX0, ty0, CX1, LOG_TEXT_Y1, COL_AREA);
        for (int l = 0; l < per; l++) {
            int k = s_log_text_page * per + l;
            if (k >= lines) break;
            char line[96];
            int n = lens[k] < (int)sizeof line - 1 ? lens[k] : (int)sizeof line - 1;
            memcpy(line, s_log_text + starts[k], n);
            line[n] = '\0';
            text(CX0 + 8, ty0 + 6 + l * LOG_LINE_H, line, LOG_TEXT_SCALE, 0, COL_WHITE);
        }
        return;
    }
    s_log_open = -1;
    int pages = pages_of(s_log.known, LOG_PER_PAGE);
    if (s_log_page >= pages) s_log_page = 0;
    snprintf(buf, sizeof buf, T("%d of %d enemies"), s_log.known, s_log.total);
    page_title("Tattle Log", buf);
    pager(s_log_page, pages);
    bar(CX0 + 2, BODY_Y, CX1 - CX0 - 2, 5, s_log.known, s_log.total, COL_GOLD);
    if (s_log.known == 0) {
        text_center((CX0 + CX1) / 2, 100, "No tattles yet", 1.0f, 0, COL_WHITE);
        text_center((CX0 + CX1) / 2, 124, "Tattle enemies in battle with Goombario", 0.7f, 0, COL_HINT);
        return;
    }
    for (int slot = 0; slot < LOG_PER_PAGE; slot++) {
        int i = s_log_page * LOG_PER_PAGE + slot;
        if (i >= s_log.known) break;
        float x0, y0, x1, y1;
        log_entry_rect(slot, &x0, &y0, &x1, &y1);
        tile(x0, y0, x1, y1, 6, COL_TAB_ON);
        text(x0 + 7, y0 + 1, s_log.name[i], fit(s_log.name[i], x1 - x0 - 14, 0.8f, 0.55f), 0, COL_WHITE);
    }
}

static void log_tap(int px, int py) {
    if (s_log_open >= 0) {
        if (in(px, py, CX0 - 4, HEAD_Y - 5, CX0 + 54, HEAD_Y + 23)) s_log_open = -1;
        else if (py >= 38) s_log_text_page++;   /* wraps in draw_log */
        return;
    }
    int np = pager_tap(px, py, s_log_page, pages_of(s_log.known, LOG_PER_PAGE));
    if (np >= 0) { s_log_page = np; return; }
    for (int slot = 0; slot < LOG_PER_PAGE; slot++) {
        int i = s_log_page * LOG_PER_PAGE + slot;
        if (i >= s_log.known) break;
        float x0, y0, x1, y1;
        log_entry_rect(slot, &x0, &y0, &x1, &y1);
        if (in(px, py, x0, y0 - 1, x1, y1 + 2)) {
            log_open(i);
            return;
        }
    }
}

/* ---- Found ----------------------------------------------------------------- */
#define FOUND_ROWS 7
#define FOUND_ROW_H 19
#define FOUND_Y0 70

/* an area's spots, tapped open from its row: what lies where (a spoiler, asked for) */
static int s_found_open = -1, s_spots_page;
static pm3ds_tracker_spots_t s_spots;

static const char *spot_src(int src) {
    switch (src) {
        case PM3DS_SPOT_BLOCK:        return T("Block");
        case PM3DS_SPOT_TREE:         return T("Tree");
        case PM3DS_SPOT_BUSH:         return T("Bush");
        case PM3DS_SPOT_PANEL:        return T("Hidden panel");
        case PM3DS_SPOT_CHEST:        return T("Chest");
        case PM3DS_SPOT_HIDDEN_BLOCK: return T("Hidden block");
        case PM3DS_SPOT_CRATE:        return T("Crate");
        case PM3DS_SPOT_SUPER_BLOCK:  return T("Super Block");
        default:                      return T("On the ground");
    }
}

static void found_open(int i) {
    s_found_open = i;
    s_spots_page = 0;
    pm3ds_tracker_spots(i, &s_spots);
}

static void draw_found_area(void) {
    const pm3ds_tracker_area_t *a = &s_found.area[s_found_open];
    char buf[48];
    int pages = pages_of(s_spots.count, FOUND_ROWS);
    if (s_spots_page >= pages) s_spots_page = 0;
    chip(CX0, HEAD_Y - 1, CX0 + 50, HEAD_Y + 19, "Back", COL_SEL, 0.85f);
    float tx = CX0 + 56, tw = (pages > 1 ? 218 : CX1) - tx - 4;
    text(tx, HEAD_Y, a->name, fit(a->name, tw, 1.0f, 0.55f), 0, COL_WHITE);
    pager(s_spots_page, pages);
    bar(CX0 + 2, BODY_Y, CX1 - CX0 - 2, 5, a->found, a->total, COL_GOLD);
    snprintf(buf, sizeof buf, T("%d of %d found"), a->found, a->total);
    text(CX0 + 2, 54, buf, 0.7f, 0, COL_WHITE);
    text_right(CX1 - 2, 55, T("This room first"), 0.62f, COL_HINT);
    for (int r = 0; r < FOUND_ROWS; r++) {
        int i = s_spots_page * FOUND_ROWS + r;
        if (i >= s_spots.count) break;
        const typeof(s_spots.spot[0]) *s = &s_spots.spot[i];
        float y0 = FOUND_Y0 + r * FOUND_ROW_H;
        tile(CX0, y0, CX1, y0 + 17, 6, s->found ? COL_DONE_ROW : s->here ? COL_SEL : COL_TAB_ON);
        float x = CX0 + 4;
        if (s->icon && draw_icon(s->icon, x, y0 + 1, 15, s->found ? RGBA(255, 255, 255, 150) : COL_WHITE)) x += 18;
        else x += 2;
        const char *what = s->item[0] ? s->item : spot_src(s->src);
        text(x, y0 + 1, what, fit(what, 150 - x, 0.72f, 0.5f), 0, s->found ? COL_DONE : COL_WHITE);
        /* where: what holds it, and the room */
        if (s->here) snprintf(buf, sizeof buf, "%s, %s", spot_src(s->src), T("this room"));
        else snprintf(buf, sizeof buf, T("%s, room %d"), spot_src(s->src), s->room);
        text_right(CX1 - 4, y0 + 2, buf, fit(buf, CX1 - 158, 0.62f, 0.45f), s->found ? COL_DONE : COL_HINT);
    }
}

static void draw_found(void) {
    const pm3ds_tracker_t *f = &s_found;
    char buf[48];
    if (s_found_open >= 0 && s_found_open < f->count) {
        draw_found_area();
        return;
    }
    s_found_open = -1;
    int pages = pages_of(f->count, FOUND_ROWS);
    if (s_found_page >= pages) s_found_page = 0;
    snprintf(buf, sizeof buf, T("%d of %d item spots"), f->found, f->total);
    page_title("Found", buf);
    pager(s_found_page, pages);
    bar(CX0 + 2, BODY_Y, CX1 - CX0 - 2, 5, f->found, f->total, COL_GOLD);
    /* column heads: star pieces, badges, hidden blocks and panels, every spot.
     * A randomizer seed puts other items in the spots: only the totals hold. */
    float hy = 54;
    if (!f->rando) {
        image(UI_IMG_STAR_PIECE, 171, hy, 0.85f, COL_WHITE);
        image(UI_IMG_BP, 205, hy, 0.85f, COL_WHITE);
        qblock(240, hy, 13, true);
    }
    text_center(289, hy + 1, f->rando ? "Checked" : "All", 0.75f, 0, COL_WHITE);
    text(CX0 + 2, hy + 2, T("Tap an area: where things are"), fit(T("Tap an area: where things are"), 104, 0.6f, 0.45f), 0, COL_HINT);
    for (int r = 0; r < FOUND_ROWS; r++) {
        int i = s_found_page * FOUND_ROWS + r;
        if (i >= f->count) break;
        const pm3ds_tracker_area_t *a = &f->area[i];
        float y0 = FOUND_Y0 + r * FOUND_ROW_H;
        bool here = i == f->here, done = a->found == a->total;
        tile(CX0, y0, CX1, y0 + 17, 6, here ? COL_SEL : done ? COL_DONE_ROW : COL_TAB_ON);
        float x = CX0 + 6;
        if (here) {
            image(UI_IMG_MAP_WALK0, CX0 - 1, y0 - 6, 0.75f, COL_WHITE);
            x = CX0 + 18;
        }
        float avail = (f->rando ? 262 : 158) - x;
        text(x, y0 + 1, a->name, fit(a->name, avail, 0.75f, 0.55f), 0, done ? COL_DONE : COL_WHITE);
        if (!f->rando) {
            count_cell(178, y0 + 1, a->sp_found, a->sp_total, 0.72f);
            count_cell(212, y0 + 1, a->badge_found, a->badge_total, 0.72f);
            count_cell(246, y0 + 1, a->hidden_found, a->hidden_total, 0.72f);
        }
        snprintf(buf, sizeof buf, "%d/%d", a->found, a->total);
        text_right(CX1 - 4, y0 + 1, buf, 0.72f, done ? COL_DONE : COL_WHITE);
    }
}

static void found_tap(int px, int py) {
    if (s_found_open >= 0) {
        if (in(px, py, CX0 - 4, HEAD_Y - 5, CX0 + 54, HEAD_Y + 23)) {
            s_found_open = -1;
            return;
        }
        int np = pager_tap(px, py, s_spots_page, pages_of(s_spots.count, FOUND_ROWS));
        if (np >= 0) s_spots_page = np;
        return;
    }
    int np = pager_tap(px, py, s_found_page, pages_of(s_found.count, FOUND_ROWS));
    if (np >= 0) {
        s_found_page = np;
        return;
    }
    for (int r = 0; r < FOUND_ROWS; r++) {     /* a row: that area's spots */
        int i = s_found_page * FOUND_ROWS + r;
        float y0 = FOUND_Y0 + r * FOUND_ROW_H;
        if (i < s_found.count && in(px, py, CX0, y0 - 1, CX1, y0 + 18)) {
            found_open(i);
            return;
        }
    }
}

/* ---- Recipes --------------------------------------------------------------- */
/* Every dish Tayce T. cooks, by name, the cooked ones lit. Dishes she hasn't
 * cooked yet stay ??? unless "Show all" is on. A dish opens on what it does
 * and every way to cook it (n64_shim/journal.c). */
static pm3ds_recipe_list_t s_cook;
static pm3ds_recipe_detail_t s_dish;
static int s_cook_page, s_cook_open = -1, s_dish_page;   /* s_cook_open: s_cook index shown */
static bool s_cook_all;
static char s_dish_name[PM3DS_RECIPE_NAME];
static short s_dish_starts[8], s_dish_lens[8];
static int s_dish_lines;
#define COOK_ROWS 7
#define COOK_PER_PAGE (COOK_ROWS * 2)
#define COOK_ALL_X0 150
#define COOK_ALL_X1 214
#define DISH_DESC_Y 58
#define DISH_DESC_SCALE 0.7f
#define DISH_DESC_LINE_H 12
#define DISH_DESC_LINES 3
#define DISH_WAY_H 19

static bool cook_known(int i) { return s_cook.done[i] || s_cook_all; }

static void cook_open(int i) {
    s_cook_open = i;
    s_dish_page = 0;
    memcpy(s_dish_name, s_cook.name[i], sizeof s_dish_name);
    pm3ds_recipe_detail(s_cook.item[i], &s_dish);
    s_dish_lines = wrap_text(s_dish.desc, DISH_DESC_SCALE, CX1 - CX0 - 12, s_dish_starts, s_dish_lens, DISH_DESC_LINES);
}

/* the ways list starts under the description */
static float dish_ways_y0(void) { return DISH_DESC_Y + s_dish_lines * DISH_DESC_LINE_H + 8 + 16; }
static int dish_ways_per_page(void) { return (int)((PANEL_Y1 - 4 - dish_ways_y0()) / DISH_WAY_H); }

/* ingredient k of a way: icon and name from x, within w; the x after it */
static float dish_ingr(int k, float x, float y, float w) {
    if (draw_icon(s_dish.ingr_icon[k], x, y + 1, 14, COL_WHITE)) x += 16;
    const char *name = s_dish.ingr_name[k];
    float sc = fit(name, w - 16, 0.68f, 0.45f);
    text(x, y + 1, name, sc, 0, COL_WHITE);
    return x + text_width(name, sc);
}

static void draw_dish(void) {
    char buf[24];
    int per = dish_ways_per_page(), pages = pages_of(s_dish.ways, per);
    if (s_dish_page >= pages) s_dish_page = 0;
    chip(CX0, HEAD_Y - 1, CX0 + 50, HEAD_Y + 19, "Back", COL_SEL, 0.85f);
    float nx = CX0 + 56;
    if (draw_icon(s_cook.icon[s_cook_open], nx, HEAD_Y - 1, 20, COL_WHITE)) nx += 23;
    text(nx, HEAD_Y, s_dish_name, fit(s_dish_name, (pages > 1 ? 218 : CX1) - nx - 4, 1.0f, 0.55f), 0, COL_WHITE);
    pager(s_dish_page, pages);
    /* what eating it does */
    float x = CX0;
    if (s_dish.hp > 0) {
        card(x, 34, x + 56, 54, COL_CARD);
        image(UI_IMG_HEART, x + 4, 36, 1.0f, COL_WHITE);
        snprintf(buf, sizeof buf, "+%d", s_dish.hp);
        text_right(x + 52, 36, buf, 0.85f, COL_WHITE);
        x += 60;
    }
    if (s_dish.fp > 0) {
        card(x, 34, x + 56, 54, COL_CARD);
        image(UI_IMG_FLOWER, x + 4, 36, 1.0f, COL_WHITE);
        snprintf(buf, sizeof buf, "+%d", s_dish.fp);
        text_right(x + 52, 36, buf, 0.85f, COL_WHITE);
    }
    bool done = s_cook.done[s_cook_open];
    text_right(CX1 - 2, 38, done ? "Cooked" : "Not cooked yet", 0.72f, done ? COL_DONE : COL_HINT);
    if (s_dish_lines > 0) {
        card(CX0, DISH_DESC_Y - 2, CX1, DISH_DESC_Y + s_dish_lines * DISH_DESC_LINE_H + 4, COL_AREA);
        for (int l = 0; l < s_dish_lines; l++) {
            char line[96];
            int n = s_dish_lens[l] < (int)sizeof line - 1 ? s_dish_lens[l] : (int)sizeof line - 1;
            memcpy(line, s_dish.desc + s_dish_starts[l], n);
            line[n] = '\0';
            text(CX0 + 6, DISH_DESC_Y + l * DISH_DESC_LINE_H, line, DISH_DESC_SCALE, 0, COL_WHITE);
        }
    }
    float y = dish_ways_y0();
    text(CX0 + 2, y - 16, "How to cook it", 0.72f, 0, COL_SUB);
    if (s_dish.ways == 0) {
        text(CX0 + 6, y + 2, "Anything that doesn't go together", 0.72f, 0, COL_WHITE);
        return;
    }
    text_right(CX1 - 2, y - 16, "Mixing two needs the Cookbook", 0.62f, COL_HINT);
    for (int r = 0; r < per; r++) {
        int i = s_dish_page * per + r;
        if (i >= s_dish.ways) break;
        float y0 = y + r * DISH_WAY_H;
        tile(CX0, y0, CX1, y0 + DISH_WAY_H - 2, 6, COL_TAB_ON);
        if (s_dish.way[i].b < 0) {
            float xe = dish_ingr(s_dish.way[i].a, CX0 + 5, y0, 150);
            const char *note = s_dish.way[i].luck ? "by luck" : s_dish.way[i].cookbook ? "with the Cookbook" : "on its own";
            text(xe + 8, y0 + 2, note, 0.62f, 0, COL_HINT);
        } else {
            float xe = dish_ingr(s_dish.way[i].a, CX0 + 5, y0, 112);
            text(xe + 4, y0 + 1, "+", 0.75f, 0, COL_HINT);
            dish_ingr(s_dish.way[i].b, xe + 14, y0, CX1 - xe - 18);
        }
    }
}

static void cook_entry_rect(int slot, float *x0, float *y0, float *x1, float *y1) {
    *x0 = CX0 + (slot / COOK_ROWS) * 126;
    *y0 = 56 + (slot % COOK_ROWS) * 21;
    *x1 = *x0 + 122;
    *y1 = *y0 + 18;
}

/* ---- Chuck Quizmo's questions (GitHub #80), a page of the Recipes tab ---------- */
#define QUIZ_X0 (CX1 - 64)                /* the Recipes <-> Quizzes chip, on the subtitle line */
static bool s_quiz;                       /* the Recipes tab shows the quizzes */
static bool s_quiz_spoil;                 /* show the answers of questions not asked yet */
static int s_quiz_page, s_quiz_open = -1;
static int s_quiz_done, s_quiz_tries, s_quiz_right;
static char s_quiz_ans[PM3DS_QUIZ_COUNT][PM3DS_QUIZ_OPT];
static char s_quiz_text[400];
static char s_quiz_opts[3][PM3DS_QUIZ_OPT];
static short s_quiz_starts[8], s_quiz_lens[8];
static int s_quiz_lines;
#define QUIZ_TEXT_SCALE 0.68f
#define QUIZ_LINE_H 13

static void quiz_refresh(void) {
    pm3ds_quiz_stats(&s_quiz_done, &s_quiz_tries, &s_quiz_right);
    for (int q = 0; q < PM3DS_QUIZ_COUNT; q++) {
        char opts[3][PM3DS_QUIZ_OPT];
        int n = pm3ds_quiz_options(q, opts), r = pm3ds_quiz_right(q);
        snprintf(s_quiz_ans[q], sizeof s_quiz_ans[q], "%s", r < n ? opts[r] : "");
    }
}

static void quiz_open(int q) {
    s_quiz_open = q;
    pm3ds_quiz_question(q, s_quiz_text, sizeof s_quiz_text);
    pm3ds_quiz_options(q, s_quiz_opts);
    s_quiz_lines = wrap_text(s_quiz_text, QUIZ_TEXT_SCALE, CX1 - CX0 - 12, s_quiz_starts, s_quiz_lens, 8);
}

static bool quiz_shown(int q) { return q < s_quiz_done || s_quiz_spoil; }

static void draw_quiz(void) {
    char buf[64];
    if (s_quiz_open >= 0) {
        int q = s_quiz_open;
        chip(CX0, HEAD_Y - 1, CX0 + 50, HEAD_Y + 19, "Back", COL_SEL, 0.85f);
        snprintf(buf, sizeof buf, T("Question %d"), q + 1);
        text(CX0 + 58, HEAD_Y, buf, 1.0f, 0, COL_WHITE);
        if (q >= s_quiz_done) text_right(CX1 - 2, HEAD_Y + 3, "Not asked yet", 0.68f, COL_HINT);
        card(CX0, 36, CX1, 36 + s_quiz_lines * QUIZ_LINE_H + 6, COL_AREA);
        for (int l = 0; l < s_quiz_lines; l++) {
            char line[96];
            int n = s_quiz_lens[l] < (int)sizeof line - 1 ? s_quiz_lens[l] : (int)sizeof line - 1;
            memcpy(line, s_quiz_text + s_quiz_starts[l], n);
            line[n] = '\0';
            text(CX0 + 6, 39 + l * QUIZ_LINE_H, line, QUIZ_TEXT_SCALE, 0, COL_WHITE);
        }
        float y = 36 + s_quiz_lines * QUIZ_LINE_H + 14;
        for (int k = 0; k < 3; k++) {
            bool right = quiz_shown(q) && k == pm3ds_quiz_right(q);
            tile(CX0, y, CX1, y + 20, 6, right ? COL_SEL : COL_TAB_ON);
            text(CX0 + 8, y + 2, s_quiz_opts[k], fit(s_quiz_opts[k], CX1 - CX0 - 60, 0.78f, 0.5f), 0, COL_WHITE);
            if (right) text_right(CX1 - 6, y + 4, "Right", 0.68f, COL_WHITE);
            y += 24;
        }
        return;
    }
    int pages = pages_of(PM3DS_QUIZ_COUNT, COOK_PER_PAGE);
    if (s_quiz_page >= pages) s_quiz_page = 0;
    snprintf(buf, sizeof buf, T("%d of %d answered, %d right of %d"), s_quiz_done, PM3DS_QUIZ_COUNT, s_quiz_right, s_quiz_tries);
    text(CX0 + 2, HEAD_Y, "Chuck Quizmo", fit("Chuck Quizmo", 88, 1.05f, 0.6f), 0, COL_WHITE);
    text(CX0 + 2, SUB_Y, buf, fit(buf, QUIZ_X0 - CX0 - 6, 0.8f, 0.45f), 0, COL_SUB);
    chip(COOK_ALL_X0, HEAD_Y, COOK_ALL_X1, HEAD_Y + 18, s_quiz_spoil ? "Hide new" : "Spoilers",
         s_quiz_spoil ? COL_SEL : COL_OFF, 0.7f);
    chip(QUIZ_X0, SUB_Y - 2, CX1, SUB_Y + 14, "Recipes", COL_TAB_ON, fit("Recipes", 56, 0.66f, 0.45f));
    pager(s_quiz_page, pages);
    bar(CX0 + 2, BODY_Y, CX1 - CX0 - 2, 5, s_quiz_done, PM3DS_QUIZ_COUNT, COL_GOLD);
    for (int slot = 0; slot < COOK_PER_PAGE; slot++) {
        int q = s_quiz_page * COOK_PER_PAGE + slot;
        if (q >= PM3DS_QUIZ_COUNT) break;
        float x0, y0, x1, y1;
        cook_entry_rect(slot, &x0, &y0, &x1, &y1);
        tile(x0, y0, x1, y1, 6, q < s_quiz_done ? COL_TAB_ON : COL_OFF);
        snprintf(buf, sizeof buf, "%d", q + 1);
        text(x0 + 4, y0 + 2, buf, 0.66f, 0, q < s_quiz_done ? COL_WHITE : RGBA(255, 255, 255, 170));
        const char *a = quiz_shown(q) ? s_quiz_ans[q] : "???";
        text(x0 + 22, y0 + 1, a, fit(a, x1 - x0 - 26, 0.72f, 0.45f), 0, q < s_quiz_done ? COL_WHITE : RGBA(255, 255, 255, 170));
    }
}

static void quiz_tap(int px, int py) {
    if (s_quiz_open >= 0) {
        if (in(px, py, CX0 - 4, HEAD_Y - 5, CX0 + 54, HEAD_Y + 23)) s_quiz_open = -1;
        return;
    }
    if (pager_tap(px, py, s_quiz_page, pages_of(PM3DS_QUIZ_COUNT, COOK_PER_PAGE)) < 0 &&
        in(px, py, QUIZ_X0 - 2, SUB_Y - 5, CX1 + 2, SUB_Y + 17)) {   /* the page arrows come first */
        s_quiz = false;
        return;
    }
    if (in(px, py, COOK_ALL_X0 - 2, HEAD_Y - 4, COOK_ALL_X1 + 2, HEAD_Y + 22)) {
        s_quiz_spoil = !s_quiz_spoil;
        return;
    }
    int np = pager_tap(px, py, s_quiz_page, pages_of(PM3DS_QUIZ_COUNT, COOK_PER_PAGE));
    if (np >= 0) { s_quiz_page = np; return; }
    for (int slot = 0; slot < COOK_PER_PAGE; slot++) {
        int q = s_quiz_page * COOK_PER_PAGE + slot;
        if (q >= PM3DS_QUIZ_COUNT) break;
        float x0, y0, x1, y1;
        cook_entry_rect(slot, &x0, &y0, &x1, &y1);
        if (in(px, py, x0, y0 - 1, x1, y1 + 2)) {
            quiz_open(q);
            return;
        }
    }
}

static void draw_cook(void) {
    char buf[40];
    if (s_quiz) {
        draw_quiz();
        return;
    }
    if (s_cook_open >= 0 && s_cook_open < s_cook.count) {
        draw_dish();
        return;
    }
    s_cook_open = -1;
    int pages = pages_of(s_cook.count, COOK_PER_PAGE);
    if (s_cook_page >= pages) s_cook_page = 0;
    snprintf(buf, sizeof buf, T("%d of %d cooked"), s_cook.cooked, s_cook.count);
    page_title("Recipes", buf);
    chip(COOK_ALL_X0, HEAD_Y, COOK_ALL_X1, HEAD_Y + 18, s_cook_all ? "Hide new" : "Show all",
         s_cook_all ? COL_SEL : COL_OFF, 0.7f);
    chip(QUIZ_X0, SUB_Y - 2, CX1, SUB_Y + 14, "Quizzes", COL_TAB_ON, fit("Quizzes", 56, 0.66f, 0.45f));
    pager(s_cook_page, pages);
    bar(CX0 + 2, BODY_Y, CX1 - CX0 - 2, 5, s_cook.cooked, s_cook.count, COL_GOLD);
    for (int slot = 0; slot < COOK_PER_PAGE; slot++) {
        int i = s_cook_page * COOK_PER_PAGE + slot;
        if (i >= s_cook.count) break;
        float x0, y0, x1, y1;
        cook_entry_rect(slot, &x0, &y0, &x1, &y1);
        bool known = cook_known(i);
        tile(x0, y0, x1, y1, 6, s_cook.done[i] ? COL_TAB_ON : COL_OFF);
        /* not cooked: a dark silhouette, the name hidden unless "Show all" */
        draw_icon(s_cook.icon[i], x0 + 4, y0 + 2, 14, s_cook.done[i] ? COL_WHITE : RGBA(70, 45, 25, known ? 200 : 150));
        const char *name = known ? s_cook.name[i] : "???";
        text(x0 + 21, y0 + 1, name, fit(name, x1 - x0 - 26, 0.75f, 0.5f), 0, known ? COL_WHITE : RGBA(255, 255, 255, 170));
    }
}

static void cook_tap(int px, int py) {
    if (s_quiz) {
        quiz_tap(px, py);
        return;
    }
    if (s_cook_open < 0 && pager_tap(px, py, s_cook_page, pages_of(s_cook.count, COOK_PER_PAGE)) < 0 &&
        in(px, py, QUIZ_X0 - 2, SUB_Y - 5, CX1 + 2, SUB_Y + 17)) {   /* the page arrows come first */
        s_quiz = true;
        s_quiz_open = -1;
        quiz_refresh();
        return;
    }
    if (s_cook_open >= 0) {
        if (in(px, py, CX0 - 4, HEAD_Y - 5, CX0 + 54, HEAD_Y + 23)) {
            s_cook_open = -1;
            return;
        }
        int np = pager_tap(px, py, s_dish_page, pages_of(s_dish.ways, dish_ways_per_page()));
        if (np >= 0) s_dish_page = np;
        else if (py >= dish_ways_y0()) s_dish_page++;   /* wraps in draw_dish */
        return;
    }
    if (in(px, py, COOK_ALL_X0 - 2, HEAD_Y - 4, COOK_ALL_X1 + 2, HEAD_Y + 22)) {
        s_cook_all = !s_cook_all;
        return;
    }
    int np = pager_tap(px, py, s_cook_page, pages_of(s_cook.count, COOK_PER_PAGE));
    if (np >= 0) { s_cook_page = np; return; }
    for (int slot = 0; slot < COOK_PER_PAGE; slot++) {
        int i = s_cook_page * COOK_PER_PAGE + slot;
        if (i >= s_cook.count) break;
        float x0, y0, x1, y1;
        cook_entry_rect(slot, &x0, &y0, &x1, &y1);
        if (in(px, py, x0, y0 - 1, x1, y1 + 2)) {
            if (cook_known(i)) cook_open(i);
            return;
        }
    }
}

/* ---- battle page and touch menus ------------------------------------------ */
/* the game's list menu as buttons: two columns, TM_ROWS rows per page */
#define TM_Y0 BODY_Y
#define TM_ROW_H 30
#define TM_ROWS 5
#define TM_PER_PAGE (TM_ROWS * 2)
/* the pad shown in battle when no menu is open */
#define PAD_A_X 244
#define PAD_A_Y 124
#define PAD_A_R 54
#define PAD_B_X 150
#define PAD_B_Y 158
#define PAD_B_R 32
static const float s_pad_stick[4] = { 60, 56, 156, 100 };   /* x0 y0 x1 y1: hold the stick left */

static bool tm_open(void) { return s_tm.kind != PM3DS_TM_NONE && s_info.in_game; }
/* ---- Badges: the pause menu's badge list, a tap puts one on or takes it off,
 * and three loadouts underneath to save and switch whole sets ---- */
static pm3ds_badge_list_t s_badges;
static int s_badge_page, s_badge_msg_timer;
static char s_badge_msg[48];   /* shown in place of the BP line for a while */
static bool s_loadout_saving;  /* Save tapped: the next loadout tapped stores the badges on now */
static int s_loadout_count[PM3DS_BADGE_LOADOUTS];
#define BADGE_ROWS 5
#define GEAR_Y0 154                         /* boots and hammer (GitHub #79), above the loadouts */
#define GEAR_Y1 172
#define GEAR_X(g, l) (CX0 + (g) * 126 + 22 + (l) * 34)   /* level l's chip, 32 wide */
#define BADGE_PER_PAGE (BADGE_ROWS * 2)
#define LOADOUT_Y0 176
#define LOADOUT_Y1 198
#define LOADOUT_X(n) (CX0 + 66 + (n) * 34)   /* the three loadout chips, 30 wide */
#define LOADOUT_SAVE_X0 (CX0 + 172)

static void badge_entry_rect(int slot, float *x0, float *y0, float *x1, float *y1) {
    *x0 = CX0 + (slot / BADGE_ROWS) * 126;
    *y0 = 53 + (slot % BADGE_ROWS) * 20;
    *x1 = *x0 + 122;
    *y1 = *y0 + 18;
}

static void badge_say(const char *msg) {
    snprintf(s_badge_msg, sizeof s_badge_msg, "%s", msg);
    s_badge_msg_timer = 150;
}

static void badges_refresh(void) {
    pm3ds_badge_query(&s_badges);
    for (int n = 0; n < PM3DS_BADGE_LOADOUTS; n++) s_loadout_count[n] = pm3ds_badge_loadout_count(n);
}

static void draw_badges(void) {
    char buf[48];
    int pages = pages_of(s_badges.count, BADGE_PER_PAGE), left = s_badges.bp_max - s_badges.bp_used;
    if (s_badge_page >= pages) s_badge_page = 0;
    if (!s_badges.can_change) snprintf(buf, sizeof buf, "%s", T("Badges change outside battles and menus."));
    else if (s_badge_msg_timer > 0) snprintf(buf, sizeof buf, "%s", s_badge_msg);
    else snprintf(buf, sizeof buf, T("BP left: %d of %d"), left, s_badges.bp_max);
    text(CX0 + 2, HEAD_Y, "Badges", 1.05f, 0, COL_WHITE);
    text_in(CX0 + 2, SUB_Y, buf, pages > 1 ? 156 : 246, 0.8f, 0.5f, 0, COL_SUB);
    pager(s_badge_page, pages);
    bar(CX0 + 2, BODY_Y, CX1 - CX0 - 2, 5, s_badges.bp_used, s_badges.bp_max, COL_GOLD);
    if (s_badges.count == 0) {
        text_center((CX0 + CX1) / 2, 100, "No badges yet.", 0.8f, 0, COL_HINT);
    }
    for (int slot = 0; slot < BADGE_PER_PAGE; slot++) {
        int i = s_badge_page * BADGE_PER_PAGE + slot;
        if (i >= s_badges.count) break;
        float x0, y0, x1, y1;
        badge_entry_rect(slot, &x0, &y0, &x1, &y1);
        bool on = s_badges.on[i], fits = on || s_badges.cost[i] <= left;
        tile(x0, y0, x1, y1, 6, on ? COL_SEL : fits ? COL_TAB_ON : COL_OFF);
        draw_icon(s_badges.icon[i], x0 + 4, y0 + 2, 14, fits ? COL_WHITE : RGBA(255, 255, 255, 150));
        text(x0 + 21, y0 + 1, s_badges.name[i], fit(s_badges.name[i], x1 - x0 - 42, 0.75f, 0.45f), 0, COL_WHITE);
        snprintf(buf, sizeof buf, "%d", s_badges.cost[i]);
        text_right(x1 - 5, y0 + 2, buf, 0.75f, on ? COL_WHITE : COL_HINT);
    }
    /* boots and hammer: any level found so far (GitHub #79) */
    for (int g = 0; g < 2; g++) {
        static const char *const names[3] = { "Normal", "Super", "Ultra" };
        int best = pm3ds_gear_best(g), cur = pm3ds_gear_level(g);
        if (best < 0) continue;
        draw_ui_icon(g ? UI_ICON_HAMMER : UI_ICON_BOOTS, CX0 + g * 126 + 2, GEAR_Y0 + 1, 16, COL_WHITE);
        for (int l = 0; l <= best && l < 3; l++) {
            u32 face = l == cur ? COL_SEL : s_badges.can_change ? COL_TAB_ON : COL_OFF;
            chip(GEAR_X(g, l), GEAR_Y0, GEAR_X(g, l) + 32, GEAR_Y1, names[l], face, fit(names[l], 28, 0.6f, 0.4f));
        }
    }
    /* loadouts: 1 2 3 put a saved set on; Save, then a number, stores the badges on now */
    text(CX0 + 2, LOADOUT_Y0 + 4, "Loadouts", fit("Loadouts", 60, 0.75f, 0.5f), 0, COL_SUB);
    for (int n = 0; n < PM3DS_BADGE_LOADOUTS; n++) {
        char num[4];
        snprintf(num, sizeof num, "%d", n + 1);
        u32 face = s_loadout_saving ? RGBA(255, 196, 64, 255) : s_loadout_count[n] ? COL_SEL : COL_OFF;
        chip(LOADOUT_X(n), LOADOUT_Y0, LOADOUT_X(n) + 30, LOADOUT_Y1, num, face, 0.85f);
    }
    chip(LOADOUT_SAVE_X0, LOADOUT_Y0, CX1, LOADOUT_Y1, s_loadout_saving ? "Save to..." : "Save",
         s_loadout_saving ? RGBA(255, 196, 64, 255) : COL_TAB_ON, 0.8f);
}

static void loadout_tap(int n) {
    char buf[48];
    if (s_loadout_saving) {
        s_loadout_saving = false;
        int k = pm3ds_badge_loadout_save(n);
        snprintf(buf, sizeof buf, T("Loadout %d saved: %d badges"), n + 1, k);
        badge_say(buf);
    } else if (s_loadout_count[n] == 0) {
        snprintf(buf, sizeof buf, T("Loadout %d is empty: tap Save first"), n + 1);
        badge_say(buf);
    } else {
        int skipped, on = pm3ds_badge_loadout_load(n, &skipped);
        if (on < 0) return;   /* can't change now: the BP line says so */
        if (skipped) snprintf(buf, sizeof buf, T("Loadout %d on (%d didn't fit)"), n + 1, skipped);
        else snprintf(buf, sizeof buf, T("Loadout %d on"), n + 1);
        badge_say(buf);
    }
    badges_refresh();
}

static void badges_tap(int px, int py) {
    int np = pager_tap(px, py, s_badge_page, pages_of(s_badges.count, BADGE_PER_PAGE));
    if (np >= 0) { s_badge_page = np; return; }
    if (py >= GEAR_Y0 - 1 && py < GEAR_Y1 + 2) {   /* boots and hammer */
        for (int g = 0; g < 2; g++)
            for (int l = 0; l <= pm3ds_gear_best(g) && l < 3; l++)
                if (px >= GEAR_X(g, l) - 1 && px < GEAR_X(g, l) + 33 && l != pm3ds_gear_level(g)) {
                    if (pm3ds_gear_set(g, l)) badge_say(T(g ? "Hammer changed" : "Boots changed"));
                    return;
                }
        return;
    }
    if (py >= LOADOUT_Y0 - 3 && py < LOADOUT_Y1 + 4) {
        for (int n = 0; n < PM3DS_BADGE_LOADOUTS; n++) {
            if (px >= LOADOUT_X(n) - 2 && px < LOADOUT_X(n) + 32) { loadout_tap(n); return; }
        }
        if (px >= LOADOUT_SAVE_X0 - 2) s_loadout_saving = !s_loadout_saving;
        return;
    }
    s_loadout_saving = false;
    for (int slot = 0; slot < BADGE_PER_PAGE; slot++) {
        int i = s_badge_page * BADGE_PER_PAGE + slot;
        if (i >= s_badges.count) break;
        float x0, y0, x1, y1;
        badge_entry_rect(slot, &x0, &y0, &x1, &y1);
        if (in(px, py, x0, y0 - 1, x1, y1 + 2)) {
            int r = pm3ds_badge_toggle(s_badges.item[i]);
            if (r == PM3DS_BADGE_NO_BP) badge_say(T("Not enough BP."));
            else if (r == PM3DS_BADGE_TOO_MANY) badge_say(T("Too many badges on."));
            else s_badge_msg_timer = 0;
            badges_refresh();
            return;
        }
    }
}

static bool battle_page(void) { return s_info.in_battle && s_tab <= TAB_GAME_LAST; }

static int tm_pages(void) { return (s_tm.count + TM_PER_PAGE - 1) / TM_PER_PAGE; }

/* short lists get one column of wide buttons */
static void tm_entry_rect(int slot, float *x0, float *y0, float *x1, float *y1) {
    int col = slot / TM_ROWS, row = slot % TM_ROWS;
    *x0 = CX0 + col * 126;
    *x1 = s_tm.count <= TM_ROWS ? CX1 : *x0 + 122;
    *y0 = TM_Y0 + row * TM_ROW_H;
    *y1 = *y0 + TM_ROW_H - 4;
}

static void disc(float cx, float cy, float r, u32 c) { round_rect(cx - r, cy - r, cx + r, cy + r, r, c); }

/* The menus' icons (boots, hammer, items, ...) decoded from the game's own
 * HudScripts (pm3ds_tm_icon_rgba), so no art ships with the port: 64 cells of
 * 32x32 in one texture, filled as menus show icons they haven't shown before.
 * The first UI_ICON_COUNT cells hold the pages' own icons (pm3ds_ui_icon_script),
 * decoded once the title is up (the Options rail shows them there too, GitHub
 * #51) and never given away. */
#define ICON_CELLS 64
#define ICON_TEX 256
static C3D_Tex s_icons;
static bool s_icons_ok, s_icons_dirty, s_ui_icons;
static u8 *s_icon_canvas;
static const void *s_icon_key[ICON_CELLS];
static u8 s_icon_w[ICON_CELLS], s_icon_h[ICON_CELLS];   /* 0: the script has no image */
static int s_icon_used = UI_ICON_COUNT;

/* decode script's icon into cell i; false if it has none */
static bool icon_decode(const void *script, int i) {
    u8 px[32 * 32 * 4];
    int w = 0, h = 0;
    s_icon_key[i] = script;
    s_icon_w[i] = s_icon_h[i] = 0;
    if (!script || !pm3ds_tm_icon_rgba(script, px, &w, &h)) return false;
    int cx = (i % 8) * 32, cy = (i / 8) * 32;
    for (int y = 0; y < 32; y++) memset(s_icon_canvas + ((size_t)(cy + y) * ICON_TEX + cx) * 4, 0, 32 * 4);
    for (int y = 0; y < h; y++)
        memcpy(s_icon_canvas + ((size_t)(cy + y) * ICON_TEX + cx) * 4, px + (size_t)y * w * 4, (size_t)w * 4);
    s_icon_w[i] = (u8)w;
    s_icon_h[i] = (u8)h;
    s_icons_dirty = true;
    return true;
}

/* the cell holding script's icon (decoding it on first use), -1 if none */
static int icon_cell(const void *script) {
    if (!script) return -1;
    for (int i = 0; i < s_icon_used; i++)
        if (s_icon_key[i] == script) return s_icon_w[i] ? i : -1;
    if (!s_icon_canvas && !(s_icon_canvas = (u8 *)calloc(ICON_TEX * ICON_TEX, 4))) return -1;
    int i = s_icon_used < ICON_CELLS ? s_icon_used++
                                     : UI_ICON_COUNT + (int)((uintptr_t)script >> 2) % (ICON_CELLS - UI_ICON_COUNT);
    return icon_decode(script, i) ? i : -1;
}

/* decode what the open menu needs and upload once (from the tick) */
static void icons_update(void) {
    if (!s_ui_icons && (s_info.in_game || s_info.ui_ready) &&
        (s_icon_canvas || (s_icon_canvas = (u8 *)calloc(ICON_TEX * ICON_TEX, 4)))) {
        for (int i = 0; i < UI_ICON_COUNT; i++) icon_decode(pm3ds_ui_icon_script(i), i);
        s_ui_icons = true;
    }
    for (int i = 0; i < s_tm.count && s_tm.kind != PM3DS_TM_NONE; i++) icon_cell(s_tm.e[i].icon);
    if (!s_icons_dirty) return;
    s_icons_dirty = false;
    if (s_icons_ok || C3D_TexInit(&s_icons, ICON_TEX, ICON_TEX, GPU_RGBA8)) {
        upload_rgba8(&s_icons, s_icon_canvas, ICON_TEX, ICON_TEX);
        C3D_TexSetFilter(&s_icons, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&s_icons, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s_icons_ok = true;
    }
}

/* cell i fitted into a size x size box at (x, y) */
static void draw_cell(int i, float x, float y, float size, u32 c) {
    float w = s_icon_w[i], h = s_icon_h[i], s = size / (w > h ? w : h);
    float x0 = x + (size - w * s) / 2, y0 = y + (size - h * s) / 2;
    float u0 = (float)((i % 8) * 32) / ICON_TEX, v0 = (float)((i / 8) * 32) / ICON_TEX;
    quad2(&s_icons, x0, y0, x0 + w * s, y0 + h * s, u0, v0, u0 + w / ICON_TEX, v0 + h / ICON_TEX, c, c);
}

/* an entry's icon fitted into a size x size box at (x, y); false if it has none */
static bool draw_icon(const void *script, float x, float y, float size, u32 c) {
    int i = s_icons_ok ? icon_cell(script) : -1;
    if (i < 0) return false;
    draw_cell(i, x, y, size, c);
    return true;
}

/* a page icon (UI_ICON_*); false until it is decoded and uploaded */
static bool draw_ui_icon(int id, float x, float y, float size, u32 c) {
    if (!s_icons_ok || !s_ui_icons || id < 0 || id >= UI_ICON_COUNT || !s_icon_w[id]) return false;
    draw_cell(id, x, y, size, c);
    return true;
}

/* The command reel as big tiles: what opens Jump or Hammer on a bottom row,
 * the rest (Strategies, Items, Star Spirits; a partner's whole reel) above it.
 * Same order as the game's reel inside a row. False (use the list) if a row
 * would need more than three tiles. */
#define TILE_Y1 (PANEL_Y1 - 8)
static bool wheel_tile_rect(int idx, float *x0, float *y0, float *x1, float *y1) {
    if (s_tm.kind != PM3DS_TM_WHEEL || s_tm.count < 1) return false;
    int n[2] = { 0, 0 }, pos = -1, row = 0;
    for (int i = 0; i < s_tm.count; i++) {
        int r = s_tm.e[i].attack ? 1 : 0;
        if (i == idx) { pos = n[r]; row = r; }
        n[r]++;
    }
    if (n[0] > 3 || n[1] > 3 || pos < 0) return false;
    int rows = (n[0] > 0) + (n[1] > 0);
    float gap = 6, h = (TILE_Y1 - TM_Y0 - (rows - 1) * gap) / rows;
    float top = TM_Y0 + (rows == 2 && row == 1 ? h + gap : 0);
    float w = (CX1 - CX0 - (n[row] - 1) * 5.0f) / n[row];
    *x0 = CX0 + pos * (w + 5);
    *x1 = *x0 + w;
    *y0 = top;
    *y1 = top + h;
    return true;
}

static void draw_wheel_tiles(void) {
    for (int i = 0; i < s_tm.count; i++) {
        const pm3ds_tm_entry_t *e = &s_tm.e[i];
        float x0, y0, x1, y1;
        wheel_tile_rect(i, &x0, &y0, &x1, &y1);
        bool sel = i == s_tm.cursor;
        tile(x0, y0, x1, y1, 10, !e->enabled ? COL_OFF : sel ? COL_SEL : COL_TAB_ON);
        float cy = (y0 + y1) / 2 - 26;   /* icon 32 + gap 4 + label 16 */
        draw_icon(e->icon, (x0 + x1) / 2 - 16, cy, 32, e->enabled ? COL_WHITE : RGBA(255, 255, 255, 160));
        float sc = fit(e->name, x1 - x0 - 8, 1.0f, 0.5f);
        text_center((x0 + x1) / 2, cy + 36 + (1.0f - sc) * 8, e->name, sc, 0,
                    e->enabled ? COL_WHITE : RGBA(255, 255, 255, 120));
    }
}

/* in battle the rail shows Mario's numbers instead of tabs */
static void draw_battle_rail(void) {
    const pm3ds_hud_info_t *h = &s_info;
    char buf[24];
    for (int k = 0; k < 2; k++) {
        float y0 = 6 + k * 50;
        int v = k ? h->fp : h->hp, m = k ? h->max_fp : h->max_hp;
        tile(5, y0, 46, y0 + 46, 8, COL_TAB_ON);
        image(k ? UI_IMG_FLOWER : UI_IMG_HEART, 17.5f, y0 + 4, 1.0f, COL_WHITE);
        snprintf(buf, sizeof buf, "%d/%d", v, m);
        text_center(25.5f, y0 + 21, buf, fit(buf, 36, 0.62f, 0.45f), 0, COL_WHITE);
        bar(10, y0 + 37, 31, 4, v, m, k ? COL_FP : COL_HP);
    }
    if (h->max_star_power > 0) {
        float y0 = 106;
        int n = h->max_star_power < 7 ? h->max_star_power : 7, full = h->star_power / 256;
        int cols = n > 4 ? 2 : 1, rows = (n + cols - 1) / cols;
        tile(5, y0, 46, y0 + 46, 8, COL_TAB_ON);
        for (int i = 0; i < n; i++) {
            float x = cols == 1 ? 21.5f : 15.5f + (i % 2) * 12;
            float y = y0 + 23 - rows * 5 + (i / cols) * 10;
            image(i < full ? UI_IMG_POW_STAR : UI_IMG_POW_EMPTY, x, y, 1.0f, COL_WHITE);
        }
    }
    float y0 = 156;
    tile(5, y0, 46, y0 + 46, 8, COL_TAB_ON);
    image(UI_IMG_COIN, 17.5f, y0 + 6, 1.0f, COL_WHITE);
    snprintf(buf, sizeof buf, "%d", h->coins);
    text_center(25.5f, y0 + 24, buf, fit(buf, 36, 0.7f, 0.5f), 0, COL_WHITE);
}

/* Choosing a target: the enemies' side of the stage, cut live from the top
 * screen's last frame (pm3ds_gfx_top_copy), with a numbered marker over each
 * target; tap one in the picture or its numbered tile underneath */
#define TGT_FX0 CX0
#define TGT_FY0 (BODY_Y + 2)
#define TGT_FX1 CX1
#define TGT_FY1 164
#define TGT_TY0 170
#define TGT_TY1 200
#define TGT_TILES 4
#define TGT_FY1_2ROWS 147   /* the picture's bottom with two rows of tiles */
extern C3D_Tex *pm3ds_gfx_top_copy(void);
static float s_tgt_x0, s_tgt_y0, s_tgt_k;   /* the top-screen area shown: origin and scale to the strip */
static int tgt_tiles(void);
static float tgt_fy1(void) { return tgt_tiles() > 2 ? TGT_FY1_2ROWS : TGT_FY1; }   /* the picture's bottom */

/* top-screen area X0..X1, Y0..Y1 into the bottom-screen rect x0..x1, y0..y1;
 * the copy is the framebuffer turned a quarter: pixel X, Y at
 * u = (240 - Y) / 256, v = (512 - X) / 512 */
static void top_area(C3D_Tex *t, float X0, float Y0, float X1, float Y1, float x0, float y0, float x1, float y1) {
    if (s_vcount + 6 > UI_MAX_VERTS) return;
    float ua = (240.0f - Y0) / 256.0f, ub = (240.0f - Y1) / 256.0f;
    float va = (512.0f - X0) / 512.0f, vb = (512.0f - X1) / 512.0f;
    use_tex(t);
    vert(x0, y0, COL_WHITE, ua, va); vert(x1, y0, COL_WHITE, ua, vb); vert(x0, y1, COL_WHITE, ub, va);
    vert(x1, y0, COL_WHITE, ua, vb); vert(x1, y1, COL_WHITE, ub, vb); vert(x0, y1, COL_WHITE, ub, va);
}

/* the part of the top screen around every target, shaped like the strip */
static void tgt_area(void) {
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < s_tm.count; i++) {
        const pm3ds_tm_entry_t *e = &s_tm.e[i];
        if (e->pr <= 0) continue;
        if (e->px - e->pr < minx) minx = e->px - e->pr;
        if (e->px + e->pr > maxx) maxx = e->px + e->pr;
        if (e->py - e->pr < miny) miny = e->py - e->pr;
        if (e->py + e->pr > maxy) maxy = e->py + e->pr;
    }
    const float aspect = (TGT_FX1 - TGT_FX0) / (float)(tgt_fy1() - TGT_FY0);
    if (minx > maxx) { minx = 200; maxx = 400; miny = 60; maxy = 200; }
    float cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    float w = maxx - minx + 24, h = maxy - miny + 24;
    if (w < 170) w = 170;   /* one small enemy alone stays sharp */
    if (w / h < aspect) w = h * aspect; else h = w / aspect;
    if (w > 400) { w = 400; h = w / aspect; }
    if (h > 240) { h = 240; w = h * aspect; }
    float x0 = cx - w / 2, y0 = cy - h / 2;
    if (x0 < 0) x0 = 0;
    if (x0 + w > 400) x0 = 400 - w;
    if (y0 < 0) y0 = 0;
    if (y0 + h > 240) y0 = 240 - h;
    s_tgt_x0 = x0;
    s_tgt_y0 = y0;
    s_tgt_k = (TGT_FX1 - TGT_FX0) / w;
}

static float tgt_bx(float X) { return TGT_FX0 + (X - s_tgt_x0) * s_tgt_k; }
static float tgt_by(float Y) { return TGT_FY0 + (Y - s_tgt_y0) * s_tgt_k; }

static int tgt_tiles(void) { return s_tm.count < TGT_TILES ? s_tm.count : TGT_TILES; }
/* 3 or 4 targets: two rows of two, wide enough for long names (GitHub #60) */
static int tgt_cols(void) { return tgt_tiles() > 2 ? 2 : tgt_tiles(); }

static void tgt_tile_rect(int slot, float *x0, float *y0, float *x1, float *y1) {
    int cols = tgt_cols(), col = slot % cols, row = slot / cols;
    float gap = 5, w = (TGT_FX1 - TGT_FX0 - (cols - 1) * gap) / cols;
    *x0 = TGT_FX0 + col * (w + gap);
    *x1 = *x0 + w;
    if (tgt_tiles() > 2) {
        *y0 = TGT_FY1_2ROWS + 5 + row * 29;
        *y1 = *y0 + 26;
    } else {
        *y0 = TGT_TY0;
        *y1 = TGT_TY1;
    }
}

static void draw_targets(void) {
    char buf[24];
    int pages = pages_of(s_tm.count, TGT_TILES);
    if (s_tm.kind != s_tm_kind_seen || s_tm.count != s_tm_count_seen) {
        s_tm_kind_seen = s_tm.kind;
        s_tm_count_seen = s_tm.count;
        s_tm_page = s_tm.cursor >= 0 ? s_tm.cursor / TGT_TILES : 0;
    }
    if (s_tm_page >= pages) s_tm_page = 0;
    page_title(s_tm.all_targets ? "Targets: all" : s_tm.title, "Tap an enemy");
    pager(s_tm_page, pages);

    /* the stage */
    tgt_area();
    C3D_Tex *t = pm3ds_gfx_top_copy();
    round_rect(TGT_FX0 - 2, TGT_FY0 - 2, TGT_FX1 + 2, tgt_fy1() + 2, 6, COL_PANEL_EDGE);
    if (t) {
        float w = (TGT_FX1 - TGT_FX0) / s_tgt_k, h = (tgt_fy1() - TGT_FY0) / s_tgt_k;
        top_area(t, s_tgt_x0, s_tgt_y0, s_tgt_x0 + w, s_tgt_y0 + h, TGT_FX0, TGT_FY0, TGT_FX1, tgt_fy1());
    } else {
        fill(TGT_FX0, TGT_FY0, TGT_FX1, tgt_fy1(), RGBA(90, 70, 60, 255));
    }
    for (int i = 0; i < s_tm.count; i++) {
        const pm3ds_tm_entry_t *e = &s_tm.e[i];
        if (e->pr <= 0) continue;
        bool sel = s_tm.all_targets || i == s_tm.cursor;
        float bx = tgt_bx(e->px), r = e->pr * s_tgt_k;
        if (sel) {   /* a soft box around the chosen one */
            float bt = tgt_by(e->py) - r, bb = tgt_by(e->py) + r;
            round_rect(bx - r * 0.8f, bt < TGT_FY0 ? TGT_FY0 : bt, bx + r * 0.8f, bb > tgt_fy1() ? tgt_fy1() : bb, 8,
                       RGBA(255, 214, 120, 70));
        }
        float my = tgt_by(e->py - e->pr * 0.75f) - 6;
        if (my < TGT_FY0 + 10) my = TGT_FY0 + 10;
        if (my > tgt_fy1() - 10) my = tgt_fy1() - 10;
        snprintf(buf, sizeof buf, "%d", i + 1);
        disc(bx, my, 9, sel ? RGBA(230, 120, 30, 255) : COL_PANEL_EDGE);
        disc(bx, my, 7, sel ? COL_SEL : COL_PANEL);
        text_center(bx, my - 7, buf, 0.75f, 0, COL_WHITE);
    }

    /* the numbered tiles */
    int n = tgt_tiles();
    for (int slot = 0; slot < n; slot++) {
        int i = s_tm_page * TGT_TILES + slot;
        if (i >= s_tm.count) break;
        const pm3ds_tm_entry_t *e = &s_tm.e[i];
        float x0, y0, x1, y1;
        tgt_tile_rect(slot, &x0, &y0, &x1, &y1);
        bool sel = s_tm.all_targets || i == s_tm.cursor;
        tile(x0, y0, x1, y1, 7, sel ? COL_SEL : COL_TAB_ON);
        float right = x1 - 5;
        float ty = y0 + (y1 - y0) / 2 - 9;   /* text centred in the tile (30 or 26 high) */
        if (tgt_cols() <= 2 && e->enemy) {   /* room for the HP: known once tattled */
            bool known = e->cost_kind == PM3DS_TM_COST_HP;
            if (known) snprintf(buf, sizeof buf, "HP %d/%d", e->cost, e->max_hp);
            else strcpy(buf, "HP ?");
            text_right(right, ty + 3, buf, 0.62f, known ? RGBA(200, 40, 40, 255) : COL_SUB);
            right -= 52;
        }
        snprintf(buf, sizeof buf, "%d", i + 1);
        text(x0 + 6, ty, buf, 0.85f, 0, COL_WHITE);
        float left = x0 + 18;
        text(left, ty + 1, e->name, fit(e->name, right - left, 0.8f, 0.5f), 0, COL_WHITE);
    }
}

static bool targets_tap(int px, int py) {
    int pages = pages_of(s_tm.count, TGT_TILES);
    int np = pager_tap(px, py, s_tm_page, pages);
    if (np >= 0) {
        s_tm_page = np;
        return true;
    }
    if (in(px, py, TGT_FX0, TGT_FY0, TGT_FX1, tgt_fy1()) && s_tgt_k > 0) {   /* the nearest one in the picture */
        int best = -1;
        float bd = 1e9f;
        for (int i = 0; i < s_tm.count; i++) {
            const pm3ds_tm_entry_t *e = &s_tm.e[i];
            if (e->pr <= 0) continue;
            float dx = px - tgt_bx(e->px), dy = py - tgt_by(e->py), d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) {
            pm3ds_tm_pick(s_tm.kind, best);
            return true;
        }
        return false;
    }
    for (int slot = 0; slot < tgt_tiles(); slot++) {
        int i = s_tm_page * TGT_TILES + slot;
        if (i >= s_tm.count) break;
        float x0, y0, x1, y1;
        tgt_tile_rect(slot, &x0, &y0, &x1, &y1);
        if (in(px, py, x0, y0, x1, y1)) {
            pm3ds_tm_pick(s_tm.kind, i);
            return true;
        }
    }
    return false;
}

static void draw_tmenu(void) {
    char buf[24];
    if (s_tm.kind == PM3DS_TM_TARGET) {
        draw_targets();
        return;
    }
    /* keep the page on the game's cursor when a menu opens */
    if (s_tm.kind != s_tm_kind_seen || s_tm.count != s_tm_count_seen) {
        s_tm_kind_seen = s_tm.kind;
        s_tm_count_seen = s_tm.count;
        s_tm_page = s_tm.cursor >= 0 ? s_tm.cursor / TM_PER_PAGE : 0;
    }
    if (s_tm_page >= tm_pages()) s_tm_page = 0;
    const char *title = s_tm.kind == PM3DS_TM_TARGET && s_tm.all_targets ? "Targets: all" : s_tm.title;
    page_title(title, s_tm.kind == PM3DS_TM_WHEEL ? "Pick a command" : s_tm.kind == PM3DS_TM_TARGET ? "Tap a target" : NULL);
    float tx, ty, tx1, ty1;
    if (wheel_tile_rect(0, &tx, &ty, &tx1, &ty1)) {
        draw_wheel_tiles();
        return;
    }
    pager(s_tm_page, tm_pages());
    for (int slot = 0; slot < TM_PER_PAGE; slot++) {
        int i = s_tm_page * TM_PER_PAGE + slot;
        if (i >= s_tm.count) break;
        const pm3ds_tm_entry_t *e = &s_tm.e[i];
        float x0, y0, x1, y1;
        tm_entry_rect(slot, &x0, &y0, &x1, &y1);
        bool sel = i == s_tm.cursor;
        tile(x0, y0, x1, y1, 8, !e->enabled ? COL_OFF : sel ? COL_SEL : COL_TAB_ON);
        /* cost on the right, then the name in what is left */
        float right = x1 - 6;
        buf[0] = 0;
        switch (e->cost_kind) {
            case PM3DS_TM_COST_FP: snprintf(buf, sizeof buf, "%d FP", e->cost); break;
            case PM3DS_TM_COST_SP: snprintf(buf, sizeof buf, "%d SP", e->cost); break;
            case PM3DS_TM_COST_COINS: snprintf(buf, sizeof buf, "%d c", e->cost); break;
            case PM3DS_TM_COST_STAR_PIECES: snprintf(buf, sizeof buf, "%d sp", e->cost); break;
            case PM3DS_TM_COST_HP: snprintf(buf, sizeof buf, "%d/%d", e->cost, e->max_hp); break;
        }
        u32 tc = e->enabled ? COL_WHITE : RGBA(255, 255, 255, 120);
        if (buf[0]) {
            float cw = strlen(buf) * 8.0f;
            small_text(right - cw, y0 + 9, buf, 12, e->cost_kind == PM3DS_TM_COST_HP ? RGBA(200, 40, 40, 255)
                                                                                     : RGBA(90, 60, 30, 255));
            right -= cw + 4;
        }
        float left = x0 + 6;
        if (draw_icon(e->icon, x0 + 4, y0 + 3, 20, e->enabled ? COL_WHITE : RGBA(255, 255, 255, 160))) left += 22;
        float sc = fit(e->name, right - left, 0.9f, 0.55f);
        text(left, y0 + 5 + (0.9f - sc) * 8, e->name, sc, 0, tc);
    }
}

static void draw_pad(void) {
    bool a = pm3ds_touch_pad & 0x8000, b = pm3ds_touch_pad & 0x4000, st = pm3ds_touch_stick_x != 0;
    page_title("Action command", "Tap or hold");
    disc(PAD_A_X + 3, PAD_A_Y + 3, PAD_A_R, COL_SHADOW);
    disc(PAD_A_X, PAD_A_Y, PAD_A_R, RGBA(40, 70, 170, 255));
    disc(PAD_A_X, PAD_A_Y, PAD_A_R - 4, a ? RGBA(120, 170, 255, 255) : RGBA(70, 120, 230, 255));
    text_center(PAD_A_X, PAD_A_Y - 24, "A", 3.0f, 1, COL_WHITE);
    disc(PAD_B_X + 3, PAD_B_Y + 3, PAD_B_R, COL_SHADOW);
    disc(PAD_B_X, PAD_B_Y, PAD_B_R, RGBA(30, 120, 50, 255));
    disc(PAD_B_X, PAD_B_Y, PAD_B_R - 4, b ? RGBA(130, 220, 140, 255) : RGBA(60, 170, 80, 255));
    text_center(PAD_B_X, PAD_B_Y - 16, "B", 2.0f, 1, COL_WHITE);
    tile(s_pad_stick[0], s_pad_stick[1], s_pad_stick[2], s_pad_stick[3], 10, st ? RGBA(255, 196, 64, 255) : COL_TAB_ON);
    text_center((s_pad_stick[0] + s_pad_stick[2]) / 2, s_pad_stick[1] + 6, "< Stick", 1.0f, 0, COL_WHITE);
    text_center((s_pad_stick[0] + s_pad_stick[2]) / 2, s_pad_stick[1] + 24, "hold", 0.7f, 0, COL_HINT);
}

/* the touch pad's buttons under the finger (held state, every frame) */
static void pad_held(int px, int py) {
    float dx, dy;
    if (tm_open() || !battle_page()) return;
    dx = px - PAD_A_X; dy = py - PAD_A_Y;
    if (dx * dx + dy * dy <= PAD_A_R * PAD_A_R) pm3ds_touch_pad |= 0x8000;
    dx = px - PAD_B_X; dy = py - PAD_B_Y;
    if (dx * dx + dy * dy <= PAD_B_R * PAD_B_R) pm3ds_touch_pad |= 0x4000;
    if (in(px, py, s_pad_stick[0], s_pad_stick[1], s_pad_stick[2], s_pad_stick[3])) pm3ds_touch_stick_x = -80;
}

/* a tap on the menu: true if it was used */
static bool tmenu_tap(int px, int py) {
    if (!tm_open()) return false;
    if (s_tm.kind == PM3DS_TM_TARGET) return targets_tap(px, py);
    float tx0, ty0, tx1, ty1;
    if (wheel_tile_rect(0, &tx0, &ty0, &tx1, &ty1)) {
        for (int i = 0; i < s_tm.count; i++) {
            wheel_tile_rect(i, &tx0, &ty0, &tx1, &ty1);
            if (in(px, py, tx0, ty0, tx1, ty1)) {
                pm3ds_tm_pick(s_tm.kind, i);
                return true;
            }
        }
        return false;
    }
    int np = pager_tap(px, py, s_tm_page, tm_pages());
    if (np >= 0) {
        s_tm_page = np;
        return true;
    }
    for (int slot = 0; slot < TM_PER_PAGE; slot++) {
        int i = s_tm_page * TM_PER_PAGE + slot;
        if (i >= s_tm.count) break;
        float x0, y0, x1, y1;
        tm_entry_rect(slot, &x0, &y0, &x1, &y1);
        if (in(px, py, x0, y0, x1, y1)) {
            pm3ds_tm_pick(s_tm.kind, i);
            return true;
        }
    }
    return false;
}

/* ---- Options and Controls pages ------------------------------------------ */
/* their own rail: Settings, Controls, Debug */
#define OPT_Y0 BODY_Y
#define CHEAT_ROW_H 17
/* n64_shim/jukebox.c */
extern const char *pm3ds_jukebox_name(int i);
extern int pm3ds_jukebox_count(void);
extern void pm3ds_sound_changed(void);
extern void pm3ds_jukebox_changed(void);
static const char *const s_fps_names[2] = { "30 fps", "60 fps" };
static const char *const s_text_names[TEXT_SPEEDS] = { "Normal", "Fast", "Instant" };

/* The Settings page: a list in sections, dragged up and down (a scroll bar
 * shows where); a row changes its setting when tapped without dragging. */
enum { OPT_LANG, OPT_SCREEN, OPT_FPS, OPT_3D, OPT_COLORS, OPT_TEXT, OPT_FF_SPEED, OPT_FF_BUTTON, OPT_DROPS, OPT_BEEP, OPT_DPAD,
       OPT_AUTOSAVE, OPT_UPDATE, OPT_HUD, OPT_ICE, OPT_MUSIC, OPT_SFX, OPT_JUKEBOX, OPT_STEER_SPIN, OPT_STEER_JUMP, OPT_UPDATE_NOW, OPT_PACK, OPT_DIM, OPT_COUNT };
#define OPT_HEADER 0x100          /* + section number */
static const int s_opt_list[] = {
    OPT_HEADER + 0, OPT_LANG, OPT_PACK, OPT_SCREEN, OPT_FPS, OPT_3D, OPT_COLORS, OPT_HUD, OPT_ICE, OPT_DIM,
    OPT_HEADER + 1, OPT_TEXT, OPT_FF_SPEED, OPT_FF_BUTTON, OPT_AUTOSAVE, OPT_DROPS, OPT_BEEP, OPT_DPAD, OPT_STEER_SPIN, OPT_STEER_JUMP,
    OPT_HEADER + 2, OPT_MUSIC, OPT_SFX, OPT_JUKEBOX,
    OPT_HEADER + 3, OPT_UPDATE_NOW, OPT_UPDATE,
};
static const char *const s_opt_sections[] = { "Display", "Gameplay", "Sound", "System" };
#define OPT_LIST_N (int)(sizeof s_opt_list / sizeof s_opt_list[0])
#define OPT_ROW_PX 23             /* a setting */
#define OPT_HEAD_PX 17            /* a section title */
#define OPT_LIST_Y1 198           /* the list's bottom, a line above the panel face's end (204) */
#define OPT_LIST_X1 (CX1 - 9)     /* the scroll bar to its right */
static float s_opt_scroll;

static float opt_item_h(int k) { return s_opt_list[k] >= OPT_HEADER ? OPT_HEAD_PX : OPT_ROW_PX; }
static float opt_list_h(void) {
    float h = 0;
    for (int k = 0; k < OPT_LIST_N; k++) h += opt_item_h(k);
    return h;
}
static float opt_scroll_max(void) {
    float m = opt_list_h() + 4 - (OPT_LIST_Y1 - OPT_Y0);   /* + room under the last row */
    return m > 0 ? m : 0;
}

static void opt_value(int id, char *buf, size_t size) {
    extern bool pm3ds_lite_effects;
    const char *v = "";
    switch (id) {
        case OPT_LANG:      v = pm3ds_lang_name(pm3ds_cfg.language); break;
        case OPT_PACK: {    /* GitHub #84: a language pack's name (already UI-encoded, not translated) */
            const char *name = pm3ds_langpack_current_name();
            if (name != NULL) { snprintf(buf, size, "%s", name); return; }
            v = pm3ds_cfg.lang_pack[0] ? "Not found" : "Original";
            break;
        }
        case OPT_SCREEN:    v = s_view_names[pm3ds_cfg.view_mode % 4]; break;
        case OPT_FPS:       v = s_fps_names[pm3ds_cfg.fps60]; break;
        case OPT_3D:        v = pm3ds_lite_effects ? "Off (Old 3DS)" : pm3ds_depth3d_names[pm3ds_cfg.depth3d % DEPTH3D_LEVELS]; break;
        case OPT_TEXT:      v = s_text_names[pm3ds_cfg.text_speed % TEXT_SPEEDS]; break;
        case OPT_FF_SPEED:  snprintf(buf, size, "%dx", pm3ds_cfg.ff_speed); return;
        case OPT_FF_BUTTON: v = pm3ds_cfg.ff_toggle ? "Toggle" : "Hold"; break;
        case OPT_DROPS:     v = pm3ds_cfg.stat_drops ? "On" : "Off"; break;
        case OPT_BEEP:      v = pm3ds_cfg.hp_beep ? "On" : "Off"; break;
        case OPT_HUD:       v = pm3ds_cfg.battle_hud ? "On" : "Off"; break;
        case OPT_ICE:       v = pm3ds_cfg.ice_fx ? "On" : "Off"; break;
        case OPT_DIM:       v = pm3ds_cfg.bottom_dim ? "Off when idle" : "Always on"; break;
        case OPT_DPAD:      v = pm3ds_cfg.dpad_menus ? "Cursor" : "C buttons"; break;
        case OPT_MUSIC:     v = pm3ds_cfg.music_on ? "On" : "Off"; break;
        case OPT_SFX:       v = pm3ds_cfg.sfx_on ? "On" : "Off"; break;
        case OPT_JUKEBOX:   v = pm3ds_jukebox_name(pm3ds_cfg.jukebox); break;
        case OPT_STEER_SPIN: v = pm3ds_cfg.steer_spin ? "On" : "Off"; break;
        case OPT_STEER_JUMP: v = pm3ds_cfg.steer_jump ? "On" : "Off"; break;
        case OPT_AUTOSAVE: {
            static const char *const names[PM3DS_AUTOSAVE_COUNT] = { "Off", "Each area", "5 min", "10 min", "15 min" };
            v = names[pm3ds_cfg.autosave % PM3DS_AUTOSAVE_COUNT];
            break;
        }
        case OPT_COLORS: {
            static const char *const names[PM3DS_COLOR_COUNT] = { "Original", "Vivid", "Deuteranopia", "Protanopia", "Tritanopia" };
            v = names[pm3ds_cfg.color_mode % PM3DS_COLOR_COUNT];
            break;
        }
        case OPT_UPDATE:    v = pm3ds_cfg.update_check ? "On" : "Off"; break;
        case OPT_UPDATE_NOW:
            switch (pm3ds_update_state()) {
                case PM3DS_UPD_RUNNING: v = "Checking..."; break;
                case PM3DS_UPD_CURRENT: v = "Up to date"; break;
                case PM3DS_UPD_NO_WIFI: v = "No Wi-Fi"; break;
                case PM3DS_UPD_FAILED:  v = "No answer"; break;
                case PM3DS_UPD_NEWER: {
                    int f = pm3ds_update_found();
                    snprintf(buf, size, "v%d.%d.%d", f / 10000, f / 100 % 100, f % 100);
                    return;
                }
                default: v = "Check"; break;
            }
            break;
    }
    snprintf(buf, size, "%s", T(v));
}

static const char *const s_opt_labels[OPT_COUNT] = {
    "Language", "Screen", "Frame rate", "3D depth", "Colors", "Text speed", "Fast-forward speed", "Fast-forward button",
    "Heart & flower drops", "Low HP beep", "D-pad in menus", "Autosave", "Check at launch",
    "Battle HP/FP bar", "Ice Power effect", "Music", "Sound effects", "Jukebox", "Steerable spin", "Steerable jump",
    "Check for updates", "Game text", "Bottom screen",
};

/* Quit game (in game, GitHub #67): back to the title, saving first or not.
 * Saving goes through src/world/menus.c (pm3ds_touch_req 5, the quick save's
 * checks); quitting without saving is the game's soft reset, which the N64
 * reset button started at any moment. */
volatile int pm3ds_quit_result;   /* menus.c: 1 saved and quitting, -1 can't save now */
static int s_quit;                /* 0 closed, 1 asking, 2 waiting for the save, 3 can't save now */
static int s_quit_timer;
#define QUIT_X0 74
#define QUIT_X1 300
#define QUIT_ROW_Y(i) (96 + (i) * 30)

static void quit_now(void) {
    extern void pm3ds_quit_to_title(void);
    pm3ds_quit_to_title();
    s_quit = 0;
    s_tab = TAB_STATS;
}

static void draw_quit(void) {
    static const char *const rows[3] = { "Save and quit", "Quit without saving", "Cancel" };
    panel(QUIT_X0 - 6, 52, QUIT_X1 + 6, 196, COL_PANEL);
    text_center((QUIT_X0 + QUIT_X1) / 2, 62, "Quit to the title screen?", 0.9f, 0, COL_WHITE);
    for (int i = 0; i < 3; i++) {
        u32 face = i == 0 && s_quit == 2 ? RGBA(255, 196, 64, 255) : i == 2 ? COL_OFF : COL_SEL;
        chip(QUIT_X0 + 20, QUIT_ROW_Y(i), QUIT_X1 - 20, QUIT_ROW_Y(i) + 22, rows[i], face, 0.8f);
    }
    if (s_quit == 3) text_center((QUIT_X0 + QUIT_X1) / 2, 182, "Can't save right now.", 0.62f, 0, COL_HINT);
}

static void quit_tap(int px, int py) {
    for (int i = 0; i < 3; i++) {
        if (!in(px, py, QUIT_X0 + 16, QUIT_ROW_Y(i) - 3, QUIT_X1 - 16, QUIT_ROW_Y(i) + 25)) continue;
        if (i == 0 && s_quit != 2) {
            pm3ds_quit_result = 0;
            pm3ds_touch_req = 5;
            s_quit = 2;
            s_quit_timer = 0;
        } else if (i == 1) {
            quit_now();
        } else if (i == 2) {
            s_quit = 0;
        }
        return;
    }
}

static void quit_tick(void) {
    if (s_quit != 2) return;
    if (pm3ds_quit_result == 0 && ++s_quit_timer > 45) {   /* the world menus didn't run: a battle, a cutscene */
        if (pm3ds_touch_req == 5) pm3ds_touch_req = 0;
        pm3ds_quit_result = -1;
    }
    if (pm3ds_quit_result > 0) {
        s_quit = 0;
        s_tab = TAB_STATS;
    } else if (pm3ds_quit_result < 0) {
        s_quit = 3;
    }
}

/* Jukebox (GitHub #78): a song instead of the area's, from Settings > Sound */
static bool s_juke;
static int s_juke_pick;
#define JUKE_ROW_Y (OPT_Y0 + 34)
static void draw_jukebox(void) {
    const char *n = pm3ds_jukebox_name(s_juke_pick);
    page_title("Jukebox", "Plays instead of the area's music");
    tile(CX0, JUKE_ROW_Y, CX1, JUKE_ROW_Y + 30, 6, COL_TAB_ON);
    text(CX0 + 10, JUKE_ROW_Y + 5, "<", 1.1f, 0, COL_GOLD);
    text_center((CX0 + CX1) / 2, JUKE_ROW_Y + 7, n, fit(n, CX1 - CX0 - 60, 0.95f, 0.5f), 0, COL_WHITE);
    text(CX1 - 20, JUKE_ROW_Y + 5, ">", 1.1f, 0, COL_GOLD);
    chip(CX0 + 8, JUKE_ROW_Y + 48, (CX0 + CX1) / 2 - 6, JUKE_ROW_Y + 72, "Play", COL_GOLD, 0.9f);
    chip((CX0 + CX1) / 2 + 6, JUKE_ROW_Y + 48, CX1 - 8, JUKE_ROW_Y + 72, "Off", pm3ds_cfg.jukebox >= 0 ? COL_SEL : COL_OFF, 0.9f);
    if (pm3ds_cfg.jukebox >= 0) {
        const char *cur = pm3ds_jukebox_name(pm3ds_cfg.jukebox);
        text_center((CX0 + CX1) / 2, JUKE_ROW_Y + 84, cur, fit(cur, CX1 - CX0 - 20, 0.72f, 0.5f), 0, COL_HINT);
    }
    button(s_btn_ff, "Back");
}

static void jukebox_set(int song) {
    pm3ds_cfg.jukebox = song;
    pm3ds_settings_save();
    pm3ds_jukebox_changed();
}

static void jukebox_tap(int px, int py) {
    int n = pm3ds_jukebox_count();
    if (py >= JUKE_ROW_Y && py < JUKE_ROW_Y + 30) {
        if (px < (CX0 + CX1) / 2) s_juke_pick = (s_juke_pick + n - 1) % n;
        else s_juke_pick = (s_juke_pick + 1) % n;
    } else if (py >= JUKE_ROW_Y + 44 && py < JUKE_ROW_Y + 76) {
        if (px < (CX0 + CX1) / 2) jukebox_set(s_juke_pick);
        else jukebox_set(-1);
    }
}

static void draw_options(void) {
    char buf[48];
    if (s_juke) {
        draw_jukebox();
        return;
    }
    float max = opt_scroll_max();
    if (s_opt_scroll > max) s_opt_scroll = max;
    if (s_opt_scroll < 0) s_opt_scroll = 0;
    float y = OPT_Y0 - s_opt_scroll;
    /* rows cut at the list's edges (rotated framebuffer: x = 240 - screen y) */
    flush();
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 240 - OPT_LIST_Y1, 0, 240 - (OPT_Y0 - 2), 320);
    for (int k = 0; k < OPT_LIST_N; k++) {
        float h = opt_item_h(k);
        if (y + h > OPT_Y0 - 4 && y < OPT_LIST_Y1 + 4) {
            int id = s_opt_list[k];
            if (id >= OPT_HEADER) {   /* a section: its name and a line */
                const char *t = s_opt_sections[id - OPT_HEADER];
                float x1 = text(CX0 + 3, y + 2, t, 0.72f, 0, COL_HINT);
                fill(x1 + 6, y + 9, OPT_LIST_X1, y + 10, COL_PANEL_EDGE);
            } else {
                tile(CX0, y, OPT_LIST_X1, y + OPT_ROW_PX - 3, 6, COL_TAB_ON);
                const char *label = s_opt_labels[id];
                text(CX0 + 7, y + 2, label, fit(label, 138, 0.85f, 0.6f), 0, COL_WHITE);
                opt_value(id, buf, sizeof buf);
                chip(206, y + 2, OPT_LIST_X1 - 4, y + OPT_ROW_PX - 5, buf, COL_SEL, fit(buf, 76, 0.78f, 0.5f));
            }
        }
        y += h;
    }
    flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    /* what scrolled past the list's edges: the panel face over it again, then the title */
    fill(CX0 - 4, PANEL_Y0 + 3, CX1 + 4, OPT_Y0 - 2, COL_PANEL);
    fill(CX0 - 4, OPT_LIST_Y1, CX1 + 4, PANEL_Y1 - 2, COL_PANEL);
    if (s_opt_scroll < max)   /* more below: a line where the list is cut */
        fill(CX0, OPT_LIST_Y1 + 1, OPT_LIST_X1, OPT_LIST_Y1 + 2, COL_PANEL_EDGE);
    page_title("Settings", "Drag to scroll, tap to change");
    if (max > 0) {   /* the scroll bar */
        float track = OPT_LIST_Y1 - OPT_Y0, view = track / (track + max);
        float th = track * view, ty = OPT_Y0 + (track - th) * (s_opt_scroll / max);
        round_rect(CX1 - 5, OPT_Y0, CX1 - 1, OPT_LIST_Y1, 2, COL_PANEL_EDGE);
        round_rect(CX1 - 5, ty, CX1 - 1, ty + th, 2, COL_SEL);
    }
    if (s_info.in_game) button(s_btn_opt, "Quit game");
    else small_text(12, 219, PM3DS_APP_NAME " v" PM3DS_VERSION_STR, 40, COL_PANEL_EDGE);
    button(s_btn_ff, "Back");
    if (s_quit) draw_quit();
}

/* a tap on the list (released without dragging): the setting at y */
static void options_tap(int px, int py) {
    if (px < CX0 - 4 || px >= OPT_LIST_X1 || py < OPT_Y0 || py >= OPT_LIST_Y1) return;
    float y = OPT_Y0 - s_opt_scroll;
    for (int k = 0; k < OPT_LIST_N; k++) {
        float h = opt_item_h(k);
        if (py >= y && py < y + h) {
            int id = s_opt_list[k];
            if (id >= OPT_HEADER) return;
            switch (id) {
                case OPT_LANG:
                    pm3ds_cfg.language = (pm3ds_cfg.language + 1) % PM3DS_LANG_COUNT;
                    pm3ds_lang_set(pm3ds_cfg.language);
                    break;
                case OPT_PACK: {   /* the ROM's text -> each pack on the SD card -> the ROM's */
                    int n = pm3ds_langpack_scan(), cur = -1;
                    for (int i = 0; i < n; i++)
                        if (!strcmp(pm3ds_langpack_id(i), pm3ds_cfg.lang_pack)) cur = i;
                    snprintf(pm3ds_cfg.lang_pack, sizeof pm3ds_cfg.lang_pack, "%s", cur + 1 < n ? pm3ds_langpack_id(cur + 1) : "");
                    pm3ds_langpack_request(pm3ds_cfg.lang_pack);
                    break;
                }
                case OPT_SCREEN: pm3ds_cfg.view_mode = (pm3ds_cfg.view_mode + 1) % 4; break;   /* Original -> Fill -> Wide -> Sharp wide */
                case OPT_FPS: pm3ds_cfg.fps60 = !pm3ds_cfg.fps60; break;
                case OPT_3D: {
                    extern bool pm3ds_lite_effects;
                    if (!pm3ds_lite_effects) pm3ds_cfg.depth3d = (pm3ds_cfg.depth3d + 1) % DEPTH3D_LEVELS;
                    break;
                }
                case OPT_TEXT: pm3ds_cfg.text_speed = (pm3ds_cfg.text_speed + 1) % TEXT_SPEEDS; break;
                case OPT_FF_SPEED: pm3ds_cfg.ff_speed = pm3ds_cfg.ff_speed >= 4 ? 2 : pm3ds_cfg.ff_speed + 1; break;
                case OPT_FF_BUTTON: pm3ds_cfg.ff_toggle = !pm3ds_cfg.ff_toggle; break;
                case OPT_DROPS: pm3ds_cfg.stat_drops = !pm3ds_cfg.stat_drops; break;
                case OPT_BEEP: pm3ds_cfg.hp_beep = !pm3ds_cfg.hp_beep; break;
                case OPT_HUD: pm3ds_cfg.battle_hud = !pm3ds_cfg.battle_hud; break;
                case OPT_ICE: pm3ds_cfg.ice_fx = !pm3ds_cfg.ice_fx; break;
                case OPT_DIM: pm3ds_cfg.bottom_dim = !pm3ds_cfg.bottom_dim; break;
                case OPT_DPAD: pm3ds_cfg.dpad_menus = !pm3ds_cfg.dpad_menus; break;
                case OPT_MUSIC: pm3ds_cfg.music_on = !pm3ds_cfg.music_on; pm3ds_sound_changed(); break;
                case OPT_SFX: pm3ds_cfg.sfx_on = !pm3ds_cfg.sfx_on; pm3ds_sound_changed(); break;
                case OPT_JUKEBOX:
                    s_juke = true;
                    if (pm3ds_cfg.jukebox >= 0) s_juke_pick = pm3ds_cfg.jukebox;
                    return;
                case OPT_STEER_SPIN: pm3ds_cfg.steer_spin = !pm3ds_cfg.steer_spin; break;
                case OPT_STEER_JUMP: pm3ds_cfg.steer_jump = !pm3ds_cfg.steer_jump; break;
                case OPT_AUTOSAVE: pm3ds_cfg.autosave = (uint8_t)((pm3ds_cfg.autosave + 1) % PM3DS_AUTOSAVE_COUNT); break;
                case OPT_COLORS:
                    pm3ds_cfg.color_mode = (uint8_t)((pm3ds_cfg.color_mode + 1) % PM3DS_COLOR_COUNT);
                    pm3ds_color_mode_changed();
                    break;
                case OPT_UPDATE: pm3ds_cfg.update_check = !pm3ds_cfg.update_check; break;   /* from the next launch */
                case OPT_UPDATE_NOW: pm3ds_update_check_now(); return;   /* GitHub #82 */
            }
            pm3ds_settings_save();
            return;
        }
        y += h;
    }
}

/* every 3DS button and what it does; tap to change */
#define CTL_ROWS 9
#define CTL_Y0 32
#define CTL_ROW_H 19
static void draw_controls(void) {
    page_title("Controls", NULL);
    text_right(CX1, HEAD_Y + 3, "Tap an action to change it", 0.62f, COL_HINT);
    for (int k = 0; k < KEY_IDX_COUNT; k++) {
        float x = k < CTL_ROWS ? CX0 : CX0 + 126, y = CTL_Y0 + (k % CTL_ROWS) * CTL_ROW_H;
        tile(x, y, x + 122, y + CTL_ROW_H - 2, 6, COL_TAB_ON);
        const char *key = pm3ds_settings_key_name(k), *act = pm3ds_settings_action_name(pm3ds_cfg.keymap[k]);
        round_rect(x + 3, y + 2, x + 47, y + CTL_ROW_H - 4, 6, RGBA(90, 70, 60, 255));
        text_center(x + 25, y + 2, key, fit(key, 40, 0.7f, 0.45f), 1, COL_WHITE);
        text(x + 52, y + 2, act, fit(act, 66, 0.75f, 0.5f), 0, COL_WHITE);
    }
    button(s_btn_opt, "Defaults");
    button(s_btn_ff, "Back");
}

/* a save's cheats, each switched on its own (n64_shim/cheats.c) */
static const char *const s_cheat_names[PM3DS_CHEAT_COUNT] = {
    "Infinite HP", "Infinite FP", "Infinite Star Power", "999 coins", "Unlimited BP", "No level-up limits", "Free QoL badges", "Levels past 27", "Action commands",
};
static const char *const s_cheat_help[PM3DS_CHEAT_COUNT] = {
    "Stays full", "Stays full", "Stays full", "Stays at 999", "Any badges fit", "Up to 95 HP, FP, 99 BP", "Speedy Spin, I Spy...", "To 99, 1+ SP a fight", "From now on",
};

/* Cheats' second page, "Start with" from the Discord: extras for this file, whenever */
static bool s_cheat_give;
#define GIVE_ROW_H 20
static void draw_give(void) {
    static const char *const names[6] = { "Max HP +5", "Max FP +5", "Max BP +3", "Level +1", "Boots & hammer +1", "Star Pieces +10" };
    page_title("Give", "Tap to add to this file");
    for (int i = 0; i < PM3DS_GIVE_COUNT; i++) {
        float y = OPT_Y0 + i * GIVE_ROW_H;
        tile(CX0, y, CX1, y + GIVE_ROW_H - 3, 6, COL_TAB_ON);
        if (i < 6) {
            text(CX0 + 7, y + 1, names[i], fit(names[i], 180, 0.82f, 0.6f), 0, COL_WHITE);
        } else {
            const char *n = i == PM3DS_GIVE_ITEM ? pm3ds_cheat_give_item() : pm3ds_cheat_give_badge();
            text(CX0 + 7, y + 1, "<", 0.82f, 0, COL_GOLD);
            text(CX0 + 26, y + 1, n, fit(n, 150, 0.82f, 0.5f), 0, COL_WHITE);
            text(CX0 + 186, y + 1, ">", 0.82f, 0, COL_GOLD);
        }
        chip(266, y + 2, CX1 - 4, y + GIVE_ROW_H - 5, "Give", COL_GOLD, 0.75f);
    }
    button(s_btn_opt, "Switches");
    button(s_btn_ff, "Back");
}

static void draw_cheats(void) {
    if (s_cheat_give) {
        draw_give();
        return;
    }
    page_title("Cheats", "Tap one to switch it");
    for (int i = 0; i < PM3DS_CHEAT_COUNT; i++) {
        float y = OPT_Y0 + i * CHEAT_ROW_H;
        /* a Level 99 save has these two whatever the switch says */
        bool on = pm3ds_cheat_on(i) || (pm3ds_level99() && (i == PM3DS_CHEAT_LEVELS || i == PM3DS_CHEAT_NOCAP));
        tile(CX0, y, CX1, y + CHEAT_ROW_H - 3, 6, on ? COL_SEL : COL_TAB_ON);
        text_in(CX0 + 7, y, s_cheat_names[i], 112, 0.72f, 0.5f, 0, COL_WHITE);
        text_in(CX0 + 124, y + 1, s_cheat_help[i], 270 - 4 - (CX0 + 124), 0.58f, 0.45f, 0, COL_HINT);   /* up to the switch */
        chip(270, y + 1, CX1 - 4, y + CHEAT_ROW_H - 4, on ? "On" : "Off", on ? COL_GOLD : COL_OFF, 0.6f);
    }
    button(s_btn_opt, "Give");
    button(s_btn_ff, "Back");
}

static void button(const float x[2], const char *label) {
    round_rect(x[0] + 2, BTN_Y0 + 2, x[1] + 2, BTN_Y1 + 2, 10, COL_SHADOW);
    round_rect(x[0], BTN_Y0, x[1], BTN_Y1, 10, COL_PANEL_EDGE);
    round_rect(x[0] + 2, BTN_Y0 + 2, x[1] - 2, BTN_Y1 - 2, 8, COL_TAB_ON);
    text_center((x[0] + x[1]) / 2, BTN_Y0 + 3, label, fit(label, x[1] - x[0] - 8, 1.0f, 0.6f), 0, COL_WHITE);
}

/* the middle button: Items in the world; with a menu open it holds B (Back),
 * or Z (Swap Mario and partner) on the battle command reel */
static const char *mid_button(uint16_t *pad) {
    *pad = 0;
    if (tm_open()) {
        if (s_tm.kind == PM3DS_TM_WHEEL) { *pad = 0x2000; return "Swap"; }
        *pad = 0x4000;
        return "Back";
    }
    if (s_info.in_battle) return NULL;
    return s_info.in_game ? "Items" : NULL;
}

static void draw_buttons(void) {
    uint16_t mid_pad;
    const char *mid = mid_button(&mid_pad);
    button(s_btn_opt, "Options");
    if (mid && !mid_pad && s_info.in_game) {     /* the world: Items and Badges */
        button(s_btn_items_half, mid);
        button(s_btn_badges, "Badges");
    } else if (mid) {
        button(s_btn_items, mid);
    }
    if (pm3ds_ff_button_on) {   /* lit while on */
        round_rect(s_btn_ff[0], BTN_Y0, s_btn_ff[1], BTN_Y1, 10, COL_PANEL_EDGE);
        round_rect(s_btn_ff[0] + 2, BTN_Y0 + 2, s_btn_ff[1] - 2, BTN_Y1 - 2, 8, RGBA(255, 196, 64, 255));
        text_center((s_btn_ff[0] + s_btn_ff[1]) / 2, BTN_Y0 + 3, "Fast >> on", 1.0f, 0, COL_WHITE);
    } else {
        button(s_btn_ff, "Fast >>");
    }
}

/* ---- the title and the file menu by touch (Discord suggestion) ------------- */
#define TITLE_CARD_Y0 60
#define TITLE_CARD_Y1 170
#define TITLE_LANG_Y0 178
#define TITLE_LANG_Y1 204
static int s_title_start;   /* steps left of the START a tap on the title's card holds */

#define FM_TILE_X(c) (12 + (c) * 150)
#define FM_TILE_Y(r) (34 + (r) * 54)
#define FM_TILE_W 146
#define FM_TILE_H 50
#define FM_ROW_Y 144
#define FM_ROW_H 26
#define FM_KEY_X0 10
#define FM_KEY_W 23
#define FM_KEY_Y0 42
#define FM_KEY_H 22

static void fm_file_tile(const pm3ds_fm_info_t *q, int i, float x, float y, float w, float h, bool lit) {
    char line[24];
    tile(x, y, x + w, y + h, 8, lit ? COL_SEL : q->has[i] ? COL_TAB_ON : COL_OFF);
    snprintf(line, sizeof line, "%d", i + 1);
    text(x + 7, y + 4, line, 0.9f, 0, COL_HINT);
    if (q->has[i]) {
        text(x + 22, y + 4, q->name[i], fit(q->name[i], w - 28, 0.9f, 0.6f), 0, COL_WHITE);
        snprintf(line, sizeof line, "Lv %d   %d/7", q->level[i], q->spirits[i]);
        text(x + 22, y + 26, line, 0.72f, 0, COL_HINT);
    } else {
        text(x + 22, y + 4, "New file", fit("New file", w - 28, 0.9f, 0.6f), 0, COL_HINT);
    }
}

static void draw_file_menu(void) {
    pm3ds_fm_info_t q;
    pm3ds_fm_query(&q);
    panel(4, 4, 316, 206, COL_PANEL);
    switch (q.menu) {
        case PM3DS_FM_MAIN: {
            static const char *const titles[4] = { "Choose a file", "Choose a file to delete", "Copy which file?", "Copy to which file?" };
            text_center(160, 11, titles[q.state], 0.95f, 0, COL_WHITE);
            for (int i = 0; i < 4; i++)
                fm_file_tile(&q, i, FM_TILE_X(i % 2), FM_TILE_Y(i / 2), FM_TILE_W, FM_TILE_H, q.selected == i);
            if (q.state == PM3DS_FMS_FILE) {
                chip(12, FM_ROW_Y, 102, FM_ROW_Y + FM_ROW_H, "Delete", COL_OFF, 0.85f);
                chip(115, FM_ROW_Y, 205, FM_ROW_Y + FM_ROW_H, "Copy", COL_SEL, 0.85f);
                chip(218, FM_ROW_Y, 308, FM_ROW_Y + FM_ROW_H, "Back", COL_SEL, 0.85f);
            } else {
                chip(100, FM_ROW_Y, 220, FM_ROW_Y + FM_ROW_H, "Cancel", COL_SEL, 0.85f);
            }
            break;
        }
        case PM3DS_FM_CONFIRM: {
            static const char *const titles[4] = { "Start this file?", "Delete this file?", "Use this name?", "Copy over this file?" };
            text_center(160, 11, titles[q.state], 0.95f, 0, COL_WHITE);
            if (q.state == PM3DS_FMC_CREATE) text_center(160, 56, q.typed, 1.3f, 0, COL_WHITE);
            else if (q.selected >= 0) fm_file_tile(&q, q.selected, 87, 40, FM_TILE_W, FM_TILE_H, true);
            chip(40, 112, 150, 148, "Yes", COL_SEL, 1.0f);
            chip(170, 112, 280, 148, "No", COL_OFF, 1.0f);
            break;
        }
        case PM3DS_FM_MESSAGE: {
            static const char *const titles[3] = { "File deleted", "File copied", "File created" };
            text_center(160, 70, titles[q.state], 1.1f, 0, COL_WHITE);
            chip(105, 112, 215, 148, "OK", COL_SEL, 1.0f);
            break;
        }
        case PM3DS_FM_INPUT: {
            char label[8];
            text_center(160, 9, "Enter a file name!", 0.8f, 0, COL_HINT);
            text_center(160, 22, q.typed[0] ? q.typed : "-", 1.0f, 0, COL_WHITE);
            int shown = 0;
            for (int row = 0; row < q.rows; row++) {
                bool any = false;
                for (int col = 0; col < q.cols; col++) {
                    int kind = pm3ds_fm_key(col, row, label);
                    if (!kind) continue;
                    float x = FM_KEY_X0 + col * FM_KEY_W, y = FM_KEY_Y0 + shown * FM_KEY_H;
                    float w = kind == PM3DS_FM_CELL_WIDE ? FM_KEY_W * 3 : FM_KEY_W;
                    chip(x, y, x + w - 2, y + FM_KEY_H - 2, label, kind == PM3DS_FM_CELL_WIDE ? COL_SEL : COL_TAB_ON, 0.72f);
                    any = true;
                }
                if (any) shown++;
            }
            break;
        }
        case PM3DS_FM_LANGUAGE:
            text_center(160, 11, "Language", 0.95f, 0, COL_WHITE);
            for (int i = 0; i < 4; i++)
                chip(70, 40 + i * 36, 250, 70 + i * 36, pm3ds_lang_name(i), q.selected == i ? COL_SEL : COL_TAB_ON, 0.9f);
            break;
    }
}

static void fm_page_tap(int px, int py) {
    pm3ds_fm_info_t q;
    pm3ds_fm_query(&q);
    switch (q.menu) {
        case PM3DS_FM_MAIN:
            for (int i = 0; i < 4; i++) {
                if (in(px, py, FM_TILE_X(i % 2), FM_TILE_Y(i / 2), FM_TILE_X(i % 2) + FM_TILE_W, FM_TILE_Y(i / 2) + FM_TILE_H)) {
                    pm3ds_fm_tap_main(i % 2 ? 2 : 0, i / 2);   /* the menu's grid: files at columns 0 and 2 */
                    return;
                }
            }
            if (py < FM_ROW_Y || py >= FM_ROW_Y + FM_ROW_H) return;
            if (q.state != PM3DS_FMS_FILE) {
                if (in(px, py, 100, FM_ROW_Y, 220, FM_ROW_Y + FM_ROW_H)) pm3ds_fm_tap_main(1, 2);
            } else if (px >= 12 && px < 102) {
                pm3ds_fm_tap_main(0, 2);   /* Delete */
            } else if (px >= 115 && px < 205) {
                pm3ds_fm_tap_main(1, 2);   /* Copy */
            } else if (px >= 218 && px < 308) {
                pm3ds_fm_tap_main(2, 2);   /* the title */
            }
            break;
        case PM3DS_FM_CONFIRM:
            if (in(px, py, 40, 112, 150, 148)) pm3ds_fm_tap_yesno(true);
            else if (in(px, py, 170, 112, 280, 148)) pm3ds_fm_tap_yesno(false);
            break;
        case PM3DS_FM_MESSAGE:
            if (in(px, py, 105, 112, 215, 148)) pm3ds_fm_tap_ok();
            break;
        case PM3DS_FM_INPUT: {
            char label[8];
            int shown = 0;
            for (int row = 0; row < q.rows; row++) {
                bool any = false;
                for (int col = 0; col < q.cols; col++) {
                    int kind = pm3ds_fm_key(col, row, label);
                    if (!kind) continue;
                    float x = FM_KEY_X0 + col * FM_KEY_W, y = FM_KEY_Y0 + shown * FM_KEY_H;
                    float w = kind == PM3DS_FM_CELL_WIDE ? FM_KEY_W * 3 : FM_KEY_W;
                    if (in(px, py, x, y, x + w, y + FM_KEY_H)) {
                        pm3ds_fm_tap_key(col, row);
                        return;
                    }
                    any = true;
                }
                if (any) shown++;
            }
            break;
        }
        case PM3DS_FM_LANGUAGE:
            for (int i = 0; i < 4; i++)
                if (in(px, py, 70, 40 + i * 36, 250, 70 + i * 36)) pm3ds_fm_tap_language(i);
            break;
    }
}

/* ---- new-game picker ------------------------------------------------------ */
/* The file menu (src/filemenu/filemenu_main.c) opens it when an empty file is
 * picked: Classic or Randomizer (and whether the save allows cheats), then
 * what the randomizer shuffles and the seed. Drawn on both screens (the top one over the file menu); touch on the
 * bottom one, +Control Pad / A / B / Y / START on either. */
enum { NG_CLOSED, NG_MODE, NG_RANDO };
enum { NG_SND_NONE, NG_SND_MOVE, NG_SND_NEXT, NG_SND_BACK, NG_SND_TOGGLE };   /* for the file menu to play */
#define NG_TOGGLES 7
#define NG_ROW_SEED NG_TOGGLES
#define NG_ROW_START (NG_TOGGLES + 1)
#define NG_ROW_Y0 44
#define NG_ROW_H 16
static const struct { unsigned flag; const char *label, *help; } s_ng_toggles[NG_TOGGLES] = {
    { PM3DS_RANDO_COINS, "Coins & star pieces", "Coins and star pieces lying around are shuffled too." },
    { PM3DS_RANDO_KEYS, "Key items", "Key items move too, always somewhere you can reach before you need them." },
    { PM3DS_RANDO_SHOPS, "Shops", "Shops sell items from the world, and the world holds the shops' stock." },
    { PM3DS_RANDO_GIFTS, "Gifts", "What characters give you is shuffled too." },
    { PM3DS_RANDO_HIDDEN, "Keys in hidden spots", "Key items can also hide in hidden panels and blocks." },
    { PM3DS_RANDO_ENEMIES, "Enemies", "Every enemy group fights as another group from the same chapter." },
    { PM3DS_RANDO_BADGES, "Badge costs", "Badges swap their BP and FP costs. Speedy Spin stays." },
};
static int s_ng_page, s_ng_cursor, s_ng_result, s_ng_sound;
static unsigned s_ng_flags, s_ng_seed;
static bool s_ng_fresh;   /* just opened: the press that opened it is still down */
static bool s_ng_hold;    /* closed, but the game sees no buttons until they are let go */
static bool s_ng_kbd;     /* type a seed: the keyboard opens from the tick, outside a frame */
static bool s_ng_cheats;  /* the new save allows cheats (n64_shim/cheats.c) */
static bool s_ng_hard;    /* the new save is in hard mode (n64_shim/cheats.c) */
static bool s_ng_skip;    /* the new save skips the intro (n64_shim/cheats.c) */
static bool s_ng_l99;     /* the new save goes up to level 99 (n64_shim/cheats.c) */
/* the two cards, then the new save's four switches in two rows under them */
#define NG_CARD_Y1 136
#define NG_SW_Y0 143
#define NG_SW_H 22
#define NG_SW_GAP 3

void pm3ds_new_game_picker_open(void) {
    s_ng_page = NG_MODE;
    s_ng_cursor = (pm3ds_cfg.rando_flags & PM3DS_RANDO_ON) ? 1 : 0;
    s_ng_flags = pm3ds_cfg.rando_flags & ~PM3DS_RANDO_ON;
    s_ng_seed = 0;
    s_ng_result = 0;
    s_ng_fresh = true;
    s_ng_cheats = false;
    s_ng_hard = false;
    s_ng_skip = false;
    s_ng_l99 = false;
}

/* src/filemenu/filemenu_main.c, each frame: -1 while open, 1 once a game
 * mode is chosen, else 0; *sound: NG_SND_* to play */
int pm3ds_new_game_picker(int *sound) {
    *sound = s_ng_sound;
    s_ng_sound = NG_SND_NONE;
    if (s_ng_page != NG_CLOSED) return -1;
    int r = s_ng_result;
    s_ng_result = 0;
    return r;
}

/* n64_shim/os_cont.c: the game's buttons are the picker's for now */
bool pm3ds_new_game_picker_blocks_input(void) { return s_ng_page != NG_CLOSED || s_ng_hold; }

static void ng_close(int result, int sound) {
    s_ng_page = NG_CLOSED;
    s_ng_result = result;
    s_ng_sound = sound;
    s_ng_hold = true;
}

static void ng_start(bool rando) {
    unsigned flags = rando ? (s_ng_flags | PM3DS_RANDO_ON) : 0;
    pm3ds_rando_choose(flags, s_ng_seed);
    pm3ds_cheats_choose(s_ng_cheats);
    pm3ds_hard_choose(s_ng_hard);
    pm3ds_skip_intro_choose(s_ng_skip);
    pm3ds_level99_choose(s_ng_l99);
    pm3ds_cfg.rando_flags = s_ng_flags | (rando ? PM3DS_RANDO_ON : 0);
    pm3ds_settings_save();
    ng_close(1, NG_SND_NEXT);
}

static SwkbdCallbackResult ng_seed_filter(void *user, const char **msg, const char *text, size_t len) {
    (void)user;
    for (size_t i = 0; i < len; i++) {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
              (text[i] >= 'A' && text[i] <= 'F'))) {
            *msg = "A seed is up to 8 characters: 0-9 and A-F.";
            return SWKBD_CALLBACK_CONTINUE;
        }
    }
    return SWKBD_CALLBACK_OK;
}

static void ng_keyboard(void) {
    SwkbdState kb;
    char buf[16] = "";
    if (s_ng_seed) snprintf(buf, sizeof buf, "%08X", s_ng_seed);
    swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, 8);
    swkbdSetHintText(&kb, "Seed (0-9, A-F)");
    swkbdSetInitialText(&kb, buf);
    swkbdSetValidation(&kb, SWKBD_NOTBLANK_NOTEMPTY, 0, 0);
    swkbdSetFilterCallback(&kb, ng_seed_filter, NULL);
    if (swkbdInputText(&kb, buf, sizeof buf) == SWKBD_BUTTON_CONFIRM) {
        s_ng_seed = (unsigned)strtoul(buf, NULL, 16);
        s_ng_sound = NG_SND_NEXT;
    }
}

static void ng_rando_activate(int row) {
    if (row < NG_TOGGLES) {
        s_ng_flags ^= s_ng_toggles[row].flag;
        s_ng_sound = NG_SND_TOGGLE;
    } else if (row == NG_ROW_SEED) {
        s_ng_kbd = true;
    } else {
        ng_start(true);
    }
}

static void ng_buttons(void) {
    static u32 prev;
    u32 held = hidKeysHeld() & ~KEY_TOUCH;
    u32 kd = held & ~prev;
    prev = held;
    if (s_ng_hold && held == 0) s_ng_hold = false;
    if (s_ng_page == NG_CLOSED) return;
    if (s_ng_fresh) {
        s_ng_fresh = false;
        return;
    }
    if (s_ng_page == NG_MODE) {
        if (kd & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) {
            s_ng_cursor ^= 1;
            s_ng_sound = NG_SND_MOVE;
        } else if (kd & (KEY_A | KEY_START)) {
            if (s_ng_cursor == 0) {
                ng_start(false);
            } else {
                s_ng_page = NG_RANDO;
                s_ng_cursor = NG_ROW_START;
                s_ng_sound = NG_SND_NEXT;
            }
        } else if (kd & KEY_Y) {
            s_ng_cheats = !s_ng_cheats;
            s_ng_sound = NG_SND_TOGGLE;
        } else if (kd & KEY_X) {
            s_ng_hard = !s_ng_hard;
            s_ng_sound = NG_SND_TOGGLE;
        } else if (kd & KEY_R) {
            s_ng_skip = !s_ng_skip;
            s_ng_sound = NG_SND_TOGGLE;
        } else if (kd & KEY_L) {
            s_ng_l99 = !s_ng_l99;
            s_ng_sound = NG_SND_TOGGLE;
        } else if (kd & KEY_B) {
            ng_close(0, NG_SND_BACK);
        }
        return;
    }
    if (kd & KEY_UP) {
        s_ng_cursor = (s_ng_cursor + NG_ROW_START) % (NG_ROW_START + 1);
        s_ng_sound = NG_SND_MOVE;
    } else if (kd & KEY_DOWN) {
        s_ng_cursor = (s_ng_cursor + 1) % (NG_ROW_START + 1);
        s_ng_sound = NG_SND_MOVE;
    } else if ((kd & (KEY_LEFT | KEY_RIGHT)) && s_ng_cursor < NG_TOGGLES) {
        ng_rando_activate(s_ng_cursor);
    } else if (kd & KEY_A) {
        ng_rando_activate(s_ng_cursor);
    } else if ((kd & KEY_Y) && s_ng_cursor == NG_ROW_SEED) {
        s_ng_seed = 0;
        s_ng_sound = NG_SND_TOGGLE;
    } else if (kd & KEY_START) {
        ng_start(true);
    } else if (kd & KEY_B) {
        s_ng_page = NG_MODE;
        s_ng_cursor = 1;
        s_ng_sound = NG_SND_BACK;
    }
}

/* Cheats, Hard mode; Skip intro, Level 99 */
static bool *ng_switch(int k) {
    return k == 0 ? &s_ng_cheats : k == 1 ? &s_ng_hard : k == 2 ? &s_ng_skip : &s_ng_l99;
}

static void ng_touch(int px, int py) {
    if (s_ng_page == NG_MODE) {
        if (in(px, py, 16, 60, 156, NG_CARD_Y1)) {
            s_ng_cursor = 0;
            ng_start(false);
        } else if (in(px, py, 164, 60, 304, NG_CARD_Y1)) {
            s_ng_page = NG_RANDO;
            s_ng_cursor = NG_ROW_START;
            s_ng_sound = NG_SND_NEXT;
        } else if (py >= NG_SW_Y0 && py < NG_SW_Y0 + 2 * NG_SW_H + NG_SW_GAP && px >= 16 && px < 304) {
            int k = ((py - NG_SW_Y0) >= NG_SW_H + NG_SW_GAP / 2 ? 2 : 0) + (px >= 160 ? 1 : 0);
            bool *sw = ng_switch(k);
            *sw = !*sw;
            s_ng_sound = NG_SND_TOGGLE;
        } else if (in(px, py, 8, 212, 88, 236)) {
            ng_close(0, NG_SND_BACK);
        }
        return;
    }
    if (in(px, py, 8, 212, 88, 236)) {
        s_ng_page = NG_MODE;
        s_ng_cursor = 1;
        s_ng_sound = NG_SND_BACK;
    } else if (in(px, py, 196, 212, 316, 236)) {
        ng_start(true);
    } else if (py >= NG_ROW_Y0 && py < NG_ROW_Y0 + (NG_ROW_SEED + 1) * NG_ROW_H) {
        s_ng_cursor = (py - NG_ROW_Y0) / NG_ROW_H;
        ng_rando_activate(s_ng_cursor);
    }
}

/* console-font text word-wrapped to 36 columns, each line centred on cx */
static void ng_help(float cx, float y, const char *str) {
    /* translated whole, then wrapped: the lines of the English text were looked up one by one
     * and found nothing (GitHub #85). Breaks at a space, or before a Chinese/Japanese character. */
    char line[160];
    str = T(str);
    while (*str) {
        int i = 0, brk = -1, len;
        float w = 0;
        while (str[i]) {
            uint32_t cp = pm3ds_txt_next(str + i, &len);
            float cw = (cp & 0x80000000u) ? 10 : 8;
            if (w + cw > 288 && i > 0) break;
            if (cp == ' ' || (cp & 0x80000000u)) brk = i;
            w += cw;
            i += len;
        }
        int cut = str[i] && brk > 0 ? brk : i;
        if (cut > (int)sizeof line - 1) cut = (int)sizeof line - 1;
        memcpy(line, str, cut);
        line[cut] = 0;
        small_text_center(cx, y, line, 1.0f, COL_PANEL_EDGE);
        y += 10;
        str += cut;
        while (*str == ' ') str++;
    }
}

/* a pill button; lit: the cursor is on it */
static void ng_button(float x0, float y0, float x1, float y1, const char *label, bool lit) {
    if (lit) round_rect(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 11, RGBA(255, 196, 64, 255));
    round_rect(x0, y0, x1, y1, 8, COL_BTN);
    text_center((x0 + x1) / 2, y0 + 3, label, 1.0f, 1, COL_WHITE);
}

/* the picker at width w (top 400, bottom 320), centred */
static void draw_new_game(float w) {
    float dx = (w - 320) / 2;
    char buf[16];
    if (w > 320) fill(0, 0, w, 240, RGBA(40, 20, 0, 190));   /* dim the file menu behind it */
    panel(dx + 4, 4, dx + 316, 204, COL_PANEL);
    if (s_ng_page == NG_MODE) {
        static const char *const names[2] = { "Classic", "Randomizer" };
        static const char *const desc[2][2] = { { "The original", "adventure" }, { "Items & badges", "in new places" } };
        text_center(dx + 160, 12, "New game", 1.3f, 0, COL_WHITE);
        small_text_center(dx + 160, 38, "How do you want to play?", 1.0f, COL_PANEL_EDGE);
        for (int i = 0; i < 2; i++) {
            float x0 = dx + (i ? 164 : 16), x1 = x0 + 140;
            bool lit = s_ng_cursor == i;
            if (lit) round_rect(x0 - 4, 56, x1 + 4, NG_CARD_Y1 + 4, 12, RGBA(255, 196, 64, 255));
            round_rect(x0, 60, x1, NG_CARD_Y1, 10, COL_PANEL_EDGE);
            round_rect(x0 + 2, 62, x1 - 2, NG_CARD_Y1 - 2, 8, lit ? RGBA(255, 214, 120, 255) : COL_TAB);
            text_center((x0 + x1) / 2, 68, names[i], 1.2f, 0, COL_WHITE);
            small_text_center((x0 + x1) / 2, 104, desc[i][0], 1.0f, COL_PANEL_EDGE);
            small_text_center((x0 + x1) / 2, 116, desc[i][1], 1.0f, COL_PANEL_EDGE);
        }
        /* switches for the new save, set once: cheats (used from Options > Cheats in game)
         * hard mode (GitHub #69), level 99 (GitHub #83) */
        for (int k = 0; k < 4; k++) {
            float x0 = dx + (k & 1 ? 164 : 16), x1 = x0 + 140;
            float y0 = NG_SW_Y0 + (k >> 1) * (NG_SW_H + NG_SW_GAP), y1 = y0 + NG_SW_H;
            bool on = *ng_switch(k);
            static const char *const labels[4] = { "Cheats", "Hard mode", "Skip intro", "Level 99" };
            round_rect(x0, y0, x1, y1, 8, on ? RGBA(255, 214, 120, 150) : RGBA(205, 190, 170, 120));
            text(x0 + 6, y0 + 4, labels[k], fit(labels[k], 96, 0.8f, 0.45f), 0, COL_WHITE);
            round_rect(x1 - 34, y0 + 2, x1 - 3, y1 - 2, 8, COL_PANEL_EDGE);
            round_rect(x1 - 32, y0 + 4, x1 - 5, y1 - 4, 6, on ? RGBA(255, 214, 120, 255) : RGBA(205, 190, 170, 255));
            text_center(x1 - 18, y0 + 3, on ? "On" : "Off", 0.8f, 0, COL_WHITE);
        }
        /* 8 px a character (10 a Chinese one): narrowed to fit between Back (to x 88) and the edge */
        small_text_center_in(dx + 204, 219, "Y:cheats X:hard R:skip L:99", 1.0f, 224, COL_PANEL_EDGE);
        ng_button(dx + 8, 212, dx + 88, 234, "Back", false);
        return;
    }
    text_center(dx + 160, 8, "Randomizer", 1.2f, 0, COL_WHITE);
    small_text_center(dx + 160, 34, "Items and badges are always shuffled.", 1.0f, COL_PANEL_EDGE);
    for (int row = 0; row <= NG_ROW_SEED; row++) {
        float y = NG_ROW_Y0 + row * NG_ROW_H;
        const char *value;
        if (row < NG_TOGGLES) {
            value = (s_ng_flags & s_ng_toggles[row].flag) ? "On" : "Off";
        } else {
            if (s_ng_seed) snprintf(buf, sizeof buf, "%08X", s_ng_seed);
            value = s_ng_seed ? buf : "Random";
        }
        /* hidden spots only matter for shuffled key items */
        bool idle = row < NG_TOGGLES && s_ng_toggles[row].flag == PM3DS_RANDO_HIDDEN && !(s_ng_flags & PM3DS_RANDO_KEYS);
        u32 tint = idle ? RGBA(255, 255, 255, 110) : COL_WHITE;
        if (s_ng_cursor == row) round_rect(dx + 10, y, dx + 310, y + NG_ROW_H, 8, RGBA(255, 214, 120, 150));
        text(dx + 18, y + 2, row < NG_TOGGLES ? s_ng_toggles[row].label : "Seed", 1.0f, 0, tint);
        float vw = text_width(value, 1.0f) + 24;
        if (vw < 72) vw = 72;
        bool on = row < NG_TOGGLES && (s_ng_flags & s_ng_toggles[row].flag);
        round_rect(dx + 302 - vw, y + 1, dx + 302, y + NG_ROW_H - 1, 9, COL_PANEL_EDGE);
        round_rect(dx + 304 - vw, y + 3, dx + 300, y + NG_ROW_H - 3, 7,
                   (row == NG_ROW_SEED || on) && !idle ? RGBA(255, 214, 120, 255) : RGBA(205, 190, 170, 255));
        text_center(dx + 302 - vw / 2, y + 2, value, 1.0f, 0, tint);
    }
    const char *help = s_ng_cursor < NG_TOGGLES ? s_ng_toggles[s_ng_cursor].help
                       : s_ng_cursor == NG_ROW_SEED ? "A: type a seed, Y: random. Same seed and settings, same game."
                                                    : "START or A: begin. B: back.";
    ng_help(dx + 160, 176, help);
    ng_button(dx + 8, 212, dx + 88, 234, "Back", false);
    ng_button(dx + 196, 212, dx + 312, 234, "Start", s_ng_cursor == NG_ROW_START);
}

/* ---- update notice -------------------------------------------------------- */
/* pm3ds_update.c found a newer release: a notice over the menu until it is
 * tapped away (not in battle, not over the new-game picker) */
#define UPD_OK_X0 196
#define UPD_OK_X1 292
#define UPD_SKIP_X0 28
#define UPD_SKIP_X1 168
#define UPD_BTN_Y0 166
#define UPD_BTN_Y1 188
static bool update_notice_up(void) {
    return pm3ds_update_available() && !s_info.in_battle && s_ng_page == NG_CLOSED;
}

static void draw_update_notice(void) {
    int v = pm3ds_update_available();
    char line[48], skip[24];
    fill(0, 0, 320, 240, RGBA(40, 20, 0, 150));
    panel(10, 28, 310, 200, COL_PANEL);
    text_center(160, 38, "Update available", 1.2f, 0, COL_WHITE);
    snprintf(line, sizeof line, T("%s v%d.%d.%d is out."), PM3DS_APP_NAME, v / 10000, v / 100 % 100, v % 100);
    small_text_center(160, 70, line, 1.0f, COL_PANEL_EDGE);
    snprintf(line, sizeof line, T("You have v%s."), PM3DS_VERSION_STR);
    small_text_center(160, 82, line, 1.0f, COL_PANEL_EDGE);
    small_text_center(160, 104, "Get it on GitHub:", 1.0f, COL_PANEL_EDGE);
    small_text_center(160, 116, "github.com/" PM3DS_GITHUB_REPO, 1.0f, COL_PANEL_EDGE);
    small_text_center(160, 136, "FBI can install it from the", 1.0f, COL_PANEL_EDGE);
    small_text_center(160, 148, "QR code on that page.", 1.0f, COL_PANEL_EDGE);
    snprintf(skip, sizeof skip, T("Skip v%d.%d.%d"), v / 10000, v / 100 % 100, v % 100);
    ng_button(UPD_SKIP_X0, UPD_BTN_Y0, UPD_SKIP_X1, UPD_BTN_Y1, skip, false);
    ng_button(UPD_OK_X0, UPD_BTN_Y0, UPD_OK_X1, UPD_BTN_Y1, "OK", false);
}

/* ---- save import notice --------------------------------------------------- */
/* os_flash.c converted an import.fla / import.srm at boot (or found no save in it) */
static bool import_notice_up(void) {
    return pm3ds_save_import_result() != 0 && !s_info.in_battle && s_ng_page == NG_CLOSED;
}

static void draw_import_notice(void) {
    int n = pm3ds_save_import_result();
    char line[48];
    fill(0, 0, 320, 240, RGBA(40, 20, 0, 150));
    panel(10, 28, 310, 200, COL_PANEL);
    if (n > 0) {
        text_center(160, 38, "Save imported", 1.2f, 0, COL_WHITE);
        snprintf(line, sizeof line, n == 1 ? T("%d save file converted from your") : T("%d save files converted from your"), n);
        small_text_center(160, 74, line, 1.0f, COL_PANEL_EDGE);
        small_text_center(160, 86, "N64 save (now named import.*.done).", 1.0f, COL_PANEL_EDGE);
        if (pm3ds_save_import_backup()[0]) {
            small_text_center(160, 110, "Your previous saves were kept as", 1.0f, COL_PANEL_EDGE);
            snprintf(line, sizeof line, PM3DS_SD_DIR_SHOWN "/%s", pm3ds_save_import_backup());
            small_text_center(160, 122, line, 1.0f, COL_PANEL_EDGE);
        }
    } else {
        text_center(160, 38, "Save not imported", 1.2f, 0, COL_WHITE);
        small_text_center(160, 74, "The import file holds no Paper Mario", 1.0f, COL_PANEL_EDGE);
        small_text_center(160, 86, "save this version can read. It was", 1.0f, COL_PANEL_EDGE);
        small_text_center(160, 98, "renamed to import.*.bad.", 1.0f, COL_PANEL_EDGE);
        small_text_center(160, 122, "Your saves were not changed.", 1.0f, COL_PANEL_EDGE);
    }
    ng_button(UPD_OK_X0, UPD_BTN_Y0, UPD_OK_X1, UPD_BTN_Y1, "OK", false);
}

static void draw_menu_page(void);
static void draw_menu(void) {
    draw_menu_page();
    if (import_notice_up()) draw_import_notice();
    else if (update_notice_up()) draw_update_notice();
}

static void draw_menu_page(void) {
    gradient(0, 0, 320, 240, COL_BG_TOP, COL_BG_BOTTOM);
    if (s_ng_page != NG_CLOSED) {
        draw_new_game(320);
        return;
    }
    if (s_tab == TAB_DEBUG) {
        draw_debug();
        return;
    }
    if (s_tab == TAB_CHEATS && !cheats_shown()) s_tab = TAB_OPTIONS;   /* back on the title screen */
    if (s_tab == TAB_OPTIONS || s_tab == TAB_CONTROLS || s_tab == TAB_CHEATS) {
        rail_tab_t tabs[4];
        int ids[4], n = opt_rail(tabs, ids), active = 0;
        for (int i = 0; i < n; i++)
            if (ids[i] == s_tab) active = i;
        panel(PANEL_X0, PANEL_Y0, 316, PANEL_Y1, COL_PANEL);
        draw_rail(tabs, n, active);
        if (s_tab == TAB_OPTIONS) draw_options();
        else if (s_tab == TAB_CONTROLS) draw_controls();
        else draw_cheats();
        return;
    }
    if (!s_info.in_game && pm3ds_fm_active()) {
        draw_file_menu();
        draw_buttons();
        return;
    }
    if (!s_info.in_game) {
        panel(20, TITLE_CARD_Y0, 300, TITLE_CARD_Y1, COL_PANEL);
        text_center(160, 78, PM3DS_APP_NAME, 2.0f, 0, COL_WHITE);
        text_center(160, 124, s_info.demo ? "- Demo -" : "Tap or press START", 1.0f, 0, COL_WHITE);
        small_text_center(160, 154, "v" PM3DS_VERSION_STR, 1.0f, COL_PANEL_EDGE);
        if (pm3ds_title_has_language() && !s_info.demo) chip(90, TITLE_LANG_Y0, 230, TITLE_LANG_Y1, "Game language", COL_SEL, 0.8f);
        { extern bool pm3ds_low_memory_start(void);
          if (pm3ds_low_memory_start()) small_text_center(160, 172, T("Low memory start: close and reopen the game"), 1.0f, COL_PANEL_EDGE); }
        draw_buttons();
        return;
    }
    panel(PANEL_X0, PANEL_Y0, 316, PANEL_Y1, COL_PANEL);
    if (battle_page()) {
        draw_battle_rail();
        if (tm_open()) draw_tmenu();
        else draw_pad();
        draw_buttons();
        return;
    }
    draw_rail(s_game_rail, NUM_GAME_TABS, s_tab);
    if (tm_open()) {   /* an item or partner list in the world */
        draw_tmenu();
    } else {
        switch (s_tab) {
            case TAB_STATS: draw_stats(); break;
            case TAB_MAP:   draw_map();   break;
            case TAB_PARTY: draw_party(); break;
            case TAB_LOG:   draw_log();   break;
            case TAB_FOUND: draw_found(); break;
            case TAB_COOK:  draw_cook();  break;
            case TAB_BADGES: draw_badges(); break;
        }
    }
    draw_buttons();
}

/* ---- input --------------------------------------------------------------- */
static int s_tab_before_options = TAB_STATS;

static bool in(int px, int py, float x0, float y0, float x1, float y1) {
    return px >= x0 && px < x1 && py >= y0 && py < y1;
}

static void select_tab(int tab) {
    s_tab = tab;
    if (tab == TAB_LOG) { s_log_open = -1; pm3ds_tattle_query(&s_log); }
    if (tab == TAB_STATS || tab == TAB_MAP || tab == TAB_FOUND) pm3ds_tracker_query(&s_found);
    if (tab == TAB_FOUND && s_found.here >= 0) s_found_page = s_found.here / FOUND_ROWS;
    if (tab == TAB_FOUND) s_found_open = -1;
    if (tab == TAB_MAP) s_map_follow = true;
    if (tab == TAB_COOK) { s_cook_open = -1; s_quiz_open = -1; pm3ds_recipe_query(&s_cook); if (s_quiz) quiz_refresh(); }
    if (tab == TAB_BADGES) { s_badge_msg_timer = 0; s_loadout_saving = false; badges_refresh(); }
}

/* Settings > Bottom screen (GitHub #86, "play it as it was"): in game the bottom
 * screen's light goes off after a while without a touch, and a touch turns it on
 * again. The HOME Menu, sleep and quitting always get it back on. */
#define LCD_IDLE_MS 10000
static bool s_lcd_off;
static u64 s_lcd_idle_ms;

static void lcd_light(bool on) {
    static int ready;   /* 0 not tried, 1 up, -1 no gsp::Lcd */
    if (ready == 0) ready = R_SUCCEEDED(gspLcdInit()) ? 1 : -1;
    if (ready < 0) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    s_lcd_off = !on;
    s_lcd_idle_ms = osGetTime();
}

/* the light back on whatever happens next: boot, a panic screen, quitting */
void pm3ds_bottom_screen_lcd_restore(void) {
    if (s_lcd_off) lcd_light(true);
}

static void lcd_apt_hook(APT_HookType hook, void *param) {
    (void)param;
    if (s_lcd_off) lcd_light(true);   /* suspend, sleep, exit: the system's screens are lit */
    s_lcd_idle_ms = osGetTime();
    (void)hook;
}

static void lcd_tick(void) {
    static aptHookCookie cookie;
    static bool hooked;
    if (!hooked) {
        aptHook(&cookie, lcd_apt_hook, NULL);
        hooked = true;
        s_lcd_idle_ms = osGetTime();
    }
    bool want = pm3ds_cfg.bottom_dim && s_info.in_game && !s_quit && s_ng_page == NG_CLOSED;
    if (!want) {
        if (s_lcd_off) lcd_light(true);
        s_lcd_idle_ms = osGetTime();
    } else if (!s_lcd_off && osGetTime() - s_lcd_idle_ms > LCD_IDLE_MS) {
        lcd_light(false);
    }
}

static void handle_touch(void) {
    /* own edge detection: the input shim also scans HID, which can eat the
     * one-scan hidKeysDown() edge before this runs */
    static bool was_down;
    static int last_x, last_y, drag_dist;
    touchPosition tp = { 0, 0 };
    bool down = (hidKeysHeld() & KEY_TOUCH) != 0;
    if (down) hidTouchRead(&tp);
    bool pressed = down && !was_down, released = !down && was_down;
    was_down = down;
    {   /* Settings > Bottom screen: a touch on the dark screen only turns the light back on */
        static bool swallow;
        if (down) s_lcd_idle_ms = osGetTime();
        if (s_lcd_off) {
            if (pressed) {
                lcd_light(true);
                swallow = true;
            }
            return;
        }
        if (swallow) {
            if (!down) swallow = false;
            return;
        }
    }

    {   /* the Settings list: drag to scroll, a tap acts on release */
        static bool opt_held;
        static int opt_x0, opt_y0, opt_prev_y, opt_moved;
        bool on_list = s_tab == TAB_OPTIONS && !s_quit && !s_juke && s_ng_page == NG_CLOSED && !import_notice_up() && !update_notice_up();
        if (pressed && on_list && in(tp.px, tp.py, CX0 - 4, OPT_Y0, CX1 + 4, OPT_LIST_Y1)) {
            opt_held = true;
            opt_x0 = tp.px; opt_y0 = opt_prev_y = tp.py;
            opt_moved = 0;
        } else if (down && opt_held) {
            int dy = tp.py - opt_prev_y;
            opt_moved += abs(dy);
            if (opt_moved > 4) s_opt_scroll -= dy;
            opt_prev_y = tp.py;
        } else if (released && opt_held) {
            opt_held = false;
            if (on_list && opt_moved <= 4) options_tap(opt_x0, opt_y0);
        }
        if (!on_list) opt_held = false;
    }

    if (down && !pressed && s_tab == TAB_MAP && s_info.in_game && !tm_open()) {   /* drag the map */
        int dx = tp.px - last_x, dy = tp.py - last_y;
        if (in(last_x, last_y, MAP_X0, MAP_Y0, MAP_X1, MAP_Y1) && (dx || dy)) {
            drag_dist += abs(dx) + abs(dy);
            if (drag_dist > 4) {
                if (!pm3ds_cfg.map_dragged) {
                    pm3ds_cfg.map_dragged = true;
                    pm3ds_settings_save();
                }
                s_map_follow = false;
                s_map_cx -= dx;
                s_map_cy -= dy;
            }
        }
    }
    if (down) { last_x = tp.px; last_y = tp.py; }

    if (import_notice_up()) {   /* the notices take every tap until they are closed */
        pm3ds_touch_pad = 0;
        pm3ds_touch_stick_x = 0;
        if (pressed && in(tp.px, tp.py, UPD_OK_X0, UPD_BTN_Y0, UPD_OK_X1, UPD_BTN_Y1)) pm3ds_save_import_dismiss();
        return;
    }
    if (update_notice_up()) {
        pm3ds_touch_pad = 0;
        pm3ds_touch_stick_x = 0;
        if (pressed && in(tp.px, tp.py, UPD_OK_X0, UPD_BTN_Y0, UPD_OK_X1, UPD_BTN_Y1)) pm3ds_update_dismiss(false);
        else if (pressed && in(tp.px, tp.py, UPD_SKIP_X0, UPD_BTN_Y0, UPD_SKIP_X1, UPD_BTN_Y1)) pm3ds_update_dismiss(true);
        return;
    }

    /* held buttons: the battle pad and Back/Swap */
    pm3ds_touch_pad = 0;
    pm3ds_touch_stick_x = 0;
    if (down && s_tab <= TAB_GAME_LAST && s_info.in_game) {
        uint16_t mid_pad;
        pad_held(tp.px, tp.py);
        if (mid_button(&mid_pad) && mid_pad && in(tp.px, tp.py, s_btn_items[0], BTN_Y0, s_btn_items[1], BTN_Y1))
            pm3ds_touch_pad |= mid_pad;
    }
    if (s_title_start > 0) {   /* a tap on the title's card: START, long enough for a game step to see it */
        s_title_start--;
        pm3ds_touch_pad |= 0x1000;
    }
    if (!pressed) return;
    drag_dist = 0;
    if (s_ng_page != NG_CLOSED) {
        ng_touch(tp.px, tp.py);
        return;
    }

    if (s_tab == TAB_DEBUG) {
        if (in(tp.px, tp.py, 250, 214, 316, 236)) s_tab = TAB_OPTIONS;
        return;
    }
    bool back = in(tp.px, tp.py, s_btn_ff[0], BTN_Y0, s_btn_ff[1], BTN_Y1);
    if (s_tab == TAB_OPTIONS || s_tab == TAB_CONTROLS || s_tab == TAB_CHEATS) {
        rail_tab_t tabs[4];
        int ids[4], r = s_quit ? -1 : rail_hit(tp.px, tp.py, opt_rail(tabs, ids));
        if (r >= 0) {
            s_juke = false;
            s_tab = ids[r];
            return;
        }
    }
    if (s_tab == TAB_CHEATS) {
        int row = (tp.py - OPT_Y0) / CHEAT_ROW_H;
        if (in(tp.px, tp.py, s_btn_opt[0], BTN_Y0, s_btn_opt[1], BTN_Y1)) {
            s_cheat_give = !s_cheat_give;
            return;
        }
        if (s_cheat_give) {
            int g = (tp.py - OPT_Y0) / GIVE_ROW_H;
            if (back) s_cheat_give = false;
            else if (tp.px >= CX0 - 4 && tp.py >= OPT_Y0 && g < PM3DS_GIVE_COUNT) {
                if (g < PM3DS_GIVE_ITEM || tp.px >= 250) pm3ds_cheat_give(g);
                else pm3ds_cheat_give((g == PM3DS_GIVE_ITEM ? PM3DS_GIVE_ITEM_PREV : PM3DS_GIVE_BADGE_PREV) - (tp.px >= CX0 + 100));
            }
            return;
        }
        if (back) s_tab = TAB_OPTIONS;
        else if (tp.px >= CX0 - 4 && tp.py >= OPT_Y0 && row < PM3DS_CHEAT_COUNT) pm3ds_cheat_toggle(row);
        return;
    }
    if (s_tab == TAB_OPTIONS) {
        if (s_juke) {
            if (back) s_juke = false;
            else jukebox_tap(tp.px, tp.py);
            return;
        }
        if (s_quit) {
            if (back) s_quit = 0;
            else quit_tap(tp.px, tp.py);
            return;
        }
        if (back) {
            s_tab = s_tab_before_options;
            return;
        }
        if (s_info.in_game && in(tp.px, tp.py, s_btn_opt[0], BTN_Y0, s_btn_opt[1], BTN_Y1)) s_quit = 1;
        return;   /* the list acts on release (handle_touch) */
    }
    if (s_tab == TAB_CONTROLS) {
        if (back) {
            s_tab = TAB_OPTIONS;
        } else if (in(tp.px, tp.py, s_btn_opt[0], BTN_Y0, s_btn_opt[1], BTN_Y1)) {
            pm3ds_settings_default_keymap(pm3ds_cfg.keymap);
            pm3ds_settings_save();
        } else if (tp.px >= CX0 - 4 && tp.py >= CTL_Y0 && tp.py < CTL_Y0 + CTL_ROWS * CTL_ROW_H) {
            int k = (tp.py - CTL_Y0) / CTL_ROW_H + (tp.px >= CX0 + 124 ? CTL_ROWS : 0);
            if (k < KEY_IDX_COUNT) {
                pm3ds_cfg.keymap[k] = (uint8_t)((pm3ds_cfg.keymap[k] + 1) % ACT_COUNT);
                pm3ds_settings_save();
            }
        }
        return;
    }
    int r;
    if (in(tp.px, tp.py, s_btn_opt[0], BTN_Y0, s_btn_opt[1], BTN_Y1)) {
        s_tab_before_options = s_tab;
        s_tab = TAB_OPTIONS;
    } else if (in(tp.px, tp.py, s_btn_ff[0], BTN_Y0, s_btn_ff[1], BTN_Y1)) {
        pm3ds_ff_button_on = !pm3ds_ff_button_on;
    } else if (!s_info.in_game && pm3ds_fm_active()) {
        fm_page_tap(tp.px, tp.py);
    } else if (!s_info.in_game && in(tp.px, tp.py, 20, TITLE_CARD_Y0, 300, TITLE_CARD_Y1)) {
        pm3ds_title_pick(false);
        s_title_start = 4;
    } else if (!s_info.in_game && !s_info.demo && pm3ds_title_has_language()
               && in(tp.px, tp.py, 90, TITLE_LANG_Y0, 230, TITLE_LANG_Y1)) {
        pm3ds_title_pick(true);
        s_title_start = 4;
    } else if (s_info.in_game && in(tp.px, tp.py, s_btn_items[0], BTN_Y0, s_btn_items[1], BTN_Y1)) {
        uint16_t mid_pad;
        const char *mid = mid_button(&mid_pad);
        if (mid && !mid_pad) {   /* Items or Badges; Back/Swap are held above */
            if (in(tp.px, tp.py, s_btn_badges[0], BTN_Y0, s_btn_badges[1], BTN_Y1)) select_tab(TAB_BADGES);
            else pm3ds_touch_req = 1;
        }
    } else if (battle_page()) {
        tmenu_tap(tp.px, tp.py);
    } else if (s_info.in_game && s_tab <= TAB_GAME_LAST && (r = rail_hit(tp.px, tp.py, NUM_GAME_TABS)) >= 0) {
        select_tab(r);
    } else if (s_info.in_game && s_tab <= TAB_GAME_LAST && tm_open()) {
        tmenu_tap(tp.px, tp.py);
    } else if (s_info.in_game && s_tab == TAB_STATS && in(tp.px, tp.py, QS_X0 - 4, QS_Y0 - 4, QS_X1 + 6, QS_Y1 + 4)) {
        qs_tap();
    } else if (s_info.in_game && s_tab == TAB_LOG) {
        log_tap(tp.px, tp.py);
    } else if (s_info.in_game && s_tab == TAB_COOK) {
        cook_tap(tp.px, tp.py);
    } else if (s_info.in_game && s_tab == TAB_BADGES) {
        badges_tap(tp.px, tp.py);
    } else if (s_info.in_game && s_tab == TAB_FOUND) {
        found_tap(tp.px, tp.py);
    } else if (s_info.in_game && s_tab == TAB_PARTY && partner_at(tp.px, tp.py)) {
        int id = partner_at(tp.px, tp.py);
        if ((s_info.partners_owned >> id) & 1) pm3ds_touch_req = 0x100 + id;
    } else if (s_tab == TAB_MAP && !s_map_follow && in(tp.px, tp.py, MAP_CENTRE_X0, MAP_CENTRE_Y0, MAP_X1, MAP_Y1)) {
        s_map_follow = true;
    }
}

/* ---- public -------------------------------------------------------------- */
void pm3ds_bottom_screen_init(void) {
    pm3ds_settings_load();

    init_morton();
    s_target = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
    if (s_target)
        C3D_RenderTargetSetOutput(s_target, GFX_BOTTOM, GFX_LEFT,
                                  GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                  GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                  GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    s_vbuf = (vtx_t *)linearAlloc(sizeof(vtx_t) * UI_MAX_VERTS);
    AttrInfo_Init(&s_attr);
    AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(&s_attr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&s_attr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&s_attr, 3, GPU_FLOAT, 2);
    BufInfo_Init(&s_buf);
    BufInfo_Add(&s_buf, s_vbuf, sizeof(vtx_t), 4, 0x3210);
    /* bottom-screen pixel space -> rotated PICA clip space (320x240) */
    Mtx_Zeros(&s_mtx);
    s_mtx.r[0].y = -1.0f / 120.0f; s_mtx.r[0].w = 1.0f;
    s_mtx.r[1].x = -1.0f / 160.0f; s_mtx.r[1].w = 1.0f;
    s_mtx.r[2].z = 0.5f;           s_mtx.r[2].w = -0.5f;
    s_mtx.r[3].w = 1.0f;
    /* the same for the top screen (400x240), where the boot screens draw too */
    s_mtx_top = s_mtx;
    s_mtx_top.r[1].x = -1.0f / 200.0f;
    build_atlas();
    build_map();
    build_small_font();
    if (pm3ds_cfg.bottom_dim) lcd_light(true);   /* a crash may have left it off */
}

/* once per presented frame (input + state) */
void pm3ds_bottom_screen_tick(unsigned frame) {
    s_frame = frame;
    ng_buttons();
    handle_touch();
    if (s_ng_kbd) {
        s_ng_kbd = false;
        ng_keyboard();
    }
    pm3ds_console_debug = s_tab == TAB_DEBUG;
    if (!s_font_ok && (frame % 30) == 0) build_font();
    if (frame % 5 == 0) pm3ds_hud_query(&s_info);
    qs_tick();
    if (!s_info.in_game) s_quit = 0;
    quit_tick();
    if (s_badge_msg_timer > 0) s_badge_msg_timer--;
    /* the log and tracker read a few hundred flags: only while shown, twice a second */
    if (s_info.in_game && frame % 30 == 0) {
        if (s_tab == TAB_LOG && s_log_open < 0) pm3ds_tattle_query(&s_log);
        if (s_tab == TAB_STATS || s_tab == TAB_MAP || s_tab == TAB_FOUND) pm3ds_tracker_query(&s_found);
        if (s_tab == TAB_FOUND && s_found_open >= 0) pm3ds_tracker_spots(s_found_open, &s_spots);
        if (s_tab == TAB_COOK && s_cook_open < 0) pm3ds_recipe_query(&s_cook);
        if (s_tab == TAB_BADGES) pm3ds_badge_query(&s_badges);
    }
    pm3ds_tm_get(&s_tm);
    if (s_tm.kind == PM3DS_TM_TARGET && s_info.in_battle) pm3ds_gfx_request_top_copy();   /* the portraits */
    icons_update();
    lcd_tick();
}

/* GPU state for a UI pass on target t with pixel matrix m; false if the
 * renderer is not up. after: a pass earlier in the same frame drew from the
 * vertex buffer, so its vertices stay and this one appends. */
static bool begin_ui_on(C3D_RenderTarget *t, const C3D_Mtx *m, bool after) {
    if (!t || !s_vbuf || !s_atlas_ok) return false;
    if (after) flush();
    C3D_FrameDrawOn(t);
    C3D_BindProgram(pm3ds_gfx_program());
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, pm3ds_gfx_uloc_screen(), m);
    C3D_SetAttrInfo(&s_attr);
    C3D_SetBufInfo(&s_buf);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));

    if (!after) s_vcount = 0;
    s_batch_start = s_vcount;
    s_cur_tex = NULL;
    return true;
}

static void end_ui(void) {
    flush();
    /* not GSPGPU_FlushDataCache: an IPC to the system core, slow on the
     * Old 3DS where the audio mixer has most of that core */
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)s_vbuf, sizeof(vtx_t) * (u32)s_vcount);
}

/* inside a citro3d frame, after the top screen; top, top_right: its render
 * targets (top_right NULL without 3D), for the new-game picker */
void pm3ds_bottom_screen_render(C3D_RenderTarget *top, C3D_RenderTarget *top_right) {
    bool after = false;
#if VERSION_IQUE
    s_ui_frame++;
#endif
    if (s_ng_page != NG_CLOSED) {
        C3D_RenderTarget *eyes[2] = { top, top_right };
        for (int i = 0; i < 2; i++) {
            if (eyes[i] && begin_ui_on(eyes[i], &s_mtx_top, after)) {
                draw_new_game(400);
                after = true;
            }
        }
    }
    if (!begin_ui_on(s_target, &s_mtx, after)) {
        if (after) end_ui();
        return;
    }
    draw_menu();
    end_ui();
}

/* 8x8 console-font text at an integer scale, centred on cx */
/* the width small_text_center gives str (already in the UI encoding) at scale 1 */
static float small_text_width(const char *str) {
    float w = 0;
    for (int len; *str; str += len) w += pm3ds_txt_next(str, &len) & 0x80000000u ? 10 : 8;
    return w;
}

/* centred 8x8 console-font text, in the chosen language; narrowed to max_w
 * (0: the screen) when a translation is longer */
static void small_text_center_in(float cx, float y, const char *str, float scale, float max_w, u32 c) {
    if (!s_small_ok) return;
    str = T(str);
    float w = small_text_width(str) * scale;
    if (max_w <= 0) max_w = 312;
    if (w > max_w) { scale *= max_w / w; w = max_w; }
    float x = cx - w / 2;
    for (int len; *str; str += len) {
        uint32_t cp = pm3ds_txt_next(str, &len);
        if (cp & 0x80000000u) {   /* Chinese, Japanese */
            sys_glyph(x - scale, y - 2 * scale, cp & 0x1FFFFF, 0.72f * scale, c, true, false);
            x += 10 * scale;
            continue;
        }
        unsigned char ch = (unsigned char)cp;
        if (ch >= 0x80 && ch < 0xB0) ch = (unsigned char)"AAAACEEEEIIIINOOOOUUUUsaaaaceeeeiiiinoooouuuu!?a"[ch - 0x80];   /* accents: the console font has none */
        if (ch > 32 && ch < 128) {
            float gx = (ch % 16) * 8.0f, gy = (ch / 16) * 8.0f;
            quad2(&s_small, x, y, x + 8 * scale, y + 8 * scale,
                  gx / 128.0f, gy / 64.0f, (gx + 8) / 128.0f, (gy + 8) / 64.0f, c, c);
        }
        x += 8 * scale;
    }
}

static void small_text_center(float cx, float y, const char *str, float scale, u32 c) {
    small_text_center_in(cx, y, str, scale, 0, c);
}

static const char *s_loading_label;

/* what the progress bar is for; NULL: loading the game data */
void pm3ds_bottom_screen_loading_label(const char *label) { s_loading_label = label; }

/* Boot screens, drawn on both screens so the top one is never just black.
 * The game's font comes from the ROM, so these use the console font.
 * w: 400 (top) or 320 (bottom). */
static void loading_panel(float w, int permille) {
    float cx = w / 2, dx = (w - 320) / 2;
    gradient(0, 0, w, 240, COL_BG_TOP, COL_BG_BOTTOM);
    panel(36 + dx, 62, 284 + dx, 178, COL_PANEL);
    small_text_center(cx, 82, PM3DS_APP_NAME_CAPS, 2.0f, COL_PANEL_EDGE);
    small_text_center(cx, 112, s_loading_label ? s_loading_label : "Loading game data", 1.0f, COL_PANEL_EDGE);
    bar(60 + dx, 132, 200, 10, permille, 1000, COL_FP);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", permille / 10);
    small_text_center(cx, 152, pct, 1.0f, COL_PANEL_EDGE);
}

/* lines[0] is the headline: twice the size where it fits */
static void message_panel(float w, const char *const *lines, int n) {
    float cx = w / 2, y = 52;
    gradient(0, 0, w, 240, COL_BG_TOP, COL_BG_BOTTOM);
    panel(10, 10, w - 10, 230, COL_PANEL);
    small_text_center(cx, 22, PM3DS_APP_NAME_CAPS, 2.0f, COL_PANEL_EDGE);
    for (int i = 0; i < n && i < 14; i++) {
        float s = (i == 0 && strlen(lines[i]) * 16.0f <= w - 40) ? 2.0f : 1.0f;
        small_text_center(cx, y, lines[i], s, COL_PANEL_EDGE);
        y += 4 + 8 * s;
    }
}

/* Boot: progress of the ROM load, inside a citro3d frame; top: the top
 * screen's render target */
void pm3ds_ui_loading(C3D_RenderTarget *top, int permille) {
    bool after = false;
    if (top && begin_ui_on(top, &s_mtx_top, false)) {
        loading_panel(400, permille);
        after = true;
    }
    if (!begin_ui_on(s_target, &s_mtx, after)) {
        if (after) end_ui();
        return;
    }
    loading_panel(320, permille);
    end_ui();
}

/* Boot problems (no ROM, wrong ROM): the same text on both screens */
void pm3ds_ui_message(C3D_RenderTarget *top, const char *const *lines, int n) {
    bool after = false;
    if (top && begin_ui_on(top, &s_mtx_top, false)) {
        message_panel(400, lines, n);
        after = true;
    }
    if (!begin_ui_on(s_target, &s_mtx, after)) {
        if (after) end_ui();
        return;
    }
    message_panel(320, lines, n);
    end_ui();
}
