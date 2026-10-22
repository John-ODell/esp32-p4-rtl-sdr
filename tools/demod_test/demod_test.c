/* Host test for main/demod.c: FM and AM signals with a 1 kHz tone are synthesized as 2.4 MS/s
 * u8 IQ with noise, an off-channel station and (WFM) a stereo pilot, fed through demod_process
 * in odd-sized pieces, and the audio is checked: the tone's SINAD, what is left of the
 * interferer and the pilot, and the squelch. Build and run: make -C tools/demod_test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "demod.h"

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

    /* squelch: noise alone stays shut at level 3, a carrier 20 dB up opens it */
    make(NULL, 0, 0.01);
    demod_set(DEMOD_NFM, 12500);
    demod_set_squelch(3);
    int k = run(NULL);
    double quiet = rms(k / 2, k);
    printf("%-26s floor %.1f dBFS, level %.1f, %s, rms %.0f\n", "squelch, noise only", demod_floor_db(), demod_level_db(),
           demod_open() ? "open" : "shut", quiet);
    CHECK(!demod_open() && quiet == 0, "squelch open on noise (rms %.0f)", quiet);
    station_t car[1] = {{0, 0.05, 0, 1000, 1500, 0}};
    make(car, 1, 0.01);
    k = run(NULL);
    double loud = rms(k / 2, k);
    printf("%-26s level %.1f dBFS, %s, rms %.0f\n", "squelch, carrier", demod_level_db(), demod_open() ? "open" : "shut", loud);
    CHECK(demod_open() && loud > 1000, "squelch shut on a carrier (rms %.0f)", loud);
    make(NULL, 0, 0.01);
    k = run(NULL);
    printf("%-26s %s\n", "squelch, carrier gone", demod_open() ? "open" : "shut");
    CHECK(!demod_open(), "squelch stays open after the carrier");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
