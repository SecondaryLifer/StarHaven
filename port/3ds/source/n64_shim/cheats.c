/*
 * cheats.c — Star Haven's cheats.
 *
 * A save allows them or not, chosen once on the new-game picker
 * (platform/bottom_screen.c) and never changed after, so a normal save can
 * never turn them on. A save that
 * allows them gets a Cheats page on the Options rail with a switch per cheat
 * (pm3ds_cheats.h). Both live in the save file's unused tail next to the
 * randomizer's block (SaveData.unk_1304 + 0x10, inside the CRC), so they
 * stay with the save.
 *
 * Applied once per game step (src/main_loop.c): HP, FP, star power and coins
 * are topped up. Damage that would knock Mario out in the same step is caught
 * where the battle subtracts it (src/battle/1A5830.c, dmg_player.c), and the
 * badge menu skips its BP check (src/pause/pause_badges.c).
 */
#include "common.h"

#include <string.h>

#include "pm3ds_cheats.h"

#define CHEAT_MAGIC 0x53484348   /* "SHCH" */
#define CHEAT_BLOCK_OFS 0x10     /* after rando.c's block */

typedef struct {
    s32 magic;
    s32 version;
    s32 allowed;
    s32 on;        /* bit per PM3DS_CHEAT_* */
} CheatSaveBlock;

static bool sAllowed, sNextAllowed;
static u32 sOn;
bool pm3ds_cheat_on(int cheat);

/* Hard mode (GitHub #69), also chosen on the new-game picker and kept with the
 * save: enemies have 1.5x HP and hit 1.5x as hard, and drop 1.5x the coins
 * (rounded up); Mario has action commands and guarding from the start. Its own
 * block in the tail, after keyitems.c's. */
#define HARD_MAGIC 0x53484844      /* "SHHD" */
#define HARD_BLOCK_OFS 0x74
static bool sHard, sNextHard;

void pm3ds_hard_choose(bool on) {
    sNextHard = on;
}

/* Skip intro (Discord suggestion), also from the new-game picker: the new file starts
 * where Mario lands after falling off the cliff by Goompa's house, Goompa about to join */
static bool sNextSkipIntro;

void pm3ds_skip_intro_choose(bool on) {
    sNextSkipIntro = on;
}

/* src/filemenu/filemenu_yesno.c, a new file, after the game set its first map */
void pm3ds_new_game_start(void) {
    if (!sNextSkipIntro) {
        return;
    }
    sNextSkipIntro = false;
    gGameStatusPtr->areaID = AREA_KMR;
    gGameStatusPtr->mapID = 2;   // kmr_03
    gGameStatusPtr->entryID = 2; // falling in from the cliff above
    evt_set_variable(nullptr, GB_StoryProgress, STORY_CH0_FELL_OFF_CLIFF);
    evt_set_variable(nullptr, GF_KMR02_Met_Goompa, true);
    evt_set_variable(nullptr, GF_KMR02_Met_Goombario, true);
}

bool pm3ds_hard_mode(void) {
    return sHard;
}

/* src/battle/190B20.c (enemy HP), 1A5830.c (enemy damage), src/enemy_ai.c (coins) */
s32 pm3ds_hard_scale(s32 v) {
    return sHard && v > 0 ? (v * 3 + 1) / 2 : v;
}

void pm3ds_cheats_choose(bool allowed) {
    sNextAllowed = allowed;
}

bool pm3ds_cheats_allowed(void) {
    return sAllowed;
}

bool pm3ds_cheat_on(int cheat) {
    return sAllowed && (sOn >> cheat) & 1;
}

void pm3ds_cheat_toggle(int cheat) {
    if (sAllowed && cheat >= 0 && cheat < PM3DS_CHEAT_COUNT) {
        sOn ^= 1u << cheat;
    }
}

/* src/vars_access.c, clear_saved_variables: a fresh state has none */
void pm3ds_cheats_off(void) {
    sAllowed = false;
    sOn = 0;
    sHard = false;
}

/* src/filemenu/filemenu_yesno.c, new file: the picker's choice */
void pm3ds_cheats_new_game(void) {
    sAllowed = sNextAllowed;
    sNextAllowed = false;   /* used once, so it can't carry over to another file */
    sOn = 0;
#ifdef PM3DS_TEST_CHEATS
    sAllowed = true;
    sOn = (1u << PM3DS_CHEAT_NOCAP) | (1u << PM3DS_CHEAT_QOL);
#ifdef PM3DS_TEST_LEVELS
    sOn |= (1u << PM3DS_CHEAT_LEVELS) | (1u << PM3DS_CHEAT_ACTION);
    gPlayerData.level = PM3DS_TEST_LEVELS;
    gPlayerData.starPoints = 99;
#endif
#endif
    sHard = sNextHard;
    sNextHard = false;
    if (sHard) {
        // the tutorial fights can't be won at 1.5x damage without them (GitHub #74): action
        // commands and guarding from the start, as the Lucky Star gives them later
        gPlayerData.hasActionCommands = true;
    }
}

/* src/fio.c */
void pm3ds_cheats_serialize(void* tail) {
    CheatSaveBlock b = { CHEAT_MAGIC, 1, sAllowed, (s32)sOn };
    s32 hard[2] = { HARD_MAGIC, sHard };
    memcpy((u8*)tail + CHEAT_BLOCK_OFS, &b, sizeof(b));
    memcpy((u8*)tail + HARD_BLOCK_OFS, hard, sizeof(hard));
}

void pm3ds_cheats_deserialize(const void* tail) {
    CheatSaveBlock b;
    memcpy(&b, (const u8*)tail + CHEAT_BLOCK_OFS, sizeof(b));
    s32 hard[2];
    memcpy(hard, (const u8*)tail + HARD_BLOCK_OFS, sizeof(hard));
    pm3ds_cheats_off();
    sHard = hard[0] == HARD_MAGIC && hard[1] != 0;
    if (b.magic == CHEAT_MAGIC && b.version == 1 && b.allowed) {
        sAllowed = true;
        sOn = (u32)b.on & ((1u << PM3DS_CHEAT_COUNT) - 1);
    }
}

/* GitHub #76: the level-up limits. Two digits are all the level-up screen and the status bar
 * have, so "no limit" stops at 95 HP and FP, 99 BP, and 99 with badges on. */
bool pm3ds_no_stat_caps(void) {
    return pm3ds_cheat_on(PM3DS_CHEAT_NOCAP);
}
s32 pm3ds_cap_hpfp(void) {
    return pm3ds_no_stat_caps() ? 95 : 50;
}
s32 pm3ds_cap_bp(void) {
    return pm3ds_no_stat_caps() ? 99 : 30;
}
s32 pm3ds_cap_badged(void) {
    return pm3ds_no_stat_caps() ? 99 : 75;
}

/* Options > Cheats > Give. The touch screen asks, the game step does it. */
static const s16 sGiveItems[] = {
    ITEM_MUSHROOM, ITEM_SUPER_SHROOM, ITEM_ULTRA_SHROOM, ITEM_LIFE_SHROOM,
    ITEM_HONEY_SYRUP, ITEM_MAPLE_SYRUP, ITEM_JAMMIN_JELLY,
};
static volatile s32 sGivePending = -1;
static s32 sGiveItemIdx, sGiveBadge = ITEM_FIRST_BADGE;
static char sGiveItemName[32], sGiveBadgeName[32];

void pm3ds_cheat_give(int what) {
    if (sAllowed) {
        sGivePending = what;
    }
}
const char* pm3ds_cheat_give_item(void) {
    return sGiveItemName;
}
const char* pm3ds_cheat_give_badge(void) {
    return sGiveBadgeName;
}

static void give_badge_step(s32 dir) {
    for (s32 n = ITEM_LAST_BADGE - ITEM_FIRST_BADGE + 1; n > 0; n--) {
        sGiveBadge += dir;
        if (sGiveBadge > ITEM_LAST_BADGE) sGiveBadge = ITEM_FIRST_BADGE;
        if (sGiveBadge < ITEM_FIRST_BADGE) sGiveBadge = ITEM_LAST_BADGE;
        if (gItemTable[sGiveBadge].moveID != 0 && gItemTable[sGiveBadge].nameMsg != 0) {
            break;
        }
    }
}

static void give_names(void) {
    void pm3ds_decode_msg(s32 msgID, char* out, s32 size, bool dialogue);
    pm3ds_decode_msg(gItemTable[sGiveItems[sGiveItemIdx]].nameMsg, sGiveItemName, sizeof(sGiveItemName), false);
    pm3ds_decode_msg(gItemTable[sGiveBadge].nameMsg, sGiveBadgeName, sizeof(sGiveBadgeName), false);
}

static void give_step(void) {
    PlayerData* pd = &gPlayerData;
    s32 what = sGivePending;
    sGivePending = -1;
    s32 count = ARRAY_COUNT(sGiveItems);
    switch (what) {
        case PM3DS_GIVE_HP:
            if (pd->hardMaxHP + 5 <= pm3ds_cap_hpfp()) {
                pd->hardMaxHP += 5;
                pd->curMaxHP = MIN(pd->curMaxHP + 5, pm3ds_cap_badged());
                pd->curHP = pd->curMaxHP;
            }
            break;
        case PM3DS_GIVE_FP:
            if (pd->hardMaxFP + 5 <= pm3ds_cap_hpfp()) {
                pd->hardMaxFP += 5;
                pd->curMaxFP = MIN(pd->curMaxFP + 5, pm3ds_cap_badged());
                pd->curFP = pd->curMaxFP;
            }
            break;
        case PM3DS_GIVE_BP:
            if (pd->maxBP + 3 <= pm3ds_cap_bp()) {
                pd->maxBP += 3;
            }
            break;
        case PM3DS_GIVE_LEVEL:
            if (pd->level < pm3ds_max_level()) {
                pd->level++;
            }
            break;
        case PM3DS_GIVE_GEAR:
            if (pd->bootsLevel < GEAR_RANK_ULTRA) pd->bootsLevel++;
            if (pd->hammerLevel < GEAR_RANK_ULTRA) pd->hammerLevel++;
            break;
        case PM3DS_GIVE_STAR_PIECES:
            pd->starPieces = MIN(pd->starPieces + 10, 112);
            break;
        case PM3DS_GIVE_ITEM:
            add_item(sGiveItems[sGiveItemIdx]);
            break;
        case PM3DS_GIVE_BADGE:
            add_badge(sGiveBadge);
            break;
        case PM3DS_GIVE_ITEM_NEXT: sGiveItemIdx = (sGiveItemIdx + 1) % count; break;
        case PM3DS_GIVE_ITEM_PREV: sGiveItemIdx = (sGiveItemIdx + count - 1) % count; break;
        case PM3DS_GIVE_BADGE_NEXT: give_badge_step(1); break;
        case PM3DS_GIVE_BADGE_PREV: give_badge_step(-1); break;
    }
    give_names();
}

/* QoL badges at 0 BP (Discord suggestion): written over the move table each step while on;
 * rando.c rebuilds the table, its Badge costs option included, when the cheat goes off */
static void qol_badges_step(void) {
    static const u8 moves[] = { MOVE_SPEEDY_SPIN, MOVE_I_SPY, MOVE_PEEKABOO, MOVE_CHILL_OUT_BADGE, MOVE_FIRST_ATTACK, MOVE_SPIN_ATTACK,
                                MOVE_BUMP_ATTACK, MOVE_GROUP_FOCUS, MOVE_MONEY_MONEY, MOVE_REFUND, MOVE_RUNAWAY_PAY };
    static bool wasOn;
    bool on = pm3ds_cheat_on(PM3DS_CHEAT_QOL);
    if (on) {
        for (s32 i = 0; i < (s32)ARRAY_COUNT(moves); i++) {
            gMoveTable[moves[i]].costBP = 0;
        }
    } else if (wasOn) {
        extern void pm3ds_rando_badge_costs_refresh(void);
        pm3ds_rando_badge_costs_refresh();
    }
    wasOn = on;
}

/* src/battle/level_up.c, src/battle/1A5830.c: 2 digits is what the screens have */
int pm3ds_max_level(void) {
    return pm3ds_cheat_on(PM3DS_CHEAT_LEVELS) ? 99 : 27;
}

/* src/main_loop.c, every game step */
void pm3ds_cheats_step(void) {
    PlayerData* playerData = &gPlayerData;
#ifdef PM3DS_STEER_DBG
    {   /* debug: Mario's motion, for the steerable spin and jump */
        extern void pm3ds_log(int, const char*, ...);
        static s32 n, m;
        extern u16 au_bus_get_volume(u8 busID);
        if (++m % 150 == 0) pm3ds_log(2, "STEER bus gains %d %d %d %d", au_bus_get_volume(0), au_bus_get_volume(1), au_bus_get_volume(2), au_bus_get_volume(3));
        {
            static s32 lastLevel = -1, lastSP = -1, lastAC = -1;
            if (playerData->level != lastLevel || playerData->starPoints != lastSP || playerData->hasActionCommands != lastAC) {
                lastAC = playerData->hasActionCommands;
                pm3ds_log(2, "STEER level %d sp %d maxhp %d maxfp %d maxbp %d ac %d", playerData->level, playerData->starPoints,
                          playerData->hardMaxHP, playerData->hardMaxFP, playerData->maxBP, playerData->hasActionCommands);
                lastLevel = playerData->level;
                lastSP = playerData->starPoints;
            }
        }
        if (gPlayerStatus.actionState != ACTION_STATE_IDLE && ++n % 3 == 0) {
            pm3ds_log(2, "STEER act %d pos %d,%d yaw %d speed %d stick %d,%d", gPlayerStatus.actionState,
                      (int)gPlayerStatus.pos.x, (int)gPlayerStatus.pos.z, (int)gPlayerStatus.targetYaw,
                      (int)(gPlayerStatus.curSpeed * 10), gPlayerStatus.stickAxis[0], gPlayerStatus.stickAxis[1]);
        }
    }
#endif
    qol_badges_step();
    if (sAllowed && (sGivePending >= 0 || sGiveItemName[0] == 0)) {
        give_step();
    }
    if (!sAllowed || sOn == 0) {
        return;
    }
    if (pm3ds_cheat_on(PM3DS_CHEAT_HP) && playerData->curHP < playerData->curMaxHP) {
        playerData->curHP = playerData->curMaxHP;
    }
    if (pm3ds_cheat_on(PM3DS_CHEAT_FP) && playerData->curFP < playerData->curMaxFP) {
        playerData->curFP = playerData->curMaxFP;
    }
    if (pm3ds_cheat_on(PM3DS_CHEAT_SP) && playerData->starPower < playerData->maxStarPower * SP_PER_BAR) {
        playerData->starPower = playerData->maxStarPower * SP_PER_BAR;
    }
    if (pm3ds_cheat_on(PM3DS_CHEAT_COINS) && playerData->coins < 999) {
        playerData->coins = 999;
    }
    if (pm3ds_cheat_on(PM3DS_CHEAT_ACTION)) {
        playerData->hasActionCommands = true;
    }
}

/* src/pause/pause_badges.c: BP a badge may use */
s32 pm3ds_cheats_badge_bp(s32 bp) {
    return pm3ds_cheat_on(PM3DS_CHEAT_BP) ? 999 : bp;
}

/* src/battle: the player's HP after a hit, kept full (never 0, so no KO) */
s32 pm3ds_cheats_player_hp(s32 hp) {
    return pm3ds_cheat_on(PM3DS_CHEAT_HP) ? gPlayerData.curMaxHP : hp;
}
