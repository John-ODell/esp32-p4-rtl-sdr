/* WFM / NFM / AM demodulator: decimating FIR chain, discriminator or envelope, squelch. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "demod.h"
#include "rds.h"

#define CHUNK 4800                            /* IQ pairs per pass: 2 ms */
/* mean square change of the discriminator's phase step on noise alone, per filter (demod_test) */
#define WFM_NOISE   5.6f
#define NFM25_NOISE 1.95f
#define NFM12_NOISE 0.55f

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
static float s_pd, s_msd = 1, s_ref = 1;      /* FM noise: last phase step, mean square change, its noise-only value */
static float s_quality;                       /* dB the squelch compares: FM quiet, AM above the floor */

bool demod_init(void)
{
    for (int i = 0; i < 256; i++) s_u8[i] = (i - 127.4f) / 128.0f;
    bool ok = fir_alloc(&s_a, 49, 5, 2, CHUNK) &&
              fir_alloc(&s_w1, 41, 5, 1, CHUNK / 5 + 1) &&
              fir_alloc(&s_w2, 101, 2, 1, CHUNK / 25 + 1) &&
              fir_alloc(&s_n1, 41, 5, 2, CHUNK / 5 + 1) &&
              fir_alloc(&s_n2, 161, 2, 2, CHUNK / 25 + 1);
    s_tmp = BIG_ALLOC((CHUNK / 5 + 1) * 2 * sizeof(float));
    if (!ok || !s_tmp || !rds_init()) return false;
    demod_set(DEMOD_WFM, 200000);
    return true;
}

const char *demod_mode_name(demod_mode_t m) { return m == DEMOD_WFM ? "WFM" : m == DEMOD_NFM ? "NFM" : "AM"; }
demod_mode_t demod_mode(void) { return s_mode; }

void demod_set(demod_mode_t mode, uint32_t bw_hz)
{
    s_mode = mode;
    /* 480 kS/s keeps +-100 kHz flat; everything that would fold into it is 60 dB down */
    fir_design(&s_a, 49, DEMOD_IN_RATE, 100000, 370000, 60);
    if (mode == DEMOD_WFM) {
        fir_design(&s_w1, 41, 480000, 16000, 77000, 60);
        fir_design(&s_w2, 101, 96000, 15000, 19000, 50);   /* 19 kHz pilot out */
        s_fm_gain = 1.0f / (2 * PI_F * 75000 / 480000);    /* 75 kHz deviation = full scale */
        s_ref = WFM_NOISE;
    } else {
        float cut = bw_hz * 0.4f;                          /* 25k: 10k, 12.5k: 5k, 8k: 3.2k */
        fir_design(&s_n1, 41, 480000, cut, 96000 - cut, 60);
        fir_design(&s_n2, 161, 96000, cut, cut * 1.6f, 60);
        float dev = bw_hz >= 20000 ? 5000 : 2500;
        s_fm_gain = 1.0f / (2 * PI_F * dev / DEMOD_OUT_RATE);
        s_ref = bw_hz >= 20000 ? NFM25_NOISE : NFM12_NOISE;
    }
    s_de_a = expf(-1.0f / (96000 * 75e-6f));
    s_pre = s_pim = s_deemph = s_dc = 0;
    s_hang = 0;
    s_floor_set = false;
    s_pd = 0;
    s_msd = s_ref;
    rds_reset();
}

void demod_retuned(void) { rds_reset(); }

void demod_set_squelch(int level) { s_sql = level < 0 ? 0 : level > 9 ? 9 : level; }
float demod_level_db(void) { return 10 * log10f(s_pow + 1e-12f); }
float demod_floor_db(void) { return s_floor; }
float demod_quality_db(void) { return s_quality; }
bool demod_open(void) { return s_open; }

/* Channel power of m complex samples into the smoothed level and the floor. The floor drops to
 * any lower level at once and creeps up 1 dB/s while the squelch is shut. */
static void level(const float *x, int m, float dt_s)
{
    if (m <= 0) return;
    float p = 0;
    for (int i = 0; i < m; i++) p += x[2 * i] * x[2 * i] + x[2 * i + 1] * x[2 * i + 1];
    s_pow += (p / m - s_pow) * 0.1f;                       /* about 20 ms */
    float db = demod_level_db();
    if (!s_floor_set) { s_floor = db; s_floor_set = true; }
    if (db < s_floor) s_floor = db;
    else if (!s_open || !s_sql) s_floor += 1.0f * dt_s;
}

/* FM: how much quieter the discriminator is than on noise alone (a carrier quiets it), from the
 * mean square change of its phase steps; this needs no floor, so a station already on the air
 * when you tune to it opens the squelch. AM: level above the floor. Then open or shut, with a
 * 150 ms tail. */
static void decide(float msd)
{
    if (s_mode == DEMOD_AM) s_quality = demod_level_db() - s_floor;
    else {
        s_msd += (msd - s_msd) * 0.1f;
        s_quality = 10 * log10f(s_ref / (s_msd + 1e-9f));
    }
    if (!s_sql) { s_open = true; return; }
    float thr = 3 * s_sql;
    if (s_quality > thr + 1) { s_open = true; s_hang = 75; }
    else if (s_quality < thr - 1) {
        if (s_hang > 0) s_hang--;
        else s_open = false;
    }
}

/* phase step of the discriminator, radians, and its change into *acc */
static float fm_step(float re, float im, float *acc)
{
    float d = fast_atan2(im * s_pre - re * s_pim, re * s_pre + im * s_pim);
    s_pre = re;
    s_pim = im;
    *acc += (d - s_pd) * (d - s_pd);
    s_pd = d;
    return d;
}

/* the squelch gate fades over 5 ms instead of switching, so it doesn't click */
static float s_gate;
static bool s_was_open;                       /* AM: squelch state at the last sample */

static int16_t to_s16(float v)
{
    float want = s_open ? 1.0f : 0.0f;
    if (s_gate < want) s_gate = fminf(want, s_gate + 1.0f / 240);
    else if (s_gate > want) s_gate = fmaxf(want, s_gate - 1.0f / 240);
    v *= s_gate * 0.7f * 32767;
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
        level(a, na, n / (float)DEMOD_IN_RATE);
        float *d = fir_in(&s_w1), acc = 0;
        for (int i = 0; i < na; i++) d[i] = fm_step(a[2 * i], a[2 * i + 1], &acc) * s_fm_gain;
        rds_process(d, na);                                /* the multiplex, before the audio filters */
        if (na) decide(acc / na);
        float *e = fir_in(&s_w2);
        int nb = fir_run(&s_w1, na, e);
        for (int i = 0; i < nb; i++) e[i] = s_deemph = e[i] + s_de_a * (s_deemph - e[i]);
        float aud[CHUNK / 50 + 2];
        k = fir_run(&s_w2, nb, aud);
        for (int i = 0; i < k; i++) out[i] = to_s16(aud[i]);
    } else {
        int nb = fir_run(&s_n1, na, fir_in(&s_n2));
        float ch[2 * (CHUNK / 50 + 2)];
        k = fir_run(&s_n2, nb, ch);
        level(ch, k, n / (float)DEMOD_IN_RATE);
        float acc = 0;
        for (int i = 0; i < k; i++) {
            float re = ch[2 * i], im = ch[2 * i + 1], v;
            if (s_mode == DEMOD_NFM) {
                v = fm_step(re, im, &acc) * s_fm_gain;
                s_dc += (v - s_dc) * 0.002f;               /* tuning offset out, ~15 Hz */
                v -= s_dc;
            } else {
                float env = sqrtf(re * re + im * im);
                if (s_dc == 0 || (s_open && !s_was_open)) s_dc = env;   /* a carrier that just opened the squelch */
                s_was_open = s_open;
                s_dc += (env - s_dc) * 0.001f;             /* the carrier, ~8 Hz */
                v = s_dc > 1e-6f ? (env - s_dc) / s_dc : 0;  /* modulation depth: built-in AGC */
            }
            out[i] = to_s16(v);
        }
        if (k) decide(acc / k);
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
