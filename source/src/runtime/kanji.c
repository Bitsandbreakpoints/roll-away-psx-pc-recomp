/*
 * kanji.c - the BIOS font area (see kanji.h).
 *
 * Two blocks as in the ROM: the non-kanji characters (lead bytes
 * 81h..84h: punctuation, full-width Latin letters and digits, kana) and
 * the level 1 kanji (889Fh..).  Inside a block a character's glyph is at
 * index * 30, with our own index (kanjitrans() is replaced too, so the
 * game asks us for it): the position of the character in Shift-JIS order.
 */
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/kanji.h"

#define PHYS(a)                 ((a) & 0x1FFFFFFFu)
#define BLOCK_NONKANJI          0xBFC66000u     /* (where the ROM has it) */
#define BLOCK_KANJI             0xBFC6C000u
#define BLOCK_END               0xBFC80000u
#define FIRST_KANJI             0x889Fu
#define TRAILS                  188             /* trail bytes 40h..7Eh, 80h..FCh */
#define NONKANJI_SLOTS          ((BLOCK_KANJI - BLOCK_NONKANJI) / KANJI_GLYPH_BYTES)
#define KANJI_SLOTS             ((BLOCK_END - BLOCK_KANJI) / KANJI_GLYPH_BYTES)

static KanjiRenderer s_renderer;
static uint8_t s_glyphs[(BLOCK_END - BLOCK_NONKANJI) / KANJI_GLYPH_BYTES][KANJI_GLYPH_BYTES];
static uint8_t s_drawn[(BLOCK_END - BLOCK_NONKANJI) / KANJI_GLYPH_BYTES];

void kanji_set_renderer(KanjiRenderer renderer)
{
    s_renderer = renderer;
}

void kanji_reset(void)
{
    memset(s_drawn, 0, sizeof(s_drawn));
}

static int trail_index(int trail)
{
    if (trail >= 0x40 && trail <= 0x7E) {
        return trail - 0x40;
    }
    if (trail >= 0x80 && trail <= 0xFC) {
        return trail - 0x41;
    }
    return -1;
}

/* the index of a character inside its block; -1 if it has none */
static int glyph_index(uint16_t sjis)
{
    int lead = sjis >> 8, t = trail_index(sjis & 0xFF);
    if (t < 0) {
        return -1;
    }
    int index;
    if (sjis < FIRST_KANJI) {
        index = lead >= 0x81 && lead <= 0x84 ? (lead - 0x81) * TRAILS + t : -1;
        return index < (int) NONKANJI_SLOTS ? index : -1;
    }
    index = (lead - 0x88) * TRAILS + t - trail_index(FIRST_KANJI & 0xFF);
    return index >= 0 && index < (int) KANJI_SLOTS ? index : -1;
}

/* the character at an index (the inverse of glyph_index()) */
static uint16_t glyph_code(int slot)
{
    int kanji = slot >= (int) NONKANJI_SLOTS;
    int index = kanji ? slot - (int) NONKANJI_SLOTS + trail_index(FIRST_KANJI & 0xFF) : slot;
    int lead = (kanji ? 0x88 : 0x81) + index / TRAILS, t = index % TRAILS;
    int trail = t < 0x3F ? 0x40 + t : 0x41 + t;
    return (uint16_t) (lead << 8 | trail);
}

uint32_t kanji_block_address(uint16_t sjis)
{
    return sjis < FIRST_KANJI ? BLOCK_NONKANJI : BLOCK_KANJI;
}

int kanji_read(uint32_t phys, uint32_t *value, int size)
{
    if (phys < PHYS(BLOCK_NONKANJI) || phys + size > PHYS(BLOCK_END)) {
        return 0;
    }
    *value = 0;
    for (int i = 0; i < size; i++) {
        uint32_t offset = phys + i - PHYS(BLOCK_NONKANJI);
        int slot = (int) (offset / KANJI_GLYPH_BYTES);
        if (!s_drawn[slot]) {
            s_drawn[slot] = 1;
            memset(s_glyphs[slot], 0, KANJI_GLYPH_BYTES);
            if (s_renderer) {
                s_renderer(glyph_code(slot), s_glyphs[slot]);
            }
        }
        *value |= (uint32_t) s_glyphs[slot][offset % KANJI_GLYPH_BYTES] << (8 * i);
    }
    return 1;
}

/* LIBGPU's kanjitrans(code): the glyph's index in its block (as above) */
void hle_kanjitrans(void)
{
    int index = glyph_index((uint16_t) GPR[4]);
    RETURN(index < 0 ? 0 : index);
}
