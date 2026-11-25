#ifndef GFX_INTERP_H
#define GFX_INTERP_H
/* 60 fps by frame interpolation (n64_shim/gfx_interp.c). */
#include <stdbool.h>

extern bool pm3ds_fps60;                 /* draw the in-between frames */
extern const void *pm3ds_interp_obj;     /* object the game is drawing now, or NULL */

void gfx_interp_new_tick(void);          /* before a game step builds its lists */
void gfx_interp_begin_walk(float t, bool record);   /* t 0..1 between the last step and this one */
void gfx_interp_second_eye(void);        /* the right eye walks the same lists again */
void gfx_interp_end_step(void);          /* after the step's last frame */
void gfx_interp_matrix(const void *gfx, bool proj, float m[4][4]);
/* the G_MTX at gfx is local x parent (a sprite component): blend only the parent,
 * draw the local part as this step has it (sprite keyframes snap on N64) */
void pm3ds_mtx_local(const void *gfx, float local[4][4], float parent[4][4]);
float gfx_interp_bg_shift(float *left, float *right);   /* px for the backdrop list, 0 = none */
bool gfx_interp_bg_edges(float *left, float *right);    /* the backdrop drawn this step: its edges */
bool gfx_interp_in_between(void);        /* the walk being drawn is a 60 fps in-between frame */

#endif
