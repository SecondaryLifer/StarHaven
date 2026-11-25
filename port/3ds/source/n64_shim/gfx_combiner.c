/*
 * gfx_combiner.c — N64 color combiner -> PICA200 TexEnv (M12 rewrite).
 *
 * Each RDP cycle computes (a - b) * c + d separately for RGB and alpha.
 * Each channel formula is compiled to a short list of TexEnv micro-ops
 * (REPLACE / MODULATE / ADD / SUBTRACT / INTERPOLATE / MULTIPLY_ADD); the
 * RGB and alpha lists are then zipped into shared stages. A stage has one
 * constant colour, so prim/env/1 references are folded into it, with the
 * RGB part and the alpha part chosen independently.
 *
 * Exact for every shape where b == 0, d == b, a == b or c == 0 (which is
 * nearly every combine mode the game uses), and for two constants a and b
 * (folded per channel, the negative part subtracted); a pure -(b*c)
 * reproduces the RDP's 9-bit wrap. Otherwise (a-b)*c+d falls back to
 * SUBTRACT then MULTIPLY_ADD, which clamps a negative (a-b) to 0.
 *
 * Two constants with a negative part in the second cycle can also wrap:
 * d - n*COMBINED below -128 comes out as 255 (the fire-hit ring's white-hot
 * core). wrap_cycle() parks the unwrapped part in the combiner buffer and
 * derives the wrapped part from COMBINED carried in the alpha lane.
 *
 * Second cycle: COMBINED reads GPU_PREVIOUS in the first second-cycle
 * stage and GPU_PREVIOUS_BUFFER after that (the buffer is updated by the
 * last first-cycle stage). TEXEL0/TEXEL1 swap in the second cycle, as on
 * the RDP.
 */

#include <stdint.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gfx_state.h"

typedef enum {
    S_ZERO, S_ONE, S_COMB, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_PRIM_LODF, S_HALF, S_PREV,
    S_KEYC, S_KEYS, S_K4, S_K5,      /* chroma key CENTER/SCALE, convert K4/K5 */
    S_KC0, S_KC1, S_KC2, S_KC3,      /* constants folded at build time (g_kc) */
    S_NOISE,                         /* per-pixel random: texture unit 1 */
    S_BUF,                           /* the combiner buffer as a stage left it */
    S_LODF,                          /* LOD_FRACTION: decode_cycle turns it into shade alpha or 0 */
} src_t;

typedef struct { uint8_t s; uint8_t alpha; } in_t;      /* alpha: read the source's alpha (2/3/4: its R/G/B, alpha lane only) */

typedef struct {
    uint8_t func;        /* GPU_COMBINEFUNC */
    uint8_t n;           /* operand count */
    in_t    src[3];
    uint8_t inv[3];      /* ONE_MINUS */
    uint8_t scale;       /* GPU_TEVSCALE */
} uop_t;

#define MAX_UOPS 4

static const in_t Z = { S_ZERO, 0 };

/* LOD_FRACTION reads 0: with texture LOD on, the RDP blends a texture's
 * levels (tiles) by how many texels land on a pixel; the backend binds those
 * levels as one mipmapped texture on unit 0 instead (gfx_texture.c
 * gfx_tex_get_mip), so the GPU does that blend and TEXEL0 is all a
 * "lerp(TEXEL0, TEXEL1, LOD_FRACTION)" needs. Star Haven's rainbow floor. */
bool gfx_combiner_uses_lod(const gfx_state_t *st) {
    if (!(st->othermode_h & 0x10000u)) return false;   /* G_TL_LOD */
    uint32_t w0 = st->combine_w0, w1 = st->combine_w1;
    return ((w0 >> 15) & 0x1F) == 13 || (w0 & 0x1F) == 13 ||   /* RGB c: LOD_FRACTION */
           ((w0 >> 9) & 7) == 0 || ((w1 >> 18) & 7) == 0;     /* alpha c: LOD_FRACTION */
}

/* NOISE samples a random texture on unit 1, so a combine mode that also
 * reads TEXEL1 gets a flat 0.5 for it instead (set by gfx_combiner_apply) */
static bool s_noise_flat;

static in_t rgb_a(uint32_t v) {
    static const uint8_t m[8] = { S_COMB, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_ONE, S_NOISE };
    if (v == 7 && s_noise_flat) return (in_t){ S_HALF, 0 };
    return v < 8 ? (in_t){ m[v], 0 } : Z;
}
static in_t rgb_b(uint32_t v) {
    static const uint8_t m[8] = { S_COMB, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_KEYC, S_K4 };
    return v < 8 ? (in_t){ m[v], 0 } : Z;
}
static in_t rgb_c(uint32_t v) {
    switch (v) {
        case 0: return (in_t){ S_COMB, 0 };  case 1: return (in_t){ S_T0, 0 };
        case 2: return (in_t){ S_T1, 0 };    case 3: return (in_t){ S_PRIM, 0 };
        case 4: return (in_t){ S_SHADE, 0 }; case 5: return (in_t){ S_ENV, 0 };
        case 7: return (in_t){ S_COMB, 1 };  case 8: return (in_t){ S_T0, 1 };
        case 9: return (in_t){ S_T1, 1 };    case 10: return (in_t){ S_PRIM, 1 };
        case 11: return (in_t){ S_SHADE, 1 }; case 12: return (in_t){ S_ENV, 1 };
        case 6: return (in_t){ S_KEYS, 0 };
        case 14: return (in_t){ S_PRIM_LODF, 0 };
        case 15: return (in_t){ S_K5, 0 };
        case 13: return (in_t){ S_LODF, 0 };
        default: return Z;
    }
}
static in_t rgb_d(uint32_t v) {
    static const uint8_t m[7] = { S_COMB, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_ONE };
    return v < 7 ? (in_t){ m[v], 0 } : Z;
}
static in_t a_abd(uint32_t v) {
    static const uint8_t m[7] = { S_COMB, S_T0, S_T1, S_PRIM, S_SHADE, S_ENV, S_ONE };
    return v < 7 ? (in_t){ m[v], 1 } : Z;
}
static in_t a_c(uint32_t v) {
    switch (v) {
        case 1: return (in_t){ S_T0, 1 };    case 2: return (in_t){ S_T1, 1 };
        case 3: return (in_t){ S_PRIM, 1 };  case 4: return (in_t){ S_SHADE, 1 };
        case 5: return (in_t){ S_ENV, 1 };   case 6: return (in_t){ S_PRIM_LODF, 1 };
        case 0: return (in_t){ S_LODF, 1 };
        default: return Z;
    }
}

static int is(in_t x, int s) { return x.s == s; }
static int eq(in_t x, in_t y) { return x.s == y.s && (x.alpha == y.alpha || x.s == S_ZERO || x.s == S_ONE); }

static uop_t U1(int f, in_t a) { uop_t u = { (uint8_t)f, 1, { a, Z, Z }, { 0, 0, 0 }, 0 }; return u; }
static uop_t U2(int f, in_t a, in_t b) { uop_t u = { (uint8_t)f, 2, { a, b, Z }, { 0, 0, 0 }, 0 }; return u; }
static uop_t U3(int f, in_t a, in_t b, in_t c) { uop_t u = { (uint8_t)f, 3, { a, b, c }, { 0, 0, 0 }, 0 }; return u; }

static int is_const(in_t x) {
    return x.s == S_PRIM || x.s == S_ENV || x.s == S_ONE || x.s == S_ZERO || x.s == S_PRIM_LODF || x.s == S_HALF ||
           (x.s >= S_KEYC && x.s <= S_KC3);
}

/* constants folded while compiling the current combine mode */
static uint8_t g_kc[4][4];
static int     g_nkc;

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void const_value(const gfx_state_t *st, in_t x, uint8_t out[4]) {
    uint8_t v[4];
    switch (x.s) {
        case S_PRIM: memcpy(v, st->prim, 4); break;
        case S_ENV:  memcpy(v, st->env, 4); break;
        case S_ONE:  v[0] = v[1] = v[2] = v[3] = 255; break;
        case S_PRIM_LODF: v[0] = v[1] = v[2] = v[3] = st->prim_lod_frac; break;
        case S_HALF: v[0] = v[1] = v[2] = v[3] = 128; break;
        case S_KEYC: memcpy(v, st->key_center, 3); v[3] = 0; break;
        case S_KEYS: memcpy(v, st->key_scale, 3); v[3] = 0; break;
        case S_K4:   v[0] = v[1] = v[2] = v[3] = clamp8(st->k4); break;
        case S_K5:   v[0] = v[1] = v[2] = v[3] = clamp8(st->k5); break;
        case S_KC0: case S_KC1: case S_KC2: case S_KC3: memcpy(v, g_kc[x.s - S_KC0], 4); break;
        default:     v[0] = v[1] = v[2] = v[3] = 0; break;
    }
    if (x.alpha) { out[0] = out[1] = out[2] = out[3] = v[3]; }
    else memcpy(out, v, 4);
}

static in_t fold_const(const uint8_t v[4], int alpha) {
    if (g_nkc >= 4) return Z;
    memcpy(g_kc[g_nkc], v, 4);
    return (in_t){ (uint8_t)(S_KC0 + g_nkc++), (uint8_t)alpha };
}

/* -(x * c): the RDP keeps a 9-bit result, so it is 0 down to -128 and wraps
 * to 255 below that. MODULATE then a steep ramp around one half. */
static int negated_product(in_t x, in_t c, uop_t *o) {
    const in_t PREV = { S_PREV, 0 }, HALF = { S_HALF, 0 };
    o[0] = U2(GPU_MODULATE, x, c);
    o[1] = U2(GPU_SUBTRACT, PREV, HALF); o[1].scale = GPU_TEVSCALE_4;
    o[2] = U1(GPU_REPLACE, PREV); o[2].scale = GPU_TEVSCALE_4;
    return 3;
}

/* (a - b) * c + d */
static int compile(const gfx_state_t *st, int alpha, in_t a, in_t b, in_t c, in_t d, uop_t *o) {
    const in_t PREV = { S_PREV, 0 };
    if (is(c, S_ZERO) || eq(a, b)) { o[0] = U1(GPU_REPLACE, d); return 1; }
    if (is(a, S_ZERO) && is(d, S_ZERO) && !is(b, S_ZERO)) return negated_product(b, c, o);
    if (is_const(a) && is_const(b) && !is(a, S_ZERO) && !is(b, S_ZERO) && !eq(d, b)) {
        /* two constants (e.g. ENV - key CENTER): fold a - b per channel.
         * The TexEnv cannot hold a negative, so the part below zero is
         * subtracted from d separately: d + pos*c - neg*c */
        uint8_t va[4], vb[4], pos[4], neg[4];
        int any_pos = 0, any_neg = 0;
        const_value(st, a, va);
        const_value(st, b, vb);
        for (int i = 0; i < 4; i++) {
            int k = (int)va[i] - (int)vb[i];
            pos[i] = (uint8_t)(k > 0 ? k : 0);
            neg[i] = (uint8_t)(k < 0 ? -k : 0);
            if (i < (alpha ? 4 : 3)) { any_pos |= k > 0; any_neg |= k < 0; }
        }
        in_t P = fold_const(pos, alpha), N = fold_const(neg, alpha);
        if (!any_neg || is(d, S_ZERO)) {
            if (!any_pos) { o[0] = U1(GPU_REPLACE, d); return 1; }
            if (is(d, S_ZERO)) { o[0] = U2(GPU_MODULATE, P, c); return 1; }
            o[0] = U3(GPU_MULTIPLY_ADD, P, c, d); return 1;
        }
        o[0] = U2(GPU_MODULATE, N, c);
        o[1] = U2(GPU_SUBTRACT, d, PREV);
        if (!any_pos) return 2;
        o[2] = U3(GPU_MULTIPLY_ADD, P, c, PREV); return 3;
    }
    if (is(b, S_ZERO)) {
        if (is(a, S_ONE)) {
            if (is(d, S_ZERO)) { o[0] = U1(GPU_REPLACE, c); return 1; }
            o[0] = U2(GPU_ADD, c, d); return 1;
        }
        if (is(c, S_ONE)) {
            if (is(d, S_ZERO)) { o[0] = U1(GPU_REPLACE, a); return 1; }
            o[0] = U2(GPU_ADD, a, d); return 1;
        }
        if (is(d, S_ZERO)) { o[0] = U2(GPU_MODULATE, a, c); return 1; }
        o[0] = U3(GPU_MULTIPLY_ADD, a, c, d); return 1;
    }
    if (eq(d, b)) {
        if (is(c, S_ONE)) { o[0] = U1(GPU_REPLACE, a); return 1; }
        o[0] = U3(GPU_INTERPOLATE, a, b, c); return 1;
    }
    if (is(a, S_ONE) && is(d, S_ZERO)) {
        o[0] = U2(GPU_MODULATE, b, c); o[0].inv[0] = 1; return 1;
    }
    if (is(c, S_ONE)) {
        o[0] = U2(GPU_SUBTRACT, a, b);
        if (is(d, S_ZERO)) return 1;
        o[1] = U2(GPU_ADD, PREV, d); return 2;
    }
    o[0] = U2(GPU_SUBTRACT, a, b);
    if (is(d, S_ZERO)) { o[1] = U2(GPU_MODULATE, PREV, c); return 2; }
    o[1] = U3(GPU_MULTIPLY_ADD, PREV, c, d); return 2;
}


/* A stage has a single constant colour. When one micro-op needs two
 * different constants (e.g. INTERPOLATE(PRIM, ENV, TEXEL0) from the very
 * common (PRIM - ENV) * TEXEL0 + ENV), preload one of them with a
 * REPLACE stage and read it back as PREVIOUS. Not possible if the op
 * already consumes PREVIOUS; that rare case keeps the last constant. */
static int split_consts(uop_t *o, int n) {
    for (int i = 0; i < n && n < MAX_UOPS; i++) {
        in_t ks[3];
        int nk = 0, has_prev = 0;
        for (int k = 0; k < o[i].n; k++) {
            in_t x = o[i].src[k];
            if (x.s == S_PREV) has_prev = 1;
            if (!is_const(x)) continue;
            int dup = 0;
            for (int j = 0; j < nk; j++) if (ks[j].s == x.s && ks[j].alpha == x.alpha) dup = 1;
            if (!dup) ks[nk++] = x;
        }
        if (nk == 2 && has_prev && o[i].func == GPU_MULTIPLY_ADD && is(o[i].src[0], S_PREV) && n < MAX_UOPS) {
            /* (a - b) * k1 + k2 after a SUBTRACT: one constant per stage */
            in_t k1 = o[i].src[1], k2 = o[i].src[2];
            memmove(&o[i + 2], &o[i + 1], sizeof(o[0]) * (size_t)(n - i - 1));
            o[i] = U2(GPU_MODULATE, (in_t){ S_PREV, 0 }, k1);
            o[i + 1] = U2(GPU_ADD, (in_t){ S_PREV, 0 }, k2);
            n++;
            i++;
            continue;
        }
        if (nk < 2 || has_prev) continue;
        in_t k2 = ks[nk - 1];
        memmove(&o[i + 1], &o[i], sizeof(o[0]) * (size_t)(n - i));
        o[i] = U1(GPU_REPLACE, k2);
        n++;
        uop_t *u = &o[i + 1];
        for (int k = 0; k < u->n; k++) {
            if (u->src[k].s == k2.s && u->src[k].alpha == k2.alpha) u->src[k] = (in_t){ S_PREV, 0 };
        }
        i++;
    }
    return n;
}

typedef struct {
    uop_t rgb, alpha;
    int   cycle;         /* 0 or 1 */
    bool  buf_rgb;       /* this stage's RGB goes to the combiner buffer */
} stage_t;

/* Turn an in_t into a PICA source + operand for the given channel. */
static int map_src(in_t x, int is_alpha, int inv, int stage, int first_c2_stage, int last_c1_stage,
                   GPU_TEVSRC *src, int *op) {
    GPU_TEVSRC s;
    switch (x.s) {
        case S_T0:    s = GPU_TEXTURE0; break;
        case S_T1:    s = GPU_TEXTURE1; break;
        case S_NOISE: s = GPU_TEXTURE1; break;
        case S_SHADE: s = GPU_PRIMARY_COLOR; break;
        case S_PREV:  s = GPU_PREVIOUS; break;
        case S_BUF:   s = GPU_PREVIOUS_BUFFER; break;
        case S_COMB:
            if (last_c1_stage < 0) s = GPU_PRIMARY_COLOR;           /* no first cycle: undefined on HW */
            else if (stage == first_c2_stage) s = GPU_PREVIOUS;
            else s = GPU_PREVIOUS_BUFFER;
            break;
        default:      s = GPU_CONSTANT; break;
    }
    *src = s;
    if (is_alpha && x.alpha >= 2) {   /* a colour channel into the alpha lane */
        static const int ch[3] = { GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B };
        *op = ch[x.alpha - 2];
    } else if (is_alpha) {
        *op = inv ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
    } else if (x.alpha && s != GPU_CONSTANT) {
        *op = inv ? GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_RGB_SRC_ALPHA;
    } else {
        *op = inv ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR;
    }
    return 0;
}

static void decode_cycle(const gfx_state_t *st, int cyc, in_t rgb[4], in_t al[4]) {
    uint32_t w0 = st->combine_w0, w1 = st->combine_w1;
    if (cyc == 0) {
        rgb[0] = rgb_a((w0 >> 20) & 0xF); rgb[1] = rgb_b((w1 >> 28) & 0xF);
        rgb[2] = rgb_c((w0 >> 15) & 0x1F); rgb[3] = rgb_d((w1 >> 15) & 0x7);
        al[0] = a_abd((w0 >> 12) & 0x7); al[1] = a_abd((w1 >> 12) & 0x7);
        al[2] = a_c((w0 >> 9) & 0x7);    al[3] = a_abd((w1 >> 9) & 0x7);
    } else {
        rgb[0] = rgb_a((w0 >> 5) & 0xF); rgb[1] = rgb_b((w1 >> 24) & 0xF);
        rgb[2] = rgb_c(w0 & 0x1F);       rgb[3] = rgb_d((w1 >> 6) & 0x7);
        al[0] = a_abd((w1 >> 21) & 0x7); al[1] = a_abd((w1 >> 3) & 0x7);
        al[2] = a_c((w1 >> 18) & 0x7);   al[3] = a_abd(w1 & 0x7);
        /* the RDP swaps TEXEL0/TEXEL1 in the second cycle */
        for (int i = 0; i < 4; i++) {
            if (rgb[i].s == S_T0) rgb[i].s = S_T1; else if (rgb[i].s == S_T1) rgb[i].s = S_T0;
            if (al[i].s == S_T0) al[i].s = S_T1; else if (al[i].s == S_T1) al[i].s = S_T0;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (rgb[i].s == S_LODF) rgb[i] = Z;
        if (al[i].s == S_LODF) al[i] = Z;
    }
}

/* Second cycle (a - b) * COMBINED + d with constants a, b, d, where d - n*t
 * (n the negative part of a - b) can drop below -128: the RDP's 9-bit
 * result wraps there and clamps to 255. Channels with a positive part must
 * already be saturated by d. Stages:
 *   h = d - n*t                   per channel; to the buffer. Alpha: one
 *                                 channel of t (the one with the largest n)
 *   g = (t - thr) * 16            the wrapped part, from that channel
 *   out = h + g                   alpha: the cycle's own alpha op
 * Exact for a grey COMBINED (an intensity texture, as the fire-hit ring
 * has). Returns the stage count, 0 if the shape does not apply. */
static int wrap_cycle(const gfx_state_t *st, const in_t r[4], const uop_t *ua, int na, stage_t *o, int room) {
    const in_t PREV = { S_PREV, 0 }, PREV_A = { S_PREV, 1 }, BUF = { S_BUF, 0 };
    in_t a = r[0], b = r[1], c = r[2], d = r[3];
    if (!is_const(a) || !is_const(b) || !is_const(d) || is(a, S_ZERO) || is(b, S_ZERO) || eq(a, b)) return 0;
    if (!is(c, S_COMB) || c.alpha || na != 1 || room < 4 || g_nkc + 2 > 4) return 0;
    uint8_t va[4], vb[4], vd[4], neg[4], thr[4];
    int widest = 0;
    bool wraps = false;
    const_value(st, a, va);
    const_value(st, b, vb);
    const_value(st, d, vd);
    for (int i = 0; i < 3; i++) {
        int k = (int)va[i] - (int)vb[i];
        if (k > 0 && vd[i] != 255) return 0;
        neg[i] = (uint8_t)(k < 0 ? -k : 0);
        if (neg[i] > neg[widest]) widest = i;
        int t = neg[i] ? ((vd[i] + 129) * 255 + neg[i] - 1) / neg[i] : 256;
        thr[i] = (uint8_t)(t > 255 ? 255 : t);
        wraps |= t <= 255;
    }
    if (!wraps) return 0;
    neg[3] = thr[3] = 0;
    in_t N = fold_const(neg, 0), THR = fold_const(thr, 0);
    in_t T = { S_COMB, (uint8_t)(2 + widest) };   /* alpha lane: COMBINED's red, green or blue */
    int n = 0;
    o[n].rgb = U2(GPU_MODULATE, c, N);
    o[n].alpha = U1(GPU_REPLACE, T);
    n++;
    o[n].rgb = U2(GPU_SUBTRACT, d, PREV);
    o[n].alpha = U1(GPU_REPLACE, PREV);
    o[n].buf_rgb = true;
    n++;
    o[n].rgb = U2(GPU_SUBTRACT, PREV_A, THR); o[n].rgb.scale = GPU_TEVSCALE_4;
    o[n].alpha = U1(GPU_REPLACE, PREV);
    n++;
    if (room >= 5) {   /* sharper step */
        o[n].rgb = U1(GPU_REPLACE, PREV); o[n].rgb.scale = GPU_TEVSCALE_4;
        o[n].alpha = U1(GPU_REPLACE, PREV);
        n++;
    }
    o[n].rgb = U2(GPU_ADD, BUF, PREV);
    o[n].alpha = ua[0];
    n++;
    return n;
}

static int build(const gfx_state_t *st, bool two_cycle, stage_t *stages, int *last_c1) {
    int n = 0;
    *last_c1 = -1;
    g_nkc = 0;
    int cycles = two_cycle ? 2 : 1;
    for (int cyc = 0; cyc < cycles; cyc++) {
        in_t r[4], a[4];
        decode_cycle(st, cyc, r, a);
        if (!two_cycle) {
            /* one texture fetch per pixel in 1-cycle mode: TEXEL1 is the
             * same tile's texel (the next pixel's on the RDP), not tile + 1.
             * The opening's PAPER MARIO logo (kmr_21) takes its alpha from
             * TEXEL1 and showed as a white box. */
            for (int i = 0; i < 4; i++) {
                if (r[i].s == S_T1) r[i].s = S_T0;
                if (a[i].s == S_T1) a[i].s = S_T0;
            }
        }
        uop_t ur[MAX_UOPS], ua[MAX_UOPS];
        int na = split_consts(ua, compile(st, 1, a[0], a[1], a[2], a[3], ua));
        if (cyc == 1 && n >= 1) {
            stage_t w[6];
            memset(w, 0, sizeof(w));
            int nw = wrap_cycle(st, r, ua, na, w, 6 - n);
            if (nw > 0) {
                for (int i = 0; i < nw; i++) {
                    stages[n] = w[i];
                    stages[n++].cycle = cyc;
                }
                continue;
            }
        }
        int nr = split_consts(ur, compile(st, 0, r[0], r[1], r[2], r[3], ur));
        int ns = nr > na ? nr : na;
        const in_t PREV = { S_PREV, 0 };
        for (int i = 0; i < ns && n < 6; i++, n++) {
            stages[n].rgb   = i < nr ? ur[i] : U1(GPU_REPLACE, PREV);
            stages[n].alpha = i < na ? ua[i] : U1(GPU_REPLACE, PREV);
            stages[n].cycle = cyc;
            stages[n].buf_rgb = false;
        }
        if (cyc == 0) *last_c1 = n - 1;
    }
    /* A second cycle that is just "COMBINED" adds a no-op stage; harmless. */
    return n;
}

void gfx_combiner_info(const gfx_state_t *st, bool two_cycle, gfx_cc_info_t *info) {
    stage_t stg[6];
    int last_c1;
    int n = build(st, two_cycle, stg, &last_c1);
    info->uses_tex0 = info->uses_tex1 = info->uses_noise = false;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) {
            if (k < stg[i].rgb.n) {
                if (stg[i].rgb.src[k].s == S_T0) info->uses_tex0 = true;
                if (stg[i].rgb.src[k].s == S_T1) info->uses_tex1 = true;
                if (stg[i].rgb.src[k].s == S_NOISE) info->uses_noise = true;
            }
            if (k < stg[i].alpha.n) {
                if (stg[i].alpha.src[k].s == S_T0) info->uses_tex0 = true;
                if (stg[i].alpha.src[k].s == S_T1) info->uses_tex1 = true;
            }
        }
    }
}

/* the six PICA stages (and buffer masks) for one combine state */
typedef struct {
    C3D_TexEnv env[6];
    int rgb_mask, buf_mask;
    gfx_cc_info_t info;
} cc_result_t;

static void build_envs(const gfx_state_t *st, bool two_cycle, cc_result_t *res) {
    gfx_cc_info_t *info = &res->info;
    stage_t stg[6];
    int last_c1;
    s_noise_flat = false;
    gfx_combiner_info(st, two_cycle, info);
    if (info->uses_noise && info->uses_tex1) {
        s_noise_flat = true;
        info->uses_noise = false;
    }
    int n = build(st, two_cycle, stg, &last_c1);
    s_noise_flat = false;
    int first_c2 = (two_cycle && last_c1 + 1 < n) ? last_c1 + 1 : -1;

    for (int i = 0; i < 6; i++) {
        C3D_TexEnv *env = &res->env[i];
        C3D_TexEnvInit(env);
        if (i >= n) continue;   /* pass-through: REPLACE(PREVIOUS) */

        uint8_t crgb[4] = { 0, 0, 0, 0 }, ca[4] = { 0, 0, 0, 0 };
        GPU_TEVSRC s[3];
        int op[3];

        /* RGB */
        const uop_t *u = &stg[i].rgb;
        for (int k = 0; k < 3; k++) {
            in_t x = k < u->n ? u->src[k] : (in_t){ S_PREV, 0 };
            map_src(x, 0, u->inv[k], i, first_c2, last_c1, &s[k], &op[k]);
            if (k < u->n && is_const(x)) const_value(st, x, crgb);
        }
        C3D_TexEnvSrc(env, C3D_RGB, s[0], s[1], s[2]);
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)op[0], (GPU_TEVOP_RGB)op[1], (GPU_TEVOP_RGB)op[2]);
        C3D_TexEnvFunc(env, C3D_RGB, (GPU_COMBINEFUNC)u->func);
        if (u->scale) C3D_TexEnvScale(env, C3D_RGB, (GPU_TEVSCALE)u->scale);

        /* Alpha */
        u = &stg[i].alpha;
        for (int k = 0; k < 3; k++) {
            in_t x = k < u->n ? u->src[k] : (in_t){ S_PREV, 1 };
            if (x.alpha < 2) x.alpha = 1;
            map_src(x, 1, u->inv[k], i, first_c2, last_c1, &s[k], &op[k]);
            if (k < u->n && is_const(x)) const_value(st, x, ca);
        }
        C3D_TexEnvSrc(env, C3D_Alpha, s[0], s[1], s[2]);
        C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)op[0], (GPU_TEVOP_A)op[1], (GPU_TEVOP_A)op[2]);
        C3D_TexEnvFunc(env, C3D_Alpha, (GPU_COMBINEFUNC)u->func);
        if (u->scale) C3D_TexEnvScale(env, C3D_Alpha, (GPU_TEVSCALE)u->scale);

        C3D_TexEnvColor(env, (uint32_t)crgb[0] | ((uint32_t)crgb[1] << 8) |
                             ((uint32_t)crgb[2] << 16) | ((uint32_t)ca[3] << 24));
    }
    /* second-cycle stages after the first read COMBINED from the buffer;
     * wrap_cycle() parks a value there too (RGB only) */
    int buf_mask = first_c2 >= 0 && last_c1 >= 0 && last_c1 < 4 ? 1 << last_c1 : 0;
    int rgb_mask = buf_mask;
    for (int i = 0; i < n && i < 4; i++)
        if (stg[i].buf_rgb) rgb_mask |= 1 << i;
    res->rgb_mask = rgb_mask;
    res->buf_mask = buf_mask;
}

/* Translating a combine mode costs more than the draw it is for on an Old
 * 3DS, and a frame uses a few dozen distinct ones: keep the results, keyed by
 * everything build_envs reads. Only the stages that differ from what the GPU
 * already has are handed to citro3d (each one it re-sends as commands). */
typedef struct {
    uint32_t w0, w1;
    uint8_t prim[4], env[4], key_center[3], key_scale[3], lod, two;
    int16_t k4, k5;
} cc_key_t;

#define CC_CACHE 128
static struct { cc_key_t key; bool used; cc_result_t res; } s_cc[CC_CACHE];
static C3D_TexEnv s_applied[6];
static int s_applied_rgb = -1, s_applied_buf = -1;
static bool s_applied_valid;

/* something else (the bottom screen, fills, window clears) set the stages */
void gfx_combiner_invalidate(void) { s_applied_valid = false; }

void gfx_combiner_apply(const gfx_state_t *st, bool two_cycle, gfx_cc_info_t *info) {
    cc_key_t k;
    memset(&k, 0, sizeof k);
    k.w0 = st->combine_w0;
    k.w1 = st->combine_w1;
    memcpy(k.prim, st->prim, 4);
    memcpy(k.env, st->env, 4);
    memcpy(k.key_center, st->key_center, 3);
    memcpy(k.key_scale, st->key_scale, 3);
    k.lod = st->prim_lod_frac;
    k.two = two_cycle;
    k.k4 = st->k4;
    k.k5 = st->k5;
    uint32_t h = 2166136261u;
    const uint8_t *kb = (const uint8_t *)&k;
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ kb[i]) * 16777619u;
    unsigned slot = (h ^ (h >> 16)) & (CC_CACHE - 1);
    if (!s_cc[slot].used || memcmp(&s_cc[slot].key, &k, sizeof k)) {
        build_envs(st, two_cycle, &s_cc[slot].res);
        s_cc[slot].key = k;
        s_cc[slot].used = true;
    }
    const cc_result_t *r = &s_cc[slot].res;
    *info = r->info;
#ifdef PM3DS_NO_STATE_CACHE   /* debug A/B (hardware-only flicker): all six stages every time */
    s_applied_valid = false;
#endif
    for (int i = 0; i < 6; i++) {
        if (s_applied_valid && !memcmp(&s_applied[i], &r->env[i], sizeof r->env[i])) continue;
        *C3D_GetTexEnv(i) = r->env[i];
        s_applied[i] = r->env[i];
    }
    if (!s_applied_valid || s_applied_rgb != r->rgb_mask) C3D_TexEnvBufUpdate(C3D_RGB, r->rgb_mask);
    if (!s_applied_valid || s_applied_buf != r->buf_mask) C3D_TexEnvBufUpdate(C3D_Alpha, r->buf_mask);
    s_applied_rgb = r->rgb_mask;
    s_applied_buf = r->buf_mask;
    s_applied_valid = true;
}
