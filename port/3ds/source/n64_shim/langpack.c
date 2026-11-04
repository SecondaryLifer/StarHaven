/*
 * langpack.c — language packs (GitHub #84): a fan translation's message text,
 * read from /3ds/StarHaven/lang/<id>/messages.bin (tools/make_langpack.py has
 * the format) instead of the ROM's. src/msg.c dma_load_msg, through which every
 * message is loaded, asks here first; a message the pack lacks is the ROM's.
 *
 * Packs use the US version's message ids (the decompilation's assets/us/msg).
 * The European ROM numbers two sections differently: it has more minigame
 * messages (section 08) and more credits (2E), so those are mapped back; its
 * other extra messages come after the US ones and stay the ROM's. The Japanese
 * and iQue versions have messages of their own: no packs there.
 *
 * The pack is loaded on the game's thread, before the next message after it was
 * picked, so text being drawn never loses its pack. Its text is kept in memory
 * (about 1 MB for a whole translation); if that much isn't free, messages are
 * read from the SD card one at a time.
 */
#include "common.h"
#include "pm3ds_brand.h"
#include "pm3ds_lang.h"
#include "pm3ds_langpack.h"
#include "pm3ds_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LP_DIR PM3DS_SD_DIR "/lang"
#define LP_FILE "messages.bin"
#define LP_VERSION 1
#define LP_HEADER 64
#define LP_MAX_PACKS 16
#define LP_MAX_MSGS 0x8000

#if !VERSION_JP && !VERSION_IQUE
#define LP_SUPPORTED 1
#else
#define LP_SUPPORTED 0
#endif

typedef struct {
    u32 id, ofs, len;
} LpEntry;

typedef struct {
    char id[PM3DS_LANGPACK_ID];
    char name[64];      /* UI-encoded */
} LpInfo;

static LpInfo s_list[LP_MAX_PACKS];
static int s_count;

static char s_want[PM3DS_LANGPACK_ID];   /* requested */
static char s_have[PM3DS_LANGPACK_ID];   /* loaded (or tried) */
static bool s_dirty;

static LpEntry* s_entries;
static u32 s_n;
static u8* s_text;          /* NULL: read from s_file */
static FILE* s_file;
static u32 s_text_pos, s_text_size;

#if LP_SUPPORTED
extern u8 MessagePlural[];
extern u8 MessageSingular[];
#if VERSION_PAL
extern u8 MessagePlural_de[];
#endif
#define ENDING 8
static u8 s_plural0[ENDING], s_singular0[ENDING];
#if VERSION_PAL
static u8 s_plural_de0[ENDING];
#endif
static bool s_endings_saved;
#endif

#if VERSION_PAL
/* the European message index of sections 08 and 2E -> the US one (-1: not in the US version) */
static const s16 s_pal_08[77] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, -1, 33, 34, -1, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, -1, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    61, 62, 63, 64, 65, 66, -1, 67, 68, 69, 70, 71, 72,
};
static const s16 s_pal_2e[163] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, -1, -1, -1,
    -1, -1, -1, -1, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
    57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,
    73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
    121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1,
};
#endif

static u32 rd32(const u8* p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* the header of a pack file; false if it isn't one */
static bool read_header(FILE* f, u8* h) {
    return fread(h, 1, LP_HEADER, f) == LP_HEADER && memcmp(h, "SHLP", 4) == 0 &&
           (h[4] | h[5] << 8) == LP_VERSION && (h[6] | h[7] << 8) == 0 && rd32(h + 8) <= LP_MAX_MSGS;
}

int pm3ds_langpack_scan(void) {
    s_count = 0;
#if LP_SUPPORTED
    DIR* d = opendir(LP_DIR);
    if (d == nullptr) {
        return 0;
    }
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && s_count < LP_MAX_PACKS) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= PM3DS_LANGPACK_ID || strchr(e->d_name, ' ')) {
            continue;
        }
        char path[128];
        u8 h[LP_HEADER];
        snprintf(path, sizeof path, LP_DIR "/%s/" LP_FILE, e->d_name);
        FILE* f = fopen(path, "rb");
        if (f == nullptr) {
            continue;
        }
        bool ok = read_header(f, h);
        fclose(f);
        if (!ok) {
            continue;
        }
        LpInfo* p = &s_list[s_count++];
        snprintf(p->id, sizeof p->id, "%s", e->d_name);
        char name[33];
        memcpy(name, h + 16, 32);
        name[32] = '\0';
        pm3ds_lang_encode(name[0] ? name : p->id, p->name, sizeof p->name);
    }
    closedir(d);
    /* by id, so the order in Settings stays put */
    for (int i = 1; i < s_count; i++) {
        for (int j = i; j > 0 && strcmp(s_list[j - 1].id, s_list[j].id) > 0; j--) {
            LpInfo t = s_list[j];
            s_list[j] = s_list[j - 1];
            s_list[j - 1] = t;
        }
    }
#endif
    return s_count;
}

const char* pm3ds_langpack_id(int i) {
    return i >= 0 && i < s_count ? s_list[i].id : "";
}

const char* pm3ds_langpack_name(int i) {
    return i >= 0 && i < s_count ? s_list[i].name : "";
}

const char* pm3ds_langpack_current_name(void) {
    if (!s_want[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_list[i].id, s_want)) {
            return s_list[i].name;
        }
    }
    return nullptr;
}

void pm3ds_langpack_request(const char* id) {
    snprintf(s_want, sizeof s_want, "%s", id ? id : "");
    s_dirty = strcmp(s_want, s_have) != 0;
}

static void unload(void) {
    free(s_entries);
    free(s_text);
    if (s_file != nullptr) {
        fclose(s_file);
    }
    s_entries = nullptr;
    s_text = nullptr;
    s_file = nullptr;
    s_n = 0;
#if LP_SUPPORTED
    if (s_endings_saved) {
        memcpy(MessagePlural, s_plural0, ENDING);
        memcpy(MessageSingular, s_singular0, ENDING);
#if VERSION_PAL
        memcpy(MessagePlural_de, s_plural_de0, ENDING);
#endif
    }
#endif
}

#if LP_SUPPORTED
static void set_endings(const u8* h) {
    if (!s_endings_saved) {
        memcpy(s_plural0, MessagePlural, ENDING);
        memcpy(s_singular0, MessageSingular, ENDING);
#if VERSION_PAL
        memcpy(s_plural_de0, MessagePlural_de, ENDING);
#endif
        s_endings_saved = true;
    }
    if (h[48] != 0) {
        memcpy(MessagePlural, h + 48, ENDING);
        MessagePlural[ENDING - 1] = MSG_CHAR_READ_END;
#if VERSION_PAL
        memcpy(MessagePlural_de, MessagePlural, ENDING);   /* the German one is used in German only */
#endif
        if (h[56] != 0) {   /* else the ROM's: nothing after one (written [BR][End]) */
            memcpy(MessageSingular, h + 56, ENDING);
            MessageSingular[ENDING - 1] = MSG_CHAR_READ_END;
        }
    }
}

static bool load(const char* id) {
    char path[128];
    u8 h[LP_HEADER];
    snprintf(path, sizeof path, LP_DIR "/%s/" LP_FILE, id);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        pm3ds_log(PM3DS_LOG_WARN, "language pack %s: no %s", id, path);
        return false;
    }
    if (!read_header(f, h)) {
        pm3ds_log(PM3DS_LOG_WARN, "language pack %s: not a pack (or a newer version)", id);
        fclose(f);
        return false;
    }
    u32 n = rd32(h + 8), size = rd32(h + 12);   /* n <= LP_MAX_MSGS (read_header) */
    long file_len = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        file_len = ftell(f);
    }
    if (file_len < 0 || fseek(f, LP_HEADER, SEEK_SET) != 0 ||
        (unsigned long)file_len < (unsigned long)LP_HEADER + n * 12ul ||
        size != (unsigned long)file_len - LP_HEADER - n * 12ul) {
        pm3ds_log(PM3DS_LOG_WARN, "language pack %s: its size doesn't match its header", id);
        fclose(f);
        return false;
    }
    u8* raw = malloc(n * 12 + 1);
    s_entries = malloc(n * sizeof(LpEntry) + 1);
    if (raw == nullptr || s_entries == nullptr || fread(raw, 12, n, f) != n) {
        pm3ds_log(PM3DS_LOG_WARN, "language pack %s: could not read its index", id);
        free(raw);
        fclose(f);
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        LpEntry* e = &s_entries[i];
        e->id = rd32(raw + i * 12);
        e->ofs = rd32(raw + i * 12 + 4);
        e->len = rd32(raw + i * 12 + 8);
        if ((i > 0 && e->id <= s_entries[i - 1].id) || e->len == 0 || e->ofs > size || e->len > size - e->ofs) {
            pm3ds_log(PM3DS_LOG_WARN, "language pack %s: entry %lu is broken", id, (unsigned long)i);
            free(raw);
            fclose(f);
            return false;
        }
    }
    free(raw);
    s_n = n;
    s_text_pos = LP_HEADER + n * 12;
    s_text_size = size;
    s_text = malloc(size + 1);
    if (s_text != nullptr && fread(s_text, 1, size, f) == size) {
        fclose(f);
    } else {
        free(s_text);
        s_text = nullptr;
        s_file = f;      /* not enough memory: one message at a time */
        pm3ds_log(PM3DS_LOG_INFO, "language pack %s: read from the SD card as needed", id);
    }
    set_endings(h);
    pm3ds_log(PM3DS_LOG_WARN, "language pack %s: %lu messages, %lu bytes", id, (unsigned long)n, (unsigned long)size);
    return true;
}
#endif

bool pm3ds_langpack_msg(uint32_t msgID, void* dest, uint32_t max) {
#if LP_SUPPORTED
    if (s_dirty) {
        s_dirty = false;
        unload();
        snprintf(s_have, sizeof s_have, "%s", s_want);
        if (s_want[0] && !load(s_want)) {
            unload();
        }
    }
    if (s_n == 0 || max == 0) {
        return false;
    }
    u32 sec = msgID >> 16, idx = msgID & 0xFFFF;
#if VERSION_PAL
    if (sec == 0x08 || sec == 0x2E) {
        const s16* map = sec == 0x08 ? s_pal_08 : s_pal_2e;
        u32 count = sec == 0x08 ? ARRAY_COUNT(s_pal_08) : ARRAY_COUNT(s_pal_2e);
        if (idx >= count || map[idx] < 0) {
            return false;
        }
        idx = (u32)map[idx];
    }
#endif
    u32 key = sec << 16 | idx, lo = 0, hi = s_n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (s_entries[mid].id < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= s_n || s_entries[lo].id != key) {
        return false;
    }
    const LpEntry* e = &s_entries[lo];
    u32 len = e->len < max ? e->len : max;
    if (s_text != nullptr) {
        memcpy(dest, s_text + e->ofs, len);
    } else if (fseek(s_file, (long)(s_text_pos + e->ofs), SEEK_SET) != 0 || fread(dest, 1, len, s_file) != len) {
        return false;
    }
    if (((u8*)dest)[len - 1] != MSG_CHAR_READ_END) {
        ((u8*)dest)[len - 1] = MSG_CHAR_READ_END;   /* cut to the buffer */
    }
    return true;
#else
    (void)msgID;
    (void)dest;
    (void)max;
    return false;
#endif
}
