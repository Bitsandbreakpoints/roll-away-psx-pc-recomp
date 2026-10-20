/*
 * gpu.h - the PS1 GPU in software: 1 MB of VRAM (1024 x 512 16-bit
 * pixels) and the GP0 drawing commands, rasterised as the hardware does.
 *
 * The library replacements (libgpu.c, libgs.c) build the same command
 * packets as Sony's libraries and send them here; the PC side shows the
 * display area of VRAM in the window (gpu_display_rgba()).
 */
#ifndef PSX_GPU_H
#define PSX_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_VRAM_WIDTH          1024
#define GPU_VRAM_HEIGHT         512

extern uint16_t g_vram[GPU_VRAM_HEIGHT][GPU_VRAM_WIDTH];

/* Power-on state: VRAM cleared, display off. */
void gpu_reset(void);

/* GP0: one command word or data word (commands may take several). */
void gpu_gp0(uint32_t word);

/* Several words, e.g. one primitive packet of an ordering table. */
void gpu_gp0_words(const uint32_t *words, int count);

/* VRAM transfers (LoadImage / StoreImage / MoveImage / ClearImage). */
void gpu_load_image(int x, int y, int w, int h, const uint16_t *data);
void gpu_store_image(int x, int y, int w, int h, uint16_t *data);
void gpu_move_image(int sx, int sy, int dx, int dy, int w, int h);
void gpu_fill(int x, int y, int w, int h, uint32_t rgb24);

/* Drawing environment (what PutDrawEnv() sets). */
void gpu_set_draw_area(int x1, int y1, int x2, int y2);     /* inclusive */
void gpu_set_draw_offset(int x, int y);
void gpu_set_draw_mode(uint32_t e1);                        /* GP0(E1h) */
void gpu_set_texture_window(uint32_t e2);                   /* GP0(E2h) */

/* Display (what PutDispEnv() / SetDispMask() set). */
void gpu_set_display(int x, int y, int w, int h, int rgb24);
void gpu_set_display_enabled(int enabled);

/*
 * The displayed picture as 0xAABBGGRR pixels, at the internal
 * resolution (*w / *h get the size; valid until the next call).  NULL
 * while the display is off.
 */
const uint32_t *gpu_display_rgba(int *w, int *h);

/* The displayed picture's size in the console's pixels (320 x 240 here). */
int gpu_display_width(void);
int gpu_display_lines(void);

/* Internal resolution (1..HIRES_MAX_SCALE times), precise vertices from the GTE and
 * bilinear filtering of 3D textures (needs precise vertices); (1, 0, 0)
 * draws only the original VRAM. */
void gpu_set_enhancements(int scale, int precise_vertices, int texture_filter);

/* A frame that will not be shown (fast forward): the high-resolution copy
 * skips drawing it. */
void gpu_skip_frame(void);

/* Debugging: all of VRAM as a 1024 x 512 BMP file; logs the display state. */
void gpu_debug_dump(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GPU_H */
