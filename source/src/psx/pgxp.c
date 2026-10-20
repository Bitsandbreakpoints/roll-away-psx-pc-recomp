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
    PgxpVertex v;
} PgxpSlot;

int g_pgxp_enabled;
static PgxpSlot s_slots[PGXP_SLOTS];

static PgxpSlot *slot(const void *address)
{
    return &s_slots[((uintptr_t) address >> 2) & (PGXP_SLOTS - 1)];
}

void pgxp_store(const void *address, uint32_t value, const PgxpVertex *v)
{
    PgxpSlot *s = slot(address);
    s->address = (uintptr_t) address;
    s->value = value;
    s->v = *v;
}

int pgxp_lookup(const void *address, uint32_t value, PgxpVertex *v)
{
    const PgxpSlot *s = slot(address);
    if (s->address != (uintptr_t) address || s->value != value) {
        return 0;
    }
    *v = s->v;
    return 1;
}
