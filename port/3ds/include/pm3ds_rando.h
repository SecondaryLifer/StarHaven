#ifndef PM3DS_RANDO_H
#define PM3DS_RANDO_H
/* Randomizer settings of a game (n64_shim/rando.c), chosen per new game on
 * the new-game picker (platform/bottom_screen.c). No decomp headers here. */

#define PM3DS_RANDO_ON     0x01   /* items and badges: always, when on */
#define PM3DS_RANDO_COINS  0x02   /* + coins and star pieces */
#define PM3DS_RANDO_KEYS   0x04   /* + key items (story-order logic) */
#define PM3DS_RANDO_SHOPS  0x08   /* shops trade stock with the world */
#define PM3DS_RANDO_GIFTS  0x10   /* NPC gifts join the pool */
#define PM3DS_RANDO_HIDDEN 0x20   /* key items may go to hidden panels and hidden blocks */
#define PM3DS_RANDO_ENEMIES 0x40  /* enemies: each battle becomes another of the same chapter */
#define PM3DS_RANDO_BADGES 0x80  /* badge BP and FP costs dealt out again (Discord suggestion) */
#define PM3DS_RANDO_DOORS  0x100 /* door shuffle: two-way exits lead to other rooms of the same area */
#define PM3DS_RANDO_ALL    0x1FF
#define PM3DS_RANDO_ALL_V3 0x3F   /* "everything" before enemies: what older saves and settings meant */

/* the next new game's settings: flags 0 = classic; seed 0 = roll one */
void pm3ds_rando_choose(unsigned flags, unsigned seed);
unsigned pm3ds_rando_seed(void);   /* the running game's seed, 0 if off */

#endif
