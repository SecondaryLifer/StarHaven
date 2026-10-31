/*
 * jukebox.c — Settings > Sound (GitHub #78): Music and Sound effects switches, and a
 * jukebox whose song replaces what the world asks for (src/audio/bgm_control.c).
 *
 * The bottom screen only changes port.cfg's values and sets a flag: the audio engine's
 * calls are made from a game step (pm3ds_sound_step, src/main_loop.c), where the game makes
 * its own.
 */
#include "common.h"
#include "audio/public.h"
#include "game_modes.h"
#include "pm3ds_settings.h"

static const struct { s16 id; const char *name; } sSongs[] = {
#include "jukebox_songs.inc"
};
#define NUM_SONGS (s32)ARRAY_COUNT(sSongs)

static bool sDirty = true;   /* the first step applies port.cfg */
static bool sSongDirty;

extern void pm3ds_bgm_update_bus(void);
extern void pm3ds_bgm_jukebox_changed(void);

/* src/audio/core/engine.c, src/audio/bgm_control.c */
bool pm3ds_music_on(void) { return pm3ds_cfg.music_on; }
bool pm3ds_sfx_on(void) { return pm3ds_cfg.sfx_on; }

s32 pm3ds_jukebox_song(void) {
    s32 i = pm3ds_cfg.jukebox;
    return i >= 0 && i < NUM_SONGS ? sSongs[i].id : -1;
}

/* the bottom screen */
const char *pm3ds_jukebox_name(int i) {
    return i >= 0 && i < NUM_SONGS ? sSongs[i].name : "Off";
}
int pm3ds_jukebox_count(void) { return NUM_SONGS; }
void pm3ds_sound_changed(void) { sDirty = true; }
void pm3ds_jukebox_changed(void) { sSongDirty = true; }

/* src/main_loop.c, every game step */
void pm3ds_sound_step(void) {
    if (sDirty) {
        sDirty = false;
        pm3ds_bgm_update_bus();
    }
    if (sSongDirty && gGameStatusPtr->context == CONTEXT_WORLD && get_game_mode() == GAME_MODE_WORLD) {
        sSongDirty = false;
        pm3ds_bgm_jukebox_changed();
    }
}
