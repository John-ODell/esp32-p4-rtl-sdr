#include "rpi_canvas.h"
#include "font8x16.h"

void rpi_cv_fill(const rpi_canvas_t *c, int x, int y, int w, int h, uint16_t col)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (int j = 0; j < h; j++) {
        uint16_t *p = c->px + (y + j) * c->w + x;
        for (int i = 0; i < w; i++) p[i] = col;
    }
}

void rpi_cv_rrect(const rpi_canvas_t *c, int x, int y, int w, int h, int r, uint16_t col)
{
    /* corner insets for radius 0..8, from a quarter circle */
    static const uint8_t ins[9][8] = {
        {0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0},
        {3, 2, 1, 1, 0, 0}, {4, 2, 2, 1, 0, 0, 0}, {4, 3, 2, 1, 1, 0, 0, 0},
    };
    if (r < 0) r = 0;
    if (r > 8) r = 8;
    if (2 * r > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int k = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        int in = k >= 0 ? ins[r][k] : 0;
        rpi_cv_fill(c, x + in, y + j, w - 2 * in, 1, col);
    }
}

int rpi_cv_text(const rpi_canvas_t *c, int x, int y, const char *s, uint16_t fg, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *g = font8x16[(uint8_t)*s & 0x7F];
        for (int row = 0; row < 16 * scale; row++) {
            int yy = y + row;
            if (yy < 0 || yy >= c->h) continue;
            uint8_t bits = g[row / scale];
            if (!bits) continue;
            uint16_t *p = c->px + yy * c->w;
            for (int px = 0; px < 8 * scale; px++) {
                int xx = x + px;
                if (xx >= 0 && xx < c->w && (bits & (0x80 >> (px / scale)))) p[xx] = fg;
            }
        }
    }
    return x;
}

uint16_t rpi_mix565(uint16_t a, uint16_t b, int t)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + ((br - ar) * t >> 8), g = ag + ((bg - ag) * t >> 8), bl = ab + ((bb - ab) * t >> 8);
    return (uint16_t)(r << 11 | g << 5 | bl);
}
