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

void pgxp_store(const void *address, uint32_t value, const PgxpVertex *v);

/* TRUE and *v if `address` was last stored from the GTE with `value`. */
int pgxp_lookup(const void *address, uint32_t value, PgxpVertex *v);

#ifdef __cplusplus
}
#endif

#endif /* PSX_PGXP_H */
