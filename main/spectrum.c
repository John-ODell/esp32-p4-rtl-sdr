/* Averaged power spectrum: Hann window, radix-2 complex FFT, |X|^2 averaged, dBFS. */
#include <math.h>
#include <string.h>
#include "spectrum.h"

static int s_n, s_log2n;
static float s_cos[NFFT_MAX / 2], s_sin[NFFT_MAX / 2], s_win[NFFT_MAX];
static uint16_t s_rev[NFFT_MAX];
static float s_re[NFFT_MAX], s_im[NFFT_MAX], s_pow[NFFT_MAX];
static float s_u8[256];                        /* u8 sample -> float, centered */
static float s_norm;

void spectrum_set_size(int n)
{
    if (n < 256) n = 256;
    if (n > NFFT_MAX) n = NFFT_MAX;
    s_n = n;
    for (s_log2n = 0; (1 << s_log2n) < n; s_log2n++) {}
    float wsum = 0;
    for (int i = 0; i < n; i++) {
        s_win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / n);
        wsum += s_win[i];
        unsigned r = 0;
        for (int b = 0; b < s_log2n; b++) if (i & (1 << b)) r |= 1 << (s_log2n - 1 - b);
        s_rev[i] = r;
    }
    for (int k = 0; k < n / 2; k++) {
        s_cos[k] = cosf(2 * (float)M_PI * k / n);
        s_sin[k] = -sinf(2 * (float)M_PI * k / n);
    }
    s_norm = 1.0f / (wsum * wsum);             /* full-scale complex tone -> 0 dBFS */
}

int spectrum_size(void) { return s_n; }

void spectrum_init(void)
{
    for (int i = 0; i < 256; i++) s_u8[i] = (i - 127.4f) / 128.0f;
    spectrum_set_size(1024);
}

static void fft(float *re, float *im)
{
    const int NFFT = s_n;
    for (int i = 0; i < NFFT; i++) {
        int j = s_rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2, step = NFFT / 2; len <= NFFT; len <<= 1, step >>= 1) {
        int half = len >> 1;
        for (int i = 0; i < NFFT; i += len) {
            for (int j = 0; j < half; j++) {
                float wr = s_cos[j * step], wi = s_sin[j * step];
                int a = i + j, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

void spectrum_compute(const uint8_t *iq, int navg, float *out_db)
{
    const int NFFT = s_n;
    memset(s_pow, 0, NFFT * sizeof s_pow[0]);
    for (int k = 0; k < navg; k++) {
        const uint8_t *p = iq + (size_t)k * NFFT * 2;
        for (int i = 0; i < NFFT; i++) {
            s_re[i] = s_u8[p[2 * i]] * s_win[i];
            s_im[i] = s_u8[p[2 * i + 1]] * s_win[i];
        }
        fft(s_re, s_im);
        for (int i = 0; i < NFFT; i++) s_pow[i] += s_re[i] * s_re[i] + s_im[i] * s_im[i];
    }
    float scale = s_norm / navg;
    for (int i = 0; i < NFFT; i++) {
        float v = s_pow[(i + NFFT / 2) & (NFFT - 1)] * scale;   /* fft shift */
        out_db[i] = 10.0f * log10f(v + 1e-20f);
    }
}
