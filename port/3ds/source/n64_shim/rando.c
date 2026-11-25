/*
 * rando.c — Star Haven's item randomizer (normal story order).
 *
 * Locations (tools/rando_locations.py -> rando_locations*.inc): items lying
 * around, item blocks, tree/bush drops and hidden panels (each named by the
 * save flag that marks it taken), chests (their flag too), and NPC gifts
 * (named by map and vanilla item: they have no flag of their own).
 *
 * Settings (pm3ds_rando.h flags, picked per new game):
 *   on              consumable items (not the Flower Fields berries) and badges
 *   coins           + coins and star pieces
 *   keys            + key items, placed by the logic below
 *   shops           shops trade their stock with the world (below)
 *   gifts           NPC gifts join the pool
 *   hidden          key items may also go to hidden panels and hidden blocks
 *   doors           two-way exits lead to other rooms of the same area
 *                   (rando_doors.c), each room still opening when it did
 * Gear, partners and star spirits always keep their places, and so do the
 * plain items the story asks for: the Flower Fields berries, Tayce T.'s
 * Cake (chapter 4) and Dry Dry Outpost's password goods (chapter 2).
 *
 * Shops (rando_shops*.inc): each coin shop slot trades its item with a
 * random world spot of the same kind -- consumables in the restocking shops,
 * badges in Rowf's one-time stock -- so shops sell what the world held and
 * the world holds the shops' stock. Prices: the item's highest vanilla shop
 * price (Rowf: the badge's own price), else twice its sell value.
 *
 * Key items: the story still runs in its normal order, so a key item found in
 * an area of chapter N (the chapter that needs it, at the latest) may only go
 * to a location a normal-story player can reach before chapter N: the table's
 * kav (computed from the PMR world graph by tools/rando_logic.py) must be <= N.
 * Never to a gift (those depend on the NPC's own story). Key items placed
 * first, most constrained first; one with no room left stays where it was.
 *
 * The new-game picker (platform/bottom_screen.c) sets the flags and maybe a
 * seed; a seed is rolled otherwise. Both are kept in the save file's unused
 * tail (SaveData.unk_1304, inside the CRC), so copied saves keep them. The placement is rebuilt from the seed
 * on load; a spoiler log goes to <SD folder>/spoiler_<seed>.txt.
 */
#include "common.h"
#include "battle/battle.h"
#include "message_ids.h"

#include <stdio.h>
#include <string.h>

#include "pm3ds_brand.h"
#include "pm3ds_log.h"
#include "pm3ds_keyitems.h"
#include "pm3ds_rando.h"

enum { SRC_ITEM, SRC_BLOCK, SRC_TREE, SRC_BUSH, SRC_PANEL, SRC_CHEST, SRC_GIFT, SRC_HIDDEN_BLOCK };
static const char *const s_src_names[] = { "item", "block", "tree", "bush", "hidden panel", "chest", "gift",
                                           "hidden block" };

typedef struct {
    s32 flag;          /* GF_* as the scripts write it; 0 for a gift */
    s16 item;          /* vanilla */
    u8 src;
    u8 kav;            /* first chapter whose key items may go here (99: none) */
    u8 chapter;        /* chapter of the area (99: none) */
    const char *itemName;
    const char *map;
} RandoLoc;

#define LOC(flag, item, src, map, kav, chapter) { flag, item, src, kav, chapter, #item, map },
static const RandoLoc sLocs[] = {
#if VERSION_US
#include "rando_locations_us.inc"
#elif VERSION_JP
#include "rando_locations_jp.inc"
#elif VERSION_IQUE
#include "rando_locations_ique.inc"
#else
#include "rando_locations.inc"
#endif
};
#undef LOC
#define NUM_LOCS ARRAY_COUNT(sLocs)

typedef struct {
    s16 item;          /* vanilla */
    s16 price;
    s32 descMsg;
    const char *map;
    u8 rowf;           /* Rowf's one-time badges */
    const char *itemName;
} RandoShop;

#define SHOP(item, price, desc, map, rowf) { item, price, desc, map, rowf, #item },
static const RandoShop sShops[] = {
#if VERSION_US
#include "rando_shops_us.inc"
#elif VERSION_JP
#include "rando_shops_jp.inc"
#elif VERSION_IQUE
#include "rando_shops_ique.inc"
#else
#include "rando_shops.inc"
#endif
};
#undef SHOP
#define NUM_SHOPS ARRAY_COUNT(sShops)

#define RANDO_MAGIC 0x53485244   /* "SHRD" */

typedef struct {
    s32 magic;
    s32 version;   /* 1: a mode (below); 2, 3: PM3DS_RANDO_* flags (2: before HIDDEN) */
    s32 flags;
    u32 seed;
} RandoSaveBlock;

/* version 1 saves: Items, Everything, Everything + keys (shops, gifts, hidden spots always) */
static const u32 sV1Flags[4] = {
    0,
    PM3DS_RANDO_ON | PM3DS_RANDO_SHOPS | PM3DS_RANDO_GIFTS,
    PM3DS_RANDO_ON | PM3DS_RANDO_COINS | PM3DS_RANDO_SHOPS | PM3DS_RANDO_GIFTS,
    PM3DS_RANDO_ALL_V3,
};

static u32 sFlags;             /* 0: off */
static u32 sSeed;
static u32 sNextFlags, sNextSeed;   /* the picker's choice for the next new game */
static s16 sItem[NUM_LOCS];    /* what location i holds now */
static const char* current_map(void);
static s16 sShopItem[NUM_SHOPS];

/* ---- seeded shuffle ------------------------------------------------------- */
static u32 next_rand(u32* state) {   /* splitmix32 */
    u32 z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

static bool is_key(s32 item) {
    return item > ITEM_NONE && item < ARRAY_COUNT(gItemTable) && (gItemTable[item].typeFlags & ITEM_TYPE_FLAG_KEY) &&
           !(gItemTable[item].typeFlags & ITEM_TYPE_FLAG_GEAR);
}

/* does location i's vanilla item move with these flags? */
static bool shuffleable(s32 i, u32 flags) {
    s32 item = sLocs[i].item;
    if (!(flags & PM3DS_RANDO_ON)) {
        return false;
    }
    if (sLocs[i].src == SRC_GIFT && !(flags & PM3DS_RANDO_GIFTS)) {
        return false;
    }
    if (item == ITEM_COIN || item == ITEM_STAR_PIECE) {
        return (flags & PM3DS_RANDO_COINS) != 0;
    }
    if (item <= ITEM_NONE || item >= ARRAY_COUNT(gItemTable)) {
        return false;
    }
    switch (item) {
        case ITEM_RED_BERRY:
        case ITEM_YELLOW_BERRY:
        case ITEM_BLUE_BERRY:
        case ITEM_BUBBLE_BERRY:
            return false;   // Flower Fields needs them where they grow
        case ITEM_CAKE:
            return false;   // Tayce T.'s cake: chapter 4's Gourmet Guy wants it
    }
    if (gItemTable[item].typeFlags & ITEM_TYPE_FLAG_GEAR) {
        return false;
    }
    if (gItemTable[item].typeFlags & ITEM_TYPE_FLAG_KEY) {
        // a prologue key item has nowhere earlier to go
        return (flags & PM3DS_RANDO_KEYS) && sLocs[i].chapter >= 1 && sLocs[i].chapter <= 8;
    }
    return (gItemTable[item].typeFlags & (ITEM_TYPE_FLAG_BADGE | ITEM_TYPE_FLAG_CONSUMABLE)) != 0;
}

/* a slot the story buys from: Dry Dry Outpost's password (Dried Shroom, then
 * Dusty Hammer) leads to Moustafa in chapter 2; Dried Pasta is in the Red
 * Jar's sequence */
static bool shop_story_slot(s32 i) {
    s32 item = sShops[i].item;
    return strcmp(sShops[i].map, "dro_01") == 0 &&
           (item == ITEM_DRIED_SHROOM || item == ITEM_DUSTY_HAMMER || item == ITEM_DRIED_PASTA);
}

/* may this item sit in that kind of shop slot (and come out of it)? */
static bool shop_kind_ok(s32 item, bool rowf) {
    if (item <= ITEM_NONE || item >= ARRAY_COUNT(gItemTable) || item == ITEM_COIN || item == ITEM_STAR_PIECE ||
        item == ITEM_CAKE_MIX) {   // chapter 4's cake: keep it buyable
        return false;
    }
    if (gItemTable[item].typeFlags & (ITEM_TYPE_FLAG_KEY | ITEM_TYPE_FLAG_GEAR)) {
        return false;
    }
    if (rowf) {
        return (gItemTable[item].typeFlags & ITEM_TYPE_FLAG_BADGE) != 0;
    }
    return (gItemTable[item].typeFlags & ITEM_TYPE_FLAG_CONSUMABLE) && !(gItemTable[item].typeFlags & ITEM_TYPE_FLAG_BADGE);
}

static void build(void) {
    static s16 pool[NUM_LOCS], keys[NUM_LOCS], rest[NUM_LOCS];
    static bool taken[NUM_LOCS];
    s32 n = 0, nk = 0, nr = 0, kept = 0;
    s32 i, j;
    u32 state = sSeed;

    for (i = 0; i < NUM_LOCS; i++) {
        sItem[i] = sLocs[i].item;
        taken[i] = false;
        if (shuffleable(i, sFlags)) {
            pool[n++] = i;
            if (is_key(sLocs[i].item)) {
                keys[nk++] = i;
            } else {
                rest[nr++] = i;
            }
        }
    }
    for (i = 0; i < NUM_SHOPS; i++) {
        sShopItem[i] = sShops[i].item;
    }
    if (!(sFlags & PM3DS_RANDO_ON)) {
        return;
    }
    // key items: random order, then the earliest-needed first (stable)
    for (i = nk - 1; i > 0; i--) {
        s32 t = next_rand(&state) % (u32)(i + 1);
        s16 x = keys[i];
        keys[i] = keys[t];
        keys[t] = x;
    }
    for (i = 1; i < nk; i++) {
        s16 x = keys[i];
        for (j = i; j > 0 && sLocs[keys[j - 1]].chapter > sLocs[x].chapter; j--) {
            keys[j] = keys[j - 1];
        }
        keys[j] = x;
    }
    // (only to spots that held no key item, so a key's own spot stays free for it)
#define KEY_SPOT(l, need)                                                                                      \
    (!taken[l] && !is_key(sLocs[l].item) && sLocs[l].src != SRC_GIFT && sLocs[l].kav <= (need) &&            \
     ((sFlags & PM3DS_RANDO_HIDDEN) || (sLocs[l].src != SRC_PANEL && sLocs[l].src != SRC_HIDDEN_BLOCK)))
    for (i = 0; i < nk; i++) {
        s32 need = sLocs[keys[i]].chapter;
        s32 cands = 0, pick;
        for (j = 0; j < n; j++) {
            cands += KEY_SPOT(pool[j], need);
        }
        if (cands == 0) {
            pick = keys[i];   // no room before it is needed: where it was
            kept++;
        } else {
            s32 k = next_rand(&state) % (u32)cands;
            for (j = 0; j < n; j++) {
                if (KEY_SPOT(pool[j], need) && k-- == 0) {
                    break;
                }
            }
            pick = pool[j];
        }
#undef KEY_SPOT
        taken[pick] = true;
        sItem[pick] = sLocs[keys[i]].item;
    }
    // everything else: Fisher-Yates into the spots left
    for (i = nr - 1; i > 0; i--) {
        s32 t = next_rand(&state) % (u32)(i + 1);
        s16 x = rest[i];
        rest[i] = rest[t];
        rest[t] = x;
    }
    j = 0;
    for (i = 0; i < n; i++) {
        if (!taken[pool[i]] && j < nr) {
            sItem[pool[i]] = sLocs[rest[j++]].item;
        }
    }
    // shops: each slot trades with a world spot holding the same kind of item
    for (i = 0; i < NUM_SHOPS && (sFlags & PM3DS_RANDO_SHOPS); i++) {
        s32 cands = 0, k;
        if (!shop_kind_ok(sShopItem[i], sShops[i].rowf) || shop_story_slot(i)) {
            continue;
        }
        for (j = 0; j < n; j++) {
            cands += sLocs[pool[j]].src != SRC_GIFT && shop_kind_ok(sItem[pool[j]], sShops[i].rowf);
        }
        if (cands == 0) {
            continue;
        }
        k = next_rand(&state) % (u32)cands;
        for (j = 0; j < n; j++) {
            s32 l = pool[j];
            if (sLocs[l].src != SRC_GIFT && shop_kind_ok(sItem[l], sShops[i].rowf) && k-- == 0) {
                s16 x = sItem[l];
                sItem[l] = sShopItem[i];
                sShopItem[i] = x;
                break;
            }
        }
    }
    pm3ds_log(PM3DS_LOG_INFO, "rando: seed %08lX flags %02lX, %d of %d locations shuffled, %d key items (%d kept)",
              (unsigned long)sSeed, (unsigned long)sFlags, (int)n, (int)NUM_LOCS, (int)nk, (int)kept);
}

/* ITEM_ name without the prefix, from the location tables */
static const char* item_name(s32 item) {
    static char buf[12];
    s32 i;
    for (i = 0; i < NUM_LOCS; i++) {
        if (sLocs[i].item == item) {
            return sLocs[i].itemName + 5;
        }
    }
    for (i = 0; i < NUM_SHOPS; i++) {
        if (sShops[i].item == item) {
            return sShops[i].itemName + 5;
        }
    }
    snprintf(buf, sizeof(buf), "item %d", (int)item);
    return buf;
}

static u8 sMoveBP[], sMoveFP[];
const char* pm3ds_map_place(const char* map, char* buf, int size);   /* journal.c */
/* rando_doors.c */
int pm3ds_doors_build(unsigned seed, int on);
int pm3ds_doors_redirect(const char* fromMap, const char** toMap, int* toEntry);
void pm3ds_doors_spoiler(FILE* f, const char* (*place)(const char* map, char* buf, int size));

static void build_doors(void) {
    pm3ds_doors_build(sSeed, (sFlags & PM3DS_RANDO_DOORS) != 0);
}

static void write_spoiler(void) {
    char path[96], place[48];
    FILE* f;
    s32 i;

    snprintf(path, sizeof(path), PM3DS_SD_DIR "/spoiler_%08lX.txt", (unsigned long)sSeed);
    f = fopen(path, "w");
    if (f == nullptr) {
        return;
    }
    fprintf(f, "%s randomizer, seed %08lX\nPlaces are the area and the game's name for the room (iwa_03 is a room of Mt. Rugged).\nShuffled: items, badges%s%s%s%s%s%s%s%s\n\n", PM3DS_APP_NAME,
            (unsigned long)sSeed, (sFlags & PM3DS_RANDO_COINS) ? ", coins, star pieces" : "",
            (sFlags & PM3DS_RANDO_KEYS) ? ", key items" : "",
            (sFlags & PM3DS_RANDO_KEYS) && (sFlags & PM3DS_RANDO_HIDDEN) ? " (hidden spots too)" : "",
            (sFlags & PM3DS_RANDO_SHOPS) ? ", shops" : "", (sFlags & PM3DS_RANDO_GIFTS) ? ", NPC gifts" : "",
            (sFlags & PM3DS_RANDO_ENEMIES) ? ", enemies" : "", (sFlags & PM3DS_RANDO_BADGES) ? ", badge costs" : "",
            (sFlags & PM3DS_RANDO_DOORS) ? ", doors (in each area)" : "");
    if (sFlags & PM3DS_RANDO_KEYS) {
        fprintf(f, "Key items:\n");
        for (i = 0; i < NUM_LOCS; i++) {
            if (sItem[i] != sLocs[i].item && is_key(sItem[i])) {
                fprintf(f, "  %-26s %-32s %s\n", item_name(sItem[i]), pm3ds_map_place(sLocs[i].map, place, sizeof(place)),
                        s_src_names[sLocs[i].src]);
            }
        }
        fprintf(f, "\nEverything:\n");
    }
    for (i = 0; i < NUM_LOCS; i++) {
        if (sItem[i] != sLocs[i].item) {
            fprintf(f, "%-32s %-13s %-26s -> %s\n", pm3ds_map_place(sLocs[i].map, place, sizeof(place)), s_src_names[sLocs[i].src], sLocs[i].itemName + 5,
                    item_name(sItem[i]));
        }
    }
    fprintf(f, "\nShops:\n");
    for (i = 0; i < NUM_SHOPS; i++) {
        if (sShopItem[i] != sShops[i].item) {
            fprintf(f, "%-32s %-13s %-26s -> %s\n", pm3ds_map_place(sShops[i].map, place, sizeof(place)), sShops[i].rowf ? "Rowf" : "shop",
                    item_name(sShops[i].item), item_name(sShopItem[i]));
        }
    }
    if (sFlags & PM3DS_RANDO_BADGES) {
        fprintf(f, "\nBadge costs (BP, FP):\n");
        for (i = ITEM_FIRST_BADGE; i <= ITEM_LAST_BADGE; i++) {
            u8 mv = gItemTable[i].moveID;
            if (mv != 0 && (gMoveTable[mv].costBP != sMoveBP[mv] || gMoveTable[mv].costFP != sMoveFP[mv])) {
                fprintf(f, "  %-26s %d BP -> %d BP", item_name(i), sMoveBP[mv], gMoveTable[mv].costBP);
                if (sMoveFP[mv] != 0) {
                    fprintf(f, ", %d FP -> %d FP", sMoveFP[mv], gMoveTable[mv].costFP);
                }
                fprintf(f, "\n");
            }
        }
    }
    pm3ds_doors_spoiler(f, pm3ds_map_place);
    fclose(f);
}

static void log_swap(s32 i) {
    pm3ds_log(PM3DS_LOG_INFO, "rando: %s %s %s -> %s", sLocs[i].map, s_src_names[sLocs[i].src], sLocs[i].itemName + 5,
              item_name(sItem[i]));
}

static s32 item_type(s32 item) {
    if (item == ITEM_STAR_PIECE) {
        return ITEM_TYPE_STAR_PIECE;
    }
    if (item > ITEM_NONE && item < ARRAY_COUNT(gItemTable)) {
        if (gItemTable[item].typeFlags & ITEM_TYPE_FLAG_KEY) {
            return ITEM_TYPE_KEY;
        }
        if (gItemTable[item].typeFlags & ITEM_TYPE_FLAG_BADGE) {
            return ITEM_TYPE_BADGE;
        }
    }
    return ITEM_TYPE_CONSUMABLE;
}

/* Spots whose map clears their flag on every visit, so they drop again: the
 * goomnut and Koopa Leaf bushes, the oasis fruit trees, Shiver City's potato.
 * Their randomized item comes once (a key item there was handed out on every
 * visit and filled the key item list); after that, the vanilla one. Which ones
 * were taken is kept in its own block of the save's tail (TAKEN_BLOCK_OFS). */
static const s32 sRespawnFlags[] = {
    GF_KMR02_Tree1_Goomnut, GF_NOK01_Bush4_KoopaLeaf, GF_NOK02_Bush1_KoopaLeaf,
    GF_SAM02_Item_IcedPotato, GF_SBK56_Tree1_Lemon, GF_SBK56_Tree2_Lime,
};
static u32 sTaken;                 /* bit per sRespawnFlags entry */
#define TAKEN_MAGIC 0x53485254     /* "SHRT" */
#define TAKEN_BLOCK_OFS 0x30       /* after quicksave.c's block */

static s32 respawn_index(s32 flagIndex) {
    s32 k;
    for (k = 0; k < ARRAY_COUNT(sRespawnFlags); k++) {
        if (EVT_INDEX_OF_GAME_FLAG(sRespawnFlags[k]) == flagIndex) {
            return k;
        }
    }
    return -1;
}

/* ---- game hooks ------------------------------------------------------------- */
/* src/item_entity.c, make_item_entity: the item this flagged spot holds now */
s32 pm3ds_rando_item(s32 itemID, s32 flagIndex, s32* spawnMode) {
    s32 i;
    s32 newItem;
    s32 r;

    if (sFlags == 0 || flagIndex <= 0 || flagIndex == 0xFFFF) {
        return itemID;
    }
    for (i = 0; i < NUM_LOCS; i++) {
        if (sLocs[i].flag != 0 && EVT_INDEX_OF_GAME_FLAG(sLocs[i].flag) == flagIndex && sLocs[i].item == itemID) {
            break;
        }
    }
    if (i == NUM_LOCS || sItem[i] == sLocs[i].item) {
        return itemID;
    }
    r = respawn_index(flagIndex);
    if (r >= 0 && (sTaken & (1u << r))) {
        return itemID;   // its randomized item was picked up already: back to what grows here
    }
    newItem = sItem[i];
    log_swap(i);
    // an item block pops coins and items differently
    if (*spawnMode == ITEM_SPAWN_MODE_ITEM_BLOCK_COIN || *spawnMode == ITEM_SPAWN_MODE_ITEM_BLOCK_ITEM ||
        *spawnMode == ITEM_SPAWN_MODE_ITEM_BLOCK_BADGE) {
        if (newItem == ITEM_COIN) {
            *spawnMode = ITEM_SPAWN_MODE_ITEM_BLOCK_COIN;
        } else if (gItemTable[newItem].typeFlags & ITEM_TYPE_FLAG_BADGE) {
            *spawnMode = ITEM_SPAWN_MODE_ITEM_BLOCK_BADGE;
        } else {
            *spawnMode = ITEM_SPAWN_MODE_ITEM_BLOCK_ITEM;
        }
    }
    // bush and tree drops vanish after a while, and most mark the spot taken as they appear:
    // a key item left lying there was gone for good (#31). Keep it until it is picked up and
    // mark the spot then, as the vanilla tree letters and star pieces do (make_item_entity
    // won't drop a second one while it lies there).
    switch (*spawnMode) {
        case ITEM_SPAWN_MODE_TOSS:
        case ITEM_SPAWN_MODE_TOSS_SPAWN_ONCE:
        case ITEM_SPAWN_MODE_TOSS_SPAWN_ONCE_NEVER_VANISH:
            *spawnMode = ITEM_SPAWN_MODE_TOSS_NEVER_VANISH;
            break;
        case ITEM_SPAWN_MODE_FALL:
        case ITEM_SPAWN_MODE_FALL_SPAWN_ONCE:
            *spawnMode = ITEM_SPAWN_MODE_FALL_NEVER_VANISH;
            break;
        case ITEM_SPAWN_MODE_FIXED:
            *spawnMode = ITEM_SPAWN_MODE_FIXED_NEVER_VANISH;
            break;
    }
    return newItem;
}

/* n64_shim/journal.c, the Found tab's spoiler: the item a flagged spot holds in this seed */
s32 pm3ds_rando_spot_item(s32 flag, s32 vanillaItem) {
    s32 i;

    if (sFlags == 0) {
        return vanillaItem;
    }
    for (i = 0; i < NUM_LOCS; i++) {
        if (sLocs[i].flag == flag && sLocs[i].item == vanillaItem) {
            return sItem[i];
        }
    }
    return vanillaItem;
}

/* src/item_entity.c, an item picked up: a respawning spot's randomized item is taken for good */
void pm3ds_rando_picked(s32 flagIndex, s32 itemID) {
    s32 i, r;

    if (sFlags == 0 || flagIndex <= 0 || (r = respawn_index(flagIndex)) < 0) {
        return;
    }
    for (i = 0; i < NUM_LOCS; i++) {
        if (sLocs[i].flag == sRespawnFlags[r] && sItem[i] != sLocs[i].item && sItem[i] == itemID) {
            sTaken |= 1u << r;
            pm3ds_log(PM3DS_LOG_INFO, "rando: %s %s taken", sLocs[i].map, item_name(itemID));
            return;
        }
    }
}

/* world/common/entity/Chest.inc.c: Call(pm3ds_RandoChest, GF_x) with LVarA
 * the vanilla item: LVarA/LVarB become the item now in this chest and its type */
API_CALLABLE(pm3ds_RandoChest) {
    s32 flag = *script->ptrReadPos;
    s32 item = script->varTable[10];
    s32 i;

    if (sFlags == 0) {
        return ApiStatus_DONE2;
    }
    for (i = 0; i < NUM_LOCS; i++) {
        if (sLocs[i].src == SRC_CHEST && sLocs[i].flag == flag && sLocs[i].item == item) {
            break;
        }
    }
    if (i < NUM_LOCS && sItem[i] != sLocs[i].item) {
        log_swap(i);
        script->varTable[10] = sItem[i];
        script->varTable[11] = item_type(sItem[i]);
    }
    return ApiStatus_DONE2;
}

/* world/common/complete/GiveReward.inc.c: LVar0/LVar1 (item, type) of an NPC's
 * gift become the item it gives now */
API_CALLABLE(pm3ds_RandoGift) {
    s32 item = script->varTable[0];
    const char* map;
    s32 i;

    if (sFlags == 0 || gGameStatusPtr->areaID < 0) {
        return ApiStatus_DONE2;
    }
    map = current_map();
    for (i = 0; i < NUM_LOCS; i++) {
        if (sLocs[i].src == SRC_GIFT && sLocs[i].item == item && strcmp(sLocs[i].map, map) == 0) {
            break;
        }
    }
    if (i < NUM_LOCS && sItem[i] != sLocs[i].item) {
        log_swap(i);
        script->varTable[0] = sItem[i];
        script->varTable[1] = item_type(sItem[i]);
    }
    return ApiStatus_DONE2;
}

/* Call(pm3ds_RandoAddGift, item, type, outIndex): AddItem / AddKeyItem /
 * AddBadge / AddStarPieces / AddCoin, whichever the item is; outIndex 0 for
 * none */
API_CALLABLE(pm3ds_RandoAddGift) {
    Bytecode* args = script->ptrReadPos;
    s32 item = evt_get_variable(script, *args++);
    s32 type = evt_get_variable(script, *args++);
    Bytecode out = *args++;
    PlayerData* playerData = &gPlayerData;
    s32 idx = 0;
    s32 i;

    if (item == ITEM_COIN) {
        add_coins(1);
    } else if (type == ITEM_TYPE_STAR_PIECE || item == ITEM_STAR_PIECE) {
        add_star_pieces(1);
    } else if (type == ITEM_TYPE_KEY) {
        if (item == ITEM_KOOPA_FORTRESS_KEY) {
            playerData->fortressKeyCount++;   // and into the list: the fortress doors look there
        }
        for (i = 0; i < KEY_SLOTS && KEY_ITEM(i) != ITEM_NONE; i++) {
        }
        if (i < KEY_SLOTS) {
            KEY_ITEM(i) = item;
        }
    } else if (type == ITEM_TYPE_BADGE) {
        idx = add_badge(item);
    } else {
        idx = add_item(item);
    }
    if (out != 0) {   // 0: the caller wants no index (a key item reward)
        evt_set_variable(script, out, idx);
    }
    return ApiStatus_DONE2;
}

static const char* current_map(void) {
    if (gGameStatusPtr->areaID < 0) {
        return "";
    }
    return gAreas[gGameStatusPtr->areaID].maps[gGameStatusPtr->mapID].id;
}

/* a shop entry with the item its slot sells now (price, description to match) */
static void shop_entry(ShopItemData* e, bool rowf) {
    const char* map = current_map();
    s32 i, j, item;
    bool found;

    for (i = 0; i < NUM_SHOPS; i++) {
        if (sShops[i].rowf == rowf && sShops[i].item == (s32)e->itemID && strcmp(sShops[i].map, map) == 0) {
            break;
        }
    }
    if (i == NUM_SHOPS || sShopItem[i] == sShops[i].item) {
        return;
    }
    item = sShopItem[i];
    e->itemID = item;
    e->descMsg = gItemTable[item].shortDescMsg;
    e->price = rowf ? gItemTable[item].sellValue : gItemTable[item].sellValue * 2;
    for (j = 0, found = false; j < NUM_SHOPS; j++) {   // sold in vanilla: its shop blurb, its highest price
        if (sShops[j].item == item) {
            e->descMsg = sShops[j].descMsg;
            if (!rowf && (!found || sShops[j].price > e->price)) {
                e->price = sShops[j].price;
            }
            found = true;
        }
    }
    if (e->price < 1) {
        e->price = 1;
    }
    pm3ds_log(PM3DS_LOG_INFO, "rando: %s %s %s -> %s for %d", map, rowf ? "Rowf" : "shop", item_name(sShops[i].item),
              item_name(item), (int)e->price);
}

/* src/world/script_api/shops.c, MakeShop: the inventory to use */
ShopItemData* pm3ds_rando_shop(ShopItemData* inventory) {
    static ShopItemData copy[16];
    s32 n;

    if (sFlags == 0) {
        return inventory;
    }
    for (n = 0; n < ARRAY_COUNT(copy) - 1 && inventory[n].itemID != ITEM_NONE; n++) {
        copy[n] = inventory[n];
        shop_entry(&copy[n], false);
    }
    copy[n].itemID = ITEM_NONE;
    return copy;
}

/* src/world/area_mac/mac_01/shop.c: one of Rowf's badges as he stocks it */
void pm3ds_rando_rowf(ShopItemData* entry) {
    if (sFlags != 0) {
        shop_entry(entry, true);
    }
}

/* ---- enemies (PM3DS_RANDO_ENEMIES) ------------------------------------------ */
/* A battle you start by touching an enemy becomes another regular formation of
 * the same chapter, on that formation's own stage: the same one every time in
 * this seed (a permutation per chapter, so every formation still turns up).
 * Bosses, story fights and the special ones a regular area holds (Kent C.
 * Koopa, Tubba Blubba, Buzzar, Anti Guy...) are left out of the pools, so they
 * stay what they are. A separate stream of the seed: the items don't change. */
static const struct { u8 area, from, to, tier; } sEnemyRanges[] = {
    { BTL_AREA_KMR_1, 0x00, 0x0B, 0 },
    { BTL_AREA_NOK, 0x00, 0x17, 1 },  { BTL_AREA_TRD_1, 0x00, 0x1A, 1 },
    { BTL_AREA_IWA, 0x00, 0x0B, 2 },  { BTL_AREA_SBK, 0x00, 0x10, 2 },   { BTL_AREA_ISK_1, 0x00, 0x13, 2 },
    { BTL_AREA_MIM, 0x00, 0x11, 3 },  { BTL_AREA_ARN, 0x00, 0x0E, 3 },   { BTL_AREA_DGB, 0x00, 0x03, 3 },
    { BTL_AREA_OMO, 0x00, 0x2B, 4 },  { BTL_AREA_TIK, 0x00, 0x18, 4 },   { BTL_AREA_TIK3, 0x00, 0x03, 4 },
    { BTL_AREA_JAN, 0x00, 0x10, 5 },  { BTL_AREA_JAN2, 0x00, 0x03, 5 },  { BTL_AREA_KZN, 0x00, 0x14, 5 },
    { BTL_AREA_FLO, 0x00, 0x24, 6 },  { BTL_AREA_FLO2, 0x01, 0x04, 6 },
    { BTL_AREA_SAM, 0x00, 0x0D, 7 },  { BTL_AREA_PRA, 0x00, 0x0D, 7 },   { BTL_AREA_PRA3, 0x00, 0x0B, 7 },
    { BTL_AREA_KPA, 0x00, 0x37, 8 },  { BTL_AREA_KPA3, 0x01, 0x02, 8 },  { BTL_AREA_KPA4, 0x00, 0x04, 8 },
};
#define MAX_ENEMY_BATTLES 512
#define ENEMY_TIERS 9
static u16 sEnemyFrom[MAX_ENEMY_BATTLES], sEnemyTo[MAX_ENEMY_BATTLES];
static s32 sNumEnemy;

/* Badge costs (PM3DS_RANDO_BADGES): the BP costs of the badges dealt out again among
 * them, and the FP costs among the badge moves that use FP, the same for a seed.
 * Speedy Spin keeps its cost (asked for). The vanilla costs come back whenever the
 * option is off, so another file is never affected. */
#define MOVE_TABLE_SIZE ARRAY_COUNT(gMoveTable)
static u8 sMoveBP[MOVE_TABLE_SIZE], sMoveFP[MOVE_TABLE_SIZE];
static bool sMoveCostsSaved;

static void build_badge_costs(void) {
    u32 st = sSeed ^ 0x42414447u;   /* "BADG" */
    u8 moves[ITEM_LAST_BADGE - ITEM_FIRST_BADGE + 1], fpMoves[ITEM_LAST_BADGE - ITEM_FIRST_BADGE + 1];
    u8 pool[ITEM_LAST_BADGE - ITEM_FIRST_BADGE + 1];
    s32 n = 0, nf = 0, i, j;

    if (!sMoveCostsSaved) {
        for (i = 0; i < MOVE_TABLE_SIZE; i++) {
            sMoveBP[i] = gMoveTable[i].costBP;
            sMoveFP[i] = gMoveTable[i].costFP;
        }
        sMoveCostsSaved = true;
    }
    for (i = 0; i < MOVE_TABLE_SIZE; i++) {
        gMoveTable[i].costBP = sMoveBP[i];
        gMoveTable[i].costFP = sMoveFP[i];
    }
    if (!(sFlags & PM3DS_RANDO_BADGES)) {
        return;
    }
    for (i = ITEM_FIRST_BADGE; i <= ITEM_LAST_BADGE; i++) {
        u8 mv = gItemTable[i].moveID;
        bool seen = false;
        if (mv == 0 || mv == MOVE_SPEEDY_SPIN || sMoveBP[mv] == 0) {
            continue;   // no move, the kept one, or an unused badge
        }
        for (j = 0; j < n; j++) {
            seen |= moves[j] == mv;   // several badges share a move (HP Plus A, B, C...)
        }
        if (seen) {
            continue;
        }
        moves[n++] = mv;
        if (sMoveFP[mv] != 0) {
            fpMoves[nf++] = mv;
        }
    }
    for (i = 0; i < n; i++) {
        pool[i] = sMoveBP[moves[i]];
    }
    for (i = n - 1; i > 0; i--) {
        j = next_rand(&st) % (u32)(i + 1);
        u8 t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    for (i = 0; i < n; i++) {
        gMoveTable[moves[i]].costBP = pool[i];
    }
    for (i = 0; i < nf; i++) {
        pool[i] = sMoveFP[fpMoves[i]];
    }
    for (i = nf - 1; i > 0; i--) {
        j = next_rand(&st) % (u32)(i + 1);
        u8 t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    for (i = 0; i < nf; i++) {
        gMoveTable[fpMoves[i]].costFP = pool[i];
    }
}

/* cheats.c: the free QoL badges cheat went off */
void pm3ds_rando_badge_costs_refresh(void) {
    build_badge_costs();
}

static void build_enemies(void) {
    u32 st = sSeed ^ 0x454E4D59u;   /* "ENMY" */
    s32 tier, r, k, n;

    sNumEnemy = 0;
    if (!(sFlags & PM3DS_RANDO_ENEMIES)) {
        return;
    }
    for (tier = 0; tier < ENEMY_TIERS; tier++) {
        s32 first = sNumEnemy;
        for (r = 0; r < ARRAY_COUNT(sEnemyRanges); r++) {
            if (sEnemyRanges[r].tier != tier) {
                continue;
            }
            for (k = sEnemyRanges[r].from; k <= sEnemyRanges[r].to && sNumEnemy < MAX_ENEMY_BATTLES; k++) {
                sEnemyFrom[sNumEnemy] = sEnemyTo[sNumEnemy] = (u16)((sEnemyRanges[r].area << 8) | k);
                sNumEnemy++;
            }
        }
        n = sNumEnemy - first;
        for (k = n - 1; k > 0; k--) {   /* Fisher-Yates over this tier's targets */
            s32 j = (s32)(next_rand(&st) % (u32)(k + 1));
            u16 t = sEnemyTo[first + k];
            sEnemyTo[first + k] = sEnemyTo[first + j];
            sEnemyTo[first + j] = t;
        }
    }
}

/* src/encounter.c, a battle started by touching an enemy: the battle to fight, and its stage (-1: its own) */
s32 pm3ds_rando_battle(s32 battle, s32* stage) {
    s32 i;

    if (!(sFlags & PM3DS_RANDO_ENEMIES)) {
        return battle;
    }
    for (i = 0; i < sNumEnemy; i++) {
        if (sEnemyFrom[i] == battle) {
            if (sEnemyTo[i] != battle) {
                pm3ds_log(PM3DS_LOG_INFO, "rando: battle %04X -> %04X", (unsigned)battle, (unsigned)sEnemyTo[i]);
                *stage = -1;
            }
            return sEnemyTo[i];
        }
    }
    return battle;
}

/* src/evt/map_api.c, goto_map (GotoMap and its kin), before the map change: the door
 * shuffle's destination for a door taken here (not in the title screen's demo) */
void pm3ds_rando_door(s16* areaID, s16* mapID, s32* entryID) {
    GameStatus* gs = gGameStatusPtr;
    const char* to;
    int entry = *entryID;
    s16 a, m;

    if (!(sFlags & PM3DS_RANDO_DOORS) || gs->demoState != DEMO_STATE_NONE || gs->areaID < 0 || *areaID < 0) {
        return;
    }
    to = gAreas[*areaID].maps[*mapID].id;
    if (pm3ds_doors_redirect(gAreas[gs->areaID].maps[gs->mapID].id, &to, &entry) && get_map_IDs_by_name(to, &a, &m)) {
        *areaID = a;
        *mapID = m;
        *entryID = entry;
    }
}

/* src/main_loop.c etc., clear_saved_variables: a fresh state is unrandomized */
void pm3ds_rando_off(void) {
    sFlags = 0;
    sSeed = 0;
    sTaken = 0;
    build();
    build_enemies();
    build_badge_costs();
    build_doors();
}

/* platform/bottom_screen.c, new-game picker */
void pm3ds_rando_choose(unsigned flags, unsigned seed) {
    sNextFlags = (flags & PM3DS_RANDO_ON) ? (flags & PM3DS_RANDO_ALL) : 0;
    sNextSeed = seed;
}

/* src/filemenu/filemenu_yesno.c, new file: the picker's choice */
void pm3ds_rando_new_game(void) {
    extern unsigned int pm3ds_random_seed(void);
#ifdef PM3DS_RANDO_TEST
    sNextFlags = PM3DS_RANDO_ALL;
#ifdef PM3DS_RANDO_SEED
    sNextFlags = PM3DS_RANDO_ON | PM3DS_RANDO_COINS | PM3DS_RANDO_KEYS | PM3DS_RANDO_SHOPS | PM3DS_RANDO_GIFTS;
    sNextSeed = PM3DS_RANDO_SEED;
#endif
#endif
#ifdef PM3DS_RANDO_DOORS_TEST   /* debug: -DPM3DS_RANDO_DOORS_TEST=0x<seed>: a New Game shuffles items and doors with it */
    sNextFlags = PM3DS_RANDO_ON | PM3DS_RANDO_DOORS;
    sNextSeed = PM3DS_RANDO_DOORS_TEST;
#endif
    if (sNextFlags == 0) {
        pm3ds_rando_off();
        return;
    }
    sFlags = sNextFlags;
    sTaken = 0;
    sSeed = sNextSeed != 0 ? sNextSeed : pm3ds_random_seed();
    if (sSeed == 0) {
        sSeed = 1;
    }
    build();
    build_enemies();
    build_badge_costs();
    build_doors();
    write_spoiler();
}

/* src/fio.c */
void pm3ds_rando_serialize(void* dst) {
    RandoSaveBlock b = { RANDO_MAGIC, 3, (s32)sFlags, sSeed };
    u32 taken[2] = { TAKEN_MAGIC, sTaken };
    memcpy(dst, &b, sizeof(b));
    memcpy((u8*)dst + TAKEN_BLOCK_OFS, taken, sizeof(taken));
}

void pm3ds_rando_deserialize(const void* src) {
    RandoSaveBlock b;
    u32 taken[2];
    memcpy(&b, src, sizeof(b));
    memcpy(taken, (const u8*)src + TAKEN_BLOCK_OFS, sizeof(taken));
    sTaken = taken[0] == TAKEN_MAGIC ? taken[1] : 0;
    if (b.magic == RANDO_MAGIC && b.version == 1 && b.flags > 0 && b.flags < ARRAY_COUNT(sV1Flags)) {
        sFlags = sV1Flags[b.flags];
        sSeed = b.seed;
        build();
    build_enemies();
    build_badge_costs();
    build_doors();
    } else if (b.magic == RANDO_MAGIC && (b.version == 2 || b.version == 3) && (b.flags & PM3DS_RANDO_ON)) {
        sFlags = (u32)b.flags & PM3DS_RANDO_ALL;
        if (b.version == 2) {
            sFlags |= PM3DS_RANDO_HIDDEN;   // placed before the option existed
        }
        sSeed = b.seed;
        build();
    build_enemies();
    build_badge_costs();
    build_doors();
    } else {
        pm3ds_rando_off();
    }
}

/* for the bottom screen */
unsigned pm3ds_rando_seed(void) {
    return sFlags == 0 ? 0 : sSeed;
}

#ifdef PM3DS_RANDO_TEST
#include "game_modes.h"

/* Debug (-DPM3DS_RANDO_TEST with PM3DS_START_MAP): the debug New Game randomizes everything,
 * then a bush/tree spot whose item was swapped drops it twice the way a bush does
 * (ITEM_SPAWN_MODE_FALL_SPAWN_ONCE) next to an unswapped one; 600 frames later the log tells
 * whether each is still there and whether its spot is marked taken. */
extern bool pm3ds_debug_new_game_made;

static s32 test_find(bool swapped) {
    s32 i;

    for (i = 0; i < NUM_LOCS; i++) {
        if ((sLocs[i].src == SRC_BUSH || sLocs[i].src == SRC_TREE) && sLocs[i].flag != 0 &&
            (sItem[i] != sLocs[i].item) == swapped && !get_global_flag(sLocs[i].flag)) {
            return i;
        }
    }
    return -1;
}

void pm3ds_rando_test_tick(void) {
    static s32 timer, ids[3], locs[2];
    s32 k;

    if (!pm3ds_debug_new_game_made || get_game_mode() != GAME_MODE_WORLD) {
        return;
    }
    timer++;
#ifdef PM3DS_RANDO_TEST_FLAG
    /* -DPM3DS_RANDO_TEST_FLAG=GF_x [-DPM3DS_RANDO_TEST_MODE=ITEM_SPAWN_MODE_x]: that spot's drop,
     * made the way its bush/tree makes it, lands on Mario; the log follows it and the key items */
    {
        static s32 id = -1, loc = -1;
        if (timer == 120) {
            for (k = 0; k < NUM_LOCS; k++) {
                if (sLocs[k].flag == PM3DS_RANDO_TEST_FLAG) loc = k;
            }
            if (loc >= 0) {
#ifndef PM3DS_RANDO_TEST_MODE
#define PM3DS_RANDO_TEST_MODE ITEM_SPAWN_MODE_TOSS_SPAWN_ONCE
#endif
                id = make_item_entity_nodelay(sLocs[loc].item, gPlayerStatus.pos.x, gPlayerStatus.pos.y + 10,
                                              gPlayerStatus.pos.z, PM3DS_RANDO_TEST_MODE, sLocs[loc].flag);
            }
            pm3ds_log(2, "RANDOSPOT: %s %s %s -> item %d (%s): entity %d", loc >= 0 ? sLocs[loc].map : "?",
                      loc >= 0 ? s_src_names[sLocs[loc].src] : "?", loc >= 0 ? sLocs[loc].itemName : "?",
                      loc >= 0 ? sItem[loc] : -1, loc >= 0 ? item_name(sItem[loc]) : "?", id);
        }
        if ((timer == 600 || timer == 840) && loc >= 0) {   // the map's script clears the flag on the next visit: drop again
            set_global_flag(sLocs[loc].flag);
            clear_global_flag(sLocs[loc].flag);
            if (id >= 0 && get_item_entity(id) != nullptr) {
                remove_item_entity_by_index(id);
            }
            id = make_item_entity_nodelay(sLocs[loc].item, gPlayerStatus.pos.x, gPlayerStatus.pos.y + 10,
                                          gPlayerStatus.pos.z, PM3DS_RANDO_TEST_MODE, sLocs[loc].flag);
            pm3ds_log(2, "RANDOSPOT again: entity %d item %d (taken bits %x)", id,
                      id >= 0 && get_item_entity(id) ? get_item_entity(id)->itemID : -1, sTaken);
        }
        if (timer > 120 && timer <= 900 && timer % 60 == 0 && loc >= 0) {
            ItemEntity* e = id >= 0 ? get_item_entity(id) : nullptr;
            s32 nk = 0, last = 0;
            for (k = 0; k < KEY_SLOTS; k++) {
                if (KEY_ITEM(k) != ITEM_NONE) { nk++; last = KEY_ITEM(k); }
            }
            pm3ds_log(2, "RANDOSPOT t=%d: entity %s item %d state %d flags %08X | flag %d | %d key items, last %d | coins %d",
                      (int)timer, e ? "there" : "gone", e ? e->itemID : -1, e ? e->state : -1, e ? (unsigned)e->flags : 0,
                      get_global_flag(sLocs[loc].flag), nk, last, gPlayerData.coins);
        }
        return;
    }
#endif
    if (timer == 120) {
        locs[0] = test_find(true);
        locs[1] = test_find(false);
        if (locs[0] < 0 || locs[1] < 0) {
            pm3ds_log(2, "RANDOTEST: no spots (flags %x) swapped %d plain %d", sFlags, locs[0], locs[1]);
            return;
        }
        for (k = 0; k < 3; k++) {
            s32 l = locs[k == 2 ? 1 : 0];
            ids[k] = make_item_entity(sLocs[l].item, gPlayerStatus.pos.x + 70 + 40 * k, gPlayerStatus.pos.y + 40,
                                      gPlayerStatus.pos.z, ITEM_SPAWN_MODE_FALL_SPAWN_ONCE, 0, 0, sLocs[l].flag);
        }
        pm3ds_log(2, "RANDOTEST: swapped %s (%s -> item %d): drop %d, again %d, flag %d | plain %s: drop %d, flag %d",
                  sLocs[locs[0]].map, sLocs[locs[0]].itemName, sItem[locs[0]], ids[0], ids[1],
                  get_global_flag(sLocs[locs[0]].flag), sLocs[locs[1]].map, ids[2], get_global_flag(sLocs[locs[1]].flag));
    }
    if (timer == 720 && locs[0] >= 0 && locs[1] >= 0) {
        pm3ds_log(2, "RANDOTEST: after 600 frames: swapped item %s, flag %d | plain item %s, flag %d",
                  ids[0] >= 0 && get_item_entity(ids[0]) != nullptr ? "there" : "gone", get_global_flag(sLocs[locs[0]].flag),
                  ids[2] >= 0 && get_item_entity(ids[2]) != nullptr ? "there" : "gone", get_global_flag(sLocs[locs[1]].flag));
    }
}
#endif
