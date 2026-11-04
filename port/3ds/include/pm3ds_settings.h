#ifndef PM3DS_SETTINGS_H
#define PM3DS_SETTINGS_H
/* Player options, kept in <SD folder>/port.cfg (platform/pm3ds_settings.c). */
#include <stdbool.h>
#include <stdint.h>

/* what a 3DS button does */
enum {
    ACT_NONE,
    ACT_A, ACT_B, ACT_Z, ACT_R, ACT_L, ACT_START,
    ACT_C_UP, ACT_C_DOWN, ACT_C_LEFT, ACT_C_RIGHT,
    ACT_FAST_FORWARD,
    ACT_TURBO_A,        /* held: A pressed and released on alternate reads (os_cont.c) */
    ACT_COUNT
};

/* the 3DS buttons that can be mapped, in the order the remap page lists them */
enum {
    KEY_IDX_A, KEY_IDX_B, KEY_IDX_X, KEY_IDX_Y, KEY_IDX_L, KEY_IDX_R, KEY_IDX_ZL, KEY_IDX_ZR,
    KEY_IDX_START, KEY_IDX_SELECT, KEY_IDX_DUP, KEY_IDX_DDOWN, KEY_IDX_DLEFT, KEY_IDX_DRIGHT,
    KEY_IDX_CUP, KEY_IDX_CDOWN, KEY_IDX_CLEFT, KEY_IDX_CRIGHT,
    KEY_IDX_COUNT
};

enum { TEXT_NORMAL, TEXT_FAST, TEXT_INSTANT, TEXT_SPEEDS };

/* 3D depth: how far the 3D slider at its top pushes the background back */
#define DEPTH3D_LEVELS 5
#define DEPTH3D_DEFAULT 1   /* the depth of 1.0.0 to 1.0.4 */
extern const float pm3ds_depth3d_shift[DEPTH3D_LEVELS];   /* pixels per eye at infinity */
extern const char *const pm3ds_depth3d_names[DEPTH3D_LEVELS];

typedef struct {
    int view_mode;              /* 0 Original, 1 Fill, 2 Wide, 3 Sharp wide (1:1, world to the edges) */
    bool fps60;                 /* in-between frames (n64_shim/gfx_interp.c) */
    int depth3d;                /* 0..DEPTH3D_LEVELS-1 */
    int text_speed;             /* TEXT_* */
    int ff_speed;               /* game steps per frame while fast-forwarding: 2..4 */
    bool ff_toggle;             /* the fast-forward button toggles instead of being held */
    unsigned rando_flags;       /* the new-game picker's last choice (pm3ds_rando.h; ON clear: classic) */
    uint8_t keymap[KEY_IDX_COUNT];   /* ACT_* per 3DS button */
    bool update_check;          /* ask GitHub for a newer release at launch (pm3ds_update.c) */
    int update_skip;            /* a release the player said not to mention again (version code) */
    bool map_dragged;           /* the player has dragged the bottom-screen map, so "Drag to look" is not shown */
    bool stat_drops;            /* enemies drop hearts and flowers (off: only the Finder badges' ones, GitHub #39) */
    bool hp_beep;
    uint8_t color_mode;   /* PM3DS_COLOR_*: textures as they are, more vivid, or corrected for colour blindness (#71) */
    uint8_t autosave;     /* PM3DS_AUTOSAVE_*: quick save by itself (Discord suggestion) */
    bool battle_hud;      /* the top screen's HP/FP bar in battle (GitHub #75: the bottom screen shows them too) */
    bool ice_fx;          /* the Ice Power badge's snowflakes and sound on a hit (Discord suggestion) */
    bool dpad_menus;      /* the D-pad moves the cursor while a menu is open (Discord suggestion) */
    bool music_on;        /* Sound: the music (GitHub #78; a muted bus, so anything waiting on a song still runs) */
    bool sfx_on;          /* Sound: sound effects and ambience */
    int jukebox;          /* Sound: a song played instead of the area's (n64_shim/jukebox.c); -1 off */
    bool steer_spin;      /* the stick steers a spin dash (Discord suggestion) */
    bool steer_jump;      /* the stick turns a jump in the air (Discord suggestion) */
    char lang_pack[32];   /* Game text: a language pack's folder in /3ds/StarHaven/lang (pm3ds_langpack.h); "": the ROM's */
    int language;               /* the port's own text: PM3DS_LANG_* (pm3ds_lang.h); the console's by default */               /* the HP icon's warning beep at 5 HP or less (src/hud_element.c) */
} pm3ds_settings_t;

extern pm3ds_settings_t pm3ds_cfg;

void pm3ds_settings_load(void);
void pm3ds_settings_save(void);
void pm3ds_settings_default_keymap(uint8_t *map);
uint32_t pm3ds_settings_key_bit(int key_idx);       /* KEY_* of libctru */
const char *pm3ds_settings_key_name(int key_idx);
const char *pm3ds_settings_action_name(int act);

/* fast-forward: held (or toggled) via a mapped button or the bottom-screen button */
extern bool pm3ds_ff_button_on;     /* touch toggle */
void pm3ds_ff_poll(void);           /* once per game step */
bool pm3ds_fast_forward(void);
bool pm3ds_fast_forward_raw(void);   /* the button, even where it pauses itself */

enum { PM3DS_COLOR_ORIGINAL, PM3DS_COLOR_VIVID, PM3DS_COLOR_DEUTERANOPIA, PM3DS_COLOR_PROTANOPIA,
       PM3DS_COLOR_TRITANOPIA, PM3DS_COLOR_COUNT };
void pm3ds_color_mode_changed(void);   /* n64_shim/gfx_texture.c: textures are converted again */

enum { PM3DS_AUTOSAVE_OFF, PM3DS_AUTOSAVE_AREA, PM3DS_AUTOSAVE_5, PM3DS_AUTOSAVE_10, PM3DS_AUTOSAVE_15, PM3DS_AUTOSAVE_COUNT };

#endif
