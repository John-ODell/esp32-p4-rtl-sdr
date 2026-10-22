/* Host test for main/demod.c: FM and AM signals with a 1 kHz tone are synthesized as 2.4 MS/s
 * u8 IQ with noise, an off-channel station and (WFM) a stereo pilot, fed through demod_process
 * in odd-sized pieces, and the audio is checked: the tone's SINAD, what is left of the
 * interferer and the pilot, and the squelch in every mode. A full broadcast station (audio, pilot,
 * RDS groups) checks the RDS decoder's PI, station name and RadioText. Build and run: make -C tools/demod_test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "demod.h"
#include "rds.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define FS   DEMOD_IN_RATE
#define SECS 0.5
#define N    1200000                         /* SECS of IQ */

static uint8_t s_iq[2 * N];
static int16_t s_out[N / 50 + 64];

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static uint8_t q8(double v)
{
    int a = (int)lrint(127.4 + 128 * v);
    return a < 0 ? 0 : a > 255 ? 255 : a;
}

typedef struct {
    int am;                                  /* 0 FM, 1 AM */
    double amp, offset_hz, tone_hz, dev_hz;  /* dev: FM deviation, or AM depth (0..1) */
    double pilot_dev;                        /* FM: 19 kHz pilot deviation, 0 for none */
} station_t;

/* sum of signals plus noise (sigma per I and Q, full scale = 1) */
static void make(const station_t *s, int ns, double noise)
{
    double ph[4] = {0};
    for (int n = 0; n < N; n++) {
        double t = (double)n / FS, i = noise * gauss(), q = noise * gauss();
        for (int k = 0; k < ns; k++) {
            const station_t *g = &s[k];
            double m = sin(2 * M_PI * g->tone_hz * t), a = g->amp;
            if (g->am) {
                a *= 1 + g->dev_hz * m;
                ph[k] += 2 * M_PI * g->offset_hz / FS;
            } else {
                double f = g->offset_hz + g->dev_hz * m + g->pilot_dev * sin(2 * M_PI * 19000 * t);
                ph[k] += 2 * M_PI * f / FS;
            }
            i += a * cos(ph[k]);
            q += a * sin(ph[k]);
        }
        s_iq[2 * n] = q8(i);
        s_iq[2 * n + 1] = q8(q);
    }
}

/* run the demodulator over the whole buffer in uneven pieces; returns the audio count */
static int run(double *cpu_s)
{
    static const int piece[] = {1, 7, 4800, 333, 12000, 24000, 50, 9999};
    int pos = 0, k = 0, j = 0;
    clock_t t0 = clock();
    while (pos < N) {
        int m = piece[j++ % 8];
        if (m > N - pos) m = N - pos;
        k += demod_process(s_iq + 2 * pos, m, s_out + k);
        pos += m;
    }
    if (cpu_s) *cpu_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    return k;
}

/* least-squares fit of a sine at f (Hz) over x[from..n): its power and what is left over */
static void fit(const int16_t *x, int from, int n, double f, double *sig, double *rest)
{
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0, xx = 0, mean = 0;
    for (int i = from; i < n; i++) mean += x[i];
    mean /= n - from;
    for (int i = from; i < n; i++) {
        double t = (double)i / DEMOD_OUT_RATE, s = sin(2 * M_PI * f * t), c = cos(2 * M_PI * f * t), v = x[i] - mean;
        ss += s * s; sc += s * c; cc += c * c; xs += v * s; xc += v * c; xx += v * v;
    }
    double det = ss * cc - sc * sc, a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double m = n - from;
    *sig = (a * xs + b * xc) / m;
    *rest = xx / m - *sig;
}

static double db(double x) { return 10 * log10(x + 1e-20); }

/* one case: SINAD of the 1 kHz tone, and how much of each other tone gets through */
static void tone_case(const char *name, demod_mode_t mode, uint32_t bw, const station_t *s, int ns, double noise,
                      double min_sinad, double other_hz, double max_other_db)
{
    make(s, ns, noise);
    demod_set(mode, bw);
    demod_set_squelch(0);
    double cpu;
    int k = run(&cpu);
    int want = N / 50;
    CHECK(abs(k - want) <= 1, "%s: %d audio samples, want %d", name, k, want);
    int from = DEMOD_OUT_RATE / 20;          /* 50 ms for the filters and DC trackers */
    double sig, rest;
    fit(s_out, from, k, 1000, &sig, &rest);
    double sinad = db(sig / rest), level = db(sig / (32767.0 * 32767.0 / 2));
    printf("%-26s SINAD %5.1f dB, tone %6.1f dBFS, %5.1f%% of real time", name, sinad, level, 100 * cpu / SECS);
    CHECK(sinad >= min_sinad, "%s: SINAD %.1f dB, want >= %.0f", name, sinad, min_sinad);
    CHECK(level > -20 && level < -1, "%s: tone at %.1f dBFS", name, level);
    if (other_hz > 0) {
        double o, r;
        fit(s_out, from, k, other_hz, &o, &r);
        printf(", %.0f Hz %6.1f dB under the tone", other_hz, db(sig / (o + 1e-9)));
        CHECK(db(o / sig) < max_other_db, "%s: %.0f Hz only %.1f dB under the tone", name, other_hz, db(sig / o));
    }
    printf("\n");
}

static double rms(int from, int n)
{
    double a = 0;
    for (int i = from; i < n; i++) a += (double)s_out[i] * s_out[i];
    return sqrt(a / (n - from));
}


/* ------------------------------------------------------------------ RDS station -- */

static int s_bits[16384], s_nbits;

static void put_block(uint16_t d, uint16_t ofs)
{
    uint32_t w = (uint32_t)d << 10;
    for (int i = 25; i >= 10; i--)
        if (w >> i & 1) w ^= 0x5B9u << (i - 10);
    w = (uint32_t)d << 10 | ((w & 0x3FF) ^ ofs);
    for (int i = 25; i >= 0; i--) s_bits[s_nbits++] = w >> i & 1;
}

/* groups 0A (station name) and 2A (RadioText; 2B with two letters a group if b2) over and over,
 * differentially encoded */
static void rds_bits(uint16_t pi, const char *ps, const char *rt, bool b2)
{
    enum { A = 0x0FC, B = 0x198, C = 0x168, D = 0x1B4, PTY = 10 };
    char t[64];
    memset(t, ' ', sizeof t);
    memcpy(t, rt, strlen(rt));
    int per = b2 ? 2 : 4, rsegs = ((int)strlen(rt) + per - 1) / per;
    s_nbits = 0;
    while (s_nbits + 26 * 4 * (4 + rsegs) < (int)(sizeof s_bits / sizeof *s_bits)) {
        for (int g = 0; g < 4; g++) {
            put_block(pi, A);
            put_block(PTY << 5 | g, B);
            put_block(0xE0CD, C);
            put_block((uint16_t)(ps[2 * g] << 8 | ps[2 * g + 1]), D);
        }
        for (int g = 0; g < rsegs; g++) {
            put_block(pi, A);
            if (b2) {
                put_block(2 << 12 | 1 << 11 | PTY << 5 | g, B);
                put_block(pi, 0x350);        /* C' */
                put_block((uint16_t)(t[2 * g] << 8 | t[2 * g + 1]), D);
            } else {
                put_block(2 << 12 | PTY << 5 | g, B);
                put_block((uint16_t)(t[4 * g] << 8 | t[4 * g + 1]), C);
                put_block((uint16_t)(t[4 * g + 2] << 8 | t[4 * g + 3]), D);
            }
        }
    }
    for (int i = 1; i < s_nbits; i++) s_bits[i] ^= s_bits[i - 1];
}

/* seconds of a broadcast station with RDS through the demodulator; noise per I and Q, and the
 * station's clock off from ours by ppm */
static void rds_case(const char *name, double secs, double noise, double rds_dev, double ppm, bool b2)
{
    const char *ps = "P4  SDR!", *rt = "HELLO FROM THE P4 SDR\r";
    rds_bits(0x54A8, ps, rt, b2);
    demod_set(DEMOD_WFM, 200000);
    demod_set_squelch(0);
    double ph = 0, t0 = 0.37e-3;             /* start part-way into a bit */
    for (int done = 0; done < secs * FS; done += N) {
        for (int n = 0; n < N; n++) {
            double t = t0 + (double)(done + n) / FS;
            double ts = t * (1 + ppm * 1e-6);
            int k = (int)(ts * 1187.5);
            double rds = (s_bits[k % s_nbits] ? 1 : -1) * sin(2 * M_PI * (ts * 1187.5 - k));
            double mpx = 0.5 * sin(2 * M_PI * 1000 * t) + 0.1 * sin(2 * M_PI * 19000 * ts) +
                         rds_dev / 75000 * rds * cos(2 * M_PI * 57000 * ts);
            ph += 2 * M_PI * 75000 * mpx / FS;
            s_iq[2 * n] = q8(0.3 * cos(ph) + noise * gauss());
            s_iq[2 * n + 1] = q8(0.3 * sin(ph) + noise * gauss());
        }
        run(NULL);
    }
    rds_info_t r;
    rds_get(&r);
    printf("%-26s %s, PI %04X, PS \"%s\", RT \"%s\", %u blocks, %u bad\n", name, r.sync ? "sync" : "NO SYNC", r.pi,
           r.has_ps ? r.ps : "", r.has_rt ? r.rt : "", (unsigned)r.blocks, (unsigned)r.bad_blocks);
    CHECK(r.has_pi && r.pi == 0x54A8, "%s: PI", name);
    CHECK(r.has_ps && !strcmp(r.ps, ps), "%s: PS \"%s\"", name, r.ps);
    CHECK(r.has_rt && !strcmp(r.rt, "HELLO FROM THE P4 SDR"), "%s: RadioText \"%s\"", name, r.rt);
}

int main(void)
{
    srand(1);
    if (!demod_init()) { printf("FAIL: demod_init\n"); return 1; }

    /* broadcast FM: 50 kHz deviation, pilot at 7.5 kHz deviation, a second station 400 kHz up */
    station_t wfm[2] = {{0, 0.3, 0, 1000, 50000, 7500}, {0, 0.3, 400000, 3000, 50000, 0}};
    tone_case("WFM 200k", DEMOD_WFM, 200000, wfm, 1, 0.02, 30, 19000, -40);
    tone_case("WFM + station 400k away", DEMOD_WFM, 200000, wfm, 2, 0.02, 30, 3000, -40);

    /* narrow FM: 3 kHz deviation, a neighbour one 25 kHz channel up */
    station_t nfm[2] = {{0, 0.1, 0, 1000, 3000, 0}, {0, 0.1, 25000, 2000, 3000, 0}};
    tone_case("NFM 25k", DEMOD_NFM, 25000, nfm, 1, 0.01, 25, 0, 0);
    tone_case("NFM 25k + next channel", DEMOD_NFM, 25000, nfm, 2, 0.01, 25, 2000, -30);
    station_t nfm12[1] = {{0, 0.1, 0, 1000, 1500, 0}};
    tone_case("NFM 12.5k", DEMOD_NFM, 12500, nfm12, 1, 0.01, 20, 0, 0);

    /* airband AM: 60% depth, a neighbour 8.33 kHz up */
    station_t am[2] = {{1, 0.1, 0, 1000, 0.6, 0}, {1, 0.1, 8333, 2000, 0.6, 0}};
    tone_case("AM 8k", DEMOD_AM, 8000, am, 1, 0.005, 25, 0, 0);
    tone_case("AM 8k + 8.33k neighbour", DEMOD_AM, 8000, am, 2, 0.005, 20, 2000, -25);

    /* squelch at level 3 (9 dB), per mode: a station already on the air when the mode is set
     * opens it (FM); noise alone shuts it; a station that comes up opens it again without a jump
     * in the audio (the gate fades in); noise shuts it */
    static const struct { const char *name; demod_mode_t m; uint32_t bw; station_t st; } sq[] = {
        {"WFM", DEMOD_WFM, 200000, {0, 0.1, 0, 1000, 50000, 0}},
        {"NFM 25k", DEMOD_NFM, 25000, {0, 0.03, 0, 1000, 3000, 0}},
        {"NFM 12.5k", DEMOD_NFM, 12500, {0, 0.03, 0, 1000, 1500, 0}},
        {"AM 8k", DEMOD_AM, 8000, {1, 0.03, 0, 1000, 0.5, 0}},
    };
    for (int i = 0; i < 4; i++) {
        char name[40];
        bool am = sq[i].m == DEMOD_AM;
        snprintf(name, sizeof name, "squelch %s", sq[i].name);
        demod_set(sq[i].m, sq[i].bw);
        demod_set_squelch(3);
        bool on_at_set = true;
        if (!am) {
            make(&sq[i].st, 1, 0.01);
            run(NULL);
            on_at_set = demod_open();
        }
        make(NULL, 0, 0.01);                 /* AM learns its floor here */
        run(NULL);
        make(&sq[i].st, 1, 0.01);
        int k = run(NULL);
        double loud = rms(k / 2, k);
        bool on = demod_open();
        int from = 0, jump = 0;              /* the first 0.5 ms of sound: the gate fades in, so it starts small */
        while (from < k && !s_out[from]) from++;
        for (int j = from; j < k && j < from + 24; j++) if (abs(s_out[j]) > jump) jump = abs(s_out[j]);
        float q_on = demod_quality_db();
        make(NULL, 0, 0.01);
        k = run(NULL);
        double quiet = rms(k / 2, k);
        printf("%-20s station %5.1f dB %s (rms %5.0f), noise %4.1f dB %s, first 0.5 ms peak %d\n", name, q_on,
               on ? "open" : "SHUT", loud, demod_quality_db(), demod_open() ? "OPEN" : "shut", jump);
        CHECK(on_at_set, "%s: a station on the air when the mode is set stays shut", name);
        CHECK(jump < 3000, "%s: sound starts at %d, not faded in", name, jump);
        CHECK(on && loud > 1000, "%s: shut on a station", name);
        CHECK(!demod_open() && quiet == 0, "%s: open on noise", name);
        if (!am) {                           /* a retune (memory scan hop) forgets the last channel */
            make(&sq[i].st, 1, 0.01);
            run(NULL);
            bool was = demod_open();
            demod_retuned();
            make(NULL, 0, 0.01);
            demod_process(s_iq, 4800, s_out);  /* 2 ms */
            CHECK(was && !demod_open(), "%s: squelch still open 2 ms after a retune to noise", name);
        }
    }

    rds_case("RDS, strong, 100 ppm off", 6, 0.02, 3000, 100, false);
    rds_case("RDS, weak, 30 ppm off", 8, 0.08, 2000, 30, false);
    rds_case("RDS, RadioText as 2B", 8, 0.02, 3000, 0, true);

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
