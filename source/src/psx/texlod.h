/*
 * texlod.h - detailed distant textures (texlod.c): many PS1 games draw
 * distant surfaces with small, 16-colour copies of their textures; for the
 * high-resolution picture the full-size texture each copy was made from
 * is found (by content) and drawn instead.
 */
#ifndef PSX_TEXLOD_H
#define PSX_TEXLOD_H

#include "psx/gpu_hires.h"

#ifdef __cplusplus
extern "C" {
#endif

void texlod_set_enabled(int on);
int texlod_enabled(void);

/* Everything matched so far is forgotten. */
void texlod_forget(void);

/* VRAM x, y, w x h was written (textures loaded): what used it is
 * forgotten. */
void texlod_vram_changed(int x, int y, int w, int h);

/* A 3D polygon with an 8-bit texture was drawn: its page and palette are
 * candidates for the full-size textures. */
void texlod_note(const GpuPrim *p);

/*
 * A 3D polygon with a 4-bit texture, its corners' texture coordinates u[],
 * v[]: if they lie in one 32 x 32 cell that is a half-size copy of a
 * full-size 64 x 64 texture, *p and the coordinates are changed to that
 * texture and TRUE is returned, with gain[] (x 256) bringing its colours to
 * the copy's (the game shades distant blocks darker).
 */
int texlod_upgrade(GpuPrim *p, int *u, int *v, int count, int gain[3]);

#ifdef __cplusplus
}
#endif

#endif /* PSX_TEXLOD_H */
