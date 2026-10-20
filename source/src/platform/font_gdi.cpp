/*
 * font_gdi.cpp - Shift-JIS glyphs (16 x 15 pixels, 1 bit) drawn with a
 * Windows font, for the BIOS font the game prints its dialogue with
 * (runtime/kanji.h).  No font is shipped: Windows' own are used.
 */
#include <windows.h>
#include <string.h>
#include "platform/font.h"

#define CELL_W                  16
#define CELL_H                  15
#define CANVAS                  32

static HDC s_dc;
static HBITMAP s_bitmap;
static uint32_t *s_pixels;
static HFONT s_latin, s_japanese;

static int setup(void)
{
    if (s_dc) {
        return 1;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = CANVAS;
    bi.bmiHeader.biHeight = -CANVAS;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_dc = CreateCompatibleDC(NULL);
    s_bitmap = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **) &s_pixels, NULL, 0);
    if (!s_dc || !s_bitmap) {
        return 0;
    }
    SelectObject(s_dc, s_bitmap);
    /* full-width Latin letters as the console's font draws them: bold, filling the cell */
    s_latin = CreateFontW(-14, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
    s_japanese = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH, L"MS Gothic");
    SetBkMode(s_dc, TRANSPARENT);
    SetTextColor(s_dc, RGB(255, 255, 255));
    return 1;
}

/* the character as Unicode (code page 932), full-width ASCII as ASCII */
static wchar_t to_unicode(uint16_t sjis, int *latin)
{
    char bytes[2] = { (char) (sjis >> 8), (char) sjis };
    wchar_t wc = 0;
    if (!MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes, 2, &wc, 1)) {
        /* (no code page 932: the full-width ASCII range by hand) */
        if (sjis >= 0x824F && sjis <= 0x8258) wc = (wchar_t) ('0' + sjis - 0x824F);
        else if (sjis >= 0x8260 && sjis <= 0x8279) wc = (wchar_t) ('A' + sjis - 0x8260);
        else if (sjis >= 0x8281 && sjis <= 0x829A) wc = (wchar_t) ('a' + sjis - 0x8281);
        else if (sjis == 0x8140) wc = ' ';
        else if (sjis == 0x8149) wc = '!';
        else if (sjis == 0x8148) wc = '?';
        else if (sjis == 0x8144) wc = '.';
        else if (sjis == 0x8143) wc = ',';
        else if (sjis == 0x8166) wc = '\'';
        else if (sjis == 0x815B || sjis == 0x817C) wc = '-';
        else return 0;
    }
    if (wc >= 0xFF01 && wc <= 0xFF5E) {
        wc = (wchar_t) (wc - 0xFEE0);
    } else if (wc == 0x3000) {
        wc = ' ';
    } else if (wc == 0x2019 || wc == 0x2018) {
        wc = '\'';
    } else if (wc == 0x201C || wc == 0x201D) {
        wc = '"';
    }
    *latin = wc < 0x2000;
    return wc;
}

int font_render_sjis(uint16_t sjis, uint8_t glyph[30])
{
    int latin;
    wchar_t wc = to_unicode(sjis, &latin);
    memset(glyph, 0, 30);
    if (!wc || !setup()) {
        return 0;
    }
    if (wc == ' ') {
        return 1;
    }
    memset(s_pixels, 0, CANVAS * CANVAS * 4);
    SelectObject(s_dc, latin ? s_latin : s_japanese);
    SIZE size;
    GetTextExtentPoint32W(s_dc, &wc, 1, &size);
    int x = (CELL_W - size.cx) / 2, y = (CELL_H - size.cy) / 2 + 1;
    TextOutW(s_dc, x < 0 ? 0 : x, y, &wc, 1);
    GdiFlush();
    for (int row = 0; row < CELL_H; row++) {
        for (int col = 0; col < CELL_W; col++) {
            if (s_pixels[row * CANVAS + col] & 0x00FF00) {
                glyph[row * 2 + col / 8] |= (uint8_t) (0x80 >> (col % 8));
            }
        }
    }
    return 1;
}
