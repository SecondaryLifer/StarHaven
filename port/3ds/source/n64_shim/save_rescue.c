/*
 * save_rescue.c — Settings > Restore an older save.
 *
 * The game keeps 6 save pages for its 4 files (src/fio.c): each save goes to the
 * oldest page no file is using, so a file's previous copy stays on the card until
 * two more saves have been made. os_flash.c also keeps dated copies of the whole
 * save from the last boots in <SD folder>/backup. This lists both (a file's name,
 * level and time played, and where the copy is) and writes the chosen one back as
 * the newest save of its file, the way port/3ds/local/scripts/save_rescue.py does
 * on a PC. Nothing is lost by it: the save is backed up first, and a copy from the
 * save itself only changes places with the file's current one, which then shows up
 * here as an older copy.
 */
#include "common.h"
#include "fio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_PAGES       6        /* save pages; 6 and 7 hold the globals */
#define PAGE_STRIDE     0x4000
#define RESCUE_MAX      24

extern void pm3ds_log(int, const char*, ...);
extern s32 fio_calc_file_checksum(SaveData* saveData);
extern bool fio_validate_file_checksum(SaveData* saveData);
extern bool fio_fetch_saved_file_info(void);
/* n64_shim/os_flash.c */
extern int pm3ds_backup_count(void);
extern const char* pm3ds_backup_name(int i);
extern int pm3ds_backup_read(int i, u8* img);
extern int pm3ds_backup_now(void);
extern const u8* pm3ds_flash_image(void);
extern void pm3ds_flash_write_block(int page, const void* block, u32 len);
/* n64_shim/filemenu_touch.c */
extern void pm3ds_fm_name_of(char out[40], const u8* name);

typedef struct {
    s8 backup;        /* -1: the save itself, else the backup's index (newest first) */
    u8 page;
    s8 slot;
    s32 count;
    s32 level;
    s32 timePlayed;
    u8 name[8];
} RescueEntry;

static RescueEntry sList[RESCUE_MAX];
static s32 sCount;
static SaveData sBlock;   /* one save page, aligned for the checksum's words */
static SaveData sCopy;

/* page p of img as a valid save, in sBlock */
static bool read_block(const u8* img, s32 p) {
    memcpy(&sBlock, img + p * PAGE_STRIDE, sizeof(SaveData));
    return fio_validate_file_checksum(&sBlock) && sBlock.saveSlot >= 0 && sBlock.saveSlot < 4;
}

/* each file's newest page in img (-1: none) and the highest save count */
static s32 newest_pages(const u8* img, s32 newest[4], s32 counts[NUM_PAGES]) {
    s32 top = 0;
    s32 i;

    for (i = 0; i < 4; i++) {
        newest[i] = -1;
    }
    for (i = 0; i < NUM_PAGES; i++) {
        counts[i] = -1;
        if (!read_block(img, i)) {
            continue;
        }
        counts[i] = sBlock.saveCount;
        if (newest[sBlock.saveSlot] < 0 || sBlock.saveCount > counts[newest[sBlock.saveSlot]]) {
            newest[sBlock.saveSlot] = i;
        }
        if (sBlock.saveCount > top) {
            top = sBlock.saveCount;
        }
    }
    return top;
}

static void add(s8 backup, s32 page) {
    RescueEntry* e;

    if (sCount >= RESCUE_MAX) {
        return;
    }
    e = &sList[sCount++];
    e->backup = backup;
    e->page = page;
    e->slot = sBlock.saveSlot;
    e->count = sBlock.saveCount;
    e->level = sBlock.metadata.level;
    e->timePlayed = sBlock.metadata.timePlayed;
    memcpy(e->name, sBlock.metadata.filename, sizeof(e->name));
}

/* the same save is already in the list or on the card */
static bool known(s32 slot, s32 count) {
    s32 i;

    for (i = 0; i < sCount; i++) {
        if (sList[i].slot == slot && sList[i].count == count) {
            return true;
        }
    }
    return false;
}

/* what can be restored: the save's older copies, then each backup's files that the
 * save doesn't already hold. Returns how many */
s32 pm3ds_rescue_scan(void) {
    const u8* img = pm3ds_flash_image();
    s32 newest[4], counts[NUM_PAGES], cur[NUM_PAGES], curSlot[NUM_PAGES];
    s32 i, b, n;

    sCount = 0;
    newest_pages(img, newest, counts);
    for (i = 0; i < NUM_PAGES; i++) {
        cur[i] = counts[i];
        curSlot[i] = -1;
        if (counts[i] < 0 || !read_block(img, i)) {
            continue;
        }
        curSlot[i] = sBlock.saveSlot;
        if (newest[sBlock.saveSlot] != i) {
            add(-1, i);
        }
    }
    n = pm3ds_backup_count();
    for (b = 0; b < n; b++) {
        s32 bnewest[4], bcounts[NUM_PAGES];
        u8* full = malloc(0x20000);
        if (full == nullptr) {
            break;
        }
        if (!pm3ds_backup_read(b, full)) {
            free(full);
            continue;
        }
        newest_pages(full, bnewest, bcounts);
        for (i = 0; i < 4; i++) {
            s32 k;
            bool onCard = false;
            if (bnewest[i] < 0 || !read_block(full, bnewest[i])) {
                continue;
            }
            for (k = 0; k < NUM_PAGES; k++) {
                if (curSlot[k] == sBlock.saveSlot && cur[k] == sBlock.saveCount) {
                    onCard = true;
                }
            }
            if (!onCard && !known(sBlock.saveSlot, sBlock.saveCount)) {
                add(b, bnewest[i]);
            }
        }
        free(full);
    }
    return sCount;
}

/* entry i for the list: its file (1-4), name (UI text), level, minutes played and,
 * for a backup, when it was made ("" for an older copy in the save) */
bool pm3ds_rescue_describe(s32 i, s32* file, char name[40], s32* level, s32* minutes, char* when, s32 size) {
    RescueEntry* e;

    if (i < 0 || i >= sCount) {
        return false;
    }
    e = &sList[i];
    *file = e->slot + 1;
    pm3ds_fm_name_of(name, e->name);
    *level = e->level;
    *minutes = e->timePlayed / 3600;   /* frames: 60 a second */
    when[0] = '\0';
    if (e->backup >= 0) {
        const char* bn = pm3ds_backup_name(e->backup);
        const char* d = strrchr(bn, '_');   /* <save>_YYYYMMDD-HHMMSS.fla */
        if (d != nullptr && strlen(d) >= 16) {
            snprintf(when, size, "%.4s-%.2s-%.2s %.2s:%.2s", d + 1, d + 5, d + 7, d + 10, d + 12);
        } else {
            snprintf(when, size, "%s", bn);
        }
    }
    return true;
}

/* entry i becomes the newest save of its file. 1: done, 0: it couldn't be */
s32 pm3ds_rescue_restore(s32 i) {
    const u8* img;
    RescueEntry e;
    s32 newest[4], counts[NUM_PAGES];
    s32 top, page, k;

    if (i < 0 || i >= sCount) {
        return 0;
    }
    e = sList[i];
    if (e.backup >= 0) {
        u8* full = malloc(0x20000);
        bool ok = full != nullptr && pm3ds_backup_read(e.backup, full) && read_block(full, e.page);
        free(full);
        if (!ok) {
            return 0;
        }
    } else if (!read_block(pm3ds_flash_image(), e.page)) {
        return 0;
    }
    /* the page must still hold the copy that was listed: the game may have saved
     * (autosave) onto it since the list was made, and a backup may have been added */
    if (sBlock.saveSlot != e.slot || sBlock.saveCount != e.count) {
        pm3ds_log(2, "rescue: page %d changed since it was listed, nothing restored", (int)e.page);
        return 0;
    }
    /* sBlock holds the copy; first a backup of the save as it is now */
    sCopy = sBlock;
    if (!pm3ds_backup_now()) {
        pm3ds_log(2, "rescue: no backup could be written, nothing restored");
        return 0;
    }
    img = pm3ds_flash_image();
    top = newest_pages(img, newest, counts);
    sBlock = sCopy;
    if (e.backup < 0) {
        page = e.page;   /* in place: the file's current copy becomes an older one */
    } else {
        /* where the game would save next: a page no file uses, an empty one first */
        page = -1;
        for (k = 0; k < NUM_PAGES; k++) {
            if (k == newest[0] || k == newest[1] || k == newest[2] || k == newest[3]) {
                continue;
            }
            if (page < 0 || counts[k] < counts[page]) {
                page = k;
            }
        }
        if (page < 0) {
            return 0;
        }
    }
    if (gGameStatusPtr->saveCount > top) {
        top = gGameStatusPtr->saveCount;
    }
    sBlock.saveCount = top + 1;
    sBlock.crc1 = 0;
    sBlock.crc2 = ~sBlock.crc1;
    sBlock.crc1 = fio_calc_file_checksum(&sBlock);
    sBlock.crc2 = ~sBlock.crc1;
    pm3ds_flash_write_block(page, &sBlock, sizeof(SaveData));
    gGameStatusPtr->saveCount = sBlock.saveCount;   /* the next save counts on from it */
    fio_fetch_saved_file_info();
    gSaveSlotMetadata[sBlock.saveSlot] = sBlock.metadata;   /* the file screen shows it */
    gSaveSlotHasData[sBlock.saveSlot] = true;
    pm3ds_log(2, "rescue: %s page %d -> page %d as file %d, count %d", e.backup < 0 ? "older copy" : "backup",
              (int)e.page, (int)page, sBlock.saveSlot + 1, sBlock.saveCount);
    pm3ds_rescue_scan();
    return 1;
}
