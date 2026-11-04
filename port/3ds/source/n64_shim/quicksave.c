/*
 * quicksave.c — a quick save (the bottom screen's Save) loads where Mario stood.
 *
 * A save block's map puts Mario back on the save block (EnterSavePoint reads
 * the saved position). Other maps replay the entrance he came in by, which is
 * wrong for some: a map entered swimming on Sushie put him on top of the water
 * without her (GitHub #37). So a quick save marks itself in the save's unused
 * tail (SaveData.unk_1304 + 0x20, after rando.c's and cheats.c's blocks), and
 * once the loaded map's entrance is over and Mario can move, he is put back on
 * the spot he saved on. That spot is solid ground: quick saves are refused
 * while riding a partner (src/world/menus.c).
 */
#include "common.h"
#include "game_modes.h"
#include "pm3ds_settings.h"

#include <string.h>

#define QS_MAGIC 0x53485153     /* "SHQS" */
#define QS_BLOCK_OFS 0x20

typedef struct {
    s32 magic;
    s32 version;
    s32 quick;
} QuickSaveBlock;

bool pm3ds_qs_saving;            /* set around fio_save_game by the quick save */
static bool sRestore;
static s16 sArea, sMap;

/* src/fio.c */
/* Autosave (Settings > Autosave, Discord suggestion): a quick save by itself, in each new
 * area or every few minutes of play, whenever the Save button could save (src/world/menus.c).
 * Any save or load counts as the last one. */
static u32 sLastSaveFrame;
static s16 sLastSaveArea = -1, sLastSaveMap = -1;
volatile int pm3ds_autosave_flash;   /* platform/bottom_screen.c shows "Saved!" */

static void autosave_mark(void) {
    sLastSaveFrame = gPlayerData.frameCounter;
    sLastSaveArea = gGameStatusPtr->areaID;
    sLastSaveMap = gGameStatusPtr->mapID;
}

bool pm3ds_autosave_due(void) {
    static const u32 minutes[PM3DS_AUTOSAVE_COUNT] = { 0, 0, 5, 10, 15 };
    switch (pm3ds_cfg.autosave) {
        case PM3DS_AUTOSAVE_AREA:
            return gGameStatusPtr->areaID != sLastSaveArea || gGameStatusPtr->mapID != sLastSaveMap;
        case PM3DS_AUTOSAVE_5:
        case PM3DS_AUTOSAVE_10:
        case PM3DS_AUTOSAVE_15:
            return gPlayerData.frameCounter - sLastSaveFrame >= minutes[pm3ds_cfg.autosave] * 60 * 60;
        default:
            return false;
    }
}

void pm3ds_autosave_done(void) {
    pm3ds_autosave_flash = 1;
}

void pm3ds_qs_serialize(void* tail) {
    autosave_mark();
    QuickSaveBlock b = { QS_MAGIC, 1, pm3ds_qs_saving };
    memcpy((u8*)tail + QS_BLOCK_OFS, &b, sizeof(b));
}

void pm3ds_qs_deserialize(const void* tail) {
    QuickSaveBlock b;
    memcpy(&b, (const u8*)tail + QS_BLOCK_OFS, sizeof(b));
    sRestore = b.magic == QS_MAGIC && b.version == 1 && b.quick;
    autosave_mark();
    sArea = gGameStatusPtr->areaID;
    sMap = gGameStatusPtr->mapID;
}

/* every game step (src/main_loop.c) */
void pm3ds_qs_step(void) {
    PlayerStatus* ps = &gPlayerStatus;
#ifdef PM3DS_TEST_MEMSTAT
    { extern void pm3ds_test_memstat(void); pm3ds_test_memstat(); }
#endif

    if (!sRestore || gGameStatusPtr == nullptr) {
        return;
    }
    if (gGameStatusPtr->areaID != sArea || gGameStatusPtr->mapID != sMap) {
        sRestore = false;        /* left the map first (or another file): nothing to do */
        return;
    }
    if (get_game_mode() != GAME_MODE_WORLD || ps->inputDisabledCount != 0 || (ps->flags & PS_FLAG_INPUT_DISABLED)) {
        return;                  /* the entrance is still playing */
    }
    sRestore = false;
    f32 dx = ps->pos.x - gGameStatusPtr->savedPos.x;
    f32 dz = ps->pos.z - gGameStatusPtr->savedPos.z;
    f32 dy = ps->pos.y - gGameStatusPtr->savedPos.y;
    if (dx * dx + dz * dz + dy * dy < 30.0f * 30.0f) {
        return;                  /* a save block's map already put him there */
    }
    ps->pos.x = gGameStatusPtr->savedPos.x;
    ps->pos.y = gGameStatusPtr->savedPos.y;
    ps->pos.z = gGameStatusPtr->savedPos.z;
    {
        Npc* partner = get_npc_safe(NPC_PARTNER);
        if (partner != nullptr) {
            partner->pos.x = ps->pos.x;
            partner->pos.y = ps->pos.y;
            partner->pos.z = ps->pos.z;
        }
    }
    // the camera jumps along, as after a pipe or door, instead of gliding over from the
    // entrance (Discord: reloading a save by the Windy Mill). Its target follows Mario only
    // in the player's physics, later in the step than the camera can update, so a reinit
    // alone settled on the entrance and the camera still glided over (still so in 1.3.5):
    // the target moves with him here.
    gCameras[CAM_DEFAULT].targetPos = ps->pos;
    gCameras[CAM_DEFAULT].needsReinit = true;
    {
        extern void pm3ds_log(int, const char*, ...);
        pm3ds_log(2, "quick save: Mario put back at %d %d %d", (int)ps->pos.x, (int)ps->pos.y, (int)ps->pos.z);
    }
}

/* src/filemenu/filemenu_yesno.c: a new file (the file select read every slot, the last one's mark lingers) */
void pm3ds_qs_new_game(void) {
    sRestore = false;
}

/* platform/bottom_screen.c, Quit game (GitHub #67): the game's soft reset (src/main_loop.c), as the
 * N64 reset button started it: the screen fades out, then startup clears the state for the title */
void pm3ds_quit_to_title(void) {
    sfx_play_sound(SOUND_MENU_NEXT);
    gOverrideFlags |= GLOBAL_OVERRIDES_SOFT_RESET;
}

/* platform/pm3ds_settings.c, pm3ds_fast_forward: places where fast-forward pauses itself.
 * The Lava Piranha's entrance (kzn_19) softlocked on consoles when it started with
 * fast-forward on (GitHub #66, Discord; never in Azahar), so it runs at normal speed
 * there until the boss is beaten. */
bool pm3ds_ff_paused_here(void) {
    MapConfig* get_current_map_config(void);
    if (gGameStatusPtr == nullptr || gGameStatusPtr->areaID != AREA_KZN) {
        return false;
    }
    MapConfig* mc = get_current_map_config();
    return mc != nullptr && strcmp(mc->id, "kzn_19") == 0 &&
           evt_get_variable(nullptr, GB_StoryProgress) < STORY_CH5_DEFEATED_LAVA_PIRANHA;
}

/* src/world/area_kzn/kzn_19/npc.c: how far the Lava Piranha's entrance got, into log.txt,
 * so a stall there on a console shows where (GitHub #66) */
API_CALLABLE(pm3ds_LogStep) {
    extern void pm3ds_log(int, const char*, ...);
    pm3ds_log(3, "kzn_19 boss entrance: step %d (frame %d, ff %d)", (int)evt_get_variable(script, *script->ptrReadPos),
              (int)gGameStatusPtr->frameCounter, (int)pm3ds_fast_forward_raw());
    return ApiStatus_DONE2;
}
