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
    for (p = n - 1 + f->phase; p < total; p += f->d) {
        const float *x = f->buf + (p - (n - 1)) * w;
        if (w == 2) {
            float re = 0, im = 0;
            for (int j = 0; j < n; j++) { re += h[j] * x[2 * j]; im += h[j] * x[2 * j + 1]; }
            out[2 * k] = re;
            out[2 * k + 1] = im;
        } else {
            float a = 0;
            for (int j = 0; j < n; j++) a += h[j] * x[j];
            out[k] = a;
        }
        k++;
    }
    f->phase = p - total;
    memmove(f->buf, f->buf + m * w, (n - 1) * w * sizeof(float));
    return k;
}

/* within 1e-5 rad */
float fast_atan2(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx == 0) return 0;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = PI_F - r;
    return y < 0 ? -r : r;
}
