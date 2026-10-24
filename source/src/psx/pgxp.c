/*
 * pgxp.c - the table of precise vertices (see pgxp.h): direct-mapped by
 * address; a newer store to an address that maps to the same slot
 * replaces the older one.
 */
#include "psx/pgxp.h"

#define PGXP_SLOTS              (1 << 16)

typedef struct {
    uintptr_t address;
    uint32_t value;
    uint32_t frame;                     /* g_pgxp_frame when stored */
    PgxpVertex v;
} PgxpSlot;

uint32_t g_pgxp_frame;
int g_pgxp_age_stats[8];                /* (debugging: lookups by age in frames) */

int g_pgxp_enabled;
int g_pgxp_track;
static PgxpSlot s_slots[PGXP_SLOTS];
uint32_t g_pgxp_stored[PGXP_RAM_WORDS / 32];

static PgxpSlot *slot(const void *address)
{
    return &s_slots[((uintptr_t) address >> 2) & (PGXP_SLOTS - 1)];
}

void pgxp_store(const void *address, uint32_t value, const PgxpVertex *v)
{
    PgxpSlot *s = slot(address);
    uintptr_t word = ((uintptr_t) address - PGXP_RAM_ADDRESS) >> 2;
    if (word < PGXP_RAM_WORDS) {
        g_pgxp_stored[word >> 5] |= 1u << (word & 31);
    }
    s->address = (uintptr_t) address;
    s->value = value;
    s->frame = g_pgxp_frame;
    s->v = *v;
}

int pgxp_lookup(const void *address, uint32_t value, PgxpVertex *v)
{
    const PgxpSlot *s = slot(address);
    if (s->address != (uintptr_t) address || s->value != value) {
        return 0;
    }
    uint32_t age = g_pgxp_frame - s->frame;
    g_pgxp_age_stats[age < 7 ? age : 7]++;
    *v = s->v;
    return 1;
}
