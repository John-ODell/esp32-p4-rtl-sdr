#pragma once
#include <stdint.h>
/* 8x16 text (DejaVu Sans Mono raster). scale 1 or 2. Returns x after the last glyph.
 * Clipped to the panel. Background is painted, so it overwrites old text in place. */
int rpi_lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int rpi_lcd_textf(int x, int y, uint16_t fg, uint16_t bg, int scale, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
