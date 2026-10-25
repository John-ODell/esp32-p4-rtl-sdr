#pragma once
/* Shared DSP pieces for demod.c and rds.c. No ESP-IDF calls (tools/demod_test builds them). */
#include <math.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#include <stdlib.h>
#define BIG_ALLOC(n) calloc(1, (n))
#endif

#define PI_F 3.14159265f

/* Decimating FIR, real taps, real (w = 1) or interleaved complex (w = 2) samples. The caller
 * writes up to cap new samples at fir_in(), then fir_run() filters them and keeps n - 1. */
typedef struct {
    int n, d, w, cap, phase;
    float *h, *buf;
} fir_t;

bool fir_alloc(fir_t *f, int n_max, int d, int w, int cap);
/* Kaiser-windowed low pass, pass and stop edges in Hz at rate fs, atten_db in the stop band.
 * The length comes from the transition width, odd and at most n_max. Clears the history. */
void fir_design(fir_t *f, int n_max, float fs, float pass, float stop, float atten_db);
static inline float *fir_in(fir_t *f) { return f->buf + (f->n - 1) * f->w; }
/* filter m new samples (written at fir_in) into out; returns the outputs made */
int fir_run(fir_t *f, int m, float *out);

/* within 1e-5 rad; inline, because the FM discriminator calls it 480,000 times a second */
static inline float fast_atan2(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float mx = fmaxf(ax, ay), mn = fminf(ax, ay);
    if (mx == 0) return 0;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    r = ay > ax ? 1.57079637f - r : r;
    r = x < 0 ? PI_F - r : r;
    return copysignf(r, y);
}
