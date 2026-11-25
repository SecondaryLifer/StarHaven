/*
 * gfx_interp.c — 60 fps by frame interpolation.
 *
 * The game steps at 30 Hz (its logic is tuned for it). After each step the
 * renderer draws the new display lists twice: first with every matrix half way
 * between this step and the previous one, then as they are. Only matrices move
 * (camera, Mario, NPCs, models, effects); 2D (HUD, text, backgrounds drawn as
 * rectangles) stays at 30 Hz.
 *
 * Matching a matrix with its counterpart one step earlier: the game's
 * gSPMatrix is wrapped (PR/gbi.h) to note, for each G_MTX command it writes,
 * the call site and the object being drawn (pm3ds_interp_obj, set by the draw
 * loops that have one). The key is (site, object, how many matrices that pair
 * emitted before in this walk, projection or not). Pairs that changed too much
 * between steps (a camera cut, a teleport, a big spin) are drawn as they are.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pm3ds_log.h"
#include "gfx_interp.h"

/* ---- game side: which call site wrote each G_MTX ------------------------- */
#define SITE_SLOTS 8192   /* power of two */
typedef struct { const void *gfx; uint32_t site; const void *obj; uint32_t tick; } site_t;
static site_t s_sites[SITE_SLOTS];
static uint32_t s_tick = 1;
const void *pm3ds_interp_obj;

static inline uint32_t hash_ptr(const void *p) { return ((uint32_t)(uintptr_t)p >> 3) * 2654435761u; }

void pm3ds_mtx_site(const void *gfx, const char *file, int line) {
    uint32_t h = hash_ptr(gfx);
    for (int probe = 0; probe < 16; probe++) {
        site_t *s = &s_sites[(h + probe) & (SITE_SLOTS - 1)];
        if (s->tick != s_tick || s->gfx == gfx) {
            s->gfx = gfx;
            s->site = (uint32_t)(uintptr_t)file * 2654435761u ^ (uint32_t)line * 40503u;
            s->obj = pm3ds_interp_obj;
            s->tick = s_tick;
            return;
        }
    }
}

static const site_t *site_of(const void *gfx) {
    uint32_t h = hash_ptr(gfx);
    for (int probe = 0; probe < 16; probe++) {
        const site_t *s = &s_sites[(h + probe) & (SITE_SLOTS - 1)];
        if (s->tick != s_tick) return NULL;
        if (s->gfx == gfx) return s;
    }
    return NULL;
}

/* sprite components (src/sprite.c spr_appendGfx_component): M = local x parent.
 * local = the sprite animation's own offset/rotation/scale for the component,
 * which snaps together with its raster on N64; blending it drew the new raster
 * at a half-way offset (Bzzap's wing frames are wider and re-centred by +8:
 * GitHub #53). Only the parent (facing yaw x the NPC/actor/player) is blended. */
#define LOCAL_SLOTS 1024   /* power of two */
typedef struct { const void *gfx; uint32_t tick; float l[4][4], p[4][4]; } local_t;
static local_t s_local[LOCAL_SLOTS];

void pm3ds_mtx_local(const void *gfx, float l[4][4], float p[4][4]) {
    uint32_t h = hash_ptr(gfx);
    for (int probe = 0; probe < 16; probe++) {
        local_t *s = &s_local[(h + probe) & (LOCAL_SLOTS - 1)];
        if (s->tick != s_tick || s->gfx == gfx) {
            s->gfx = gfx;
            s->tick = s_tick;
            memcpy(s->l, l, sizeof s->l);
            memcpy(s->p, p, sizeof s->p);
            return;
        }
    }
}

static const local_t *local_of(const void *gfx) {
    uint32_t h = hash_ptr(gfx);
    for (int probe = 0; probe < 16; probe++) {
        const local_t *s = &s_local[(h + probe) & (LOCAL_SLOTS - 1)];
        if (s->tick != s_tick) return NULL;
        if (s->gfx == gfx) return s;
    }
    return NULL;
}

/* ---- render side ----------------------------------------------------------- */
#define OCC_SLOTS 4096
typedef struct { uint32_t k, n, walk; } occ_t;
static occ_t s_occ[OCC_SLOTS];
static uint32_t s_walk = 1;

#define MTX_SLOTS 4096
typedef struct { uint32_t key, tick; float m[4][4]; } saved_t;
static saved_t s_saved[2][MTX_SLOTS];
static int s_cur;                 /* s_saved[s_cur] fills this step, the other holds the last one */
static uint32_t s_cur_tick = 1;   /* tick tag of the entries in s_saved[s_cur] */

static float s_t = 1.0f;
static bool s_record;
static uint32_t s_n_mtx, s_n_site, s_n_lerp, s_n_reject, s_n_bg;
static float s_bg_sum;

bool pm3ds_fps60 = true;

/* the sky backdrop's scroll (src/background.c), per step: the whole value, and
 * the parts from the camera's sideways move and its turn (degrees) */
typedef struct { uint32_t tick; float scroll, left, width, move, turn; } bg_note_t;
static bg_note_t s_bg_cur, s_bg_prev;

void pm3ds_bg_scroll_note(float scroll, int left, int width, float move, float turn) {
    s_bg_cur = (bg_note_t){ s_tick, scroll, (float)left, (float)width, move, turn };
}

bool gfx_interp_bg_edges(float *left, float *right) {
    if (s_bg_cur.tick != s_tick || s_bg_cur.width <= 0.0f) return false;
    *left = s_bg_cur.left;
    *right = s_bg_cur.left + s_bg_cur.width;
    return true;
}

/* Where the backdrop is drawn, in N64 pixels to the right of where the game
 * put it. The game draws it at the whole pixel of its scroll value, once a
 * step: slow pans moved it a pixel every few steps while the world glided at
 * 60 fps. Here it goes to the exact value, and on an in-between frame half way
 * back to the last step's. A cut (the camera jumping or snapping round) keeps
 * the step's own position. */
float gfx_interp_bg_shift(float *left, float *right) {
    if (s_bg_cur.tick != s_tick || s_bg_cur.width <= 0.0f) return 0.0f;
    float w = s_bg_cur.width;
    float shift = s_bg_cur.scroll - floorf(s_bg_cur.scroll);   /* >= 0: the game wraps it */
#ifdef PM3DS_BG_OLD   /* debug A/B: the backdrop at whole pixels, as 1.3.15 drew it */
    shift = 0.0f;
#endif
    if (s_t < 1.0f && s_bg_prev.tick == s_tick - 1 && s_bg_prev.width == w) {
        float d = s_bg_cur.scroll - s_bg_prev.scroll;
        while (d > w / 2) d -= w;            /* the shorter way round the wrap */
        while (d < -w / 2) d += w;
        float turn = s_bg_cur.turn - s_bg_prev.turn;
        while (turn > 180.0f) turn -= 360.0f;
        while (turn < -180.0f) turn += 360.0f;
        float move = s_bg_cur.move - s_bg_prev.move;
        if (move <= 48.0f && move >= -48.0f && turn <= 45.0f && turn >= -45.0f) {
            shift += (s_t - 1.0f) * d;       /* where it was half a step ago */
            if (d != 0.0f) {
                s_n_bg++;
                s_bg_sum += fabsf(d);
            }
        }
    }
    *left = s_bg_cur.left;
    *right = s_bg_cur.left + w;
    return shift;
}

/* the walk being drawn is a 60 fps in-between frame */
bool gfx_interp_in_between(void) { return s_t < 1.0f; }

void gfx_interp_new_tick(void) {
    s_tick++;
    if (s_bg_cur.tick == s_tick - 1) s_bg_prev = s_bg_cur;
}

void gfx_interp_begin_walk(float t, bool record) {
    s_t = t;
    s_record = record;
    s_walk++;
}

void gfx_interp_second_eye(void) {
    s_record = false;
    s_walk++;
}

void gfx_interp_end_step(void) {
    static uint32_t steps;
    if (++steps % 300 == 0)
        pm3ds_log(PM3DS_LOG_INFO, "INTERP mtx=%lu with site=%lu lerped=%lu rejected=%lu bg=%lu avg %.2fpx (last 300 steps)",
                  (unsigned long)s_n_mtx, (unsigned long)s_n_site, (unsigned long)s_n_lerp,
                  (unsigned long)s_n_reject, (unsigned long)s_n_bg, s_n_bg ? s_bg_sum / s_n_bg : 0.0);
    if (steps % 300 == 0) { s_n_mtx = s_n_site = s_n_lerp = s_n_reject = s_n_bg = 0; s_bg_sum = 0; }
    s_cur ^= 1;
    s_cur_tick++;
}

static uint32_t next_occurrence(uint32_t k) {
    uint32_t h = k * 2654435761u;
    for (int probe = 0; probe < 32; probe++) {
        occ_t *o = &s_occ[(h + probe) & (OCC_SLOTS - 1)];
        if (o->walk != s_walk) {
            o->k = k;
            o->n = 0;
            o->walk = s_walk;
            return 0;
        }
        if (o->k == k) return ++o->n;
    }
    return 0xFFFFFFFFu;
}

static saved_t *slot(int which, uint32_t key, uint32_t tick, bool insert) {
    uint32_t h = key * 2654435761u;
    for (int probe = 0; probe < 32; probe++) {
        saved_t *s = &s_saved[which][(h + probe) & (MTX_SLOTS - 1)];
        if (s->tick != tick) return insert ? s : NULL;
        if (s->key == key) return s;
    }
    return NULL;
}

/* translation row and the 3x3 part: is prev -> cur a plausible half step? */
static bool similar(const float a[4][4], const float b[4][4], bool proj) {
    float d2 = 0.0f, n2 = 0.0f;
    if (proj) {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float d = a[i][j] - b[i][j];
                d2 += d * d;
                n2 += b[i][j] * b[i][j];
            }
        return d2 <= 0.04f * n2;     /* a camera cut changes far more than 20% */
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float d = a[i][j] - b[i][j];
            d2 += d * d;
            n2 += b[i][j] * b[i][j];
        }
    if (d2 > 0.25f * n2) return false;   /* about 35 degrees or a big scale jump */
    float dx = a[3][0] - b[3][0], dy = a[3][1] - b[3][1], dz = a[3][2] - b[3][2];
    return dx * dx + dy * dy + dz * dz <= 48.0f * 48.0f;
}

void gfx_interp_matrix(const void *gfx, bool proj, float m[4][4]) {
    if (!s_record && s_t >= 1.0f) return;
    s_n_mtx += s_record;
    const site_t *s = site_of(gfx);
    if (!s) return;
    s_n_site += s_record;
    /* a sprite component: save and blend its parent, then put its own part back
     * on top. A copy: the second eye walks the same note again. */
    const local_t *lo = proj ? NULL : local_of(gfx);
    float pt[4][4];
    float (*v)[4] = m;
    if (lo) {
        memcpy(pt, lo->p, sizeof pt);
        v = pt;
    }
    uint32_t k = s->site ^ hash_ptr(s->obj) ^ (proj ? 0x9E3779B9u : 0);
    uint32_t occ = next_occurrence(k);
    if (occ == 0xFFFFFFFFu) return;
    uint32_t key = k ^ (occ * 0x85EBCA6Bu);
    if (s_record) {
        saved_t *e = slot(s_cur, key, s_cur_tick, true);
        if (e) {
            e->key = key;
            e->tick = s_cur_tick;
            memcpy(e->m, v, sizeof e->m);
        }
    }
    if (s_t >= 1.0f) return;
    const saved_t *p = slot(s_cur ^ 1, key, s_cur_tick - 1, false);
    if (!p) return;
    if (!similar(p->m, v, proj)) {
        s_n_reject++;
        return;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) v[i][j] = p->m[i][j] + (v[i][j] - p->m[i][j]) * s_t;
    if (lo)   /* row vectors, as guMtxCatF: M = local x parent */
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                m[i][j] = lo->l[i][0] * pt[0][j] + lo->l[i][1] * pt[1][j] + lo->l[i][2] * pt[2][j] + lo->l[i][3] * pt[3][j];
    s_n_lerp++;
}
