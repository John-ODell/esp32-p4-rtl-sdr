/* Host test for main/adsb.c: known DF17 frames (from "The 1090 MHz Riddle") are turned into
 * 2 MS/s u8 IQ with noise and a random carrier phase, fed through adsb_process in odd-sized
 * chunks, and the decoded aircraft are checked. Build and run: make -C tools/adsb_test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adsb.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void hex2bin(const char *h, uint8_t *b) { for (int i = 0; h[2 * i]; i++) sscanf(h + 2 * i, "%2hhx", &b[i]); }

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static uint8_t *iq;
static size_t niq, cap;

static void put(double i, double q)
{
    if (niq == cap) { cap = cap ? cap * 2 : 1 << 20; iq = realloc(iq, cap * 2); }
    int a = (int)lrint(127.4 + 128 * i), b = (int)lrint(127.4 + 128 * q);
    iq[2 * niq] = a < 0 ? 0 : a > 255 ? 255 : a;
    iq[2 * niq + 1] = b < 0 ? 0 : b > 255 ? 255 : b;
    niq++;
}

static double s_noise = 0.02;
static void quiet(int n) { while (n--) put(s_noise * gauss(), s_noise * gauss()); }

/* one frame: 8 us preamble + 112 us PPM, 2 samples per us; flip = data bit to corrupt or -1 */
static void frame(const char *hex, double amp, int flip)
{
    uint8_t m[14];
    hex2bin(hex, m);
    if (flip >= 0) m[flip / 8] ^= 0x80 >> (flip % 8);
    double ph = 2 * M_PI * rand() / RAND_MAX;
    int on[16] = {1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 16 + 224; k++) {
        int hi;
        if (k < 16) hi = on[k];
        else { int b = (k - 16) / 2; int bit = (m[b / 8] >> (7 - b % 8)) & 1; hi = ((k - 16) % 2 == 0) == bit; }
        double a = hi ? amp : 0;
        put(a * cos(ph) + s_noise * gauss(), a * sin(ph) + s_noise * gauss());
    }
}

static const adsb_ac_t *find(uint32_t icao)
{
    for (int i = 0; i < adsb_count(); i++) if (adsb_get(i)->icao == icao) return adsb_get(i);
    return NULL;
}

static void run(void)
{
    /* feed in uneven chunks to cross the demodulator's chunk edges */
    for (size_t off = 0; off < niq;) {
        size_t n = 1000 + rand() % 9000;
        if (off + n > niq) n = niq - off;
        adsb_process(iq + 2 * off, n);
        off += n;
    }
    niq = 0;
}

int main(void)
{
    srand(7);
    adsb_init();
    quiet(20000);
    frame("8D4840D6202CC371C32CE0576098", 0.3, -1);           /* KLM1023 identification */
    quiet(5000);
    frame("8D40621D58C386435CC412692AD6", 0.25, -1);          /* position, odd */
    quiet(5000);
    frame("8D40621D58C382D690C8AC2863A7", 0.25, -1);          /* position, even (newest) */
    quiet(5000);
    frame("8D485020994409940838175B284F", 0.2, -1);           /* velocity */
    quiet(5000);
    frame("8D4840D6202CC371C32CE0576098", 0.3, 60);           /* one bit wrong: repaired */
    quiet(5000);
    frame("8DABCDEF202CC371C32CE0576098", 0.3, -1);           /* bad CRC: rejected */
    quiet(5000);
    frame("8D4840D6202CC371C32CE0576098", 0.08, -1);          /* weak: pulse power ~9 dB over the noise */
    quiet(20000);
    run();

    adsb_stats_t st;
    adsb_get_stats(&st);
    printf("samples %llu  preambles %llu  ok %llu  fixed %llu  bad %llu\n",
           (unsigned long long)st.samples, (unsigned long long)st.preambles,
           (unsigned long long)st.msgs_ok, (unsigned long long)st.msgs_fixed, (unsigned long long)st.msgs_bad);
    for (int i = 0; i < adsb_count(); i++) {
        const adsb_ac_t *a = adsb_get(i);
        printf("%06X %-8s alt %d  spd %d trk %d vr %d  pos %d %.5f %.5f  msgs %u\n", a->icao, a->flight,
               a->alt_ft, a->speed_kt, a->track_deg, a->vrate_fpm, a->has_pos, a->lat, a->lon, a->msgs);
    }
    const adsb_ac_t *klm = find(0x4840D6), *pos = find(0x40621D), *vel = find(0x485020);
    CHECK(klm && !strcmp(klm->flight, "KLM1023"), "callsign KLM1023");
    CHECK(klm && klm->msgs == 3, "KLM: clean + repaired + weak = 3 msgs, got %u", klm ? klm->msgs : 0);
    CHECK(pos && pos->has_pos && fabs(pos->lat - 52.25720) < 1e-3 && fabs(pos->lon - 3.91937) < 1e-3,
          "position 52.2572 3.9194, got %.5f %.5f", pos ? pos->lat : 0, pos ? pos->lon : 0);
    CHECK(pos && pos->alt_ft == 38000, "altitude 38000, got %d", pos ? pos->alt_ft : 0);
    CHECK(vel && vel->speed_kt == 159 && vel->track_deg == 183 && vel->vrate_fpm == -832,
          "velocity 159 kt 183 deg -832 fpm, got %d %d %d", vel ? vel->speed_kt : 0, vel ? vel->track_deg : 0,
          vel ? vel->vrate_fpm : 0);
    CHECK(!find(0xABCDEF), "bad CRC frame must not make an aircraft");
    CHECK(st.msgs_fixed == 1, "one repaired frame, got %llu", (unsigned long long)st.msgs_fixed);

    /* pure noise for 2 s: no aircraft may appear */
    adsb_init();
    s_noise = 0.05;
    quiet(4000000);
    run();
    adsb_get_stats(&st);
    printf("noise 2 s: preambles %llu, ok %llu, aircraft %d\n", (unsigned long long)st.preambles,
           (unsigned long long)st.msgs_ok, adsb_count());
    CHECK(adsb_count() == 0, "ghost aircraft from noise");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
