/*
 * retry_battle.c — Settings > Retry battle: a fight lost to a Game Over can be fought
 * again from its start instead.
 *
 * When the encounter loads a battle (src/encounter.c, pre-battle), a copy is taken of
 * what the fight starts from: the player's data (HP, FP, items, badges, Star Power,
 * partners), the story's flags and bytes and the encounter itself (which enemy, first
 * strike, boss song, scripted or not). The map stays loaded while a battle runs.
 *
 * A lost fight that would end in a Game Over then waits for the player's choice
 * (src/battle/btl_states_actions.c; platform/bottom_screen.c shows it on both screens,
 * A or B or a tap). Game over goes on as before. Retry battle leaves the battle the way
 * a fled one does, back to the map, and the post-battle step (src/encounter.c) puts the
 * copy back and starts the same encounter's pre-battle step again, so bosses, scripted
 * fights and first strikes begin as they did the first time.
 */
#include "common.h"
#include "game_modes.h"

#include <string.h>

extern void pm3ds_log(int, const char*, ...);
extern bool pm3ds_retry_battle_on(void);   /* platform/pm3ds_settings.c */
extern u64 osGetTime(void);                /* libctru: milliseconds */

static PlayerData sPlayer;
static EncounterStatus sEncounter;
static s32 sGlobalFlags[ARRAY_COUNT(gCurrentSaveFile.globalFlags)];
static s8 sGlobalBytes[ARRAY_COUNT(gCurrentSaveFile.globalBytes)];
static s32 sAreaFlags[ARRAY_COUNT(gCurrentSaveFile.areaFlags)];
static s8 sAreaBytes[ARRAY_COUNT(gCurrentSaveFile.areaBytes)];
static bool sEnteringBattle;
static s32 sBattleID;
static bool sHave;          /* a copy of the running fight's start */
static s32 sAskFrames;
static bool sRetryChosen;
static u64 sAskedAt;        /* the last game step that waited for the choice */

volatile int pm3ds_retry_asking;   /* platform/bottom_screen.c: the choice is up */
volatile int pm3ds_retry_tap;      /* platform/bottom_screen.c: 1 Retry battle, 2 Game over tapped */
bool pm3ds_retry_restarting;       /* src/encounter.c: the pre-battle step of a retried fight */

/* src/encounter.c, pre-battle: the battle is loading */
void pm3ds_retry_snapshot(void) {
    pm3ds_retry_restarting = false;
    if (gGameStatusPtr->demoState != DEMO_STATE_NONE) {
        sHave = false;
        return;
    }
    sPlayer = gPlayerData;
    sEncounter = gCurrentEncounter;
    memcpy(sGlobalFlags, gCurrentSaveFile.globalFlags, sizeof(sGlobalFlags));
    memcpy(sGlobalBytes, gCurrentSaveFile.globalBytes, sizeof(sGlobalBytes));
    memcpy(sAreaFlags, gCurrentSaveFile.areaFlags, sizeof(sAreaFlags));
    memcpy(sAreaBytes, gCurrentSaveFile.areaBytes, sizeof(sAreaBytes));
    sEnteringBattle = (gPlayerStatus.flags & PS_FLAG_ENTERING_BATTLE) != 0;
    sBattleID = gCurrentBattleID;
    sHave = true;
    sRetryChosen = false;
    pm3ds_log(1, "RETRY snapshot battle %04X hp %d fp %d item0 %d", (int)sBattleID, gPlayerData.curHP,   /* debug level: every fight */
              gPlayerData.curFP, gPlayerData.invItems[0]);
}

/* src/battle/btl_states_actions.c, a lost fight about to become a Game Over: true while
 * the player is still choosing (the battle waits); false to go on, with
 * pm3ds_retry_chosen() telling which way */
bool pm3ds_retry_wait_choice(void) {
    s32 choice;

    if (!sHave || sBattleID != gCurrentBattleID || !pm3ds_retry_battle_on()
        || gGameStatusPtr->demoState != DEMO_STATE_NONE) {
        sRetryChosen = false;
        return false;
    }
    if (!pm3ds_retry_asking) {
        pm3ds_retry_asking = 1;
        pm3ds_retry_tap = 0;
        sAskFrames = 0;
    }
    sAskFrames++;
    sAskedAt = osGetTime();
    choice = pm3ds_retry_tap;
    /* the buttons after a moment, so a press meant for the fight doesn't choose */
    if (choice == 0 && sAskFrames > 20) {
        if (gGameStatusPtr->pressedButtons[0] & BUTTON_A) {
            choice = 1;
        } else if (gGameStatusPtr->pressedButtons[0] & BUTTON_B) {
            choice = 2;
        }
    }
#ifdef PM3DS_RETRY_AUTO   /* debug: retry every lost fight after a second */
    if (choice == 0 && sAskFrames > 30) {
        choice = 1;
    }
#endif
    if (choice == 0) {
        return true;
    }
    pm3ds_retry_asking = 0;
    pm3ds_retry_tap = 0;
    sRetryChosen = choice == 1;
    if (!sRetryChosen) {
        sHave = false;
    }
    pm3ds_log(2, "RETRY choice %s", sRetryChosen ? "retry" : "game over");
    return false;
}

/* platform/bottom_screen.c, gfx_present.c: the choice is on screen. Only while the fight
 * still waits for it, so a prompt can never outlive the battle it belongs to */
bool pm3ds_retry_prompt_up(void) {
    return pm3ds_retry_asking && osGetTime() - sAskedAt < 1000;
}

bool pm3ds_retry_chosen(void) {
    return sRetryChosen;
}

/* src/encounter.c, post-battle: true when this fight is to be fought again; the copy
 * is back and the caller starts the pre-battle step */
bool pm3ds_retry_take(void) {
    u32 playTime;

    if (!sRetryChosen || !sHave) {
        sRetryChosen = false;
        sHave = false;
        return false;
    }
    sRetryChosen = false;
    playTime = gPlayerData.frameCounter;   /* the time played keeps counting */
    gPlayerData = sPlayer;
    gPlayerData.frameCounter = playTime;
    gCurrentEncounter = sEncounter;
    memcpy(gCurrentSaveFile.globalFlags, sGlobalFlags, sizeof(sGlobalFlags));
    memcpy(gCurrentSaveFile.globalBytes, sGlobalBytes, sizeof(sGlobalBytes));
    memcpy(gCurrentSaveFile.areaFlags, sAreaFlags, sizeof(sAreaFlags));
    memcpy(gCurrentSaveFile.areaBytes, sAreaBytes, sizeof(sAreaBytes));
    if (sEnteringBattle) {
        gPlayerStatus.flags |= PS_FLAG_ENTERING_BATTLE;
    }
    pm3ds_retry_restarting = true;
    set_screen_overlay_color(SCREEN_LAYER_FRONT, 0, 0, 0);
    set_screen_overlay_params_front(OVERLAY_SCREEN_COLOR, 255.0f);
    pm3ds_log(2, "RETRY restart battle %04X hp %d fp %d item0 %d", (int)sBattleID, gPlayerData.curHP,
              gPlayerData.curFP, gPlayerData.invItems[0]);
    return true;
}
