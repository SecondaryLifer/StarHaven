/*
 * m13_debug.c — periodic game-logic probe for headless runs.
 *
 * One GAMESTAT line plus the live script table every 300 presented frames:
 * which map, where the player is, and which opcode / API function each
 * script is sitting on. Resolve the fn= addresses with addr2line.
 */
#include "common.h"
#include "map.h"
#include "model.h"
#include "game_modes.h"
#include "effects.h"
#include <malloc.h>
#include "pm3ds_brand.h"
#if defined(PM3DS_SHOP_TEST) || defined(PM3DS_HUD_TEST)
#include "hud_element.h"
#endif
#if defined(PM3DS_BATTLE_SWEEP) || defined(PM3DS_STORY_SWEEP) || defined(PM3DS_AUTO_WIN) || defined(PM3DS_AUTO_LOSE) || defined(PM3DS_ENEMY_HP1)
#include "../../../../src/battle/battle.h"
#endif
#ifdef PM3DS_BATTLE_SWEEP
void set_battle_formation(Battle* battle);
#endif

#if defined(PM3DS_START_MAP) || defined(PM3DS_MAP_SWEEP) || defined(PM3DS_DRAWLOG_FRAME) || \
    defined(PM3DS_RO_CHECK) || defined(PM3DS_MAGENTA_HUNT) || defined(PM3DS_OVERLAY_DBG) || \
    defined(PM3DS_TELEPORT_X) || defined(PM3DS_COLLIDER_CHECK) || defined(PM3DS_START_BARE) || \
    defined(PM3DS_FX_TEST) || defined(PM3DS_TEST_LEVELUP) || defined(PM3DS_BATTLE_SWEEP) || \
    defined(PM3DS_STORY_SWEEP) || defined(PM3DS_FX_SWEEP) || defined(PM3DS_NO_BATTLES) || \
    defined(PM3DS_SHOP_TEST) || defined(PM3DS_FX_DUST) || defined(PM3DS_ENC_TEST) || defined(PM3DS_FX_LIST) || \
    defined(PM3DS_JOURNAL_TEST) || defined(PM3DS_NO_TITLE_DEMO) || defined(PM3DS_QUICKSAVE_EVERY) || \
    defined(PM3DS_SHOP_DIALOG) || defined(PM3DS_MODEL_DUMP) || defined(PM3DS_TOUR) || defined(PM3DS_AUTO_WIN) || defined(PM3DS_AUTO_LOSE) || \
    defined(PM3DS_ENEMY_HP1) || defined(PM3DS_RANDO_TEST) || defined(PM3DS_FLAME_TEST) || defined(PM3DS_MUS_DBG) || defined(PM3DS_GOTO)
/* make_cia.sh refuses to publish a binary containing this string */
__attribute__((used)) const char pm3ds_debug_build_marker[] = "PM3DS-DEBUG-BUILD";
#endif

extern s32 gNumScripts;
extern ScriptList* gCurrentScriptListPtr;
void pm3ds_log(int, const char*, ...);
MapConfig* get_current_map_config(void);

extern MusicControlData gMusicControlData[2];

#ifdef PM3DS_MAP_SWEEP
/* Debug: visit every map in gAreas[] (entry 0), one every PM3DS_MAP_SWEEP
 * presented frames, logging "SWEEP <area>/<map> <name>" so run logs and
 * screenshots can be lined up per map. Random battles are disabled so a
 * sweep can't stall in one. Starts once the game is in the world. */
extern AreaConfig gAreas[];


static void pm3ds_map_sweep_tick(u32 frame) {
#ifndef PM3DS_SWEEP_FIRST_AREA
#define PM3DS_SWEEP_FIRST_AREA 0
#endif
    static s32 area = PM3DS_SWEEP_FIRST_AREA, map = -1, nextFrame = 0, done = 0;
    if (done || gGameStatusPtr == nullptr) {
        return;
    }
    if (get_game_mode() != GAME_MODE_WORLD) {
        if (nextFrame != 0 && (s32)frame > nextFrame + 3 * PM3DS_MAP_SWEEP) {
            nextFrame = frame;   /* stuck outside the world: try the next map anyway */
        } else {
            return;
        }
    }
    if (nextFrame == 0) {
        nextFrame = frame + PM3DS_MAP_SWEEP;
        return;
    }
    if ((s32)frame < nextFrame) {
        return;
    }
    if (++map >= gAreas[area].mapCount) {
        map = 0;
        do {
            area++;
        } while (gAreas[area].maps != nullptr && gAreas[area].mapCount == 0);
        if (gAreas[area].maps == nullptr) {
            pm3ds_log(2, "SWEEP DONE");
            done = 1;
            return;
        }
    }
    /* the chapter-end curtain call only works when reached through the story */
    if (strcmp(gAreas[area].maps[map].id, "kmr_23") == 0) {
        pm3ds_log(2, "SWEEP %d/%d %s skipped", area, map, gAreas[area].maps[map].id);
        nextFrame = frame;
        return;
    }
    gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_BATTLES;
    pm3ds_log(2, "SWEEP %d/%d %s", area, map, gAreas[area].maps[map].id);
    printf("\x1b[4;1HSWEEP %d/%d %s      \n", area, map, gAreas[area].maps[map].id);   /* labels screenshots */
    gGameStatusPtr->areaID = area;
    gGameStatusPtr->mapID = map;
    gGameStatusPtr->entryID = 0;
    set_map_transition_effect(TRANSITION_STANDARD);
    set_game_mode(GAME_MODE_CHANGE_MAP);
    nextFrame = frame + PM3DS_MAP_SWEEP;
}
#endif

#ifdef PM3DS_TOUR
/* Debug: a scenic tour for the showcase video, PM3DS_TOUR frames per map, battles off.
 * Once Mario has walked in, he is steered around: forward along the way he came in
 * (away from the exit), to either side and forward again, with a hop in each leg.
 * Logs "TOUR <i> <map>" so the recording can be cut per map. */
extern uint16_t pm3ds_debug_pad_or;
extern int8_t pm3ds_debug_stick_x, pm3ds_debug_stick_y;

static void tour_steer(f32 worldYaw) {   /* the stick is camera-relative (player_input_to_move_vector) */
    f32 a = (worldYaw - gCameras[CAM_DEFAULT].curYaw) * (3.14159265f / 180.0f);
    pm3ds_debug_stick_x = (int8_t)(80.0f * sinf(a));
    pm3ds_debug_stick_y = (int8_t)(80.0f * cosf(a));
}

static void pm3ds_tour_tick(u32 frame) {
    static const char* const maps[] = { "mac_01", "nok_01", "mim_01", "obk_01", "dro_02", "isk_01", "omo_03",
                                        "jan_00", "kzn_02", "flo_00", "sam_01", "pra_02", "hos_01", "hos_03" };
    static s32 i = -1, nextFrame, startFrame, moved = -1;
    static f32 inYaw;
    if (gGameStatusPtr == nullptr) {
        return;
    }
    if (i < 0 && nextFrame == 0) {   /* not the title demo: wait for the New Game's own start map */
        if (gGameStatusPtr->demoState != 0 || get_game_mode() != GAME_MODE_WORLD ||
            gGameStatusPtr->areaID != PM3DS_START_AREA || gGameStatusPtr->mapID != PM3DS_START_MAP) {
            return;
        }
        nextFrame = frame + PM3DS_TOUR;
        return;
    }
    pm3ds_debug_pad_or = 0;
    pm3ds_debug_stick_x = pm3ds_debug_stick_y = 0;
    if (i >= 0 && i < (s32)ARRAY_COUNT(maps) && get_game_mode() == GAME_MODE_WORLD) {
        s32 t = (s32)frame - startFrame;
        if (moved != i && t >= 70 && !(gPlayerStatus.flags & PS_FLAG_INPUT_DISABLED)) {
            moved = i;   /* walked in and in control: remember which way is into the map */
            inYaw = gPlayerStatus.targetYaw;
            startFrame = frame - 70;
            t = 70;
        }
        if (moved == i) {
            if (t < 110) tour_steer(inYaw);
            else if (t < 145) tour_steer(inYaw + 70.0f);
            else if (t < 180) tour_steer(inYaw - 70.0f);
            else if (t < 215) tour_steer(inYaw);
            if ((t >= 95 && t < 99) || (t >= 130 && t < 134) || (t >= 165 && t < 169) || (t >= 200 && t < 204)) {
                pm3ds_debug_pad_or = 0x8000;   /* A: hop */
            }
        }
    }
    if (i >= (s32)ARRAY_COUNT(maps) || get_game_mode() != GAME_MODE_WORLD || (s32)frame < nextFrame) {
        return;
    }
    if (++i >= (s32)ARRAY_COUNT(maps)) {
        pm3ds_log(2, "TOUR DONE");
        return;
    }
    for (s32 a = 0; gAreas[a].maps != nullptr; a++) {
        for (s32 m = 0; m < gAreas[a].mapCount; m++) {
            if (strcmp(gAreas[a].maps[m].id, maps[i]) == 0) {
                gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_BATTLES;
                pm3ds_log(2, "TOUR %d %s", (int)i, maps[i]);
                gGameStatusPtr->areaID = a;
                gGameStatusPtr->mapID = m;
                gGameStatusPtr->entryID = 0;
                set_map_transition_effect(TRANSITION_STANDARD);
                set_game_mode(GAME_MODE_CHANGE_MAP);
                nextFrame = frame + PM3DS_TOUR;
                startFrame = frame;
                return;
            }
        }
    }
    pm3ds_log(2, "TOUR %d %s not found", (int)i, maps[i]);
}
#endif

#ifdef PM3DS_FX_SWEEP
/* Debug: spawn every effect PlayEffect knows beside Mario, one every
 * PM3DS_FX_SWEEP frames, logging "FXSWEEP <i>/<n> <name>" so screenshots can
 * be lined up per effect. The scripts in fx_sweep_table.inc are generated from
 * the game's own PlayEffect calls by port/3ds/tools/gen_fx_sweep.py. Whatever
 * an entry left running is removed before the next one starts. */
extern EffectInstance* gEffectInstances[96];
#define FXS_PROLOGUE \
    Call(GetPlayerPos, LVar0, LVar1, LVar2) \
    Add(LVar0, 40) \
    Add(LVar1, 15) \
    Set(LVar3, LVar0) \
    Add(LVar3, 60) \
    Set(LVar4, LVar1) \
    Add(LVar4, 30) \
    Set(LVar5, LVar2) \
    Set(LVar6, 30) Set(LVar7, 30) Set(LVar8, 30) Set(LVar9, 30) Set(LVarA, 30) \
    Set(LVarB, 30) Set(LVarC, 30) Set(LVarD, 30) Set(LVarE, 30) Set(LVarF, 0)
#include "fx_sweep_table.inc"

static void pm3ds_fx_sweep_tick(u32 frame) {
#ifndef PM3DS_FX_SWEEP_FIRST
#define PM3DS_FX_SWEEP_FIRST 0   /* table index to start at */
#endif
    static s32 next = PM3DS_FX_SWEEP_FIRST - 1, worldFrames, done;
    static u8 before[ARRAY_COUNT(gEffectInstances)];
    s32 i;
    if (done || gGameStatusPtr == nullptr || get_game_mode() != GAME_MODE_WORLD) {
        return;
    }
    gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_BATTLES;
    if (++worldFrames < 150 || worldFrames % PM3DS_FX_SWEEP != 0) {
        return;
    }
    for (i = 0; i < ARRAY_COUNT(gEffectInstances); i++) {
        if (gEffectInstances[i] != nullptr && !before[i]) {
            remove_effect(gEffectInstances[i]);
        }
    }
#ifdef PM3DS_FX_SWEEP_ONLY   /* repeat the PM3DS_FX_SWEEP_FIRST effect (hardware checks) */
    next = PM3DS_FX_SWEEP_FIRST;
#else
    if (++next >= ARRAY_COUNT(fx_sweep_table)) {
        pm3ds_log(2, "FXSWEEP DONE");
        done = 1;
        return;
    }
#endif
    for (i = 0; i < ARRAY_COUNT(gEffectInstances); i++) {
        before[i] = gEffectInstances[i] != nullptr;
    }
    pm3ds_log(2, "FXSWEEP %d/%d %s at frame %u", next, (s32)ARRAY_COUNT(fx_sweep_table),
              fx_sweep_table[next].name, (unsigned)frame);
    start_script(fx_sweep_table[next].script, EVT_PRIORITY_1, 0);
}
#endif

#ifdef PM3DS_BATTLE_SWEEP
/* Debug: start every battle formation in turn from the world, let each run
 * PM3DS_BATTLE_SWEEP frames, then win it without the celebration and return
 * to the world for the next one. <SD folder>/sweep.txt may hold
 * "<first area> <last area>" to split the sweep across emulator instances,
 * optionally followed by the first area's starting formation index.
 * Logs "BSWEEP <area>/<index> <formation size>" per battle. */
extern bool pm3ds_debug_new_game_made;   /* src/filemenu/filemenu_yesno.c, with PM3DS_START_MAP */

static void pm3ds_battle_sweep_tick(u32 frame) {
    static s32 first = -1, last = -1, area, idx, phase, timer, done;
    if (done || gGameStatusPtr == nullptr) {
        return;
    }
    if (first < 0) {
        FILE* f = fopen(PM3DS_SD_DIR "/sweep.txt", "r");
        first = 0;
        last = ARRAY_COUNT(gBattleAreas) - 1;
        if (f != nullptr) {
            idx = 0;
            if (fscanf(f, "%d %d %d", &first, &last, &idx) < 2) {
                first = 0;
                last = ARRAY_COUNT(gBattleAreas) - 1;
            }
            fclose(f);
        }
        area = first;
        pm3ds_log(2, "BSWEEP range %d..%d", (int)first, (int)last);
    }
    switch (phase) {
        case 0:   /* in the world: wait, then start the next formation */
            if (get_game_mode() != GAME_MODE_WORLD || gGameStatusPtr->demoState != DEMO_STATE_NONE) {
                timer = 0;
                return;
            }
#ifdef PM3DS_START_MAP
            if (!pm3ds_debug_new_game_made) {   /* the title's world scenes come before New Game */
                return;
            }
#endif
            if (++timer < 45) {
                return;
            }
            for (;;) {
                if (area > last || area >= (s32)ARRAY_COUNT(gBattleAreas)) {
                    pm3ds_log(2, "BSWEEP DONE");
                    done = true;
                    return;
                }
                BattleArea* ba = &gBattleAreas[area];
                if (ba->battles != nullptr && (*ba->battles)[idx].formation != nullptr) {
                    break;
                }
                area++;
                idx = 0;
            }
#ifdef PM3DS_BATTLE_SWEEP_PARTNERS
            {   /* rotate through the eight partners so each one's battle actor shows up */
                static const s8 sweepPartners[] = {
                    PARTNER_GOOMBARIO, PARTNER_KOOPER, PARTNER_BOMBETTE, PARTNER_PARAKARRY,
                    PARTNER_BOW, PARTNER_WATT, PARTNER_SUSHIE, PARTNER_LAKILESTER,
                };
                static s32 sweepCount;
                s32 partner = sweepPartners[sweepCount++ % ARRAY_COUNT(sweepPartners)];
                gPlayerData.partners[partner].enabled = true;
                gPlayerData.curPartner = partner;
                pm3ds_log(2, "BSWEEP partner %d", (int)partner);
            }
#endif
            pm3ds_log(2, "BSWEEP %d/%d size=%d at frame %u", (int)area, (int)idx,
                      (int)(*gBattleAreas[area].battles)[idx].formationSize, (unsigned)frame);
            gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_BATTLES;   /* no world encounters in between */
            evt_set_variable(nullptr, GF_Tutorial_SwapTurnOrder, true);   /* Goombario's tutorial zooms in on Mario */
            gPlayerData.curHP = gPlayerData.curMaxHP;
            set_battle_formation(nullptr);
            set_battle_stage(-1);
            gEncounterState = ENCOUNTER_STATE_NONE;   /* no world encounter owns this battle */
            load_battle((area << 8) | idx);
#ifdef PM3DS_DRAWLOG_FRAME
            { extern uint32_t pm3ds_drawlog_frame; static int armed;
              if (!armed++) pm3ds_drawlog_frame = frame + 90; }   /* log one frame of the first battle */
#endif
            idx++;
            phase = 1;
            timer = -1;   /* counts down until the battle starts */
            break;
        case 1:   /* in battle: run, then win without the celebration */
            if (get_game_mode() == GAME_MODE_BATTLE) {
                if (timer < 0) {
                    timer = 0;   /* arrived */
                }
                if (++timer == PM3DS_BATTLE_SWEEP) {
                    pm3ds_log(2, "BSWEEP end at frame %u state=%d", (unsigned)frame, (int)gBattleState);
                    gPlayerData.curHP = gPlayerData.curMaxHP;
                    gBattleStatus.flags1 |= BS_FLAGS1_DISABLE_CELEBRATION;
                    btl_set_state(BATTLE_STATE_VICTORY);
                } else if (timer > PM3DS_BATTLE_SWEEP + 900) {
                    pm3ds_log(3, "BSWEEP stuck in battle (state=%d)", (int)gBattleState);
                    timer = PM3DS_BATTLE_SWEEP - 1;   /* try again */
                }
            } else if (get_game_mode() == GAME_MODE_WORLD && timer >= PM3DS_BATTLE_SWEEP) {
                phase = 0;   /* back in the world */
                timer = 0;
                gEncounterState = ENCOUNTER_STATE_NEUTRAL;
                clear_printers();   /* a battle cut short mid-speech never frees its printer */
            } else if (timer <= 0 && --timer < -600) {
                pm3ds_log(3, "BSWEEP battle did not start");
                phase = 0;
                timer = 0;
            }
            break;
    }
}
#endif

#ifdef PM3DS_ENEMY_HP1
/* Debug: enemies stay at 1 HP and Mario at 99 star points, so the next hit ends a battle the
 * real way (death animations, star points, a level-up) without playing it out. */
static void pm3ds_enemy_hp1_tick(void) {
    static bool armed;
    if (get_game_mode() != GAME_MODE_BATTLE) {
        armed = false;
        return;
    }
    if (!armed) {
        armed = true;
        gPlayerData.starPoints = 99;
    }
    for (s32 i = 0; i < ARRAY_COUNT(gBattleStatus.enemyActors); i++) {
        Actor* enemy = gBattleStatus.enemyActors[i];
        if (enemy != nullptr && enemy->curHP > 1) {
            enemy->curHP = 1;
        }
    }
}
#endif

#ifdef PM3DS_AUTO_WIN
/* Debug: win every battle PM3DS_AUTO_WIN frames after it starts, as if the last enemy fell,
 * so a story battle's defeat script and the scene after it run (e.g. -DPM3DS_AUTO_WIN=300). */
static void pm3ds_auto_win_tick(void) {
    static s32 timer;
    if (get_game_mode() != GAME_MODE_BATTLE) {
        timer = 0;
        return;
    }
    if (++timer == PM3DS_AUTO_WIN) {
        pm3ds_log(2, "AUTOWIN state=%d", (int)gBattleState);
        btl_set_state(BATTLE_STATE_VICTORY);
    }
}
#endif

#ifdef PM3DS_AUTO_LOSE
/* Debug: lose every battle PM3DS_AUTO_LOSE frames after it starts (game over and continue) */
static void pm3ds_auto_lose_tick(void) {
    static s32 timer;
    if (get_game_mode() != GAME_MODE_BATTLE) {
        timer = 0;
        return;
    }
    if (++timer == PM3DS_AUTO_LOSE) {
        pm3ds_log(2, "AUTOLOSE state=%d", (int)gBattleState);
        btl_set_state(BATTLE_STATE_DEFEAT);
    }
}
#endif

#ifdef PM3DS_STORY_SWEEP
/* Debug: visit maps with chosen story progress so their cutscenes run.
 * <SD folder>/story_sweep.txt holds "<map> <entry> <story>" per
 * line; each case runs PM3DS_STORY_SWEEP frames with A pressed every 40
 * frames (advances dialogue), scripted battles are won after 5 s, and
 * "SSWEEP <n> <map> entry=<e> story=<s>" lines mark the cases in the log. */
extern AreaConfig gAreas[];
extern s32 gCurrentBattleID;

static s32 pm3ds_find_map(const char* name, s32* areaOut, s32* mapOut) {
    for (s32 a = 0; gAreas[a].maps != nullptr; a++) {
        for (s32 m = 0; m < gAreas[a].mapCount; m++) {
            if (strcmp(gAreas[a].maps[m].id, name) == 0) {
                *areaOut = a;
                *mapOut = m;
                return true;
            }
        }
    }
    return false;
}

static void pm3ds_story_sweep_tick(u32 frame) {
    extern u16 pm3ds_debug_pad_or;
    static FILE* f;
    static s32 opened, done, caseNo, timer, waitWorld, battleTimer;
    if (done || gGameStatusPtr == nullptr) {
        pm3ds_debug_pad_or = 0;
        return;
    }
    if (!opened) {
        if (get_game_mode() != GAME_MODE_WORLD) {
            return;   /* wait for the debug start to reach the world */
        }
        opened = true;
        f = fopen(PM3DS_SD_DIR "/story_sweep.txt", "r");
        if (f == nullptr) {
            pm3ds_log(3, "SSWEEP no story_sweep.txt");
            done = true;
            return;
        }
        timer = PM3DS_STORY_SWEEP;   /* start the first case right away */
    }
    /* tap A: two frames of every forty */
    pm3ds_debug_pad_or = (frame % 40) < 2 ? 0x8000 : 0;

    if (get_game_mode() == GAME_MODE_BATTLE) {
        if (++battleTimer == 150) {
            pm3ds_log(2, "SSWEEP win battle %d/%d", (int)(gCurrentBattleID >> 8), (int)(gCurrentBattleID & 0xFF));
            gBattleStatus.flags1 |= BS_FLAGS1_DISABLE_CELEBRATION;
            btl_set_state(BATTLE_STATE_VICTORY);
        } else if (battleTimer > 1200) {
            pm3ds_log(3, "SSWEEP stuck in battle state=%d", (int)gBattleState);
            battleTimer = 140;
        }
        return;
    }
    battleTimer = 0;

    if (++timer < PM3DS_STORY_SWEEP) {
        return;
    }
    if (get_game_mode() != GAME_MODE_WORLD) {
        if (++waitWorld == 900) {
            pm3ds_log(3, "SSWEEP stuck in game mode %d", (int)get_game_mode());
        }
        return;
    }
    waitWorld = 0;

    char line[64], name[16];
    s32 entry, story, area, map;
    for (;;) {
        if (fgets(line, sizeof(line), f) == nullptr) {
            pm3ds_log(2, "SSWEEP DONE");
            done = true;
            pm3ds_debug_pad_or = 0;
            return;
        }
        if (sscanf(line, "%15s %d %d", name, &entry, &story) == 3 && pm3ds_find_map(name, &area, &map)) {
            break;
        }
    }
    caseNo++;
    {   /* Peach's castle during an intermission: play as Peach with Twink, as
         * osr_03's SetPlayerAsPeach does; everywhere else Mario with Goombario */
        static const s8 peachStories[] = { -105, -76, -54, -14, 6, 38, 58, 88 };   /* STORY_CHx_BEGAN_PEACH_MISSION */
        s32 asPeach = false, k;
        for (k = 0; k < ARRAY_COUNT(peachStories); k++) {
            if (story == peachStories[k] && strncmp(name, "kkj_", 4) == 0) {
                asPeach = true;
            }
        }
        if (asPeach) {
            gGameStatusPtr->peachFlags |= PEACH_FLAG_IS_PEACH;
            gPlayerData.curPartner = PARTNER_TWINK;
        } else {
            gGameStatusPtr->peachFlags &= ~PEACH_FLAG_IS_PEACH;
            gPlayerData.curPartner = PARTNER_GOOMBARIO;
        }
    }
    pm3ds_log(2, "SSWEEP %d %s entry=%d story=%d at frame %u", (int)caseNo, name, (int)entry, (int)story, (unsigned)frame);
    gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_BATTLES;
    gPlayerData.curHP = gPlayerData.curMaxHP;
    clear_printers();
    evt_set_variable(nullptr, GB_StoryProgress, story);
    gGameStatusPtr->areaID = area;
    gGameStatusPtr->mapID = map;
    gGameStatusPtr->entryID = entry;
    set_map_transition_effect(TRANSITION_STANDARD);
    set_game_mode(GAME_MODE_CHANGE_MAP);
    timer = 0;
}
#endif

#ifdef PM3DS_RO_CHECK
/* Debug: real hardware maps .text/.rodata read-only, Azahar does not. Keep a
 * copy of the read-only image and report every byte that changes, so writes
 * that would data-abort on a console show up in emulator runs. */
static u8* sROCopy;
void pm3ds_ro_check_init(void) {
    extern char __start__[], __exidx_end[];
    u32 len = (u32)(__exidx_end - __start__);
    sROCopy = malloc(len);
    if (sROCopy != nullptr) {
        memcpy(sROCopy, __start__, len);
    }
}

static void pm3ds_ro_check(u32 frame) {
    extern char __start__[], __exidx_end[];
    u8* copy = sROCopy;
    static s32 reports;
    u32 len = (u32)(__exidx_end - __start__);
    if (copy == nullptr) {
        return;
    }
    if (frame % 30 != 0 || reports >= 24) {
        return;
    }
    for (u32 i = 0; i < len; i += 4) {
        if (*(u32*)(__start__ + i) != *(u32*)(copy + i)) {
            u32 j = i;
            while (j < len && *(u32*)(__start__ + j) != *(u32*)(copy + j)) {
                j += 4;
            }
            reports++;
            pm3ds_log(3, "ROCHECK frame %u: read-only bytes changed at %p..%p (%u bytes)",
                      (unsigned)frame, (void*)(__start__ + i), (void*)(__start__ + j), (unsigned)(j - i));
            memcpy(copy + i, __start__ + i, j - i);
            i = j;
        }
    }
}
#endif

void pm3ds_game_debug_tick(u32 frame) {
#if defined(PM3DS_START_MAP) || defined(PM3DS_MAP_SWEEP) || defined(PM3DS_DRAWLOG_FRAME) || \
    defined(PM3DS_RO_CHECK) || defined(PM3DS_MAGENTA_HUNT) || defined(PM3DS_OVERLAY_DBG) || \
    defined(PM3DS_TELEPORT_X) || defined(PM3DS_COLLIDER_CHECK) || defined(PM3DS_START_BARE) || \
    defined(PM3DS_FX_TEST) || defined(PM3DS_TEST_LEVELUP) || defined(PM3DS_BATTLE_SWEEP) || \
    defined(PM3DS_STORY_SWEEP) || defined(PM3DS_FX_SWEEP) || defined(PM3DS_NO_BATTLES) || \
    defined(PM3DS_SHOP_TEST) || defined(PM3DS_FX_DUST) || defined(PM3DS_ENC_TEST) || defined(PM3DS_FX_LIST) || \
    defined(PM3DS_JOURNAL_TEST) || defined(PM3DS_NO_TITLE_DEMO) || defined(PM3DS_QUICKSAVE_EVERY) || \
    defined(PM3DS_SHOP_DIALOG) || defined(PM3DS_MODEL_DUMP) || defined(PM3DS_TOUR) || defined(PM3DS_AUTO_WIN) || defined(PM3DS_AUTO_LOSE) || \
    defined(PM3DS_ENEMY_HP1) || defined(PM3DS_RANDO_TEST) || defined(PM3DS_FLAME_TEST) || defined(PM3DS_MUS_DBG) || defined(PM3DS_GOTO)
    if (frame == 1) {
        pm3ds_log(2, "%s", pm3ds_debug_build_marker);   /* keeps the marker linked */
    }
#endif
#ifdef PM3DS_RO_CHECK
    pm3ds_ro_check(frame);
#endif
#ifdef PM3DS_HUD_TEST
    {   /* debug: the battle move icons side by side in the world (issue #5: Tattle's) */
        extern HudScript HES_MoveDiamond, HES_MoveBlueOrb, HES_MoveGreenOrb, HES_MoveRedOrb;
        extern HudScript HES_MoveDiamond_disabled, HES_MoveBlueOrbDisabled, HES_MoveGreenOrbDisabled, HES_MoveRedOrbDisabled;
        static HudScript* const scripts[8] = { &HES_MoveDiamond, &HES_MoveBlueOrb, &HES_MoveGreenOrb, &HES_MoveRedOrb,
            &HES_MoveDiamond_disabled, &HES_MoveBlueOrbDisabled, &HES_MoveGreenOrbDisabled, &HES_MoveRedOrbDisabled };
        static s32 made;
        if (!made && get_game_mode() == GAME_MODE_WORLD && frame > 60) {
            made = 1;
            for (s32 i = 0; i < 8; i++) {   /* as they are, then as the battle menu draws them */
                s32 id = hud_element_create(scripts[i]);
                hud_element_set_render_pos(id, 60 + (i % 4) * 50, 70 + (i / 4) * 40);
                id = hud_element_create(scripts[i]);
                hud_element_set_render_pos(id, 60 + (i % 4) * 50, 160 + (i / 4) * 25);
                hud_element_set_scale(id, 0.45f);
                hud_element_set_flags(id, HUD_ELEMENT_FLAG_FILTER_TEX);
            }
            pm3ds_log(2, "HUDTEST: move icons created");
        }
    }
#endif
    {   /* canary: something overwriting the global GameStatus pointer */
        extern GameStatus gGameStatus;
        static s32 reported;
        if (gGameStatusPtr != &gGameStatus && !reported) {
            reported = 1;
            pm3ds_log(3, "CANARY gGameStatusPtr clobbered: %p (expected %p) at frame %u",
                      (void*)gGameStatusPtr, (void*)&gGameStatus, (unsigned)frame);
        }
    }
#ifdef PM3DS_MAP_SWEEP
    pm3ds_map_sweep_tick(frame);
#endif
#ifdef PM3DS_TOUR
    pm3ds_tour_tick(frame);
#endif
#ifdef PM3DS_FX_SWEEP
    pm3ds_fx_sweep_tick(frame);
#endif
#ifdef PM3DS_BATTLE_SWEEP
    pm3ds_battle_sweep_tick(frame);
#endif
#ifdef PM3DS_AUTO_LOSE
    pm3ds_auto_lose_tick();
#endif
#ifdef PM3DS_AUTO_WIN
    pm3ds_auto_win_tick();
#endif
#ifdef PM3DS_ENEMY_HP1
    pm3ds_enemy_hp1_tick();
#endif
#ifdef PM3DS_RANDO_TEST
    { extern void pm3ds_rando_test_tick(void); pm3ds_rando_test_tick(); }
#endif
#ifdef PM3DS_STORY_SWEEP
    pm3ds_story_sweep_tick(frame);
#endif
#ifdef PM3DS_COLLIDER_CHECK
    { extern void pm3ds_validate_collision(const char*); pm3ds_validate_collision("frame tick"); }
    {   /* debug: report the first collider whose aabb pointer leaves the heap */
        extern CollisionData gCollisionData;
        extern u8 heap_collisionHead[];
        static s32 reported;
        u32 lo = (u32)heap_collisionHead, hi = lo + COLLISION_HEAP_SIZE;
        for (s32 i = 0; i < gCollisionData.numColliders && reported < 4 && gCollisionData.colliderList; i++) {
            Collider* c = &gCollisionData.colliderList[i];
            u32 a = (u32)c->aabb;
            if (c->numTriangles != 0 && a != 0 && (a < lo || a >= hi)) {
                reported++;
                pm3ds_log(3, "COLCHECK frame %u collider %d/%d aabb=%08X tri=%d flags=%08X parent=%d list=%p",
                          (unsigned)frame, i, gCollisionData.numColliders, a, c->numTriangles, c->flags,
                          c->parentModelIndex, (void*)gCollisionData.colliderList);
                break;
            }
        }
    }
#endif
#ifdef PM3DS_TEST_LEVELUP
    {   /* debug: two seconds into the first battle, win it with 99 star points
         * banked so the celebration runs the level-up screen */
        static s32 battleFrames, done;
        if (!done && gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_BATTLE && ++battleFrames > 60) {
            done = 1;
            gPlayerData.starPoints = 99;
            gBattleStatus.totalStarPoints = 3;
#ifdef PM3DS_TEST_CAPS   /* every stat at the game's limit: nothing left to raise (GitHub #76) */
            gPlayerData.hardMaxHP = gPlayerData.curMaxHP = gPlayerData.curHP = 50;
            gPlayerData.hardMaxFP = gPlayerData.curMaxFP = gPlayerData.curFP = 50;
            gPlayerData.maxBP = 30;
#endif
            pm3ds_log(2, "LEVELUP TEST: forcing victory at frame %u", (unsigned)frame);
            btl_set_state(BATTLE_STATE_VICTORY);
        }
    }
#endif
#if defined(PM3DS_DRAWLOG_FRAME) && defined(PM3DS_DRAWLOG_PREBATTLE)
    {   /* debug: dump the draws PM3DS_DRAWLOG_PREBATTLE frames into the first pre-battle (issue #5 white box) */
        extern uint32_t pm3ds_drawlog_frame;
        static int armedPre;
        if (!armedPre && gEncounterState == ENCOUNTER_STATE_PRE_BATTLE) {
            armedPre = 1;
            pm3ds_drawlog_frame = frame + PM3DS_DRAWLOG_PREBATTLE;
            pm3ds_log(2, "DRAWLOG armed for frame %u", (unsigned)pm3ds_drawlog_frame);
        }
    }
#endif
#ifdef PM3DS_FX_DUST
    {   /* debug: landing dust beside Mario every half second (issue #5 white box) */
        static s32 worldFrames;
        if (gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && ++worldFrames > 150 && worldFrames % 30 == 0) {
            fx_landing_dust(0, gPlayerStatus.pos.x + 30.0f, gPlayerStatus.pos.y, gPlayerStatus.pos.z, 0.0f);
        }
    }
#endif
#ifdef PM3DS_FX_LIST
    {   /* debug: the effects of GitHub #40 (Super Block upgrade) and #49 (Watt's
         * Power Shock) beside Mario in the world, one every 2.5 s */
        static s32 worldFrames;
        if (gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && ++worldFrames > 150
            && worldFrames % 150 == 0) {
            f32 x = gPlayerStatus.pos.x + 30, y = gPlayerStatus.pos.y + 12.5f, z = gPlayerStatus.pos.z;
            s32 which = (worldFrames / 150) % 8;
            pm3ds_log(2, "FXLIST %d at frame %u", which, (unsigned)frame);
            switch (which) {
                case 6: fx_motion_blur_flame(0, x, y, z, 1.0f, 90); break;
                case 7: { static EffectInstance* glow; fx_bulb_glow(0, x, y + 10, z, 1.0f, &glow); break; }
                case 0: fx_radial_shimmer(4, x, y, z, 1.0f, 75); break;
                case 1: fx_radial_shimmer(9, x, y, z, 1.0f, 75); break;
                case 2: fx_radial_shimmer(8, x, y, z, 1.3f, 75); break;
                case 3: fx_energy_orb_wave(0, x, y, z, 0.7f, 75); break;
                case 4: fx_snaking_static(0, x, y, z, 1.0f, 75); break;
                case 5: fx_sparkles(FX_SPARKLES_2, x, y + 20, z, 30.0f); break;
            }
        }
    }
#endif
#ifdef PM3DS_FX_TEST
    {   /* debug: spawn battle effects beside Mario in turn, one every 3 s */
        static s32 worldFrames;
        static EffectInstance* dmgFx;
        if (gGameStatusPtr != nullptr && (get_game_mode() == GAME_MODE_WORLD || get_game_mode() == GAME_MODE_BATTLE)
            && ++worldFrames > 150
            && worldFrames % 90 == 0) {
            f32 x = gPlayerStatus.pos.x, y = gPlayerStatus.pos.y, z = gPlayerStatus.pos.z;
            if (get_game_mode() == GAME_MODE_BATTLE && gBattleStatus.playerActor != nullptr) {
                Actor* mario = gBattleStatus.playerActor;   /* world player is parked at y = -1000 */
                x = mario->curPos.x;
                y = mario->curPos.y;
                z = mario->curPos.z;
            }
            s32 which = (worldFrames / 90) % 5;
            pm3ds_log(2, "FXTEST %d at frame %u", which, (unsigned)frame);
            switch (which) {
                case 0:
                    if (dmgFx != nullptr) {
                        dmgFx->data.damageIndicator->timeLeft = 5;   /* battle code retires it like this */
                    }
                    fx_damage_indicator(0, x + 20, y + 30, z, 10.0f, -55.0f, 7, &dmgFx);
#ifdef PM3DS_DRAWLOG_FRAME
                    { extern uint32_t pm3ds_drawlog_frame; static int armed;
                      if (!armed && get_game_mode() == GAME_MODE_BATTLE) { armed = 1; pm3ds_drawlog_frame = frame + 12; } }
#endif
                    break;
                case 1: fx_fire_breath(2, x - 80, y + 40, z, x + 80, y + 20, z, 50, 1, 24); break;
                case 2:
                    fx_damage_stars(FX_DAMAGE_STARS_3, x + 20, y + 30, z, 0.0f, -1.0f, 0.0f, 5);
#ifdef PM3DS_DRAWLOG_FRAME
                    { extern uint32_t pm3ds_drawlog_frame; static int armedStars;   /* mid-fade (issue #5 white box) */
                      if (!armedStars) { armedStars = 1; pm3ds_drawlog_frame = frame + PM3DS_DRAWLOG_FRAME; } }
#endif
                    break;
                case 3: fx_attack_result_text(0, x, y + 20, z - 10.0f, 3.0f, 90); break;   /* "Nice" */
                case 4: fx_underwater(0, -50.0f, 20.0f, 0.0f, 1.0f, 60); break;   /* Sushie's Tidal Wave tint */
            }
        }
    }
#endif
#ifdef PM3DS_NO_BATTLES
    if (gGameStatusPtr != nullptr) {   /* debug: field tests that must not be cut short by an encounter */
        gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_BATTLES;
    }
#endif
#ifdef PM3DS_TELEPORT_X
    {   /* debug: put the player at a fixed spot ~3 s after first reaching the world */
        static s32 worldFrames, done;
        if (!done && gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && ++worldFrames > 180) {
            done = 1;
            gPlayerStatus.pos.x = PM3DS_TELEPORT_X;
            gPlayerStatus.pos.y = PM3DS_TELEPORT_Y;
            gPlayerStatus.pos.z = PM3DS_TELEPORT_Z;
            pm3ds_log(2, "TELEPORT player -> (%d,%d,%d)", PM3DS_TELEPORT_X, PM3DS_TELEPORT_Y, PM3DS_TELEPORT_Z);
        }
    }
#endif
#ifdef PM3DS_JOURNAL_TEST
    {   /* debug: something for the Tattle and Found tabs: the first 40 actor types tattled, a few pickups */
        static s32 done;
        if (!done && gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && gGameStatusPtr->demoState == 0) {
            done = 1;   /* not in the title demo: New Game would clear it again */
            for (s32 i = 0; i < 5; i++) {
                set_global_byte(EVT_INDEX_OF_GAME_BYTE(GB_Tattles_00) + i, 0xFF);
            }
            evt_set_variable(nullptr, GF_ARN04_Item_StarPiece, true);
            evt_set_variable(nullptr, GF_ARN02_Item_DizzyDial, true);
            evt_set_variable(nullptr, GF_ARN02_ItemBlock_CoinA, true);
            evt_set_variable(nullptr, GF_ARN03_ItemBlock_Coin, true);
            evt_set_variable(nullptr, GF_ARN04_ItemBlock_Coin, true);
            evt_set_variable(nullptr, GF_ARN04_ItemBlock_SuperShroom, true);
            evt_set_variable(nullptr, GF_DRO02_HiddenPanel, true);
            pm3ds_log(2, "JOURNAL test flags set");
        }
    }
#endif
#ifdef PM3DS_ENC_TEST
    {   /* debug: real world encounters in turn (encounter PM3DS_ENC_TEST onwards): walk the enemy into
         * the player, then give every enemy actor 1 HP at the first player menu so a jump wins */
        static s32 worldFrames, enc = PM3DS_ENC_TEST, lowered;
        if (gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD) {
            lowered = 0;
            if (++worldFrames > 150 && enc < gCurrentEncounter.numEncounters) {
                Encounter* e = gCurrentEncounter.encounterList[enc];
                Enemy* en = e != nullptr && e->count > 0 ? e->enemy[0] : nullptr;
                Npc* npc = en != nullptr ? get_npc_safe(en->npcID) : nullptr;
                if (npc == nullptr || (en->flags & ENEMY_FLAG_DISABLE_AI)) {
                    enc++;
                    worldFrames = 0;
                } else {
                    if (worldFrames == 151) {
                        pm3ds_log(2, "ENCTEST encounter %d battle %04X npc %d", (int)enc, (int)e->battle, (int)en->npcID);
                    }
                    npc->pos.x = gPlayerStatus.pos.x + 10.0f;
                    npc->pos.y = gPlayerStatus.pos.y;
                    npc->pos.z = gPlayerStatus.pos.z;
                }
            }
        } else if (gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_BATTLE) {
            if (worldFrames > 150) {
                enc++;
            }
            worldFrames = 0;
            if (!lowered && gBattleState == BATTLE_STATE_PLAYER_MENU) {
                lowered = 1;
                for (s32 i = 0; i < MAX_ENEMY_ACTORS; i++) {
                    if (gBattleStatus.enemyActors[i] != nullptr) {
                        gBattleStatus.enemyActors[i]->curHP = 1;
                    }
                }
                pm3ds_log(2, "ENCTEST enemies at 1 HP, frame %u", (unsigned)frame);
            }
        }
    }
#endif
#ifdef PM3DS_MUS_DBG
    if (gGameStatusPtr != nullptr && frame % 120 == 0) {   /* debug: the BGM player 0 song, every 2 s */
        pm3ds_log(2, "MUSDBG map=%d.%d load=%d song=%d state=%d", gGameStatusPtr->areaID, gGameStatusPtr->mapID,
                  gGameStatusPtr->loadType, (int)gMusicControlData[0].requestedSongID, gMusicControlData[0].state);
    }
#endif
#ifdef PM3DS_GOTO
    {   /* debug: after PM3DS_GOTO frames in the world, a normal map change to
         * PM3DS_GOTO_AREA / _MAP / _ENTRY (e.g. through a door without walking there) */
        static s32 worldFrames, done;
        if (!done && gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && ++worldFrames == PM3DS_GOTO) {
            done = 1;
            pm3ds_log(2, "GOTO %d.%d entry %d", PM3DS_GOTO_AREA, PM3DS_GOTO_MAP, PM3DS_GOTO_ENTRY);
            gGameStatusPtr->areaID = PM3DS_GOTO_AREA;
            gGameStatusPtr->mapID = PM3DS_GOTO_MAP;
            gGameStatusPtr->entryID = PM3DS_GOTO_ENTRY;
            set_map_transition_effect(TRANSITION_STANDARD);
            set_game_mode(GAME_MODE_CHANGE_MAP);
        }
    }
#endif
#ifdef PM3DS_AMB_CYCLE
    {   /* debug: Boo's Mansion's yard, the gate room, back, the mansion, over and over with
         * changing waits (GitHub #56: the forest's ambience stayed on inside) */
        extern bool pm3ds_debug_new_game_made;
        static s32 frames, step, wait = 200, n;
        if (pm3ds_debug_new_game_made && gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD
            && gGameStatusPtr->context == CONTEXT_WORLD && ++frames >= wait) {
            static const struct { s16 area, map, entry; } route[] = {
                { AREA_MIM, 11, 0 }, { AREA_MIM, 10, 1 }, { AREA_OBK, 0, 0 }, { AREA_MIM, 10, 0 },
            };
            s32 i = step++ % 4;
            frames = 0;
            n++;
            wait = 25 + (n * 37) % 170;
            pm3ds_log(2, "AMBCYCLE %d to %d.%d wait %d", (int)n, route[i].area, route[i].map, (int)wait);
            gGameStatusPtr->areaID = route[i].area;
            gGameStatusPtr->mapID = route[i].map;
            gGameStatusPtr->entryID = route[i].entry;
            set_map_transition_effect(TRANSITION_STANDARD);
            set_game_mode(GAME_MODE_CHANGE_MAP);
        }
    }
#endif
#ifdef PM3DS_FLAME_TEST
    {   /* debug: the Super Block's three motion-blur flames around Mario, as in its upgrade (GitHub #57) */
        static s32 worldFrames;
        if (gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && ++worldFrames == 200) {
            for (s32 i = 0; i < PM3DS_FLAME_TEST; i++)
                fx_motion_blur_flame(0, gPlayerStatus.pos.x + (i - 1) * 30.0f, gPlayerStatus.pos.y + 20.0f, gPlayerStatus.pos.z, 1.0f, -1);
            pm3ds_log(2, "FLAMETEST %d flames", PM3DS_FLAME_TEST);
        }
    }
#endif
#ifdef PM3DS_QUICKSAVE_EVERY
    {   /* debug: press the Stats tab quick save every N frames in the world (result: -DPM3DS_ROOM_DBG log) */
        extern volatile s32 pm3ds_touch_req;
        extern volatile s32 pm3ds_quicksave_result;
        extern s32 pm3ds_player_in_room;
        if (gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && frame % PM3DS_QUICKSAVE_EVERY == 0) {
            pm3ds_quicksave_result = 0;
            pm3ds_touch_req = 2;
        }
        if (frame % PM3DS_QUICKSAVE_EVERY == 10 && pm3ds_quicksave_result != 0) {
            pm3ds_log(2, "QSDBG result=%d in_room=%d input_off=%d pos=(%.0f,%.0f,%.0f)", (int)pm3ds_quicksave_result,
                      (int)pm3ds_player_in_room, (int)gPlayerStatus.inputDisabledCount,
                      gPlayerStatus.pos.x, gPlayerStatus.pos.y, gPlayerStatus.pos.z);
        }
    }
#endif
#ifdef PM3DS_MODEL_DUMP
    {   /* debug: list the map's models once (tree id, flags, center, render mode, texture) */
        static s32 worldFrames, done;
        if (!done && gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD && ++worldFrames > 300) {
            done = 1;
            for (s32 i = 0; i < MAX_MODELS; i++) {
                Model* m = get_model_from_list_index(i);
                if (m == nullptr || m->flags == 0) {
                    continue;
                }
                pm3ds_log(2, "MODEL list=%d id=0x%X flags=%04X center=(%.0f,%.0f,%.0f) rm=%d tex=%d", (int)i,
                          (unsigned)m->modelID, (unsigned)m->flags, m->center.x, m->center.y, m->center.z,
                          (int)m->renderMode, (int)m->textureID);
            }
        }
    }
#endif
#ifdef PM3DS_SHOP_DIALOG
    {   /* debug: open the map's shop owner dialog ~4 s after first reaching the world */
        static s32 worldFrames, done;
        if (!done && gGameStatusPtr != nullptr && gGameStatusPtr->mapShop != nullptr &&
            get_game_mode() == GAME_MODE_WORLD && ++worldFrames > 240) {
            extern EvtScript EVS_ShopOwnerDialog;
            done = 1;
            start_script(&EVS_ShopOwnerDialog, EVT_PRIORITY_1, 0);
            pm3ds_log(2, "SHOPDLG started, coins=%d", (int)gPlayerData.coins);
#ifdef PM3DS_SHOP_FF   /* fast-forward from here: undrawn steps, as on an Old 3DS (the sell crash) */
            { extern bool pm3ds_ff_button_on; pm3ds_ff_button_on = true; }
#endif
        }
    }
#endif
#ifdef PM3DS_SHOP_TEST
    /* debug: where the map's shop items and the player are, every 2 s */
    if (frame % 60 == 0 && gGameStatusPtr != nullptr && gGameStatusPtr->mapShop != nullptr) {
        Shop* shop = gGameStatusPtr->mapShop;
        for (s32 i = 0; i < shop->numItems; i++) {
            pm3ds_log(2, "SHOP item %d id=%d price=%d at (%d,%d,%d)", (int)i, (int)shop->staticInventory[i].itemID,
                      (int)shop->staticInventory[i].price, (int)gGameStatusPtr->shopItemEntities[i].pos.x,
                      (int)gGameStatusPtr->shopItemEntities[i].pos.y, (int)gGameStatusPtr->shopItemEntities[i].pos.z);
        }
        pm3ds_log(2, "SHOP player (%d,%d,%d) coins=%d flags=%X", (int)gPlayerStatus.pos.x, (int)gPlayerStatus.pos.y,
                  (int)gPlayerStatus.pos.z, (int)gPlayerData.coins, (unsigned)shop->flags);
    }
#endif
    if (frame % 300 != 0 || gGameStatusPtr == nullptr) {
        return;
    }
    MapConfig* mc = get_current_map_config();
    /* gNpcCount, not get_npc_by_index: the NPC list pointer is null until the game first
     * sets up its NPCs, and an Old 3DS can still be loading at frame 300 (crash, 1.0.13) */
    extern s16 gNpcCount;
    s32 npcs = gNpcCount;
    pm3ds_log(2, "GAMESTAT mode=%d ctx=%d f=%u area=%d map=%d (%s) entry=%d player=(%.0f,%.0f,%.0f) scripts=%d npcs=%d story=%d",
              get_game_mode(), gGameStatusPtr->context, (unsigned)frame, gGameStatusPtr->areaID, gGameStatusPtr->mapID,
              mc != nullptr ? mc->id : "-", gGameStatusPtr->entryID,
              gPlayerStatus.pos.x, gPlayerStatus.pos.y, gPlayerStatus.pos.z, gNumScripts, npcs,
              gCurrentSaveFile.globalBytes[0]);
    {
        char buf[160];
        s32 n = snprintf(buf, sizeof(buf), "HPSTAT intro=%d demo=%d sb=%d mario=%d/%d fp=%d/%d coins=%d sp=%d enemies:",
                         gGameStatusPtr->introPart, gGameStatusPtr->demoState, gStatusBar.displayHP,
                         gPlayerData.curHP, gPlayerData.curMaxHP, gPlayerData.curFP, gPlayerData.curMaxFP,
                         gPlayerData.coins, gPlayerData.starPoints);
        for (s32 i = 0; i < ARRAY_COUNT(gBattleStatus.enemyActors) && n < (s32)sizeof(buf) - 12; i++) {
            Actor* a = gBattleStatus.enemyActors[i];
            if (a != nullptr) {
                n += snprintf(buf + n, sizeof(buf) - n, " %d/%d", a->curHP, a->maxHP);
            }
        }
        pm3ds_log(2, "%s", buf);
        extern s32 gBattleState;
        {
            extern u32 linearSpaceFree(void);
            extern u32 __ctru_heap_size, __ctru_linear_heap_size;
            struct mallinfo mi = mallinfo();
            extern void pm3ds_rom_stream_stats(u32 *hits, u32 *misses, u32 *miss_ms);
            u32 rh, rm, rms;
            pm3ds_rom_stream_stats(&rh, &rm, &rms);
            pm3ds_log(2, "MEMSTAT heap=%uK used=%uK free=%uK linear=%uK linfree=%uK rom hits=%u misses=%u miss_ms=%u",
                      (unsigned)(__ctru_heap_size >> 10), (unsigned)(mi.uordblks >> 10),
                      (unsigned)((__ctru_heap_size - mi.arena + mi.fordblks) >> 10),
                      (unsigned)(__ctru_linear_heap_size >> 10), (unsigned)(linearSpaceFree() >> 10),
                      (unsigned)rh, (unsigned)rm, (unsigned)rms);
        }
        pm3ds_log(2, "MUSSTAT enabled=%d demo=%d m0 flags=%04X state=%d req=%d name=%08X",
                  gGameStatusPtr->musicEnabled, gGameStatusPtr->demoState, gMusicControlData[0].flags,
                  gMusicControlData[0].state, (int)gMusicControlData[0].requestedSongID,
                  (unsigned)gMusicControlData[0].songName);
        pm3ds_log(2, "BTLSTAT state=%d player=%p partner=%p e0=%p e1=%p e2=%p",
                  gBattleState, (void*)gBattleStatus.playerActor, (void*)gBattleStatus.partnerActor,
                  (void*)gBattleStatus.enemyActors[0], (void*)gBattleStatus.enemyActors[1],
                  (void*)gBattleStatus.enemyActors[2]);
    }
    if (gCurrentScriptListPtr == nullptr) {
        return;
    }
    s32 shown = 0;
    for (s32 i = 0; i < MAX_SCRIPTS && shown < 16; i++) {
        Evt* s = (*gCurrentScriptListPtr)[i];
        if (s == nullptr || s->stateFlags == 0) {
            continue;
        }
        shown++;
        pm3ds_log(2, "  evt id=%d state=%02X op=%02X blk=%d fn=%p pc=%p",
                  s->id, s->stateFlags, s->curOpcode, s->blocked,
                  (void*)s->callFunction, (void*)s->ptrReadPos);
    }
}

/* M14: report the first player/camera float to turn NaN (and where). */
void pm3ds_nan_watch(const char* where) {
    static s32 reported;
    PlayerStatus* ps = &gPlayerStatus;
    struct { const char* name; f32 v; } f[] = {
        { "pos.x", ps->pos.x }, { "pos.y", ps->pos.y }, { "pos.z", ps->pos.z },
        { "targetYaw", ps->targetYaw }, { "curYaw", ps->curYaw }, { "curSpeed", ps->curSpeed },
        { "spriteFacingAngle", ps->spriteFacingAngle }, { "stickAxis0", ps->stickAxis[0] },
        { "stickAxis1", ps->stickAxis[1] }, { "camYaw", gCameras[CAM_DEFAULT].curYaw },
        { "heading", ps->heading }, { "runSpeed", ps->runSpeed }, { "walkSpeed", ps->walkSpeed },
        { "jumpVel", ps->gravityIntegrator[0] },
    };
    if (reported >= 4) {
        return;
    }
    for (u32 i = 0; i < ARRAY_COUNT(f); i++) {
        if (f[i].v != f[i].v) {
            reported++;
            pm3ds_log(3, "NANWATCH %s: %s is NaN (action=%d flags=%08X anim=%08X)", where, f[i].name,
                      ps->actionState, ps->flags, ps->animFlags);
            for (u32 k = 0; k < ARRAY_COUNT(f); k++) {
                pm3ds_log(3, "NANWATCH   %s=%f", f[k].name, (double)f[k].v);
            }
            return;
        }
    }
}

#ifdef PM3DS_UBSAN_FLOAT
/* Debug: build with EXTRA_CFLAGS="-fsanitize=float-cast-overflow
 * -DPM3DS_UBSAN_FLOAT" (compile only; devkitARM has no libubsan) to log every
 * float -> integer conversion that overflows the target type. ARM saturates
 * those (a negative float cast to u32 becomes 0) where the N64's compiler
 * wrapped them, so each logged site is a candidate port bug. One line per
 * site. */
typedef struct { const char* file; u32 line, col; } UbLoc;
typedef struct { u16 kind, info; char name[1]; } UbType;
typedef struct { UbLoc loc; const UbType* from; const UbType* to; } UbFloatCast;

static void pm3ds_ubsan_float_cast(void* data, void* value) {
    static const char* seenFile[128];
    static u32 seenLine[128];
    static s32 numSeen;
    UbFloatCast* d = data;
    f64 v = 0;
    s32 i;

    for (i = 0; i < numSeen; i++) {
        if (seenLine[i] == d->loc.line && seenFile[i] == d->loc.file) {
            return;
        }
    }
    if (numSeen < ARRAY_COUNT(seenLine)) {
        seenFile[numSeen] = d->loc.file;
        seenLine[numSeen] = d->loc.line;
        numSeen++;
    }
    if (d->from->info == 32) {
        union { u32 bits; f32 f; } u;
        u.bits = (u32)(uintptr_t)value;
        v = u.f;
    } else if (d->from->info == 64 && value != nullptr) {
        v = *(f64*)value;
    }
    pm3ds_log(3, "FLOATCAST %s:%u:%u %s -> %s value %f", d->loc.file, (unsigned)d->loc.line,
              (unsigned)d->loc.col, d->from->name, d->to->name, v);
}

void __ubsan_handle_float_cast_overflow(void* data, void* value) {
    pm3ds_ubsan_float_cast(data, value);
}

void __ubsan_handle_float_cast_overflow_abort(void* data, void* value) {
    pm3ds_ubsan_float_cast(data, value);
}
#endif
