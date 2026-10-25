/* WFM / NFM / AM demodulator: CIC then FIR decimation, discriminator or envelope, squelch. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "demod.h"
#include "rds.h"

#define CHUNK 4800                            /* IQ pairs per pass: 2 ms */
/* mean square change of the discriminator's phase step on noise alone, per filter (demod_test) */
#define WFM_NOISE   3.1f
#define NFM25_NOISE 1.95f
#define NFM12_NOISE 0.55f

/* ------------------------------------------------------------------------- state -- */

/* 2.4 MS/s -> 480 kS/s: a 5th-order CIC on the raw bytes, integer adds only. Its nulls sit on
 * every multiple of 480 kHz, so what would fold into +-100 kHz is about 60 dB down, and the
 * passband droops 1.7 dB at 75 kHz (FM doesn't mind; NFM and AM use the middle). */
#define CIC_N 5
#define CIC_GAIN 3125                         /* 5^5 */
static uint32_t s_int[CIC_N][2], s_comb[CIC_N][2];
static int s_cic_ph;
static fir_t s_w1, s_w2;                      /* WFM audio: 480k -> 96k -> 48k, real */
static fir_t s_n1, s_n2;                      /* NFM / AM channel: 480k -> 96k -> 48k, complex */
static float *s_tmp;                          /* stage outputs that feed two places */
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
    bool ok = fir_alloc(&s_w1, 41, 5, 1, CHUNK / 5 + 1) &&
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
    memset(s_int, 0, sizeof s_int);
    memset(s_comb, 0, sizeof s_comb);
    s_cic_ph = 0;
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

/* new station: nothing heard before carries over (a squelch held open by the last channel would
 * hold a scan on this one) */
void demod_retuned(void)
{
    rds_reset();
    s_msd = s_ref;
    s_hang = 0;
    if (s_sql) s_open = false;
}

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
    int step = m >= 64 ? 4 : 1, used = 0;                  /* every 4th sample is plenty for a level */
    for (int i = 0; i < m; i += step, used++) p += x[2 * i] * x[2 * i] + x[2 * i + 1] * x[2 * i + 1];
    s_pow += (p / used - s_pow) * 0.1f;                    /* about 20 ms */
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

/* fm_step over m complex samples into d, times gain; returns the sum of the squared changes. The
 * state lives in locals, so the stores to d don't make the compiler reload it every sample. */
static float fm_block(const float *a, int m, float *d, float gain)
{
    float pre = s_pre, pim = s_pim, pd = s_pd, acc = 0;
    for (int i = 0; i < m; i++) {
        float re = a[2 * i], im = a[2 * i + 1];
        float v = fast_atan2(im * pre - re * pim, re * pre + im * pim);
        pre = re;
        pim = im;
        acc += (v - pd) * (v - pd);
        pd = v;
        d[i] = v * gain;
    }
    s_pre = pre;
    s_pim = pim;
    s_pd = pd;
    return acc;
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

/* CIC_STEP: one input pair through the integrators. CIC_COMB: the combs, on every 5th pair. */
#define CIC_STEP(re, im) do { \
        i0 += (re); i1 += i0; i2 += i1; i3 += i2; i4 += i3; \
        q0 += (im); q1 += q0; q2 += q1; q3 += q2; q4 += q3; \
    } while (0)
#define CIC_COMB(v, c) do { \
        uint32_t t_; \
        t_ = v - c[0]; c[0] = v; v = t_; t_ = v - c[1]; c[1] = v; v = t_; \
        t_ = v - c[2]; c[2] = v; v = t_; t_ = v - c[3]; c[3] = v; v = t_; \
        t_ = v - c[4]; c[4] = v; v = t_; \
    } while (0)

/* n IQ pairs at 2.4 MS/s into a, interleaved, at 480 kS/s; returns the pairs made. The sums wrap
 * in 32 bits, which the combs undo exactly. Whole groups of 5 run unrolled. */
static int cic(const uint8_t *iq, int n, float *a)
{
    const float scale = 1.0f / (128.0f * CIC_GAIN), dc = 127.4f * CIC_GAIN;
    uint32_t i0 = s_int[0][0], i1 = s_int[1][0], i2 = s_int[2][0], i3 = s_int[3][0], i4 = s_int[4][0];
    uint32_t q0 = s_int[0][1], q1 = s_int[1][1], q2 = s_int[2][1], q3 = s_int[3][1], q4 = s_int[4][1];
    uint32_t ci[CIC_N], cq[CIC_N];
    for (int s = 0; s < CIC_N; s++) { ci[s] = s_comb[s][0]; cq[s] = s_comb[s][1]; }
    int ph = s_cic_ph, k = 0;
    const uint8_t *p = iq, *end = iq + 2 * n;
    while (p < end) {
        if (ph == 0 && end - p >= 10) {
            CIC_STEP(p[0], p[1]); CIC_STEP(p[2], p[3]); CIC_STEP(p[4], p[5]);
            CIC_STEP(p[6], p[7]); CIC_STEP(p[8], p[9]);
            p += 10;
        } else {
            CIC_STEP(p[0], p[1]);
            p += 2;
            if (++ph < 5) continue;
            ph = 0;
        }
        uint32_t vi = i4, vq = q4;
        CIC_COMB(vi, ci);
        CIC_COMB(vq, cq);
        a[2 * k] = ((float)vi - dc) * scale;
        a[2 * k + 1] = ((float)vq - dc) * scale;
        k++;
    }
    s_int[0][0] = i0; s_int[1][0] = i1; s_int[2][0] = i2; s_int[3][0] = i3; s_int[4][0] = i4;
    s_int[0][1] = q0; s_int[1][1] = q1; s_int[2][1] = q2; s_int[3][1] = q3; s_int[4][1] = q4;
    for (int s = 0; s < CIC_N; s++) { s_comb[s][0] = ci[s]; s_comb[s][1] = cq[s]; }
    s_cic_ph = ph;
    return k;
}

static int chunk(const uint8_t *iq, int n, int16_t *out)
{
    float *a = s_mode == DEMOD_WFM ? s_tmp : fir_in(&s_n1);
    int na = cic(iq, n, a);
    int k = 0;
    if (s_mode == DEMOD_WFM) {
        level(a, na, n / (float)DEMOD_IN_RATE);
        float *d = fir_in(&s_w1), acc = fm_block(a, na, d, s_fm_gain);
        rds_process(d, na);                                /* the multiplex, before the audio filters */
        if (na) decide(acc / na);
        float *e = fir_in(&s_w2);
        int nb = fir_run(&s_w1, na, e);
        float de = s_deemph;
        for (int i = 0; i < nb; i++) e[i] = de = e[i] + s_de_a * (de - e[i]);
        s_deemph = de;
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
