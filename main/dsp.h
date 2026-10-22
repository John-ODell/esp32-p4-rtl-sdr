#pragma once
/* Shared DSP pieces for demod.c and rds.c. No ESP-IDF calls (tools/demod_test builds them). */
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
float fast_atan2(float y, float x);
