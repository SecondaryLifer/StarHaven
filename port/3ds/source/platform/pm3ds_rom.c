/*
 * pm3ds_rom.c — the player's ROM: find it, check it, load it, and put back
 * the game data that a release build leaves out of the program.
 *
 * The ROM goes on the SD card in /3ds/StarHaven under any name; baserom.z64
 * there is tried first, then every Paper Mario ROM in the folders of
 * PM3DS_ROM_DIRS (pm3ds_brand.h), told apart by its header, not its name.
 * .n64 and .v64 dumps (other byte orders) are converted while loading. Dev
 * builds may still carry it in romfs. The ROM is checked here once, then read on demand
 * (n64_shim/rom_stream.c): it no longer has to fit in RAM. Only the game's data is kept (pm3ds_rom_layout.h: 48 MB
 * for Europe, whose 64 MB dumps repeat 0x2000000-0x3000000 after it; 40 MB
 * for the US), and a CRC32 of it in .z64 order must match the version the
 * port is built for.
 *
 * The table in romfs (PM3DS_ROMDATA_PATH: one per version, so a package can
 * hold both), written at packaging time by tools/romdata.py, lists
 * every object the build compiled from the ROM and then zeroed: images,
 * palettes and fonts (raw bytes), vertex lists (big-endian shorts) and
 * display lists (big-endian words, plus the addresses the linker relocated).
 * Local dev builds have no such file: their data is still compiled in.
 */
#include <3ds.h>
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "pm3ds_brand.h"
#include "pm3ds_log.h"
#include "pm3ds_rom_layout.h"
#include "pm3ds_rom_stream.h"

#define ROM_DIR      PM3DS_SD_DIR
#define ROM_SIZE     ((uint32_t)PM3DS_ROM_DATA_SIZE)
#define ROM_CRC32    ((uint32_t)PM3DS_ROM_CRC32)     /* 0: not checked (romdata.bin has the real one) */
#define ROM_MAX_FILE 0x4000000u      /* larger dumps: the tail is a mirror or padding */

extern void pm3ds_pi_rom_ready(void);
extern void pm3ds_gfx_present_loading(int permille);
extern void pm3ds_gfx_present_message(const char *const *lines, int n);
extern void pm3ds_bottom_screen_loading_label(const char *label);
extern void pm3ds_bottom_screen_reload_art(void);
extern volatile int pm3ds_boot_permille;

/* romdata.bin's link-time address of this object gives the load offset */
const uint32_t pm3ds_romdata_anchor = 0x504D5244u;
/* "FULL" as compiled; romdata.py changes it to "STRP" in the copy it zeroes,
 * so a stripped program that can't find its table refuses to start */
volatile uint32_t pm3ds_romdata_state = 0x46554C4Cu;
#define ROMDATA_STRIPPED 0x53545250u

typedef struct {
    char magic[4];               /* "PMRD" */
    uint32_t version;            /* 1 */
    uint32_t rom_size, rom_crc32;
    uint32_t anchor;             /* link-time &pm3ds_romdata_anchor */
    uint32_t n_blobs, n_ptrs;
    uint32_t data_crc32;         /* of all objects as built, in table order */
    char rom_name[32];
} romdata_header_t;
typedef struct { uint32_t dst, rom, size, kind; } romdata_blob_t;    /* kind: 0 raw, 1 Vtx, 2 Gfx */
typedef struct { uint32_t dst, value; } romdata_patch_t;   /* a kept address word, link-time value */

static uint8_t *s_table;          /* PM3DS_ROMDATA_PATH, NULL in builds with the data inside */
static size_t s_table_size;
static uint32_t s_want_size = ROM_SIZE, s_want_crc = ROM_CRC32;

/* ---- CRC32 (slicing by 4) ------------------------------------------------ */
static uint32_t s_crc_tab[4][256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        s_crc_tab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 4; t++)
            s_crc_tab[t][i] = (s_crc_tab[t - 1][i] >> 8) ^ s_crc_tab[0][s_crc_tab[t - 1][i] & 0xFF];
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (; n >= 4; n -= 4, p += 4) {
        crc ^= (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        crc = s_crc_tab[3][crc & 0xFF] ^ s_crc_tab[2][(crc >> 8) & 0xFF] ^
              s_crc_tab[1][(crc >> 16) & 0xFF] ^ s_crc_tab[0][crc >> 24];
    }
    while (n--) crc = (crc >> 8) ^ s_crc_tab[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}

/* ---- byte orders ---------------------------------------------------------- */
enum { ORDER_Z64 = PM3DS_ORDER_Z64, ORDER_V64 = PM3DS_ORDER_V64, ORDER_N64 = PM3DS_ORDER_N64,
       ORDER_UNKNOWN = PM3DS_ORDER_UNKNOWN };

static int detect_order(const uint8_t *p) {
    uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    if (w == 0x80371240u) return ORDER_Z64;
    if (w == 0x37804012u) return ORDER_V64;    /* 16-bit halves swapped */
    if (w == 0x40123780u) return ORDER_N64;    /* 32-bit words reversed */
    return ORDER_UNKNOWN;
}

static void to_z64(uint8_t *p, size_t n, int order) {
    uint32_t *w = (uint32_t *)p;
    if (order == ORDER_V64) {
        for (size_t i = 0; i < n / 4; i++) w[i] = ((w[i] & 0x00FF00FFu) << 8) | ((w[i] >> 8) & 0x00FF00FFu);
    } else if (order == ORDER_N64) {
        for (size_t i = 0; i < n / 4; i++) w[i] = __builtin_bswap32(w[i]);
    }
}

/* ---- the SD folder -------------------------------------------------------- */
/* Builds before the rename kept the save, settings and ROM in /3ds/PaperMario3DS:
 * move that folder once, before anything else touches the SD card. Then make
 * sure the folder exists, so a first start without a ROM leaves the folder the
 * ROM goes into (GitHub #50). */
void pm3ds_sd_migrate(void) {
    struct stat st;
    if (stat(PM3DS_SD_DIR, &st) != 0 && stat(PM3DS_SD_DIR_OLD, &st) == 0) {
        if (rename(PM3DS_SD_DIR_OLD, PM3DS_SD_DIR) == 0)
            pm3ds_log(PM3DS_LOG_INFO, "sd: moved " PM3DS_SD_DIR_OLD " to " PM3DS_SD_DIR);
        else
            pm3ds_log(PM3DS_LOG_ERROR, "sd: could not move " PM3DS_SD_DIR_OLD " to " PM3DS_SD_DIR);
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir(PM3DS_SD_DIR, 0777);
}

/* ---- loading -------------------------------------------------------------- */
static void restore_chunk(uint32_t base, const uint8_t *buf, uint32_t len);

typedef struct {
    char path[256];
    int order;                   /* of the accepted file */
    char reason[48];             /* why the last candidate was refused */
    char detail[48];
} rom_status_t;

#if !VERSION_IQUE
/* the iQue Player's dump: 40 MB, an N64 header with a blank name and game ID */
static bool is_ique_rom(FILE *f, long sz) {
    uint8_t h[64];
    if (sz != 0x2800000 || fread(h, 1, sizeof h, f) != sizeof h) return false;
    rewind(f);
    int order = detect_order(h);
    if (order == ORDER_UNKNOWN) return false;
    to_z64(h, sizeof h, order);
    for (int i = 0x20; i < 0x3F; i++)   /* name, then the game ID */
        if (h[i] != 0 && !(i >= 0x34 && i < 0x3B)) return false;
    return true;
}
#endif

/* Read one candidate through (ROM_SIZE bytes), converting to .z64 order and
 * checking the CRC as it goes. Returns true if it is the right ROM. */
static bool load_candidate(const char *path, rom_status_t *st) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    snprintf(st->path, sizeof st->path, "%s", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
#if !VERSION_IQUE
    if (is_ique_rom(f, sz)) {   /* the other app's ROM: say which one it needs */
        fclose(f);
        /* the combined package runs it itself (universal/starter.c picks the copy) */
        extern char __pm3ds_copy_area[] __attribute__((weak));
        snprintf(st->reason, sizeof st->reason, "This is the iQue ROM");
        snprintf(st->detail, sizeof st->detail, "%s", __pm3ds_copy_area ? "" : "it needs Star Haven iQue");
        pm3ds_log(PM3DS_LOG_WARN, "rom: %s is the iQue ROM", path);
        return false;
    }
#endif
    if (sz < (long)s_want_size || sz > (long)ROM_MAX_FILE) {
        fclose(f);
        snprintf(st->reason, sizeof st->reason, "Wrong file size");
        snprintf(st->detail, sizeof st->detail, "%ld bytes, needs " PM3DS_ROM_SIZES, sz);
        pm3ds_log(PM3DS_LOG_WARN, "rom: %s is %ld bytes, not %lu..%lu", path, sz, (unsigned long)s_want_size,
                  (unsigned long)ROM_MAX_FILE);
        return false;
    }
    /* chunks: one 64 MB fread becomes a single FS request whose buffer the
     * kernel must map, and real hardware kernel-panics in fs */
    const size_t chunk = 512 * 1024;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    if (!buf) {
        fclose(f);
        snprintf(st->reason, sizeof st->reason, "Not enough memory");
        st->detail[0] = 0;
        return false;
    }
    size_t n = 0;
    int order = ORDER_UNKNOWN;
    bool other_game = false;
    uint32_t crc = 0;
    uint64_t t0 = svcGetSystemTick();
    while (n < s_want_size) {
        size_t want = s_want_size - n < chunk ? s_want_size - n : chunk;
        size_t got = fread(buf, 1, want, f);
        if (got != want) break;
        if (n == 0) {
            order = detect_order(buf);
            if (order == ORDER_UNKNOWN) break;
        }
        to_z64(buf, got, order);
        /* the header's game code and region: another game or version is
         * refused before reading the rest */
        if (n == 0 && (other_game = memcmp(buf + 0x3B, PM3DS_ROM_GAME_ID, 4) != 0)) break;
        crc = crc_update(crc, buf, got);
        restore_chunk((uint32_t)n, buf, (uint32_t)got);
        n += got;
        pm3ds_boot_permille = (int)((uint64_t)n * 1000 / s_want_size);
        pm3ds_gfx_present_loading(pm3ds_boot_permille);
    }
    fclose(f);
    pm3ds_boot_permille = -1;
    char game_id[5] = { 0 }, region = 0;
    bool is_pm = false;
    if (other_game) {           /* buf still holds the header */
        memcpy(game_id, buf + 0x3B, 4);
        region = (char)buf[0x3E];
        is_pm = !memcmp(buf + 0x3B, "NMQ", 3);
    }
    free(buf);
    st->order = order;
    if (order == ORDER_UNKNOWN) {
        snprintf(st->reason, sizeof st->reason, "Not an N64 ROM");
        st->detail[0] = 0;
        pm3ds_log(PM3DS_LOG_WARN, "rom: %s does not start with an N64 header", path);
        return false;
    }
#if VERSION_IQUE
    if (other_game && is_pm && (region == 'E' || region == 'P')) {   /* the other app's ROM */
        extern char __pm3ds_copy_area[] __attribute__((weak));
        snprintf(st->reason, sizeof st->reason, "This is not the iQue ROM");
        snprintf(st->detail, sizeof st->detail, "%s", __pm3ds_copy_area ? "" : "it needs Star Haven (USA/Europe)");
        pm3ds_log(PM3DS_LOG_WARN, "rom: %s is the %s ROM", path, region == 'E' ? "US" : "EU");
        return false;
    }
#endif
    if (other_game) {
        snprintf(st->reason, sizeof st->reason, "This is not the right ROM");
        snprintf(st->detail, sizeof st->detail, "%s game ID %.4s, needs " PM3DS_ROM_GAME_ID,
                 is_pm ? (region == 'E' ? "US version," : region == 'J' ? "Japanese version," : "") : "", game_id);
        pm3ds_log(PM3DS_LOG_WARN, "rom: %s has game ID %.4s, not " PM3DS_ROM_GAME_ID, path, game_id);
        return false;
    }
    if (n != s_want_size) {
        snprintf(st->reason, sizeof st->reason, "Could not read the file");
        snprintf(st->detail, sizeof st->detail, "stopped at %lu KB", (unsigned long)(n >> 10));
        pm3ds_log(PM3DS_LOG_WARN, "rom: read of %s stopped at %zu bytes", path, n);
        return false;
    }
    uint64_t ms = (svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000);
    pm3ds_log(PM3DS_LOG_INFO, "rom: %s read in %llu ms (%.1f MB/s), %s order, CRC32 %08lX", path,
              (unsigned long long)ms, ms ? (double)n / 1048576.0 / ((double)ms / 1000.0) : 0.0,
              order == ORDER_Z64 ? "z64" : order == ORDER_V64 ? "v64" : "n64", (unsigned long)crc);
    if (s_want_crc && crc != s_want_crc) {
        snprintf(st->reason, sizeof st->reason, "This is not the right ROM");
        snprintf(st->detail, sizeof st->detail, "CRC32 %08lX, needs %08lX", (unsigned long)crc,
                 (unsigned long)s_want_crc);
        return false;
    }
    return true;
}

static bool has_rom_ext(const char *name) {
    size_t l = strlen(name);
    if (l < 4 || name[l - 4] != '.') return false;
    char e[4] = { (char)tolower((unsigned char)name[l - 3]), (char)tolower((unsigned char)name[l - 2]),
                  (char)tolower((unsigned char)name[l - 1]), 0 };
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64");
}

/* the file starts with Paper Mario's header (any version, any byte order) */
static bool is_paper_mario(const char *path) {
    uint32_t h[0x40 / 4];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    int order = n == sizeof h ? detect_order((const uint8_t *)h) : ORDER_UNKNOWN;
    if (order == ORDER_UNKNOWN) return false;
    to_z64((uint8_t *)h, sizeof h, order);
#if VERSION_IQUE
    /* the iQue dump leaves the game ID blank; its CRC is checked after (load_candidate) */
    if (!memcmp((const uint8_t *)h + 0x3B, "\0\0\0\0", 4)) return true;
#endif
    return !memcmp((const uint8_t *)h + 0x3B, "NMQ", 3);
}

static bool find_rom(rom_status_t *st) {
    static const char *const fixed[] = {
        ROM_DIR "/baserom.z64", ROM_DIR "/baserom.n64", ROM_DIR "/baserom.v64",
    };
    static const char *const dirs[] = { PM3DS_ROM_DIRS };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (load_candidate(fixed[i], st)) return true;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        size_t l = strlen(dirs[i]);
        const char *sep = l && dirs[i][l - 1] == '/' ? "" : "/";
        struct dirent *e;
        char path[300];
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;
            bool rom_name = has_rom_ext(e->d_name);
            if (i == 0 && rom_name && !strncmp(e->d_name, "baserom.", 8)) continue;   /* tried above */
            snprintf(path, sizeof path, "%s%s%s", dirs[i], sep, e->d_name);
            /* a .z64/.n64/.v64 in our folder is checked whatever it holds, so a bad
             * dump says why on screen; anything else only if it is Paper Mario */
            if (!(i == 0 && rom_name) && !is_paper_mario(path)) continue;
            if (load_candidate(path, st)) {
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }
    return load_candidate("romfs:/baserom.z64", st);
}

#if defined(PM3DS_NETLOG_IP) && defined(PM3DS_NETLOG_PORT)
/* Dev builds: fetch the ROM once from the PC that receives the Wi-Fi log
 * (its dev server serves the developer's own dump) and keep it on the SD. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static bool dev_download(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return false;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PM3DS_NETLOG_PORT);
    a.sin_addr.s_addr = htonl(PM3DS_NETLOG_IP);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return false;
    }
    static const char req[] = "GET /baserom.z64 HTTP/1.0\r\nHost: pm3ds\r\n\r\n";
    send(fd, req, sizeof req - 1, 0);
    static char buf[64 * 1024];
    /* status line and headers */
    size_t have = 0;
    char *body = NULL;
    while (!body && have < sizeof buf - 1) {
        ssize_t k = recv(fd, buf + have, sizeof buf - 1 - have, 0);
        if (k <= 0) break;
        have += (size_t)k;
        buf[have] = 0;
        body = strstr(buf, "\r\n\r\n");
    }
    if (!body || strncmp(buf, "HTTP/1.", 7) || strncmp(buf + 9, "200", 3)) {
        pm3ds_log(PM3DS_LOG_WARN, "rom: the dev server has no ROM to download");
        close(fd);
        return false;
    }
    size_t total = 0;
    {
        const char *cl = strstr(buf, "Content-Length:");
        if (cl && cl < body) total = (size_t)strtoul(cl + 15, NULL, 10);
    }
    body += 4;
    mkdir("sdmc:/3ds", 0777);
    mkdir(ROM_DIR, 0777);
    FILE *f = fopen(ROM_DIR "/baserom.part", "wb");
    if (!f) {
        close(fd);
        return false;
    }
    pm3ds_bottom_screen_loading_label("Copying the ROM from your PC");
    size_t got = have - (size_t)(body - buf);
    fwrite(body, 1, got, f);
    for (;;) {
        ssize_t k = recv(fd, buf, sizeof buf, 0);
        if (k <= 0) break;
        fwrite(buf, 1, (size_t)k, f);
        got += (size_t)k;
        pm3ds_boot_permille = (int)((uint64_t)got * 1000 / (total ? total : ROM_MAX_FILE));
        pm3ds_gfx_present_loading(pm3ds_boot_permille > 1000 ? 1000 : pm3ds_boot_permille);
    }
    fclose(f);
    close(fd);
    pm3ds_boot_permille = -1;
    pm3ds_bottom_screen_loading_label(NULL);
    if ((total && got != total) || got < s_want_size || got > ROM_MAX_FILE) {
        pm3ds_log(PM3DS_LOG_WARN, "rom: download stopped at %zu bytes", got);
        remove(ROM_DIR "/baserom.part");
        return false;
    }
    remove(ROM_DIR "/baserom.z64");
    rename(ROM_DIR "/baserom.part", ROM_DIR "/baserom.z64");
    pm3ds_log(PM3DS_LOG_INFO, "rom: copied %zu bytes from the dev server", got);
    return true;
}
#endif

/* ---- error screen --------------------------------------------------------- */
static void show_problem(const rom_status_t *st) {
    char l_reason[48], l_detail[48];
    snprintf(l_reason, sizeof l_reason, "%s", st->reason[0] ? st->reason : "No ROM found");
    snprintf(l_detail, sizeof l_detail, "%s", st->detail);
    /* the package runs whichever version the ROM is (universal/starter.c):
     * it has a copy area (universal/universal.ld), a one-version build hasn't */
    extern char __pm3ds_copy_area[] __attribute__((weak));
    const char *lines[] = {
        l_reason,
        l_detail,
        "",
        "Put your own Paper Mario ROM",
        __pm3ds_copy_area ? "(USA, Europe or iQue) on the SD card" :
#if VERSION_IQUE
        "(the iQue Player's) on the SD card",
#else
        "(USA or Europe) on the SD card",
#endif
        "in " PM3DS_SD_DIR_SHOWN ", any file name",
        "(.z64, .n64 or .v64)",
        "",
        "Press START to exit",
    };
    pm3ds_log(PM3DS_LOG_WARN, "rom: %s %s (%s)", l_reason, l_detail, st->path);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        pm3ds_gfx_present_message(lines, (int)(sizeof lines / sizeof lines[0]));
    }
}

/* ---- the data the build left out ----------------------------------------- */
static void read_table(void) {
    FILE *f = fopen(PM3DS_ROMDATA_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz >= (long)sizeof(romdata_header_t)) {
        s_table = (uint8_t *)malloc((size_t)sz);
        if (s_table && fread(s_table, 1, (size_t)sz, f) == (size_t)sz) {
            s_table_size = (size_t)sz;
        } else {
            free(s_table);
            s_table = NULL;
        }
    }
    fclose(f);
    if (!s_table) return;
    const romdata_header_t *h = (const romdata_header_t *)s_table;
    size_t need = sizeof *h + (size_t)h->n_blobs * sizeof(romdata_blob_t) +
                  (size_t)h->n_ptrs * sizeof(romdata_patch_t);
    if (memcmp(h->magic, "PMRD", 4) || h->version != 1 || need > s_table_size) {
        pm3ds_log(PM3DS_LOG_ERROR, "rom: " PM3DS_ROMDATA_PATH " is damaged or from another version");
        free(s_table);
        s_table = NULL;
        return;
    }
    s_want_size = h->rom_size;
    s_want_crc = h->rom_crc32;
}

static bool zero_range(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

/* Putting the data back rides along with the boot-time ROM check: each
 * chunk read for the CRC also fills the objects it covers (prepare_restore
 * sorts them by ROM offset), and finish_restore converts them to native order
 * and checks them once the file is known to be the right ROM. A rejected
 * candidate only leaves bytes the accepted one overwrites. */
static const romdata_blob_t *s_sort_blobs;
static uint32_t *s_order, *s_kept, s_next, s_delta;
static bool s_restoring;

static int by_rom(const void *a, const void *b) {
    uint32_t x = s_sort_blobs[*(const uint32_t *)a].rom, y = s_sort_blobs[*(const uint32_t *)b].rom;
    return x < y ? -1 : x > y;
}

static void restore_abort(void) {
    const romdata_header_t *h = (const romdata_header_t *)s_table;
    const romdata_patch_t *p = (const romdata_patch_t *)((const romdata_blob_t *)(h + 1) + h->n_blobs);
    if (s_kept)
        for (uint32_t i = 0; i < h->n_ptrs; i++) memcpy((void *)(uintptr_t)(p[i].dst + s_delta), &s_kept[i], 4);
    free(s_kept);
    free(s_order);
    s_kept = s_order = NULL;
    s_restoring = false;
}

/* check the table against this program before anything is written */
static bool prepare_restore(void) {
    extern char __exidx_end[], __bss_start__[];
    /* the combined program (universal/universal.ld) keeps each game copy's
     * data in one shared area past .bss instead */
    extern char __pm3ds_copy_area[] __attribute__((weak)), __pm3ds_copy_area_end[] __attribute__((weak));
    const romdata_header_t *h = (const romdata_header_t *)s_table;
    const romdata_blob_t *b = (const romdata_blob_t *)(h + 1);
    const romdata_patch_t *p = (const romdata_patch_t *)(b + h->n_blobs);
    uintptr_t lo = (uintptr_t)__exidx_end, hi = (uintptr_t)__bss_start__;
    if (__pm3ds_copy_area && __pm3ds_copy_area_end > __pm3ds_copy_area) {
        lo = (uintptr_t)__pm3ds_copy_area;
        hi = (uintptr_t)__pm3ds_copy_area_end;
    }
    s_delta = (uint32_t)(uintptr_t)&pm3ds_romdata_anchor - h->anchor;
    /* The display lists' address words stayed in the program (the loader
     * relocates them): set them aside while the rest is checked and put
     * back, then return them. */
    s_kept = (uint32_t *)malloc((h->n_ptrs + 1) * sizeof(uint32_t));
    s_order = (uint32_t *)malloc((h->n_blobs + 1) * sizeof(uint32_t));
    if (!s_kept || !s_order) {
        free(s_kept);
        free(s_order);
        s_kept = s_order = NULL;
        return false;
    }
    for (uint32_t i = 0; i < h->n_ptrs; i++) {
        uintptr_t d = p[i].dst + s_delta;
        if (d < lo || d + 4 > hi) {
            pm3ds_log(PM3DS_LOG_ERROR, "rom: romdata.bin does not fit this program (address word %lu)",
                      (unsigned long)i);
            for (uint32_t k = 0; k < i; k++) memcpy((void *)(uintptr_t)(p[k].dst + s_delta), &s_kept[k], 4);
            free(s_kept);
            free(s_order);
            s_kept = s_order = NULL;
            return false;
        }
        memcpy(&s_kept[i], (const void *)d, 4);
        memset((void *)d, 0, 4);
    }
    /* check everything before changing anything: a table from another
     * build would scribble over live data */
    bool fits = true;
    for (uint32_t i = 0; i < h->n_ptrs && fits; i++) fits = s_kept[i] == p[i].value + s_delta;
    for (uint32_t i = 0; i < h->n_blobs && fits; i++) {
        uintptr_t d = b[i].dst + s_delta;
        fits = d >= lo && d + b[i].size <= hi && b[i].rom + b[i].size <= h->rom_size && b[i].kind <= 2 &&
               zero_range((const uint8_t *)d, b[i].size);
        if (!fits)
            pm3ds_log(PM3DS_LOG_ERROR, "rom: romdata.bin does not fit this program (object %lu at %08lX)",
                      (unsigned long)i, (unsigned long)d);
    }
    s_restoring = true;
    if (!fits) {
        restore_abort();
        return false;
    }
    for (uint32_t i = 0; i < h->n_blobs; i++) s_order[i] = i;
    s_sort_blobs = b;
    qsort(s_order, h->n_blobs, sizeof *s_order, by_rom);
    return true;
}

/* the ROM bytes [base, base+len) in .z64 order, during a candidate's check */
static void restore_chunk(uint32_t base, const uint8_t *buf, uint32_t len) {
    if (!s_restoring) return;
    const romdata_header_t *h = (const romdata_header_t *)s_table;
    const romdata_blob_t *b = (const romdata_blob_t *)(h + 1);
    if (base == 0) s_next = 0;   /* a new candidate */
    uint32_t end = base + len;
    for (uint32_t j = s_next; j < h->n_blobs && b[s_order[j]].rom < end; j++) {
        const romdata_blob_t *o = &b[s_order[j]];
        uint32_t a0 = o->rom > base ? o->rom : base;
        uint32_t a1 = o->rom + o->size < end ? o->rom + o->size : end;
        if (a0 < a1) memcpy((uint8_t *)(uintptr_t)(o->dst + s_delta) + (a0 - o->rom), buf + (a0 - base), a1 - a0);
    }
    while (s_next < h->n_blobs && b[s_order[s_next]].rom + b[s_order[s_next]].size <= end) s_next++;
}

/* the accepted ROM's bytes are in place: native order, then the check */
static bool finish_restore(void) {
    const romdata_header_t *h = (const romdata_header_t *)s_table;
    const romdata_blob_t *b = (const romdata_blob_t *)(h + 1);
    const romdata_patch_t *p = (const romdata_patch_t *)(b + h->n_blobs);
    uint64_t t0 = svcGetSystemTick();
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < h->n_blobs; i++) {
        uint8_t *d = (uint8_t *)(uintptr_t)(b[i].dst + s_delta);
        uint32_t n = b[i].size;
        if (b[i].kind == 1) {                 /* Vtx: 6 big-endian shorts, 4 colour bytes */
            for (uint32_t v = 0; v + 16 <= n; v += 16)
                for (int k = 0; k < 12; k += 2) {
                    uint8_t t = d[v + k];
                    d[v + k] = d[v + k + 1];
                    d[v + k + 1] = t;
                }
        } else if (b[i].kind == 2) {          /* Gfx: big-endian words */
            for (uint32_t w = 0; w + 4 <= n; w += 4) {
                uint32_t x;
                memcpy(&x, d + w, 4);
                x = __builtin_bswap32(x);
                memcpy(d + w, &x, 4);
            }
        }
        bytes += n;
    }
    /* the same bytes as the build that had them compiled in? (address words
     * at their link-time values, as in the ELF the CRC was taken from) */
    for (uint32_t i = 0; i < h->n_ptrs; i++)
        memcpy((void *)(uintptr_t)(p[i].dst + s_delta), &p[i].value, 4);
    uint32_t crc = 0;
    for (uint32_t i = 0; i < h->n_blobs; i++)
        crc = crc_update(crc, (const uint8_t *)(uintptr_t)(b[i].dst + s_delta), b[i].size);
    restore_abort();                                    /* the address words, as loaded */
    pm3ds_log(crc == h->data_crc32 ? PM3DS_LOG_INFO : PM3DS_LOG_ERROR,
              "rom: put back %lu objects (%lu KB) and %lu addresses in %llu ms, CRC32 %08lX (%s)",
              (unsigned long)h->n_blobs, (unsigned long)(bytes >> 10), (unsigned long)h->n_ptrs,
              (unsigned long long)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000)), (unsigned long)crc,
              crc == h->data_crc32 ? "as built" : "MISMATCH");
    return crc == h->data_crc32;
}

/* Load and check the ROM and restore the game data. False: the player chose
 * to exit from the error screen (or the data could not be restored). */
bool pm3ds_rom_init(void) {
    crc_init();
    read_table();
    rom_status_t st;
    memset(&st, 0, sizeof st);
    if (s_table && !prepare_restore()) {
        snprintf(st.reason, sizeof st.reason, "Damaged install");
        snprintf(st.detail, sizeof st.detail, "reinstall the game");
        show_problem(&st);
        return false;
    }
    bool ok = find_rom(&st);
#if defined(PM3DS_NETLOG_IP) && defined(PM3DS_NETLOG_PORT)
    if (!ok && !st.path[0] && dev_download()) ok = find_rom(&st);
#endif
    if (ok && !pm3ds_rom_stream_open(st.path, st.order, s_want_size)) {
        snprintf(st.reason, sizeof st.reason, "Could not open the ROM");
        snprintf(st.detail, sizeof st.detail, "not enough memory?");
        ok = false;
    }
    if (!ok) {
        if (s_restoring) restore_abort();
        show_problem(&st);
        return false;
    }
    if (!s_table && pm3ds_romdata_state == ROMDATA_STRIPPED) {
        pm3ds_log(PM3DS_LOG_ERROR, "rom: this build needs " PM3DS_ROMDATA_PATH);
        snprintf(st.reason, sizeof st.reason, "Damaged install");
        snprintf(st.detail, sizeof st.detail, "reinstall the game");
        show_problem(&st);
        return false;
    }
    if (s_table) {
        if (!finish_restore()) {
            snprintf(st.reason, sizeof st.reason, "Damaged install");
            snprintf(st.detail, sizeof st.detail, "reinstall the game");
            show_problem(&st);
            return false;
        }
        free(s_table);
        s_table = NULL;
        pm3ds_bottom_screen_reload_art();
    }
    pm3ds_pi_rom_ready();
    pm3ds_log(PM3DS_LOG_INFO, "rom: %lu bytes, read from the SD card on demand", (unsigned long)s_want_size);
    return true;
}
