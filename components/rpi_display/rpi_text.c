#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "pins.h"
#include "font8x16.h"

/* One text row at scale 2. Only the CPU touches it (rpi_lcd_blit copies it into the DMA strip),
 * so it goes in PSRAM when there is some and leaves 30 KB of internal RAM for USB DMA. */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define LINE_ALLOC(n) heap_caps_malloc_prefer((n), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#else
#include <stdlib.h>
#define LINE_ALLOC(n) malloc(n)
#endif
static uint16_t *s_line;

int rpi_lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (!s_line && !(s_line = LINE_ALLOC(LCD_LONG * 32 * sizeof *s_line)))
        return x;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    int cw = 8 * scale, ch = 16 * scale;
    int n = strlen(s);
    if (x < 0 || y < 0 || y + ch > LCD_H) return x;
    if (x + n * cw > LCD_W) n = (LCD_W - x) / cw;
    if (n <= 0) return x;
    int w = n * cw;
    for (int row = 0; row < ch; row++) {
        uint16_t *o = s_line + row * w;
        int fy = row / scale;
        for (int i = 0; i < n; i++) {
            uint8_t bits = font8x16[(uint8_t)s[i] & 0x7F][fy];
            for (int px = 0; px < cw; px++)
                *o++ = (bits & (0x80 >> (px / scale))) ? fg : bg;
        }
    }
    rpi_lcd_blit(x, y, w, ch, s_line);   /* in range: checked above */
    return x + w;
}

int rpi_lcd_textf(int x, int y, uint16_t fg, uint16_t bg, int scale, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return rpi_lcd_text(x, y, buf, fg, bg, scale);
}
