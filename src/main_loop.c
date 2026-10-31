#include "common.h"
#include "nu/nusys.h"
#include "audio/public.h"
#include "ld_addrs.h"
#include "hud_element.h"
#include "sprite.h"
#include "overlay.h"
#include "game_modes.h"
#ifdef _3DS
#include "gfx_prof.h"   // 3DS port: sampling profiler phases (empty unless PM3DS_STEP_PROF)
#endif

s32 gOverrideFlags;
s32 gTimeFreezeMode;
u16** nuGfxCfb;
BSS s16 SoftResetDelay;

DisplayContext D_80164000[2];

s8 gGameStepDelayAmount = 1;
s8 gGameStepDelayCount = 5;

GameStatus gGameStatus = {
    .curButtons = { 0 },
    .pressedButtons = { 0 },
    .heldButtons = { 0 },
    .prevButtons = { 0 },
    .stickX = { 0 },
    .stickY = { 0 },
    .holdDelayTime = { 0 },
    .holdRepeatInterval = { 0 },
};

GameStatus* gGameStatusPtr = &gGameStatus;
s16 SoftResetOverlayAlpha = 0;
s16 SoftResetState = 0;
s32 D_800741A4 = 0;

Mtx MasterIdentityMtx = RDP_MATRIX(
    1.000000, 0.000000, 0.000000, 0.000000,
    0.000000, 1.000000, 0.000000, 0.000000,
    0.000000, 0.000000, 1.000000, 0.000000,
    0.000000, 0.000000, 0.000000, 1.000000
);

s32 D_800741E8[2] = {0, 0}; // padding?
u16 gMatrixListPos = 0;
u16 D_800741F2 = 0;
s32 gCurrentDisplayContextIndex = 0;
s32 gPauseBackgroundFade = 0;
s32 D_800741FC = 0;

void gfx_init_state(void);
void gfx_draw_background(void);

#ifdef PM3DS_TITLE_DBG
/* M9 temporary: pinpoint the title-screen per-frame hang. pm3ds_title_dbg is
 * armed by __wrap_decode_yay0 when the title ImgList header is swapped (i.e.
 * state_init_title_screen ran). Each armed frame logs its progress; the last
 * "PH f=N ..." before the freeze is the function that hung. Compile-gated, so
 * normal builds are untouched. */
extern void pm3ds_log(int, const char *, ...);
int pm3ds_title_dbg = 0;
int pm3ds_title_frame = 0;
#define PM3DS_PH(name) do { if (pm3ds_title_dbg && pm3ds_title_frame < 8) \
    pm3ds_log(2, "PH f=%d " name, pm3ds_title_frame); } while (0)
#else
#define PM3DS_PH(name) ((void)0)
#endif

void step_game_loop(void) {
    PlayerData* playerData = &gPlayerData;
    const int MAX_GAME_TIME = 1000*60*60*60 - 1; // 1000 hours minus one frame at 60 fps
#ifdef PM3DS_TITLE_DBG
    if (pm3ds_title_dbg) pm3ds_title_frame++;
    PM3DS_PH("step_game_loop begin");
#endif

#if !VERSION_JP
    update_input();
#endif

    gGameStatusPtr->frameCounter++;

    playerData->frameCounter += 2;
    if (playerData->frameCounter > MAX_GAME_TIME) {
        playerData->frameCounter = MAX_GAME_TIME;
    }

#if VERSION_JP
    update_input();
#endif

    update_max_rumble_duration();

    if (gGameStepDelayCount != 0) {
        gGameStepDelayCount-- ;
        if (gGameStepDelayCount == 0) {
            gGameStepDelayCount = gGameStepDelayAmount;
        } else {
            return;
        }
    }

    PM3DS_PH("post stepDelay gate");
#ifdef _3DS
    {   // 3DS port: cheats top up HP, FP, star power, coins (cheats.c)
        extern void pm3ds_cheats_step(void);
        extern void pm3ds_qs_step(void);
        extern void pm3ds_sound_step(void);
        pm3ds_sound_step();    // Settings > Sound (jukebox.c)
        pm3ds_cheats_step();
        pm3ds_qs_step();       // a quick save loads where Mario stood (quicksave.c)
    }
#endif
    mdl_reset_transform_flags();
    npc_iter_no_op();
    update_workers();
    update_triggers();
    PM3DS_PH("pre update_scripts");
    PROF_PHASE(PH_G_SCRIPTS);
    update_scripts();
    PROF_PHASE(PH_GAME);
    PM3DS_PH("pre update_messages");
    update_messages();
    PM3DS_PH("pre update_hud_elements");
    update_hud_elements();
    PM3DS_PH("pre step_game_mode");
    PROF_PHASE(PH_G_MODE);
    step_game_mode();
    PROF_PHASE(PH_G_ENT);
    PM3DS_PH("post step_game_mode");
    update_entities();
    PROF_PHASE(PH_GAME);
    PM3DS_PH("post update_entities");
    func_80138198();
    PM3DS_PH("post func_80138198");
    bgm_update_music_control();
    update_ambient_sounds();
    sfx_update_env_sound_params();
    update_windows();
    update_curtains();
    PM3DS_PH("end step_game_loop");

    if (gOverrideFlags & GLOBAL_OVERRIDES_SOFT_RESET) {
        switch (SoftResetState) {
            case 0:
                gOverrideFlags |= GLOBAL_OVERRIDES_200;
                disable_player_input();

                if (SoftResetOverlayAlpha == 255) {
                    SoftResetState = 1;
                    SoftResetDelay = 3;
                } else {
                    SoftResetOverlayAlpha += 10;
                    if (SoftResetOverlayAlpha > 255) {
                        SoftResetOverlayAlpha = 255;
                    }
                }
                break;
            case 1:
                gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_DRAW_FRAME;
                SoftResetDelay--;
                if (SoftResetDelay == 0) {
                    sfx_stop_env_sounds();
#ifdef _3DS
                    {   // 3DS port: Quit game uses this reset, and nothing reboots after it as on the N64:
                        // the area's ambience (Gusty Gulch's wind) played on at the title and into the
                        // next file (Discord). Stop every ambience player and forget the area's sound.
                        for (s32 k = 0; k < 4; k++) {
                            snd_ambient_stop_quick(k);
                        }
                        reset_ambient_sounds();
                    }
#endif
                    set_game_mode(GAME_MODE_STARTUP);
                    gOverrideFlags &= ~GLOBAL_OVERRIDES_SOFT_RESET;
                }
                break;
        }
    } else {
        SoftResetOverlayAlpha = 0;
        SoftResetState = 0;
    }

    if (gOverrideFlags & GLOBAL_OVERRIDES_DISABLE_BATTLES) {
        gOverrideFlags |= GLOBAL_OVERRIDES_PREV_DISABLE_BATTLES;
    } else {
        gOverrideFlags &= ~GLOBAL_OVERRIDES_PREV_DISABLE_BATTLES;
    }

    if (gOverrideFlags & GLOBAL_OVERRIDES_200) {
        gOverrideFlags |= GLOBAL_OVERRIDES_PREV_200;
    } else {
        gOverrideFlags &= ~GLOBAL_OVERRIDES_PREV_200;
    }

    if (gOverrideFlags & GLOBAL_OVERRIDES_400) {
        gOverrideFlags |= GLOBAL_OVERRIDES_PREV_400;
    } else {
        gOverrideFlags &= ~GLOBAL_OVERRIDES_PREV_400;
    }

    if (gOverrideFlags & GLOBAL_OVERRIDES_800) {
        gOverrideFlags |= GLOBAL_OVERRIDES_PREV_800;
    } else {
        gOverrideFlags &= ~GLOBAL_OVERRIDES_PREV_800;
    }

    // Unused rand_int used to advance the global random seed each visual frame
    rand_int(1);
}

void gfx_task_background(void) {
    PM3DS_PH("bg: begin");
    gDisplayContext = &D_80164000[gCurrentDisplayContextIndex];
    gMainGfxPos = &gDisplayContext->backgroundGfx[0];

    gfx_init_state();
    PM3DS_PH("bg: pre gfx_draw_background");
    gfx_draw_background();
    PM3DS_PH("bg: post gfx_draw_background");

    gDPFullSync(gMainGfxPos++);
    gSPEndDisplayList(gMainGfxPos++);

    // TODO these << 3 >> 3 shouldn't be necessary. There's almost definitely something we're missing here...
    ASSERT((s32)((u32)((gMainGfxPos - gDisplayContext->backgroundGfx) << 3) >> 3) < ARRAY_COUNT(
               gDisplayContext->backgroundGfx))

    nuGfxTaskStart(&gDisplayContext->backgroundGfx[0], (u32)(gMainGfxPos - gDisplayContext->backgroundGfx) * 8,
                   NU_GFX_UCODE_F3DEX2, NU_SC_NOSWAPBUFFER);
    PM3DS_PH("bg: post nuGfxTaskStart (end gfx_task_background)");
}

void gfx_draw_frame(void) {
    PROF_PHASE(PH_G_GFX);   // reset to the game phase by whoever runs next (the renderer sets its own)
    PM3DS_PH("draw: enter gfx_draw_frame");
    gMatrixListPos = 0;
    gMainGfxPos = &gDisplayContext->mainGfx[0];

    if (gOverrideFlags & GLOBAL_OVERRIDES_DISABLE_DRAW_FRAME) {
        PM3DS_PH("draw: early-return DISABLE_DRAW_FRAME");
        gCurrentDisplayContextIndex = gCurrentDisplayContextIndex ^ 1;
        return;
    }

    gSPMatrix(gMainGfxPos++, &MasterIdentityMtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    PM3DS_PH("draw: pre spr_render_init");
    spr_render_init();
    PM3DS_PH("draw: post spr_render_init");

    if (!(gOverrideFlags & GLOBAL_OVERRIDES_DISABLE_RENDER_WORLD)) {
        render_frame(false);
    }
    PM3DS_PH("draw: post render_frame(false)");

    player_render_interact_prompts();
    func_802C3EE4();
    PM3DS_PH("draw: post func_802C3EE4");
    render_screen_overlay_backUI();
    render_workers_backUI();
    render_hud_elements_backUI();
    render_effects_UI();
    PM3DS_PH("draw: pre render_game_mode_backUI");
    render_game_mode_backUI();
    PM3DS_PH("draw: post render_game_mode_backUI");

    if (!(gOverrideFlags & GLOBAL_OVERRIDES_WINDOWS_OVER_CURTAINS)) {
        render_window_root();
    }

    if (!(gOverrideFlags & GLOBAL_OVERRIDES_DISABLE_RENDER_WORLD) && gGameStatusPtr->debugScripts == DEBUG_SCRIPTS_NONE) {
        render_frame(true);
    }
    PM3DS_PH("draw: post render_frame(true)");

    if (!(gOverrideFlags & GLOBAL_OVERRIDES_MESSAGES_OVER_CURTAINS)
        && !(gOverrideFlags & GLOBAL_OVERRIDES_MESSAGES_OVER_FRONTUI)
    ) {
        render_messages();
    }

    render_workers_frontUI();
    render_hud_elements_frontUI();
    render_screen_overlay_frontUI();

    if (!(gOverrideFlags & GLOBAL_OVERRIDES_MESSAGES_OVER_CURTAINS)
        && (gOverrideFlags & GLOBAL_OVERRIDES_MESSAGES_OVER_FRONTUI)
    ) {
        render_messages();
    }

    render_curtains();

    if (gOverrideFlags & GLOBAL_OVERRIDES_MESSAGES_OVER_CURTAINS) {
        render_messages();
    }

    if (gOverrideFlags & GLOBAL_OVERRIDES_WINDOWS_OVER_CURTAINS) {
        render_window_root();
    }

    PM3DS_PH("draw: pre render_game_mode_frontUI");
    render_game_mode_frontUI();
    PM3DS_PH("draw: post render_game_mode_frontUI");

    if (gOverrideFlags & GLOBAL_OVERRIDES_SOFT_RESET) {
        switch (SoftResetState) {
            case 0:
            case 1:
                _render_transition_stencil(OVERLAY_SCREEN_MARIO, SoftResetOverlayAlpha, nullptr);
                break;
        }
    }

    ASSERT((s32)(((u32)(gMainGfxPos - gDisplayContext->mainGfx) << 3) >> 3) < ARRAY_COUNT(gDisplayContext->mainGfx));

    gDPFullSync(gMainGfxPos++);
    gSPEndDisplayList(gMainGfxPos++);

    nuGfxTaskStart(gDisplayContext->mainGfx, (u32)(gMainGfxPos - gDisplayContext->mainGfx) * 8, NU_GFX_UCODE_F3DEX2,
                   NU_SC_TASK_LODABLE | NU_SC_SWAPBUFFER);
    gCurrentDisplayContextIndex = gCurrentDisplayContextIndex ^ 1;
    crash_screen_set_draw_info(nuGfxCfb_ptr, SCREEN_WIDTH, SCREEN_HEIGHT);
}

void load_engine_data(void) {
    s32 i;

    DMA_COPY_SEGMENT(engine4);
    DMA_COPY_SEGMENT(engine1);
    DMA_COPY_SEGMENT(evt);
    DMA_COPY_SEGMENT(entity);
    DMA_COPY_SEGMENT(engine2);
    DMA_COPY_SEGMENT(font_width);

    gOverrideFlags = 0;
    gGameStatusPtr->unk_79 = 0;
    gGameStatusPtr->backgroundFlags = 0;
    gGameStatusPtr->musicEnabled = true;
    gGameStatusPtr->healthBarsEnabled = true;
    gGameStatusPtr->introPart = INTRO_PART_NONE;
    gGameStatusPtr->demoBattleFlags = 0;
    gGameStatusPtr->multiplayerEnabled = false;
    gGameStatusPtr->altViewportOffset.x = -8;
    gGameStatusPtr->altViewportOffset.y = 4;
    gTimeFreezeMode = TIME_FREEZE_NONE;
    gGameStatusPtr->debugQuizmo = gGameStatusPtr->unk_13C = 0;
    gGameStepDelayCount = 5;
    gGameStatusPtr->saveCount = 0;
    fio_init_flash();
    clear_input();
    general_heap_create();
    clear_render_tasks();
    clear_worker_list();
    clear_script_list();
    create_cameras();
    clear_player_status();
    spr_init_sprites(PLAYER_SPRITES_MARIO_WORLD);
    clear_entity_models();
    clear_animator_list();
    clear_model_data();
    clear_sprite_shading_data();
    reset_background_settings();
    clear_character_set();
    clear_printers();
    clear_game_mode();
    clear_npcs();
    hud_element_clear_cache();
    clear_trigger_data();
    clear_entity_data(false);
    clear_player_data();
    init_encounter_status();
    clear_screen_overlays();
    clear_effect_data();
    clear_saved_variables();
    clear_item_entity_data();
    bgm_reset_sequence_players();
    reset_ambient_sounds();
    sfx_clear_sounds();
    clear_windows();
    initialize_curtains();
    poll_rumble();

    for (i = 0; i < ARRAY_COUNT(gGameStatusPtr->holdRepeatInterval); i++) {
        gGameStatusPtr->holdRepeatInterval[i] = 3;
        gGameStatusPtr->holdDelayTime[i] = 12;
    }

    gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_DRAW_FRAME;
    set_game_mode(GAME_MODE_STARTUP);
}

/// Time freeze modes:
///  0: none
///  1: NPCs move, can't be interacted with
///  2: NPCs don't move, no partner ability, can't interact, can't use exits
///  3: NPCs don't more or animate
///  4: NPCs can move, animations don't update, can use exits
void set_time_freeze_mode(s32 mode) {
    switch (mode) {
        case TIME_FREEZE_NONE:
            gTimeFreezeMode = mode;
            gOverrideFlags &= ~(GLOBAL_OVERRIDES_800 | GLOBAL_OVERRIDES_400 | GLOBAL_OVERRIDES_200 | GLOBAL_OVERRIDES_DISABLE_BATTLES);
            resume_all_group(EVT_GROUP_FLAG_INTERACT | EVT_GROUP_FLAG_MENUS);
            break;
        case TIME_FREEZE_PARTIAL:
            gTimeFreezeMode = mode;
            gOverrideFlags &= ~(GLOBAL_OVERRIDES_800 | GLOBAL_OVERRIDES_400 | GLOBAL_OVERRIDES_200);
            gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_BATTLES;
            suspend_all_group(EVT_GROUP_FLAG_INTERACT);
            break;
        case TIME_FREEZE_FULL:
            gTimeFreezeMode = mode;
            gOverrideFlags &= ~(GLOBAL_OVERRIDES_400 | GLOBAL_OVERRIDES_800);
            gOverrideFlags |= GLOBAL_OVERRIDES_200 | GLOBAL_OVERRIDES_DISABLE_BATTLES;
            suspend_all_group(EVT_GROUP_FLAG_MENUS);
            break;
        case TIME_FREEZE_POPUP_MENU:
            gTimeFreezeMode = mode;
            gOverrideFlags &= ~GLOBAL_OVERRIDES_800;
            gOverrideFlags |= GLOBAL_OVERRIDES_400 | GLOBAL_OVERRIDES_200 | GLOBAL_OVERRIDES_DISABLE_BATTLES;
            suspend_all_group(EVT_GROUP_FLAG_MENUS);
            break;
        case TIME_FREEZE_EXIT:
            gTimeFreezeMode = mode;
            gOverrideFlags |= GLOBAL_OVERRIDES_800 | GLOBAL_OVERRIDES_400 | GLOBAL_OVERRIDES_200 | GLOBAL_OVERRIDES_DISABLE_BATTLES;
            break;
    }
}

s32 get_time_freeze_mode(void) {
    return gTimeFreezeMode;
}

#if VERSION_IQUE
static const f32 rodata_padding[] = {0.0f, 0.0f};
#endif
