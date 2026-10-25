/* Host test for main/agc.c: a model tuner (the R828D's 29 gain steps, a signal whose peak rises
 * dB for dB with the gain and clips at the ADC's rails) is fed to agc_next every 100 ms frame, as
 * the receiver does. Checks: a strong station settles inside the window and stays put, a quiet band
 * climbs to full gain, a station appearing at full gain is backed off at once, nothing moves while
 * a change settles or between the marks, and agc_peak reads u8 IQ the way demod.c does.
 * Build and run: make -C tools/agc_test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "agc.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const int G[] = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338,
                        364, 372, 386, 402, 421, 434, 439, 445, 480, 496};   /* R828D, tenths of a dB */
#define NG ((int)(sizeof G / sizeof G[0]))
#define FRAME_MS 100

static int index_of(int tenths)
{
    for (int i = 0; i < NG; i++) if (G[i] == tenths) return i;
    return -1;
}

/* the ADC's peak for a signal whose peak is base_db (dBFS) at 0 dB gain */
static float peak_at(float base_db, int i)
{
    float p = powf(10, (base_db + G[i] / 10.0f) / 20);
    return p > 0.995f ? 0.995f : p;
}

static float db(float p) { return 20 * log10f(p); }

typedef struct { int idx, changes, last_change_ms; float max_after_ms_db; } run_t;

/* frames from t0_ms to t1_ms with the signal at base_db; counts gain changes */
static void run(agc_t *a, run_t *r, float base_db, int t0_ms, int t1_ms)
{
    for (int t = t0_ms; t < t1_ms; t += FRAME_MS) {
        float pk = db(peak_at(base_db, r->idx));
        if (t > t0_ms && pk > r->max_after_ms_db) r->max_after_ms_db = pk;   /* the loudest frame after the first */
        int n = agc_next(a, peak_at(base_db, r->idx), r->idx, G, NG, t);
        if (n != r->idx) { r->changes++; r->last_change_ms = t; r->idx = n; }
    }
}

int main(void)
{
    agc_t a;

    /* a strong local FM station: past the rails at the old tuner AGC's gain */
    agc_reset(&a, 0);
    run_t r = {index_of(297), 0, 0, -200};
    run(&a, &r, -35, 0, 20000);
    float p = db(peak_at(-35, r.idx));
    printf("strong station: %.1f dB gain, peak %.1f dBFS, %d changes, last at %d ms\n", G[r.idx] / 10.0, p,
           r.changes, r.last_change_ms);
    CHECK(p <= -10 && p >= -19, "strong station settles between -19 and -10 dBFS (%.1f)", p);
    CHECK(r.last_change_ms < 2000, "and in under 2 s (last change at %d ms)", r.last_change_ms);
    CHECK(r.changes <= 3, "in a few steps (%d)", r.changes);
    CHECK(db(peak_at(-35, index_of(496))) > -1, "(the model clips at full gain, %.1f dBFS)", db(peak_at(-35, index_of(496))));

    /* a quiet band climbs to full gain, 10 dB at most per move, and stops there */
    agc_reset(&a, 0);
    r = (run_t){index_of(297), 0, 0, -200};
    run(&a, &r, -75, 0, 15000);
    printf("quiet band: %.1f dB gain, %d changes, last at %d ms\n", G[r.idx] / 10.0, r.changes, r.last_change_ms);
    CHECK(r.idx == NG - 1, "a quiet band ends at full gain (%.1f dB)", G[r.idx] / 10.0);
    CHECK(r.last_change_ms >= 1000, "after waiting a second below the low mark (%d ms)", r.last_change_ms);
    int before = r.changes;
    run(&a, &r, -75, 15000, 25000);
    CHECK(r.changes == before, "and stays there (%d more changes)", r.changes - before);

    /* a band that climbs into the window lands there without overshooting */
    agc_reset(&a, 0);
    run_t m = {index_of(297), 0, 0, -200};
    run(&a, &m, -58, 0, 20000);
    p = db(peak_at(-58, m.idx));
    printf("climbing band: %.1f dB gain, peak %.1f dBFS, loudest %.1f, %d changes\n", G[m.idx] / 10.0, p,
           m.max_after_ms_db, m.changes);
    CHECK(p <= -10 && p >= -19, "a band climbing from -28 dBFS ends between -19 and -10 dBFS (%.1f)", p);
    CHECK(m.max_after_ms_db <= -10.5f, "and never steps up past the high mark on the way (%.1f dBFS)", m.max_after_ms_db);

    /* a station keys up while the band is at full gain */
    run(&a, &r, -38, 25000, 25000 + 1000);
    p = db(peak_at(-38, r.idx));
    CHECK(p <= -10, "a station appearing at full gain is backed off within 1 s (%.1f dBFS at %.1f dB)", p, G[r.idx] / 10.0);
    run(&a, &r, -38, 26000, 40000);
    p = db(peak_at(-38, r.idx));
    CHECK(p <= -10 && p >= -19 && r.last_change_ms < 28000, "and settles in the window (%.1f dBFS, last change %d ms)",
          p, r.last_change_ms);

    /* the station goes away: back up to full gain */
    run(&a, &r, -75, 40000, 55000);
    CHECK(r.idx == NG - 1, "when it goes, the gain climbs back to full (%.1f dB)", G[r.idx] / 10.0);

    /* nothing moves while a change settles */
    agc_reset(&a, 0);
    int i = agc_next(&a, 0.995f, index_of(297), G, NG, AGC_SETTLE_MS - 1);
    CHECK(i == index_of(297), "no change during the settle time, even at the rails");
    i = agc_next(&a, 0.995f, index_of(297), G, NG, AGC_SETTLE_MS);
    CHECK(G[i] <= 297 - 100, "at the rails it drops at least 10 dB at once (%.1f dB)", G[i] / 10.0);
    int j = agc_next(&a, 0.995f, i, G, NG, AGC_SETTLE_MS + FRAME_MS);
    CHECK(j == i, "then waits for the samples at the new gain");

    /* between the marks nothing moves */
    agc_reset(&a, 0);
    r = (run_t){index_of(297), 0, 0, -200};
    run(&a, &r, -15 - 29.7f, 0, 20000);
    CHECK(r.changes == 0, "a peak at -15 dBFS, between the marks, changes nothing (%d)", r.changes);

    /* the ends of the range */
    agc_reset(&a, 0);
    CHECK(agc_next(&a, 0.995f, 0, G, NG, 1000) == 0, "at the lowest gain a clipped signal can only stay");
    CHECK(agc_next(&a, 0.01f, NG - 1, G, NG, 1000) == NG - 1 && agc_next(&a, 0.01f, NG - 1, G, NG, 3000) == NG - 1,
          "at full gain a weak band can only stay");

    /* agc_peak on u8 IQ */
    static uint8_t iq[48 * 1024];
    for (size_t k = 0; k < sizeof iq; k++) iq[k] = (k & 1) ? 128 : 127;
    float pk = agc_peak(iq, sizeof iq);
    CHECK(pk < 0.01f, "idle samples read as nearly nothing (%.4f)", pk);
    srand(1);
    for (size_t k = 0; k < sizeof iq; k += 2) {     /* a complex tone at amplitude 0.5, with a little noise */
        double ph = 2 * M_PI * 0.0137 * (k / 2);
        double ni = (rand() / (double)RAND_MAX - 0.5) * 0.02, nq = (rand() / (double)RAND_MAX - 0.5) * 0.02;
        iq[k] = (uint8_t)lrint(127.4 + 128 * (0.5 * cos(ph) + ni));
        iq[k + 1] = (uint8_t)lrint(127.4 + 128 * (0.5 * sin(ph) + nq));
    }
    pk = agc_peak(iq, sizeof iq);
    CHECK(fabsf(pk - 0.5f) < 0.03f, "a tone at half scale reads 0.5 (%.3f)", pk);
    iq[4000] = 0;
    pk = agc_peak(iq, sizeof iq);
    CHECK(pk > 0.98f, "a sample at the rail reads as clipped (%.3f)", pk);

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
