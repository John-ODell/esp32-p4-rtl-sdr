/* Shared DSP pieces: decimating FIR filters and a fast atan2. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"

bool fir_alloc(fir_t *f, int n_max, int d, int w, int cap)
{
    f->d = d; f->w = w; f->cap = cap;
    f->h = BIG_ALLOC(n_max * sizeof(float));
    f->buf = BIG_ALLOC((n_max - 1 + cap) * w * sizeof(float));
    return f->h && f->buf;
}

static float bessel_i0(float x)
{
    float s = 1, t = 1;
    for (int k = 1; k < 30; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

void fir_design(fir_t *f, int n_max, float fs, float pass, float stop, float atten_db)
{
    float dw = 2 * PI_F * (stop - pass) / fs;
    int n = (int)ceilf((atten_db - 8) / (2.285f * dw)) + 1;
    n |= 1;
    if (n > n_max) n = n_max | 1;
    if (n > n_max) n -= 2;
    float beta = atten_db > 50 ? 0.1102f * (atten_db - 8.7f) : 0.5842f * powf(atten_db - 21, 0.4f) + 0.07886f * (atten_db - 21);
    float fc = (pass + stop) / 2 / fs, sum = 0, i0b = bessel_i0(beta);
    for (int i = 0; i < n; i++) {
        float m = i - (n - 1) / 2.0f, r = 2.0f * i / (n - 1) - 1;
        float s = m == 0 ? 2 * fc : sinf(2 * PI_F * fc * m) / (PI_F * m);
        f->h[i] = s * bessel_i0(beta * sqrtf(1 - r * r)) / i0b;
        sum += f->h[i];
    }
    for (int i = 0; i < n; i++) f->h[i] /= sum;
    f->n = n;
    f->phase = 0;
    memset(f->buf, 0, (n - 1) * f->w * sizeof(float));
}

int fir_run(fir_t *f, int m, float *out)
{
    const int n = f->n, w = f->w, total = n - 1 + m;
    const float *h = f->h;
    int k = 0, p;
    /* two or four running sums, so the multiply-adds don't each wait for the last one */
    for (p = n - 1 + f->phase; p < total; p += f->d) {
        const float *x = f->buf + (p - (n - 1)) * w;
        int j = 0;
        if (w == 2) {
            float re0 = 0, im0 = 0, re1 = 0, im1 = 0;
            for (; j + 1 < n; j += 2) {
                re0 += h[j] * x[2 * j];         im0 += h[j] * x[2 * j + 1];
                re1 += h[j + 1] * x[2 * j + 2]; im1 += h[j + 1] * x[2 * j + 3];
            }
            for (; j < n; j++) { re0 += h[j] * x[2 * j]; im0 += h[j] * x[2 * j + 1]; }
            out[2 * k] = re0 + re1;
            out[2 * k + 1] = im0 + im1;
        } else {
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            for (; j + 3 < n; j += 4) {
                a0 += h[j] * x[j];         a1 += h[j + 1] * x[j + 1];
                a2 += h[j + 2] * x[j + 2]; a3 += h[j + 3] * x[j + 3];
            }
            for (; j < n; j++) a0 += h[j] * x[j];
            out[k] = (a0 + a1) + (a2 + a3);
        }
        k++;
    }
    f->phase = p - total;
    memmove(f->buf, f->buf + m * w, (n - 1) * w * sizeof(float));
    return k;
}
