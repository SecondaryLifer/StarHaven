#ifndef PM3DS_CHEATS_H
#define PM3DS_CHEATS_H
/* Cheats (n64_shim/cheats.c): allowed per save, chosen on the new-game picker
 * (platform/bottom_screen.c), then switched one by one on the Options rail's
 * Cheats page. No decomp headers here. */
#include <stdbool.h>
#include <stdint.h>

enum {
    PM3DS_CHEAT_HP,       /* HP stays full, Mario can't be knocked out */
    PM3DS_CHEAT_FP,       /* FP stays full */
    PM3DS_CHEAT_SP,       /* star power stays full */
    PM3DS_CHEAT_COINS,    /* 999 coins */
    PM3DS_CHEAT_BP,       /* equip any badges, whatever their BP */
    PM3DS_CHEAT_NOCAP,    /* level-ups past 50 HP, 50 FP, 30 BP (GitHub #76) */
    PM3DS_CHEAT_QOL,      /* quality-of-life badges cost no BP (Discord suggestion) */
    PM3DS_CHEAT_LEVELS,   /* levels up to 99, and a won fight gives at least 1 star point (GitHub #77) */
    PM3DS_CHEAT_ACTION,   /* action commands and guarding from now on, without the Lucky Star (Discord) */
    PM3DS_CHEAT_COUNT,
};

/* Options > Cheats > Give (Discord: "Start with" options): done at the next game step */
enum {
    PM3DS_GIVE_HP, PM3DS_GIVE_FP, PM3DS_GIVE_BP, PM3DS_GIVE_LEVEL, PM3DS_GIVE_GEAR, PM3DS_GIVE_STAR_PIECES,
    PM3DS_GIVE_ITEM, PM3DS_GIVE_BADGE, PM3DS_GIVE_COUNT,
    PM3DS_GIVE_ITEM_NEXT = 0x10, PM3DS_GIVE_ITEM_PREV, PM3DS_GIVE_BADGE_NEXT, PM3DS_GIVE_BADGE_PREV,
};
void pm3ds_cheat_give(int what);
const char *pm3ds_cheat_give_item(void);    /* the items' and the badges' current choice */
const char *pm3ds_cheat_give_badge(void);

int pm3ds_max_level(void);                /* 27, or 99 with PM3DS_CHEAT_LEVELS or Level 99 */
void pm3ds_cheats_choose(bool allowed);   /* the next new game's choice */
bool pm3ds_cheats_allowed(void);          /* the running game's save allows cheats */
bool pm3ds_cheat_on(int cheat);
void pm3ds_cheat_toggle(int cheat);

/* Hard mode (n64_shim/cheats.c): per save, chosen on the new-game picker */
void pm3ds_hard_choose(bool on);
bool pm3ds_hard_mode(void);
int32_t pm3ds_hard_scale(int32_t v);   /* x1.5 rounded up in hard mode */

/* Level 99 (n64_shim/cheats.c, GitHub #83): per save, chosen on the new-game picker */
void pm3ds_level99_choose(bool on);
bool pm3ds_level99(void);

void pm3ds_skip_intro_choose(bool on);   /* the new file starts after the prologue's fall */

/* Boots and hammer (n64_shim/cheats.c, GitHub #79): wear any level already found.
 * gear 0 boots, 1 hammer; levels 0 normal, 1 super, 2 ultra; -1 none */
int pm3ds_gear_level(int gear);
int pm3ds_gear_best(int gear);           /* the best one found on this save */
bool pm3ds_gear_set(int gear, int level);   /* false: not now (battle, menus) or not found yet */

#endif
