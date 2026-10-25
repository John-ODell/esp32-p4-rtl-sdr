#include "bmp.h"

static uint32_t u16le(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t u32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    int w, h, bpp;
    bool top_down;
    size_t off, stride;
} hdr_t;

static bool parse(const uint8_t *f, size_t len, hdr_t *b)
{
    if (len < 54 || f[0] != 'B' || f[1] != 'M') return false;
    uint32_t hsz = u32le(f + 14);
    int32_t w = (int32_t)u32le(f + 18), h = (int32_t)u32le(f + 22);
    b->bpp = u16le(f + 28);
    uint32_t comp = u32le(f + 30);
    /* BITMAPINFOHEADER or later; BI_RGB, or BI_BITFIELDS for 32-bit (taken as BGRA, the usual masks) */
    if (hsz < 40 || u16le(f + 26) != 1 || (b->bpp != 24 && b->bpp != 32)) return false;
    if (!(comp == 0 || (comp == 3 && b->bpp == 32))) return false;
    if (w <= 0 || w > 4096 || h == 0 || h < -4096 || h > 4096) return false;
    b->w = w;
    b->top_down = h < 0;
    b->h = h < 0 ? -h : h;
    b->off = u32le(f + 10);
    b->stride = ((size_t)w * b->bpp + 31) / 32 * 4;
    return b->off >= 54 && b->off <= len && b->stride * b->h <= len - b->off;
}

bool bmp_size(const uint8_t *f, size_t len, int *w, int *h)
{
    hdr_t b;
    if (!parse(f, len, &b)) return false;
    *w = b.w;
    *h = b.h;
    return true;
}

bool bmp_to_565(const uint8_t *f, size_t len, int w, int h, uint16_t *out)
{
    hdr_t b;
    if (!parse(f, len, &b) || b.w != w || b.h != h) return false;
    int step = b.bpp / 8;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = f + b.off + b.stride * (size_t)(b.top_down ? y : h - 1 - y);
        uint16_t *o = out + (size_t)y * w;
        for (int x = 0; x < w; x++, row += step)   /* B, G, R (, A) */
            o[x] = (uint16_t)((row[2] & 0xF8) << 8 | (row[1] & 0xFC) << 3 | row[0] >> 3);
    }
    return true;
}
