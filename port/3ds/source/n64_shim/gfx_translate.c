/*
 * gfx_translate.c — F3DEX2 display-list interpreter (M12 rewrite).
 *
 * The RSP half runs here on the CPU, fast3d-style: matrix stacks with real
 * PUSH/LOAD/MUL semantics, modelview x projection, viewport, directional
 * lighting in model space, texgen, fog, back/front culling. Transformed
 * triangles and screen rectangles go to the GPU backend (gfx_present.c),
 * which owns all PICA200 state.
 *
 * Every encoding below is the canonical F3DEX2 one emitted by the upstream
 * PR/gbi.h the game is compiled against (port/3ds/include/PR/gbi.h wraps it).
 * Matrices use the PORT layout: 16 row-major int32 s15.16 values
 * (source/n64_shim/gu_impl.c guMtxF2L), not the N64 split int/frac layout.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gfx_state.h"
#include "pm3ds_addr.h"
#include "pm3ds_gfx_tags.h"
#include "pm3ds_log.h"
#ifndef PM3DS_HOST_TEST
#include "gfx_interp.h"
#include "gfx_prof.h"
#endif

typedef struct { uint32_t w0, w1; } gfx_cmd_t;

static inline uint32_t bf(uint32_t w, int s, int n) { return (w >> s) & ((1u << n) - 1u); }

/* F3DEX2 opcodes */
#define OP_NOOP        0x00
#define OP_VTX         0x01
#define OP_MODIFYVTX   0x02
#define OP_CULLDL      0x03
#define OP_BRANCH_Z    0x04
#define OP_TRI1        0x05
#define OP_TRI2        0x06
#define OP_QUAD        0x07
#define OP_SPECIAL_3   0xD3
#define OP_SPECIAL_2   0xD4
#define OP_SPECIAL_1   0xD5
#define OP_DMA_IO      0xD6
#define OP_TEXTURE     0xD7
#define OP_POPMTX      0xD8
#define OP_GEOMETRYMODE 0xD9
#define OP_MTX         0xDA
#define OP_MOVEWORD    0xDB
#define OP_MOVEMEM     0xDC
#define OP_LOAD_UCODE  0xDD
#define OP_DL          0xDE
#define OP_ENDDL       0xDF
#define OP_SPNOOP      0xE0
#define OP_RDPHALF_1   0xE1
#define OP_SETOTHERMODE_L 0xE2
#define OP_SETOTHERMODE_H 0xE3
#define OP_TEXRECT     0xE4
#define OP_TEXRECTFLIP 0xE5
#define OP_RDPLOADSYNC 0xE6
#define OP_RDPPIPESYNC 0xE7
#define OP_RDPTILESYNC 0xE8
#define OP_RDPFULLSYNC 0xE9
#define OP_SETKEYGB    0xEA
#define OP_SETKEYR     0xEB
#define OP_SETCONVERT  0xEC
#define OP_SETSCISSOR  0xED
#define OP_SETPRIMDEPTH 0xEE
#define OP_RDPSETOTHERMODE 0xEF
#define OP_LOADTLUT    0xF0
#define OP_RDPHALF_2   0xF1
#define OP_SETTILESIZE 0xF2
#define OP_LOADBLOCK   0xF3
#define OP_LOADTILE    0xF4
#define OP_SETTILE     0xF5
#define OP_FILLRECT    0xF6
#define OP_SETFILLCOLOR 0xF7
#define OP_SETFOGCOLOR 0xF8
#define OP_SETBLENDCOLOR 0xF9
#define OP_SETPRIMCOLOR 0xFA
#define OP_SETENVCOLOR 0xFB
#define OP_SETCOMBINE  0xFC
#define OP_SETTIMG     0xFD
#define OP_SETZIMG     0xFE
#define OP_SETCIMG     0xFF

/* F3DEX2 geometry mode bits */
#define GM_CULL_FRONT     0x00000200u
#define GM_CULL_BACK      0x00000400u
#define GM_FOG            0x00010000u
#define GM_LIGHTING       0x00020000u
#define GM_TEXGEN         0x00040000u
#define GM_TEXGEN_LINEAR  0x00080000u

/* othermode_h cycle type */
#define CYC_MASK   (3u << 20)
#define CYC_COPY   (2u << 20)
#define CYC_FILL   (3u << 20)

extern int pm3ds_pi_read(uint32_t addr, void *dst, uint32_t size);

/* ---- matrices ---------------------------------------------------------- */

static void mtx_ident(float m[4][4]) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

/* r = a * b (N64 row-vector convention: v' = v * a * b). r may alias. */
static void mtx_mul(float r[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(r, t, sizeof(t));
}

static void mtx_from_port(float m[4][4], const int32_t *src) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] = (float)src[i * 4 + j] * (1.0f / 65536.0f);
        }
    }
}

/* ---- addresses ---------------------------------------------------------- */

/* Rebase segmented addresses. Only applies to 0x01..0x0F top bytes whose
 * segment has a non-zero base; everything the port builds is a host pointer
 * (image 0x001xxxxx-0x00Dxxxxx, linear/heap 0x08+/0x14+/0x30+), and with
 * VIRTUAL_TO_PHYSICAL now the identity, segment bases are host pointers too. */
#ifdef PM3DS_HACK_RUNTIME
/* The hack runtime: display lists live in the emulated RDRAM, in the N64's
 * byte order. The RSP's rule applies: segment (addr >> 24 & 0xF) base plus
 * the low 24 bits, a physical address, here a host pointer into RDRAM. */
extern uint8_t *rdram;
uint32_t gfx_seg_resolve(const gfx_state_t *st, uint32_t addr) {
    uint32_t phys = st->segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFFu);
    return (uint32_t)(uintptr_t)(rdram + (phys & 0x7FFFFFu));
}
#else
uint32_t gfx_seg_resolve(const gfx_state_t *st, uint32_t addr) {
    unsigned seg = (addr >> 24) & 0xFF;
    if (seg == 0 || seg > 0x0F) return addr;
    if (st->segments[seg] == 0) return addr;
    return st->segments[seg] + (addr & 0x00FFFFFFu);
}
#endif

/* Host memory a display list may legitimately point into: the 3DSX image,
 * the app heap, and the linear heaps (old and New 3DS). Anything else is a
 * stale/unrelocated pointer that would send the walker through garbage. */
extern char __end__[];
static inline int addr_plausible(uint32_t a) {
    if ((a & 3) != 0) return 0;
#ifdef PM3DS_HACK_RUNTIME
    return a - (uint32_t)(uintptr_t)rdram < 0x800000u;
#endif
    if (a >= 0x00100000u && a < (uint32_t)(uintptr_t)__end__) return 1;
    if (a >= 0x08000000u && a < 0x10000000u) return 1;
    if (a >= 0x14000000u && a < 0x1C000000u) return 1;
    if (a >= 0x30000000u && a < 0x38000000u) return 1;
    return 0;
}

static void log_bad_addr(const char *what, uint32_t addr, const void *cmd) {
    static int n;
    if (n++ < 12) pm3ds_log(PM3DS_LOG_WARN, "gfx: bad %s address 0x%08X (cmd at %p)", what, (unsigned)addr, cmd);
}

/* ---- state -------------------------------------------------------------- */

/* the texture cache keys paletted textures by palette content; hashing it
 * here, once per load, instead of on every draw */
void gfx_tlut_rehash(gfx_state_t *st, int bank) {
    const uint8_t *p = &st->tlut[bank * 32];
    uint32_t h = 2166136261u;
    for (int i = 0; i < 32; i++) h = (h ^ p[i]) * 16777619u;
    st->tlut_hash[bank] = h;
}

void gfx_state_init(gfx_state_t *st) {
    memset(st, 0, sizeof(*st));
    for (int b = 0; b < 16; b++) gfx_tlut_rehash(st, b);
    mtx_ident(st->mv[0]);
    mtx_ident(st->proj);
    st->vp_scale[0] = 320 * 2; st->vp_scale[1] = 240 * 2; st->vp_scale[2] = 0x1FF;
    st->vp_trans[0] = 320 * 2; st->vp_trans[1] = 240 * 2; st->vp_trans[2] = 0x1FF;
    st->lookat_dir[0][0] = 127;   /* X */
    st->lookat_dir[1][1] = 127;   /* Y */
    st->scissor.lrx = 320 << 2;
    st->scissor.lry = 240 << 2;
    st->cimg_is_screen = true;
    st->tex_scale_s = st->tex_scale_t = 0xFFFF;
    st->dirty = GFX_DIRTY_ALL;
}

static void update_mvp(gfx_state_t *st) {
    if (!st->mvp_valid) {
        mtx_mul(st->mvp, st->mv[st->mv_sp], st->proj);
        st->mvp_valid = true;
    }
}

static void normalize3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

/* Light directions are given in world/view space; bring them into model
 * space with the transposed modelview so they dot directly with the
 * model-space normals (what the F3DEX2 microcode does). */
static void update_lights(gfx_state_t *st) {
    if (st->lights_valid) return;
    const float (*m)[4] = st->mv[st->mv_sp];
    for (int i = 0; i < st->num_lights && i < GFX_MAX_LIGHTS; i++) {
        float d[3] = { st->lights[i].dir[0] / 127.0f, st->lights[i].dir[1] / 127.0f, st->lights[i].dir[2] / 127.0f };
        for (int j = 0; j < 3; j++) {
            st->light_dir_model[i][j] = d[0] * m[j][0] + d[1] * m[j][1] + d[2] * m[j][2];
        }
        normalize3(st->light_dir_model[i]);
    }
    for (int k = 0; k < 2; k++) {
        float d[3] = { st->lookat_dir[k][0] / 127.0f, st->lookat_dir[k][1] / 127.0f, st->lookat_dir[k][2] / 127.0f };
        for (int j = 0; j < 3; j++) {
            st->lookat_model[k][j] = d[0] * m[j][0] + d[1] * m[j][1] + d[2] * m[j][2];
        }
        normalize3(st->lookat_model[k]);
    }
    st->lights_valid = true;
}

/* ---- RSP: vertices / triangles ----------------------------------------- */

typedef struct {
    int16_t ob[3];
    uint16_t flag;
    int16_t tc[2];
    uint8_t cn[4];
} n64_vtx_t;

int pm3ds_gfx_dbg_vtx;
/* Stereoscopic 3D (gfx_present.c sets these per eye): perspective vertices
 * get a horizontal disparity of shift * (1 - CONV / depth) screen pixels, so
 * depth CONV sits at the screen plane, farther geometry recedes and nearer
 * geometry pops out. Ortho/2D geometry (w == 1) stays on the screen plane. */
float pm3ds_stereo_shift;              /* signed pixels at infinity, 0 = mono */
#define PM3DS_STEREO_CONV 420.0f       /* ~ the field camera's distance to Mario */

uint32_t gfx_vtx_serial;   /* gfx_vtx_t.serial source */
float pm3ds_view_ndc_x(float vp_x, float vp_w);   /* gfx_present.c */
extern unsigned pm3ds_tstat_cullcheck_visible;

/* gSPModifyVertex: change one field of a loaded vertex (the underwater
 * ripple moves its grid's corners in screen space this way). */
static void handle_modifyvtx(gfx_state_t *st, const gfx_cmd_t *g) {
    int where = (int)bf(g->w0, 16, 8);
    int idx = (int)bf(g->w0, 0, 16) / 2;
    if (idx >= GFX_VTX_POOL_SIZE) return;
    gfx_vtx_t *d = &st->vtx[idx];
    d->serial = ++gfx_vtx_serial;
    switch (where) {
        case 0x10:   /* G_MWO_POINT_RGBA */
            d->r = (uint8_t)(g->w1 >> 24); d->g = (uint8_t)(g->w1 >> 16);
            d->b = (uint8_t)(g->w1 >> 8);  d->a = (uint8_t)g->w1;
            break;
        case 0x14:   /* G_MWO_POINT_ST, s10.5 */
            d->s = (float)(int16_t)(g->w1 >> 16);
            d->t = (float)(int16_t)g->w1;
            break;
        case 0x18: { /* G_MWO_POINT_XYSCREEN, s13.2 screen pixels */
            float x = (int16_t)(g->w1 >> 16) * 0.25f, y = (int16_t)g->w1 * 0.25f;
            const float vsx = st->vp_scale[0] * 0.25f, vtx_ = st->vp_trans[0] * 0.25f;
            const float vsy = st->vp_scale[1] * 0.25f, vty = st->vp_trans[1] * 0.25f;
            d->px = x * d->cw;
            d->py = y * d->cw;
            if (vsx != 0.0f) d->cx = (d->px - d->cw * vtx_) / vsx;
            if (vsy != 0.0f) d->cy = -(d->py - d->cw * vty) / vsy;
            break;
        }
        default:     /* G_MWO_POINT_ZSCREEN: unused by the game */
            break;
    }
}

static void handle_vtx(gfx_state_t *st, const gfx_cmd_t *g) {
    int n   = (int)bf(g->w0, 12, 8);
    int end = (int)bf(g->w0, 1, 7);
    int v0  = end - n;
    uint32_t addr = gfx_seg_resolve(st, g->w1);
    if (n <= 0 || v0 < 0 || end > GFX_VTX_POOL_SIZE) return;
    if (!addr_plausible(addr)) { log_bad_addr("G_VTX", g->w1, g); return; }
    const n64_vtx_t *src = (const n64_vtx_t *)(uintptr_t)addr;

    update_mvp(st);
    const bool lighting = (st->geometry_mode & GM_LIGHTING) != 0;
    if (lighting) update_lights(st);
    const float (*m)[4] = st->mvp;
    const float vsx = st->vp_scale[0] * 0.25f, vtx_ = st->vp_trans[0] * 0.25f;
    const float vsy = st->vp_scale[1] * 0.25f, vty = st->vp_trans[1] * 0.25f;
    /* loop invariants the compiler can't hoist (the stores below may alias them);
     * x * scale / 65536 == x * (scale / 65536) exactly: a power-of-two step */
    const uint32_t gm = st->geometry_mode;
    const float tss = st->tex_scale_s * (1.0f / 65536.0f), tst = st->tex_scale_t * (1.0f / 65536.0f);
    /* A small viewport is a picture in a window (Goombario's tattle: 138x99, its camera a
     * close-up of the enemy). The disparity above is tuned for the field camera, so a close-up
     * popped out far more than the eyes can fuse at the top of the slider (Discord, 1.3.10);
     * such a window stays on the screen plane, like its frame. Full width is 320 * 2; the
     * narrowest scene viewports elsewhere are 262 wide (some battles), so 200 only catches
     * windows like that one. */
    const float stereo = st->vp_scale[0] < 200 * 2 ? 0.0f : pm3ds_stereo_shift;

    for (int i = 0; i < n; i++) {
        const n64_vtx_t *v = &src[i];
#ifdef PM3DS_HACK_RUNTIME
        n64_vtx_t be = *v;
        for (int k = 0; k < 3; k++) be.ob[k] = (int16_t)__builtin_bswap16((uint16_t)be.ob[k]);
        be.flag = __builtin_bswap16(be.flag);
        be.tc[0] = (int16_t)__builtin_bswap16((uint16_t)be.tc[0]);
        be.tc[1] = (int16_t)__builtin_bswap16((uint16_t)be.tc[1]);
        v = &be;
#endif
        gfx_vtx_t *d = &st->vtx[v0 + i];
        d->serial = ++gfx_vtx_serial;
        float x = v->ob[0], y = v->ob[1], z = v->ob[2];
        d->cx = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        d->cy = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        d->cz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        d->cw = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        /* N64 viewport: screen y is flipped by the RSP. */
        d->px =  d->cx * vsx + d->cw * vtx_;
        d->py = -d->cy * vsy + d->cw * vty;
#if defined(PM3DS_HACK_RUNTIME) && defined(PM3DS_DRAWLOG_FRAME)
        {   /* debug: what the first vertices of each load become */
            extern uint32_t pm3ds_gfx_frame_number(void);
            extern uint32_t pm3ds_drawlog_frame;
            if (i < 2 && pm3ds_gfx_frame_number() == pm3ds_drawlog_frame)
                pm3ds_log(PM3DS_LOG_INFO, "VTX ob=%d,%d,%d clip=%.1f,%.1f,%.1f,%.1f px=%.1f,%.1f vp=%d,%d/%d,%d",
                          v->ob[0], v->ob[1], v->ob[2], d->cx, d->cy, d->cz, d->cw, d->px, d->py,
                          st->vp_scale[0], st->vp_scale[1], st->vp_trans[0], st->vp_trans[1]);
        }
#endif
        if (stereo != 0.0f && d->cw != 1.0f) {
            d->px += stereo * (d->cw - PM3DS_STEREO_CONV);   /* homogeneous */
        }

        float s = v->tc[0], t = v->tc[1];
        if (lighting) {
            /* times the reciprocal: three divides per lit vertex add up on an Old 3DS */
            float nx = (int8_t)v->cn[0] * (1.0f / 127.0f);
            float ny = (int8_t)v->cn[1] * (1.0f / 127.0f);
            float nz = (int8_t)v->cn[2] * (1.0f / 127.0f);
            const gfx_light_t *amb = &st->lights[st->num_lights < GFX_MAX_LIGHTS ? st->num_lights : GFX_MAX_LIGHTS - 1];
            float r = amb->col[0], gg = amb->col[1], b = amb->col[2];
            for (int l = 0; l < st->num_lights && l < GFX_MAX_LIGHTS - 1; l++) {
                float in = nx * st->light_dir_model[l][0] + ny * st->light_dir_model[l][1] + nz * st->light_dir_model[l][2];
                if (in > 0.0f) {
                    r  += in * st->lights[l].col[0];
                    gg += in * st->lights[l].col[1];
                    b  += in * st->lights[l].col[2];
                }
            }
            d->r = (uint8_t)(r  > 255.0f ? 255 : r);
            d->g = (uint8_t)(gg > 255.0f ? 255 : gg);
            d->b = (uint8_t)(b  > 255.0f ? 255 : b);
            if (gm & GM_TEXGEN) {
                float dx = nx * st->lookat_model[0][0] + ny * st->lookat_model[0][1] + nz * st->lookat_model[0][2];
                float dy = nx * st->lookat_model[1][0] + ny * st->lookat_model[1][1] + nz * st->lookat_model[1][2];
                if (gm & GM_TEXGEN_LINEAR) {
                    dx = acosf(-dx) / 4.0f;
                    dy = acosf(-dy) / 4.0f;
                } else {
                    dx = (dx + 1.0f) / 4.0f;
                    dy = (dy + 1.0f) / 4.0f;
                }
                s = dx * st->tex_scale_s;
                t = dy * st->tex_scale_t;
            } else {
                s = s * tss;
                t = t * tst;
            }
        } else {
            d->r = v->cn[0];
            d->g = v->cn[1];
            d->b = v->cn[2];
            s = s * tss;
            t = t * tst;
        }
        d->a = v->cn[3];
        d->s = s;
        d->t = t;
        if (pm3ds_gfx_dbg_vtx > 0 && i == 0) {
            pm3ds_gfx_dbg_vtx--;
            pm3ds_log(PM3DS_LOG_INFO, "gfx: V ob=(%d,%d,%d) clip=(%.1f,%.1f,%.1f,%.1f) px=(%.1f,%.1f) rgba=%02X%02X%02X%02X gm=%08X",
                      v->ob[0], v->ob[1], v->ob[2], d->cx, d->cy, d->cz, d->cw,
                      d->cw != 0.0f ? d->px / d->cw : 0.0f, d->cw != 0.0f ? d->py / d->cw : 0.0f,
                      d->r, d->g, d->b, d->a, (unsigned)st->geometry_mode);
        }

        if (gm & GM_FOG) {
            /* F3DEX2: shade alpha becomes the fog factor. */
            float winv = d->cw > 0.0f ? 1.0f / d->cw : 32767.0f;
            float f = d->cz * winv * st->fog_mul + st->fog_offset;
            if (f < 0.0f) f = 0.0f;
            if (f > 255.0f) f = 255.0f;
            d->a = (uint8_t)f;
        }
    }
}

unsigned pm3ds_tstat_culled;
static void emit_tri(gfx_state_t *st, unsigned ia, unsigned ib, unsigned ic) {
    if (ia >= GFX_VTX_POOL_SIZE || ib >= GFX_VTX_POOL_SIZE || ic >= GFX_VTX_POOL_SIZE) {
        static int n;
        if (n++ < 6) pm3ds_log(PM3DS_LOG_WARN, "gfx: tri index out of range %u %u %u", ia, ib, ic);
        return;
    }
    const gfx_vtx_t *a = &st->vtx[ia], *b = &st->vtx[ib], *c = &st->vtx[ic];
#ifdef PM3DS_CULL_VERIFY
    if (st->cull_check) {   /* an NPC src/npc.c would have skipped: did this triangle reach the screen? */
        const float half = st->vp_scale[0] * 0.25f, centre = st->vp_trans[0] * 0.25f;
        const float kx = pm3ds_view_ndc_x(centre - half, half * 2.0f);   /* exact screen edges, no margin */
        const gfx_vtx_t *t3[3] = { a, b, c };
        unsigned all = 0x1F;
        bool behind = false;
        for (int k = 0; k < 3; k++) {
            const gfx_vtx_t *v = t3[k];
            unsigned code = 0;
            if (v->cx >  kx * v->cw) code |= 0x01;
            if (v->cx < -kx * v->cw) code |= 0x02;
            if (v->cy >  v->cw) code |= 0x04;
            if (v->cy < -v->cw) code |= 0x08;
            if (v->cz < -v->cw) code |= 0x10;
            if (v->cw <= 0.0f) behind = true;
            all &= code;
        }
        if (!all) {   /* not certainly off screen: on it, or partly behind the camera */
            pm3ds_tstat_cullcheck_visible++;
            static int logged;
            if (logged++ < 20) pm3ds_log(PM3DS_LOG_WARN, "CULLVERIFY visible: ndc x %.2f y %.2f w %.1f%s",
                                         a->cw > 0.0f ? a->cx / a->cw : 0.0f, a->cw > 0.0f ? a->cy / a->cw : 0.0f, a->cw,
                                         behind ? " (partly behind the camera)" : "");
        }
    }
#endif
#ifdef PM3DS_DRAWLOG_FRAME
    {
        extern uint32_t pm3ds_gfx_frame_number(void);
        extern uint32_t pm3ds_drawlog_frame;
#ifdef PM3DS_DRAWLOG_ALLTRI_SYM   /* filter by an asset symbol instead of a raw address */
        extern unsigned char PM3DS_DRAWLOG_ALLTRI_SYM[];
#define PM3DS_DRAWLOG_ALLTRI ((uint32_t)(uintptr_t)PM3DS_DRAWLOG_ALLTRI_SYM)
#endif
#ifdef PM3DS_DRAWLOG_ALLTRI
        if (pm3ds_gfx_frame_number() == pm3ds_drawlog_frame && st->timg_addr - PM3DS_DRAWLOG_ALLTRI < 0x4000u) {
            static int k;
            if (k++ < 60)
                pm3ds_log(PM3DS_LOG_INFO, "I4TRI timg=%08X (%.1f,%.1f w%.2f s%.0f t%.0f %02X%02X%02X%02X) (%.1f,%.1f w%.2f) (%.1f,%.1f w%.2f) tile uls=%u ult=%u lrs=%u lrt=%u mask=%u,%u cm=%u,%u shift=%u,%u",
                          (unsigned)st->timg_addr, a->px / a->cw, a->py / a->cw, a->cw, a->s, a->t, a->r, a->g, a->b, a->a,
                          b->px / b->cw, b->py / b->cw, b->cw, c->px / c->cw, c->py / c->cw, c->cw,
                          st->tiles[st->tex_tile & 7].uls, st->tiles[st->tex_tile & 7].ult, st->tiles[st->tex_tile & 7].lrs,
                          st->tiles[st->tex_tile & 7].lrt, st->tiles[st->tex_tile & 7].masks, st->tiles[st->tex_tile & 7].maskt,
                          st->tiles[st->tex_tile & 7].cms, st->tiles[st->tex_tile & 7].cmt,
                          st->tiles[st->tex_tile & 7].shifts, st->tiles[st->tex_tile & 7].shiftt);
        }
#endif
        if (pm3ds_gfx_frame_number() == pm3ds_drawlog_frame && a->cw > 0 && b->cw > 0 && c->cw > 0) {
            float ax = a->px / a->cw, ay = a->py / a->cw, bx = b->px / b->cw, by = b->py / b->cw;
            float cx = c->px / c->cw, cy = c->py / c->cw;
            float area = fabsf((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5f;
            static int n;
            if (area > 15000 && n++ < 40) {
                pm3ds_log(PM3DS_LOG_INFO, "BIGTRI area=%.0f (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) w=%.1f,%.1f,%.1f rgba=%02X%02X%02X%02X cc=%06X:%08X oml=%08X gm=%08X mtx=%p",
                          area, ax, ay, bx, by, cx, cy, a->cw, b->cw, c->cw, a->r, a->g, a->b, a->a,
                          (unsigned)st->combine_w0, (unsigned)st->combine_w1, (unsigned)st->othermode_l,
                          (unsigned)st->geometry_mode, (void*)0);
            }
        }
    }
#endif
    uint32_t cull = st->geometry_mode & (GM_CULL_FRONT | GM_CULL_BACK);
    if (cull) {
        if (cull == (GM_CULL_FRONT | GM_CULL_BACK)) {
            static int n;
            if (n++ < 3) pm3ds_log(PM3DS_LOG_WARN, "gfx: CULL_BOTH geometry_mode=%08X", (unsigned)st->geometry_mode);
            pm3ds_tstat_culled++;
            return;
        }
        /* the winding of the projected triangle, without dividing by w:
         * det |x y w| over the three vertices = wa*wb*wc * the 2D cross.
         * The determinant's sign is also right when vertices lie behind the
         * camera (it says which side of the triangle's plane the eye is on,
         * and the part in front of the camera, which the GPU clips out, winds
         * the same way), so those are culled too: the RSP clips before it
         * culls. Skipping them drew a camera-facing wall's back over the whole
         * screen when the camera stood just outside a room (GitHub #11). */
        float cross = a->cx * (b->cy * c->cw - b->cw * c->cy) - a->cy * (b->cx * c->cw - b->cw * c->cx) +
                      a->cw * (b->cx * c->cy - b->cy * c->cx);
        /* counter-clockwise (cross > 0) is front-facing */
        if ((cull & GM_CULL_BACK) && cross <= 0.0f) { pm3ds_tstat_culled++; return; }
        if ((cull & GM_CULL_FRONT) && cross >= 0.0f) { pm3ds_tstat_culled++; return; }
    }
    gfx_backend_tri(st, a, b, c);
}

/* ---- RSP: matrices / words / memory ------------------------------------ */

extern int pm3ds_gfx_dbg_vtx;
static void handle_mtx(gfx_state_t *st, const gfx_cmd_t *g) {
    uint32_t param = (g->w0 & 0xFF) ^ 0x01;   /* F3DEX2 stores G_MTX_PUSH inverted */
    uint32_t addr = gfx_seg_resolve(st, g->w1);
    if (!addr_plausible(addr)) { log_bad_addr("G_MTX", g->w1, g); return; }
    float m[4][4];
#ifdef PM3DS_HACK_RUNTIME
    {   /* the N64 layout: 16 big-endian integer halves, then the 16 fraction halves */
        const uint16_t *h = (const uint16_t *)(uintptr_t)addr;
        int32_t fixed[16];
        for (int k = 0; k < 16; k++)
            fixed[k] = (int32_t)((uint32_t)__builtin_bswap16(h[k]) << 16 | __builtin_bswap16(h[16 + k]));
        mtx_from_port(m, fixed);
#ifdef PM3DS_DRAWLOG_FRAME
        {   /* debug */
            extern uint32_t pm3ds_gfx_frame_number(void);
            extern uint32_t pm3ds_drawlog_frame;
            if (pm3ds_gfx_frame_number() == pm3ds_drawlog_frame) {
                const uint32_t *w = (const uint32_t *)h;
                pm3ds_log(PM3DS_LOG_INFO, "MTXRAW p=%02X %08X %08X %08X %08X %08X %08X %08X %08X | %08X %08X",
                          (unsigned)param, (unsigned)__builtin_bswap32(w[0]), (unsigned)__builtin_bswap32(w[1]),
                          (unsigned)__builtin_bswap32(w[2]), (unsigned)__builtin_bswap32(w[3]),
                          (unsigned)__builtin_bswap32(w[4]), (unsigned)__builtin_bswap32(w[5]),
                          (unsigned)__builtin_bswap32(w[6]), (unsigned)__builtin_bswap32(w[7]),
                          (unsigned)__builtin_bswap32(w[8]), (unsigned)__builtin_bswap32(w[9]));
                pm3ds_log(PM3DS_LOG_INFO, "MTX p=%02X [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.1f %.1f %.1f %.3f]",
                          (unsigned)param, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                          m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
            }
        }
#endif
    }
#else
    mtx_from_port(m, (const int32_t *)(uintptr_t)addr);
#ifndef PM3DS_HOST_TEST
    gfx_interp_matrix(g, (param & 0x04) != 0, m);   /* 60 fps: the in-between frame */
#endif
#endif
    if (pm3ds_gfx_dbg_vtx > 0) {
        pm3ds_log(PM3DS_LOG_INFO, "gfx: MTX p=%02X [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.1f %.1f %.1f %.3f]",
                  (unsigned)param, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                  m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
    }
    if (param & 0x04) {                         /* G_MTX_PROJECTION */
        if (param & 0x02) memcpy(st->proj, m, sizeof(m));   /* LOAD */
        else              mtx_mul(st->proj, m, st->proj);  /* MUL */
    } else {
#if defined(PM3DS_DRAWLOG_ALLTRI) && defined(PM3DS_DRAWLOG_FRAME)
        { extern uint32_t pm3ds_gfx_frame_number(void); extern uint32_t pm3ds_drawlog_frame;
          if (pm3ds_gfx_frame_number() == pm3ds_drawlog_frame)
              pm3ds_log(PM3DS_LOG_INFO, "STK mtx p=%02X sp=%d at %p", (unsigned)param, st->mv_sp, (const void *)g); }
#endif
        if ((param & 0x01) && st->mv_sp < GFX_MTX_STACK_DEPTH - 1) {   /* PUSH */
            memcpy(st->mv[st->mv_sp + 1], st->mv[st->mv_sp], sizeof(m));
            st->mv_sp++;
        } else if (param & 0x01) {
            static int warned;
            if (!warned++) pm3ds_log(PM3DS_LOG_WARN, "gfx: modelview stack overflow (depth %d)", GFX_MTX_STACK_DEPTH);
        }
        if (param & 0x02) memcpy(st->mv[st->mv_sp], m, sizeof(m));
        else              mtx_mul(st->mv[st->mv_sp], m, st->mv[st->mv_sp]);
        st->lights_valid = false;
    }
    st->mvp_valid = false;
}

static void handle_popmtx(gfx_state_t *st, const gfx_cmd_t *g) {
    int n = (int)(g->w1 / 64);
#if defined(PM3DS_DRAWLOG_ALLTRI) && defined(PM3DS_DRAWLOG_FRAME)
    { extern uint32_t pm3ds_gfx_frame_number(void); extern uint32_t pm3ds_drawlog_frame;
      if (pm3ds_gfx_frame_number() == pm3ds_drawlog_frame)
          pm3ds_log(PM3DS_LOG_INFO, "STK pop n=%d sp=%d w0=%08X w1=%08X at %p", n, st->mv_sp, (unsigned)g->w0, (unsigned)g->w1, (const void *)g); }
#endif
    st->mv_sp -= n;
    if (st->mv_sp < 0) st->mv_sp = 0;
    st->mvp_valid = false;
    st->lights_valid = false;
}

static void handle_geometrymode(gfx_state_t *st, const gfx_cmd_t *g) {
    { static int n; if (n < 40 && st->load_serial > 50) { n++; pm3ds_log(PM3DS_LOG_INFO, "gfx: GEOMODE w0=%08X w1=%08X at %p", (unsigned)g->w0, (unsigned)g->w1, (const void *)g); } }
    /* w0 low 24 bits = ~clearbits, w1 = setbits */
    st->geometry_mode = (st->geometry_mode & (g->w0 & 0x00FFFFFFu)) | g->w1;
    st->dirty |= GFX_DIRTY_MODE;
}

static void handle_texture(gfx_state_t *st, const gfx_cmd_t *g) {
    st->tex_tile = (uint8_t)bf(g->w0, 8, 3);
    st->tex_levels = (uint8_t)bf(g->w0, 11, 3);
    st->tex_on = (uint8_t)bf(g->w0, 1, 7);
    st->tex_scale_s = (uint16_t)(g->w1 >> 16);
    st->tex_scale_t = (uint16_t)(g->w1 & 0xFFFF);
    st->dirty |= GFX_DIRTY_TEXTURE;
}

#define MW_NUMLIGHT  0x02
#define MW_CLIP      0x04
#define MW_SEGMENT   0x06
#define MW_FOG       0x08
#define MW_LIGHTCOL  0x0A
#define MW_PERSPNORM 0x0E

static void handle_moveword(gfx_state_t *st, const gfx_cmd_t *g) {
    uint32_t index  = bf(g->w0, 16, 8);
    uint32_t offset = g->w0 & 0xFFFF;
    switch (index) {
        case MW_SEGMENT:
            st->segments[(offset / 4) & 0xF] = g->w1;
            break;
        case MW_NUMLIGHT:
            st->num_lights = (int)(g->w1 / 24);
            if (st->num_lights > GFX_MAX_LIGHTS - 1) st->num_lights = GFX_MAX_LIGHTS - 1;
            st->lights_valid = false;
            break;
        case MW_FOG:
            st->fog_mul = (int16_t)(g->w1 >> 16);
            st->fog_offset = (int16_t)(g->w1 & 0xFFFF);
            st->dirty |= GFX_DIRTY_FOG;
            break;
        case MW_LIGHTCOL: {
            int l = (int)(offset / 24);
            if (l < GFX_MAX_LIGHTS && (offset % 24) == 0) {
                st->lights[l].col[0] = (uint8_t)(g->w1 >> 24);
                st->lights[l].col[1] = (uint8_t)(g->w1 >> 16);
                st->lights[l].col[2] = (uint8_t)(g->w1 >> 8);
            }
            break;
        }
        default:
            break;
    }
}

#define MV_VIEWPORT 8
#define MV_LIGHT    10

static void handle_movemem(gfx_state_t *st, const gfx_cmd_t *g) {
    uint32_t index = g->w0 & 0xFF;
    uint32_t ofs = bf(g->w0, 8, 8) * 8;
    uint32_t addr = gfx_seg_resolve(st, g->w1);
    if (!addr_plausible(addr)) return;
    if (index == MV_VIEWPORT) {
        const int16_t *vp = (const int16_t *)(uintptr_t)addr;
#ifdef PM3DS_HACK_RUNTIME
        for (int i = 0; i < 4; i++) {
            st->vp_scale[i] = (int16_t)__builtin_bswap16((uint16_t)vp[i]);
            st->vp_trans[i] = (int16_t)__builtin_bswap16((uint16_t)vp[4 + i]);
        }
#else
        for (int i = 0; i < 4; i++) { st->vp_scale[i] = vp[i]; st->vp_trans[i] = vp[4 + i]; }
#endif
    } else if (index == MV_LIGHT) {
        /* Light_t: u8 col[3], pad, u8 colc[3], pad, s8 dir[3], pad */
        const uint8_t *l = (const uint8_t *)(uintptr_t)addr;
        int slot = (int)(ofs / 24);
        if (slot < 2) {
            memcpy(st->lookat_dir[slot], l + 8, 3);
        } else if (slot - 2 < GFX_MAX_LIGHTS) {
            gfx_light_t *dst = &st->lights[slot - 2];
            memcpy(dst->col, l, 3);
            memcpy(dst->dir, l + 8, 3);
        }
        st->lights_valid = false;
    }
}

/* ---- RDP ---------------------------------------------------------------- */

static void set_othermode(uint32_t *mode, const gfx_cmd_t *g) {
    uint32_t len = bf(g->w0, 0, 8) + 1;
    uint32_t sft = 32 - bf(g->w0, 8, 8) - len;     /* F3DEX2 encoding */
    uint64_t mask = (((uint64_t)1 << len) - 1) << sft;
    *mode = (uint32_t)((*mode & ~mask) | (g->w1 & mask));
}

static void handle_settile(gfx_state_t *st, const gfx_cmd_t *g) {
    gfx_tile_t *t = &st->tiles[bf(g->w1, 24, 3)];
    t->fmt     = (uint8_t)bf(g->w0, 21, 3);
    t->siz     = (uint8_t)bf(g->w0, 19, 2);
    t->line    = (uint16_t)bf(g->w0, 9, 9);
    t->tmem    = (uint16_t)bf(g->w0, 0, 9);
    t->palette = (uint8_t)bf(g->w1, 20, 4);
    t->cmt     = (uint8_t)bf(g->w1, 18, 2);
    t->maskt   = (uint8_t)bf(g->w1, 14, 4);
    t->shiftt  = (uint8_t)bf(g->w1, 10, 4);
    t->cms     = (uint8_t)bf(g->w1, 8, 2);
    t->masks   = (uint8_t)bf(g->w1, 4, 4);
    t->shifts  = (uint8_t)bf(g->w1, 0, 4);
    st->dirty |= GFX_DIRTY_TEXTURE;
}

static void handle_settilesize(gfx_state_t *st, const gfx_cmd_t *g) {
    gfx_tile_t *t = &st->tiles[bf(g->w1, 24, 3)];
    t->uls = (uint16_t)bf(g->w0, 12, 12);
    t->ult = (uint16_t)bf(g->w0, 0, 12);
    t->lrs = (uint16_t)bf(g->w1, 12, 12);
    t->lrt = (uint16_t)bf(g->w1, 0, 12);
    st->dirty |= GFX_DIRTY_TEXTURE;
}

static gfx_tmem_load_t *new_load(gfx_state_t *st, uint16_t tmem) {
    /* Replace a record at the same TMEM address, else the oldest. */
    gfx_tmem_load_t *victim = &st->loads[0];
    for (int i = 0; i < GFX_TMEM_LOADS; i++) {
        if (st->loads[i].serial != 0 && st->loads[i].tmem == tmem) { victim = &st->loads[i]; break; }
        if (st->loads[i].serial < victim->serial) victim = &st->loads[i];
    }
    memset(victim, 0, sizeof(*victim));
    victim->tmem = tmem;
    victim->serial = ++st->load_serial;
    return victim;
}

static void handle_loadblock(gfx_state_t *st, const gfx_cmd_t *g) {
    const gfx_tile_t *t = &st->tiles[bf(g->w1, 24, 3)];
    uint32_t uls = bf(g->w0, 12, 12);
    uint32_t texels = bf(g->w1, 12, 12) + 1;
    uint32_t bits = 4u << st->timg_siz;
    gfx_tmem_load_t *l = new_load(st, t->tmem);
    l->addr = st->timg_addr + (uls * bits) / 8;
    l->img_siz = st->timg_siz;
    l->img_width = st->timg_width;
    l->words = (uint16_t)((texels * bits + 63) / 64);
    l->is_tile = 0;
    st->dirty |= GFX_DIRTY_TEXTURE;
}

static void handle_loadtile(gfx_state_t *st, const gfx_cmd_t *g) {
    const gfx_tile_t *t = &st->tiles[bf(g->w1, 24, 3)];
    gfx_tmem_load_t *l = new_load(st, t->tmem);
    l->addr = st->timg_addr;
    l->img_siz = st->timg_siz;
    l->img_width = st->timg_width;
    l->uls = (uint16_t)bf(g->w0, 12, 12);
    l->ult = (uint16_t)bf(g->w0, 0, 12);
    uint32_t rows = (bf(g->w1, 0, 12) - l->ult) / 4 + 1;
    l->words = (uint16_t)(t->line * rows);
    l->is_tile = 1;
    st->dirty |= GFX_DIRTY_TEXTURE;
}

static void handle_loadtlut(gfx_state_t *st, const gfx_cmd_t *g) {
    const gfx_tile_t *t = &st->tiles[bf(g->w1, 24, 3)];
    int count = (int)bf(g->w1, 14, 10) + 1;
    int base = (int)t->tmem - 256;          /* one TLUT entry per TMEM word */
    if (base < 0 || base >= 256) return;
    if (base + count > 256) count = 256 - base;
    /* a palette is 16-bit entries: it may start on any halfword, and it may be
     * read from the cart (Darth-Koopa's iQue fork: the word-only host check
     * dropped such palette loads) */
    uint32_t ta = st->timg_addr;
    if (!pm3ds_addr_is_rom_tagged(ta) && ((ta & 1) || !addr_plausible(ta & ~3u))) return;
    pm3ds_pi_read(st->timg_addr, &st->tlut[base * 2], (uint32_t)count * 2);
    for (int bank = base / 16; bank <= (base + count - 1) / 16 && bank < 16; bank++) {
        st->tlut_src[bank] = st->timg_addr + (uint32_t)(bank * 16 - base) * 2;
        gfx_tlut_rehash(st, bank);
    }
    st->dirty |= GFX_DIRTY_TEXTURE;
}

extern uint16_t SpriteShadingPalette[16];

static void handle_setcimg(gfx_state_t *st, const gfx_cmd_t *g) {
    st->cimg_addr = g->w1;
    st->cimg_width = (uint16_t)(bf(g->w0, 0, 12) + 1);
    /* Sprite shading renders a 16x1 RGBA16 palette with the RDP
     * (sprite_shading.c); there is no RDP here, so handle_texrect computes
     * it on the CPU while this target is bound. */
    st->cimg_shading_pal = gfx_seg_resolve(st, g->w1) == (uint32_t)(uintptr_t)SpriteShadingPalette;
    /* Only the 320-wide main framebuffer is drawn; the z-buffer clear and
     * render-to-texture targets (32x64 flame buffers, captures) are skipped,
     * except draw_box's tagged window targets, which get a GPU texture. */
    bool copy = st->screen_copy_tag;   /* an image that gets a copy of the screen */
    st->screen_copy_tag = false;
    bool screen = !copy && st->cimg_width == 320 && g->w1 != st->zimg_addr && !st->cimg_shading_pal;
    bool rtt = !screen && st->box_rtt_tag;
    st->box_rtt_tag = false;
    if (copy) gfx_backend_screen_copy(st, gfx_seg_resolve(st, g->w1), st->cimg_width);
    if (st->cimg_rtt) {
        gfx_backend_rtt_end(st);
        st->cimg_rtt = false;
    }
    if (rtt) st->cimg_rtt = gfx_backend_rtt_begin(st, gfx_seg_resolve(st, g->w1), st->cimg_width);
    if (screen != st->cimg_is_screen) {
        gfx_backend_flush(st);
        st->cimg_is_screen = screen;
    }
}

/* ---- rectangles ----------------------------------------------------------- */

static void handle_fillrect(gfx_state_t *st, const gfx_cmd_t *g) {
    float x1 = bf(g->w0, 12, 12) / 4.0f, y1 = bf(g->w0, 0, 12) / 4.0f;
    float x0 = bf(g->w1, 12, 12) / 4.0f, y0 = bf(g->w1, 0, 12) / 4.0f;
    uint32_t cyc = st->othermode_h & CYC_MASK;
    if (cyc == CYC_FILL || cyc == CYC_COPY) { x1 += 1.0f; y1 += 1.0f; }   /* inclusive */
    if (x1 <= x0 || y1 <= y0) return;   /* the RDP draws nothing for an inverted rectangle */
    /* Wide view: the black side borders after the backdrop (background_gfx.c)
     * would cover its continuation to the screen edges */
    if (st->bg_wide && cyc == CYC_FILL && (x1 <= st->bg_left + 0.5f || x0 >= st->bg_right - 0.5f)) return;
#ifdef PM3DS_OVERLAY_DBG
    extern uint32_t pm3ds_gfx_frame_number(void);
    if (x1 - x0 >= 280 && y1 - y0 >= 180 && st->cimg_is_screen && pm3ds_gfx_frame_number() > PM3DS_OVERLAY_DBG) {
        static uint32_t seen[16]; static int ns;
        uint32_t k = st->combine_w0 ^ st->combine_w1 ^ (st->prim[0] << 24 | st->prim[1] << 16 | st->prim[2] << 8 | st->prim[3]) ^ cyc;
        int i; for (i = 0; i < ns && seen[i] != k; i++) {}
        if (i == ns && ns < 16) {
            seen[ns++] = k;
            pm3ds_log(PM3DS_LOG_INFO, "BIGRECT %.0f,%.0f-%.0f,%.0f cyc=%u fill=%08X prim=%02X%02X%02X%02X env=%02X%02X%02X%02X cc=%06X:%08X oml=%08X",
                      x0, y0, x1, y1, (unsigned)(cyc >> 20), (unsigned)st->fill_color, st->prim[0], st->prim[1], st->prim[2], st->prim[3],
                      st->env[0], st->env[1], st->env[2], st->env[3], (unsigned)st->combine_w0, (unsigned)st->combine_w1,
                      (unsigned)st->othermode_l);
        }
    }
#endif
    gfx_backend_rect(st, x0, y0, x1, y1, 0, 0, 0, 0, false, false);
}

/* PM_CC_55 = (PRIM - ENV) * TEXEL0_ALPHA + ENV, alpha 1, sampled one TLUT
 * entry per pixel from bank 0: each shaded entry is the shadow (prim) colour
 * where the sprite's entry is opaque and the highlight (env) colour where it
 * is not. Written big-endian, like every palette the renderer reads. */
static void emulate_shading_palette(const gfx_state_t *st) {
    uint8_t *out = (uint8_t *)SpriteShadingPalette;
    for (int i = 0; i < 16; i++) {
        int opaque = st->tlut[i * 2 + 1] & 1;
        const uint8_t *c = opaque ? st->prim : st->env;
        uint16_t v = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | 1);
        out[i * 2] = (uint8_t)(v >> 8);
        out[i * 2 + 1] = (uint8_t)v;
    }
}

#ifndef PM3DS_HOST_TEST
/* x0..x1 of a backdrop line from s0 on, wrapping s at the image width */
static void bg_wrapped_strip(gfx_state_t *st, float x0, float x1, float y0, float y1, float s0, float t0, float t1,
                             float dsdx) {
    float wrap = (st->bg_right - st->bg_left) * 32.0f;   /* s10.5 units per image width */
    if (wrap <= 0.0f) return;
    while (s0 < 0.0f) s0 += wrap;
    while (s0 >= wrap) s0 -= wrap;
    for (int guard = 0; x0 < x1 && guard < 4; guard++) {
        float x = x0 + (wrap - s0) / (dsdx * 32.0f);
        if (x > x1) x = x1;
        gfx_backend_rect(st, x0, y0, x, y1, s0, t0, s0 + (x - x0) * dsdx * 32.0f, t1, false, true);
        x0 = x;
        s0 = 0.0f;
    }
}
#endif

static void handle_texrect(gfx_state_t *st, const gfx_cmd_t *g, bool flip) {
    const gfx_cmd_t *h1 = g + 1, *h2 = g + 2;
    if (st->cimg_shading_pal) {
        emulate_shading_palette(st);
        return;
    }
    float xh = bf(g->w0, 12, 12) / 4.0f, yh = bf(g->w0, 0, 12) / 4.0f;
    float xl = bf(g->w1, 12, 12) / 4.0f, yl = bf(g->w1, 0, 12) / 4.0f;
    int tile = (int)bf(g->w1, 24, 3);
    float s = (int16_t)(h1->w1 >> 16), t = (int16_t)(h1->w1 & 0xFFFF);   /* s10.5 */
    float dsdx = (int16_t)(h2->w1 >> 16) / 1024.0f;                        /* s5.10 */
    float dtdy = (int16_t)(h2->w1 & 0xFFFF) / 1024.0f;
    uint32_t cyc = st->othermode_h & CYC_MASK;
    if (cyc == CYC_COPY) { dsdx /= 4.0f; xh += 1.0f; yh += 1.0f; }
    else if (cyc == CYC_FILL) { xh += 1.0f; yh += 1.0f; }
#ifndef PM3DS_HOST_TEST
    if (st->bg_shift != 0.0f && !flip) {
        /* 60 fps (gfx_interp.c): the backdrop half way between steps. The
         * strips shift; the one at an edge keeps that edge and covers the gap
         * with the texels that wrap around to it. */
        float d = st->bg_shift;
        if (d > 0.0f && xl <= st->bg_left + 0.5f) {
            s -= d * dsdx * 32.0f;
            xh += d;
        } else if (d < 0.0f && xh >= st->bg_right - 0.5f) {
            xl += d;
        } else {
            xl += d;
            xh += d;
        }
    }
#endif
    float w = xh - xl, h = yh - yl;
    /* the RDP draws nothing for an inverted rectangle: a negative left edge
     * wraps to ~1020 in the 12-bit field (menu items sliding off-screen) */
    if (w <= 0.0f || h <= 0.0f) return;
    /* texture coordinates in s10.5 units at the far corners */
    float s1, t1;
    if (!flip) { s1 = s + dsdx * w * 32.0f; t1 = t + dtdy * h * 32.0f; }
    else       { s1 = s + dsdx * h * 32.0f; t1 = t + dtdy * w * 32.0f; }
    uint8_t saved = st->tex_tile;
    st->tex_tile = (uint8_t)tile;
    gfx_backend_rect(st, xl, yl, xh, yh, s, t, s1, t1, flip, true);
#ifndef PM3DS_HOST_TEST
    if (st->bg_wide && !flip && dsdx > 0.0f) {
        /* Wide view: the backdrop wraps around horizontally, so the strips at
         * its edges go on to the screen edges with the texels that follow */
        if (xl <= st->bg_left + 0.5f && st->bg_wide_l < xl)
            bg_wrapped_strip(st, st->bg_wide_l, xl, yl, yh, s - (xl - st->bg_wide_l) * dsdx * 32.0f, t, t1, dsdx);
        if (xh >= st->bg_right - 0.5f && st->bg_wide_r > xh)
            bg_wrapped_strip(st, xh, st->bg_wide_r, yl, yh, s1, t, t1, dsdx);
    }
#endif
    st->tex_tile = saved;
}

/* ---- the walker ------------------------------------------------------------ */

unsigned pm3ds_tstat_ops;
unsigned pm3ds_tstat_tris;
unsigned pm3ds_tstat_rects;
unsigned pm3ds_tstat_culldl, pm3ds_tstat_culldl_hit;   /* G_CULLDL seen / lists ended by it */
unsigned pm3ds_tstat_npcs_culled;   /* NPCs not drawn because they are off screen (src/npc.c) */
unsigned pm3ds_tstat_cullcheck_visible;   /* PM3DS_CULL_VERIFY: such NPCs' triangles that were on screen */


/* G_CULLDL: true when loaded vertices v0..v1 all lie outside one side of what is on screen
 * (the RSP ends the display list then). Each test is a half-space in clip space, so the
 * answer holds for vertices behind the camera too. Wide mode shows past NDC x = +-1; the
 * margin covers the 3D slider's per-eye shift and the 60 fps in-between frames. */
static bool culldl_offscreen(const gfx_state_t *st, int v0, int v1) {
    const float half = st->vp_scale[0] * 0.25f, centre = st->vp_trans[0] * 0.25f;
    const float kx = pm3ds_view_ndc_x(centre - half, half * 2.0f) + 0.15f, ky = 1.15f;
    unsigned all = 0x1F;
    for (int i = v0; i <= v1 && all; i++) {
        const gfx_vtx_t *v = &st->vtx[i];
        unsigned code = 0;
        if (v->cx >  kx * v->cw) code |= 0x01;
        if (v->cx < -kx * v->cw) code |= 0x02;
        if (v->cy >  ky * v->cw) code |= 0x04;
        if (v->cy < -ky * v->cw) code |= 0x08;
        if (v->cz < -v->cw)      code |= 0x10;   /* in front of the near plane */
        all &= code;
    }
    return all != 0;
}

#define DL_STACK 32
#define WALK_MAX_OPS 400000

static const gfx_cmd_t *s_last_dl_from;
static uint32_t s_last_dl_to;

int gfx_walk_dl(gfx_state_t *st, const void *dl) {
    const gfx_cmd_t *stack[DL_STACK];
    int sp = 0;
    const gfx_cmd_t *g = (const gfx_cmd_t *)dl;
    uint32_t rdphalf1 = 0;
    long budget = WALK_MAX_OPS;
    int n = 0;
    static int runaway_logged = 0;

    /* each list is its own RSP task, which starts with an empty matrix
     * stack: an unbalanced push in one frame must not leak into the next
     * (it filled the stack and later PUSH|MUL draws, e.g. the Save Block,
     * multiplied onto a stale matrix and covered the screen) */
    st->mv_sp = 0;
    memset(st->mv[0], 0, sizeof(st->mv[0]));   /* ...and starts from an identity modelview: */
    st->mv[0][0][0] = st->mv[0][1][1] = st->mv[0][2][2] = st->mv[0][3][3] = 1.0f;   /* effects MUL onto it */
    st->mvp_valid = false;
    st->lights_valid = false;

    while (g) {
        if (--budget <= 0) {
            if (runaway_logged++ < 4) pm3ds_log(PM3DS_LOG_WARN, "gfx: runaway DL (no G_ENDDL in %d ops)", WALK_MAX_OPS);
            break;
        }
        if (!addr_plausible((uint32_t)(uintptr_t)g)) break;
        const gfx_cmd_t *gmem = g;      /* the command in memory; g is how it reads */
#ifdef PM3DS_HACK_RUNTIME
        gfx_cmd_t dec[3];              /* big-endian words; texrect reads two more */
        for (int k = 0; k < 3; k++) {
            dec[k].w0 = __builtin_bswap32(gmem[k].w0);
            dec[k].w1 = __builtin_bswap32(gmem[k].w1);
        }
        g = dec;
#endif
        uint32_t op = g->w0 >> 24;
        PROF_OP(op);
        PROF_PHASE(PH_WALK);
        n++;
        pm3ds_tstat_ops++;
        switch (op) {
            /* no GP_ tick timers here: two svcGetSystemTick (~1.5 us on an
             * Old 3DS) per command inflated these; the sampler covers them */
            case OP_VTX:
                handle_vtx(st, g);
                break;
            case OP_TRI1:
                emit_tri(st, bf(g->w0, 16, 8) / 2, bf(g->w0, 8, 8) / 2, bf(g->w0, 0, 8) / 2);
                pm3ds_tstat_tris++;
                break;
            case OP_TRI2:
            case OP_QUAD:
                emit_tri(st, bf(g->w0, 16, 8) / 2, bf(g->w0, 8, 8) / 2, bf(g->w0, 0, 8) / 2);
                emit_tri(st, bf(g->w1, 16, 8) / 2, bf(g->w1, 8, 8) / 2, bf(g->w1, 0, 8) / 2);
                pm3ds_tstat_tris += 2;
                break;
            case OP_MTX:        handle_mtx(st, g); break;
            case OP_POPMTX:     handle_popmtx(st, g); break;
            case OP_GEOMETRYMODE: handle_geometrymode(st, g); break;
            case OP_TEXTURE:    handle_texture(st, g); break;
            case OP_MOVEWORD:   handle_moveword(st, g); break;
            case OP_MOVEMEM:    handle_movemem(st, g); break;
            case OP_DL: {
                uint32_t addr = gfx_seg_resolve(st, g->w1);
                if (!addr_plausible(addr)) { log_bad_addr("G_DL", g->w1, g); break; }
                if (bf(g->w0, 16, 8) == 0) {          /* G_DL_PUSH: call */
                    if (sp >= DL_STACK) break;
                    stack[sp++] = gmem + 1;
                }
                s_last_dl_from = gmem;
                s_last_dl_to = addr;
                g = (const gfx_cmd_t *)(uintptr_t)addr;
                continue;
            }
            case OP_BRANCH_Z: {
                /* LOD branch; always take the near (detailed) list */
                uint32_t addr = gfx_seg_resolve(st, rdphalf1);
                if (addr_plausible(addr)) { g = (const gfx_cmd_t *)(uintptr_t)addr; continue; }
                break;
            }
            case OP_ENDDL:
                if (sp == 0) { g = NULL; continue; }
                g = stack[--sp];
                continue;
            case OP_NOOP:
                if (g->w1 == PM3DS_GFX_TAG_BOX_RTT) st->box_rtt_tag = true;
                if (g->w1 == PM3DS_GFX_TAG_SCREEN_COPY) st->screen_copy_tag = true;
#ifdef PM3DS_CULL_VERIFY
                if (g->w1 == PM3DS_GFX_TAG_CULL_CHECK) st->cull_check = true;
                if (g->w1 == PM3DS_GFX_TAG_CULL_CHECK_END) st->cull_check = false;
#endif
                if (g->w1 == PM3DS_GFX_TAG_DECAL_BAND || g->w1 == PM3DS_GFX_TAG_DECAL_BAND_END) {
                    bool band = g->w1 == PM3DS_GFX_TAG_DECAL_BAND;
                    if (st->decal_band != band) {
                        st->decal_band = band;
                        st->dirty |= GFX_DIRTY_MODE;
                    }
                }
                break;
            case OP_MODIFYVTX:
                handle_modifyvtx(st, g);
                break;
            case OP_CULLDL: {
                int v0 = (int)bf(g->w0, 0, 16) / 2, v1 = (int)bf(g->w1, 0, 16) / 2;
                pm3ds_tstat_culldl++;
#ifdef PM3DS_NO_OFFSCREEN_CULL   /* debug A/B: draw every list, as before */
                break;
#endif
                if (v0 <= v1 && v1 < GFX_VTX_POOL_SIZE && culldl_offscreen(st, v0, v1)) {
                    pm3ds_tstat_culldl_hit++;
                    if (sp == 0) { g = NULL; continue; }   /* as G_ENDDL */
                    g = stack[--sp];
                    continue;
                }
                break;
            }
            case OP_SPNOOP:
            case OP_LOAD_UCODE: case OP_DMA_IO: case OP_SPECIAL_1: case OP_SPECIAL_2: case OP_SPECIAL_3:
            case OP_RDPLOADSYNC: case OP_RDPPIPESYNC: case OP_RDPTILESYNC: case OP_RDPFULLSYNC:
            case OP_RDPHALF_2:
                break;
            /* chroma key and YUV convert registers: no keying or YUV here, but the
             * combiner reads CENTER/SCALE and K4/K5 as constants */
            case OP_SETKEYR:
                st->key_center[0] = (uint8_t)(g->w1 >> 8);
                st->key_scale[0] = (uint8_t)g->w1;
                st->dirty |= GFX_DIRTY_COMBINE;
                break;
            case OP_SETKEYGB:
                st->key_center[1] = (uint8_t)(g->w1 >> 24);
                st->key_scale[1] = (uint8_t)(g->w1 >> 16);
                st->key_center[2] = (uint8_t)(g->w1 >> 8);
                st->key_scale[2] = (uint8_t)g->w1;
                st->dirty |= GFX_DIRTY_COMBINE;
                break;
            case OP_SETCONVERT:
                st->k4 = (int16_t)((int32_t)(g->w1 << 14) >> 23);   /* 9-bit signed, bits 17..9 */
                st->k5 = (int16_t)((int32_t)(g->w1 << 23) >> 23);   /* bits 8..0 */
                st->dirty |= GFX_DIRTY_COMBINE;
                break;
            case OP_RDPHALF_1:  rdphalf1 = g->w1; break;

            case OP_SETOTHERMODE_H: set_othermode(&st->othermode_h, g); st->dirty |= GFX_DIRTY_MODE | GFX_DIRTY_TEXTURE; break;
            case OP_SETOTHERMODE_L: set_othermode(&st->othermode_l, g); st->dirty |= GFX_DIRTY_MODE; break;
            case OP_RDPSETOTHERMODE:
                st->othermode_h = g->w0 & 0x00FFFFFFu;
                st->othermode_l = g->w1;
                st->dirty |= GFX_DIRTY_MODE | GFX_DIRTY_TEXTURE;
                break;
            case OP_SETCOMBINE:
                st->combine_w0 = g->w0 & 0x00FFFFFFu;
                st->combine_w1 = g->w1;
                st->dirty |= GFX_DIRTY_COMBINE;
                break;
            case OP_SETPRIMCOLOR:
                st->prim_lod_frac = (uint8_t)(g->w0 & 0xFF);
                st->prim[0] = (uint8_t)(g->w1 >> 24); st->prim[1] = (uint8_t)(g->w1 >> 16);
                st->prim[2] = (uint8_t)(g->w1 >> 8);  st->prim[3] = (uint8_t)g->w1;
                st->dirty |= GFX_DIRTY_COLORS;
                break;
            case OP_SETENVCOLOR:
                st->env[0] = (uint8_t)(g->w1 >> 24); st->env[1] = (uint8_t)(g->w1 >> 16);
                st->env[2] = (uint8_t)(g->w1 >> 8);  st->env[3] = (uint8_t)g->w1;
                st->dirty |= GFX_DIRTY_COLORS;
                break;
            case OP_SETBLENDCOLOR:
                st->blend[0] = (uint8_t)(g->w1 >> 24); st->blend[1] = (uint8_t)(g->w1 >> 16);
                st->blend[2] = (uint8_t)(g->w1 >> 8);  st->blend[3] = (uint8_t)g->w1;
                st->dirty |= GFX_DIRTY_MODE;
                break;
            case OP_SETFOGCOLOR:
                st->fog[0] = (uint8_t)(g->w1 >> 24); st->fog[1] = (uint8_t)(g->w1 >> 16);
                st->fog[2] = (uint8_t)(g->w1 >> 8);  st->fog[3] = (uint8_t)g->w1;
                st->dirty |= GFX_DIRTY_FOG;
                break;
            case OP_SETFILLCOLOR: st->fill_color = g->w1; st->dirty |= GFX_DIRTY_COLORS; break;
            case OP_SETPRIMDEPTH: st->prim_depth = (uint16_t)(g->w1 >> 16); break;
            case OP_SETSCISSOR:
                st->scissor.ulx = (uint16_t)bf(g->w0, 12, 12);
                st->scissor.uly = (uint16_t)bf(g->w0, 0, 12);
                st->scissor.lrx = (uint16_t)bf(g->w1, 12, 12);
                st->scissor.lry = (uint16_t)bf(g->w1, 0, 12);
                st->dirty |= GFX_DIRTY_MODE;
                break;
            case OP_SETTIMG:
                st->timg_fmt = (uint8_t)bf(g->w0, 21, 3);
                st->timg_siz = (uint8_t)bf(g->w0, 19, 2);
                st->timg_width = (uint16_t)(bf(g->w0, 0, 12) + 1);
                st->timg_addr = gfx_seg_resolve(st, g->w1);
                break;
            case OP_SETTILE:     handle_settile(st, g); break;
            case OP_SETTILESIZE: handle_settilesize(st, g); break;
            case OP_LOADBLOCK:   handle_loadblock(st, g); break;
            case OP_LOADTILE:    handle_loadtile(st, g); break;
            case OP_LOADTLUT:    handle_loadtlut(st, g); break;
            case OP_SETZIMG:     st->zimg_addr = g->w1; break;
            case OP_SETCIMG:     handle_setcimg(st, g); break;
            case OP_FILLRECT:    handle_fillrect(st, g); pm3ds_tstat_rects++; break;
            case OP_TEXRECT:
            case OP_TEXRECTFLIP:
                handle_texrect(st, g, op == OP_TEXRECTFLIP);
                pm3ds_tstat_rects++;
                g = gmem + 3;
                continue;
            default: {
                static uint32_t warned[8];
                if (!(warned[op >> 5] & (1u << (op & 31)))) {
                    warned[op >> 5] |= 1u << (op & 31);
                    pm3ds_log(PM3DS_LOG_WARN, "gfx: unhandled opcode 0x%02X (w0=%08X)", (unsigned)op, (unsigned)g->w0);
                    static int traced;
                    if (traced++ < 3) {
                        pm3ds_log(PM3DS_LOG_WARN, "gfx:   at %p root=%p last G_DL %p (w1=%08X) -> %08X sp=%d",
                                  (const void*)g, dl, (const void*)s_last_dl_from,
                                  s_last_dl_from ? (unsigned)s_last_dl_from->w1 : 0u, (unsigned)s_last_dl_to, sp);
                        for (int k = sp - 1; k >= 0; k--) {
                            pm3ds_log(PM3DS_LOG_WARN, "gfx:   ret[%d] %p", k, (const void*)stack[k]);
                        }
                    }
                }
                break;
            }
        }
        g = gmem + 1;
    }
    if (st->mv_sp != 0) {   /* harmless now (reset per list), but worth knowing */
        static int noted;
        if (noted++ < 3) pm3ds_log(PM3DS_LOG_INFO, "gfx: list %p ended with %d matrices pushed", dl, st->mv_sp);
    }
    return n;
}
