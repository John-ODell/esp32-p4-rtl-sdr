#pragma once
/* Uncompressed 24- or 32-bit BMP files (bottom-up or top-down) to RGB565, for a custom boot splash
 * from the SD card (#22). No ESP-IDF calls, so tools/bmp_test runs it on a PC. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read the width and height. False if f isn't a BMP this file can decode. */
bool bmp_size(const uint8_t *f, size_t len, int *w, int *h);
/* Decode an image of exactly w x h into out (w * h pixels, row by row from the top).
 * False, with out untouched, for any other size or a file it can't decode. */
bool bmp_to_565(const uint8_t *f, size_t len, int w, int h, uint16_t *out);
