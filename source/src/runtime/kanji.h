/*
 * kanji.h - the BIOS ROM's Shift-JIS font, which the game's dialogue text
 * is printed with (LIBGPU's KanjiFntPrint: Krom2RawAdd + kanjitrans).
 * The port has no BIOS: the glyphs (16 x 15 pixels, 1 bit per pixel, 2
 * bytes per row) are drawn when they are first read, by the PC side
 * (kanji_set_renderer(): a Windows font), into a font area at the ROM's
 * addresses.
 */
#ifndef RUNTIME_KANJI_H
#define RUNTIME_KANJI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KANJI_GLYPH_BYTES       30

/* draws a Shift-JIS character into glyph[30]; FALSE: unknown character */
typedef int (*KanjiRenderer)(uint16_t sjis, uint8_t glyph[KANJI_GLYPH_BYTES]);

void kanji_set_renderer(KanjiRenderer renderer);
void kanji_reset(void);

/* Krom2RawAdd(): the font area's address for a code's block */
uint32_t kanji_block_address(uint16_t sjis);

/* a read of the font area (BIOS ROM addresses); FALSE if outside it */
int kanji_read(uint32_t phys, uint32_t *value, int size);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_KANJI_H */
