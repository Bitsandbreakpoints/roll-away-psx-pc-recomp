/*
 * gpu_hires_d3d11.h - the high-resolution copy of VRAM drawn by the
 * graphics card (Direct3D 11) instead of the processor: the same work as
 * gpu_hires.c, which hands every call here when the GPU renderer is on.
 *
 * The copy is a render target `scale` times the size of VRAM.  Primitives
 * are collected as triangles with their drawing attributes per vertex and
 * drawn in order when gpu_hires.c would draw its queue; a pixel shader does
 * what plot() / plot_filtered() do there, reading the textures from the
 * original VRAM, which is uploaded as a texture before each batch.
 * Semi-transparency is the blend unit's (textured primitives in two passes:
 * the opaque texels, then the semi-transparent ones).  The mask bit is the
 * alpha channel; "check mask" works for opaque drawing only (the game never
 * sets it).  The displayed area is copied into a texture the window shows
 * directly, without coming back to the processor.
 */
#ifndef PSX_GPU_HIRES_D3D11_H
#define PSX_GPU_HIRES_D3D11_H

#include <stdint.h>
#include "psx/gpu_hires.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the copy at `scale` (filled from the original VRAM); FALSE if the
 * graphics card cannot (the reason is logged). */
int hwr_init(int scale);
void hwr_shutdown(void);

void hwr_set_true_color(int on);
void hwr_set_texture_filter(int on);

void hwr_triangle(const GpuPrim *p, const GpuEnv *e, const HiresVertex *v0,
                  const HiresVertex *v1, const HiresVertex *v2);
void hwr_rectangle(const GpuPrim *p, const GpuEnv *e, int x, int y, int w, int h,
                   int r, int g, int b, int u0, int v0, int flip_x, int flip_y);
void hwr_line(const GpuPrim *p, const GpuEnv *e, const HiresVertex *a, const HiresVertex *b);
void hwr_fill(int x, int y, int w, int h, uint16_t color);
void hwr_copy_from_original(int x, int y, int w, int h);
void hwr_move(int sx, int sy, int dx, int dy, int w, int h, int set_mask, int check_mask);
void hwr_flush(void);
void hwr_discard(void);

/* An area of the copy (original pixels) as 0xAABBGGRR pixels, read back
 * from the graphics card (tests, dumps). */
void hwr_read(uint32_t *out, int x, int y, int w, int h);

/* The same area as a texture (ID3D11ShaderResourceView *, w*scale x
 * h*scale), valid until the next call. */
void *hwr_picture(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_HIRES_D3D11_H */
