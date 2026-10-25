/* Host test for main/bmp.c: small BMP files are built in memory the way image editors write them
 * (24-bit bottom-up with row padding, 32-bit top-down, BI_BITFIELDS) and decoded, and every
 * pixel is checked against RGB565; then the files it must refuse: another size, 16-bit, RLE,
 * a short file, a bad header. Build and run: make -C tools/bmp_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bmp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t f[4096];

static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* the colour of pixel (x, y), y from the top */
static void rgb(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(x * 50 + 3);
    *g = (uint8_t)(y * 70 + 5);
    *b = (uint8_t)(x * 30 + y * 40 + 7);
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3); }

/* a w x h file; h < 0 writes it top-down. Returns its length. */
static size_t make(int w, int h, int bpp, uint32_t comp)
{
    int ah = h < 0 ? -h : h;
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4, off = 54;
    memset(f, 0xEE, sizeof f);               /* padding bytes must not matter */
    f[0] = 'B'; f[1] = 'M';
    put32(f + 2, (uint32_t)(off + stride * ah));
    put32(f + 10, (uint32_t)off);
    put32(f + 14, 40);
    put32(f + 18, (uint32_t)w);
    put32(f + 22, (uint32_t)h);
    put16(f + 26, 1);
    put16(f + 28, bpp);
    put32(f + 30, comp);
    for (int y = 0; y < ah; y++) {
        uint8_t *row = f + off + stride * (h < 0 ? y : ah - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            rgb(x, y, &r, &g, &b);
            uint8_t *p = row + x * (bpp / 8);
            p[0] = b; p[1] = g; p[2] = r;
            if (bpp == 32) p[3] = 0xFF;
        }
    }
    return off + stride * ah;
}

static int compare(const uint16_t *px, int w, int h)
{
    int bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            rgb(x, y, &r, &g, &b);
            if (px[y * w + x] != rgb565(r, g, b)) bad++;
        }
    return bad;
}

int main(void)
{
    uint16_t px[64];
    int w, h;

    size_t n = make(3, 4, 24, 0);            /* 9-byte rows padded to 12 */
    CHECK(bmp_size(f, n, &w, &h) && w == 3 && h == 4, "24-bit size reads 3x4 (%dx%d)", w, h);
    CHECK(bmp_to_565(f, n, 3, 4, px) && compare(px, 3, 4) == 0, "24-bit bottom-up with padding decodes every pixel (%d wrong)",
          compare(px, 3, 4));

    n = make(5, -3, 32, 0);
    CHECK(bmp_size(f, n, &w, &h) && w == 5 && h == 3, "a top-down file reports a positive height (%dx%d)", w, h);
    CHECK(bmp_to_565(f, n, 5, 3, px) && compare(px, 5, 3) == 0, "32-bit top-down decodes every pixel (%d wrong)", compare(px, 5, 3));

    n = make(4, 2, 32, 3);
    CHECK(bmp_to_565(f, n, 4, 2, px) && compare(px, 4, 2) == 0, "32-bit BI_BITFIELDS decodes");

    /* refused, with out untouched */
    uint16_t keep[64];
    memset(keep, 0x5A, sizeof keep);
    memcpy(px, keep, sizeof px);
    n = make(3, 4, 24, 0);
    CHECK(!bmp_to_565(f, n, 4, 3, px), "the wrong size is refused (a landscape file in portrait)");
    CHECK(!bmp_to_565(f, n - 1, 3, 4, px), "a file one byte short is refused");
    CHECK(!bmp_size(f, 53, &w, &h), "a file shorter than its header is refused");
    n = make(3, 4, 16, 0);
    CHECK(!bmp_size(f, n, &w, &h), "16-bit is refused");
    n = make(3, 4, 24, 1);
    CHECK(!bmp_size(f, n, &w, &h), "RLE is refused");
    n = make(3, 4, 24, 3);
    CHECK(!bmp_size(f, n, &w, &h), "BI_BITFIELDS is refused at 24 bits");
    n = make(3, 4, 24, 0);
    f[0] = 'P';
    CHECK(!bmp_size(f, n, &w, &h), "a file that doesn't start BM is refused");
    n = make(3, 4, 24, 0);
    put32(f + 10, (uint32_t)n + 10);
    CHECK(!bmp_size(f, n, &w, &h), "pixel data past the end is refused");
    n = make(3, 4, 24, 0);
    put32(f + 18, 0);
    CHECK(!bmp_size(f, n, &w, &h), "zero width is refused");
    CHECK(memcmp(px, keep, sizeof px) == 0, "and nothing is written when a file is refused");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
