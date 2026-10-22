/* WFM / NFM / AM demodulator: decimating FIR chain, discriminator or envelope, squelch. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "demod.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#endif

#define CHUNK 4800                            /* IQ pairs per pass: 2 ms */
#define PI_F  3.14159265f

/* ------------------------------------------------------------------- FIR filters -- */

/* Decimating FIR, real taps, real (w = 1) or interleaved complex (w = 2) samples. The caller
 * writes up to cap new samples at fir_in(), then fir_run() filters them and keeps n - 1. */
typedef struct {
    int n, d, w, cap, phase;
    float *h, *buf;
} fir_t;

static bool fir_alloc(fir_t *f, int n_max, int d, int w, int cap)
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

/* Kaiser-windowed low pass, pass and stop edges in Hz at rate fs, atten_db in the stop band.
 * The length comes from the transition width, odd and at most n_max. */
static void fir_design(fir_t *f, int n_max, float fs, float pass, float stop, float atten_db)
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

static float *fir_in(fir_t *f) { return f->buf + (f->n - 1) * f->w; }

/* filter m new samples (written at fir_in) into out; returns the outputs made */
static int fir_run(fir_t *f, int m, float *out)
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

/* ------------------------------------------------------------------------- state -- */

static fir_t s_a;                             /* 2.4 MS/s -> 480 kS/s, complex */
static fir_t s_w1, s_w2;                      /* WFM audio: 480k -> 96k -> 48k, real */
static fir_t s_n1, s_n2;                      /* NFM / AM channel: 480k -> 96k -> 48k, complex */
static float *s_tmp;                          /* stage outputs that feed two places */
static float s_u8[256];
static demod_mode_t s_mode;
static float s_fm_gain;                       /* radians per sample -> audio */
static float s_pre, s_pim;                    /* previous complex sample (discriminator) */
static float s_deemph, s_de_a;
static float s_dc;                            /* DC tracker: NFM offset, AM carrier */
static float s_pow = 1e-9f, s_floor = 0;      /* channel power (linear), floor (dB) */
static bool s_floor_set, s_open = true;
static int s_sql, s_hang;

static float fast_atan2(float y, float x)
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

bool demod_init(void)
{
    for (int i = 0; i < 256; i++) s_u8[i] = (i - 127.4f) / 128.0f;
    bool ok = fir_alloc(&s_a, 49, 5, 2, CHUNK) &&
              fir_alloc(&s_w1, 41, 5, 1, CHUNK / 5 + 1) &&
              fir_alloc(&s_w2, 101, 2, 1, CHUNK / 25 + 1) &&
              fir_alloc(&s_n1, 41, 5, 2, CHUNK / 5 + 1) &&
              fir_alloc(&s_n2, 161, 2, 2, CHUNK / 25 + 1);
    s_tmp = BIG_ALLOC((CHUNK / 5 + 1) * 2 * sizeof(float));
    if (!ok || !s_tmp) return false;
    demod_set(DEMOD_WFM, 200000);
    return true;
}

const char *demod_mode_name(demod_mode_t m) { return m == DEMOD_WFM ? "WFM" : m == DEMOD_NFM ? "NFM" : "AM"; }
demod_mode_t demod_mode(void) { return s_mode; }

void demod_reset_floor(void) { s_floor_set = false; }

void demod_set(demod_mode_t mode, uint32_t bw_hz)
{
    s_mode = mode;
    /* 480 kS/s keeps +-100 kHz flat; everything that would fold into it is 60 dB down */
    fir_design(&s_a, 49, DEMOD_IN_RATE, 100000, 370000, 60);
    if (mode == DEMOD_WFM) {
        fir_design(&s_w1, 41, 480000, 16000, 77000, 60);
        fir_design(&s_w2, 101, 96000, 15000, 19000, 50);   /* 19 kHz pilot out */
        s_fm_gain = 1.0f / (2 * PI_F * 75000 / 480000);    /* 75 kHz deviation = full scale */
    } else {
        float cut = bw_hz * 0.4f;                          /* 25k: 10k, 12.5k: 5k, 8k: 3.2k */
        fir_design(&s_n1, 41, 480000, cut, 96000 - cut, 60);
        fir_design(&s_n2, 161, 96000, cut, cut * 1.6f, 60);
        float dev = bw_hz >= 20000 ? 5000 : 2500;
        s_fm_gain = 1.0f / (2 * PI_F * dev / DEMOD_OUT_RATE);
    }
    s_de_a = expf(-1.0f / (96000 * 75e-6f));
    s_pre = s_pim = s_deemph = s_dc = 0;
    s_hang = 0;
    s_floor_set = false;
}

void demod_set_squelch(int level) { s_sql = level < 0 ? 0 : level > 9 ? 9 : level; }
float demod_level_db(void) { return 10 * log10f(s_pow + 1e-12f); }
float demod_floor_db(void) { return s_floor; }
bool demod_open(void) { return s_open; }

/* power of m complex samples into the smoothed level, then the squelch decision */
static void squelch(const float *x, int m, float dt_s)
{
    if (m <= 0) return;
    float p = 0;
    for (int i = 0; i < m; i++) p += x[2 * i] * x[2 * i] + x[2 * i + 1] * x[2 * i + 1];
    s_pow += (p / m - s_pow) * 0.1f;                       /* about 20 ms */
    float db = demod_level_db();
    if (!s_floor_set) { s_floor = db; s_floor_set = true; }
    if (db < s_floor) s_floor = db;
    else if (!s_open || !s_sql) s_floor += 1.0f * dt_s;    /* creeps up 1 dB/s while closed */
    if (!s_sql) { s_open = true; return; }
    float thr = s_floor + 3 * s_sql;
    if (db > thr + 1) { s_open = true; s_hang = 75; }       /* 150 ms tail */
    else if (db < thr - 1) {
        if (s_hang > 0) s_hang--;
        else s_open = false;
    }
}

static int16_t to_s16(float v)
{
    v *= 0.7f * 32767;
    return v > 32767 ? 32767 : v < -32767 ? -32767 : (int16_t)lrintf(v);
}

static int chunk(const uint8_t *iq, int n, int16_t *out)
{
    float *in = fir_in(&s_a);
    for (int i = 0; i < 2 * n; i++) in[i] = s_u8[iq[i]];
    float *a = s_mode == DEMOD_WFM ? s_tmp : fir_in(&s_n1);
    int na = fir_run(&s_a, n, a);
    int k = 0;
    if (s_mode == DEMOD_WFM) {
        squelch(a, na, n / (float)DEMOD_IN_RATE);
        float *d = fir_in(&s_w1);
        for (int i = 0; i < na; i++) {
            float re = a[2 * i], im = a[2 * i + 1];
            d[i] = fast_atan2(im * s_pre - re * s_pim, re * s_pre + im * s_pim) * s_fm_gain;
            s_pre = re;
            s_pim = im;
        }
        float *e = fir_in(&s_w2);
        int nb = fir_run(&s_w1, na, e);
        for (int i = 0; i < nb; i++) e[i] = s_deemph = e[i] + s_de_a * (s_deemph - e[i]);
        float aud[CHUNK / 50 + 2];
        k = fir_run(&s_w2, nb, aud);
        for (int i = 0; i < k; i++) out[i] = s_open ? to_s16(aud[i]) : 0;
    } else {
        int nb = fir_run(&s_n1, na, fir_in(&s_n2));
        float ch[2 * (CHUNK / 50 + 2)];
        k = fir_run(&s_n2, nb, ch);
        squelch(ch, k, n / (float)DEMOD_IN_RATE);
        for (int i = 0; i < k; i++) {
            float re = ch[2 * i], im = ch[2 * i + 1], v;
            if (s_mode == DEMOD_NFM) {
                v = fast_atan2(im * s_pre - re * s_pim, re * s_pre + im * s_pim) * s_fm_gain;
                s_pre = re;
                s_pim = im;
                s_dc += (v - s_dc) * 0.002f;               /* tuning offset out, ~15 Hz */
                v -= s_dc;
            } else {
                float env = sqrtf(re * re + im * im);
                if (s_dc == 0) s_dc = env;
                s_dc += (env - s_dc) * 0.001f;             /* the carrier, ~8 Hz */
                v = s_dc > 1e-6f ? (env - s_dc) / s_dc : 0;  /* modulation depth: built-in AGC */
            }
            out[i] = s_open ? to_s16(v) : 0;
        }
    }
    return k;
}

int demod_process(const uint8_t *iq, int n, int16_t *out)
{
    int k = 0;
    while (n > 0) {
        int m = n < CHUNK ? n : CHUNK;
        k += chunk(iq, m, out + k);
        iq += 2 * m;
        n -= m;
    }
    return k;
}
