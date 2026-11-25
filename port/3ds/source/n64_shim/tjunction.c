/*
 * tjunction.c — close the hairline cracks along T-junctions in map models.
 *
 * Map meshes often have a vertex of one triangle lying on the edge of another
 * (a T-junction): the long edge and the two short ones are rasterized
 * separately, and rounding leaves single pixels between them where the sky
 * shows through (Goomba Village's cliff, kmr_03; the N64 has the same cracks).
 *
 * After a map's (or battle stage's) models are loaded, every triangle of a
 * model drawn in world space is checked for vertices of any such model lying
 * on its edges (within the 0.87 units that rounding the corners to whole units
 * can put between them). Such a triangle is drawn as a fan through
 * those points instead, so both sides share the same edges. The model keeps its
 * own display list and vertices (scripts that copy or animate them see nothing
 * new): a second list, used only for drawing, repeats it with the split
 * triangles replaced by a G_VTX of their extra points into vertex slots 32..63
 * (the translator has 64; F3DEX2 lists use 0..31) and the smaller triangles.
 * An extra point copies the neighbour's position and takes texture coordinates
 * and colour from the edge it splits. New 3DS only (the Old 3DS keeps its time).
 */
#include "common.h"
#include "model.h"
#include "pm3ds_log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern u64 svcGetSystemTick(void);

#ifndef PM3DS_HACK_RUNTIME

#define TJ_SLOT0      32          /* first vertex slot for split points */
#define TJ_MAX_PTS    32          /* points per triangle (slots 32..63) */
#define TJ_HASH       1024        /* power of two, kept at most half full (2 x MAX_MODELS) */

typedef struct { ModelNode* node; Gfx* dl; s8 ctx; } TjEntry;
static TjEntry s_tab[TJ_HASH];

static inline u32 tj_hash(const void* p) { return ((u32)(uintptr_t)p >> 2) * 2654435761u >> 22; }

Gfx* pm3ds_tjunc_dl(ModelNode* node, Gfx* orig) {
    u32 h = tj_hash(node);
    for (s32 i = 0; i < TJ_HASH; i++) {
        TjEntry* e = &s_tab[(h + i) & (TJ_HASH - 1)];
        if (e->node == nullptr) return orig;
        if (e->node == node && e->ctx == gGameStatusPtr->context) return e->dl;
    }
    return orig;
}

/* drop the patched lists of the current context (its models are about to go) */
void pm3ds_tjunc_clear(void) {
    static TjEntry keep[TJ_HASH];   /* not on the game thread's stack */
    s32 nkeep = 0;
    s8 ctx = gGameStatusPtr->context;
    for (s32 i = 0; i < TJ_HASH; i++) {
        if (s_tab[i].node == nullptr) continue;
        if (s_tab[i].ctx == ctx) free(s_tab[i].dl);
        else keep[nkeep++] = s_tab[i];
    }
    memset(s_tab, 0, sizeof(s_tab));
    for (s32 i = 0; i < nkeep; i++) {   /* re-insert the other context's entries */
        u32 h = tj_hash(keep[i].node);
        for (s32 j = 0; j < TJ_HASH; j++) {
            TjEntry* e = &s_tab[(h + j) & (TJ_HASH - 1)];
            if (e->node == nullptr) { *e = keep[i]; break; }
        }
    }
}

static bool tj_insert(ModelNode* node, Gfx* dl) {
    u32 h = tj_hash(node);
    for (s32 i = 0; i < TJ_HASH / 2; i++) {   /* keep the table at most half full */
        TjEntry* e = &s_tab[(h + i) & (TJ_HASH - 1)];
        if (e->node == nullptr) {
            e->node = node;
            e->dl = dl;
            e->ctx = gGameStatusPtr->context;
            return true;
        }
    }
    return false;
}

/* ---- the vertex positions of all world-space models ---------------------- */

typedef struct { s16 x, y, z; } TjPos;
static TjPos* s_pos;
static s32 s_npos;
static u16* s_ord[3];   /* s_pos indices sorted by x, y, z */

static int tj_cmp_pos(const void* a, const void* b) {
    const TjPos* p = a;
    const TjPos* q = b;
    if (p->x != q->x) return p->x - q->x;
    if (p->y != q->y) return p->y - q->y;
    return p->z - q->z;
}
static s32 s_sort_axis;
static int tj_cmp_axis(const void* a, const void* b) {
    const s16* p = &s_pos[*(const u16*)a].x;
    const s16* q = &s_pos[*(const u16*)b].x;
    return p[s_sort_axis] - q[s_sort_axis];
}

/* host memory vertices may be in (gfx_translate.c addr_plausible): the 3DSX
 * image, the app heap, the linear heaps */
extern char __end__[];
static bool tj_addr_ok(u32 a) {
    if ((a & 7) != 0) return false;
    return (a >= 0x00100000u && a < (u32)(uintptr_t)__end__) || (a >= 0x08000000u && a < 0x10000000u) ||
           (a >= 0x14000000u && a < 0x1C000000u);
}

/* the display list of a model the patch can handle: geometry from G_VTX /
 * G_TRI1 / G_TRI2 with direct vertex addresses, no calls or matrices */
static bool tj_dl_ok(Gfx* dl, s32* ncmd) {
    for (s32 i = 0; i < 4096; i++) {
        u32 op = dl[i].words.w0 >> 24;
        switch (op) {
            case G_ENDDL:
                *ncmd = i + 1;
                return true;
            case G_DL: case G_MTX: case G_POPMTX: case G_BRANCH_Z: case G_MOVEMEM: case G_MOVEWORD:
                return false;
            case G_VTX:
                if (!tj_addr_ok(dl[i].words.w1)) return false;   /* not a relocated host pointer */
                break;
            default:
                break;
        }
    }
    return false;
}

static Model* tj_model(s32 i) {
    Model* m = (*gCurrentModels)[i];
    if (m == nullptr || m->flags == 0 || m->bakedMtx != nullptr) return nullptr;
    ModelNode* n = m->modelNode;
    if (n == nullptr || n->type != SHAPE_TYPE_MODEL || n->displayData == nullptr || n->displayData->displayList == nullptr)
        return nullptr;
    return m;
}

static void tj_collect_positions(void) {
    s32 cap = 4096;
    s_npos = 0;
    s_pos = malloc(cap * sizeof(*s_pos));
    if (s_pos == nullptr) return;
    for (s32 i = 0; i < MAX_MODELS; i++) {
        Model* m = tj_model(i);
        s32 ncmd;
        if (m == nullptr || !tj_dl_ok(m->modelNode->displayData->displayList, &ncmd)) continue;
        Gfx* dl = m->modelNode->displayData->displayList;
        for (s32 c = 0; c < ncmd; c++) {
            if ((dl[c].words.w0 >> 24) != G_VTX) continue;
            s32 n = (dl[c].words.w0 >> 12) & 0xFF;
            const Vtx* v = (const Vtx*)(uintptr_t)dl[c].words.w1;
            for (s32 k = 0; k < n; k++) {
                if (s_npos == cap) {
                    TjPos* np = realloc(s_pos, cap * 2 * sizeof(*s_pos));
                    if (np == nullptr) return;
                    s_pos = np;
                    cap *= 2;
                }
                s_pos[s_npos].x = v[k].v.ob[0];
                s_pos[s_npos].y = v[k].v.ob[1];
                s_pos[s_npos].z = v[k].v.ob[2];
                s_npos++;
            }
        }
    }
    if (s_npos == 0) return;
    qsort(s_pos, s_npos, sizeof(*s_pos), tj_cmp_pos);
    s32 u = 1;
    for (s32 i = 1; i < s_npos; i++)
        if (tj_cmp_pos(&s_pos[i], &s_pos[u - 1]) != 0) s_pos[u++] = s_pos[i];
    s_npos = u > 65535 ? 65535 : u;
    for (s32 a = 0; a < 3; a++) {
        s_ord[a] = malloc(s_npos * sizeof(u16));
        if (s_ord[a] == nullptr) { s_npos = 0; return; }
        for (s32 i = 0; i < s_npos; i++) s_ord[a][i] = i;
        s_sort_axis = a;
        qsort(s_ord[a], s_npos, sizeof(u16), tj_cmp_axis);
    }
}

/* ---- splitting ----------------------------------------------------------- */

typedef struct { s16 x, y, z; f32 t; } TjPt;

/* first index of s_ord[ax] whose coordinate is >= v */
static s32 tj_lower(s32 ax, s32 v) {
    const u16* ord = s_ord[ax];
    s32 l = 0, r = s_npos;
    while (l < r) {
        s32 m = (l + r) >> 1;
        if ((&s_pos[ord[m]].x)[ax] < v) l = m + 1; else r = m;
    }
    return l;
}

/* points lying on edge u->v (not at its ends), sorted from u to v */
static s32 tj_edge_points(const Vtx* u, const Vtx* v, const Vtx* w, TjPt* out, s32 max) {
    f32 ux = u->v.ob[0], uy = u->v.ob[1], uz = u->v.ob[2];
    f32 lx = v->v.ob[0] - ux, ly = v->v.ob[1] - uy, lz = v->v.ob[2] - uz;
    f32 l2 = lx * lx + ly * ly + lz * lz;
    if (l2 < 4.0f) return 0;
    s32 lo[3], hi[3];
    for (s32 a = 0; a < 3; a++) {
        s32 p = u->v.ob[a], q = v->v.ob[a];
        lo[a] = (p < q ? p : q) - 1;
        hi[a] = (p > q ? p : q) + 1;
    }
    /* scan the axis whose slab holds the fewest positions */
    s32 ax = 0, l = 0, r = 0;
    for (s32 a = 0; a < 3; a++) {
        s32 la = tj_lower(a, lo[a]), ra = tj_lower(a, hi[a] + 1);
        if (a == 0 || ra - la < r - l) { ax = a; l = la; r = ra; }
    }
    const u16* ord = s_ord[ax];
    s32 n = 0;
    for (s32 i = l; i < r; i++) {
        const TjPos* p = &s_pos[ord[i]];
        const s16* pc = &p->x;
        if (pc[0] < lo[0] || pc[0] > hi[0] || pc[1] < lo[1] || pc[1] > hi[1] || pc[2] < lo[2] || pc[2] > hi[2]) continue;
        const Vtx* ends[3] = { u, v, w };
        bool corner = false;
        for (s32 k = 0; k < 3; k++)
            if (ends[k]->v.ob[0] == p->x && ends[k]->v.ob[1] == p->y && ends[k]->v.ob[2] == p->z) corner = true;
        if (corner) continue;
        f32 dx = p->x - ux, dy = p->y - uy, dz = p->z - uz;
        f32 t = (dx * lx + dy * ly + dz * lz) / l2;
        if (t <= 0.0f || t >= 1.0f) continue;
        f32 ex = dx - t * lx, ey = dy - t * ly, ez = dz - t * lz;
        /* off the line by at most the rounding of whole-unit corners (sqrt(3) / 2),
         * not a whole unit (layers 1 unit apart stay as they are) */
        if (ex * ex + ey * ey + ez * ez > 0.76f) continue;
        if (n == max) return -1;
        s32 j = n++;
        while (j > 0 && out[j - 1].t > t) { out[j] = out[j - 1]; j--; }
        out[j].x = p->x; out[j].y = p->y; out[j].z = p->z; out[j].t = t;
    }
    return n;
}

/* output of one model's patch */
typedef struct {
    Gfx* gfx; s32 ngfx, capgfx;
    Vtx* vtx; s32 nvtx, capvtx;
    u8 tris[3 * 96]; s32 ntris;   /* sub-triangles of the triangle being split (slots) */
} TjOut;

static void tj_tri(TjOut* o, s32 a, s32 b, s32 c) {
    if (o->ntris < 96) {
        o->tris[o->ntris * 3 + 0] = a;
        o->tris[o->ntris * 3 + 1] = b;
        o->tris[o->ntris * 3 + 2] = c;
        o->ntris++;
    }
}

/* triangle a b c (slots) with the slots of the points on each edge, in edge
 * order: fan the first edge that has points from the opposite corner; the two
 * outer triangles of the fan keep the other two edges and are split the same way */
static void tj_fan(TjOut* o, s32 a, s32 b, s32 c, const u8* lab, s32 nab, const u8* lbc, s32 nbc, const u8* lca, s32 nca) {
    if (nab == 0 && nbc == 0 && nca == 0) {
        tj_tri(o, a, b, c);
    } else if (nab == 0) {
        if (nbc != 0) tj_fan(o, b, c, a, lbc, nbc, lca, nca, lab, 0);
        else          tj_fan(o, c, a, b, lca, nca, lab, 0, lbc, 0);
    } else {
        tj_fan(o, a, lab[0], c, nullptr, 0, nullptr, 0, lca, nca);
        for (s32 i = 0; i + 1 < nab; i++) tj_tri(o, lab[i], lab[i + 1], c);
        tj_fan(o, lab[nab - 1], b, c, nullptr, 0, lbc, nbc, nullptr, 0);
    }
}

static void tj_put(TjOut* o, u32 w0, u32 w1) {
    if (o->ngfx < o->capgfx) {
        o->gfx[o->ngfx].words.w0 = w0;
        o->gfx[o->ngfx].words.w1 = w1;
    }
    o->ngfx++;
}

static void tj_put_tris(TjOut* o, const u8* t, s32 n) {
    for (s32 i = 0; i < n; i += 2) {
        if (i + 1 < n) {
            Gfx g;
            gSP2Triangles(&g, t[i * 3], t[i * 3 + 1], t[i * 3 + 2], 0, t[i * 3 + 3], t[i * 3 + 4], t[i * 3 + 5], 0);
            tj_put(o, g.words.w0, g.words.w1);
        } else {
            Gfx g;
            gSP1Triangle(&g, t[i * 3], t[i * 3 + 1], t[i * 3 + 2], 0);
            tj_put(o, g.words.w0, g.words.w1);
        }
    }
}

static void tj_lerp_vtx(Vtx* out, const Vtx* u, const Vtx* v, const TjPt* p) {
    f32 t = p->t;
    *out = *u;
    out->v.ob[0] = p->x;
    out->v.ob[1] = p->y;
    out->v.ob[2] = p->z;
    for (s32 k = 0; k < 2; k++) out->v.tc[k] = (s16)lroundf(u->v.tc[k] + t * (v->v.tc[k] - u->v.tc[k]));
    for (s32 k = 0; k < 4; k++) out->v.cn[k] = (u8)lroundf(u->v.cn[k] + t * (v->v.cn[k] - u->v.cn[k]));
}

/* how one triangle is split: its corners from the smallest position on (same
 * winding, so a decal over a surface with the same corners is split exactly
 * like it) and the points on each edge */
typedef struct {
    s32 slot[3];
    const Vtx* v[3];
    TjPt pts[3][TJ_MAX_PTS];
    s32 np[3], total;
} TjPlan;

static bool tj_plan(TjPlan* pl, const Vtx* const* slots, const u8* tri) {
    const Vtx* v[3];
    for (s32 k = 0; k < 3; k++) {
        if (tri[k] >= TJ_SLOT0 || slots[tri[k]] == nullptr) return false;
        v[k] = slots[tri[k]];
    }
    s32 r = 0;
    for (s32 k = 1; k < 3; k++) {
        TjPos pk = { v[k]->v.ob[0], v[k]->v.ob[1], v[k]->v.ob[2] };
        TjPos pr = { v[r]->v.ob[0], v[r]->v.ob[1], v[r]->v.ob[2] };
        if (tj_cmp_pos(&pk, &pr) < 0) r = k;
    }
    for (s32 k = 0; k < 3; k++) {
        pl->slot[k] = tri[(r + k) % 3];
        pl->v[k] = v[(r + k) % 3];
    }
    pl->total = 0;
    for (s32 e = 0; e < 3; e++) {
        pl->np[e] = tj_edge_points(pl->v[e], pl->v[(e + 1) % 3], pl->v[(e + 2) % 3], pl->pts[e], TJ_MAX_PTS);
        if (pl->np[e] < 0) return false;
        pl->total += pl->np[e];
    }
    return pl->total > 0 && pl->total <= TJ_MAX_PTS;
}

/* a G_VTX of the points into slots 32.. and the fan */
static void tj_emit(TjOut* o, const TjPlan* pl) {
    u8 sl[3][TJ_MAX_PTS];
    s32 base = o->nvtx, k = 0;
    for (s32 e = 0; e < 3; e++) {
        for (s32 i = 0; i < pl->np[e]; i++, k++) {
            if (o->vtx != nullptr && o->nvtx < o->capvtx)
                tj_lerp_vtx(&o->vtx[o->nvtx], pl->v[e], pl->v[(e + 1) % 3], &pl->pts[e][i]);
            o->nvtx++;
            sl[e][i] = TJ_SLOT0 + k;
        }
    }
    Gfx g;
    gSPVertex(&g, o->vtx != nullptr ? &o->vtx[base] : nullptr, pl->total, TJ_SLOT0);
    tj_put(o, g.words.w0, g.words.w1);
    o->ntris = 0;
    tj_fan(o, pl->slot[0], pl->slot[1], pl->slot[2], sl[0], pl->np[0], sl[1], pl->np[1], sl[2], pl->np[2]);
    tj_put_tris(o, o->tris, o->ntris);
}

/* one pass over a model's list: count (o->gfx NULL; marks the triangles that
 * split in map) or write the patched list (plans only the marked ones again).
 * Returns the number of split triangles. */
static s32 tj_patch_pass(Gfx* dl, s32 ncmd, TjOut* o, u8* map) {
    static TjPlan plan[2];
    const Vtx* slots[TJ_SLOT0];
    s32 split = 0, tri = 0;
    memset(slots, 0, sizeof(slots));
    for (s32 c = 0; c < ncmd; c++) {
        u32 w0 = dl[c].words.w0, w1 = dl[c].words.w1;
        u32 op = w0 >> 24;
        if (op == G_VTX) {
            s32 n = (w0 >> 12) & 0xFF, v0 = ((w0 >> 1) & 0x7F) - n;
            const Vtx* src = (const Vtx*)(uintptr_t)w1;
            for (s32 k = 0; k < n; k++)
                if (v0 + k >= 0 && v0 + k < TJ_SLOT0) slots[v0 + k] = &src[k];
            tj_put(o, w0, w1);
        } else if (op == G_TRI1 || op == G_TRI2) {
            u8 t[6] = { ((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2,
                        ((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2 };
            s32 nt = op == G_TRI2 ? 2 : 1;
            bool sp[2] = { false, false };
            for (s32 i = 0; i < nt; i++, tri++) {
                if (o->gfx != nullptr && !(map[tri >> 3] & (1 << (tri & 7)))) continue;
                sp[i] = tj_plan(&plan[i], slots, &t[i * 3]);
                if (sp[i] && o->gfx == nullptr) map[tri >> 3] |= 1 << (tri & 7);
            }
            if (!sp[0] && !sp[1]) {
                tj_put(o, w0, w1);
                continue;
            }
            for (s32 i = 0; i < nt; i++) {   /* in order: the plain one as a G_TRI1 */
                if (sp[i]) {
                    tj_emit(o, &plan[i]);
                    split++;
                } else {
                    tj_put_tris(o, &t[i * 3], 1);
                }
            }
        } else {
            tj_put(o, w0, w1);
        }
    }
    return split;
}

void pm3ds_tjunc_build(void) {
    s32 models = 0, patched = 0, tris = 0;
    u64 t0 = svcGetSystemTick();
    pm3ds_tjunc_clear();
#ifdef PM3DS_NO_TJUNC   /* debug A/B: the lists as they are */
    return;
#endif
    /* not on the Old 3DS: Toad Town (mac_00) has 300 more triangles with it, 5% more
     * render time where every millisecond counts */
    extern bool pm3ds_lite_effects;
    if (pm3ds_lite_effects) return;
    tj_collect_positions();
    if (s_pos == nullptr || s_npos == 0) goto done;
    for (s32 i = 0; i < MAX_MODELS; i++) {
        Model* m = tj_model(i);
        s32 ncmd;
        if (m == nullptr || !tj_dl_ok(m->modelNode->displayData->displayList, &ncmd)) continue;
        models++;
        Gfx* dl = m->modelNode->displayData->displayList;
        TjOut cnt;
        memset(&cnt, 0, sizeof(cnt));
        u8* map = calloc((2 * ncmd + 7) / 8 + 1, 1);   /* at most 2 triangles a command */
        if (map == nullptr) continue;
        s32 n = tj_patch_pass(dl, ncmd, &cnt, map);
        if (n == 0) { free(map); continue; }
        /* one block: the list, then the vertices (8-byte aligned either way) */
        Gfx* block = malloc(cnt.ngfx * sizeof(Gfx) + cnt.nvtx * sizeof(Vtx));
        if (block == nullptr) { free(map); continue; }
        TjOut out;
        memset(&out, 0, sizeof(out));
        out.gfx = block;
        out.capgfx = cnt.ngfx;
        out.vtx = (Vtx*)(block + cnt.ngfx);
        out.capvtx = cnt.nvtx;
        tj_patch_pass(dl, ncmd, &out, map);
        free(map);
        if (out.ngfx != cnt.ngfx || out.nvtx != cnt.nvtx || !tj_insert(m->modelNode, block)) {
            free(block);
            continue;
        }
        patched++;
        tris += n;
    }
done:
    free(s_pos);
    for (s32 a = 0; a < 3; a++) { free(s_ord[a]); s_ord[a] = nullptr; }
    s_pos = nullptr;
    s_npos = 0;
    pm3ds_log(PM3DS_LOG_INFO, "tjunc: %d of %d models patched, %d triangles split (%u us)", (int)patched, (int)models,
              (int)tris, (unsigned)((svcGetSystemTick() - t0) / 268));
}

#else   /* the hack runtime keeps big-endian vertices: not patched */
Gfx* pm3ds_tjunc_dl(ModelNode* node, Gfx* orig) { (void)node; return orig; }
void pm3ds_tjunc_clear(void) {}
void pm3ds_tjunc_build(void) {}
#endif
