/*
 * achievements.c — Star Haven's achievements (GitHub #88).
 *
 * Forty of them, in pm3ds_hud_info.h's list order: the story (each chapter's Star
 * Spirit, Bowser), the party and gear, collections (Star Pieces, badges, recipes,
 * tattles, letters, Koopa Koot, Chuck Quizmo, Merlow), totals the game counts
 * itself (PlayerData: battles won, attacks guarded) and challenges. Most are read
 * from the game's own state twice a second (and once, silently, when a file is
 * loaded: what the save already proves counts at once); the battle ones are
 * checked when a battle is won (src/battle/btl_states_actions.c, where the game
 * counts the win), from what was watched during it (Mario's HP going down, a
 * chapter boss on the field, the badges worn). The challenges don't count while
 * one of the cheats is switched on. The bottom screen lists them (a page of the
 * Recipes tab) and pops a notice up when one is earned (platform/bottom_screen.c).
 *
 * Kept in the save file, in SaveData.pad: 32 bytes after the magic string that
 * vanilla saves leave zero and nothing reads (src/fio.c), inside the CRC, so copies
 * keep them and old versions, which never touch them, keep them as they are:
 *   pad + 0x00  'SHAC' magic        (anything else, as in an old save: none earned)
 *   pad + 0x04  version, 1
 *   pad + 0x08  file id: random, made at New Game; an older save without the block gets
 *               one made from its slot and file name, the same on every load; ties
 *               the SD card's side file (below) to this file
 *   pad + 0x0C  PlayerData.frameCounter when the block was written. Play time only
 *               grows within a file, so a block that is ahead of its file's own time
 *               came from another one (an old version's New Game saves whatever the
 *               buffer held, the block of the file loaded before) and is ignored
 *   pad + 0x10  earned bits 0-31, pad + 0x14 bits 32-63
 *   pad + 0x18  Parakarry's letters Mario has had, a bit each (n64_shim/journal.c: a
 *               delivered letter leaves no flag of its own)
 *   pad + 0x1C  the file slot + 1 the block was written for. A block from another slot
 *               (an old version's New Game saving the buffer of the file loaded before)
 *               is ignored; copying a file writes the new slot (serialize runs again)
 * A save is only written when the player saves, and the game never saves after the
 * final battle, so every achievement is also written straight away to
 * <save name>.ach next to the save on the SD card, per file slot, with the file id;
 * loading a file adds the side file's bits when the ids match.
 */
#include "common.h"
#include "battle/battle.h"
#include "hud_element.h"
#include "game_modes.h"
#include "npc.h"
#include "message_ids.h"
#include "fio.h"

#include <stdio.h>
#include <string.h>

#include "pm3ds_hud_info.h"
#include "pm3ds_cheats.h"
#include "pm3ds_brand.h"
#include "pm3ds_rom_layout.h"

#define ACH_MAGIC 0x53484143   /* "SHAC" */
#define ACH_VERSION 1
#define ACH_SIDE_MAGIC 0x53484153   /* "SHAS" */
#define ACH_SIDE_PATH PM3DS_SD_DIR "/" PM3DS_SAVE_NAME ".ach"

typedef struct {
    u32 magic;
    u32 version;
    u32 fileID;
    u32 frames;
    u32 bits[2];
    u32 letters;
    u32 owner;     /* saveSlot + 1 */
} AchSaveBlock;   /* in SaveData.pad (32 bytes) */
_Static_assert(sizeof(AchSaveBlock) <= sizeof(((SaveData*)0)->pad), "achievement block past SaveData.pad");

typedef struct {
    u32 magic;
    struct { u32 fileID; u32 bits[2]; } slot[4];
} AchSideFile;

enum {
    ACH_PROLOGUE, ACH_CH1, ACH_CH2, ACH_CH3, ACH_CH4, ACH_CH5, ACH_CH6, ACH_CH7, ACH_BOWSER,
    ACH_PARTY, ACH_ULTRA_ONE, ACH_ULTRA_ALL, ACH_GEAR,
    ACH_SP_50, ACH_SP_ALL, ACH_BADGES_40, ACH_BADGES_ALL, ACH_COOK_ONE, ACH_COOK_ALL,
    ACH_TATTLE_30, ACH_TATTLE_100, ACH_LETTERS, ACH_CHAIN, ACH_KOOT, ACH_QUIZ, ACH_MERLOW,
    ACH_LEVEL_27, ACH_COINS, ACH_WINS_100, ACH_GUARD_50,
    ACH_NO_DAMAGE, ACH_DANGER, ACH_PERIL, ACH_BOSS_NO_BADGES, ACH_BOSS_FLAWLESS, ACH_LOW_LEVEL,
    ACH_KENT, ACH_ANTI_GUY, ACH_MASTER, ACH_HARD,
    ACH_COUNT
};
_Static_assert(ACH_COUNT == PM3DS_ACH_COUNT, "pm3ds_hud_info.h's count");
_Static_assert(ACH_COUNT > 32 && ACH_COUNT <= 64, "two words of bits");
#define HIGH_MASK ((1u << (ACH_COUNT - 32)) - 1)   /* the second word's bits in use */

#define AF_HIDDEN    1   /* ??? until earned */
#define AF_CHALLENGE 2   /* not with a cheat on */

typedef struct {
    const char* name;   /* English: the bottom screen translates (pm3ds_lang_table.inc) */
    const char* how;
    s16 icon;           /* an item, for its icon */
    u8 flags;
} AchDef;

static const AchDef sAch[ACH_COUNT] = {
    [ACH_PROLOGUE]  = { "A Fresh Start", "Finish the prologue", ITEM_HAMMER, 0 },
    [ACH_CH1]       = { "Eldstar Rescued", "Rescue the Star Spirit of chapter 1", ITEM_KOOPA_FORTRESS_KEY, 0 },
    [ACH_CH2]       = { "Mamar Rescued", "Rescue the Star Spirit of chapter 2", ITEM_PULSE_STONE, 0 },
    [ACH_CH3]       = { "Skolar Rescued", "Rescue the Star Spirit of chapter 3", ITEM_BOO_PORTRAIT, 0 },
    [ACH_CH4]       = { "Muskular Rescued", "Rescue the Star Spirit of chapter 4", ITEM_TOY_TRAIN, 0 },
    [ACH_CH5]       = { "Misstar Rescued", "Rescue the Star Spirit of chapter 5", ITEM_JADE_RAVEN, 0 },
    [ACH_CH6]       = { "Klevar Rescued", "Rescue the Star Spirit of chapter 6", ITEM_MAGICAL_BEAN, 0 },
    [ACH_CH7]       = { "Kalmar Rescued", "Rescue the Star Spirit of chapter 7", ITEM_STAR_STONE, 0 },
    [ACH_BOWSER]    = { "Happy Ending", "Beat Bowser and save Princess Peach", ITEM_LUCKY_STAR, 0 },
    [ACH_PARTY]     = { "Full Party", "Have all eight partners join you", ITEM_QUICK_CHANGE, 0 },
    [ACH_ULTRA_ONE] = { "Ultra Rank", "Raise a partner to Ultra rank", ITEM_ULTRA_STONE, 0 },
    [ACH_ULTRA_ALL] = { "Super Team", "Raise every partner to Ultra rank", ITEM_GROUP_FOCUS, 0 },
    [ACH_GEAR]      = { "Ultra Gear", "Get the Ultra Boots and the Ultra Hammer", ITEM_ULTRA_HAMMER, 0 },
    [ACH_SP_50]     = { "Shiny Things", "Collect 50 Star Pieces", ITEM_STAR_PIECE, 0 },
    [ACH_SP_ALL]    = { "Star Piece Master", "Collect all 160 Star Pieces", ITEM_STAR_PIECE, 0 },
    [ACH_BADGES_40] = { "Badge Fan", "Own 40 different badges", ITEM_HP_PLUS_A, 0 },
    [ACH_BADGES_ALL] = { "Badge Collector", "Own all 80 badges", ITEM_POWER_PLUS_A, 0 },
    [ACH_COOK_ONE]  = { "Taste Test", "Have Tayce T. cook a dish", ITEM_FRIED_SHROOM, 0 },
    [ACH_COOK_ALL]  = { "Master Chef", "Have Tayce T. cook every recipe", ITEM_COOKBOOK, 0 },
    [ACH_TATTLE_30] = { "Tattletale", "Tattle 30 kinds of enemies", ITEM_PEEKABOO, 0 },
    [ACH_TATTLE_100] = { "Know Your Enemy", "Tattle 100 kinds of enemies", ITEM_I_SPY, 0 },
    [ACH_LETTERS]   = { "Special Delivery", "Deliver every letter with Parakarry", ITEM_LETTER_TO_MERLON, 0 },
    [ACH_CHAIN]     = { "Pen Pals", "Finish Goompapa's chain of letters", ITEM_LUCKY_DAY, AF_HIDDEN },
    [ACH_KOOT]      = { "Good Neighbor", "Do all of Koopa Koot's favors", ITEM_SILVER_CREDIT, 0 },
    [ACH_QUIZ]      = { "Quiz Whiz", "Answer all 64 of Chuck Quizmo's questions", ITEM_MYSTERY, 0 },
    [ACH_MERLOW]    = { "Star Shopper", "Buy every badge Merlow sells", ITEM_HAPPY_HEART_A, AF_HIDDEN },
    [ACH_LEVEL_27]  = { "Max Level", "Reach level 27", ITEM_JAMMIN_JELLY, 0 },
    [ACH_COINS]     = { "Piggy Bank", "Hold 999 coins at once", ITEM_COIN, AF_CHALLENGE },
    [ACH_WINS_100]  = { "Battle Hardened", "Win 100 battles", ITEM_POWER_JUMP, 0 },
    [ACH_GUARD_50]  = { "Good Guard", "Guard 50 attacks with A", ITEM_DEFEND_PLUS_A, 0 },
    [ACH_NO_DAMAGE] = { "Untouchable", "Win a battle without Mario taking damage", ITEM_DAMAGE_DODGE_A, AF_CHALLENGE },
    [ACH_DANGER]    = { "Living Dangerously", "Win a battle with 5 HP or less", ITEM_POWER_RUSH, AF_CHALLENGE },
    [ACH_PERIL]     = { "By a Thread", "Win a battle with only 1 HP left", ITEM_MEGA_RUSH, AF_CHALLENGE | AF_HIDDEN },
    [ACH_BOSS_NO_BADGES] = { "No Frills", "Beat a chapter boss with no badges on", ITEM_FIRST_ATTACK, AF_CHALLENGE },
    [ACH_BOSS_FLAWLESS] = { "Flawless", "Beat a chapter boss without Mario taking damage", ITEM_SPIKE_SHIELD, AF_CHALLENGE },
    [ACH_LOW_LEVEL] = { "Low Level Hero", "Beat Bowser at level 10 or lower", ITEM_ALLOR_NOTHING, AF_CHALLENGE | AF_HIDDEN },
    [ACH_KENT]      = { "Toll Paid", "Beat Kent C. Koopa on Pleasant Path", ITEM_KOOPA_LEAF, AF_HIDDEN },
    [ACH_ANTI_GUY]  = { "Anti Hero", "Beat the Anti Guy in Shy Guy's Toy Box", ITEM_LEMON_CANDY, AF_HIDDEN },
    [ACH_MASTER]    = { "Dojo Master", "Beat The Master's last challenge at the Dojo", ITEM_DIPLOMA, AF_HIDDEN },
    [ACH_HARD]      = { "Hard as Nails", "Beat Bowser in Hard mode", ITEM_P_UP_D_DOWN, AF_CHALLENGE },
};

/* journal.c */
void pm3ds_tattle_count(int* known, int* total);
void pm3ds_recipe_counts(int* cooked, int* total);
void pm3ds_letter_counts(int* delivered, int* total, bool* chainDone);
void pm3ds_favor_counts(int* done, int* total);
u32 pm3ds_letters_had(void);
void pm3ds_letters_had_set(u32 had, bool loaded);
extern uint64_t pm3ds_clock_now_us(void);   /* platform/pm3ds_clock.c */
void pm3ds_log(int level, const char* fmt, ...);

static u32 sBits[2];
static u32 sFileID;
static bool sActive;      /* a file is loaded or made in this session */
static bool sRetro;       /* just loaded: the next check is silent */
static u32 sTick;

/* earned notices for the bottom screen, oldest first */
#define NOTICE_MAX 8
static s8 sNotice[NOTICE_MAX];
static volatile int sNoticeHead, sNoticeTail;

static bool earned(s32 a) {
    return (sBits[a / 32] >> (a % 32)) & 1;
}

static u32 sSideBits[2];   /* what the side file holds for this file */

/* an older save's id: from its slot and name, so every load gives the same one */
static u32 legacy_file_id(void) {
    s32 slot = gGameStatusPtr->saveSlot & 3;
    const u8* name = (const u8*)gSaveSlotMetadata[slot].filename;
    u32 h = 2166136261u ^ (u32)slot;
    for (u32 i = 0; i < sizeof(gSaveSlotMetadata[slot].filename); i++) {
        h = (h ^ name[i]) * 16777619u;
    }
    return h != 0 ? h : 1;
}

static u32 new_file_id(void) {
    u32 id = (u32)pm3ds_clock_now_us() * 2654435761u ^ (u32)(pm3ds_clock_now_us() >> 32) ^ 0x5A17u;
    return id != 0 ? id : 1;
}

/* ---- the side file -------------------------------------------------------- */
static void side_read(AchSideFile* f) {
    memset(f, 0, sizeof(*f));
    FILE* fp = fopen(ACH_SIDE_PATH, "rb");
    if (fp != nullptr) {
        if (fread(f, sizeof(*f), 1, fp) != 1 || f->magic != ACH_SIDE_MAGIC) {
            memset(f, 0, sizeof(*f));
        }
        fclose(fp);
    }
}

static void side_write(void) {
    AchSideFile f;
    s32 slot = gGameStatusPtr->saveSlot & 3;
    side_read(&f);
    f.magic = ACH_SIDE_MAGIC;
    f.slot[slot].fileID = sFileID;
    f.slot[slot].bits[0] = sBits[0];
    f.slot[slot].bits[1] = sBits[1];
    sSideBits[0] = sBits[0];
    sSideBits[1] = sBits[1];
    FILE* fp = fopen(ACH_SIDE_PATH, "wb");
    if (fp != nullptr) {
        fwrite(&f, sizeof(f), 1, fp);
        fclose(fp);
    }
}

/* ---- earning ---------------------------------------------------------------- */
static bool any_cheat_on(void) {
    for (s32 c = 0; c < PM3DS_CHEAT_COUNT; c++) {
        if (pm3ds_cheat_on(c)) {
            return true;
        }
    }
    return false;
}

static void earn(s32 a, bool silent) {
    if (earned(a) || ((sAch[a].flags & AF_CHALLENGE) && any_cheat_on())) {
        return;
    }
    sBits[a / 32] |= 1u << (a % 32);
    pm3ds_log(2, "achievement %d earned%s", (int)a, silent ? " (from the save)" : "");
    if (!silent) {
        s32 next = (sNoticeTail + 1) % NOTICE_MAX;
        if (next != sNoticeHead) {
            sNotice[sNoticeTail] = a;
            sNoticeTail = next;
        }
    }
}

/* ---- what the game's state proves ------------------------------------------ */
static s32 story(void) {
    return evt_get_variable(nullptr, GB_StoryProgress);
}

static bool flag(s32 f) {
    return evt_get_variable(nullptr, f) != 0;
}

static bool bowser_beaten(void) {
    return flag(GF_KKJ25_Defeated_Bowser) || story() >= STORY_EPILOGUE;
}

static const u8 sPartners[] = { PARTNER_GOOMBARIO, PARTNER_KOOPER, PARTNER_BOMBETTE, PARTNER_PARAKARRY,
                                PARTNER_BOW, PARTNER_WATT, PARTNER_SUSHIE, PARTNER_LAKILESTER };

static s32 badges_owned(void) {
    u8 seen[(ITEM_LAST_BADGE - ITEM_FIRST_BADGE) / 8 + 1];
    s32 n = 0;
    memset(seen, 0, sizeof(seen));
    for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.badges); i++) {
        s32 b = gPlayerData.badges[i];
        if (IS_BADGE(b)) {
            s32 k = b - ITEM_FIRST_BADGE;
            if (!(seen[k / 8] & (1 << (k % 8)))) {
                seen[k / 8] |= 1 << (k % 8);
                n++;
            }
        }
    }
    return n;
}

static void check_state(bool silent) {
    static const struct { u8 ach; s8 story; } chapters[] = {
        { ACH_PROLOGUE, STORY_CH0_RETURNED_TO_TOAD_TOWN }, { ACH_CH1, STORY_CH1_STAR_SPIRIT_RESCUED },
        { ACH_CH2, STORY_CH2_STAR_SPIRIT_RESCUED },        { ACH_CH3, STORY_CH3_STAR_SPIRIT_RESCUED },
        { ACH_CH4, STORY_CH4_STAR_SPIRIT_RESCUED },        { ACH_CH5, STORY_CH5_DEFEATED_LAVA_PIRANHA },
        { ACH_CH6, STORY_CH6_STAR_SPIRIT_RESCUED },        { ACH_CH7, STORY_CH7_STAR_SPIRIT_RESCUED },
    };
    s32 st = story();
    for (u32 i = 0; i < ARRAY_COUNT(chapters); i++) {
        if (st >= chapters[i].story) {
            earn(chapters[i].ach, silent);
        }
    }
    if (bowser_beaten()) {
        earn(ACH_BOWSER, silent);
        if (pm3ds_hard_mode()) {
            earn(ACH_HARD, silent);
        }
        if (gPlayerData.level <= 10) {
            earn(ACH_LOW_LEVEL, silent);
        }
    }

    s32 joined = 0, ultra = 0;
    for (u32 i = 0; i < ARRAY_COUNT(sPartners); i++) {
        PartnerData* p = &gPlayerData.partners[sPartners[i]];
        joined += p->enabled != 0;
        ultra += p->enabled && p->level >= PARTNER_RANK_ULTRA;
    }
    if (joined == ARRAY_COUNT(sPartners)) earn(ACH_PARTY, silent);
    if (ultra > 0) earn(ACH_ULTRA_ONE, silent);
    if (ultra == ARRAY_COUNT(sPartners)) earn(ACH_ULTRA_ALL, silent);
    if ((gPlayerData.bootsLevel >= GEAR_RANK_ULTRA || pm3ds_gear_best(0) >= GEAR_RANK_ULTRA) &&
        (gPlayerData.hammerLevel >= GEAR_RANK_ULTRA || pm3ds_gear_best(1) >= GEAR_RANK_ULTRA)) {
        earn(ACH_GEAR, silent);
    }

    if (gPlayerData.starPiecesCollected >= 50) earn(ACH_SP_50, silent);
    if (gPlayerData.starPiecesCollected >= 160) earn(ACH_SP_ALL, silent);
    s32 badges = badges_owned();
    if (badges >= 40) earn(ACH_BADGES_40, silent);
    if (badges >= 80) earn(ACH_BADGES_ALL, silent);
    int n, total;
    pm3ds_recipe_counts(&n, &total);
    if (n > 0) earn(ACH_COOK_ONE, silent);
    if (total > 0 && n >= total) earn(ACH_COOK_ALL, silent);
    pm3ds_tattle_count(&n, &total);
    if (n >= 30) earn(ACH_TATTLE_30, silent);
    if (n >= 100) earn(ACH_TATTLE_100, silent);
    bool chain;
    pm3ds_letter_counts(&n, &total, &chain);
    if (total > 0 && n >= total) earn(ACH_LETTERS, silent);
    if (chain) earn(ACH_CHAIN, silent);
    pm3ds_favor_counts(&n, &total);
    if (total > 0 && n >= total) earn(ACH_KOOT, silent);
    if (evt_get_variable(nullptr, GB_CompletedQuizzes) >= PM3DS_QUIZ_COUNT) earn(ACH_QUIZ, silent);
    bool merlow = true;
    for (s32 i = 0; i < 15 && merlow; i++) {
        merlow = flag(GF_HOS06_MerlowBadge_00 + i);
    }
    if (merlow) earn(ACH_MERLOW, silent);

    if (gPlayerData.level >= 27) earn(ACH_LEVEL_27, silent);
    if (gPlayerData.coins >= 999) earn(ACH_COINS, silent);
    if (gPlayerData.battlesWon >= 100) earn(ACH_WINS_100, silent);
    if (gPlayerData.hitsBlocked >= 50) earn(ACH_GUARD_50, silent);
    if (flag(GF_NOK11_Defeated_KentC)) earn(ACH_KENT, silent);
    if (flag(GF_OMO13_Defeated_AntiGuy)) earn(ACH_ANTI_GUY, silent);
    if (evt_get_variable(nullptr, GB_MAC00_DojoRank) >= 5) earn(ACH_MASTER, silent);
}

/* ---- battles ---------------------------------------------------------------- */
static const u8 sChapterBosses[] = {
    ACTOR_TYPE_GOOMBA_KING, ACTOR_TYPE_KOOPA_BROS, ACTOR_TYPE_GREEN_NINJAKOOPA, ACTOR_TYPE_RED_NINJAKOOPA,
    ACTOR_TYPE_BLACK_NINJAKOOPA, ACTOR_TYPE_YELLOW_NINJAKOOPA, ACTOR_TYPE_TUTANKOOPA, ACTOR_TYPE_TUBBA_HEART,
    ACTOR_TYPE_TUBBA_BLUBBA, ACTOR_TYPE_GENERAL_GUY, ACTOR_TYPE_TOY_TANK, ACTOR_TYPE_LAVA_PIRANHA_PHASE_1,
    ACTOR_TYPE_LAVA_PIRANHA_PHASE_2, ACTOR_TYPE_HUFF_N_PUFF, ACTOR_TYPE_CRYSTAL_KING, ACTOR_TYPE_HALLWAY_BOWSER,
    ACTOR_TYPE_FINAL_BOWSER_1, ACTOR_TYPE_FINAL_BOWSER_1_DUP, ACTOR_TYPE_FINAL_BOWSER_2, ACTOR_TYPE_FINAL_BOWSER_2_DUP,
};

static bool sInBattle, sHurt, sBoss;
static s32 sLastHP, sBadgesOn;

static void watch_battle(void) {
    if (!sInBattle) {
        sInBattle = true;
        sHurt = false;
        sBoss = false;
        sLastHP = gPlayerData.curHP;
        sBadgesOn = 0;
        for (s32 i = 0; i < ARRAY_COUNT(gPlayerData.equippedBadges); i++) {
            sBadgesOn += IS_BADGE(gPlayerData.equippedBadges[i]);
        }
    }
    if (gPlayerData.curHP < sLastHP) {
        sHurt = true;
    }
    sLastHP = gPlayerData.curHP;
    /* the field is set up past the start states; before that the list may still hold the last battle's */
    if (!sBoss && gBattleState >= BATTLE_STATE_BEGIN_TURN && gBattleState < BATTLE_STATE_VICTORY) {
        for (s32 i = 0; i < MAX_ENEMY_ACTORS && !sBoss; i++) {
            Actor* a = gBattleStatus.enemyActors[i];
            for (u32 k = 0; a != nullptr && k < ARRAY_COUNT(sChapterBosses) && !sBoss; k++) {
                sBoss = a->actorType == sChapterBosses[k];
            }
        }
    }
}

/* src/battle/btl_states_actions.c: the battle is won (the game has just counted it) */
void pm3ds_ach_battle_won(void) {
    if (!sActive || !sInBattle || gGameStatusPtr->demoState != DEMO_STATE_NONE ||
        gCurrentEncounter.battleOutcome != OUTCOME_PLAYER_WON) {
        return;
    }
    if (gPlayerData.curHP < sLastHP) {
        sHurt = true;
    }
    if (!sHurt) earn(ACH_NO_DAMAGE, false);
    if (gPlayerData.curHP <= 5) earn(ACH_DANGER, false);
    if (gPlayerData.curHP == 1) earn(ACH_PERIL, false);
    if (sBoss && sBadgesOn == 0) earn(ACH_BOSS_NO_BADGES, false);
    if (sBoss && !sHurt) earn(ACH_BOSS_FLAWLESS, false);
    /* battlesWon has just gone up */
    if (gPlayerData.battlesWon >= 100) earn(ACH_WINS_100, false);
}

/* ---- every game step (src/main_loop.c) ------------------------------------ */
void pm3ds_ach_step(void) {
    if (!sActive || gGameStatusPtr == nullptr || gGameStatusPtr->demoState != DEMO_STATE_NONE ||
        gGameStatusPtr->introPart >= 0) {
        sInBattle = false;
        return;
    }
    s32 mode = get_game_mode();
    bool inGame = mode == GAME_MODE_WORLD || mode == GAME_MODE_CHANGE_MAP || mode == GAME_MODE_ENTER_WORLD ||
                  mode == GAME_MODE_BATTLE || mode == GAME_MODE_END_BATTLE || mode == GAME_MODE_PAUSE ||
                  mode == GAME_MODE_UNPAUSE;
    if (mode == GAME_MODE_BATTLE) {
        watch_battle();
    } else if (mode != GAME_MODE_END_BATTLE) {
        sInBattle = false;
    }
    if (inGame && (sRetro || ++sTick % 30 == 0)) {
        check_state(sRetro);
        sRetro = false;
    }
#ifdef PM3DS_ACH_TEST
    {   /* debug builds only: -DPM3DS_ACH_TEST=<n> earns achievements n and n+1 ten seconds into the game (the notices) */
        static u32 t;
        if (inGame && ++t == 600) {
            earn(PM3DS_ACH_TEST, false);
            earn(PM3DS_ACH_TEST + 1, false);
        }
    }
#endif
    /* against what the side file holds, not against this step's start: the battle
     * ones are earned later in the frame (pm3ds_ach_battle_won) */
    if (sActive && (sBits[0] != sSideBits[0] || sBits[1] != sSideBits[1])) {
        side_write();
    }
}

/* ---- the save ----------------------------------------------------------------- */
/* src/fio.c, fio_serialize_state */
void pm3ds_ach_serialize(void* pad) {
    AchSaveBlock b = { ACH_MAGIC, ACH_VERSION, sFileID, gPlayerData.frameCounter, { sBits[0], sBits[1] },
                       pm3ds_letters_had(), (u32)(gGameStatusPtr->saveSlot & 3) + 1 };
    if (!sActive) {
        return;   /* nothing loaded or made (the title's demo never saves) */
    }
    memcpy(pad, &b, sizeof(b));
}

/* src/fio.c, fio_deserialize_state (gPlayerData is the file's by now) */
void pm3ds_ach_deserialize(const void* pad) {
    AchSaveBlock b;
    memcpy(&b, pad, sizeof(b));
    sBits[0] = sBits[1] = 0;
    sNoticeHead = sNoticeTail = 0;
    sInBattle = false;
    s32 slot = gGameStatusPtr->saveSlot & 3;
    if (b.magic == ACH_MAGIC && b.version == ACH_VERSION && b.fileID != 0 && b.owner == (u32)slot + 1 &&
        b.frames <= gPlayerData.frameCounter) {
        sFileID = b.fileID;
        sBits[0] = b.bits[0];
        sBits[1] = b.bits[1] & HIGH_MASK;
        pm3ds_letters_had_set(b.letters, true);
    } else {
        sFileID = legacy_file_id();   /* an older save: none earned yet, but the side file may have some */
        pm3ds_letters_had_set(0, true);
    }
    AchSideFile f;
    side_read(&f);
    if (f.slot[slot].fileID == sFileID) {
        sBits[0] |= f.slot[slot].bits[0];
        sBits[1] |= f.slot[slot].bits[1] & HIGH_MASK;
        sSideBits[0] = f.slot[slot].bits[0];
        sSideBits[1] = f.slot[slot].bits[1] & HIGH_MASK;
    } else {
        sSideBits[0] = sSideBits[1] = 0;
    }
    sActive = true;
    sRetro = true;   /* what the save proves counts at once, without notices */
}

/* src/filemenu/filemenu_yesno.c, a new file */
void pm3ds_ach_new_game(void) {
    sBits[0] = sBits[1] = 0;
    sNoticeHead = sNoticeTail = 0;
    sFileID = new_file_id();
    sSideBits[0] = sSideBits[1] = 0;
    sInBattle = false;
    sActive = true;
    sRetro = false;
    pm3ds_letters_had_set(0, false);
}

/* ---- the bottom screen ------------------------------------------------------ */
static const void* item_icon(s32 item) {
    extern IconHudScriptPair gItemHudScripts[];
    return gItemHudScripts[gItemTable[item].hudElemID].enabled;
}

int pm3ds_ach_query(pm3ds_ach_list_t* out) {
    out->count = ACH_COUNT;
    out->earned = 0;
    for (s32 a = 0; a < ACH_COUNT; a++) {
        out->a[a].name = sAch[a].name;
        out->a[a].how = sAch[a].how;
        out->a[a].icon = item_icon(sAch[a].icon);
        out->a[a].earned = sActive && earned(a);
        out->a[a].hidden = (sAch[a].flags & AF_HIDDEN) != 0;
        out->earned += out->a[a].earned;
    }
    return out->count;
}

/* the next achievement to announce, -1 none */
int pm3ds_ach_next_notice(void) {
    if (sNoticeHead == sNoticeTail) {
        return -1;
    }
    s32 a = sNotice[sNoticeHead];
    sNoticeHead = (sNoticeHead + 1) % NOTICE_MAX;
    return a;
}
