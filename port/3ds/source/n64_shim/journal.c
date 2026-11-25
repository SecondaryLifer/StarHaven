/*
 * journal.c — game-side half of the bottom screen's Tattle log and item tracker.
 *
 * Tattle log: every enemy Goombario has tattled, from the save's tattle bits
 * (GB_Tattles_00.., one per actor type, as battles set them) with the name and
 * tattle text the game shows. Item tracker: per area, how many of the item
 * spots the randomizer knows (rando_locations.inc: field items, blocks, hidden
 * blocks, trees, bushes, hidden panels, chests) have been collected, from the
 * same flags the scripts set. Gifts have no flag and are left out. Recipes:
 * every dish Tayce T. can cook, every way to cook it, and which ones she has
 * cooked (her own discovered flags).
 *
 * Plain structs out (pm3ds_hud_info.h), so platform/bottom_screen.c needs no
 * decomp headers.
 */
#include "common.h"
#include "battle/battle.h"
#include "hud_element.h"
#include "map.h"
#include "game_modes.h"
#include "message_ids.h"

#include <string.h>
#include <stdio.h>

#include "pm3ds_hud_info.h"
#include "pm3ds_rando.h"
#include "pm3ds_brand.h"
#include "pm3ds_rom_layout.h"
#include "pm3ds_keyitems.h"
#include "pm3ds_lang.h"

extern s32 bActorTattles[];
void pm3ds_decode_msg(s32 msgID, char* out, s32 size, bool dialogue);   /* touch_menu.c */

/* ---- tattle log ------------------------------------------------------------ */
#define NUM_ACTOR_TYPES (ACTOR_TYPE_SLOT_MACHINE_START_DUP3 + 1)

/* types whose tattle is its own message but with the same name and (up to
 * spaces and line breaks) the same text as another type: the flying
 * Magikoopas and the boss's copies. They log under the base type (GitHub #55). */
static const u8 sTattleAlias[][2] = {
    { ACTOR_TYPE_FLYING_MAGIKOOPA,        ACTOR_TYPE_MAGIKOOPA },
    { ACTOR_TYPE_FLYING_MAGICLONE,        ACTOR_TYPE_MAGICLONE },
    { ACTOR_TYPE_FLYING_RED_MAGIKOOPA,    ACTOR_TYPE_RED_MAGIKOOPA },
    { ACTOR_TYPE_FLYING_GREEN_MAGIKOOPA,  ACTOR_TYPE_GREEN_MAGIKOOPA },
    { ACTOR_TYPE_FLYING_YELLOW_MAGIKOOPA, ACTOR_TYPE_YELLOW_MAGIKOOPA },
    { ACTOR_TYPE_FLYING_GRAY_MAGIKOOPA,   ACTOR_TYPE_GRAY_MAGIKOOPA },
    { ACTOR_TYPE_FLYING_WHITE_MAGIKOOPA,  ACTOR_TYPE_WHITE_MAGIKOOPA },
    { ACTOR_TYPE_FLYING_MAGIKOOPA_BOSS,   ACTOR_TYPE_MAGIKOOPA_BOSS },
    { ACTOR_TYPE_MAGIKOOPA_DUP2,          ACTOR_TYPE_MAGIKOOPA_BOSS },
};

static s32 tattle_base_type(s32 t) {
    for (u32 i = 0; i < ARRAY_COUNT(sTattleAlias); i++) {
        if (sTattleAlias[i][0] == t) {
            return sTattleAlias[i][1];
        }
    }
    return t;
}

/* the message a type is logged under */
static s32 tattle_msg(s32 t) {
    return bActorTattles[tattle_base_type(t)];
}

/* one entry per tattle message (types that share one, share the entry), in
 * actor-type order */
static u8 sEntryType[NUM_ACTOR_TYPES];
static s32 sNumEntries = -1;
static char sEntryName[NUM_ACTOR_TYPES][PM3DS_TATTLE_NAME];

static void list_entries(void) {
    sNumEntries = 0;
    for (s32 t = 0; t < NUM_ACTOR_TYPES; t++) {
        s32 msg = tattle_msg(t);
        bool dup = false;
        if (msg == MSG_NONE || msg == 0) {
            continue;
        }
        for (s32 e = 0; e < sNumEntries && !dup; e++) {
            dup = tattle_msg(sEntryType[e]) == msg;
        }
        if (!dup) {
            sEntryType[sNumEntries++] = tattle_base_type(t);
        }
    }
}

static bool type_tattled(s32 t) {
    s32 bits = get_global_byte(EVT_INDEX_OF_GAME_BYTE(GB_Tattles_00) + t / 8);
    return (bits >> (t % 8)) & 1;
}

/* every type with this entry's message counts */
static bool entry_known(s32 e) {
    s32 msg = tattle_msg(sEntryType[e]);
    for (s32 t = 0; t < NUM_ACTOR_TYPES; t++) {
        if (tattle_msg(t) == msg && type_tattled(t)) {
            return true;
        }
    }
    return false;
}

int pm3ds_tattle_query(pm3ds_tattle_list_t* out) {
    out->known = 0;
    out->total = 0;
    if (gGameStatusPtr == nullptr) {
        return 0;
    }
    if (sNumEntries < 0) {
        list_entries();
    }
    out->total = sNumEntries;
    for (s32 e = 0; e < sNumEntries && out->known < PM3DS_TATTLE_MAX; e++) {
        if (!entry_known(e)) {
            continue;
        }
        if (sEntryName[e][0] == '\0') {
            pm3ds_decode_msg(bActorNames[sEntryType[e]], sEntryName[e], PM3DS_TATTLE_NAME, false);
        }
        out->entry[out->known] = e;
        memcpy(out->name[out->known], sEntryName[e], PM3DS_TATTLE_NAME);
        out->known++;
    }
    return out->known;
}

/* the counts alone, without decoding names (achievements.c) */
void pm3ds_tattle_count(int* known, int* total) {
    *known = *total = 0;
    if (gGameStatusPtr == nullptr) {
        return;
    }
    if (sNumEntries < 0) {
        list_entries();
    }
    *total = sNumEntries;
    for (s32 e = 0; e < sNumEntries; e++) {
        *known += entry_known(e);
    }
}

void pm3ds_tattle_text(int entry, char* out, int size) {
    out[0] = '\0';
    if (entry >= 0 && entry < sNumEntries) {
        pm3ds_decode_msg(bActorTattles[sEntryType[entry]], out, size, true);
    }
}

/* a message was just loaded (src/msg.c): hard mode shows its enemy stats in
 * the tattles, in battle and in the Log tab alike */
void pm3ds_msg_loaded(u32 msgID, void* dest, s32 cap) {
    extern bool pm3ds_hard_mode(void);
    void pm3ds_hard_tattle_stats(u8* m, s32 cap);   /* touch_menu.c */

    if (!pm3ds_hard_mode()) {
        return;
    }
    for (s32 t = 0; t < NUM_ACTOR_TYPES; t++) {
        if ((u32)bActorTattles[t] == msgID) {
            pm3ds_hard_tattle_stats(dest, cap);
            return;
        }
    }
}

/* ---- Chuck Quizmo (GitHub #80) ---------------------------------------------- */
/* GB_CompletedQuizzes counts the right answers, and question n is
 * MSG_QuizQuestion_01 + n with choices MSG_QuizChoice_01 + n; the right choices
 * as world/common/complete/Quizmo.inc.c has them (every map's copy is the same) */
void dma_load_msg(u32 msgID, void* dest);   /* src/msg.c */

static const u8 sQuizAnswers[PM3DS_QUIZ_COUNT] = {
    2, 1, 1, 2, 2, 0, 2, 0,
    2, 1, 0, 2, 1, 1, 0, 2,
    0, 2, 1, 0, 0, 2, 1, 0,
    2, 1, 1, 2, 2, 1, 1, 1,
    0, 2, 2, 2, 2, 0, 1, 1,
    2, 1, 2, 1, 2, 0, 0, 1,
    1, 0, 1, 2, 1, 0, 2, 2,
    1, 2, 0, 2, 2, 1, 1, 1,
};

void pm3ds_quiz_stats(int* done, int* tries, int* right) {
    *done = *tries = *right = 0;
    if (gGameStatusPtr == nullptr) {
        return;
    }
    s32 n = evt_get_variable(nullptr, GB_CompletedQuizzes);
    *done = n < 0 ? 0 : n > PM3DS_QUIZ_COUNT ? PM3DS_QUIZ_COUNT : n;
    *tries = gPlayerData.quizzesAnswered;
    *right = gPlayerData.quizzesCorrect;
}

int pm3ds_quiz_right(int q) {
    return q >= 0 && q < PM3DS_QUIZ_COUNT ? sQuizAnswers[q] : 0;
}

void pm3ds_quiz_question(int q, char* out, int size) {
    static u8 buf[0x400 + 1];
    out[0] = '\0';
    if (q < 0 || q >= PM3DS_QUIZ_COUNT) {
        return;
    }
    dma_load_msg(MSG_QuizQuestion_01 + q, buf);
    buf[0x400] = MSG_CHAR_READ_END;
    // the first page only says "Question.": start after it
    s32 i = 0;
    while (i < 0x400 && buf[i] != MSG_CHAR_READ_NEXT && buf[i] != MSG_CHAR_READ_END) {
        i++;
    }
    pm3ds_decode_msg((s32)(buf[i] == MSG_CHAR_READ_NEXT ? buf + i + 1 : buf), out, size, true);
}

int pm3ds_quiz_options(int q, char out[3][PM3DS_QUIZ_OPT]) {
    static u8 buf[0x400], one[96];
    s32 n = 0;
    if (q < 0 || q >= PM3DS_QUIZ_COUNT) {
        return 0;
    }
    dma_load_msg(MSG_QuizChoice_01 + q, buf);
    for (s32 k = 0; k < 3; k++) {
        out[k][0] = '\0';
        // each choice's text follows its [Option k] (0xFF 0x21 k) up to the line's end
        for (s32 i = 0; i + 2 < (s32)sizeof(buf) && buf[i] != MSG_CHAR_READ_END; i++) {
            if (buf[i] != MSG_CHAR_READ_FUNCTION || buf[i + 1] != MSG_READ_FUNC_OPTION || buf[i + 2] != k) {
                continue;
            }
            // up to the line's end; function codes (the iQue's [Right 8] in front of a
            // choice) are copied with their arguments and the decoder skips them
            extern s32 pm3ds_msg_func_args(u8 f);
            s32 len = 0;
            for (s32 j = i + 3; j < (s32)sizeof(buf) && len < (s32)sizeof(one) - 1;) {
                u8 b = buf[j];
                if (b == MSG_CHAR_READ_FUNCTION) {
                    s32 n = j + 1 < (s32)sizeof(buf) ? pm3ds_msg_func_args(buf[j + 1]) : -1;
                    if (n < 0 || buf[j + 1] == MSG_READ_FUNC_OPTION || len + 2 + n >= (s32)sizeof(one) - 1 ||
                        j + 2 + n > (s32)sizeof(buf)) {
                        break;
                    }
                    for (s32 k = 0; k < 2 + n; k++) {
                        one[len++] = buf[j++];
                    }
                    continue;
                }
                // spaces stay (the decoder drops leading ones): the US and EU choices have
                // them between words, the iQue's pad some choices with them (GitHub #91)
                if (b >= 0xF0 && b != MSG_CHAR_READ_SPACE && b != MSG_CHAR_READ_FULL_SPACE &&
                    b != MSG_CHAR_READ_HALF_SPACE) {
                    break;
                }
                one[len++] = b;
                j++;
            }
            one[len] = MSG_CHAR_READ_END;
            pm3ds_decode_msg((s32)one, out[k], PM3DS_QUIZ_OPT, false);
            n = k + 1;
            break;
        }
    }
    return n;
}

/* ---- item tracker ---------------------------------------------------------- */
enum { SRC_ITEM, SRC_BLOCK, SRC_TREE, SRC_BUSH, SRC_PANEL, SRC_CHEST, SRC_GIFT, SRC_HIDDEN_BLOCK,   /* rando.c */
       SRC_CRATE, SRC_SUPER_BLOCK };                                                                  /* tracker only */

typedef struct {
    s32 flag;
    s16 item;
    u8 src;
    const char* map;
} TrackedSpot;

#define LOC(flag, item, src, map, kav, chapter) { flag, item, src, map },
static const TrackedSpot sSpots[] = {
#if VERSION_US
#include "rando_locations_us.inc"
#elif VERSION_JP
#include "rando_locations_jp.inc"
#elif VERSION_IQUE
#include "rando_locations_ique.inc"
#else
#include "rando_locations.inc"
#endif
    /* not randomizer spots, but things to find all the same: items in crates
     * (GitHub #44; the obk_05 apples have no flag and come back) and the Super
     * Blocks (GitHub #43) */
    { GF_KPA17_Crate_TastyTonic, ITEM_TASTY_TONIC, SRC_CRATE, "kpa_17" },
    { GF_KPA17_Crate_LifeShroom, ITEM_LIFE_SHROOM, SRC_CRATE, "kpa_17" },
    { GF_OBK03_Crate_SuperShroom, ITEM_SUPER_SHROOM, SRC_CRATE, "obk_03" },
    { GF_OBK04_Crate_MapleSyrup, ITEM_MAPLE_SYRUP, SRC_CRATE, "obk_04" },
    { GF_OBK06_Crate_StarPiece, ITEM_STAR_PIECE, SRC_CRATE, "obk_06" },
    { GF_TIK07_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "tik_07" },
    { GF_TIK10_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "tik_10" },
    { GF_TIK02_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "tik_12" },
    { GF_TIK17_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "tik_17" },
    { GF_TIK19_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "tik_19" },
    { GF_IWA10_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "iwa_10" },
    { GF_SBK56_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "sbk_56" },
    { GF_ISK10_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "isk_10" },
    { GF_DGB04_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "dgb_04" },
    { GF_OMO11_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "omo_11" },
    { GF_JAN08_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "jan_08" },
    { GF_KZN04_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "kzn_04" },
    { GF_KZN09_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "kzn_09" },
    { GF_FLO08_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "flo_08" },
    { GF_FLO16_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "flo_16" },
    { GF_SAM08_SuperBlock, ITEM_NONE, SRC_SUPER_BLOCK, "sam_08" },
};
#undef LOC

/* story order */
static const struct { const char* prefix; const char* name; } sTrackedAreas[] = {
    { "kmr", "Goomba Region" },   { "mac", "Toad Town" },        { "tik", "Toad Town Tunnels" },
    { "nok", "Koopa Region" },    { "trd", "Koopa Bros. Fortress" }, { "iwa", "Mt. Rugged" },
    { "dro", "Dry Dry Outpost" }, { "sbk", "Dry Dry Desert" },   { "isk", "Dry Dry Ruins" },
    { "mim", "Forever Forest" },  { "obk", "Boo's Mansion" },    { "arn", "Gusty Gulch" },
    { "dgb", "Tubba's Castle" },  { "omo", "Shy Guy's Toy Box" }, { "jan", "Lavalava Island" },
    { "kzn", "Mt. Lavalava" },    { "flo", "Flower Fields" },    { "sam", "Shiver Region" },
    { "pra", "Crystal Palace" },  { "kpa", "Bowser's Castle" },  { "kkj", "Peach's Castle" },
    { "hos", "Star Region" },     { "osr", "Castle Grounds" },
};

/* "Mt. Rugged (iwa_03)" for a map name: the randomizer's spoiler log listed bare map
 * names, which players can't place (Discord) */
const char* pm3ds_map_place(const char* map, char* buf, int size) {
    for (u32 i = 0; i < ARRAY_COUNT(sTrackedAreas); i++) {
        if (strncmp(map, sTrackedAreas[i].prefix, 3) == 0) {
            snprintf(buf, size, "%s (%s)", sTrackedAreas[i].name, map);
            return buf;
        }
    }
    snprintf(buf, size, "%s", map);
    return buf;
}

MapConfig* get_current_map_config(void);

/* spots whose map clears their flag again, so the item comes back (GitHub #41):
 * they never stay found, so they don't count */
static const s32 sRegrowing[] = {
    GF_KMR02_Tree1_Goomnut, GF_NOK01_Bush4_KoopaLeaf, GF_NOK02_Bush1_KoopaLeaf,
    GF_SAM02_Item_IcedPotato, GF_SBK56_Tree1_Lemon, GF_SBK56_Tree2_Lime,
};

static bool regrows(s32 flag) {
    for (u32 i = 0; i < ARRAY_COUNT(sRegrowing); i++) {
        if (sRegrowing[i] == flag) {
            return true;
        }
    }
    return false;
}

int pm3ds_tracker_query(pm3ds_tracker_t* out) {
    memset(out, 0, sizeof(*out));
    out->here = -1;
    if (gGameStatusPtr == nullptr) {
        return 0;
    }
    out->rando = pm3ds_rando_seed() != 0;
    MapConfig* mc = gGameStatusPtr->areaID >= 0 ? get_current_map_config() : nullptr;
    const char* mapID = mc != nullptr ? mc->id : nullptr;
    for (u32 a = 0; a < ARRAY_COUNT(sTrackedAreas) && out->count < PM3DS_TRACKER_AREAS; a++) {
        pm3ds_tracker_area_t* r = &out->area[out->count];
        snprintf(r->name, sizeof(r->name), "%s", sTrackedAreas[a].name);
        for (u32 i = 0; i < ARRAY_COUNT(sSpots); i++) {
            const TrackedSpot* s = &sSpots[i];
            if (s->flag == 0 || strncmp(s->map, sTrackedAreas[a].prefix, 3) != 0 || regrows(s->flag)) {
                continue;
            }
            bool got = evt_get_variable(nullptr, s->flag) != 0;
            if (mapID != nullptr && strcmp(s->map, mapID) == 0) {
                out->room_total++;
                out->room_found += got;
            }
            r->total++;
            r->found += got;
            if (s->item == ITEM_STAR_PIECE) {
                r->sp_total++;
                r->sp_found += got;
            } else if (s->item != ITEM_NONE && (gItemTable[s->item].typeFlags & ITEM_TYPE_FLAG_BADGE)) {
                r->badge_total++;
                r->badge_found += got;
            }
            if (s->src == SRC_HIDDEN_BLOCK || s->src == SRC_PANEL) {
                r->hidden_total++;
                r->hidden_found += got;
            }
        }
        if (r->total == 0) {
            continue;
        }
        if (mapID != nullptr && strncmp(mapID, sTrackedAreas[a].prefix, 3) == 0) {
            out->here = out->count;
        }
        out->found += r->found;
        out->total += r->total;
        out->count++;
    }
    return out->count;
}

static const void* item_hud_script(s32 item);

/* the spots of the area that is out->area[area] in pm3ds_tracker_query (areas without spots skipped) */
int pm3ds_tracker_spots(int area, pm3ds_tracker_spots_t* out) {
    extern s32 pm3ds_rando_spot_item(s32 flag, s32 vanillaItem);
    memset(out, 0, sizeof(*out));
    if (gGameStatusPtr == nullptr) {
        return 0;
    }
    MapConfig* mc = gGameStatusPtr->areaID >= 0 ? get_current_map_config() : nullptr;
    const char* mapID = mc != nullptr ? mc->id : nullptr;
    s32 n = -1;
    for (u32 a = 0; a < ARRAY_COUNT(sTrackedAreas); a++) {
        bool any = false;
        for (u32 i = 0; i < ARRAY_COUNT(sSpots) && !any; i++) {
            any = sSpots[i].flag != 0 && strncmp(sSpots[i].map, sTrackedAreas[a].prefix, 3) == 0 && !regrows(sSpots[i].flag);
        }
        if (!any || ++n != area) {
            continue;
        }
        for (s32 pass = 0; pass < 2; pass++) {          // Mario's room first
            for (u32 i = 0; i < ARRAY_COUNT(sSpots) && out->count < PM3DS_TRACKER_SPOTS; i++) {
                const TrackedSpot* s = &sSpots[i];
                if (s->flag == 0 || strncmp(s->map, sTrackedAreas[a].prefix, 3) != 0 || regrows(s->flag)) {
                    continue;
                }
                bool here = mapID != nullptr && strcmp(s->map, mapID) == 0;
                if (here != (pass == 0)) {
                    continue;
                }
                typeof(out->spot[0])* o = &out->spot[out->count++];
                s32 item = s->item != ITEM_NONE ? pm3ds_rando_spot_item(s->flag, s->item) : ITEM_NONE;
                if (item == ITEM_COIN) {
                    snprintf(o->item, sizeof(o->item), "Coin");   /* its name message is empty; the menu translates */
                    o->icon = item_hud_script(item);
                } else if (item != ITEM_NONE) {
                    pm3ds_decode_msg(gItemTable[item].nameMsg, o->item, sizeof(o->item), false);
                    o->icon = item_hud_script(item);
                }
                o->src = s->src;
                o->found = evt_get_variable(nullptr, s->flag) != 0;
                o->here = here;
                o->room = (unsigned char)atoi(s->map + 4);
            }
        }
        return out->count;
    }
    return 0;
}

/* ---- recipes --------------------------------------------------------------- */
/* Tayce T.'s tables (src/world/area_mac/mac_02/npc/tayce_t.inc.c, recipes.inc.c);
 * the game sets one discovered flag per dish it hands out */
typedef struct { s32 quality, itemID; } RecipeResult;
typedef struct { s32 inputA, inputB, output; } RecipePair;
extern s32 CookingIngredients[];
extern RecipeResult CookingResults[];
extern s8 SingleRecipesWithoutCookbook[], SingleRecipesWithCookbook[], DoubleRecipesMatrix[];
extern RecipePair ExtraDoubleRecipes[];
extern s32 MysteryResultOptions[];
extern const s32 CookableItemIDs[], CookableDiscoveredFlags[];
extern const s32 pm3ds_RecipeTableSizes[];
enum { RT_INGREDIENTS, RT_RESULTS, RT_EXTRAS, RT_MYSTERY, RT_COOKABLE };

static bool recipe_mystery(s32 item) {
    for (s32 i = 0; i < pm3ds_RecipeTableSizes[RT_MYSTERY]; i++) {
        if (MysteryResultOptions[i] == item) {
            return true;
        }
    }
    return false;
}

/* calls add(a, b, cookbook, ctx) for every way to cook item; b = ITEM_NONE: on its own */
static s32 recipe_ways(s32 item, void (*add)(s32 a, s32 b, bool cookbook, void* ctx), void* ctx) {
    s32 n = pm3ds_RecipeTableSizes[RT_INGREDIENTS], ways = 0;
    for (s32 i = 0; i < n; i++) {
        bool plain = CookingResults[SingleRecipesWithoutCookbook[i]].itemID == item && SingleRecipesWithoutCookbook[i] != 0;
        bool book = CookingResults[SingleRecipesWithCookbook[i]].itemID == item && SingleRecipesWithCookbook[i] != 0;
        if (plain || book) {
            if (add) add(CookingIngredients[i], ITEM_NONE, !plain, ctx);
            ways++;
        }
    }
    for (s32 a = 0; a < n; a++) {
        for (s32 b = a; b < n; b++) {
            s32 r = DoubleRecipesMatrix[b + a * n];
            if (r > 0 && CookingResults[r].itemID == item) {
                if (add) add(CookingIngredients[a], CookingIngredients[b], true, ctx);
                ways++;
            }
        }
    }
    for (s32 i = 0; i < pm3ds_RecipeTableSizes[RT_EXTRAS]; i++) {
        if (CookingResults[ExtraDoubleRecipes[i].output].itemID == item) {
            if (add) add(ExtraDoubleRecipes[i].inputA, ExtraDoubleRecipes[i].inputB, true, ctx);
            ways++;
        }
    }
    return ways;
}

static const void* item_hud_script(s32 item) {
    extern IconHudScriptPair gItemHudScripts[];
    return gItemHudScripts[gItemTable[item].hudElemID].enabled;
}

static s32 sRecipeItem[PM3DS_RECIPE_MAX], sRecipeFlag[PM3DS_RECIPE_MAX];
static char sRecipeName[PM3DS_RECIPE_MAX][PM3DS_RECIPE_NAME];
static s32 sNumRecipes = -1;

/* the dishes that can be cooked at all (a few discovered flags never get set),
 * sorted by name */
static void list_recipes(void) {
    sNumRecipes = 0;
    for (s32 i = 0; i < pm3ds_RecipeTableSizes[RT_COOKABLE] && sNumRecipes < PM3DS_RECIPE_MAX; i++) {
        s32 item = CookableItemIDs[i];
        if (item != ITEM_MISTAKE && recipe_ways(item, nullptr, nullptr) == 0 && !recipe_mystery(item)) {
            continue;
        }
        char name[PM3DS_RECIPE_NAME];
        pm3ds_decode_msg(gItemTable[item].nameMsg, name, sizeof(name), false);
        s32 k = sNumRecipes++;
        while (k > 0 && strcmp(sRecipeName[k - 1], name) > 0) {
            sRecipeItem[k] = sRecipeItem[k - 1];
            sRecipeFlag[k] = sRecipeFlag[k - 1];
            memcpy(sRecipeName[k], sRecipeName[k - 1], PM3DS_RECIPE_NAME);
            k--;
        }
        sRecipeItem[k] = item;
        sRecipeFlag[k] = CookableDiscoveredFlags[i];
        memcpy(sRecipeName[k], name, PM3DS_RECIPE_NAME);
    }
}

int pm3ds_recipe_query(pm3ds_recipe_list_t* out) {
    out->count = 0;
    out->cooked = 0;
    if (gGameStatusPtr == nullptr) {
        return 0;
    }
    if (sNumRecipes < 0) {
        list_recipes();
    }
    for (s32 i = 0; i < sNumRecipes; i++) {
        out->item[i] = sRecipeItem[i];
        out->done[i] = evt_get_variable(nullptr, sRecipeFlag[i]) != 0;
        out->icon[i] = item_hud_script(sRecipeItem[i]);
        memcpy(out->name[i], sRecipeName[i], PM3DS_RECIPE_NAME);
        out->cooked += out->done[i];
    }
    out->count = sNumRecipes;
    return out->count;
}

/* the counts alone (achievements.c) */
void pm3ds_recipe_counts(int* cooked, int* total) {
    *cooked = *total = 0;
    if (gGameStatusPtr == nullptr) {
        return;
    }
    if (sNumRecipes < 0) {
        list_recipes();
    }
    for (s32 i = 0; i < sNumRecipes; i++) {
        *cooked += evt_get_variable(nullptr, sRecipeFlag[i]) != 0;
    }
    *total = sNumRecipes;
}

/* ---- Parakarry's letters and Koopa Koot's favors ------------------------------ */
/* A delivered letter leaves no flag: Parakarry hands it over and it leaves the key
 * items (world/common/complete/LetterDelivery.inc.c). So a letter is delivered once
 * Mario has had it and has it no more, and which ones he has had is kept (a bit each,
 * saved by achievements.c): seen in the key items (the three Parakarry lost on Mt.
 * Rugged only once he has joined and handed them back: before that Mario gives them
 * to him), a found one's spot taken (whichever spot holds it in a randomizer seed),
 * and for a letter of Goompapa's chain, the next one: each recipient hands it over.
 * A save from before this was kept proves what its flags prove when it is loaded:
 * Parakarry's three were handed back if he has joined, Lucky Day (not in a randomizer
 * seed) ends the chain. */
#define LETTER_PARAKARRY -2   /* Mt. Rugged's three */
#define LETTER_FOUND -1
typedef struct {
    s16 item;
    s8 prev;            /* the chain letter before it, or LETTER_* */
    const char* from;   /* English (T) */
} LetterDef;

static const LetterDef sLetters[PM3DS_LETTER_COUNT] = {
    { ITEM_LETTER_TO_MERLON,          LETTER_PARAKARRY, "From Merlee" },
    { ITEM_LETTER_TO_KOLORADO,        LETTER_PARAKARRY, "From his wife" },
    { ITEM_LETTER_CHAIN_GOOMPAPA_1,   LETTER_PARAKARRY, "From a fishing pal" },
    { ITEM_LETTER_TO_GOOMPA,          LETTER_FOUND, "From Koopa Koot" },
    { ITEM_LETTER_TO_MORT_T,          LETTER_FOUND, "From his wife" },
    { ITEM_LETTER_TO_NOMADIMOUSE,     LETTER_FOUND, "From Moustafa" },
    { ITEM_LETTER_TO_FICE_T,          LETTER_FOUND, "From Bootler" },
    { ITEM_LETTER_TO_IGOR,            LETTER_FOUND, "An order from Gusty Gulch" },
    { ITEM_LETTER_TO_RUSS_T,          LETTER_FOUND, "From a kid on Lavalava Island" },
    { ITEM_LETTER_TO_MINH_T,          LETTER_FOUND, "From the Wise Wisterwood" },
    { ITEM_LETTER_TO_MAYOR_PENGUIN,   LETTER_FOUND, "From Yoshi's Village" },
    { ITEM_LETTER_TO_MERLOW,          LETTER_FOUND, "From Merle, his dad" },
    { ITEM_LETTER_CHAIN_MUSS_T,       2, "From Goompapa" },
    { ITEM_LETTER_CHAIN_KOOVER_1,     12, "From Muss T." },
    { ITEM_LETTER_CHAIN_FISHMAEL,     13, "From Koover" },
    { ITEM_LETTER_CHAIN_KOOVER_2,     14, "From Fishmael" },
    { ITEM_LETTER_CHAIN_MR_E,         15, "From Koover" },
    { ITEM_LETTER_CHAIN_MISS_T,       16, "From Mr. E." },
    { ITEM_LETTER_CHAIN_LITTLE_MOUSER, 17, "From Miss T." },
    { ITEM_LETTER_CHAIN_FRANKY,       18, "From Little Mouser" },
    { ITEM_LETTER_CHAIN_DANE_T_1,     19, "From Franky" },
    { ITEM_LETTER_CHAIN_YOSHI_KID,    20, "From Dane T." },
    { ITEM_LETTER_CHAIN_DANE_T_2,     21, "From the Red Yoshi Kid" },
    { ITEM_LETTER_CHAIN_FROST_T,      22, "From Dane T." },
    { ITEM_LETTER_CHAIN_GOOMPAPA_2,   23, "From Frost T." },
};
#define LETTER_CHAIN_LAST 24
_Static_assert(PM3DS_LETTER_COUNT <= 32, "a bit per letter");

static bool key_held(s32 item) {
    for (s32 i = 0; i < KEY_SLOTS; i++) {
        if (KEY_ITEM(i) == item) {
            return true;
        }
    }
    return false;
}

/* the spot that holds this item (the randomizer's, in a seed) has been taken */
static bool spot_taken(s32 item) {
    extern s32 pm3ds_rando_spot_item(s32 flag, s32 vanillaItem);
    for (u32 i = 0; i < ARRAY_COUNT(sSpots); i++) {
        const TrackedSpot* s = &sSpots[i];
        if (s->flag != 0 && s->item != ITEM_NONE && pm3ds_rando_spot_item(s->flag, s->item) == item &&
            evt_get_variable(nullptr, s->flag) != 0) {
            return true;
        }
    }
    return false;
}

static u32 sLetterHad;       /* bit per sLetters entry: Mario has had it */
static bool sLetterLoaded;   /* a file was just loaded */

/* achievements.c keeps the bits in the save */
u32 pm3ds_letters_had(void) {
    return sLetterHad;
}

void pm3ds_letters_had_set(u32 had, bool loaded) {
    sLetterHad = had;
    sLetterLoaded = loaded;
}

/* states of all letters (out[i]: PM3DS_ERRAND_*), seen[i]: found at least once */
static void letter_states(u8* out, u8* seen) {
    bool joined = evt_get_variable(nullptr, GB_StoryProgress) >= STORY_CH2_PARAKARRY_JOINED_PARTY;
    bool held[PM3DS_LETTER_COUNT], found[PM3DS_LETTER_COUNT];
    for (s32 i = 0; i < PM3DS_LETTER_COUNT; i++) {
        const LetterDef* l = &sLetters[i];
        held[i] = key_held(l->item);
        found[i] = l->prev < 0 && spot_taken(l->item);
        if ((held[i] && (l->prev != LETTER_PARAKARRY || joined)) || (l->prev == LETTER_FOUND && found[i]) ||
            (l->prev == LETTER_PARAKARRY && joined && sLetterLoaded)) {
            sLetterHad |= 1u << i;
        }
    }
    if (sLetterLoaded && pm3ds_rando_seed() == 0) {
        for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.badges); i++) {
            if (gPlayerData.badges[i] == ITEM_LUCKY_DAY) {   /* Goompapa's reward for the chain's last letter */
                sLetterHad |= 1u << LETTER_CHAIN_LAST;
            }
        }
    }
    sLetterLoaded = false;
    for (s32 i = PM3DS_LETTER_COUNT - 1; i >= 0; i--) {   /* sLetters lists the chain in order */
        if (sLetters[i].prev >= 0 && (sLetterHad & (1u << i))) {
            sLetterHad |= 1u << sLetters[i].prev;
        }
    }
    for (s32 i = 0; i < PM3DS_LETTER_COUNT; i++) {
        bool had = (sLetterHad >> i) & 1;
        out[i] = held[i] ? PM3DS_ERRAND_HELD : had ? PM3DS_ERRAND_DONE : PM3DS_ERRAND_NOT_YET;
        seen[i] = held[i] || had || found[i];
    }
}

typedef struct {
    s16 item;           /* ITEM_NONE: what_en says what */
    const char* what;
} FavorDef;

/* src/world/area_nok/nok_02/koot_favors.c: KootFavorData, in KOOT_FAVOR_* order; each
 * favor's unlocked, gave up and complete flags follow one another from KootFavor00's */
static const FavorDef sFavors[PM3DS_FAVOR_COUNT] = {
    { ITEM_KOOT_KOOPA_LEGENDS, nullptr },  { ITEM_SLEEPY_SHEEP, nullptr },
    { ITEM_KOOT_THE_TAPE, nullptr },       { ITEM_KOOPA_TEA, nullptr },
    { ITEM_KOOT_LUIGI_AUTOGRAPH, nullptr }, { ITEM_KOOT_EMPTY_WALLET, nullptr },
    { ITEM_TASTY_TONIC, nullptr },         { ITEM_KOOT_MERLUVLEE_AUTOGRAPH, nullptr },
    { ITEM_NONE, "Read him the Toad Town news" }, { ITEM_LIFE_SHROOM, nullptr },
    { ITEM_NUTTY_CAKE, nullptr },          { ITEM_NONE, "Quiet the noisy house next door" },
    { ITEM_KOOT_OLD_PHOTO, nullptr },      { ITEM_KOOPASTA, nullptr },
    { ITEM_KOOT_GLASSES, nullptr },        { ITEM_LIME, nullptr },
    { ITEM_KOOKY_COOKIE, nullptr },        { ITEM_KOOT_PACKAGE, nullptr },
    { ITEM_COCONUT, nullptr },             { ITEM_KOOT_RED_JAR, nullptr },
};

static u8 favor_state(s32 i, bool* seen, bool* current) {
    bool unlocked = evt_get_variable(nullptr, GF_NOK02_KootFavor00_Unlocked + 3 * i) != 0;
    bool gaveUp = evt_get_variable(nullptr, GF_NOK02_KootFavor00_GaveUp + 3 * i) != 0;
    bool done = evt_get_variable(nullptr, GF_NOK02_KootFavor00_Complete + 3 * i) != 0;
    *current = !done && evt_get_variable(nullptr, GB_KootFavor_State) == 2 &&   /* KOOT_FAVOR_STATE_2: accepted */
               evt_get_variable(nullptr, GB_KootFavor_Current) == i;
    *seen = unlocked || gaveUp || done;
    return done ? PM3DS_ERRAND_DONE : *current ? PM3DS_ERRAND_HELD : gaveUp ? PM3DS_ERRAND_GAVE_UP : PM3DS_ERRAND_NOT_YET;
}

void pm3ds_letter_counts(int* delivered, int* total, bool* chainDone) {
    u8 st[PM3DS_LETTER_COUNT], seen[PM3DS_LETTER_COUNT];
    *delivered = *total = 0;
    *chainDone = false;
    if (gGameStatusPtr == nullptr) {
        return;
    }
    letter_states(st, seen);
    for (s32 i = 0; i < PM3DS_LETTER_COUNT; i++) {
        *delivered += st[i] == PM3DS_ERRAND_DONE;
    }
    *total = PM3DS_LETTER_COUNT;
    *chainDone = st[LETTER_CHAIN_LAST] == PM3DS_ERRAND_DONE;
}

void pm3ds_favor_counts(int* done, int* total) {
    *done = *total = 0;
    if (gGameStatusPtr == nullptr) {
        return;
    }
    for (s32 i = 0; i < PM3DS_FAVOR_COUNT; i++) {
        bool seen, current;
        *done += favor_state(i, &seen, &current) == PM3DS_ERRAND_DONE;
    }
    *total = PM3DS_FAVOR_COUNT;
}

/* decoded once */
static char sLetterTo[PM3DS_LETTER_COUNT][PM3DS_UI_DESC];
static char sFavorWhat[PM3DS_FAVOR_COUNT][PM3DS_UI_NAME];

/* who the letter is for: the first sentence of its description ("A letter to Mr. E. in Dry
 * Dry Outpost.": the sentence ends before the one naming Parakarry, as in every language;
 * names have full stops of their own). Chinese: up to the first full stop. */
static void letter_who(char* s) {
    static char karry[PM3DS_UI_NAME];
    if (karry[0] == '\0') {
        pm3ds_decode_msg(MSG_Menus_Party_Parakarry, karry, sizeof(karry), false);
    }
    for (s32 i = 0; s[i] != '\0'; i++) {
        if (s[i] == PM3DS_TXT_ESC && s[i + 1] && s[i + 2] && s[i + 3]) {
            u32 cp = ((u32)(s[i + 1] & 0x7F) << 14) | ((u32)(s[i + 2] & 0x7F) << 7) | (s[i + 3] & 0x7F);
            if (cp == 0x3002) {   /* 。 */
                s[i + 4] = '\0';
                return;
            }
            i += 3;
        }
    }
    char* end = karry[0] != '\0' ? strstr(s, karry) : nullptr;
    s32 cut = -1;
    for (s32 i = 0; end != nullptr && s + i < end; i++) {
        if ((s[i] == '.' || s[i] == '!') && s[i + 1] == ' ') {
            cut = i;
        }
    }
    if (cut >= 0) {
        s[cut + 1] = '\0';
    }
}

void pm3ds_errands_query(pm3ds_errands_t* out) {
    u8 st[PM3DS_LETTER_COUNT], seen[PM3DS_LETTER_COUNT];
    memset(out, 0, sizeof(*out));
    if (gGameStatusPtr == nullptr) {
        return;
    }
    letter_states(st, seen);
    for (s32 i = 0; i < PM3DS_LETTER_COUNT; i++) {
        s32 item = sLetters[i].item;
        if (sLetterTo[i][0] == '\0') {
            pm3ds_decode_msg(gItemTable[item].fullDescMsg, sLetterTo[i], PM3DS_UI_DESC, false);
            letter_who(sLetterTo[i]);
        }
        memcpy(out->letter[i].to, sLetterTo[i], PM3DS_UI_DESC);
        out->letter[i].from = sLetters[i].from;
        out->letter[i].icon = item_hud_script(item);
        out->letter[i].state = st[i];
        out->letter[i].seen = seen[i];
        out->delivered += st[i] == PM3DS_ERRAND_DONE;
    }
    for (s32 i = 0; i < PM3DS_FAVOR_COUNT; i++) {
        bool seenF, current;
        s32 item = sFavors[i].item;
        out->favor[i].state = favor_state(i, &seenF, &current);
        out->favor[i].seen = seenF;
        out->favor[i].current = current;
        if (item != ITEM_NONE) {
            if (sFavorWhat[i][0] == '\0') {
                pm3ds_decode_msg(gItemTable[item].nameMsg, sFavorWhat[i], PM3DS_UI_NAME, false);
            }
            memcpy(out->favor[i].what, sFavorWhat[i], PM3DS_UI_NAME);
            out->favor[i].icon = item_hud_script(item);
        } else {
            out->favor[i].what_en = sFavors[i].what;
            out->favor[i].icon = item_hud_script(ITEM_KOOT_PACKAGE);
        }
        out->favors_done += out->favor[i].state == PM3DS_ERRAND_DONE;
    }
}

/* ---- Badges ------------------------------------------------------------------ */
s32 pause_badges_try_equip(s16 badgeID);
s32 pause_badges_try_remove(s16 badgeID);
s32 pause_get_total_equipped_bp_cost(void);
void enforce_hpfp_limits(void);
s32 pm3ds_cheats_badge_bp(s32 bp);

static char sBadgeName[ITEM_LAST_BADGE + 1][PM3DS_BADGE_NAME];   /* decoded on first use */

static bool badge_can_change(void) {
    return gGameStatusPtr != nullptr && get_game_mode() == GAME_MODE_WORLD;   /* not in battle or a menu */
}

static bool badge_on(s32 item) {
    for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.equippedBadges); i++) {
        if (gPlayerData.equippedBadges[i] == item) {
            return true;
        }
    }
    return false;
}

int pm3ds_badge_query(pm3ds_badge_list_t* out) {
    out->count = 0;
    if (gGameStatusPtr == nullptr) {
        return 0;
    }
    for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.badges) && out->count < PM3DS_BADGE_MAX; i++) {
        s32 item = gPlayerData.badges[i];
        if (!IS_BADGE(item)) {
            continue;
        }
        /* in the pause menu's order (pause_badges_comparator), by insertion */
        s32 k = out->count++;
        while (k > 0 && gItemTable[out->item[k - 1]].sortValue > gItemTable[item].sortValue) {
            out->item[k] = out->item[k - 1];
            k--;
        }
        out->item[k] = item;
    }
    for (s32 k = 0; k < out->count; k++) {
        s32 item = out->item[k];
        if (sBadgeName[item][0] == 0) {
            pm3ds_decode_msg(gItemTable[item].nameMsg, sBadgeName[item], PM3DS_BADGE_NAME, false);
        }
        memcpy(out->name[k], sBadgeName[item], PM3DS_BADGE_NAME);
        out->cost[k] = gMoveTable[gItemTable[item].moveID].costBP;
        out->on[k] = badge_on(item);
        out->icon[k] = item_hud_script(item);
    }
    out->bp_max = pm3ds_cheats_badge_bp(gPlayerData.maxBP);
    out->bp_used = pause_get_total_equipped_bp_cost();
    out->can_change = badge_can_change();
    return out->count;
}

/* Badge loadouts (Discord suggestion): three sets of equipped badges per save
 * file, in <save name>.badges next to the save on the SD card (the save's own
 * tail is full). A new game on a file slot clears its loadouts. */
#define LOADOUT_MAGIC 0x5348424C   /* "SHBL" */
#define LOADOUT_PATH PM3DS_SD_DIR "/" PM3DS_SAVE_NAME ".badges"
typedef struct {
    s32 magic, version;
    s16 badge[4][PM3DS_BADGE_LOADOUTS][64];   /* file slot, loadout, equipped badges (0 ends) */
} LoadoutFile;
static LoadoutFile sLoadouts;
static bool sLoadoutsRead;

static void loadouts_read(void) {
    if (sLoadoutsRead) {
        return;
    }
    sLoadoutsRead = true;
    memset(&sLoadouts, 0, sizeof(sLoadouts));
    FILE* f = fopen(LOADOUT_PATH, "rb");
    if (f != nullptr) {
        if (fread(&sLoadouts, sizeof(sLoadouts), 1, f) != 1 || sLoadouts.magic != LOADOUT_MAGIC || sLoadouts.version != 1) {
            memset(&sLoadouts, 0, sizeof(sLoadouts));
        }
        fclose(f);
    }
}

static void loadouts_write(void) {
    sLoadouts.magic = LOADOUT_MAGIC;
    sLoadouts.version = 1;
    FILE* f = fopen(LOADOUT_PATH, "wb");
    if (f != nullptr) {
        fwrite(&sLoadouts, sizeof(sLoadouts), 1, f);
        fclose(f);
    }
}

static s16* loadout(s32 n) {
    s32 slot = gGameStatusPtr != nullptr ? gGameStatusPtr->saveSlot & 3 : 0;
    loadouts_read();
    return sLoadouts.badge[slot][n];
}

int pm3ds_badge_loadout_count(int n) {
    if (n < 0 || n >= PM3DS_BADGE_LOADOUTS) {
        return 0;
    }
    s16* l = loadout(n);
    s32 k = 0;
    while (k < 64 && l[k] != 0) {
        k++;
    }
    return k;
}

int pm3ds_badge_loadout_save(int n) {
    if (n < 0 || n >= PM3DS_BADGE_LOADOUTS) {
        return 0;
    }
    s16* l = loadout(n);
    s32 k = 0;
    for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.equippedBadges); i++) {
        if (IS_BADGE(gPlayerData.equippedBadges[i])) {
            l[k++] = gPlayerData.equippedBadges[i];
        }
    }
    while (k < 64) {
        l[k++] = 0;
    }
    loadouts_write();
    sfx_play_sound(SOUND_MENU_BADGE_EQUIP);
    return pm3ds_badge_loadout_count(n);
}

/* everything off, then the loadout's badges on in order while they fit; ones not owned
 * anymore or past the BP are skipped */
int pm3ds_badge_loadout_load(int n, int* skipped) {
    *skipped = 0;
    if (!badge_can_change() || n < 0 || n >= PM3DS_BADGE_LOADOUTS) {
        return -1;
    }
    s16 want[64];
    memcpy(want, loadout(n), sizeof(want));
    for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.equippedBadges); i++) {
        gPlayerData.equippedBadges[i] = 0;
    }
    s32 on = 0;
    for (s32 k = 0; k < 64 && want[k] != 0; k++) {
        bool owned = false;
        for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.badges); i++) {
            if (gPlayerData.badges[i] == want[k]) {
                owned = true;
                break;
            }
        }
        if (owned && pause_badges_try_equip(want[k]) == 3) {   // EQUIP_RESULT_SUCCESS
            on++;
        } else {
            (*skipped)++;
        }
    }
    enforce_hpfp_limits();
    sfx_play_sound(SOUND_MENU_BADGE_EQUIP);
    return on;
}

/* src/filemenu/filemenu_yesno.c, a new file: the slot's old loadouts go */
void pm3ds_badge_loadouts_clear_slot(s32 slot) {
    loadouts_read();
    memset(sLoadouts.badge[slot & 3], 0, sizeof(sLoadouts.badge[0]));
    loadouts_write();
}

/* platform/bottom_screen.c, a badge tapped: on if it fits, off if it was on (as the pause menu does) */
int pm3ds_badge_toggle(int item) {
    if (!badge_can_change() || !IS_BADGE(item)) {
        return PM3DS_BADGE_NOT_NOW;
    }
    switch (pause_badges_try_equip(item)) {
        case 0:   // EQUIP_RESULT_ALREADY_EQUIPPED
            pause_badges_try_remove(item);
            sfx_play_sound(SOUND_MENU_BADGE_UNEQUIP);
            enforce_hpfp_limits();
            return PM3DS_BADGE_REMOVED;
        case 1:   // EQUIP_RESULT_NOT_ENOUGH_BP
            sfx_play_sound(SOUND_MENU_ERROR);
            return PM3DS_BADGE_NO_BP;
        case 2:   // EQUIP_RESULT_TOO_MANY_BADGES
            sfx_play_sound(SOUND_MENU_ERROR);
            return PM3DS_BADGE_TOO_MANY;
        default:
            sfx_play_sound(SOUND_MENU_BADGE_EQUIP);
            enforce_hpfp_limits();
            return PM3DS_BADGE_EQUIPPED;
    }
}

/* the detail's ingredient slot for item, added on first use */
static s16 sIngrItem[PM3DS_RECIPE_INGR];

static s32 recipe_ingr(pm3ds_recipe_detail_t* d, s32 item) {
    for (s32 i = 0; i < d->ingr_count; i++) {
        if (sIngrItem[i] == item) {
            return i;
        }
    }
    if (d->ingr_count >= PM3DS_RECIPE_INGR) {
        return -1;
    }
    s32 i = d->ingr_count++;
    sIngrItem[i] = item;
    pm3ds_decode_msg(gItemTable[item].nameMsg, d->ingr_name[i], PM3DS_RECIPE_NAME, false);
    d->ingr_icon[i] = item_hud_script(item);
    return i;
}

static void recipe_add_way(s32 a, s32 b, bool cookbook, void* ctx) {
    pm3ds_recipe_detail_t* d = ctx;
    if (d->ways >= PM3DS_RECIPE_WAYS) {
        return;
    }
    s32 ia = recipe_ingr(d, a), ib = b == ITEM_NONE ? -1 : recipe_ingr(d, b);
    if (ia < 0 || (b != ITEM_NONE && ib < 0)) {
        return;
    }
    d->way[d->ways].a = ia;
    d->way[d->ways].b = ib;
    d->way[d->ways].cookbook = cookbook;
    d->way[d->ways].luck = a == ITEM_MYSTERY;
    d->ways++;
}

void pm3ds_recipe_detail(int item, pm3ds_recipe_detail_t* out) {
    memset(out, 0, sizeof(*out));
    if (gGameStatusPtr == nullptr || item < ITEM_FIRST_CONSUMABLE || item > ITEM_LAST_CONSUMABLE) {
        return;
    }
    out->hp = gItemTable[item].potencyA;
    out->fp = gItemTable[item].potencyB;
    pm3ds_decode_msg(gItemTable[item].fullDescMsg, out->desc, sizeof(out->desc), false);
    recipe_ways(item, recipe_add_way, out);
    if (recipe_mystery(item)) {
        recipe_add_way(ITEM_MYSTERY, ITEM_NONE, false, out);
    }
}

#ifdef PM3DS_SHOT_JOURNAL
/* debug builds only (README screenshots): a chapter-2 file's worth of tattles and
 * found item spots, so the Tattle and Found tabs have something to show */
void pm3ds_journal_debug_fill(void) {
    static const s32 types[] = { ACTOR_TYPE_GOOMBA, ACTOR_TYPE_PARAGOOMBA, ACTOR_TYPE_SPIKED_GOOMBA,
        ACTOR_TYPE_RED_GOOMBA, ACTOR_TYPE_FUZZY, ACTOR_TYPE_KOOPA_TROOPA, ACTOR_TYPE_PARATROOPA,
        ACTOR_TYPE_BOB_OMB, ACTOR_TYPE_BULLET_BILL, ACTOR_TYPE_BILL_BLASTER, ACTOR_TYPE_CLEFT,
        ACTOR_TYPE_MONTY_MOLE, ACTOR_TYPE_BANDIT, ACTOR_TYPE_POKEY, ACTOR_TYPE_SWOOPER,
        ACTOR_TYPE_BUZZY_BEETLE };
    static const char* areas[] = { "kmr", "mac", "nok", "trd", "iwa", "dro", "sbk" };
    for (u32 k = 0; k < ARRAY_COUNT(types); k++) {
        s32 i = EVT_INDEX_OF_GAME_BYTE(GB_Tattles_00) + types[k] / 8;
        set_global_byte(i, get_global_byte(i) | (1 << (types[k] % 8)));
    }
    for (s32 i = 0; i < pm3ds_RecipeTableSizes[RT_COOKABLE]; i += 3) {   /* every third dish cooked */
        evt_set_variable(nullptr, CookableDiscoveredFlags[i], true);
    }
    for (u32 i = 0, n = 0; i < ARRAY_COUNT(sSpots); i++) {
        for (u32 a = 0; a < ARRAY_COUNT(areas); a++) {
            if (sSpots[i].flag != 0 && strncmp(sSpots[i].map, areas[a], 3) == 0 && (n++ % 4) != 3) {
                evt_set_variable(nullptr, sSpots[i].flag, true);
            }
        }
    }
}
#endif

/* ---- "Where to go next" -------------------------------------------------------- */
/* one hint per stretch of the story (include/enums.h, StoryProgress): shown while the
 * story byte is below .before. Written for this port; English, the bottom screen
 * translates them (pm3ds_lang_table.inc). The map each step is set on (Set(GB_StoryProgress,
 * ...) in src/world) is what they were checked against. */
typedef struct {
    s8 before;
    const char* text;
} StoryHint;

static const StoryHint sHints[] = {
    { STORY_CH0_FELL_OFF_CLIFF, "Look around Goomba Village and talk to Goompa." },
    { STORY_CH0_FOUND_HAMMER, "Goompa fell off the cliff. Find him below, then search the forest's bushes for a Hammer." },
    { STORY_CH0_GOOMBARIO_JOINED_PARTY, "Beat Jr. Troopa in the forest playground, then go back up to Goomba Village." },
    { STORY_CH0_DEFEATED_GOOMBA_KING, "Smash the block on the village gate with your Hammer and follow Goomba Road east to the Goomba King." },
    { STORY_CH0_OPENED_BRIDGE_TO_TOAD_TOWN, "Find the switch near the Goomba King's fort to lower the bridge to Toad Town." },
    { STORY_CH0_RETURNED_TO_TOAD_TOWN, "Go through Toad Town and up to Shooting Star Summit, where the Star Spirits are waiting." },
    { STORY_CH1_KNOCKED_SWITCH_FROM_TREE, "Talk to Merlon in Toad Town, then take Pleasant Path east. Where it ends at a gap, hit the tree with your Hammer." },
    { STORY_CH1_ARRIVED_AT_KOOPA_VILLAGE, "Hit the switch that fell from the tree to make a bridge, and go on to Koopa Village." },
    { STORY_CH1_KOOPER_JOINED_PARTY, "Help the Koopas with the Fuzzies, then chase the one that took Kooper's shell into the woods." },
    { STORY_CH1_ARRIVED_AT_KOOPA_FORTRESS, "With Kooper, make the next bridge on Pleasant Path past Koopa Village and go on to Koopa Bros. Fortress." },
    { STORY_CH1_BOMBETTE_JOINED_PARTY, "Look for keys in the fortress. When you fall into the dungeon, talk to the Bob-omb in the cell." },
    { STORY_CH1_STAR_SPIRIT_RESCUED, "Bombette can blow up cracked walls. Climb the fortress and beat the Koopa Bros." },
    { STORY_CH1_RETURNED_TO_TOAD_TOWN, "Head back to Toad Town along Pleasant Path." },
    { STORY_CH2_ARRIVED_AT_MT_RUGGED, "Go to the train station in the south of Toad Town. Clear what blocks the tracks and ride to Mt. Rugged." },
    { STORY_CH2_PARAKARRY_JOINED_PARTY, "A Paratroopa mail carrier lost three letters on Mt. Rugged. Find them and bring them back to him." },
    { STORY_CH2_ARRIVED_AT_DRY_DRY_OUTPOST, "Parakarry carries you over gaps. Cross Mt. Rugged, then head east across Dry Dry Desert to Dry Dry Outpost." },
    { STORY_CH2_BOUGHT_SECRET_ITEMS, "In Dry Dry Outpost, talk to the Nomadimouse hiding his face in the back of town. Fruit from the oasis south of town makes him talk." },
    { STORY_CH2_GOT_PULSE_STONE, "Check the door where he waited and climb onto the rooftops. The town's leader is up there." },
    { STORY_CH2_UNCOVERED_DRY_DRY_RUINS, "Walk the desert with the Pulse Stone. Where it flashes fastest, put it on the small rock." },
    { STORY_CH2_SOLVED_ARTIFACT_PUZZLE, "In Dry Dry Ruins, drain the sand, find the Super Hammer, and put the Pyramid, Diamond and Lunar Stones in place." },
    { STORY_CH2_STAR_SPIRIT_RESCUED, "Take the new stairs to the bottom of the ruins and beat Tutankoopa." },
    { STORY_CH3_SAW_BOO_ENTER_FOREST, "Return to Toad Town and go to the gate of Forever Forest, near Tayce T.'s house." },
    { STORY_CH3_ARRIVED_AT_BOOS_MANSION, "Go into Forever Forest. To find your way, take the path that looks different from the others." },
    { STORY_CH3_GOT_RECORD, "Go into Boo's Mansion and look around for a Record." },
    { STORY_CH3_WEIGHED_DOWN_CHANDELIER, "Play the Record on the phonograph to distract a Boo and take the Weight. Hang it from the chandelier by the sofa." },
    { STORY_CH3_GOT_SUPER_BOOTS, "Go through the door the glass case hid and down to the storeroom. Win the Boos' game for new Boots." },
    { STORY_CH3_BOW_JOINED_PARTY, "Spin Jump through boarded floors to find Boo's Portrait and hang it back up. A Boo takes you upstairs." },
    { STORY_CH3_ARRIVED_AT_TUBBAS_MANOR, "With Bow, head through Gusty Gulch to Tubba Blubba's Castle." },
    { STORY_CH3_TUBBA_WOKE_UP, "Find keys to get through the castle. Tubba Blubba sleeps upstairs: sneak to the chest by his bed." },
    { STORY_CH3_ESCAPED_TUBBAS_MANOR, "Run! Get out of the castle before Tubba Blubba catches you." },
    { STORY_CH3_HEART_ESCAPED_WINDY_MILL, "The talking key opens Windy Mill in Gusty Gulch. Go down its well and chase Tubba Blubba's heart." },
    { STORY_CH3_STAR_SPIRIT_RESCUED, "Follow the heart back to Tubba Blubba and beat him." },
    { STORY_CH4_FRYING_PAN_STOLEN, "Head back to Toad Town." },
    { STORY_CH4_ENTERED_THE_TOY_BOX, "Shy Guys are causing trouble in Toad Town. Watch the second house west of the shop in the south. Bow can hide you." },
    { STORY_CH4_RETURNED_STOREROOM_KEY, "In Shy Guy's Toy Box, find the Storeroom Key and give it back to the shopkeeper in the south of Toad Town." },
    { STORY_CH4_RETURNED_TOY_TRAIN, "Take the Toy Train from his storeroom and put it into the Toy Box from outside." },
    { STORY_CH4_GOT_TAYCE_TS_CAKE, "Ride the train through the Toy Box, find Tayce T.'s Frying Pan and bring it back to her." },
    { STORY_CH4_GAVE_CAKE_TO_GOURMET_GUY, "Give the Cake to the hungry Gourmet Guy in the Toy Box." },
    { STORY_CH4_SOLVED_COLOR_PUZZLE, "On the moving walkways, hide with Bow to slip past. Then hit the colored blocks in the right order." },
    { STORY_CH4_WATT_JOINED_PARTY, "In the dark rooms, beat the Big Lantern Ghost and free the light it carries." },
    { STORY_CH4_STAR_SPIRIT_RESCUED, "Watt lights up dark rooms. Follow the Shy Guys running from the light to General Guy and beat him." },
    { STORY_CH5_WHALE_MOUTH_OPEN, "Go to the port in Toad Town and hit the strange island by the docks." },
    { STORY_CH5_REACHED_LAVA_LAVA_ISLAND, "The whale has a bellyache: go into its mouth and help. Then it takes you to Lavalava Island." },
    { STORY_CH5_SUSHIE_JOINED_PARTY, "Go through Jade Jungle to Yoshi's Village. When the Yoshi kids go missing, find their babysitter first." },
    { STORY_CH5_ALL_YOSHI_CHILDREN_RESCUED, "Find all five Yoshi kids in Jade Jungle. Sushie lets you swim." },
    { STORY_CH5_REACHED_RAPHAELS_TREE, "Put the Jade Raven from the village chief on the big stone statue, and follow the new path to a giant tree." },
    { STORY_CH5_ENTERED_MT_LAVA_LAVA, "Climb the giant tree and talk to Raphael the Raven. He'll get you to Mt. Lavalava." },
    { STORY_CH5_GOT_ULTRA_HAMMER, "Inside Mt. Lavalava, slow the lava with the blue blocks and find the Ultra Hammer." },
    { STORY_CH5_DEFEATED_LAVA_PIRANHA, "Smash the stone blocks with the Ultra Hammer and go deep into the volcano." },
    { STORY_CH5_BEGAN_PEACH_MISSION, "The volcano is erupting! Run back out." },
    { STORY_CH5_TRADED_VASE_FOR_SEED, "The eruption threw a chest into the jungle near where you met Sushie. Bring what's inside to Kolorado." },
    { STORY_CH5_RETURNED_TO_TOAD_TOWN, "Ride the whale back to Toad Town." },
    { STORY_CH6_FLOWER_GATE_OPEN, "Bring four Magical Seeds (Toad Town, Forever Forest, Jade Jungle, Mt. Rugged) to Minh T. in Toad Town." },
    { STORY_CH6_GOT_MAGICAL_BEAN, "In Flower Fields, help the flower east of the big tree: Monty Moles are bothering her." },
    { STORY_CH6_GOT_FERTILE_SOIL, "Smash the tree near Petunia for a Red Berry and give it to the hungry red flower. The flower past his gate has Fertile Soil." },
    { STORY_CH6_GOT_WATER_STONE, "A Yellow Berry opens the yellow flower's gate. Get a Crystal Berry from Posie and trade it to Rosie for the Water Stone." },
    { STORY_CH6_FILLED_SPRING_WITH_WATER, "Put the Water Stone in the dry spring past the yellow flower's gate." },
    { STORY_CH6_LAKILESTER_JOINED_PARTY, "Find the Sun in Flower Fields and hear him out. On the way back, a Lakitu gets in your way." },
    { STORY_CH6_DESTROYED_PUFF_PUFF_MACHINE, "Ride Lakilester, stomp the colored blocks in the right order, and smash the Puff Puff Machine." },
    { STORY_CH6_GREW_MAGIC_BEANSTALK, "Ask the Wise Wisterwood for advice. Then plant in order: Fertile Soil, the Magical Bean, Miracle Water." },
    { STORY_CH6_STAR_SPIRIT_RESCUED, "Climb the beanstalk above the clouds and beat Huff N. Puff." },
    { STORY_CH7_INVITED_TO_STARBORN_VALLEY, "Return to Toad Town and talk to Merlon." },
    { STORY_CH7_ARRIVED_AT_SHIVER_CITY, "Get the Ultra Boots deep in Toad Town Tunnels. Then find a blue door on a high ledge: Watt shows the hidden blocks." },
    { STORY_CH7_MAYOR_MURDER_SOLVED, "In Shiver City, visit the Mayor Penguin. If you get blamed for trouble, find Herringway past the frozen pond." },
    { STORY_CH7_ARRIVED_AT_STARBORN_VALLEY, "Take the snowy path west of Shiver City across Shiver Snowfield to Starborn Valley." },
    { STORY_CH7_GOT_SNOWMAN_SCARF, "In Starborn Valley, talk to Merle." },
    { STORY_CH7_GOT_SNOWMAN_BUCKET, "Go back to Shiver City: the Mayor Penguin has a treasure for you too." },
    { STORY_CH7_UNLOCKED_SHIVER_MOUNTAIN, "Put the Scarf and the Bucket on the snowmen by the cliff so they all look the same." },
    { STORY_CH7_GOT_STAR_STONE, "Climb Shiver Mountain and break the cracked wall. In the icy cave, don't trust your eyes: keep walking." },
    { STORY_CH7_ARRIVED_AT_CRYSTAL_PALACE, "Use the Star Stone at the top of Shiver Mountain to reach the Crystal Palace." },
    { STORY_CH7_EXTENDED_PALACE_BRIDGE, "In the Crystal Palace, what you do on one side of the glass happens on the other. Watch out for copycats." },
    { STORY_CH7_SOLVED_ALBINO_DINO_PUZZLE, "Find the room under a statue. Talk to the Albino Dinos to turn them and push the statues into place." },
    { STORY_CH7_STAR_SPIRIT_RESCUED, "Beat the Crystal King. Two of the three kings are fakes: hit them all." },
    { STORY_CH8_OPENED_PATH_TO_STAR_WAY, "Go to Shooting Star Summit. The Star Spirits are waiting there." },
    { STORY_CH8_REACHED_BOWSERS_CASTLE, "Climb the Star Way to Star Haven and ride the Star Ship to Bowser's Castle." },
    { STORY_CH8_REACHED_PEACHS_CASTLE, "Make your way up through Bowser's Castle to Princess Peach's Castle." },
    { STORY_EPILOGUE, "Climb to the top of Peach's Castle and beat Bowser!" },
    { 127, "You saved the Mushroom Kingdom! See what's left on the Found and Achievements pages." },
};

const char* pm3ds_story_hint(void) {
    if (gGameStatusPtr == nullptr) {
        return nullptr;
    }
    if (gGameStatusPtr->peachFlags & PEACH_FLAG_IS_PEACH) {
        return "You're Princess Peach for now. Look around the castle: Twink can help.";
    }
    s32 story = evt_get_variable(nullptr, GB_StoryProgress);
    for (u32 i = 0; i < ARRAY_COUNT(sHints); i++) {
        if (story < sHints[i].before) {
            return sHints[i].text;
        }
    }
    return sHints[ARRAY_COUNT(sHints) - 1].text;
}
