/*
 * font.h - glyphs drawn with a Windows font (font_gdi.cpp).
 */
#ifndef PLATFORM_FONT_H
#define PLATFORM_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A Shift-JIS character as 16 x 15 pixels, 1 bit each, 2 bytes per row
 * (the BIOS font's layout); FALSE if it cannot be drawn. */
int font_render_sjis(uint16_t sjis, uint8_t glyph[30]);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_FONT_H */
