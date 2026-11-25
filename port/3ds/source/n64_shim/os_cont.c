/*
 * os_cont.c — N64 controller shim, backed by libctru HID.
 * Fidelity tier: FAITHFUL.
 *
 * Button map (N64 -> 3DS): the player's keymap (platform/pm3ds_settings.c,
 * remapped on the bottom screen's Options page). The default, chosen so an
 * old 3DS (no ZL/ZR/C-stick) still reaches every input Paper Mario uses:
 *   A     -> A            Start -> Start         Stick  -> Circle Pad
 *   B     -> B            Z     -> L             R      -> R
 *   C-Down  (partner ability) -> X or D-Pad Down
 *   C-Left  (partner menu)    -> Y or D-Pad Left
 *   C-Right (item menu)       -> D-Pad Right
 *   C-Up    (camera/tattle)   -> D-Pad Up
 *   N64 L                     -> Select
 * New 3DS extras: ZL = C-Left, ZR = fast-forward (held), C-stick = C buttons.
 * (N64 D-pad is unused by the game, so it is free to carry the C buttons.)
 */

#include <stdint.h>
#include <string.h>
#ifndef PM3DS_HOST_TEST
#include <3ds.h>
#include <math.h>
#include "pm3ds_settings.h"
#endif

#define CONT_A      0x8000
#define CONT_B      0x4000
#define CONT_Z      0x2000
#define CONT_START  0x1000
#define CONT_UP     0x0800
#define CONT_DOWN   0x0400
#define CONT_LEFT   0x0200
#define CONT_RIGHT  0x0100
#define CONT_L      0x0020
#define CONT_R      0x0010
#define CONT_E      0x0008  /* C-Up    */
#define CONT_D      0x0004  /* C-Down  */
#define CONT_C      0x0002  /* C-Left  */
#define CONT_F      0x0001  /* C-Right */

typedef struct {
    uint16_t button;
    int8_t   stick_x;
    int8_t   stick_y;
    uint8_t  errno_;
} OSContPad;

static OSContPad g_pad = {0};

int osContInit(void *mq, uint8_t *bitpattern, void *status) {
    (void)mq; (void)status;
    if (bitpattern) *bitpattern = 0x01;
#ifndef PM3DS_HOST_TEST
    hidInit();
#endif
    return 0;
}

uint16_t pm3ds_debug_pad_or;
int8_t pm3ds_debug_stick_x, pm3ds_debug_stick_y;   /* debug: steer Mario (the showcase tour) */

/* this read's buttons split by where they came from: the console's buttons, and
 * the bottom screen's pad (one game step late already, it is read once a frame)
 * plus debug ones. src/input.c delays the first by a step in battle. */
uint16_t pm3ds_pad_phys, pm3ds_pad_other;

void osContStartReadData(void *mq) {
    (void)mq;
#ifndef PM3DS_HOST_TEST
    hidScanInput();
    u32 kHeld = hidKeysHeld();
    circlePosition cp;
    hidCircleRead(&cp);

    static const uint16_t act_bits[ACT_COUNT] = {
        [ACT_A] = CONT_A, [ACT_B] = CONT_B, [ACT_Z] = CONT_Z, [ACT_R] = CONT_R, [ACT_L] = CONT_L,
        [ACT_START] = CONT_START, [ACT_C_UP] = CONT_E, [ACT_C_DOWN] = CONT_D, [ACT_C_LEFT] = CONT_C,
        [ACT_C_RIGHT] = CONT_F,
    };
    uint16_t b = 0;
    bool turbo = false;
    /* "D-pad in menus" (Discord suggestion): in an open menu the D-pad moves the cursor
     * like the stick does, instead of the C buttons it carries by default */
    extern bool pm3ds_menu_open(void);
    u32 dpad = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT;
    bool dpad_stick = pm3ds_cfg.dpad_menus && (kHeld & dpad) && pm3ds_menu_open();
    for (int k = 0; k < KEY_IDX_COUNT; k++) {
        if (!(kHeld & pm3ds_settings_key_bit(k))) continue;
        if (dpad_stick && (pm3ds_settings_key_bit(k) & dpad)) continue;
        /* Select + R takes a screenshot (platform/pm3ds_screenshot.c): R held with
         * Select would scroll a menu or swap Mario's and the partner's turns */
        if (k == KEY_IDX_R && (kHeld & KEY_SELECT)) continue;
        b |= act_bits[pm3ds_cfg.keymap[k] % ACT_COUNT];
        turbo |= pm3ds_cfg.keymap[k] == ACT_TURBO_A;
    }
    {   /* Turbo A: about 15 fresh A presses a second while held, for mashing (running
         * away, mash action commands) without hurting your hand. By the clock, not per
         * read: the controller is read more than once per game step. A itself stays a
         * normal button: some action commands need it held */
        if (turbo && (osGetTime() / 34) % 2 == 0) b |= CONT_A;
    }

    /* Circle pad on 3DS reports −0x9C ... +0x9C-ish; N64 stick is ±80. A
     * resting pad sits a few units off centre (an N64 stick reads 0 there),
     * which walked Mario slowly: a round dead zone, then the rest of the
     * range stretched so the edge still gives full speed. */
    int8_t sx = 0, sy = 0;
    {
        const float dead = 14.0f, full = 150.0f;
        float m = sqrtf((float)cp.dx * cp.dx + (float)cp.dy * cp.dy);
        if (m > dead) {
            float k = (m >= full ? 80.0f : 80.0f * (m - dead) / (full - dead)) / m;
            sx = (int8_t)(cp.dx * k);
            sy = (int8_t)(cp.dy * k);
        }
    }

    if (dpad_stick) {
        if (kHeld & KEY_DUP) sy = 80;
        if (kHeld & KEY_DDOWN) sy = -80;
        if (kHeld & KEY_DLEFT) sx = -80;
        if (kHeld & KEY_DRIGHT) sx = 80;
    }

    uint16_t phys = b;   /* the console's own buttons, before the touch and debug ones join */
    {   /* debug sweeps hold buttons here (always 0 otherwise) */
        extern uint16_t pm3ds_debug_pad_or;
        b |= pm3ds_debug_pad_or;
        if (pm3ds_debug_stick_x || pm3ds_debug_stick_y) {
            sx = pm3ds_debug_stick_x;
            sy = pm3ds_debug_stick_y;
        }
    }
    {   /* the bottom screen's battle pad (platform/bottom_screen.c) */
        extern uint16_t pm3ds_touch_pad;
        extern int8_t pm3ds_touch_stick_x;
        b |= pm3ds_touch_pad;
        if (pm3ds_touch_stick_x) sx = pm3ds_touch_stick_x;
    }
    {   /* the new-game picker has the buttons (platform/bottom_screen.c) */
        extern bool pm3ds_new_game_picker_blocks_input(void);
        if (pm3ds_new_game_picker_blocks_input()) { b = 0; phys = 0; sx = 0; sy = 0; }
    }
    pm3ds_pad_phys = phys;
    pm3ds_pad_other = b & ~phys;
    g_pad.button  = b;
    g_pad.stick_x = sx;
    g_pad.stick_y = sy;
    g_pad.errno_  = 0;
#endif
}

void osContGetReadData(OSContPad *pads) {
    if (!pads) return;
    pads[0] = g_pad;
    /* zero out ports 1..3 */
    for (int i = 1; i < 4; i++) memset(&pads[i], 0, sizeof(pads[i]));
}

int  osContStartQuery(void *mq) { (void)mq; return 0; }
void osContGetQuery (void *status) { (void)status; }

int  osMotorInit (void *mq, void *m, int ch) { (void)mq; (void)m; (void)ch; return 0; }
int  osMotorStart(void *m) { (void)m; return 0; }
int  osMotorStop (void *m) { (void)m; return 0; }

/* M13: the nusys entry points the game actually calls (src/main.c,
 * src/input.c). Both were empty weak stubs, so contBitPattern was garbage
 * and the game never saw a button press. */
uint8_t nuContInit(void) {
    uint8_t pattern = 0;
    osContInit(NULL, &pattern, NULL);
    return pattern;
}

void nuContDataGet(OSContPad *contdata, uint32_t padno) {
    if (!contdata) return;
    osContStartReadData(NULL);
    *contdata = padno == 0 ? g_pad : (OSContPad){ 0, 0, 0, 0 };
}
