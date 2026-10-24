/*
 * pgxp.h - precise vertices ("PGXP"): the GTE keeps the screen positions
 * it projects as floats, and each time a position is stored to memory
 * (swc2 of SXY0..2) the float version is remembered for that address.
 * When the GPU later reads a vertex from a packet at that address, and
 * the word still holds the same integer position, it can use the float
 * position and depth instead (sub-pixel accuracy, perspective-correct
 * textures).
 */
#ifndef PSX_PGXP_H
#define PSX_PGXP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y;                 /* screen position (as SXY, without the draw offset) */
    float z;                    /* view-space depth, > 0 */
} PgxpVertex;

extern int g_pgxp_enabled;

/* The GTE's stores are kept track of also without precise vertices (the
 * widescreen mode tells 3D from 2D by them). */
extern int g_pgxp_track;

/* vertical blanks so far (pgxp_store notes when) */
extern uint32_t g_pgxp_frame;

void pgxp_store(const void *address, uint32_t value, const PgxpVertex *v);

/* TRUE and *v if `address` was last stored from the GTE with `value`. */
int pgxp_lookup(const void *address, uint32_t value, PgxpVertex *v);

/* A bit per word of main RAM (at PGXP_RAM_ADDRESS in the PC's memory) that
 * a precise position was ever stored to: most loads of the game's code
 * are from other words, and pgxp_lookup() on them (a miss in a 1.8 MB
 * table) was a third of the time.  Bits are never cleared (a set bit only
 * means: look). */
#define PGXP_RAM_ADDRESS        0x80000000u
#define PGXP_RAM_WORDS          (0x200000u / 4)
extern uint32_t g_pgxp_stored[PGXP_RAM_WORDS / 32];

#ifdef __cplusplus
}
#endif

#endif /* PSX_PGXP_H */
