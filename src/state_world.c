#include "functions.h"
#include "npc.h"
#include "game_modes.h"
#ifdef _3DS
#include "gfx_prof.h"   // 3DS port: sampling profiler phases (empty unless PM3DS_STEP_PROF)
#endif

void state_world_draw_aux_ui(void);

void state_init_world(void) {
    set_game_mode_render_frontUI(0, state_world_draw_aux_ui);
}

#ifdef _3DS
// 3DS port, Settings > "Stuck? Back to the door" (GitHub #90, platform/bottom_screen.c):
// a softlock (stuck in a wall, a cutscene that never ends) reloads the room from the
// entrance Mario came in by, the way a loading zone does. Scripts end with the map;
// input is left disabled once, as an exit script leaves it for the entry script.
extern volatile s32 pm3ds_unstuck_req, pm3ds_unstuck_result;
extern s16 WorldMenuState;

static void pm3ds_unstuck(void) {
    PlayerStatus* playerStatus = &gPlayerStatus;
    pm3ds_unstuck_req = 0;
    // not with a popup menu up (the reload clears the HUD elements its windows still draw),
    // nor while a battle starts or ends (its song and bookkeeping would be left half done)
    if (gGameStatusPtr->demoState != DEMO_STATE_NONE || gGameStatusPtr->context != CONTEXT_WORLD ||
        WorldMenuState != 0 /* WORLD_MENU_STATE_NONE, src/world/menus.c */ || gTimeFreezeMode == TIME_FREEZE_POPUP_MENU ||
        ((gWindows[WIN_POPUP_CONTENT].flags & WINDOW_FLAG_INITIALIZED) &&
         !(gWindows[WIN_POPUP_CONTENT].flags & WINDOW_FLAG_HIDDEN)) ||
        (gEncounterState != ENCOUNTER_STATE_NEUTRAL && gEncounterState != ENCOUNTER_STATE_CONVERSATION)) {
        pm3ds_unstuck_result = -1;
        return;
    }
    // riding Sushie or Lakilester: keep riding, as their own exits do (an entrance on water or lava)
    if (gPartnerStatus.partnerActionState != PARTNER_ACTION_NONE &&
        (gPartnerStatus.actingPartner == PARTNER_SUSHIE || gPartnerStatus.actingPartner == PARTNER_LAKILESTER)) {
        gGameStatusPtr->keepUsingPartnerOnMapChange = true;
    }
    playerStatus->inputDisabledCount = 1;
    gPartnerStatus.inputDisabledCount = 1;
    gOverrideFlags |= GLOBAL_OVERRIDES_40;
    enable_player_static_collisions();
    set_time_freeze_mode(TIME_FREEZE_NONE);
    pm3ds_unstuck_result = 1;
    set_map_transition_effect(TRANSITION_STANDARD);
    set_game_mode(GAME_MODE_CHANGE_MAP);
    {   extern void pm3ds_log(int, const char*, ...);
        pm3ds_log(2, "unstuck: area %d map %d entry %d", gGameStatusPtr->areaID, gGameStatusPtr->mapID,
                  gGameStatusPtr->entryID); }
}
#endif

void state_step_world(void) {
#ifdef _3DS
    if (pm3ds_unstuck_req) {
        pm3ds_unstuck();
        if (pm3ds_unstuck_result > 0) {
            return;
        }
    }
    PROF_PHASE(PH_W_ENC);
    update_encounters();
    PROF_PHASE(PH_W_NPCS);
    update_npcs();
    PROF_PHASE(PH_W_PLAYER);
    update_player();
    PROF_PHASE(PH_W_ITEMS);
    update_item_entities();
    PROF_PHASE(PH_W_FX);
    update_effects();
    PROF_PHASE(PH_W_MODELS);
    iterate_models();
    PROF_PHASE(PH_W_CAM);
    update_cameras();
    PROF_PHASE(PH_G_MODE);
#else
    update_encounters();
    update_npcs();
    update_player();
    update_item_entities();
    update_effects();
    iterate_models();
    update_cameras();
#endif
}

void state_drawUI_world(void) {
    draw_status_ui();
    draw_encounter_ui();
    imgfx_update_cache();
}

void state_world_draw_aux_ui(void) {
    draw_first_strike_ui();
}
