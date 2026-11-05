/*
 * starter.c — main() of the release package, which holds the game three
 * times: built for the European, the US and the iQue ROM (tools/universal.py
 * links them; the US and iQue copies' symbols carry a us_ / iq_ prefix). This
 * looks at the player's ROM on the SD card, loads the matching copy's data
 * into the area the copies share (universal/universal.ld) and hands over.
 *
 * One ROM (or none): run its copy; with none, the PAL copy shows what to do.
 * More than one version on the card: ask which to play.
 */
#include <3ds.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "pm3ds_brand.h"

int pal_main(int argc, char **argv);   /* the PAL copy's port_main.c main(), renamed */
int us_main(int argc, char **argv);    /* the US copy's */
int iq_main(int argc, char **argv);    /* the iQue copy's */
extern char __pm3ds_copy_area[], __pm3ds_copy_area_end[];   /* universal.ld */

/* the old SD folder, moved before looking for ROMs in the new one (each copy
 * does this again, with a log line: pm3ds_rom.c pm3ds_sd_migrate) */
static void sd_migrate(void) {
    struct stat st;
    if (stat(PM3DS_SD_DIR, &st) != 0 && stat(PM3DS_SD_DIR_OLD, &st) == 0) rename(PM3DS_SD_DIR_OLD, PM3DS_SD_DIR);
}

/* the region letter of an N64 Paper Mario ROM file in any byte order:
 * 'P' (Europe), 'E' (USA), 'C' (the iQue Player's: 40 MB, blank name and game
 * ID); 0 for anything else */
static char rom_region(const char *path) {
    unsigned char h[0x40];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(h, 1, sizeof h, f);
    long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    fclose(f);
    if (n != sizeof h) return 0;
    uint32_t w = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    if (w == 0x37804012u) {                       /* .v64: 16-bit halves swapped */
        for (size_t i = 0; i < n; i += 2) {
            unsigned char t = h[i];
            h[i] = h[i + 1];
            h[i + 1] = t;
        }
    } else if (w == 0x40123780u) {                /* .n64: 32-bit words reversed */
        for (size_t i = 0; i < n; i += 4) {
            unsigned char a = h[i], b = h[i + 1];
            h[i] = h[i + 3];
            h[i + 1] = h[i + 2];
            h[i + 2] = b;
            h[i + 3] = a;
        }
    } else if (w != 0x80371240u) {
        return 0;
    }
    if (!memcmp(h + 0x3B, "NMQ", 3))              /* game code: Paper Mario */
        return (h[0x3E] == 'P' || h[0x3E] == 'E') ? (char)h[0x3E] : 0;
    if (size != 0x2800000) return 0;
    for (int i = 0x20; i < 0x3F; i++)             /* name, then the game ID */
        if (h[i] != 0 && !(i >= 0x34 && i < 0x3B)) return 0;
    return 'C';
}

/* which versions are on the card, as the copies look for them (pm3ds_rom.c):
 * by header, in the folders of PM3DS_ROM_DIRS, whatever the files are called */
static void find_versions(bool *eu, bool *us, bool *iq) {
    static const char *const dirs[] = { PM3DS_ROM_DIRS };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        size_t l = strlen(dirs[i]);
        const char *sep = l && dirs[i][l - 1] == '/' ? "" : "/";
        struct dirent *e;
        char path[300];
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;
            snprintf(path, sizeof path, "%s%s%s", dirs[i], sep, e->d_name);
            char r = rom_region(path);
            *eu |= r == 'P';
            *us |= r == 'E';
            *iq |= r == 'C';
        }
        closedir(d);
    }
}

/* ---- the top screen while asking: the boot screens' look, drawn by hand ---- */
static u8 *s_top;   /* BGR8, column-major from the bottom (the panel's native layout) */

static void put(int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || x >= 400 || y < 0 || y >= 240) return;
    u8 *p = s_top + ((size_t)x * 240 + (239 - y)) * 3;
    p[0] = b; p[1] = g; p[2] = r;
}

static void box(int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b) {
    for (int x = x0; x < x1; x++)
        for (int y = y0; y < y1; y++) put(x, y, r, g, b);
}

/* the console's 8x8 font at an integer scale, centred on x = 200 */
static void text(int y, const char *str, int scale, u8 r, u8 g, u8 b) {
    PrintConsole *pc = consoleGetDefault();
    int x = 200 - (int)strlen(str) * 4 * scale;
    for (; *str; str++, x += 8 * scale) {
        unsigned char c = (unsigned char)*str;
        if (!pc->font.gfx || c >= pc->font.numChars) continue;
        const u8 *gl = (const u8 *)pc->font.gfx + c * 8;
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
                if (gl[j] & (0x80 >> i)) box(x + i * scale, y + j * scale, x + (i + 1) * scale, y + (j + 1) * scale, r, g, b);
    }
}

static void draw_top(void) {
    gfxSetDoubleBuffering(GFX_TOP, false);
    s_top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int y = 0; y < 240; y++) {   /* orange gradient */
        int r = 250 - (250 - 222) * y / 239, g = 196 - (196 - 130) * y / 239, b = 110 - (110 - 60) * y / 239;
        box(0, y, 400, y + 1, (u8)r, (u8)g, (u8)b);
    }
    box(79, 65, 327, 181, 80, 40, 10);        /* shadow */
    box(76, 62, 324, 178, 116, 72, 40);       /* rim */
    box(78, 64, 322, 176, 255, 247, 222);     /* panel */
    text(82, PM3DS_APP_NAME_CAPS, 2, 116, 72, 40);
    text(116, "More than one Paper Mario ROM", 1, 116, 72, 40);
    text(140, "Choose one on the", 1, 116, 72, 40);
    text(152, "bottom screen", 1, 116, 72, 40);
    gfxFlushBuffers();
}

/* several versions present: 'E' (USA), 'P' (Europe), 'C' (iQue), 0 if the
 * player closed the app */
static char ask(bool eu, bool us, bool iq) {
    gfxInitDefault();
    draw_top();
    consoleInit(GFX_BOTTOM, NULL);
    printf("\n\n  " PM3DS_APP_NAME "\n\n"
           "  Your SD card has more than one ROM.\n"
           "  Which one do you want to play?\n\n");
    if (us) printf("    A   USA\n");
    if (eu) printf("    B   Europe (English, German,\n"
                   "        French, Spanish)\n");
    if (iq) printf("    X   iQue (Chinese)\n");
    char pick = 0;
    while (!pick && aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if ((k & KEY_A) && us) pick = 'E';
        else if ((k & KEY_B) && eu) pick = 'P';
        else if ((k & KEY_X) && iq) pick = 'C';
        gfxFlushBuffers();      /* without these the console never reaches the screen */
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return pick;
}

/* romfs:/copy_<v>.bin (tools/package.sh): "SHCP", the area's link address,
 * the data's size, its zlib size, then the zlib stream. Loaded into the area
 * crt0 zeroed, so the copy's .bss after its data starts at 0. */
static const char *load_copy(const char *v) {
    static char why[96];
    char path[48];
    snprintf(path, sizeof path, "romfs:/copy_%s.bin", v);
    if (R_FAILED(romfsInit())) return "romfs could not be opened";
    FILE *f = fopen(path, "rb");
    uint32_t h[4];
    const char *err = NULL;
    if (!f || fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "SHCP", 4)) {
        err = "the game data is missing (reinstall)";
    } else if (h[1] != (uint32_t)(uintptr_t)__pm3ds_copy_area) {
        /* the copies' data holds absolute addresses: it only works where it was linked */
        snprintf(why, sizeof why, "loaded at 0x%lX, not 0x%lX", (unsigned long)(uintptr_t)__pm3ds_copy_area,
                 (unsigned long)h[1]);
        err = why;
    } else if (h[2] > (uint32_t)(__pm3ds_copy_area_end - __pm3ds_copy_area)) {
        err = "the game data doesn't fit (reinstall)";
    } else {
        unsigned char *z = (unsigned char *)malloc(h[3]);
        uLongf out = h[2];
        if (!z) err = "not enough memory";
        else if (fread(z, 1, h[3], f) != h[3]) err = "the game data is damaged (reinstall)";
        else if (uncompress((Bytef *)__pm3ds_copy_area, &out, z, h[3]) != Z_OK || out != h[2])
            err = "the game data is damaged (reinstall)";
        free(z);
    }
    if (f) fclose(f);
    romfsExit();
    return err;
}

static void show_error(const char *err) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("\n\n  " PM3DS_APP_NAME "\n\n  Could not start:\n  %s\n\n  Press START to exit\n", err);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
}

int main(int argc, char **argv) {
    sd_migrate();
    bool eu = false, us = false, iq = false;
    find_versions(&eu, &us, &iq);
    int n = eu + us + iq;
    char pick = n == 0 ? 'P' : n > 1 ? ask(eu, us, iq) : us ? 'E' : iq ? 'C' : 'P';
    if (!pick) return 0;
    const char *v = pick == 'E' ? "us" : pick == 'C' ? "iq" : "pal";
    const char *err = load_copy(v);
    if (err) {
        show_error(err);
        return 0;
    }
    if (pick == 'E') return us_main(argc, argv);
    if (pick == 'C') return iq_main(argc, argv);
    return pal_main(argc, argv);
}
