/*
 * gpu_hires.h - the high-resolution copy of VRAM ("internal resolution").
 *
 * The software GPU (gpu.c) always draws the original 1024 x 512 VRAM;
 * that stays the truth for textures and for what the game reads back.
 * When the internal scale is above 1 or precise vertices are on, every
 * primitive is also queued here and drawn into a VRAM `scale` times as
 * wide and high, with float vertex positions (and perspective-correct
 * interpolation where the GTE gave depths, see pgxp.h).  Textures are
 * read from the original VRAM.  The window shows this copy.
 *
 * The queue is drawn in horizontal bands on all processor cores when the
 * picture is needed or VRAM is changed some other way.
 */
#ifndef PSX_GPU_HIRES_H
#define PSX_GPU_HIRES_H

#include <stdint.h>
#include "psx/balls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HIRES_MAX_SCALE         4       /* (higher does not keep 60 fps with software drawing) */

/* One primitive's drawing attributes (shared with gpu.c). */
typedef struct {
    int textured, raw, semi, gouraud, dither;
    int blend;
    int page_x, page_y, depth;
    int clut_x, clut_y;
} GpuPrim;

/* The drawing state a queued primitive needs. */
typedef struct {
    int clip_x1, clip_y1, clip_x2, clip_y2;     /* inclusive, original pixels */
    int win_mask_x, win_mask_y, win_off_x, win_off_y;
    int set_mask, check_mask;
} GpuEnv;

typedef struct {
    float x, y;                 /* original pixels, with the drawing offset */
    float z;                    /* > 0: depth for perspective correction */
    int r, g, b;
    int u, v;
} HiresVertex;

/* scale 0: off; 1..HIRES_MAX_SCALE.  The copy starts as the original
 * VRAM enlarged. */
void hires_configure(int scale);
int hires_scale(void);          /* 0 while off */

/* Bilinear texture filtering for 3D polygons (those whose three vertices
 * have a depth from the GTE); 2D graphics stay sharp. */
void hires_set_texture_filter(int on);

void hires_triangle(const GpuPrim *p, const GpuEnv *e, const HiresVertex *v0,
                    const HiresVertex *v1, const HiresVertex *v2);
void hires_rectangle(const GpuPrim *p, const GpuEnv *e, int x, int y, int w, int h,
                     int r, int g, int b, int u0, int v0, int flip_x, int flip_y);
void hires_line(const GpuPrim *p, const GpuEnv *e, const HiresVertex *a, const HiresVertex *b);
void hires_fill(int x, int y, int w, int h, uint16_t color);

/* Ball `ball` as a smooth sphere (balls.h); called for each packet of its
 * model, queued once per frame at the first. */
void hires_ball(BallRecord *ball, const GpuEnv *e, int off_x, int off_y);

/* After the CPU wrote original VRAM: copies the area, enlarged. */
void hires_copy_from_original(int x, int y, int w, int h);
void hires_move(int sx, int sy, int dx, int dy, int w, int h, int set_mask, int check_mask);

/* Draws everything queued. */
void hires_flush(void);

/* Forgets what was queued (a frame that is not shown). */
void hires_discard(void);

/* An area of the copy as 0xAABBGGRR pixels (out: w*scale x h*scale). */
void hires_read(uint32_t *out, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_HIRES_H */
