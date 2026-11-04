/*
 * touch_menu.c — game-side half of the bottom screen's touch menus.
 *
 * Each list menu that waits for a choice calls pm3ds_tm_poll() from its
 * "choosing" state (_3DS hooks in src/battle/btl_states_menus.c and the
 * popup menu, src/8a860_len_3f30.c). That publishes the entries for the
 * bottom screen and hands back a tap made there: the hook moves its cursor to
 * pm3ds_tm_index() and treats pm3ds_tm_confirm() like an A press, so a tap
 * takes the same path as the buttons (sounds, errors for disabled entries).
 */
#include "common.h"
#include "battle/battle.h"
#include "hud_element.h"
#include "nu/nusys.h"
#include "ld_addrs.h"
#include "game_modes.h"

#include <string.h>

#include "pm3ds_touch_menu.h"
#include "pm3ds_lang.h"
#if VERSION_IQUE
#include "ique_charmap.inc"
#endif

void dma_load_msg(u32 msgID, void* dest);

/* src/battle/btl_states_menus.c */
extern s32 WheelOptionCount;
extern s32 WheelOptionName[6];
extern s32 WheelOptionEnabled[6];
extern s8 BattleMenu_CurPos;
extern s8 BattleMenu_HomePos;
extern s32 MovesOptionCount;
extern s32 MovesOptionIndexMap[24];
extern s32 MovesOptionNames[24];
extern s32 MovesOptionDisplayCosts[24];
extern s32 MovesOptionDiscounts[24];
extern s32 MovesOptionEnabled[24];
extern s8 MovesCursorPos;
extern s32 UsingSpiritsSubmenu;
extern s32 StratsOptionCount;
extern s32 StratsOptionNames[5];
extern s32 StratsOptionEnabled[];
extern s8 StratsCursorPos;
extern s32 PartnerNameMessages[];
extern HudScript* WheelOptionHudScript[6];
extern s32 WheelOptionSubmenu[6];
extern HudScript* MovesOptionHudScripts[24];
extern HudScript* StratsOptionHudScripts[];
/* src/hud_element.c (its type is local there) */
typedef struct {
    s16 width;
    s16 height;
    s16 size;
} TmHudElementSize;
extern TmHudElementSize HudElemSizes[];
/* src/8a860_len_3f30.c */
extern PopupMenu* gPopupMenu;
extern s32 PopupMenu_SelectedIndex;

uint16_t pm3ds_touch_pad;
int8_t pm3ds_touch_stick_x;

static pm3ds_touch_menu_t sMenu;
static u32 sStamp;               /* gGameStatus.frameCounter when last published */
static s32 sNameMsg[PM3DS_TM_MAX + 1];   /* what each name was decoded from ([MAX] = title) */
static s32 sPickKind, sPickIndex;
static u32 sPickStamp;
static bool sConfirm;

/* ---- message text -> font codes ------------------------------------------ */
/* arguments of each MSG_READ_FUNC_* (0xFF xx ...); -1 = stop decoding */
static const s8 sFuncArgs[0x30] = {
    1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 2, 0, 2,     /* 00 font .. 0F speed */
    2, 1, 1, 1, 1, 1, 3, 2, 7, 1, 2, 2, 1, 3, 1, 1,     /* 10 set x .. 1F end choice */
    1, 1, 0, 0, 0, 0, -1, 1, 1, 1, 1, 0, 4, -1, 1, 1,   /* 20 set cancel .. 2F voice */
};

/* dialogue: a whole text box (tattles), whose style codes are skipped instead of
 * ending the text and whose pages run on as one paragraph */
static void decode_msg_as(s32 msgID, char* out, s32 size, bool dialogue) {
    static u8 buf[2048];
    const u8* m;
    s32 n = 0;
    s32 i;

    out[0] = '\0';
    if (msgID == MSG_NONE || msgID == 0) {
        return;
    }
    if (MSG_IS_ID(msgID)) {
        dma_load_msg(msgID, buf);
        m = buf;
    } else {
        m = (const u8*)msgID;
    }
    for (i = 0; i < (s32)sizeof(buf) && n < size - 1; i++) {
        u8 c = m[i];
        if (c == MSG_CHAR_READ_END) {
            break;
        }
        if (c == MSG_CHAR_READ_ENDL || c == MSG_CHAR_READ_SPACE || c == MSG_CHAR_READ_FULL_SPACE ||
            c == MSG_CHAR_READ_HALF_SPACE || c == MSG_CHAR_READ_NEXT) {
            if (n > 0 && out[n - 1] != ' ') {
                out[n++] = ' ';
            }
        } else if (c == MSG_CHAR_READ_PAUSE) {
            i++;   // its length in frames
        } else if (c == MSG_CHAR_READ_FUNCTION) {
            u8 f = m[++i];
            if (f >= ARRAY_COUNT(sFuncArgs) || sFuncArgs[f] < 0) {
                break;
            }
            i += sFuncArgs[f];
        } else if (c == MSG_CHAR_READ_STYLE) {
            if (!dialogue) {
                break;   // only dialogue boxes use styles
            }
            u8 style = m[++i];   // arguments as msg.c reads them
            i += style == MSG_STYLE_CHOICE ? 4 : style == MSG_STYLE_POSTCARD ? 1 : 0;
#if VERSION_IQUE
        } else if (c >= MSG_CHAR_MULTIBYTE_FIRST && c <= MSG_CHAR_MULTIBYTE_LAST) {
            /* a Chinese character: a lead and a trail byte (src/msg_draw.c), kept
             * as its code point (pm3ds_lang.h), drawn with the ROM's font */
            u8 trail = m[i + 1];
            if (trail == 0 || trail >= 0xF0) {
                continue;   // not a pair: leave the next byte to the loop
            }
            i++;
            s32 glyph = (MSG_CHAR_ZH_START - MSG_CHAR_MULTIBYTE_FIRST) + c +
                        (trail - 1) * (MSG_CHAR_MULTIBYTE_LAST - MSG_CHAR_MULTIBYTE_FIRST + 1);
            u32 cp = glyph >= PM3DS_IQUE_GLYPH_FIRST && glyph < PM3DS_IQUE_GLYPH_END
                         ? pm3ds_ique_glyph_cp[glyph - PM3DS_IQUE_GLYPH_FIRST] : 0;
            if (cp == 0) {
                out[n++] = '?';
                continue;
            }
            if (n + 4 >= size) {
                break;
            }
            out[n++] = PM3DS_TXT_ESC;
            out[n++] = (char)(0x80 | (cp >> 14));
            out[n++] = (char)(0x80 | ((cp >> 7) & 0x7F));
            out[n++] = (char)(0x80 | (cp & 0x7F));
#endif
        } else if (c < 0xF0 && c + 0x20 < 0x100) {
            out[n++] = (char)(c + 0x20);
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

static void decode_msg(s32 msgID, char* out, s32 size) {
    decode_msg_as(msgID, out, size, false);
}

/* for n64_shim/journal.c */
void pm3ds_decode_msg(s32 msgID, char* out, s32 size, bool dialogue) {
    decode_msg_as(msgID, out, size, dialogue);
}

/* decode only when the message changed: these run every frame */
static void set_name(s32 slot, s32 msgID) {
    char* dst = slot == PM3DS_TM_MAX ? sMenu.title : sMenu.e[slot].name;
    s32 size = slot == PM3DS_TM_MAX ? sizeof(sMenu.title) : sizeof(sMenu.e[0].name);
    if (sNameMsg[slot] != msgID || dst[0] == '\0') {
        sNameMsg[slot] = msgID;
        decode_msg(msgID, dst, size);
    }
}

static void set_entry(s32 i, s32 msgID, bool enabled, s32 cost, s32 costKind) {
    set_name(i, msgID);
    sMenu.e[i].enabled = enabled;
    sMenu.e[i].cost = cost;
    sMenu.e[i].max_hp = 0;
    sMenu.e[i].cost_kind = costKind;
    sMenu.e[i].attack = false;
    sMenu.e[i].icon = nullptr;
    sMenu.e[i].pr = 0;
    sMenu.e[i].enemy = false;
}

static void target_place(pm3ds_tm_entry_t* e, Actor* t, SelectableTarget* target);

/* the battle's current wheel option names the open submenu */
static void wheel_title(void) {
    s32 pos = BattleMenu_CurPos + BattleMenu_HomePos;
    if (pos >= 0 && pos < WheelOptionCount) {
        set_name(PM3DS_TM_MAX, WheelOptionName[pos]);
    }
}

/* ---- publishing ------------------------------------------------------------ */
static void publish(s32 kind) {
    s32 i;
    s32 n = 0;

    sMenu.kind = kind;
    sMenu.all_targets = 0;
    sMenu.partner = (gBattleStatus.flags1 & BS_FLAGS1_PARTNER_ACTING) != 0;
    switch (kind) {
        case PM3DS_TM_WHEEL:
            n = WheelOptionCount;
            for (i = 0; i < n && i < PM3DS_TM_MAX; i++) {
                set_entry(i, WheelOptionName[i], WheelOptionEnabled[i] == true, 0, PM3DS_TM_COST_NONE);
                sMenu.e[i].icon = WheelOptionHudScript[i];
                sMenu.e[i].attack = WheelOptionSubmenu[i] == BTL_MENU_TYPE_JUMP ||
                                    WheelOptionSubmenu[i] == BTL_MENU_TYPE_SMASH;
            }
            sMenu.cursor = BattleMenu_CurPos + BattleMenu_HomePos;
            set_name(PM3DS_TM_MAX, sMenu.partner ? PartnerNameMessages[gPlayerData.curPartner] : MSG_NONE);
            if (gBattleState == BATTLE_STATE_PEACH_MENU || gBattleState == BATTLE_STATE_TWINK_MENU) {
                // the final battle: Peach's and Twink's turns use the same wheel (GitHub #58)
                strcpy(sMenu.title, gBattleState == BATTLE_STATE_PEACH_MENU ? "Peach" : "Twink");
                sNameMsg[PM3DS_TM_MAX] = -2;
            } else if (!sMenu.partner) {
                strcpy(sMenu.title, "Mario");
                sNameMsg[PM3DS_TM_MAX] = -2;
            }
            break;
        case PM3DS_TM_MOVES:
            n = MovesOptionCount;
            for (i = 0; i < n && i < PM3DS_TM_MAX; i++) {
                s32 o = MovesOptionIndexMap[i];
                s32 shown = MovesOptionDisplayCosts[o];
                s32 cost = shown - MovesOptionDiscounts[o];
                if (shown != 0 && cost <= 0) {
                    cost = 1;
                }
                if (UsingSpiritsSubmenu) {
                    cost = shown;   // star power bars
                }
                set_entry(i, MovesOptionNames[o], MovesOptionEnabled[o] > 0, cost,
                          shown == 0 ? PM3DS_TM_COST_NONE
                                     : (UsingSpiritsSubmenu ? PM3DS_TM_COST_SP : PM3DS_TM_COST_FP));
                sMenu.e[i].icon = MovesOptionHudScripts[o];
            }
            sMenu.cursor = MovesCursorPos;
            wheel_title();
            break;
        case PM3DS_TM_STRATS:
            n = StratsOptionCount;
            for (i = 0; i < n && i < PM3DS_TM_MAX; i++) {
                set_entry(i, StratsOptionNames[i], StratsOptionEnabled[i] != 0, 0, PM3DS_TM_COST_NONE);
                sMenu.e[i].icon = StratsOptionHudScripts[i];
            }
            sMenu.cursor = StratsCursorPos;
            wheel_title();
            break;
        case PM3DS_TM_POPUP: {
            PopupMenu* p = gPopupMenu;
            s32 costKind = PM3DS_TM_COST_NONE;
            if (p == nullptr) {
                sMenu.kind = PM3DS_TM_NONE;
                return;
            }
            if (p->popupType == POPUP_MENU_SELL_ITEM) {
                costKind = PM3DS_TM_COST_COINS;
            } else if (p->popupType == POPUP_MENU_TRADE_FOR_BADGE) {
                costKind = PM3DS_TM_COST_STAR_PIECES;
            }
            n = p->numEntries;
            for (i = 0; i < n && i < PM3DS_TM_MAX; i++) {
                set_entry(i, p->nameMsg[i], p->enabled[i], costKind ? p->value[i] : 0, costKind);
                sMenu.e[i].icon = p->ptrIcon[i];
            }
            sMenu.cursor = PopupMenu_SelectedIndex;
            if (gGameStatusPtr->context == CONTEXT_BATTLE) {
                wheel_title();
            } else {
                const char* t = p->popupType == POPUP_MENU_SWITCH_PARTNER ? "Partners"
                              : p->popupType == POPUP_MENU_SELL_ITEM      ? "Sell"
                              : p->popupType == POPUP_MENU_TRADE_FOR_BADGE ? "Trade"
                                                                           : "Items";
                strcpy(sMenu.title, t);
                sNameMsg[PM3DS_TM_MAX] = -2;
            }
            break;
        }
        case PM3DS_TM_TARGET: {
            Actor* actor = sMenu.partner ? gBattleStatus.partnerActor : gBattleStatus.playerActor;
            n = actor->targetListLength;
            for (i = 0; i < n && i < PM3DS_TM_MAX; i++) {
                SelectableTarget* target = &actor->targetData[actor->targetIndexList[i]];
                Actor* t = get_actor(target->actorID);
                s32 msgID;
                if (target->actorID == ACTOR_PLAYER) {
                    msgID = MSG_Menus_Battle_TargetMario;
                } else if (target->actorID == ACTOR_PARTNER) {
                    msgID = PartnerNameMessages[(gBattleStatus.flags2 & BS_FLAGS2_PEACH_BATTLE) ? PARTNER_TWINK
                                                                                                 : gPlayerData.curPartner];
                } else {
                    msgID = get_actor_part(t, target->partID)->staticData->overrideNameMsg;
                    if (msgID == MSG_NONE) {
                        msgID = bActorNames[t->actorType];
                    }
                }
                set_entry(i, msgID, true, -1, PM3DS_TM_COST_NONE);
                if (t != nullptr && (target->actorID & ACTOR_CLASS_MASK) == ACTOR_CLASS_ENEMY &&
                    is_actor_health_bar_visible(t)) {
                    sMenu.e[i].cost = t->curHP;
                    sMenu.e[i].max_hp = t->maxHP;
                    sMenu.e[i].cost_kind = PM3DS_TM_COST_HP;
                }
                target_place(&sMenu.e[i], t, target);
            }
            sMenu.cursor = actor->selectedTargetIndex;
            sMenu.all_targets = !(gBattleStatus.curTargetListFlags & TARGET_FLAG_SELECT_ONE);
            strcpy(sMenu.title, "Target");
            sNameMsg[PM3DS_TM_MAX] = -2;
            break;
        }
    }
    sMenu.count = n < PM3DS_TM_MAX ? n : PM3DS_TM_MAX;
    sStamp = gGameStatusPtr->frameCounter;
}

/* ---- target positions ---------------------------------------------------------- */
/* Where a target stands on the top screen: the part's middle and its height,
 * through the battle camera, so the bottom screen can cut the stage around the
 * targets from the top screen's last frame. */
static void target_place(pm3ds_tm_entry_t* e, Actor* t, SelectableTarget* target) {
    extern void pm3ds_gfx_n64_to_top(float x, float y, float* tx, float* ty);
    ActorPart* part = t != nullptr ? get_actor_part(t, target->partID) : nullptr;
    f32 x, y, z, w, h;
    s32 sx, sy, sz, tx_, ty_, tz;
    float cx, cy, topx, topy;

    e->pr = 0;
    if (t == nullptr) {
        return;
    }
    e->enemy = (target->actorID & ACTOR_CLASS_MASK) == ACTOR_CLASS_ENEMY;
    if (part != nullptr && part->size.y != 0) {
        w = part->size.x; h = part->size.y;
    } else {
        w = t->size.x; h = t->size.y;
    }
    /* where the game's own target cursor goes (create_current_pos_target_list):
     * the top of the target as it stands now. A part's absolutePos can lag (a
     * Paratroopa knocked to the ground, Mario and the partner), GitHub #42 */
    x = target->truePos.x;
    y = target->truePos.y - h;
    z = target->truePos.z;
    get_screen_coords(CAM_BATTLE, x, y + h * 0.5f, z, &sx, &sy, &sz);
    get_screen_coords(CAM_BATTLE, x, y + h, z, &tx_, &ty_, &tz);
    pm3ds_gfx_n64_to_top(sx, sy, &cx, &cy);
    pm3ds_gfx_n64_to_top(tx_, ty_, &topx, &topy);
    {
        float half = fabsf(cy - topy);   /* half its height on screen, and its width */
        float hw = (w * 0.5f) * (half / (h * 0.5f > 1.0f ? h * 0.5f : 1.0f));
        if (hw > half) half = hw;
        half *= 1.35f;            /* some room around it */
        if (half < 14.0f) half = 14.0f;
        if (half > 90.0f) half = 90.0f;
        e->px = (int16_t)cx;
        e->py = (int16_t)cy;
        e->pr = (int16_t)half;
    }
}

/* ---- icons ------------------------------------------------------------------ */
/* The first image a HudScript shows: a CI4 raster and its 16-colour palette,
 * either in memory (hs_SetCI: the battle menu icons) or in the ROM's icon block
 * (hs_SetIcon: items and badges). Palettes stay big-endian RGBA5551 in memory,
 * as the N64 loads them. IA8 and RGBA images are left out (no menu uses them). */
int pm3ds_tm_icon_rgba(const void* script, uint8_t* out, int* w, int* h) {
    static u8 rom[2048];
    static u8 romPal[32];
    const s32* pos = script;
    const u8* raster = nullptr;
    const u8* pal = nullptr;
    s32 preset = 0, cw = 0, ch = 0;
    s32 sw, sh, x, y, i;

    if (pos == nullptr) {
        return 0;
    }
    for (i = 0; i < 64 && raster == nullptr; i++) {
        switch (*pos++) {
            case HUD_ELEMENT_OP_SetCI:
                raster = (const u8*)pos[1];
                pal = (const u8*)pos[2];
                break;
            case HUD_ELEMENT_OP_SetImage: {
                s32 size = cw ? cw * ch / 2 : HudElemSizes[preset].size;
                if (size <= 0 || size > (s32)sizeof(rom)) {
                    return 0;
                }
                nuPiReadRom((s32)icon_ROM_START + pos[1], rom, size);
                nuPiReadRom((s32)icon_ROM_START + pos[2], romPal, sizeof(romPal));
                raster = rom;
                pal = romPal;
                break;
            }
            case HUD_ELEMENT_OP_SetTileSize:
                preset = *pos++;
                break;
            case HUD_ELEMENT_OP_SetSizesAutoScale:
            case HUD_ELEMENT_OP_SetSizesFixedScale:
                preset = pos[0];
                pos += 2;
                break;
            case HUD_ELEMENT_OP_SetCustomSize:
                cw = pos[0];
                ch = pos[1];
                pos += 2;
                break;
            case HUD_ELEMENT_OP_SetFlags:
                if (*pos++ & HUD_ELEMENT_FLAG_FMT_IA8) {
                    return 0;
                }
                break;
            case HUD_ELEMENT_OP_AddTexelOffsetX:
            case HUD_ELEMENT_OP_AddTexelOffsetY:
            case HUD_ELEMENT_OP_SetScale:
            case HUD_ELEMENT_OP_SetAlpha:
            case HUD_ELEMENT_OP_SetVariable:
            case HUD_ELEMENT_OP_ClearFlags:
            case HUD_ELEMENT_OP_PlaySound:
                pos++;
                break;
            case HUD_ELEMENT_OP_SetTexelOffset:
            case HUD_ELEMENT_OP_RandomDelay:
            case HUD_ELEMENT_OP_RandomRestart:
            case HUD_ELEMENT_OP_SetPivot:
                pos += 2;
                break;
            case HUD_ELEMENT_OP_RandomBranch:
                pos += 1 + pos[0];
                break;
            case HUD_ELEMENT_OP_SetVisible:
            case HUD_ELEMENT_OP_SetHidden:
            case HUD_ELEMENT_OP_Loop:   // only marks where Restart jumps back to
                break;
            default:   // End, Restart, Delete, an RGBA or IA8 image: nothing to show
                return 0;
        }
    }
    if (raster == nullptr || pal == nullptr) {
        return 0;
    }
    sw = cw ? cw : HudElemSizes[preset].width;
    sh = ch ? ch : HudElemSizes[preset].height;
    if (sw <= 0 || sh <= 0) {
        return 0;
    }
    *w = sw > 32 ? 32 : sw;
    *h = sh > 32 ? 32 : sh;
    for (y = 0; y < *h; y++) {
        for (x = 0; x < *w; x++) {
            s32 sx = x * sw / *w, sy = y * sh / *h;
            s32 n = sy * sw + sx;
            s32 ci = (raster[n / 2] >> ((n & 1) ? 0 : 4)) & 15;
            u16 c = (pal[ci * 2] << 8) | pal[ci * 2 + 1];
            u8* d = out + (y * *w + x) * 4;
            d[0] = ((c >> 11) & 31) * 255 / 31;
            d[1] = ((c >> 6) & 31) * 255 / 31;
            d[2] = ((c >> 1) & 31) * 255 / 31;
            d[3] = (c & 1) ? 255 : 0;
        }
    }
    return 1;
}

/* ---- game hooks ------------------------------------------------------------ */
/* From a menu's choosing state, every frame: publish it; true when a tap on
 * it is waiting (then pm3ds_tm_index() is the entry and pm3ds_tm_confirm()
 * reports the "A press" once). */
bool pm3ds_tm_poll(s32 kind) {
    publish(kind);
    if (sPickKind != kind) {
        return false;
    }
    sPickKind = PM3DS_TM_NONE;
    if (gGameStatusPtr->frameCounter - sPickStamp > 20 || sPickIndex < 0 || sPickIndex >= sMenu.count) {
        return false;
    }
    sConfirm = true;
    return true;
}

s32 pm3ds_tm_index(void) {
    return sPickIndex;
}

bool pm3ds_tm_pending(void) {
    return sConfirm;
}

bool pm3ds_tm_confirm(void) {
    bool c = sConfirm;
    sConfirm = false;
    return c;
}

/* first visible row of a scrolling list so that row `sel` shows with one row
 * of context either side (the rule the game's lists follow) */
s32 pm3ds_tm_scroll(s32 sel, s32 first, s32 count, s32 visible) {
    if (sel < first + 1) {
        first = sel - 1;
    }
    if (sel >= first + visible - 1) {
        first = sel - visible + 2;
    }
    if (first > count - visible) {
        first = count - visible;
    }
    if (first < 0) {
        first = 0;
    }
    return first;
}

/* ---- bottom screen side ---------------------------------------------------- */
int pm3ds_tm_get(pm3ds_touch_menu_t* out) {
    if (gGameStatusPtr == nullptr || sMenu.kind == PM3DS_TM_NONE || gGameStatusPtr->frameCounter - sStamp > 4) {
        out->kind = PM3DS_TM_NONE;
        return PM3DS_TM_NONE;
    }
    *out = sMenu;
    return sMenu.kind;
}

void pm3ds_tm_pick(int kind, int index) {
    if (gGameStatusPtr == nullptr) {
        return;
    }
    sPickKind = kind;
    sPickIndex = index;
    sPickStamp = gGameStatusPtr->frameCounter;
}

/* n64_shim/os_cont.c, the "D-pad in menus" option: a menu is open and moving its cursor,
 * so the D-pad can be the stick there instead of the C buttons it carries elsewhere */
bool pm3ds_menu_open(void) {
    extern s16 WorldMenuState;
    if (gGameStatusPtr == nullptr) {
        return false;
    }
    switch (get_game_mode()) {
        case GAME_MODE_PAUSE:
        case GAME_MODE_FILE_SELECT:
        case GAME_MODE_LANGUAGE_SELECT:
            return true;
        case GAME_MODE_WORLD:
            if (WorldMenuState != 0) {   // the item and partner popups
                return true;
            }
            break;
    }
    return sMenu.kind != PM3DS_TM_NONE && gGameStatusPtr->frameCounter - sStamp <= 4;
}
