/*
 * filemenu_touch.c — the file menu by touch (Discord suggestion).
 *
 * The bottom screen draws what the menu shows and sends taps here. A tap moves the game's own
 * cursor and presses A for one step (src/filemenu/filemenu_common.c calls pm3ds_fm_apply just
 * after it read the buttons), so the menu's checks, prompts and saving stay the game's.
 */
#include "common.h"
#include "filemenu.h"
#include "game_modes.h"
#include "pm3ds_fm_touch.h"

#include <string.h>

extern SaveMetadata gSaveSlotMetadata[4];
extern u8 gSaveSlotHasData[4];

enum { TAP_NONE, TAP_MAIN, TAP_YESNO, TAP_OK, TAP_KEY, TAP_LANGUAGE };
static s32 sTap, sTapCol, sTapRow;

static void set_tap(s32 tap, s32 col, s32 row) {
    sTap = tap;
    sTapCol = col;
    sTapRow = row;
}

void pm3ds_fm_tap_main(int col, int row) { set_tap(TAP_MAIN, col, row); }
void pm3ds_fm_tap_yesno(bool yes) { set_tap(TAP_YESNO, 0, yes ? 0 : 1); }
void pm3ds_fm_tap_ok(void) { set_tap(TAP_OK, 0, 0); }
void pm3ds_fm_tap_key(int col, int row) { set_tap(TAP_KEY, col, row); }
void pm3ds_fm_tap_language(int row) { set_tap(TAP_LANGUAGE, 0, row); }

#if VERSION_PAL
extern bool LanguageSelected;   // src/state_title_screen.c
bool pm3ds_title_has_language(void) { return true; }
void pm3ds_title_pick(bool language) { LanguageSelected = language; }
#else
bool pm3ds_title_has_language(void) { return false; }
void pm3ds_title_pick(bool language) { }
#endif

bool pm3dsFileMenuUp;   /* src/filemenu/filemenu_common.c: after its init, the menus can be read */

bool pm3ds_fm_active(void) {
    if (gGameStatusPtr == nullptr || !pm3dsFileMenuUp) {
        return false;
    }
    return get_game_mode() == GAME_MODE_FILE_SELECT || get_game_mode() == GAME_MODE_LANGUAGE_SELECT;
}

/* src/filemenu/filemenu_common.c, with the buttons just read; a tap that doesn't match what the
 * menu shows now, or comes while its windows still open, is dropped */
void pm3ds_fm_apply(bool inputAllowed) {
    MenuPanel* menu = filemenu_menus[filemenu_currentMenu];
    s32 tap = sTap;

    sTap = TAP_NONE;
    if (tap == TAP_NONE || !inputAllowed || !menu->initialized) {
        return;
    }
    switch (filemenu_currentMenu) {
        case FILE_MENU_MAIN:
            if (tap != TAP_MAIN) {
                return;
            }
            menu->col = sTapCol;
            menu->row = sTapRow;
            break;
        case FILE_MENU_CONFIRM:
            if (tap != TAP_YESNO) {
                return;
            }
            menu->row = sTapRow;
            break;
        case FILE_MENU_MESSAGE:
            if (tap != TAP_OK) {
                return;
            }
            break;
        case FILE_MENU_INPUT_NAME:
            if (tap != TAP_KEY) {
                return;
            }
            menu->col = sTapCol;
            menu->row = sTapRow;
            break;
#if VERSION_PAL
        case FILE_MENU_SELECT_LANG:
            if (tap != TAP_LANGUAGE) {
                return;
            }
            menu->row = sTapRow;
            break;
#endif
        default:
            return;
    }
    // the handlers work out menu->selected from col and row themselves, and act on it
    filemenu_pressedButtons = BUTTON_A;
}

/* the name characters a game font and the bottom screen's share: the rest is left to the buttons */
static char ascii_of(u8 c) {
    if (c == MSG_CHAR_READ_SPACE) {
        return ' ';
    }
    if (c >= MSG_CHAR_DIGIT_0 && c <= MSG_CHAR_DIGIT_0 + 9) {
        return '0' + (c - MSG_CHAR_DIGIT_0);
    }
    if (c >= MSG_CHAR_UPPER_A && c < MSG_CHAR_UPPER_A + 26) {
        return 'A' + (c - MSG_CHAR_UPPER_A);
    }
    if (c >= MSG_CHAR_LOWER_A && c < MSG_CHAR_LOWER_A + 26) {
        return 'a' + (c - MSG_CHAR_LOWER_A);
    }
    if (c == 0x0E) {
        return '.';
    }
    if (c == 0x0D) {
        return '-';
    }
    return 0;
}

static void name_of(char out[9], const u8* name) {
    s32 last = -1;
    s32 i;

    for (i = 0; i < 8; i++) {
        char a = ascii_of(name[i]);
        out[i] = a != 0 ? a : '?';
        if (out[i] != ' ') {
            last = i;
        }
    }
    out[last + 1] = 0;
}

void pm3ds_fm_query(pm3ds_fm_info_t* q) {
    MenuPanel* main = filemenu_menus[FILE_MENU_MAIN];
    MenuPanel* input = filemenu_menus[FILE_MENU_INPUT_NAME];
    s32 i;

    q->menu = PM3DS_FM_MAIN;
    q->state = PM3DS_FMS_FILE;
    q->selected = main->selected <= FM_MAIN_OPT_FILE_4 ? main->selected : -1;
    switch (main->state) {
        case FM_MAIN_SELECT_DELETE:    q->state = PM3DS_FMS_DELETE; break;
        case FM_MAIN_SELECT_COPY_FROM: q->state = PM3DS_FMS_COPY_FROM; break;
        case FM_MAIN_SELECT_COPY_TO:   q->state = PM3DS_FMS_COPY_TO; break;
    }
    switch (filemenu_currentMenu) {
        case FILE_MENU_CONFIRM:
            q->menu = PM3DS_FM_CONFIRM;
            switch (filemenu_menus[FILE_MENU_CONFIRM]->state) {
                case FM_CONFIRM_DELETE: q->state = PM3DS_FMC_DELETE; break;
                case FM_CONFIRM_CREATE: q->state = PM3DS_FMC_CREATE; break;
                case FM_CONFIRM_START:  q->state = PM3DS_FMC_START; break;
                default:                q->state = PM3DS_FMC_OVERWRITE; break;
            }
            break;
        case FILE_MENU_MESSAGE:
            q->menu = PM3DS_FM_MESSAGE;
            switch (filemenu_menus[FILE_MENU_MESSAGE]->state) {
                case FM_MESSAGE_DELETED: q->state = PM3DS_FMM_DELETED; break;
                case FM_MESSAGE_COPIED:  q->state = PM3DS_FMM_COPIED; break;
                default:                 q->state = PM3DS_FMM_CREATED; break;
            }
            break;
        case FILE_MENU_INPUT_NAME:
            q->menu = PM3DS_FM_INPUT;
            break;
#if VERSION_PAL
        case FILE_MENU_SELECT_LANG:
            q->menu = PM3DS_FM_LANGUAGE;
            q->selected = filemenu_menus[FILE_MENU_SELECT_LANG]->row;
            break;
#endif
    }
    for (i = 0; i < 4; i++) {
        q->has[i] = gSaveSlotHasData[i];
        q->level[i] = gSaveSlotMetadata[i].level;
        q->spirits[i] = gSaveSlotMetadata[i].spiritsRescued;
        name_of(q->name[i], gSaveSlotMetadata[i].filename);
    }
    name_of(q->typed, filemenu_filename);
    q->cols = input->numCols;
    q->rows = input->numRows;
}

int pm3ds_fm_key(int col, int row, char label[8]) {
    MenuPanel* menu = filemenu_menus[FILE_MENU_INPUT_NAME];
    u8 c = menu->gridData[(menu->state * menu->numCols * menu->numRows) + (menu->numCols * row) + col];
    const char* wide = nullptr;
    char a;

    switch (c) {
        case MSG_CHAR_MENU_SPACE: wide = "Space"; break;
        case MSG_CHAR_MENU_BACK:  wide = "Back"; break;
        case MSG_CHAR_MENU_END:   wide = "END"; break;
    }
    if (wide != nullptr) {
        strcpy(label, wide);
        return PM3DS_FM_CELL_WIDE;
    }
    a = c == MSG_CHAR_READ_SPACE ? 0 : ascii_of(c);
    if (a == 0) {
        return 0;
    }
    label[0] = a;
    label[1] = 0;
    return PM3DS_FM_CELL_LETTER;
}
