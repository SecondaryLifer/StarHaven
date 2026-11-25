/*
 * gfx_present.c — PICA200 backend for the F3DEX2 interpreter (M12 rewrite).
 *
 * Owns the render target, the vertex shader, the per-frame vertex buffer
 * and all GPU render state. The translator (gfx_translate.c) hands it
 * transformed triangles and screen rectangles; this file decodes the RDP
 * state that governs them (cycle type, render mode -> depth/blend/alpha
 * compare/fog, scissor, combiner, textures) and batches draws until that
 * state changes.
 *
 * Coordinates: vertices arrive as homogeneous N64 PIXEL coordinates
 * (x 0..320, y 0..240 y-down, z = clip z, w = clip w). The shader's fixed
 * `screen` matrix pillarboxes that into the 400x240 top screen, applies the
 * 90-degree framebuffer rotation, and maps N64 z/w in [-1,1] to the PICA
 * depth range [-1,0] (near -> -1).
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>
#include "vshader_shbin.h"

#include "pm3ds_log.h"
#include "gfx_interp.h"
#include "gfx_prof.h"
#include "pm3ds_settings.h"
#include "gfx_state.h"

unsigned pm3ds_gfx_task_count(void);
extern const void *pm3ds_gfx_drain_dl_list(const void **out, int max, int *count);

/* othermode_l */
#define RM_AC_MASK       0x3u
#define RM_AC_THRESHOLD  0x1u
#define RM_ZS_PRIM       0x4u
#define RM_Z_CMP         0x10u
#define RM_Z_UPD         0x20u
#define RM_ZMODE_DEC     0xC00u
#define RM_CVG_X_ALPHA   0x1000u
#define RM_ALPHA_CVG_SEL 0x2000u
#define RM_FORCE_BL      0x4000u
/* othermode_h */
#define OMH_CYC_MASK   (3u << 20)
#define OMH_CYC_2      (1u << 20)
#define OMH_CYC_COPY   (2u << 20)
#define OMH_CYC_FILL   (3u << 20)
#define OMH_TF_MASK    (3u << 12)
#define OMH_TF_POINT   0u
/* geometry mode */
#define GM_ZBUFFER 0x1u
#define GM_FOG     0x10000u

#define SCREEN_X_OFFSET 40.0f       /* (400 - 320) / 2 pillarbox */
#define PM3DS_VIEW_ORIGINAL 0       /* pixel-exact 320x240, pillarboxed */
#define PM3DS_VIEW_FILL     1       /* TV-safe box scaled 1.2 to full height */
#define PM3DS_VIEW_WIDE     2       /* Fill + the world drawn to the screen edges */
#define PM3DS_VIEW_SHARP    3       /* Original's 1:1 pixels + the world drawn to the screen edges */

typedef struct {
    float x, y, z, w;
    uint8_t r, g, b, a;
    float u0, v0, u1, v1;
} gpu_vtx_t;

#define FRAME_MAX_VERTS (96 * 1024)
#define FRAME_MAX_IDX   (128 * 1024)

static C3D_RenderTarget *g_target, *g_target_right;
static DVLB_s          *g_dvlb;
static shaderProgram_s  g_prog;
static int              g_uloc_screen = -1;
/* NOISE combiner input: a random texture on unit 1 (when TEXEL1 is unused),
 * addressed by the vertex's N64 pixel position (one texel per pixel) plus an
 * offset that moves every frame. Texture coordinates come from the CPU: a
 * texcoord2 output from the shader hung the GPU on real hardware. */
static C3D_Tex          g_noise_tex;
static bool             g_noise_ok;
static bool             g_noise_uv;       /* the open batch samples noise on unit 1 */
static float            g_noise_ofs_u, g_noise_ofs_v;
static C3D_Mtx          g_screen_mtx;

/* N64 pixel (x, y) -> top-screen pixel (s*x + ox, s*y + oy). Paper Mario only
 * draws inside the 296x200 TV-safe box (12..308, 20..220) of its 320x240
 * frame, so "fill" scales that box by 1.2 to the full 240-pixel height;
 * "original" is the pixel-exact 320x240 image pillarboxed into 400. */
static float g_view_s = 1.0f, g_view_ox = SCREEN_X_OFFSET, g_view_oy = 0.0f;
static int   g_view_mode;   /* PM3DS_VIEW_* */

int pm3ds_view_mode(void) { return g_view_mode; }

/* the 3D world past the 4:3 edges: Wide (scaled 1.2) and Sharp wide (1:1) */
static inline bool view_wide(void) { return g_view_mode == PM3DS_VIEW_WIDE || g_view_mode == PM3DS_VIEW_SHARP; }

/* How far past +-1 a camera's NDC x reaches on screen: Wide shows N64 x from
 * -ox/s to (400-ox)/s, beyond the viewport. 1 in the 4:3 modes (the scissor
 * stops at the viewport there). For the game's model culling (model.c). */
float pm3ds_view_ndc_x(float vp_x, float vp_w) {
    if (!view_wide() || vp_w <= 0.0f) return 1.0f;
    float xl = -g_view_ox / g_view_s, xr = (400.0f - g_view_ox) / g_view_s;
    float h = vp_w * 0.5f, c = vp_x + h;
    float m = (c - xl) / h, r = (xr - c) / h;
    if (r > m) m = r;
    return m > 1.0f ? m : 1.0f;
}

/* Wide = Fill's 1.2 scale, plus the 3D world shown past the 4:3 edge (Sharp
 * wide: the same at Original's 1:1, so text and HUD stay pixel-exact): the
 * full-width scissor is opened up and full-width rectangles (clears,
 * backgrounds, fades) are stretched to the screen edges with matching
 * texture coordinates. The game draws whole maps, so the extra width is
 * real scenery; things culled by the game near the edges may pop in. */
void pm3ds_set_view_mode(int mode) {
    if (mode < PM3DS_VIEW_ORIGINAL || mode > PM3DS_VIEW_SHARP) mode = PM3DS_VIEW_WIDE;
    g_view_mode = mode;
    bool fill = mode == PM3DS_VIEW_FILL || mode == PM3DS_VIEW_WIDE;   /* the 1.2 scale */
    g_view_s  = fill ? 1.2f : 1.0f;
    g_view_ox = 200.0f - 160.0f * g_view_s;
    g_view_oy = 120.0f - 120.0f * g_view_s;
    /* pixel-space homogeneous -> tilted PICA clip space (see header). Rows
     * act on (x, y, z, w), with screen x' = s*x + ox*w, y' = s*y + oy*w:
     *   out.x = w - y'/120          (screen y 0..240 -> fb x)
     *   out.y = w - x'/200          (screen x 0..400)
     *   out.z = 0.5 z - 0.5 w       (N64 [-1,1] -> PICA [-1,0], near -> -1)
     */
    Mtx_Zeros(&g_screen_mtx);
    g_screen_mtx.r[0].y = -g_view_s / 120.0f; g_screen_mtx.r[0].w = 1.0f - g_view_oy / 120.0f;
    g_screen_mtx.r[1].x = -g_view_s / 200.0f; g_screen_mtx.r[1].w = 1.0f - g_view_ox / 200.0f;
    g_screen_mtx.r[2].z = 0.5f;               g_screen_mtx.r[2].w = -0.5f;
    g_screen_mtx.r[3].w = 1.0f;
}
static C3D_AttrInfo     g_attr;
static C3D_BufInfo      g_buf;
static gpu_vtx_t       *g_vbuf;
static int              g_vcount;       /* vertices written this frame */
static uint16_t        *g_ibuf;         /* triangles as indices into g_vbuf + g_vbase */
static int              g_icount;       /* indices written this frame */
static int              g_batch_start;  /* first index of the open batch */
static int              g_vbase;        /* the attribute buffer's start (16-bit indices reach 64K past it) */
/* Each pool vertex (G_VTX) is converted to a GPU vertex once and shared by
 * the triangles that use it, while its serial and the draw state that went
 * into its texture coordinates (g_vc_epoch) stay the same. */
static uint16_t         g_vc_idx[GFX_VTX_POOL_SIZE];
static uint32_t         g_vc_serial[GFX_VTX_POOL_SIZE], g_vc_ep[GFX_VTX_POOL_SIZE];
static uint32_t         g_vc_epoch = 1;
static C3D_FogLut       g_fog_lut;
static bool             g_fog_lut_bound;   /* citro3d uploads all 128 entries on every bind: only when it changed */
static int16_t          g_fog_lut_mul = 0x7FFF, g_fog_lut_ofs = 0x7FFF;

/* Texture parameters the open batch was set up with (for UV generation). */
static struct {
    bool on;
    const gfx_tile_t *tile;
    float w, h;          /* POT-padded GPU texture size */
    float off_s, off_t;  /* texel origin of the tile in that texture (window texture) */
    bool screen;         /* sampling the screen copy: N64 pixel -> render-target texel */
    float us, ut, ub, vs, vt, vb;   /* u = s*us + t*ut + ub, v likewise (tex_coeffs) */
} g_tex[2];
static int  g_applied_tile = -1;
static bool g_applied_rect;
static bool g_state_valid;
static bool g_draw_enabled;

static gfx_state_t g_st;
static bool        g_st_inited;
static uint32_t    g_frame_no;

/* draw_box window contents (render-to-texture). One texture, reused by
 * every window in turn: the GPU runs the draws in order, and rebinding the
 * texture clears its cache. N64 pixel (x, y) -> texel (x, y), row 0 at v = 0
 * like every uploaded texture. */
#define RTT_W 512
#define RTT_H 256
static C3D_Tex           g_rtt_tex;
static C3D_RenderTarget *g_rtt_target;
static C3D_Mtx           g_rtt_mtx;
static bool              g_in_rtt;
static uint32_t          g_rtt_addr, g_rtt_stride, g_rtt_bytes;   /* the image it stands for */
static C3D_RenderTarget *g_cur_target;                            /* the eye being drawn */

/* Screen copy (PM3DS_GFX_TAG_SCREEN_COPY): the eye's render target as it is
 * at that point of the frame, copied into a texture. The render target is
 * 240x400 (rotated) in 8x8 tiles; the copy keeps each row of tiles and pads
 * it to the texture's 256-texel width. */
#define SCR_W 256
#define SCR_H 512
static C3D_Tex  g_scr_tex;
static bool     g_scr_ok;
static uint32_t g_scr_addr, g_scr_stride, g_scr_bytes;   /* the N64 image it stands for */

/* Previous frame (pm3ds_request_prev_frame): the same kind of copy, made at
 * the start of a frame while the render target still holds the last one. The
 * battle-start blur, Dizzy Dial and fireworks draw it every frame; a copy on
 * the CPU each time (wait for the GPU, convert 76800 pixels, re-upload them as
 * textures) slowed battle starts down on hardware. */
static C3D_Tex  g_prev_tex;
static bool     g_prev_ok;
static uint16_t *g_prev_req;                 /* asked for by this game step */
static uint32_t g_prev_addr, g_prev_bytes;   /* the N64 image it stands for */
static int      g_prev_ttl;                  /* presents it stays mapped (60 fps in-between frames) */

/* Top screen for the bottom screen (the target picker's stage picture): the
 * same copy as g_prev_tex while pm3ds_gfx_request_top_copy() keeps asking, every
 * frame on a New 3DS and every 4th on an Old 3DS (its bottom screen redraws that
 * often). In VRAM when there is room (the fast copy), else linear memory;
 * allocated the first time it is asked for. */
static C3D_Tex  g_top_tex;
static bool     g_top_ok, g_top_tried, g_top_valid;
static int      g_top_want;

void pm3ds_gfx_request_top_copy(void) { g_top_want = 3; }

/* the last frame's top screen (left eye), or NULL: 240x400 rotated in a
 * 256x512 texture, top-screen pixel (X, Y) at u = (240 - Y) / 256, v = (512 - X) / 512 */
C3D_Tex *pm3ds_gfx_top_copy(void) { return g_top_valid ? &g_top_tex : NULL; }

/* N64 screen pixel -> top-screen pixel, as the current view mode places it */
void pm3ds_gfx_n64_to_top(float x, float y, float *tx, float *ty) {
    *tx = g_view_s * x + g_view_ox;
    *ty = g_view_s * y + g_view_oy;
}

/* per-frame stats */
static unsigned g_stat_draws, g_stat_verts;
extern unsigned pm3ds_tstat_ops, pm3ds_tstat_tris, pm3ds_tstat_rects, pm3ds_tstat_culled;
extern unsigned pm3ds_tstat_culldl, pm3ds_tstat_culldl_hit, pm3ds_tstat_npcs_culled;

void pm3ds_gfx_present_init(void) {
    g_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(g_target, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                              GX_TRANSFER_RAW_COPY(0) |
                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                              GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    /* right-eye target for the 3D slider (only drawn while it is up) */
    g_target_right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(g_target_right, GFX_TOP, GFX_RIGHT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                              GX_TRANSFER_RAW_COPY(0) |
                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                              GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    g_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&g_prog);
    shaderProgramSetVsh(&g_prog, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_prog);
    g_uloc_screen = shaderInstanceGetUniformLocation(g_prog.vertexShader, "screen");
    /* 64x64 random intensities, one texel per N64 pixel, like the RDP's
     * per-pixel noise */
    if (C3D_TexInit(&g_noise_tex, 64, 64, GPU_L8)) {
        uint8_t *px = (uint8_t *)linearAlloc(64 * 64);
        if (px) {
            uint32_t r = 0x12345678u;
            for (int i = 0; i < 64 * 64; i++) { r = r * 1664525u + 1013904223u; px[i] = (uint8_t)(r >> 24); }
            C3D_TexUpload(&g_noise_tex, px);   /* random in any texel order: no swizzle needed */
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_noise_tex.data, g_noise_tex.size);
            linearFree(px);
            C3D_TexSetFilter(&g_noise_tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&g_noise_tex, GPU_REPEAT, GPU_REPEAT);
            g_noise_ok = true;
        }
    }

    AttrInfo_Init(&g_attr);
    AttrInfo_AddLoader(&g_attr, 0, GPU_FLOAT, 4);           /* position */
    AttrInfo_AddLoader(&g_attr, 1, GPU_UNSIGNED_BYTE, 4);   /* color    */
    AttrInfo_AddLoader(&g_attr, 2, GPU_FLOAT, 2);           /* uv0      */
    AttrInfo_AddLoader(&g_attr, 3, GPU_FLOAT, 2);           /* uv1      */

    g_vbuf = (gpu_vtx_t *)linearAlloc(sizeof(gpu_vtx_t) * FRAME_MAX_VERTS);
    g_ibuf = (uint16_t *)linearAlloc(sizeof(uint16_t) * FRAME_MAX_IDX);
#ifdef PM3DS_MEMBENCH
    { extern void pm3ds_membench(void *lin); pm3ds_membench(g_vbuf); }
#endif
    BufInfo_Init(&g_buf);
    BufInfo_Add(&g_buf, g_vbuf, sizeof(gpu_vtx_t), 4, 0x3210);

    pm3ds_set_view_mode(PM3DS_VIEW_WIDE);

    if (C3D_TexInitVRAM(&g_rtt_tex, RTT_W, RTT_H, GPU_RGBA8)) {
        g_rtt_target = C3D_RenderTargetCreateFromTex(&g_rtt_tex, GPU_TEXFACE_2D, 0, (GPU_DEPTHBUF)-1);
        if (!g_rtt_target) C3D_TexDelete(&g_rtt_tex);
    }
    if (!g_rtt_target) pm3ds_log(PM3DS_LOG_WARN, "gfx: no window texture; rotating windows draw without contents");
    g_scr_ok = C3D_TexInitVRAM(&g_scr_tex, SCR_W, SCR_H, GPU_RGBA8);
    if (!g_scr_ok) pm3ds_log(PM3DS_LOG_WARN, "gfx: no screen-copy texture; the underwater ripple is left out");
    g_prev_ok = C3D_TexInitVRAM(&g_prev_tex, SCR_W, SCR_H, GPU_RGBA8);
    if (!g_prev_ok) pm3ds_log(PM3DS_LOG_WARN, "gfx: no previous-frame texture; the CPU copies it instead");
    /* pixel space -> texture target: x' = 2x/W - 1, y' = 2y/H - 1 (texture
     * row 0, which v = 0 samples, is at the bottom of a render target) */
    Mtx_Zeros(&g_rtt_mtx);
    g_rtt_mtx.r[0].x = 2.0f / RTT_W;  g_rtt_mtx.r[0].w = -1.0f;
    g_rtt_mtx.r[1].y = 2.0f / RTT_H;  g_rtt_mtx.r[1].w = -1.0f;
    g_rtt_mtx.r[2].z = 0.5f;          g_rtt_mtx.r[2].w = -0.5f;
    g_rtt_mtx.r[3].w = 1.0f;

    C3D_CullFace(GPU_CULL_NONE);   /* culling is done on the CPU in N64 terms */
    C3D_DepthMap(true, -1.0f, 0.0f);
}

/* ---- render state --------------------------------------------------------- */

/* citro3d re-sends a whole register group whenever one of its setters is
 * called, even with the values it already has; per draw that is most of
 * the GPU command cost on an Old 3DS. These pass a call on only when its
 * arguments differ from the last one this frame (g_fx_epoch: anything else
 * that set the state, like the bottom screen, starts a new epoch). */
static uint32_t g_fx_epoch = 1;
typedef struct { uint32_t ep; int32_t a[4]; } fx_cache_t;
static fx_cache_t g_fxc_scissor, g_fxc_depth, g_fxc_depthmap, g_fxc_blend, g_fxc_atest, g_fxc_fogmode, g_fxc_fogcolor;

/* decal depth: the nudge toward the viewer that lets co-planar decals win,
 * and how far behind a decal the surface below may lie before a map model's
 * decal is cut off there (window depth; about 15 units at 500 from the camera) */
#define DECAL_NUDGE 0.0002f
#define DECAL_BAND 0.001f
static bool g_batch_band;                 /* the open batch is a map model's decal */
static bool g_band_den;                   /* ...and its depth test, for draw_decal_banded */
static GPU_TESTFUNC g_band_dfn;
static GPU_WRITEMASK g_band_dm;
/* one cache per setter (several call sites set the same state) */
#ifdef PM3DS_NO_STATE_CACHE   /* debug A/B (hardware-only flicker): every setter reaches citro3d */
#define FX_SKIP 0
#else
#define FX_SKIP 1
#endif
#define FX_CACHED(cache, call, n, ...) \
    do { \
        const int32_t v_[4] = { __VA_ARGS__ }; \
        if (FX_SKIP && (cache).ep == g_fx_epoch && !memcmp((cache).a, v_, sizeof v_)) break; \
        (cache).ep = g_fx_epoch; memcpy((cache).a, v_, sizeof v_); \
        call; \
    } while (0)

static void set_scissor(const gfx_state_t *st) {
    if (g_in_rtt) {   /* texel space: framebuffer y = N64 y */
        int x0 = st->scissor.ulx >> 2, y0 = st->scissor.uly >> 2;
        int x1 = (st->scissor.lrx + 3) >> 2, y1 = (st->scissor.lry + 3) >> 2;
        if (x1 > RTT_W) x1 = RTT_W;
        if (y1 > RTT_H) y1 = RTT_H;
        if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }
        FX_CACHED(g_fxc_scissor, C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)x0, (u32)y0, (u32)x1, (u32)y1), 4, x0, y0, x1, y1);
        return;
    }
    float ulx = st->scissor.ulx / 4.0f, uly = st->scissor.uly / 4.0f;
    float lrx = st->scissor.lrx / 4.0f, lry = st->scissor.lry / 4.0f;
    if (lrx > 320.0f) lrx = 320.0f;
    if (lry > 240.0f) lry = 240.0f;
    int left   = (int)(240.0f - (g_view_s * lry + g_view_oy)), right  = (int)(240.0f - (g_view_s * uly + g_view_oy));
    int top    = (int)(400.0f - (g_view_s * lrx + g_view_ox)), bottom = (int)(400.0f - (g_view_s * ulx + g_view_ox));
    if (view_wide() && ulx <= 12.5f && lrx >= 307.5f) { top = 0; bottom = 400; }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > 240) right = 240;
    if (bottom > 400) bottom = 400;
    if (right <= left || bottom <= top) { left = top = 0; right = bottom = 1; }
    FX_CACHED(g_fxc_scissor, C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)left, (u32)top, (u32)right, (u32)bottom), 4, left, top, right, bottom);
}

static void update_fog_lut(const gfx_state_t *st) {
    if (st->fog_mul == g_fog_lut_mul && st->fog_offset == g_fog_lut_ofs) return;
    g_fog_lut_mul = st->fog_mul;
    g_fog_lut_ofs = st->fog_offset;
    /* depth d in [0,1] with near = 1 (C3D_DepthMap(-1, 0) of z in [-1,0]);
     * N64 z/w = 1 - 2d. PICA fog: out = mix(fog, color, lut) -> lut = 1 - fog. */
    float data[256];
    float vals[129];
    for (int i = 0; i <= 128; i++) {
        float d = i / 128.0f;
        float zn = 1.0f - 2.0f * d;
        float f = (zn * st->fog_mul + st->fog_offset) / 255.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        vals[i] = 1.0f - f;
    }
    for (int i = 0; i < 128; i++) { data[i] = vals[i]; data[128 + i] = vals[i + 1] - vals[i]; }
    FogLut_FromArray(&g_fog_lut, data);
    g_fog_lut_bound = false;
}

#ifdef PM3DS_DRAWLOG_FRAME
/* frame whose draws are logged (DRAW / BIGDRAW / BIGTRI); PM3DS_MAGENTA_HUNT
 * moves it to the frame after a magenta-tinted one */
uint32_t pm3ds_drawlog_frame = PM3DS_DRAWLOG_FRAME;
#endif

static uint32_t rgba8(const uint8_t c[4]) {
    return (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
}

/* The colour combiner's final alpha with every texel and the shade fully
 * opaque: how far constants (prim/env alpha) scale the surface down. */
static float combined_alpha_max(const gfx_state_t *st, bool two_cycle) {
    uint32_t w0 = st->combine_w0, w1 = st->combine_w1;
    float prim = st->prim[3] / 255.0f, env = st->env[3] / 255.0f, plod = st->prim_lod_frac / 255.0f;
    float comb = 0.0f;
    for (int c = 0; c < (two_cycle ? 2 : 1); c++) {
        uint32_t a = c ? (w1 >> 21) & 7 : (w0 >> 12) & 7, b = c ? (w1 >> 3) & 7 : (w1 >> 12) & 7;
        uint32_t m = c ? (w1 >> 18) & 7 : (w0 >> 9) & 7, d = c ? w1 & 7 : (w1 >> 9) & 7;
        /* a/b/d: COMBINED TEXEL0 TEXEL1 PRIM SHADE ENV 1 0; c: LOD_FRAC .. PRIM_LOD_FRAC 0 */
        const float abd[8] = { comb, 1.0f, 1.0f, prim, 1.0f, env, 1.0f, 0.0f };
        const float cin[8] = { 0.0f, 1.0f, 1.0f, prim, 1.0f, env, plod, 0.0f };
        comb = (abd[a] - abd[b]) * cin[m] + abd[d];
        if (comb < 0.0f) comb = 0.0f;
        if (comb > 1.0f) comb = 1.0f;
    }
    return comb;
}

/* Texture units as citro3d has them: binding the same texture with the same
 * settings again would only make it send the unit's registers again. The
 * whole C3D_Tex is compared, not just its address (a freed cache entry's
 * address comes back for another texture). Reset each frame: the bottom
 * screen binds its own. */
static struct { const C3D_Tex *tex; C3D_Tex copy; bool valid; } g_bound[3];
void pm3ds_tex_bind(int unit, C3D_Tex *t) {
    if (unit >= 0 && unit < 3) {
#ifndef PM3DS_NO_BIND_SKIP   /* debug A/B (hardware-only flicker): every bind reaches citro3d */
        if (g_bound[unit].valid && g_bound[unit].tex == t && !memcmp(&g_bound[unit].copy, t, sizeof *t)) return;
#endif
        g_bound[unit].tex = t;
        g_bound[unit].copy = *t;
        g_bound[unit].valid = true;
    }
    C3D_TexBind(unit, t);
}
static void tex_bind_invalidate(void) { for (int i = 0; i < 3; i++) g_bound[i].valid = false; }
/* t's texels are about to change on the GPU (a window drawn into it, a screen
 * copy): its next bind must reach citro3d, whose bind also clears the PICA's
 * texture cache. Skipped, the console samples the texels it cached from the
 * last use (emulators keep no texture cache, so only hardware shows it). */
static void tex_bind_forget(const C3D_Tex *t) {
    for (int i = 0; i < 3; i++)
        if (g_bound[i].tex == t) g_bound[i].valid = false;
}

/* N64 texel coordinates (s10.5, before the tile's shift) -> normalized GPU
 * texcoords are affine; work the factors out once per state change, so a
 * vertex costs two multiply-adds per axis instead of several divisions */
static void tex_coeffs(int unit) {
    const gfx_tile_t *tl = g_tex[unit].tile;
    float fs = 1.0f, ft = 1.0f;
    if (tl->shifts) fs = tl->shifts <= 10 ? 1.0f / (float)(1 << tl->shifts) : (float)(1 << (16 - tl->shifts));
    if (tl->shiftt) ft = tl->shiftt <= 10 ? 1.0f / (float)(1 << tl->shiftt) : (float)(1 << (16 - tl->shiftt));
    /* x = s * xs + xb, y = t * yt + yb: texels in the GPU texture */
    float xs = fs / 32.0f, xb = g_tex[unit].off_s - tl->uls / 4.0f;
    float yt = ft / 32.0f, yb = g_tex[unit].off_t - tl->ult / 4.0f;
    if (g_tex[unit].screen) {   /* N64 pixel -> render target texel (see set_scissor) */
        /* the target's 400 rows land in the last 400 of the texture's 512:
         * u = (240 - (view_s * y + view_oy)) / SCR_W, v = (SCR_H - (view_s * x + view_ox)) / SCR_H */
        g_tex[unit].us = 0.0f;
        g_tex[unit].ut = -g_view_s * yt / SCR_W;
        g_tex[unit].ub = (240.0f - g_view_s * yb - g_view_oy) / SCR_W;
        g_tex[unit].vs = -g_view_s * xs / SCR_H;
        g_tex[unit].vt = 0.0f;
        g_tex[unit].vb = (SCR_H - g_view_s * xb - g_view_ox) / SCR_H;
        return;
    }
    g_tex[unit].us = xs / g_tex[unit].w;
    g_tex[unit].ut = 0.0f;
    g_tex[unit].ub = xb / g_tex[unit].w;
    g_tex[unit].vs = 0.0f;
    g_tex[unit].vt = yt / g_tex[unit].h;
    g_tex[unit].vb = yb / g_tex[unit].h;
}

static void apply_state(gfx_state_t *st, bool rect) {
    uint32_t l = st->othermode_l;
    uint32_t cyc = st->othermode_h & OMH_CYC_MASK;
    gfx_cc_info_t info = { false, false, false };
#ifdef PM3DS_DRAWLOG_FRAME
    if (g_frame_no == pm3ds_drawlog_frame) {
        const gfx_tile_t *t = &st->tiles[st->tex_tile & 7];
        pm3ds_log(PM3DS_LOG_INFO, "DRAW rect=%d cc=%06X:%08X omh=%08X oml=%08X prim=%02X%02X%02X%02X env=%02X%02X%02X%02X fog=%02X%02X%02X%02X gm=%08X tex=%u/%u timg=%08X",
                  rect, (unsigned)st->combine_w0, (unsigned)st->combine_w1, (unsigned)st->othermode_h, (unsigned)l,
                  st->prim[0], st->prim[1], st->prim[2], st->prim[3], st->env[0], st->env[1], st->env[2], st->env[3],
                  st->fog[0], st->fog[1], st->fog[2], st->fog[3], (unsigned)st->geometry_mode, t->fmt, t->siz,
                  (unsigned)st->timg_addr);
    }
#endif

    /* --- combiner --- */
    if (cyc == OMH_CYC_FILL) {
        uint32_t c16 = st->fill_color >> 16;
        uint8_t r = (uint8_t)(((c16 >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((c16 >> 6) & 0x1F) * 255 / 31);
        uint8_t b = (uint8_t)(((c16 >> 1) & 0x1F) * 255 / 31);
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        /* the alpha bit only matters in a window texture (its clear is transparent) */
        uint32_t a = (!g_in_rtt || (c16 & 1)) ? 0xFF000000u : 0;
        C3D_TexEnvColor(env, (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | a);
        for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
        gfx_combiner_invalidate();
    } else if (cyc == OMH_CYC_COPY) {
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
        gfx_combiner_invalidate();
        info.uses_tex0 = true;
    } else {
        GP_BEGIN(tc);
        PROF_PHASE(PH_COMB);
        gfx_combiner_apply(st, cyc == OMH_CYC_2, &info);
        PROF_PHASE(PH_STATE);
        GP_END(tc, GP_COMB);
    }

    /* --- textures --- */
    bool linear = cyc != OMH_CYC_COPY && (st->othermode_h & OMH_TF_MASK) != OMH_TF_POINT;
    for (int u = 0; u < 2; u++) {
        g_tex[u].on = false;
        bool want = u == 0 ? info.uses_tex0 : info.uses_tex1;
        if (!want) continue;
        int tile = (st->tex_tile + u) & 7;
        g_tex[u].off_s = g_tex[u].off_t = 0.0f;
        g_tex[u].screen = false;
        {   /* a load from the window texture's image: sample the GPU texture */
            uint32_t src;
            int col, row;
            /* a copy of the screen (this frame's so far, or the last frame's) */
            C3D_Tex *scr = NULL;
            uint32_t off = 0, stride = 0;
            if ((g_scr_bytes || g_prev_bytes) && gfx_tex_load_source(st, tile, &src, &col, &row)) {
                if (g_scr_bytes && src - g_scr_addr < g_scr_bytes) {
                    scr = &g_scr_tex; off = src - g_scr_addr; stride = g_scr_stride;
                } else if (g_prev_bytes && src - g_prev_addr < g_prev_bytes) {
                    scr = &g_prev_tex; off = src - g_prev_addr; stride = 320 * 2;
                }
            }
            if (scr) {
                GPU_TEXTURE_FILTER_PARAM f = linear ? GPU_LINEAR : GPU_NEAREST;
                C3D_TexSetFilter(scr, f, f);
                C3D_TexSetWrap(scr, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
                pm3ds_tex_bind(u, scr);
                g_tex[u].on = true;
                g_tex[u].screen = true;
                g_tex[u].tile = &st->tiles[tile];
                g_tex[u].w = SCR_W;
                g_tex[u].h = SCR_H;
                g_tex[u].off_s = (float)(col + (int)(off % stride) / 2);
                g_tex[u].off_t = (float)(row + (int)(off / stride));
                continue;
            }
            if (g_rtt_bytes && gfx_tex_load_source(st, tile, &src, &col, &row) && src - g_rtt_addr < g_rtt_bytes) {
                uint32_t off = src - g_rtt_addr;
                GPU_TEXTURE_FILTER_PARAM f = linear ? GPU_LINEAR : GPU_NEAREST;
                C3D_TexSetFilter(&g_rtt_tex, f, f);
                C3D_TexSetWrap(&g_rtt_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
                pm3ds_tex_bind(u, &g_rtt_tex);
                g_tex[u].on = true;
                g_tex[u].tile = &st->tiles[tile];
                g_tex[u].w = RTT_W;
                g_tex[u].h = RTT_H;
                g_tex[u].off_s = (float)(col + (int)(off % g_rtt_stride) / 2);
                g_tex[u].off_t = (float)(row + (int)(off / g_rtt_stride));
                continue;
            }
        }
        unsigned w = 0, h = 0;
        if (u == 0 && st->tex_levels > 0 && gfx_combiner_uses_lod(st)) {
            /* texture LOD: the tile chain as one mipmapped texture (gfx_texture.c) */
            extern C3D_Tex *gfx_tex_get_mip(const gfx_state_t *st, int tile0, int levels, unsigned *w, unsigned *h, int *scale);
            int scale = 1;
            C3D_Tex *mt = gfx_tex_get_mip(st, tile, st->tex_levels, &w, &h, &scale);
            if (mt) {
                const gfx_tile_t *t = &st->tiles[tile];
                extern void gfx_tex_wrap_modes(const gfx_tile_t *t, int *cms, int *cmt);
                int cms, cmt;
                gfx_tex_wrap_modes(t, &cms, &cmt);
                GPU_TEXTURE_FILTER_PARAM f = linear ? GPU_LINEAR : GPU_NEAREST;
                C3D_TexSetFilter(mt, f, f);
                C3D_TexSetFilterMipmap(mt, GPU_LINEAR);
                C3D_TexSetWrap(mt, (cms & 2) ? GPU_CLAMP_TO_EDGE : (cms & 1) ? GPU_MIRRORED_REPEAT : GPU_REPEAT,
                               (cmt & 2) ? GPU_CLAMP_TO_EDGE : (cmt & 1) ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
                /* the GPU measures texels of the scaled-up texture per 3DS pixel; the
                 * RDP measured level-0 texels per N64 pixel */
                C3D_TexSetLodBias(mt, log2f(g_view_s) - log2f((float)scale));
                pm3ds_tex_bind(0, mt);
                g_tex[0].on = true;
                g_tex[0].tile = t;
                g_tex[0].w = (float)w;
                g_tex[0].h = (float)h;
                continue;
            }
        }
        GP_BEGIN(tt);
        PROF_PHASE(PH_TEX);
        gfx_tex_entry_t *e = gfx_tex_get(st, tile, &w, &h);
        PROF_PHASE(PH_STATE);
        GP_END(tt, GP_TEX);
        if (!e) {
#ifdef PM3DS_TITLE_DBG
            extern int gfx_tex_miss_reason;
            static uint32_t seen[32][3];
            static int nseen;
            const gfx_tile_t *mt = &st->tiles[tile];
            uint32_t k[3] = { st->combine_w0 ^ st->combine_w1, (uint32_t)gfx_tex_miss_reason << 24 | mt->tmem,
                              (uint32_t)mt->fmt << 8 | mt->siz };
            int i;
            for (i = 0; i < nseen; i++) if (!memcmp(seen[i], k, sizeof(k))) break;
            if (i == nseen && nseen < 32) {
                memcpy(seen[nseen++], k, sizeof(k));
                pm3ds_log(PM3DS_LOG_WARN, "TEXMISS unit=%d tile=%d reason=%d fmt=%u siz=%u tmem=%u uls=%u ult=%u lrs=%u lrt=%u cc=%06X:%08X",
                          u, tile, gfx_tex_miss_reason, mt->fmt, mt->siz, mt->tmem, mt->uls, mt->ult, mt->lrs, mt->lrt,
                          (unsigned)st->combine_w0, (unsigned)st->combine_w1);
            }
#endif
            continue;
        }
        const gfx_tile_t *t = &st->tiles[tile];
        {   /* mask smaller than the tile: see gfx_tex_wrap_modes */
            extern void gfx_tex_wrap_modes(const gfx_tile_t *t, int *cms, int *cmt);
            int cms, cmt;
            gfx_tex_wrap_modes(t, &cms, &cmt);
            gfx_tex_bind_unit(e, u, linear, cms, cmt);
        }
        g_tex[u].on = true;
        g_tex[u].tile = t;
        g_tex[u].w = (float)pm3ds_tex_pot(w);
        g_tex[u].h = (float)pm3ds_tex_pot(h);
    }
    for (int u = 0; u < 2; u++)
        if (g_tex[u].on) tex_coeffs(u);
#ifdef PM3DS_DRAWLOG_FRAME
    if (g_frame_no == pm3ds_drawlog_frame && (info.uses_tex0 || info.uses_tex1)) {
        for (int u = 0; u < 2; u++) {
            const gfx_tile_t *t = &st->tiles[(st->tex_tile + u) & 7];
            pm3ds_log(PM3DS_LOG_INFO, "  unit%d want=%d on=%d %gx%g tile s=%u..%u t=%u..%u tmem=%u line=%u mask=%u/%u cm=%u/%u",
                      u, u == 0 ? info.uses_tex0 : info.uses_tex1, g_tex[u].on, g_tex[u].w, g_tex[u].h,
                      t->uls, t->lrs, t->ult, t->lrt, t->tmem, t->line, t->masks, t->maskt, t->cms, t->cmt);
        }
    }
#endif
    /* NOISE reads unit 1 when the combine mode leaves TEXEL1 unused (the
     * combiner maps it to a flat 0.5 otherwise) */
    g_noise_uv = info.uses_noise && !info.uses_tex1 && g_noise_ok;
    if (g_noise_uv) {
        static bool logged;
        if (!logged) {
            logged = true;
            pm3ds_log(PM3DS_LOG_INFO, "gfx: first NOISE draw (frame %u)", (unsigned)g_frame_no);
        }
        pm3ds_tex_bind(1, &g_noise_tex);
    }
    g_applied_tile = st->tex_tile;
    PROF_PHASE(PH_FX);
#ifdef PM3DS_TITLE_DBG
    {   /* one line per distinct render state that samples a texture */
        static uint32_t seen[64][4];
        static int nseen;
        if (g_tex[0].on && nseen < 64) {
            uint32_t k[4] = { st->combine_w0, st->combine_w1, st->othermode_h, st->othermode_l };
            int i;
            for (i = 0; i < nseen; i++) if (!memcmp(seen[i], k, sizeof(k))) break;
            if (i == nseen) {
                memcpy(seen[nseen++], k, sizeof(k));
                const gfx_tile_t *t = g_tex[0].tile;
                pm3ds_log(PM3DS_LOG_INFO, "RSTATE cc=%06X:%08X omh=%08X oml=%08X fmt=%u siz=%u gm=%08X rect=%d",
                          (unsigned)k[0], (unsigned)k[1], (unsigned)k[2], (unsigned)k[3], t->fmt, t->siz,
                          (unsigned)st->geometry_mode, rect);
            }
        }
    }
#endif

    /* --- depth --- */
    bool zcmp, zupd;
    if (cyc == OMH_CYC_FILL || cyc == OMH_CYC_COPY) {
        zcmp = zupd = false;
    } else if (rect) {
        zcmp = (l & RM_Z_CMP) && (l & RM_ZS_PRIM);
        zupd = (l & RM_Z_UPD) && (l & RM_ZS_PRIM);
    } else {
        bool zb = (st->geometry_mode & GM_ZBUFFER) != 0;
        zcmp = zb && (l & RM_Z_CMP);
        zupd = zb && (l & RM_Z_UPD);
    }
    /* blender formula MEM*A + MEM*B (or MEM without FORCE_BL): the colour
     * stays as it is and only depth is written -- the battle-start iris
     * draws its star into the z-buffer this way */
    /* the blender word that applies: the second cycle's in 2-cycle mode,
     * the first cycle's in 1-cycle mode (render modes like CLD_SURF + NOOP2
     * set only that one) */
    int bsh = cyc == OMH_CYC_2 ? 0 : 2;
    uint32_t p2_ = (l >> (28 + bsh)) & 3, m2_ = (l >> (20 + bsh)) & 3;
    bool keep_color = cyc != OMH_CYC_FILL && cyc != OMH_CYC_COPY && p2_ == 1 && (m2_ == 1 || !(l & RM_FORCE_BL));
    GPU_WRITEMASK wmask = keep_color ? (zupd ? GPU_WRITE_DEPTH : 0) : (zupd ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    {
        bool den = g_in_rtt ? false : (zcmp || zupd || keep_color);
        GPU_TESTFUNC dfn = g_in_rtt ? GPU_ALWAYS : zcmp ? GPU_GEQUAL : GPU_ALWAYS;
        GPU_WRITEMASK dm = g_in_rtt ? (keep_color ? (GPU_WRITEMASK)0 : GPU_WRITE_COLOR) : wmask;
        FX_CACHED(g_fxc_depth, C3D_DepthTest(den, dfn, dm), 3, den, dfn, dm);
        g_band_den = den;
        g_band_dfn = dfn;
        g_band_dm = dm;
    }
    /* decal: nudge toward the viewer so co-planar decals win */
    {
        int decal = (l & RM_ZMODE_DEC) == RM_ZMODE_DEC;
        FX_CACHED(g_fxc_depthmap, C3D_DepthMap(true, -1.0f, decal ? DECAL_NUDGE : 0.0f), 1, decal);
        /* a map model's decal also may not draw where the surface below is
         * farther away: see draw_decal_banded */
        g_batch_band = decal && st->decal_band && !g_in_rtt && !rect && zcmp && !keep_color;
#ifdef PM3DS_NO_DECAL_BAND   /* debug A/B (GitHub #63 on hardware): decals without the stencil band */
        g_batch_band = false;
#endif
    }

    /* --- blend (the blender word that applies, see bsh) --- */
    uint32_t p2 = (l >> (28 + bsh)) & 3, a2 = (l >> (24 + bsh)) & 3, m2 = (l >> (20 + bsh)) & 3, b2 = (l >> (16 + bsh)) & 3;
    bool blend = false, additive = false;
    if (cyc != OMH_CYC_FILL && cyc != OMH_CYC_COPY && (l & RM_FORCE_BL) && p2 == 0 && m2 == 1) {
        if (b2 == 0 && a2 == 0) blend = true;          /* IN*A + MEM*(1-A) */
        else if (b2 == 2) { blend = true; additive = true; }  /* IN*A + MEM */
    }
    /* coverage x alpha with alpha as coverage (TEX_EDGE): the RDP writes any
     * pixel whose coverage (8 * alpha) is at least 1 and anti-aliases it
     * against memory. A plain 1/8 test plus blend reproduces that badly: soft
     * texture edges pick up the colour of neighbouring transparent texels and
     * write depth (fringes on foliage). So the cutoff stays at half the
     * texel alpha; only when a constant (prim/env alpha) scales the whole
     * surface down -- the Heart Block's heart at 72/255, which a fixed 0.5
     * cutoff dropped entirely, depth writes included -- is the cutoff scaled
     * with it and the surface blended. */
    float cvg_amax = 1.0f;
    bool cvg_surface = !blend && cyc != OMH_CYC_FILL && cyc != OMH_CYC_COPY &&
                       (l & RM_CVG_X_ALPHA) && (l & RM_ALPHA_CVG_SEL) && (l & RM_AC_MASK) != RM_AC_THRESHOLD;
    if (cvg_surface) cvg_amax = combined_alpha_max(st, cyc == OMH_CYC_2);
    bool cvg_blend = cvg_surface && cvg_amax < 0.99f;
    if (blend && additive) {
        FX_CACHED(g_fxc_blend, C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE), 1, 1);
    } else if (blend || cvg_blend) {
        /* in a window texture the alpha left behind is what the window shows
         * through later: compose it with "over" */
        FX_CACHED(g_fxc_blend, C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                                 g_in_rtt ? GPU_ONE : GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA), 1, g_in_rtt ? 3 : 2);
    } else {
        FX_CACHED(g_fxc_blend, C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO), 1, 0);
    }

    /* --- alpha compare --- */
    if ((l & RM_AC_MASK) == RM_AC_THRESHOLD) {
        int ref = st->blend[3];
        FX_CACHED(g_fxc_atest, C3D_AlphaTest(true, ref ? GPU_GEQUAL : GPU_GREATER, ref), 2, 1, ref);
    } else if (cvg_blend) {
        int ref = (int)(cvg_amax * 0x80);
        FX_CACHED(g_fxc_atest, C3D_AlphaTest(true, GPU_GEQUAL, ref < 1 ? 1 : ref), 2, 2, ref < 1 ? 1 : ref);
    } else if (cyc != OMH_CYC_FILL && !blend && (l & RM_CVG_X_ALPHA)) {
        /* coverage x alpha: a zero-alpha texel gets no coverage and is not
         * written (sprites use this without ALPHA_CVG_SEL) */
        FX_CACHED(g_fxc_atest, C3D_AlphaTest(true, GPU_GEQUAL, 0x80), 2, 2, 0x80);
    } else if (cyc == OMH_CYC_COPY) {
        FX_CACHED(g_fxc_atest, C3D_AlphaTest(true, GPU_GREATER, 0), 2, 3, 0);
    } else {
        FX_CACHED(g_fxc_atest, C3D_AlphaTest(false, GPU_ALWAYS, 0), 2, 0, 0);
    }

    /* --- fog (first-cycle blender = CLR_FOG with shade alpha) --- */
    if (!rect && (st->geometry_mode & GM_FOG) && ((l >> 30) & 3) == 3) {
        update_fog_lut(st);
        FX_CACHED(g_fxc_fogmode, C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false), 1, 1);
        uint32_t fc = rgba8(st->fog) & 0x00FFFFFFu;
        FX_CACHED(g_fxc_fogcolor, C3D_FogColor(fc), 1, (int32_t)fc);
        if (!g_fog_lut_bound) {
            C3D_FogLutBind(&g_fog_lut);
            g_fog_lut_bound = true;
        }
    } else {
        FX_CACHED(g_fxc_fogmode, C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false), 1, 0);
    }

    set_scissor(st);
    g_applied_rect = rect;
    g_state_valid = true;
}

static void draw_tris(const u16 *idx, int n) {
    extern bool pm3ds_c3d_draw_tris(const u16 *idx, int count);
    if (!pm3ds_c3d_draw_tris(idx, n))
        C3D_DrawElements(GPU_TRIANGLES, n, C3D_UNSIGNED_SHORT, idx);
}

/* The RDP's decal depth mode passes a pixel only when it is about as deep as
 * what is already there; a depth test passes everything in front. The RDP's
 * band grows with the surface's depth slope, so seen edge-on (the Koopa Bros.
 * Fortress cell, GitHub #24) the N64 itself draws a bombed wall's cracks over
 * the hole; a fixed band here stops them at its edge. One depth test can't
 * check a band, so with a
 * stencil bit: mark where the surface below lies more than DECAL_BAND behind
 * the decal, draw the decal where nothing is marked, then clear the marks.
 * Only for map models (PM3DS_GFX_TAG_DECAL_BAND): entity shadows are decals
 * too, there are many, and they stay one draw each. */
static void draw_decal_banded(const u16 *idx, int n) {
    C3D_DepthMap(true, -1.0f, -DECAL_BAND);
    C3D_DepthTest(true, GPU_GEQUAL, 0);
    C3D_StencilTest(true, GPU_ALWAYS, 1, 0xFF, 0x01);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    draw_tris(idx, n);

    C3D_DepthMap(true, -1.0f, DECAL_NUDGE);
    C3D_DepthTest(g_band_den, g_band_dfn, g_band_dm);
    C3D_StencilTest(true, GPU_EQUAL, 0, 0x01, 0x00);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    draw_tris(idx, n);

    C3D_DepthTest(true, GPU_ALWAYS, 0);
    C3D_StencilTest(true, GPU_ALWAYS, 0, 0xFF, 0x01);
    C3D_StencilOp(GPU_STENCIL_REPLACE, GPU_STENCIL_REPLACE, GPU_STENCIL_REPLACE);
    draw_tris(idx, n);

    /* back to what the state caches hold */
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_DepthTest(g_band_den, g_band_dfn, g_band_dm);
}

/* Close the open batch: one draw call for everything since g_batch_start. */
void gfx_backend_flush(gfx_state_t *st) {
    (void)st;
    int n = g_icount - g_batch_start;
    if (n > 0) {
        GP_BEGIN(tf);
        PROF_SAVE(ph);
#ifdef PM3DS_STEP_PROF
        {   /* the context flush C3D_DrawElements would do, timed on its own */
            extern void C3Di_UpdateContext(void);
            extern void pm3ds_c3d_count_flags(void);
            pm3ds_c3d_count_flags();
            PROF_PHASE(PH_CTX);
            C3Di_UpdateContext();
        }
#endif
        PROF_PHASE(PH_DRAW);
        if (g_batch_band)
            draw_decal_banded(g_ibuf + g_batch_start, n);
        else
            draw_tris(g_ibuf + g_batch_start, n);
        PROF_RESTORE(ph);
        GP_END(tf, GP_FLUSH);
        g_stat_draws++;
    }
    g_batch_start = g_icount;
}

/* attribute buffer from vertex `base` on (16-bit indices count from there) */
static void set_vbase(int base) {
    g_vbase = base;
    BufInfo_Init(&g_buf);
    BufInfo_Add(&g_buf, g_vbuf + base, sizeof(gpu_vtx_t), 4, 0x3210);
    C3D_SetBufInfo(&g_buf);
    g_vc_epoch++;
}

/* room for nverts more vertices and nidx indices (flushes first if the
 * 16-bit indices would run out); false when the frame's budget is spent */
static bool vtx_room(gfx_state_t *st, int nverts, int nidx) {
    if (g_vcount + nverts > FRAME_MAX_VERTS || g_icount + nidx > FRAME_MAX_IDX) {
        static int warned;
        if (!warned++) pm3ds_log(PM3DS_LOG_WARN, "gfx: frame vertex budget (%d/%d) exhausted", FRAME_MAX_VERTS, FRAME_MAX_IDX);
        return false;
    }
    if (g_vcount + nverts - g_vbase > 0xFFFF) {
        gfx_backend_flush(st);
        set_vbase(g_vcount);
    }
    return true;
}

static unsigned g_stat_offscreen;
/* Share of the GPU command buffer (PM3DS_CMDBUF_BYTES) the game's lists may
 * use; the rest stays for the bottom screen and the end of the frame. A frame
 * that would go past it loses its remaining draws instead of crashing
 * (citro3d's GPUCMD_Add calls svcBreak on overflow). */
#define CMDBUF_STOP 0.85f
static bool g_cmdbuf_full;
/* live fill level (C3D_GetCmdBufUsage is only updated when a frame ends) */
static float cmdbuf_use(void) {
    u32 *buf, size, offset;
    GPUCMD_GetBuffer(&buf, &size, &offset);
    return size ? (float)offset / (float)size : 0.0f;
}
static float g_cmdbuf_peak;   /* highest use in a frame, for the PERF line */
#ifdef PM3DS_STEP_PROF
/* debug: how many state changes leave the draw state as it was */
static unsigned g_gp_applies, g_gp_same, g_gp_verts;
static void gp_count_apply(const gfx_state_t *st, bool rect) {
    g_gp_applies++;
#ifdef PM3DS_GP_SAME   /* compare each state change with the last one: costs as much as the change (palette hash) */
    static uint8_t last[512 + 256];
    uint8_t cur[sizeof last];
    size_t n = 0;
#define GP_PUT(p, sz) do { memcpy(cur + n, (p), (sz)); n += (sz); } while (0)
    size_t a = offsetof(gfx_state_t, othermode_h), b = offsetof(gfx_state_t, timg_addr);
    GP_PUT((const uint8_t *)st + a, b - a);
    GP_PUT(&st->geometry_mode, sizeof st->geometry_mode);
    GP_PUT(&st->fog_mul, 4);
    GP_PUT(&st->tex_tile, 1);
    GP_PUT(&rect, 1);
    GP_PUT(&g_in_rtt, 1);
    for (int u = 0; u < 2; u++) {
        int tile = (st->tex_tile + u) & 7;
        uint32_t src = 0;
        int col = 0, row = 0;
        gfx_tex_load_source(st, tile, &src, &col, &row);
        GP_PUT(&st->tiles[tile], sizeof st->tiles[tile]);
        GP_PUT(&src, 4);
    }
    uint32_t th = 2166136261u;
    for (int i = 0; i < 512; i++) th = (th ^ st->tlut[i]) * 16777619u;
    GP_PUT(&th, 4);
#undef GP_PUT
    if (!memcmp(cur, last, n)) g_gp_same++;
    memcpy(last, cur, n);
#else
    (void)st; (void)rect;
#endif
}
#endif

static bool begin_prims(gfx_state_t *st, bool rect, int nverts) {
    if (!g_draw_enabled || !(st->cimg_is_screen || st->cimg_rtt)) { g_stat_offscreen++; return false; }
    if (g_cmdbuf_full) return false;
    if (!vtx_room(st, nverts, nverts)) return false;
    if (st->dirty || !g_state_valid || rect != g_applied_rect || st->tex_tile != g_applied_tile) {
        gfx_backend_flush(st);
        /* commands are only added here (state + the draw of the batch) */
        float use = cmdbuf_use();
        if (use > g_cmdbuf_peak) g_cmdbuf_peak = use;
        if (use > CMDBUF_STOP) {
            static int warned;
            if (!warned++) pm3ds_log(PM3DS_LOG_WARN, "gfx: GPU command buffer nearly full, dropping the rest of the frame");
            g_cmdbuf_full = true;
            return false;
        }
#ifdef PM3DS_STEP_PROF
        gp_count_apply(st, rect);
#endif
        GP_BEGIN(t);
        PROF_PHASE(PH_STATE);
        apply_state(st, rect);
        PROF_PHASE(PH_WALK);
        GP_END(t, GP_STATE);
        g_vc_epoch++;   /* texture coordinates of cached vertices are stale */
        st->dirty = 0;
    }
    return true;
}

/* N64 texel space -> normalized GPU texcoords for one texture unit. */
static inline void tex_uv(int unit, float s, float t, float *u, float *v) {
    if (!g_tex[unit].on) { *u = *v = 0.0f; return; }
    *u = s * g_tex[unit].us + t * g_tex[unit].ut + g_tex[unit].ub;
    *v = s * g_tex[unit].vs + t * g_tex[unit].vt + g_tex[unit].vb;
}

static inline uint16_t put_vtx(const gfx_vtx_t *v) {
    uint16_t idx = (uint16_t)(g_vcount - g_vbase);
    gpu_vtx_t *o = &g_vbuf[g_vcount++];
    o->x = v->px; o->y = v->py; o->z = v->cz; o->w = v->cw;
    o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a;
    tex_uv(0, v->s, v->t, &o->u0, &o->v0);
    if (g_noise_uv) {   /* pixel position / 64 + this frame's offset */
        float iw = (v->cw > 1e-6f || v->cw < -1e-6f) ? 1.0f / v->cw : 0.0f;
        o->u1 = v->px * iw * (1.0f / 64.0f) + g_noise_ofs_u;
        o->v1 = v->py * iw * (1.0f / 64.0f) + g_noise_ofs_v;
    } else {
        tex_uv(1, v->s, v->t, &o->u1, &o->v1);
    }
    return idx;
}

/* a pool vertex's GPU copy, converted on first use */
static inline uint16_t pool_vtx(const gfx_state_t *st, const gfx_vtx_t *v) {
    unsigned i = (unsigned)(v - st->vtx);
    if (i < GFX_VTX_POOL_SIZE) {
        if (g_vc_serial[i] == v->serial && g_vc_ep[i] == g_vc_epoch) return g_vc_idx[i];
        g_vc_serial[i] = v->serial;
        g_vc_ep[i] = g_vc_epoch;
        return g_vc_idx[i] = put_vtx(v);
    }
    return put_vtx(v);
}

void gfx_backend_tri(gfx_state_t *st, const gfx_vtx_t *a, const gfx_vtx_t *b, const gfx_vtx_t *c) {
    if (!begin_prims(st, false, 3)) return;
    PROF_PHASE(PH_PUT);
    if (st->othermode_l & RM_ZS_PRIM) {
        /* depth source = primitive: every pixel tests and writes the prim
         * depth (the battle "Nice"/"Good" text sits in front of the actors
         * this way); keep x/y, make z/w that constant */
        float z = (st->prim_depth & 0x7FFF) / 16383.5f - 1.0f;
        gfx_vtx_t v[3] = { *a, *b, *c };
        for (int i = 0; i < 3; i++) v[i].cz = z * v[i].cw;
        g_ibuf[g_icount++] = put_vtx(&v[0]);
        g_ibuf[g_icount++] = put_vtx(&v[1]);
        g_ibuf[g_icount++] = put_vtx(&v[2]);
        PROF_PHASE(PH_WALK);
        return;
    }
    g_ibuf[g_icount++] = pool_vtx(st, a);
    g_ibuf[g_icount++] = pool_vtx(st, b);
    g_ibuf[g_icount++] = pool_vtx(st, c);
    PROF_PHASE(PH_WALK);
}

extern float pm3ds_stereo_shift;
static bool g_stereo_far;   /* walking the background list: put it at infinity */

void gfx_backend_rect(gfx_state_t *st, float x0, float y0, float x1, float y1,
                      float s0, float t0, float s1, float t1, bool flip, bool textured) {
    if (!begin_prims(st, true, 6)) return;
    if (g_stereo_far && !g_in_rtt) { x0 += pm3ds_stereo_shift; x1 += pm3ds_stereo_shift; }
#ifdef PM3DS_DRAWLOG_FRAME
    if (g_frame_no == pm3ds_drawlog_frame && (x1 - x0) * (y1 - y0) > 20000) {
        const gfx_tile_t *t = &st->tiles[st->tex_tile & 7];
        pm3ds_log(PM3DS_LOG_INFO, "BIGDRAW rect %.0f,%.0f-%.0f,%.0f tex=%d fmt=%u siz=%u cc=%06X:%08X prim=%02X%02X%02X%02X oml=%08X omh=%08X timg=%08X",
                  x0, y0, x1, y1, textured, t->fmt, t->siz, (unsigned)st->combine_w0, (unsigned)st->combine_w1,
                  st->prim[0], st->prim[1], st->prim[2], st->prim[3], (unsigned)st->othermode_l, (unsigned)st->othermode_h,
                  (unsigned)st->timg_addr);
    }
#endif
    if (view_wide() && !g_in_rtt && !st->bg_wide && x0 <= 12.5f && x1 >= 307.5f && x1 > x0) {
        /* full-width rectangle: carry it to the screen edges (the backdrop
         * does that itself, with its wrapped texels: gfx_translate.c) */
        float xl = -g_view_ox / g_view_s, xr = (400.0f - g_view_ox) / g_view_s;
        float nx0 = x0 < xl ? x0 : xl, nx1 = x1 > xr ? x1 : xr;
        if (textured) {
            if (!flip) { float d = (s1 - s0) / (x1 - x0); s0 += (nx0 - x0) * d; s1 += (nx1 - x1) * d; }
            else       { float d = (t1 - t0) / (x1 - x0); t0 += (nx0 - x0) * d; t1 += (nx1 - x1) * d; }
        }
        x0 = nx0; x1 = nx1;
    }
    float z = -1.0f;
    if (st->othermode_l & RM_ZS_PRIM) z = (st->prim_depth & 0x7FFF) / 16383.5f - 1.0f;
    gfx_vtx_t v[4];
    memset(v, 0, sizeof(v));
    const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    for (int i = 0; i < 4; i++) {
        v[i].px = xs[i]; v[i].py = ys[i]; v[i].cz = z; v[i].cw = 1.0f;
        v[i].r = v[i].g = v[i].b = v[i].a = 0xFF;
    }
    if (textured) {
        /* corners: 0 = (s0,t0) at upper-left; flip swaps the s/t axes */
        float ss[4], tt[4];
        if (!flip) { ss[0] = s0; tt[0] = t0; ss[1] = s1; tt[1] = t0; ss[2] = s1; tt[2] = t1; ss[3] = s0; tt[3] = t1; }
        else       { ss[0] = s0; tt[0] = t0; ss[1] = s0; tt[1] = t1; ss[2] = s1; tt[2] = t1; ss[3] = s1; tt[3] = t0; }
        for (int i = 0; i < 4; i++) { v[i].s = ss[i]; v[i].t = tt[i]; }
    }
    uint16_t i0 = put_vtx(&v[0]), i1 = put_vtx(&v[1]), i2 = put_vtx(&v[2]), i3 = put_vtx(&v[3]);
    uint16_t *o = &g_ibuf[g_icount];
    o[0] = i0; o[1] = i1; o[2] = i2;
    o[3] = i0; o[4] = i2; o[5] = i3;
    g_icount += 6;
}

/* ---- window texture -------------------------------------------------------- */

/* The whole texture to transparent black, drawn in the command stream (a GX
 * fill would run before the frame's draws, not between two windows). */
static void rtt_clear(gfx_state_t *st) {
    if (!vtx_room(st, 4, 6)) return;
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvColor(env, 0);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    gfx_combiner_invalidate();
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    g_fx_epoch++;   /* set directly above */
    static const float xs[4] = { 0, RTT_W, RTT_W, 0 }, ys[4] = { 0, 0, RTT_H, RTT_H };
    static const uint8_t quad[6] = { 0, 1, 2, 0, 2, 3 };
    uint16_t first = (uint16_t)(g_vcount - g_vbase);
    for (int i = 0; i < 4; i++) {
        gpu_vtx_t *o = &g_vbuf[g_vcount++];
        memset(o, 0, sizeof(*o));
        o->x = xs[i]; o->y = ys[i]; o->z = -1.0f; o->w = 1.0f;
    }
    for (int i = 0; i < 6; i++) g_ibuf[g_icount + i] = (uint16_t)(first + quad[i]);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, g_ibuf + g_icount);
    g_icount += 6;
    g_batch_start = g_icount;
    g_state_valid = false;
}

bool gfx_backend_rtt_begin(gfx_state_t *st, uint32_t addr, uint16_t width) {
    if (!g_rtt_target || !g_draw_enabled || width == 0 || width > RTT_W) return false;
    {
        static bool logged;
        if (!logged) {
            logged = true;
            pm3ds_log(PM3DS_LOG_INFO, "gfx: first window texture draw (frame %u)", (unsigned)g_frame_no);
        }
    }
    gfx_backend_flush(st);
    C3D_FrameDrawOn(g_rtt_target); g_fx_epoch++;
    tex_bind_forget(&g_rtt_tex);   /* this window replaces the last one's texels */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uloc_screen, &g_rtt_mtx);
    g_in_rtt = true;
    rtt_clear(st);
    g_rtt_addr = addr;
    g_rtt_stride = (uint32_t)width * 2u;
    g_rtt_bytes = g_rtt_stride * RTT_H;
    st->dirty = GFX_DIRTY_ALL;
    return true;
}

void gfx_backend_rtt_end(gfx_state_t *st) {
    gfx_backend_flush(st);
    C3D_FrameDrawOn(g_cur_target); g_fx_epoch++;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uloc_screen, &g_screen_mtx);
    g_in_rtt = false;
    g_state_valid = false;
    st->dirty = GFX_DIRTY_ALL;
}

bool pm3ds_gfx_rtt_available(void) { return g_rtt_target != NULL; }
bool pm3ds_gfx_screen_copy_available(void) { return g_scr_ok; }

/* GX queue slots kept free for the rest of the frame: the draw-list splits,
 * both eyes' transfers and the frame end */
#define GX_QUEUE_RESERVE 8
static int g_scr_copies, g_scr_copies_max = 32;   /* this eye's screen copies, and its share */
static bool g_scr_made;                           /* g_scr_tex holds a copy (any frame, any eye) */

/* The game copies the framebuffer drawn so far into the image at addr: copy
 * the eye's render target into g_scr_tex instead. The draws so far go to the
 * GPU first (C3D_FrameSplit), then the copy runs in the same GX queue, so the
 * draws after it see the finished copy. */
void gfx_backend_screen_copy(gfx_state_t *st, uint32_t addr, uint16_t width) {
    if (!g_scr_ok || !g_draw_enabled || g_in_rtt || width == 0) return;
    /* a copy takes two GX queue slots (the split and the copy); several effects
     * copying every frame, in both eyes, can fill the queue and the frame end
     * needs a few more (GitHub #57: Super Block flames on Lavalava Island).
     * Without room the effect reads the last copy, a frame or a few draws old.
     * In 3D each eye gets its share, so the right eye isn't left without one. */
    extern int pm3ds_gx_queue_room(void);
    if (pm3ds_gx_queue_room() < 2 + GX_QUEUE_RESERVE || g_scr_copies >= g_scr_copies_max) {
        static unsigned skipped;
        if (skipped++ % 600 == 0) pm3ds_log(PM3DS_LOG_INFO, "gfx: screen copy skipped, GX queue full (%u)", skipped);
        if (g_scr_made) {   /* the last copy (this eye's, or the other eye's), not the black N64 buffer */
            g_scr_addr = addr;
            g_scr_stride = (uint32_t)width * 2u;
            g_scr_bytes = g_scr_stride * 240u;
        }
        return;
    }
    g_scr_copies++;
    g_scr_made = true;
    gfx_backend_flush(st);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    /* 50 rows of 30 tiles (8x8 RGBA8 = 256 bytes each); the texture's rows
     * are 32 tiles, so 2 tiles of gap after each row */
    GX_TextureCopy((u32 *)g_cur_target->frameBuf.colorBuf, GX_BUFFER_DIM(30 * 256 / 16, 0),
                   (u32 *)g_scr_tex.data, GX_BUFFER_DIM(30 * 256 / 16, 2 * 256 / 16),
                   30 * 256 * 50, GX_TRANSFER_RAW_COPY(1));
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    tex_bind_forget(&g_scr_tex);   /* new texels: the next bind clears the texture cache */
    g_scr_addr = addr;
    g_scr_stride = (uint32_t)width * 2u;
    g_scr_bytes = g_scr_stride * 240u;
    g_state_valid = false;
    st->dirty = GFX_DIRTY_ALL;
}

/* ---- frame ------------------------------------------------------------------ */

#define MAX_DLS_PER_FRAME 8

extern uint64_t pm3ds_t_game, pm3ds_t_present, pm3ds_t_wait;
extern volatile const char *pm3ds_bc_main;   /* platform/pm3ds_netlog.c */
extern volatile unsigned pm3ds_bc_frames;
extern unsigned pm3ds_tstat_uploads;
extern char pm3ds_perf_text[3][64];
uint64_t pm3ds_t_audio;
unsigned pm3ds_tstat_uploads;

/* Pause backdrop: the game wants the current frame as 320x240 RGBA5551 in
 * N64 byte order (gfx_transfer_frame_to_depth). Copy the previous top-screen
 * image off the render target with the GX engine and convert the 4:3 area. */
static uint16_t *g_capture_dst;
void pm3ds_request_frame_capture(uint16_t *dst) { g_capture_dst = dst; }

/* The game draws the previous frame from dst (320x240 RGBA5551) as textures:
 * copy it on the GPU (g_prev_tex), or on the CPU without a texture. */
void pm3ds_request_prev_frame(uint16_t *dst) {
#ifndef PM3DS_PREV_CPU   /* debug: the old CPU copy, to compare */
    if (g_prev_ok) g_prev_req = dst;
    else
#endif
        g_capture_dst = dst;
}

static void capture_frame(uint16_t *dst) {
    uint32_t *tmp = (uint32_t *)linearAlloc(240 * 400 * 4);
    if (!tmp) { pm3ds_log(PM3DS_LOG_WARN, "capture: no linear memory for the frame copy (free %u)", (unsigned)linearSpaceFree()); return; }
    /* outside a frame: citro3d queues GX commands issued mid-frame until
     * C3D_FrameEnd, so use its synchronous transfer (waits for the GPU) */
    C3D_SyncDisplayTransfer((u32 *)g_target->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), tmp, GX_BUFFER_DIM(240, 400),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(tmp, 240 * 400 * 4);
    /* LCD layout: column-major, x = screen column, bottom row first */
    for (int y = 0; y < 240; y++) {
        int sy = (int)(g_view_s * y + g_view_oy);
        for (int x = 0; x < 320; x++) {
            int sx = (int)(g_view_s * x + g_view_ox);
            uint32_t p = (sx >= 0 && sx < 400 && sy >= 0 && sy < 240) ? tmp[sx * 240 + (239 - sy)] : 0x000000FFu;
            uint16_t v = (uint16_t)((((p >> 24) & 0xFF) >> 3) << 11 | (((p >> 16) & 0xFF) >> 3) << 6 |
                                    (((p >> 8) & 0xFF) >> 3) << 1 | 1);
            dst[y * 320 + x] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
    /* the texture cache tells rewritten images apart by a few sampled bytes
     * per row; this frame's edges are often as black as the last one's, so
     * strips captured frames ago came back (torn, blocky battle-start wipe) */
    extern void gfx_tex_invalidate_range(const void *addr, size_t len);
    gfx_tex_invalidate_range(dst, 320 * 240 * sizeof(uint16_t));
#ifdef PM3DS_CAPTURE_LOG   /* debug: what the copy looked like (pause backdrop reports) */
    {
        uint64_t sum = 0;
        for (int i = 0; i < 240 * 400; i += 97) sum += (tmp[i] >> 8) & 0xFF;
        pm3ds_log(PM3DS_LOG_INFO, "CAPTURE frame -> %p, avg green %u, sample %08X",
                  (void *)dst, (unsigned)(sum / (240 * 400 / 97)), (unsigned)tmp[120 * 240 + 120]);
        /* what the game gets back: 8x6 cells of the 320x240 copy, RGB 4 bits each */
        char grid[8 * 6 * 4 + 8];
        int k = 0;
        for (int cy = 0; cy < 6; cy++) {
            for (int cx = 0; cx < 8; cx++) {
                uint32_t r = 0, g = 0, b = 0;
                for (int y = cy * 40 + 4; y < cy * 40 + 40; y += 8) {
                    for (int x = cx * 40 + 4; x < cx * 40 + 40; x += 8) {
                        uint16_t v = dst[y * 320 + x];
                        v = (uint16_t)((v >> 8) | (v << 8));
                        r += (v >> 11) & 31; g += (v >> 6) & 31; b += (v >> 1) & 31;
                    }
                }
                k += snprintf(grid + k, sizeof(grid) - k, "%X%X%X ", (unsigned)(r / 25 >> 1), (unsigned)(g / 25 >> 1),
                              (unsigned)(b / 25 >> 1));
            }
            if (cy == 2) { pm3ds_log(PM3DS_LOG_INFO, "CAPTURE grid %s", grid); k = 0; }
        }
        pm3ds_log(PM3DS_LOG_INFO, "CAPTURE grid %s", grid);
    }
#endif
    linearFree(tmp);
}

/* 3D slider position, 0 = down (mono). Not on the Old 3DS: drawing every
 * frame twice would halve its frame rate (Options says 3D is off there). */
static float slider_now(void) {
    extern bool pm3ds_lite_effects;
    if (pm3ds_lite_effects) return 0.0f;
#ifdef PM3DS_FORCE_STEREO   /* debug builds: the emulator's slider stays at 0 */
    return 1.0f;
#else
    return osGet3DSliderState();
#endif
}

#ifdef PM3DS_STEP_PROF
uint64_t pm3ds_gp_ticks[GP_COUNT];
static unsigned g_gp_renders;

/* Sampling profiler: a thread on another core reads, 1000 times a second,
 * which phase (and display list command) the main thread is in. Exclusive
 * shares at almost no cost to the measured code. */
volatile uint8_t pm3ds_prof_phase, pm3ds_prof_op;
static volatile uint32_t g_samples[PH_COUNT + 256];
static void sampler_entry(void *arg) {
    (void)arg;
    for (;;) {
        svcSleepThread(1000 * 1000);
        uint8_t ph = pm3ds_prof_phase;
        g_samples[ph == PH_WALK ? PH_COUNT + pm3ds_prof_op : ph]++;
    }
}
static void sampler_start(void) {
    static bool started;
    if (started) return;
    started = true;
    /* New 3DS: core 2, so the sampler doesn't share the mixer's time-limited core 1
     * (only asked there: Azahar in Old 3DS mode aborts on a core 2 thread) */
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    Thread t = n3ds ? threadCreate(sampler_entry, NULL, 0x1000, 0x30, 2, true) : NULL;
    if (!t) t = threadCreate(sampler_entry, NULL, 0x1000, 0x30, 1, true);   /* below the audio mixer */
    pm3ds_log(PM3DS_LOG_INFO, "prof: sampler %s", t ? "running" : "failed");
}
static void sampler_report(void) {
    static const char *const names[PH_COUNT] = { "walk", "state", "draw", "tex", "game", "wait", "bottom", "end", "present",
                                                    "comb", "ctx", "fx", "put", "g_scripts", "g_mode", "g_ent", "g_gfx",
                                                    "w_enc", "w_npcs", "w_player", "w_items", "w_fx", "w_models", "w_cam",
                                                    "r_ent", "r_models", "r_player", "r_npcs", "r_workers", "r_fx", "r_tasks", "r_sort" };
    uint32_t snap[PH_COUNT + 256], total = 0;
    for (int i = 0; i < PH_COUNT + 256; i++) { snap[i] = g_samples[i]; g_samples[i] = 0; total += snap[i]; }
    if (!total) return;
    char buf[320];
    int n = snprintf(buf, sizeof buf, "SAMPLE n=%lu", (unsigned long)total);
    for (int i = 0; i < PH_SUB; i++)
        if (i != PH_WALK && snap[i]) n += snprintf(buf + n, sizeof buf - n, " %s=%lu%%", names[i], (unsigned long)(snap[i] * 100 / total));
    pm3ds_log(PM3DS_LOG_INFO, "%s", buf);
    /* per mille: the log line is at most 256 bytes, so the finer phases get their own */
    n = snprintf(buf, sizeof buf, "SAMPLE sub");
    for (int i = PH_SUB; i < PH_COUNT; i++)
        if (snap[i]) n += snprintf(buf + n, sizeof buf - n, " %s=%lu", names[i], (unsigned long)(snap[i] * 1000 / total));
    pm3ds_log(PM3DS_LOG_INFO, "%s", buf);
    {   /* which citro3d state groups each draw re-sent (C3Di_UpdateContext), per draw x100 */
        extern unsigned pm3ds_c3d_flag_draws, pm3ds_c3d_flag_cnt[32];
        if (pm3ds_c3d_flag_draws) {
            n = snprintf(buf, sizeof buf, "C3DFLAGS draws=%u", pm3ds_c3d_flag_draws);
            for (int b = 0; b < 32; b++)
                if (pm3ds_c3d_flag_cnt[b]) n += snprintf(buf + n, sizeof buf - n, " %d=%u", b, pm3ds_c3d_flag_cnt[b] * 100 / pm3ds_c3d_flag_draws);
            pm3ds_log(PM3DS_LOG_INFO, "%s", buf);
            memset(pm3ds_c3d_flag_cnt, 0, sizeof pm3ds_c3d_flag_cnt);
            pm3ds_c3d_flag_draws = 0;
        }
    }
    n = snprintf(buf, sizeof buf, "SAMPLE ops");
    for (int k = 0; k < 10 && n < (int)sizeof buf - 16; k++) {   /* the busiest display list commands */
        int best = -1;
        for (int i = 0; i < 256; i++) if (snap[PH_COUNT + i] && (best < 0 || snap[PH_COUNT + i] > snap[PH_COUNT + best])) best = i;
        if (best < 0) break;
        n += snprintf(buf + n, sizeof buf - n, " %02X=%lu%%", best, (unsigned long)(snap[PH_COUNT + best] * 100 / total));
        snap[PH_COUNT + best] = 0;
    }
    pm3ds_log(PM3DS_LOG_INFO, "%s", buf);
}
#endif

#ifdef PM3DS_MEMBENCH
#include <stdlib.h>
/* debug: what a vertex store costs on this CPU, by destination (log only) */
__attribute__((noinline)) static void mb_fill(gpu_vtx_t *o, int n, unsigned wrap) {
    for (int i = 0; i < n; i++) {
        gpu_vtx_t *d = &o[wrap ? (i & (wrap - 1)) : i];
        float f = (float)i;
        d->x = f; d->y = f * 0.5f; d->z = f * 0.25f; d->w = 1.0f;
        d->r = d->g = d->b = d->a = (uint8_t)i;
        d->u0 = f * 0.1f + 1.0f; d->v0 = f * 0.2f + 2.0f; d->u1 = f * 0.3f; d->v1 = f * 0.4f;
    }
}
__attribute__((noinline)) static float mb_read(const gpu_vtx_t *o, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += o[i].x + o[i].v1;
    return s;
}
void pm3ds_membench(void *lin) {
    enum { N = 8192 };
    gpu_vtx_t *heap = (gpu_vtx_t *)malloc(sizeof(gpu_vtx_t) * N);
    if (!heap || !lin) return;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t t[8];
        t[0] = svcGetSystemTick();
        mb_fill((gpu_vtx_t *)lin, N, 0);          /* linear heap (the GPU vertex buffer) */
        t[1] = svcGetSystemTick();
        mb_fill(heap, N, 0);                      /* normal heap */
        t[2] = svcGetSystemTick();
        mb_fill(heap, N, 128);                    /* 4.5 KB ring, stays in L1 */
        t[3] = svcGetSystemTick();
        memcpy(lin, heap, sizeof(gpu_vtx_t) * N); /* heap -> linear copy */
        t[4] = svcGetSystemTick();
        volatile float r1 = mb_read((const gpu_vtx_t *)lin, N);
        t[5] = svcGetSystemTick();
        volatile float r2 = mb_read(heap, N);
        t[6] = svcGetSystemTick();
        for (int i = 0; i < 1000; i++) (void)svcGetSystemTick();
        t[7] = svcGetSystemTick();
        (void)r1; (void)r2;
        double k = 1e9 / SYSCLOCK_ARM11 / N;   /* ns per vertex */
        pm3ds_log(PM3DS_LOG_INFO, "MEMBENCH pass %d ns/vtx: fill linear %.1f heap %.1f l1 %.1f | copy heap->linear %.1f | read linear %.1f heap %.1f | svcGetSystemTick %.0f ns",
                  pass, (t[1] - t[0]) * k, (t[2] - t[1]) * k, (t[3] - t[2]) * k, (t[4] - t[3]) * k, (t[5] - t[4]) * k,
                  (t[6] - t[5]) * k, (t[7] - t[6]) * 1e9 / SYSCLOCK_ARM11 / 1000);
    }
    free(heap);
}
#endif

static bool g_skip_auto;   /* Old 3DS (pm3ds_gfx_extra_steps) */

/* CPU time of this game step's render_lists calls (waits left out), and the
 * GPU's time on the frames before them (citro3d measures each frame from
 * C3D_FrameEnd until the GPU is done; the next C3D_FrameBegin waits for it) */
static uint64_t g_step_render_ticks;
static float g_step_gpu_ms;
static u32 g_frame_vbl;   /* vblank count when the last frame began (C3D_FrameRate 60: every vblank) */
static int g_step_renders;
/* Old 3DS: this frame is late, start it now instead of at the next vblank */
static bool g_late_start;

/* Translate and draw the frame's display lists (once per eye in stereo). */
static void render_lists(const void *const *lists, int nlists) {
#if defined(PM3DS_DRAWLOG_ALLTRI) && defined(PM3DS_DRAWLOG_FRAME)
    { extern int pm3ds_gfx_dbg_vtx; if (g_frame_no == pm3ds_drawlog_frame) pm3ds_gfx_dbg_vtx = 400; }
#endif
#ifdef PM3DS_FRAME_DUMP
    {   /* debug: the last PM3DS_FRAME_DUMP_N frames shown from frame PM3DS_FRAME_DUMP on, every
         * one (60 fps in-between frames too), as 400x240 RGB565, appended to
         * sdmc:/3ds/StarHaven/frames.raw */
#ifndef PM3DS_FRAME_DUMP_N
#define PM3DS_FRAME_DUMP_N 24
#endif
        static int dumped;
        static bool prev_inb;   /* the frame on the target was an in-between one */
        bool inb = prev_inb;
        prev_inb = gfx_interp_in_between();
        if (g_frame_no >= PM3DS_FRAME_DUMP && dumped < PM3DS_FRAME_DUMP_N) {
            static uint16_t img[400 * 240];   /* the whole top screen, RGB565 */
            uint32_t *tmp = (uint32_t *)linearAlloc(240 * 400 * 4);
            if (tmp) {   /* as capture_frame */
                C3D_SyncDisplayTransfer((u32 *)g_target->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), tmp, GX_BUFFER_DIM(240, 400),
                                        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
                GSPGPU_InvalidateDataCache(tmp, 240 * 400 * 4);
                for (int y = 0; y < 240; y++)
                    for (int x = 0; x < 400; x++) {
                        uint32_t p = tmp[x * 240 + (239 - y)];
                        img[y * 400 + x] = (uint16_t)((((p >> 24) & 0xFF) >> 3) << 11 | (((p >> 16) & 0xFF) >> 2) << 5 |
                                                      (((p >> 8) & 0xFF) >> 3));
                    }
                linearFree(tmp);
            }
            FILE *f = fopen("sdmc:/3ds/StarHaven/frames.raw", dumped ? "ab" : "wb");
            if (f) { fwrite(img, 2, 400 * 240, f); fclose(f); }
            pm3ds_log(PM3DS_LOG_INFO, "FRAMEDUMP %d (step frame %u, in-between %d)", dumped, (unsigned)g_frame_no,
                      inb ? 1 : 0);
            dumped++;
        }
    }
#endif
    uint64_t t0 = svcGetSystemTick();
    /* C3D_FRAME_SYNCDRAW in two steps, so a hang report can tell a missing
     * vblank from a GPU that never finished the previous frame */
    pm3ds_bc_main = "vblank wait";
    PROF_PHASE(PH_WAIT);
    /* A frame that is already late starts right away: waiting for the next
     * vblank turned a 34 ms frame into a 50 ms one (a steady 20 fps on an Old
     * 3DS where most frames missed 33 ms by a millisecond). */
    bool late = g_late_start;
    g_late_start = false;
    if (!late) C3D_FrameSync();
    g_frame_vbl = C3D_FrameCounter(0);
    pm3ds_bc_main = "gpu wait";
    C3D_FrameBegin(0);
    /* the last frame asked for its buffer swap when the GPU finished it
     * (C3D_FrameBegin waited for that); see the wait before C3D_FrameEnd */
    u32 swap_vbl = C3D_FrameCounter(0);
    pm3ds_t_wait += svcGetSystemTick() - t0;
    g_step_gpu_ms += C3D_GetDrawingTime();
    uint64_t t_busy = svcGetSystemTick();
    if (g_frame_no <= 5)
        pm3ds_log(PM3DS_LOG_INFO, "gfx: frame %u begins after %llu ms", (unsigned)g_frame_no,
                  (unsigned long long)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000)));
    pm3ds_bc_main = "draw lists";
    PROF_PHASE(PH_PRESENT);
    if (g_prev_req) {   /* queued before the clear below: the target still shows the last frame */
        GX_TextureCopy((u32 *)g_target->frameBuf.colorBuf, GX_BUFFER_DIM(30 * 256 / 16, 0),
                       (u32 *)g_prev_tex.data, GX_BUFFER_DIM(30 * 256 / 16, 2 * 256 / 16),
                       30 * 256 * 50, GX_TRANSFER_RAW_COPY(1));
        g_prev_addr = (uint32_t)(uintptr_t)g_prev_req;
        g_prev_bytes = 320 * 2 * 240;
        g_prev_ttl = 3;
        g_prev_req = NULL;
    } else if (g_prev_ttl > 0 && --g_prev_ttl == 0) {
        g_prev_bytes = 0;
    }
    if (g_top_want > 0) {   /* also before the clear: the last frame's top screen */
        g_top_want--;
        if (!g_top_tried) {
            g_top_tried = true;
            bool vram = C3D_TexInitVRAM(&g_top_tex, SCR_W, SCR_H, GPU_RGBA8);
            g_top_ok = vram || C3D_TexInit(&g_top_tex, SCR_W, SCR_H, GPU_RGBA8);
            pm3ds_log(PM3DS_LOG_INFO, "gfx: top-screen copy for the bottom screen in %s",
                      !g_top_ok ? "nothing (no memory)" : vram ? "VRAM" : "linear memory");
            if (g_top_ok) {
                C3D_TexSetFilter(&g_top_tex, GPU_LINEAR, GPU_LINEAR);
                C3D_TexSetWrap(&g_top_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            }
        }
        if (g_top_ok && (!g_skip_auto || !g_top_valid || (g_frame_no & 3) == 0)) {
            GX_TextureCopy((u32 *)g_target->frameBuf.colorBuf, GX_BUFFER_DIM(30 * 256 / 16, 0),
                           (u32 *)g_top_tex.data, GX_BUFFER_DIM(30 * 256 / 16, 2 * 256 / 16),
                           30 * 256 * 50, GX_TRANSFER_RAW_COPY(1));
            g_top_valid = true;
        }
    } else {
        g_top_valid = false;
    }
    C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, 0x000000FFu, 0);
    C3D_FrameDrawOn(g_target); g_fx_epoch++;
    gfx_combiner_invalidate();   /* the bottom screen set its own stages last frame */
    tex_bind_invalidate();
    g_fog_lut_bound = false;
    g_fx_epoch++;
    g_cur_target = g_target;
    g_in_rtt = false;
    g_rtt_bytes = 0;
    g_scr_bytes = 0;
    g_scr_copies = 0;
    C3D_BindProgram(&g_prog);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uloc_screen, &g_screen_mtx);
    {   /* a new noise pattern every frame */
        static uint32_t r = 0x9E3779B9u;
        r = r * 1664525u + 1013904223u;
        g_noise_ofs_u = (float)((r >> 8) & 63) / 64.0f;
        g_noise_ofs_v = (float)((r >> 16) & 63) / 64.0f;
    }
    C3D_SetAttrInfo(&g_attr);
    g_vcount = g_icount = g_batch_start = 0;
    set_vbase(0);   /* also C3D_SetBufInfo */

    g_state_valid = false;
    g_cmdbuf_full = false;
    g_draw_enabled = true;
    g_stat_draws = 0;
    pm3ds_tstat_ops = pm3ds_tstat_tris = pm3ds_tstat_rects = pm3ds_tstat_culled = 0;
    pm3ds_tstat_culldl = pm3ds_tstat_culldl_hit = 0;
    g_stat_offscreen = 0;

    { extern int pm3ds_gfx_dbg_vtx;
      (void)pm3ds_gfx_dbg_vtx; }
    /* 3D slider: walk the lists once per eye with a horizontal disparity
     * (see pm3ds_stereo_shift); the first list is the background, which is
     * pushed to infinity. Mono costs nothing extra. */
    float slider = slider_now();
    bool stereo = slider > 0.02f;
    static bool stereo_on;
    if (stereo != stereo_on) {
        gfxSet3D(stereo);
        stereo_on = stereo;
    }
    /* pixels per eye at infinity with the slider at the top (Options: 3D depth) */
    const float max_shift = pm3ds_depth3d_shift[pm3ds_cfg.depth3d % DEPTH3D_LEVELS];
    for (int eye = 0; eye < (stereo ? 2 : 1); eye++) {
        if (eye == 1) {
            gfx_interp_second_eye();
            gfx_backend_flush(&g_st);
            C3D_RenderTargetClear(g_target_right, C3D_CLEAR_ALL, 0x000000FFu, 0);
            C3D_FrameDrawOn(g_target_right); g_fx_epoch++;
            g_cur_target = g_target_right;
            g_rtt_bytes = 0;
            g_scr_bytes = 0;
            g_scr_copies = 0;
            g_state_valid = false;
        }
        g_scr_copies_max = stereo ? 5 : 32;   /* 3D: 5 copies (10 queue slots) per eye */
        pm3ds_stereo_shift = stereo ? (eye == 0 ? -1.0f : 1.0f) * slider * max_shift : 0.0f;
        for (int i = 0; i < nlists; i++) {
            g_st.cimg_is_screen = true;
            g_stereo_far = stereo && i == 0 && nlists > 1;
            g_st.bg_wide = view_wide() && i == 0 && nlists > 1 &&
                           gfx_interp_bg_edges(&g_st.bg_left, &g_st.bg_right);
            g_st.bg_wide_l = -g_view_ox / g_view_s - max_shift - 3.0f;   /* room for the 3D shift */
            g_st.bg_wide_r = (400.0f - g_view_ox) / g_view_s + max_shift + 3.0f;
            g_st.bg_shift = i == 0 && nlists > 1 ? gfx_interp_bg_shift(&g_st.bg_left, &g_st.bg_right) : 0.0f;
            GP_BEGIN(tw);
            gfx_walk_dl(&g_st, lists[i]);
            GP_END(tw, GP_WALK);
            g_st.bg_shift = 0.0f;
            g_st.bg_wide = false;
            if (g_st.cimg_rtt) {   /* a list that ended inside a window texture */
                gfx_backend_rtt_end(&g_st);
                g_st.cimg_rtt = false;
            }
        }
        g_stereo_far = false;
    }
    pm3ds_stereo_shift = 0.0f;
    gfx_backend_flush(&g_st);
    g_stat_verts = (unsigned)g_vcount;

    pm3ds_bc_main = "bottom screen";
    {
        extern void pm3ds_bottom_screen_render(C3D_RenderTarget *, C3D_RenderTarget *);
        /* Old 3DS: every 4th frame, every other one for half a second after
         * a touch (it holds its image in between, and touches are read in
         * pm3ds_bottom_screen_tick either way); ~1.3 ms a draw */
        static unsigned bottom_n, touch_frames;
        extern bool pm3ds_new_game_picker_blocks_input(void);   /* the picker draws on the top screen too */
        if (hidKeysHeld() & KEY_TOUCH) touch_frames = 15;
        else if (touch_frames) touch_frames--;
        extern bool pm3ds_retry_prompt_up(void);   /* Retry battle's choice is on the top screen too (retry_battle.c) */
        if (!g_skip_auto || (bottom_n++ & (touch_frames ? 1 : 3)) == 0 || pm3ds_new_game_picker_blocks_input()
            || pm3ds_retry_prompt_up()) {
            GP_BEGIN(tb);
            PROF_PHASE(PH_BOTTOM);
            pm3ds_bottom_screen_render(g_target, stereo ? g_target_right : NULL);
            GP_END(tb, GP_BOTTOM);
        }
    }
#ifdef PM3DS_SLOW_MS   /* test: a slower console (frame skip, pacing) */
    for (uint64_t t = svcGetSystemTick(); svcGetSystemTick() - t < (uint64_t)SYSCLOCK_ARM11 / 1000 * PM3DS_SLOW_MS;) {}
#endif
    pm3ds_bc_main = "frame end";
    GP_BEGIN(te);
    PROF_PHASE(PH_END);
    /* Without flags citro3d flushes the whole linear heap (32 MB) from the
     * CPU cache every frame. The GPU only needs what was written for it: this
     * frame's vertices, the bottom screen's (flushed there), textures
     * (flushed on upload) and the command list (GX_CMDLIST_FLUSH). */
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_vbuf, (u32)(sizeof(gpu_vtx_t) * g_vcount));
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_ibuf, (u32)(sizeof(uint16_t) * g_icount));
    if (late) {
        /* not started on a vblank: let one pass since the last frame's swap
         * request, so this frame's copy goes to the buffer that is really in
         * the back (gfxScreenSwapBuffers flips libctru's index at once, the
         * screen at the next vblank). A Toad Town frame takes ~28 ms, so
         * this rarely waits. */
        uint64_t tw = svcGetSystemTick();
        PROF_PHASE(PH_WAIT);
        while (C3D_FrameCounter(0) == swap_vbl) C3D_FrameSync();
        pm3ds_t_wait += svcGetSystemTick() - tw;
        t_busy += svcGetSystemTick() - tw;   /* not render time */
        PROF_PHASE(PH_END);
    }
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    GP_END(te, GP_FRAMEEND);
    {   /* Select + R: both screens to PNG files (platform/pm3ds_screenshot.c) */
        extern void pm3ds_screenshot_after_frame(C3D_RenderTarget *top);
        pm3ds_screenshot_after_frame(g_target);
    }
    pm3ds_bc_main = "frame done";
    g_step_render_ticks += svcGetSystemTick() - t_busy;
    g_step_renders++;
#ifdef PM3DS_STEP_PROF
    pm3ds_gp_ticks[GP_RENDER] += svcGetSystemTick() - t_busy;
    g_gp_renders++;
    g_gp_verts += (unsigned)g_vcount;
#endif
}

/* 60 fps with the 3D slider up: every frame is drawn once per eye, so a game
 * step walks the lists four times. On a New 3DS that fits in most places but
 * not in all of them, so the in-between frames stay on only while there is
 * room: each frame's CPU and GPU work, one after the other, has to fit in one
 * 60 Hz frame (16.7 ms), and the game step runs next to the second one's GPU
 * work. Every time 60 fps doesn't hold, 30 fps is kept twice as long before the
 * next try (2 s up to 16 s), so a heavy room doesn't flip between the two. */
uint64_t pm3ds_t_game_step;            /* port_main.c: the last game step's CPU time */
static bool g_stereo_interp;
static float g_si_render_ms, g_si_gpu_ms, g_si_game_ms;   /* running averages */
static float g_si_late;                /* goes up by 1 per late step, down by 0.2 per step on time */
static int g_si_hold, g_si_backoff, g_si_on_steps;
static bool g_si_was_stereo;

static void stereo_interp_update(bool stereo, bool counted, bool interp_was_on, unsigned vblanks) {
    float render_ms = g_step_renders ? (float)g_step_render_ticks * 1000.0f / SYSCLOCK_ARM11 / g_step_renders : 0.0f;
    float gpu_ms = g_step_renders ? g_step_gpu_ms / g_step_renders : 0.0f;
    float game_ms = (float)pm3ds_t_game_step * 1000.0f / SYSCLOCK_ARM11;
    g_step_render_ticks = 0;
    g_step_gpu_ms = 0.0f;
    g_step_renders = 0;
    if (!stereo) {
        g_si_was_stereo = false;
        return;
    }
    if (!g_si_was_stereo) {   /* slider just went up: measure a few 30 fps steps first */
        g_si_was_stereo = true;
        g_stereo_interp = false;
        g_si_render_ms = render_ms;
        g_si_gpu_ms = gpu_ms;
        g_si_game_ms = game_ms;
        g_si_hold = 8;
        g_si_late = 0.0f;
        g_si_backoff = 0;
        return;
    }
    {   /* battles stay at 30 with the slider up: the in-between frames rarely
         * fit there, and dropping in and out of 60 felt worse than a steady 30 */
        extern s32 get_game_mode(void);
        static bool was_battle;
        bool battle = get_game_mode() == 8;   /* GAME_MODE_BATTLE */
        if (battle != was_battle) {
            was_battle = battle;
            g_stereo_interp = false;
            g_si_late = 0.0f;
            g_si_backoff = 0;
            g_si_hold = 8;   /* after a battle: measure a few steps at 30, then decide */
            pm3ds_log(PM3DS_LOG_INFO, "3D: %s", battle ? "battle, 30 fps" : "battle over");
        }
        if (battle) return;
    }
    if (!counted || render_ms == 0.0f) return;   /* fast-forward, or nothing new drawn */
    g_si_render_ms += (render_ms - g_si_render_ms) * 0.2f;
    g_si_gpu_ms += (gpu_ms - g_si_gpu_ms) * 0.2f;
    g_si_game_ms += (game_ms - g_si_game_ms) * 0.2f;
    /* a step that took more than two 60 Hz frames ran late */
    if (interp_was_on) {
        g_si_late = vblanks > 2 ? g_si_late + 1.0f : (g_si_late > 0.2f ? g_si_late - 0.2f : 0.0f);
        if (++g_si_on_steps == 600) g_si_backoff = 0;   /* held for 20 s: back to short waits */
    }
    if (g_si_hold > 0) {
        g_si_hold--;
        return;
    }
    /* a frame's CPU and GPU work; the game step overlaps the GPU's */
    float need = g_si_render_ms + (g_si_gpu_ms > g_si_game_ms ? g_si_gpu_ms : g_si_game_ms);
#ifdef PM3DS_SI_FORCE   /* debug: the emulator's timing says nothing about a 3DS */
    need = 0.0f;
    g_si_late = 0.0f;
#endif
    bool on = g_stereo_interp;
    if (on && (need > 16.0f || g_si_late >= 3.0f)) on = false;
    else if (!on && need < 14.0f) on = true;
    if (on != g_stereo_interp) {
        pm3ds_log(PM3DS_LOG_INFO, "3D: %s fps (render %.1f ms, gpu %.1f ms, game %.1f ms, late %.1f, wait %d s)",
                  on ? "60" : "30", g_si_render_ms, g_si_gpu_ms, g_si_game_ms, g_si_late, on ? 0 : 2 << g_si_backoff);
        g_stereo_interp = on;
        if (on) {
            g_si_hold = 30;
            g_si_on_steps = 0;
        } else {
            g_si_hold = 60 << g_si_backoff;   /* 2 s, 4 s, 8 s, 16 s */
            if (g_si_backoff < 3) g_si_backoff++;
        }
        g_si_late = 0.0f;
    }
}

/* Frame skip (Old 3DS): game steps owed because frames came late; the main
 * loop runs up to one of them per pass on top of the normal step. */
static bool g_skip_auto, g_skip_auto_set;
static float g_skip_debt;
static int g_skipped_steps;   /* extra steps run before this frame (no in-between frame then) */
int pm3ds_gfx_extra_steps(void) {
    if (!g_skip_auto_set) {
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
#ifdef PM3DS_SIM_O3DS
        n3ds = false;
#endif
        g_skip_auto = !n3ds;
        g_skip_auto_set = true;
    }
    g_skipped_steps = 0;
#ifdef PM3DS_TEST_CATCHUP   /* test: an Old 3DS that is always one frame behind */
    if (g_skip_auto) return g_skipped_steps = 1;
#endif
    if (g_skip_debt >= 1.0f) {
        g_skip_debt -= 1.0f;
        g_skipped_steps = 1;
    }
    return g_skipped_steps;
}

/* loading frames drawn / skipped because the GPU was still busy (netlog) */
volatile unsigned pm3ds_load_frames_ok, pm3ds_load_frames_busy;

/* Boot: the loading panel on both screens while the ROM loads. At most ~10
 * a second, and never waits for the GPU or vsync: the load is what matters. */
void pm3ds_gfx_present_loading(int permille) {
    static uint64_t last;
    uint64_t now = svcGetSystemTick();
    if (last && now - last < SYSCLOCK_ARM11 / 10 && permille < 1000) return;
    if (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        pm3ds_load_frames_busy++;
        return;
    }
    pm3ds_load_frames_ok++;
    last = now;
    C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, 0x000000FFu, 0);
    { extern void pm3ds_ui_loading(C3D_RenderTarget *, int); pm3ds_ui_loading(g_target, permille); }
    C3D_FrameEnd(0);
}

/* Boot problems (no ROM...): the message on both screens */
void pm3ds_gfx_present_message(const char *const *lines, int n) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(g_target, C3D_CLEAR_ALL, 0x000000FFu, 0);
    { extern void pm3ds_ui_message(C3D_RenderTarget *, const char *const *, int); pm3ds_ui_message(g_target, lines, n); }
    C3D_FrameEnd(0);
}

static uint32_t g_shown;   /* frames put on screen (60 a second with interpolation) */

void pm3ds_gfx_present_frame(void) {
    PROF_PHASE(PH_PRESENT);
    g_frame_no++;
    pm3ds_bc_frames = g_frame_no;
    if (!g_st_inited) { gfx_state_init(&g_st); g_st_inited = true; }
    g_st.decal_band = false;   /* a frame cut short can't leave it on */

#ifdef PM3DS_CAPTURE_TEST
    if (g_frame_no % 600 == 0) {
        static uint16_t test_buf[320 * 240];
        capture_frame(test_buf);
    }
#endif
#if defined(PM3DS_MAGENTA_HUNT) && defined(PM3DS_DRAWLOG_FRAME)
    {   /* debug: read the previous frame back; if it is tinted magenta, log
         * every draw of this one */
        static int hits;
        uint32_t *tmp = hits < 2 ? (uint32_t *)linearAlloc(240 * 400 * 4) : NULL;
        if (tmp) {
            C3D_SyncDisplayTransfer((u32 *)g_target->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), tmp, GX_BUFFER_DIM(240, 400),
                                    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
            GSPGPU_InvalidateDataCache(tmp, 240 * 400 * 4);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int x = 60; x < 340; x += 7) {
                for (int y = 20; y < 220; y += 7, n++) {
                    uint32_t px = tmp[x * 240 + y];
                    r += px >> 24; g += (px >> 16) & 0xFF; b += (px >> 8) & 0xFF;
                }
            }
            r /= n; g /= n; b /= n;
            if (r > g + 40 && b > g + 40) {
                hits++;
                pm3ds_drawlog_frame = g_frame_no;
                pm3ds_log(PM3DS_LOG_WARN, "MAGENTA frame %u avg %u,%u,%u: logging this frame's draws",
                          (unsigned)g_frame_no, (unsigned)r, (unsigned)g, (unsigned)b);
            }
            linearFree(tmp);
        }
    }
#endif
    if (g_capture_dst) {   /* the previous frame is still on the target */
        capture_frame(g_capture_dst);
        g_capture_dst = NULL;
    }
    /* All display lists the game submitted since the last frame, in order
     * (background list, then the main list). With nothing new (loading,
     * DISABLE_DRAW_FRAME) the screen keeps the last frame: only keep the
     * 30 Hz pace. */
    static const void *last[MAX_DLS_PER_FRAME];
    static int nlast;
    const void *now[MAX_DLS_PER_FRAME];
    int n = 0;
    pm3ds_gfx_drain_dl_list(now, MAX_DLS_PER_FRAME, &n);
    if (n > 0) { memcpy(last, now, sizeof(now[0]) * n); nlast = n; }
    bool render = n > 0 || nlast == 0;
    /* 60 fps: one game step spans two frames. New lists are drawn twice,
     * first half way from the last step's matrices (gfx_interp.c), then as
     * they are. With the 3D slider up only while there is time for both
     * eyes twice (stereo_interp_update). */
    bool fps60 = pm3ds_fps60;
    /* Pacing: citro3d counts every vblank (port_main.c: rate 60) and a game step is due
     * every second one. A frame that starts late starts at the next vblank
     * instead of the next 30 Hz tick (a 40 ms frame showed every 66 ms), and
     * on the Old 3DS the steps it fell behind are made up by running extra
     * game steps without drawing them (pm3ds_gfx_extra_steps). */
    static u32 due;           /* vblank the next step's first frame is due at */
    static bool due_valid;
    {   /* wait for it; render_lists' C3D_FrameSync waits the last vblank */
        PROF_PHASE(PH_WAIT);
        uint64_t t0 = svcGetSystemTick();
        pm3ds_bc_main = "frame sync";
        while (due_valid && (s32)(due - (render ? 1u : 0u) - C3D_FrameCounter(0)) > 0) C3D_FrameSync();
        pm3ds_t_wait += svcGetSystemTick() - t0;
    }
    u32 step_vbl = C3D_FrameCounter(0) + (render ? 1u : 0u);
    bool interp_on = false;
    /* Old 3DS, frame due already: start it now (render_lists) */
    g_late_start = render && g_skip_auto && due_valid && !pm3ds_fast_forward() &&
                   (s32)(C3D_FrameCounter(0) - due) >= 0;
    if (render) {
        bool interp = fps60 && n > 0 && (slider_now() <= 0.02f || g_stereo_interp) && !pm3ds_fast_forward() &&
                      !g_skipped_steps;
#ifdef PM3DS_INTERP_ONLY   /* test: show only the in-between frames (pairing errors stay on screen) */
        if (interp) {
            gfx_interp_begin_walk(0.5f, true);
            render_lists(last, nlast);
            step_vbl = g_frame_vbl;
            g_shown++;
            gfx_interp_end_step();
            goto drawn;
        }
#endif
        interp_on = interp;
        bool first = true;
        if (interp) {
            gfx_interp_begin_walk(0.5f, false);
            render_lists(last, nlast);
            step_vbl = g_frame_vbl;
            first = false;
            g_shown++;
        }
        gfx_interp_begin_walk(1.0f, n > 0);
        render_lists(last, nlast);
        if (first) step_vbl = g_frame_vbl;
        g_shown++;
        if (n > 0) gfx_interp_end_step();
    }
#ifdef PM3DS_INTERP_ONLY
drawn:
#endif
    if (due_valid && render && n > 0 && g_skip_auto && !pm3ds_fast_forward()) {
        /* steps this frame came late by, owed to the game (capped: a long
         * load is not made up for) */
        int late = (s32)(step_vbl - due);
        if (late > 0) g_skip_debt += late * 0.5f;
        if (g_skip_debt > 2.0f) g_skip_debt = 2.0f;
    }
    due = step_vbl + 2;
    due_valid = true;
    {
        static u32 last_vbl;
        u32 vbl = C3D_FrameCounter(0);
#ifdef PM3DS_STEP_PROF
        {   /* debug: a slow draw, and how many textures it uploaded */
            static unsigned up0;
            float rms = (float)g_step_render_ticks * 1000.0f / SYSCLOCK_ARM11;
            unsigned up = pm3ds_tstat_uploads >= up0 ? pm3ds_tstat_uploads - up0 : pm3ds_tstat_uploads;
            if (rms > 20.0f)
                pm3ds_log(PM3DS_LOG_INFO, "SLOWDRAW %.1fms (gpu %.1f) in %d renders, %u uploads, vbl %u", rms,
                          g_step_gpu_ms, g_step_renders, up, (unsigned)(vbl - last_vbl));
            up0 = pm3ds_tstat_uploads;
        }
#endif
        stereo_interp_update(slider_now() > 0.02f, fps60 && n > 0 && !pm3ds_fast_forward(), interp_on, vbl - last_vbl);
        last_vbl = vbl;
    }

#ifdef PM3DS_TITLE_DBG
    if ((g_frame_no % 300) == 0) {
        pm3ds_log(PM3DS_LOG_INFO, "GFXSTAT f=%lu dls=%d ops=%u tris=%u culled=%u offscr=%u rects=%u verts=%u draws=%u",
                  (unsigned long)g_frame_no, nlast, pm3ds_tstat_ops, pm3ds_tstat_tris, pm3ds_tstat_culled,
                  g_stat_offscreen, pm3ds_tstat_rects, g_stat_verts, g_stat_draws);
    }
#endif
    { extern void pm3ds_game_debug_tick(uint32_t); pm3ds_game_debug_tick(g_frame_no); }
    { extern void pm3ds_bottom_screen_tick(unsigned); pm3ds_bottom_screen_tick(g_frame_no); }
    /* once a second: measured frame rate and where the CPU time went
     * (game = gfxRetrace_Callback incl. step+draw list build, render = DL
     * translation + GPU submit + vsync wait), for hardware reports */
    {
        static uint64_t last_tick;
        static uint32_t last_frame;
        uint64_t now = svcGetSystemTick();
        if (last_tick == 0) {
            last_tick = now;
            last_frame = g_frame_no;
        } else if (now - last_tick >= SYSCLOCK_ARM11) {
            double secs = (double)(now - last_tick) / SYSCLOCK_ARM11;
            uint32_t frames = g_frame_no - last_frame;
            static uint32_t last_shown;
            double fps = (g_shown - last_shown) / secs;
            last_shown = g_shown;
            double ms_game = frames ? (double)pm3ds_t_game * 1000.0 / SYSCLOCK_ARM11 / frames : 0.0;
            double ms_wait = frames ? (double)pm3ds_t_wait * 1000.0 / SYSCLOCK_ARM11 / frames : 0.0;
            double ms_rend = frames ? (double)pm3ds_t_present * 1000.0 / SYSCLOCK_ARM11 / frames - ms_wait : 0.0;
            snprintf(pm3ds_perf_text[0], sizeof pm3ds_perf_text[0], "FPS %4.1f/%d  game %4.1fms  render %4.1fms", fps, pm3ds_fps60 ? 60 : 30, ms_game, ms_rend);
            snprintf(pm3ds_perf_text[1], sizeof pm3ds_perf_text[1], "idle %4.1fms  tris %u  draws %u", ms_wait, pm3ds_tstat_tris, g_stat_draws);
            {   /* audio CPU per second of wall time, as a share of one core:
                 * the DSP work on the mixer's core, the engine on the game's */
                extern uint64_t pm3ds_t_audio, pm3ds_t_audio_engine;
                double aud = (double)pm3ds_t_audio * 100.0 / SYSCLOCK_ARM11 / secs;
                double eng = (double)pm3ds_t_audio_engine * 100.0 / SYSCLOCK_ARM11 / secs;
                extern int s_audio_core;
                snprintf(pm3ds_perf_text[2], sizeof pm3ds_perf_text[2], "tex %u/s  audio %.1f%% core%d  eng %.1f%%",
                         pm3ds_tstat_uploads, aud, s_audio_core, eng);
                pm3ds_log(PM3DS_LOG_INFO, "PERF2 audio=%.1f%% engine=%.1f%% up=%u", aud, eng, pm3ds_tstat_uploads);
#ifdef PM3DS_STEP_PROF
                {   /* debug: the mixer's commands, % of one core */
                    extern uint64_t pm3ds_alist_ticks[16];
                    double k = 100.0 / SYSCLOCK_ARM11 / secs;
                    pm3ds_log(PM3DS_LOG_INFO, "AUDPROF adpcm=%.1f env=%.1f resample=%.1f polef=%.1f mixer=%.1f load/save=%.1f other=%.1f",
                              pm3ds_alist_ticks[1] * k, pm3ds_alist_ticks[3] * k, pm3ds_alist_ticks[5] * k,
                              pm3ds_alist_ticks[14] * k, pm3ds_alist_ticks[12] * k,
                              (pm3ds_alist_ticks[4] + pm3ds_alist_ticks[6]) * k,
                              (pm3ds_alist_ticks[0] + pm3ds_alist_ticks[2] + pm3ds_alist_ticks[9] + pm3ds_alist_ticks[10] +
                               pm3ds_alist_ticks[11] + pm3ds_alist_ticks[13] + pm3ds_alist_ticks[15]) * k);
                    memset(pm3ds_alist_ticks, 0, sizeof pm3ds_alist_ticks);
                }
#endif
                pm3ds_t_audio = pm3ds_t_audio_engine = 0;
            }
#ifdef PM3DS_STEP_PROF
            if (g_gp_renders) {   /* debug: where a render's CPU time goes, ms per render */
                double k = 1000.0 / SYSCLOCK_ARM11 / g_gp_renders;
                extern unsigned pm3ds_prof_tex_lookups, pm3ds_prof_tex_chain, pm3ds_prof_tex_memo;
                pm3ds_log(PM3DS_LOG_INFO, "GPROF n=%u applies=%u same=%u render=%.2f walk=%.2f vtx=%.2f tri=%.2f state=%.2f tex=%.2f comb=%.2f draw=%.2f bottom=%.2f end=%.2f texget=%u chain=%u memo=%u verts=%u",
                          g_gp_renders, g_gp_applies / g_gp_renders, g_gp_same / g_gp_renders, pm3ds_gp_ticks[GP_RENDER] * k, pm3ds_gp_ticks[GP_WALK] * k, pm3ds_gp_ticks[GP_VTX] * k,
                          pm3ds_gp_ticks[GP_TRI] * k, pm3ds_gp_ticks[GP_STATE] * k, pm3ds_gp_ticks[GP_TEX] * k,
                          pm3ds_gp_ticks[GP_COMB] * k, pm3ds_gp_ticks[GP_FLUSH] * k,
                          pm3ds_gp_ticks[GP_BOTTOM] * k, pm3ds_gp_ticks[GP_FRAMEEND] * k,
                          pm3ds_prof_tex_lookups / g_gp_renders, pm3ds_prof_tex_chain / g_gp_renders, pm3ds_prof_tex_memo / g_gp_renders, g_gp_verts / g_gp_renders);
                memset(pm3ds_gp_ticks, 0, sizeof pm3ds_gp_ticks);
                g_gp_applies = g_gp_same = g_gp_verts = 0;
                pm3ds_prof_tex_lookups = pm3ds_prof_tex_chain = pm3ds_prof_tex_memo = 0;
                sampler_start();
                sampler_report();
                g_gp_renders = 0;
            }
#endif
            pm3ds_log(PM3DS_LOG_INFO, "PERF fps=%.1f game=%.2fms render=%.2fms idle=%.2fms tris=%u draws=%u up=%u cmd=%.0f%% 3d=%d culldl=%u/%u npcskip=%.1f",
                      fps, ms_game, ms_rend, ms_wait, pm3ds_tstat_tris, g_stat_draws, pm3ds_tstat_uploads,
                      g_cmdbuf_peak * 100.0f, g_si_was_stereo ? (g_stereo_interp ? 60 : 30) : 0,
                      pm3ds_tstat_culldl_hit, pm3ds_tstat_culldl,
                      (double)pm3ds_tstat_npcs_culled / (frames ? frames : 1));
            pm3ds_tstat_npcs_culled = 0;   /* counted by the game's steps, so summed per log line */
            g_cmdbuf_peak = 0.0f;
            pm3ds_tstat_uploads = 0;
            last_tick = now;
            last_frame = g_frame_no;
            pm3ds_t_game = pm3ds_t_present = pm3ds_t_wait = 0;
        }
    }
    PROF_PHASE(PH_GAME);   /* the main loop runs the next game step now */
}

void pm3ds_gfx_present_shutdown(void) {
    if (g_vbuf) { linearFree(g_vbuf); g_vbuf = NULL; }
    if (g_ibuf) { linearFree(g_ibuf); g_ibuf = NULL; }
    shaderProgramFree(&g_prog);
    if (g_dvlb) { DVLB_Free(g_dvlb); g_dvlb = NULL; }
    if (g_target) { C3D_RenderTargetDelete(g_target); g_target = NULL; }
}

uint32_t pm3ds_gfx_frame_number(void) { return g_frame_no; }

uint64_t pm3ds_t_game, pm3ds_t_present, pm3ds_t_wait;

/* for the bottom screen's Debug page and its own draw pass */
char pm3ds_perf_text[3][64];
shaderProgram_s *pm3ds_gfx_program(void) { return &g_prog; }
int pm3ds_gfx_uloc_screen(void) { return g_uloc_screen; }
