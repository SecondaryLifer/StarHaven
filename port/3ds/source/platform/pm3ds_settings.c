/*
 * pm3ds_settings.c — player options, kept in <SD folder>/port.cfg as
 * key=value lines (unknown keys are ignored, so older and newer builds can
 * share the file). Edited from the bottom screen's Options page.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "pm3ds_brand.h"
#include "pm3ds_rando.h"
#include "pm3ds_settings.h"
#include "pm3ds_langpack.h"
#include "pm3ds_lang.h"

#define CFG_PATH PM3DS_SD_DIR "/port.cfg"

extern bool pm3ds_fps60;                       /* n64_shim/gfx_interp.c */
extern void pm3ds_set_view_mode(int mode);     /* n64_shim/gfx_present.c */

pm3ds_settings_t pm3ds_cfg;
/* Old 3DS: fewer extras (src/entity.c: only Mario's and the partner's
 * shadows; audio_dsp.c: no reverb) */
bool pm3ds_lite_effects;
const float pm3ds_depth3d_shift[DEPTH3D_LEVELS] = { 3.0f, 5.0f, 7.0f, 9.0f, 11.0f };
const char *const pm3ds_depth3d_names[DEPTH3D_LEVELS] = { "Low", "Normal", "Deep", "Deeper", "Deepest" };
bool pm3ds_ff_button_on;

static const uint32_t s_key_bits[KEY_IDX_COUNT] = {
    KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR,
    KEY_START, KEY_SELECT, KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT,
    KEY_CSTICK_UP, KEY_CSTICK_DOWN, KEY_CSTICK_LEFT, KEY_CSTICK_RIGHT,
};
static const char *const s_key_names[KEY_IDX_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
    "START", "SELECT", "D-Up", "D-Down", "D-Left", "D-Right",
    "C-Up", "C-Down", "C-Left", "C-Right",
};
static const char *const s_act_names[ACT_COUNT] = {
    "-", "A", "B", "Z", "R", "L", "Start",
    "C-Up", "C-Down", "C-Left", "C-Right", "Fast >>", "Turbo A",
};
static const char *const s_key_cfg[KEY_IDX_COUNT] = {
    "a", "b", "x", "y", "l", "r", "zl", "zr", "start", "select",
    "dup", "ddown", "dleft", "dright", "cup", "cdown", "cleft", "cright",
};

/* Paper Mario's controls on a 3DS without ZL/ZR/C-stick, plus the extras:
 * N64 C buttons on the D-pad (the game never reads the N64 D-pad), X/Y as
 * partner ability / partner menu, ZR held for fast-forward. */
void pm3ds_settings_default_keymap(uint8_t *m) {
    static const uint8_t def[KEY_IDX_COUNT] = {
        ACT_A, ACT_B, ACT_C_DOWN, ACT_C_LEFT, ACT_Z, ACT_R, ACT_TURBO_A, ACT_FAST_FORWARD,
        ACT_START, ACT_L, ACT_C_UP, ACT_C_DOWN, ACT_C_LEFT, ACT_C_RIGHT,
        ACT_C_UP, ACT_C_DOWN, ACT_C_LEFT, ACT_C_RIGHT,
    };
    memcpy(m, def, sizeof def);
}

uint32_t pm3ds_settings_key_bit(int k) { return k >= 0 && k < KEY_IDX_COUNT ? s_key_bits[k] : 0; }
const char *pm3ds_settings_key_name(int k) { return k >= 0 && k < KEY_IDX_COUNT ? s_key_names[k] : "?"; }
const char *pm3ds_settings_action_name(int a) { return a >= 0 && a < ACT_COUNT ? s_act_names[a] : "?"; }

static void apply(void) {
    pm3ds_set_view_mode(pm3ds_cfg.view_mode);
    pm3ds_fps60 = pm3ds_cfg.fps60;
}

void pm3ds_settings_load(void) {
    pm3ds_cfg.view_mode = 2;   /* Wide */
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
#ifdef PM3DS_SIM_O3DS
    n3ds = false;   /* test build: a New 3DS at Old 3DS speed */
#endif
    pm3ds_lite_effects = !n3ds;
    pm3ds_cfg.fps60 = n3ds;    /* an Old 3DS can't keep up with the in-between frames */
    pm3ds_cfg.depth3d = DEPTH3D_DEFAULT;
    pm3ds_cfg.text_speed = TEXT_NORMAL;
    pm3ds_cfg.ff_speed = 3;
    pm3ds_cfg.ff_toggle = false;
    pm3ds_cfg.rando_flags = PM3DS_RANDO_COINS | PM3DS_RANDO_SHOPS | PM3DS_RANDO_GIFTS;
    pm3ds_cfg.update_check = true;
    pm3ds_cfg.update_skip = 0;
    pm3ds_cfg.map_dragged = false;
    pm3ds_cfg.stat_drops = true;
    pm3ds_cfg.hp_beep = true;
    pm3ds_cfg.battle_hud = true;
    pm3ds_cfg.ice_fx = true;
    pm3ds_cfg.music_on = true;
    pm3ds_cfg.sfx_on = true;
    pm3ds_cfg.jukebox = -1;
    pm3ds_cfg.steer_spin = false;
    pm3ds_cfg.steer_jump = false;
    pm3ds_cfg.dpad_menus = true;
    pm3ds_cfg.language = -1;
    pm3ds_cfg.lang_pack[0] = '\0';
    pm3ds_settings_default_keymap(pm3ds_cfg.keymap);
    FILE *f = fopen(CFG_PATH, "r");
    if (f) {
        char line[80], name[32];
        while (fgets(line, sizeof line, f)) {
            int v;
            if (sscanf(line, "view_mode=%d", &v) == 1) pm3ds_cfg.view_mode = v % 4;
            else if (sscanf(line, "view_fill=%d", &v) == 1) pm3ds_cfg.view_mode = v ? 1 : 0;   /* older builds */
            else if (sscanf(line, "fps60=%d", &v) == 1) pm3ds_cfg.fps60 = v != 0;
            else if (sscanf(line, "depth3d=%d", &v) == 1 && v >= 0 && v < DEPTH3D_LEVELS) pm3ds_cfg.depth3d = v;
            else if (sscanf(line, "text_speed=%d", &v) == 1 && v >= 0 && v < TEXT_SPEEDS) pm3ds_cfg.text_speed = v;
            else if (sscanf(line, "ff_speed=%d", &v) == 1 && v >= 2 && v <= 4) pm3ds_cfg.ff_speed = v;
            else if (sscanf(line, "ff_toggle=%d", &v) == 1) pm3ds_cfg.ff_toggle = v != 0;
            else if (sscanf(line, "update_check=%d", &v) == 1) pm3ds_cfg.update_check = v != 0;
            else if (sscanf(line, "map_dragged=%d", &v) == 1) pm3ds_cfg.map_dragged = v != 0;
            else if (sscanf(line, "stat_drops=%d", &v) == 1) pm3ds_cfg.stat_drops = v != 0;
            else if (sscanf(line, "hp_beep=%d", &v) == 1) pm3ds_cfg.hp_beep = v != 0;
            else if (sscanf(line, "battle_hud=%d", &v) == 1) pm3ds_cfg.battle_hud = v != 0;
            else if (sscanf(line, "ice_fx=%d", &v) == 1) pm3ds_cfg.ice_fx = v != 0;
            else if (sscanf(line, "music_on=%d", &v) == 1) pm3ds_cfg.music_on = v != 0;
            else if (sscanf(line, "sfx_on=%d", &v) == 1) pm3ds_cfg.sfx_on = v != 0;
            else if (sscanf(line, "jukebox=%d", &v) == 1) pm3ds_cfg.jukebox = v;
            else if (sscanf(line, "steer_spin=%d", &v) == 1) pm3ds_cfg.steer_spin = v != 0;
            else if (sscanf(line, "steer_jump=%d", &v) == 1) pm3ds_cfg.steer_jump = v != 0;
            else if (sscanf(line, "dpad_menus=%d", &v) == 1) pm3ds_cfg.dpad_menus = v != 0;
            else if (sscanf(line, "autosave=%d", &v) == 1 && v >= 0 && v < PM3DS_AUTOSAVE_COUNT) pm3ds_cfg.autosave = (uint8_t)v;
            else if (sscanf(line, "color_mode=%d", &v) == 1 && v >= 0 && v < PM3DS_COLOR_COUNT) pm3ds_cfg.color_mode = (uint8_t)v;
            else if (!strncmp(line, "lang_pack=", 10)) {
                snprintf(pm3ds_cfg.lang_pack, sizeof pm3ds_cfg.lang_pack, "%s", line + 10);
                pm3ds_cfg.lang_pack[strcspn(pm3ds_cfg.lang_pack, "\r\n ")] = '\0';
            }
            else if (sscanf(line, "language=%d", &v) == 1 && v >= 0 && v < PM3DS_LANG_COUNT) pm3ds_cfg.language = v;
            else if (sscanf(line, "update_skip=%d", &v) == 1) pm3ds_cfg.update_skip = v;
            else if (sscanf(line, "rando_flags=%d", &v) == 1) pm3ds_cfg.rando_flags = (unsigned)v & PM3DS_RANDO_ALL;
            else if (sscanf(line, "rando=%d", &v) == 1 && v >= 1 && v <= 3) {   /* older builds: a mode */
                static const unsigned modes[4] = { 0, PM3DS_RANDO_ON | PM3DS_RANDO_SHOPS | PM3DS_RANDO_GIFTS,
                    PM3DS_RANDO_ON | PM3DS_RANDO_COINS | PM3DS_RANDO_SHOPS | PM3DS_RANDO_GIFTS, PM3DS_RANDO_ALL_V3 & ~PM3DS_RANDO_HIDDEN };
                pm3ds_cfg.rando_flags = modes[v];
            }
            else if (sscanf(line, "key_%31[a-z]=%d", name, &v) == 2 && v >= 0 && v < ACT_COUNT) {
                for (int k = 0; k < KEY_IDX_COUNT; k++)
                    if (!strcmp(name, s_key_cfg[k])) pm3ds_cfg.keymap[k] = (uint8_t)v;
            }
        }
        fclose(f);
    }
    if (pm3ds_cfg.language < 0) pm3ds_cfg.language = pm3ds_lang_from_system();
    pm3ds_lang_set(pm3ds_cfg.language);
    pm3ds_langpack_scan();
    pm3ds_langpack_request(pm3ds_cfg.lang_pack);
    apply();
}

void pm3ds_settings_save(void) {
    apply();
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "view_mode=%d\nfps60=%d\ndepth3d=%d\ntext_speed=%d\nff_speed=%d\nff_toggle=%d\nrando_flags=%u\n", pm3ds_cfg.view_mode,
            pm3ds_cfg.fps60, pm3ds_cfg.depth3d, pm3ds_cfg.text_speed, pm3ds_cfg.ff_speed, pm3ds_cfg.ff_toggle, pm3ds_cfg.rando_flags);
    fprintf(f, "update_check=%d\nupdate_skip=%d\n", pm3ds_cfg.update_check, pm3ds_cfg.update_skip);
    fprintf(f, "map_dragged=%d\n", pm3ds_cfg.map_dragged);
    fprintf(f, "stat_drops=%d\n", pm3ds_cfg.stat_drops);
    fprintf(f, "hp_beep=%d\n", pm3ds_cfg.hp_beep);
    fprintf(f, "battle_hud=%d\n", pm3ds_cfg.battle_hud);
    fprintf(f, "ice_fx=%d\n", pm3ds_cfg.ice_fx);
    fprintf(f, "music_on=%d\n", pm3ds_cfg.music_on);
    fprintf(f, "sfx_on=%d\n", pm3ds_cfg.sfx_on);
    fprintf(f, "jukebox=%d\n", pm3ds_cfg.jukebox);
    fprintf(f, "steer_spin=%d\n", pm3ds_cfg.steer_spin);
    fprintf(f, "steer_jump=%d\n", pm3ds_cfg.steer_jump);
    fprintf(f, "dpad_menus=%d\n", pm3ds_cfg.dpad_menus);
    fprintf(f, "lang_pack=%s\n", pm3ds_cfg.lang_pack);
    fprintf(f, "color_mode=%d\n", pm3ds_cfg.color_mode);
    fprintf(f, "autosave=%d\n", pm3ds_cfg.autosave);
    fprintf(f, "language=%d\n", pm3ds_cfg.language);
    for (int k = 0; k < KEY_IDX_COUNT; k++) fprintf(f, "key_%s=%d\n", s_key_cfg[k], pm3ds_cfg.keymap[k]);
    fclose(f);
}

/* once per game step, from the main loop */
static bool s_ff_held;
void pm3ds_ff_poll(void) {
    u32 held = hidKeysHeld();
    bool now = false;
    for (int k = 0; k < KEY_IDX_COUNT; k++)
        if (pm3ds_cfg.keymap[k] == ACT_FAST_FORWARD && (held & s_key_bits[k])) now = true;
    static bool was;
    if (pm3ds_cfg.ff_toggle) {
        if (now && !was) pm3ds_ff_button_on = !pm3ds_ff_button_on;
        s_ff_held = false;
    } else {
        s_ff_held = now;
    }
    was = now;
}

bool pm3ds_fast_forward_raw(void) { return s_ff_held || pm3ds_ff_button_on; }
bool pm3ds_fast_forward(void) {
    extern bool pm3ds_ff_paused_here(void);   /* n64_shim/quicksave.c */
    return pm3ds_fast_forward_raw() && !pm3ds_ff_paused_here();
}

int pm3ds_settings_text_speed(void) { return pm3ds_cfg.text_speed; }   /* src/msg.c */
unsigned int pm3ds_random_seed(void) { return (unsigned)osGetTime() ^ ((unsigned)svcGetSystemTick() * 2654435761u); }

/* for the decomp side (src/enemy_ai.c), which doesn't include this header */
bool pm3ds_stat_drops_on(void) { return pm3ds_cfg.stat_drops; }
bool pm3ds_battle_hud_on(void) { return pm3ds_cfg.battle_hud; }   /* src/inventory.c */
bool pm3ds_ice_fx_on(void) { return pm3ds_cfg.ice_fx; }   /* src/battle/dmg_player.c */
bool pm3ds_steer_spin(void) { return pm3ds_cfg.steer_spin; }   /* src/world/action/spin.c */
bool pm3ds_steer_jump(void) { return pm3ds_cfg.steer_jump; }   /* src/7BB60.c */
bool pm3ds_hp_beep_on(void) { return pm3ds_cfg.hp_beep; }   /* src/hud_element.c */
