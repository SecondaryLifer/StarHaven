/* pm3ds_hud_info.h — game state and art for the bottom-screen menu, as plain
 * C so the 3DS-side renderer (platform/bottom_screen.c) needs no decomp
 * headers. Filled by n64_shim/hud_info.c. */
#ifndef PM3DS_HUD_INFO_H
#define PM3DS_HUD_INFO_H

#include <stdint.h>

#define PM3DS_HUD_MAX_SPACES 40
#define PM3DS_HUD_PARTNERS   8

typedef struct {
    int in_game;          /* a save is loaded and the player is in the world/battle/pause */
    int in_battle;
    int demo;             /* attract demo running */
    int ui_ready;         /* past the boot and logos: the ROM and the HUD graphics are there (page icons) */
    int hp, max_hp, fp, max_fp, bp, level;
    int star_points, coins, star_pieces;
    int star_power, max_star_power;   /* star power in 1/256 bars */
    int chapter;          /* 0 prologue, 1..8, 9 epilogue */
    unsigned play_seconds;
    int partner;          /* PartnerIDs, 0 = none */
    unsigned partners_owned;          /* bit n = partner n has joined */
    char partner_name[16];
    char location[32];
    /* world map (pause menu data) */
    int map_count;
    int map_current;      /* index into map_x/map_y of Mario's location, -1 unknown */
    int16_t map_x[PM3DS_HUD_MAX_SPACES], map_y[PM3DS_HUD_MAX_SPACES];
    uint8_t map_visited[PM3DS_HUD_MAX_SPACES];
} pm3ds_hud_info_t;

void pm3ds_hud_query(pm3ds_hud_info_t *out);

/* names from the game's messages, in the UI encoding (pm3ds_lang.h): the iQue
 * ROM's are Chinese, 4 bytes a character */
#if VERSION_IQUE
#define PM3DS_UI_NAME 64
#define PM3DS_UI_DESC 512
#else
#define PM3DS_UI_NAME 24
#define PM3DS_UI_DESC 192
#endif

/* Tattle log (n64_shim/journal.c): enemies Goombario has tattled, in actor order */
#define PM3DS_TATTLE_MAX  160
#define PM3DS_TATTLE_NAME PM3DS_UI_NAME
typedef struct {
    int known, total;                     /* tattled / enemies with a tattle */
    short entry[PM3DS_TATTLE_MAX];        /* for pm3ds_tattle_text */
    char name[PM3DS_TATTLE_MAX][PM3DS_TATTLE_NAME];
} pm3ds_tattle_list_t;
int pm3ds_tattle_query(pm3ds_tattle_list_t *out);
void pm3ds_tattle_text(int entry, char *out, int size);   /* whole tattle as one paragraph */

/* Item tracker (n64_shim/journal.c): collected item spots per area, story order */
#define PM3DS_TRACKER_AREAS 24
typedef struct {
    char name[24];
    unsigned char found, total;               /* every tracked spot */
    unsigned char sp_found, sp_total;         /* star pieces */
    unsigned char badge_found, badge_total;   /* badges */
    unsigned char hidden_found, hidden_total; /* hidden blocks and panels */
} pm3ds_tracker_area_t;
typedef struct {
    int count, found, total;
    int rando;                                /* spots hold shuffled items: only the totals mean anything */
    int here;                                 /* area[] index of Mario's area, -1 if it has no spots */
    int room_found, room_total;               /* spots in the map Mario is in */
    pm3ds_tracker_area_t area[PM3DS_TRACKER_AREAS];
} pm3ds_tracker_t;
int pm3ds_tracker_query(pm3ds_tracker_t *out);
/* one area's spots (area: an index of pm3ds_tracker_t.area): the room's first, then by room */
#define PM3DS_TRACKER_SPOTS 64
typedef struct {
    int count;
    struct {
        char item[PM3DS_UI_NAME];             /* what lies there now (the randomizer's item in a seed) */
        const void *icon;                     /* HudScript, or NULL */
        unsigned char src;                    /* PM3DS_SPOT_* */
        unsigned char found, here;            /* taken; in Mario's room */
        unsigned char room;                   /* the map's number in its area (kmr_04: 4) */
    } spot[PM3DS_TRACKER_SPOTS];
} pm3ds_tracker_spots_t;
enum { PM3DS_SPOT_ITEM, PM3DS_SPOT_BLOCK, PM3DS_SPOT_TREE, PM3DS_SPOT_BUSH, PM3DS_SPOT_PANEL, PM3DS_SPOT_CHEST,
       PM3DS_SPOT_GIFT, PM3DS_SPOT_HIDDEN_BLOCK, PM3DS_SPOT_CRATE, PM3DS_SPOT_SUPER_BLOCK };
int pm3ds_tracker_spots(int area, pm3ds_tracker_spots_t *out);

/* Recipes (n64_shim/journal.c): every dish Tayce T. can cook, by name */
#define PM3DS_RECIPE_MAX  56
#define PM3DS_RECIPE_NAME PM3DS_UI_NAME
typedef struct {
    int count, cooked;
    short item[PM3DS_RECIPE_MAX];                 /* for pm3ds_recipe_detail */
    unsigned char done[PM3DS_RECIPE_MAX];         /* cooked at least once */
    const void *icon[PM3DS_RECIPE_MAX];           /* HudScript */
    char name[PM3DS_RECIPE_MAX][PM3DS_RECIPE_NAME];
} pm3ds_recipe_list_t;
int pm3ds_recipe_query(pm3ds_recipe_list_t *out);

/* Badges (n64_shim/journal.c): the badges Mario has, in the pause menu's order,
 * and switching one on or off from the bottom screen */
#define PM3DS_BADGE_MAX  128
#define PM3DS_BADGE_NAME PM3DS_UI_NAME
typedef struct {
    int count, bp_max, bp_used;
    int can_change;                               /* in the world, no menu or battle open */
    short item[PM3DS_BADGE_MAX];
    unsigned char cost[PM3DS_BADGE_MAX];
    unsigned char on[PM3DS_BADGE_MAX];            /* equipped */
    const void *icon[PM3DS_BADGE_MAX];            /* HudScript */
    char name[PM3DS_BADGE_MAX][PM3DS_BADGE_NAME];
} pm3ds_badge_list_t;
int pm3ds_badge_query(pm3ds_badge_list_t *out);
enum { PM3DS_BADGE_REMOVED, PM3DS_BADGE_EQUIPPED, PM3DS_BADGE_NO_BP, PM3DS_BADGE_TOO_MANY, PM3DS_BADGE_NOT_NOW };
int pm3ds_badge_toggle(int item);
/* badge loadouts: three saved sets per save file, kept on the SD card */
#define PM3DS_BADGE_LOADOUTS 3
int pm3ds_badge_loadout_count(int n);          /* badges in loadout n, 0 if empty */
int pm3ds_badge_loadout_save(int n);           /* the badges on now; returns how many */
int pm3ds_badge_loadout_load(int n, int *skipped);   /* returns how many went on; -1 not now */
/* one dish: what it does and every way to cook it */
#define PM3DS_RECIPE_WAYS  48
#define PM3DS_RECIPE_INGR  36
typedef struct {
    int hp, fp;                                   /* eating it restores */
    char desc[PM3DS_UI_DESC];
    int ways;
    struct {
        short a, b;                               /* ingr[] indices; b -1: on its own */
        unsigned char cookbook;                   /* only once Tayce T. has the Cookbook */
        unsigned char luck;                       /* a cooked Mystery? (a) turns into it by luck */
    } way[PM3DS_RECIPE_WAYS];
    int ingr_count;
    char ingr_name[PM3DS_RECIPE_INGR][PM3DS_RECIPE_NAME];
    const void *ingr_icon[PM3DS_RECIPE_INGR];
} pm3ds_recipe_detail_t;
void pm3ds_recipe_detail(int item, pm3ds_recipe_detail_t *out);

/* Chuck Quizmo's questions (GitHub #80): they come in order, one more after each right answer */
#define PM3DS_QUIZ_COUNT 64
#define PM3DS_QUIZ_OPT 40
void pm3ds_quiz_stats(int *done, int *tries, int *right);   /* questions done, answers given, right ones */
int pm3ds_quiz_right(int q);                                /* the right choice, 0..2 */
void pm3ds_quiz_question(int q, char *out, int size);
int pm3ds_quiz_options(int q, char out[3][PM3DS_QUIZ_OPT]);   /* how many */

/* N64 images the menu draws, decoded once into the UI atlas */
enum {
    UI_IMG_HEART, UI_IMG_FLOWER, UI_IMG_COIN, UI_IMG_STAR_POINT, UI_IMG_STAR_PIECE,
    UI_IMG_BP, UI_IMG_MARIO, UI_IMG_TIME, UI_IMG_POW_STAR, UI_IMG_POW_EMPTY,
    UI_IMG_LABEL_STATS, UI_IMG_LABEL_MAP, UI_IMG_LABEL_PARTY,
    UI_IMG_MAP_WALK0, UI_IMG_MAP_WALK1, UI_IMG_MAP_LOCATION, UI_IMG_CORNERS,
    UI_IMG_PARTNER_FIRST,                                    /* 8 portraits, PARTNER ids 1..8 */
    UI_IMG_PARTNER_DISABLED_FIRST = UI_IMG_PARTNER_FIRST + PM3DS_HUD_PARTNERS,
    UI_IMG_WORLD_MAP = UI_IMG_PARTNER_DISABLED_FIRST + PM3DS_HUD_PARTNERS,
    UI_IMG_COUNT
};

enum { UI_FMT_RGBA16, UI_FMT_RGBA32, UI_FMT_CI4, UI_FMT_CI8, UI_FMT_IA8, UI_FMT_I4 };

typedef struct {
    const uint8_t  *img;   /* big-endian N64 texels */
    const uint16_t *pal;   /* big-endian RGBA5551 palette for CI formats */
    int w, h, fmt;
} pm3ds_ui_image_t;

int pm3ds_ui_get_image(int id, pm3ds_ui_image_t *out);   /* 0 if unavailable */

/* icons the pages draw, as the game's HudScripts (decoded like the menus'
 * icons, so they need the ROM: only ask once a game is running) */
enum {
    UI_ICON_STAR_PIECE, UI_ICON_COMPASS, UI_ICON_SETTINGS, UI_ICON_CONTROLS,
    UI_ICON_DEBUG, UI_ICON_BOOTS, UI_ICON_HAMMER, UI_ICON_COOK, UI_ICON_CHEATS,
    UI_ICON_COUNT
};
const void *pm3ds_ui_icon_script(int id);

/* the game's dialogue font: 16x16 CI4 glyphs, code = ASCII - 0x20 */
typedef struct {
    const uint8_t  *rasters;   /* 128 bytes per glyph */
    const uint8_t  *widths;    /* advance per glyph */
    const uint16_t *palettes;  /* 16-entry palettes, 8 entries apart */
    int glyph_count;
} pm3ds_ui_font_t;

int pm3ds_ui_get_font(pm3ds_ui_font_t *out);   /* 0 until the game has loaded it */

#endif
