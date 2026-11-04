#ifndef PM3DS_FM_TOUCH_H
#define PM3DS_FM_TOUCH_H
/* The file menu on the touch screen (Discord suggestion): n64_shim/filemenu_touch.c reads the
 * game's menu for platform/bottom_screen.c and turns taps into its cursor and A press.
 * No decomp headers here. */
#include <stdbool.h>

enum { PM3DS_FM_MAIN, PM3DS_FM_CONFIRM, PM3DS_FM_MESSAGE, PM3DS_FM_INPUT, PM3DS_FM_LANGUAGE };
enum { PM3DS_FMS_FILE, PM3DS_FMS_DELETE, PM3DS_FMS_COPY_FROM, PM3DS_FMS_COPY_TO };
enum { PM3DS_FMC_START, PM3DS_FMC_DELETE, PM3DS_FMC_CREATE, PM3DS_FMC_OVERWRITE };
enum { PM3DS_FMM_DELETED, PM3DS_FMM_COPIED, PM3DS_FMM_CREATED };
enum { PM3DS_FM_CELL_LETTER = 1, PM3DS_FM_CELL_WIDE };   /* what a keyboard cell is */

typedef struct {
    int menu;        /* PM3DS_FM_* */
    int state;       /* PM3DS_FMS_*, _FMC_* or _FMM_* by menu */
    int selected;    /* the file the menu's cursor is on, or -1 */
    bool has[4];
    char name[4][9];
    int level[4];
    int spirits[4];
    char typed[9];   /* the name being entered */
    int cols, rows;  /* the keyboard's grid */
    int language;
} pm3ds_fm_info_t;

bool pm3ds_title_has_language(void);     /* the EU ROM's title has a language choice under Start */
void pm3ds_title_pick(bool language);    /* what START does there */
bool pm3ds_fm_active(void);
void pm3ds_fm_query(pm3ds_fm_info_t *q);
int pm3ds_fm_key(int col, int row, char label[8]);   /* PM3DS_FM_CELL_*, or 0: not on the page */

/* taps, done at the menu's next step */
void pm3ds_fm_tap_main(int col, int row);
void pm3ds_fm_tap_yesno(bool yes);
void pm3ds_fm_tap_ok(void);
void pm3ds_fm_tap_key(int col, int row);
void pm3ds_fm_tap_language(int row);

#endif
