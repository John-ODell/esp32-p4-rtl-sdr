#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "adsb.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"                   /* keep internal RAM for the USB DMA transfers */
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#endif

void (*adsb_on_frame)(const uint8_t *msg, int bytes);

/* ------------------------------------------------------------------------- CRC-24 -- */

#define POLY 0xFFF409u
static uint32_t s_crc_tab[256];
static uint32_t s_syn112[112];               /* residual of a single flipped bit, long frames */

static uint32_t residual(const uint8_t *m, int bytes)
{
    uint32_t c = 0;
    for (int i = 0; i < bytes - 3; i++) c = ((c << 8) ^ s_crc_tab[((c >> 16) ^ m[i]) & 0xFF]) & 0xFFFFFF;
    uint32_t pi = (uint32_t)m[bytes - 3] << 16 | m[bytes - 2] << 8 | m[bytes - 1];
    return c ^ pi;
}

static void crc_init(void)
{
    for (int b = 0; b < 256; b++) {
        uint32_t c = (uint32_t)b << 16;
        for (int k = 0; k < 8; k++) c = (c & 0x800000) ? (c << 1) ^ POLY : c << 1;
        s_crc_tab[b] = c & 0xFFFFFF;
    }
    uint8_t m[14];
    for (int k = 0; k < 112; k++) {
        memset(m, 0, sizeof m);
        m[k / 8] = 0x80 >> (k % 8);
        s_syn112[k] = residual(m, 14);
    }
}

/* ---------------------------------------------------------------------- aircraft -- */

static adsb_ac_t *s_ac;                            /* [ADSB_MAX_AC], PSRAM */
static int s_nac;
static adsb_stats_t s_st;

int64_t adsb_now_ms(void) { return (int64_t)(s_st.samples / (ADSB_RATE / 1000)); }
int adsb_count(void) { return s_nac; }
const adsb_ac_t *adsb_get(int i) { return (i >= 0 && i < s_nac) ? &s_ac[i] : NULL; }
void adsb_get_stats(adsb_stats_t *s) { *s = s_st; }

static adsb_ac_t *aircraft(uint32_t icao)
{
    for (int i = 0; i < s_nac; i++) if (s_ac[i].icao == icao) return &s_ac[i];
    int slot = s_nac;
    if (s_nac == ADSB_MAX_AC) {               /* full: reuse the stalest */
        slot = 0;
        for (int i = 1; i < s_nac; i++) if (s_ac[i].seen_ms < s_ac[slot].seen_ms) slot = i;
    } else {
        s_nac++;
    }
    adsb_ac_t *a = &s_ac[slot];
    memset(a, 0, sizeof *a);
    a->icao = icao;
    a->alt_ft = INT32_MIN;
    a->speed_kt = -1;
    a->cpr_ms[0] = a->cpr_ms[1] = -100000;
    return a;
}

void adsb_test_add(const adsb_ac_t *a)
{
    adsb_ac_t *d = aircraft(a->icao);
    *d = *a;
}

void adsb_expire(int timeout_ms)
{
    int64_t now = adsb_now_ms();
    for (int i = 0; i < s_nac;)
        if (now - s_ac[i].seen_ms > timeout_ms) s_ac[i] = s_ac[--s_nac];
        else i++;
}

/* --------------------------------------------------------------------- decoding -- */

/* bits first..last, 1-based over the whole frame */
static uint32_t bits(const uint8_t *m, int first, int last)
{
    uint32_t v = 0;
    for (int b = first - 1; b < last; b++) v = v << 1 | ((m[b / 8] >> (7 - b % 8)) & 1);
    return v;
}

static int cpr_nl(double lat)
{
    lat = fabs(lat);
    if (lat < 1e-9) return 59;
    if (lat > 87) return 1;
    if (lat == 87) return 2;
    double a = 1 - cos(M_PI / 30);                 /* 1 - cos(pi / (2 * NZ)), NZ = 15 */
    double c = cos(M_PI / 180 * lat);
    return (int)floor(2 * M_PI / acos(1 - a / (c * c)));
}

static double modd(double a, double b) { double r = fmod(a, b); return r < 0 ? r + b : r; }

/* global airborne CPR from an even/odd pair; newest says which half to use */
static bool cpr_global(adsb_ac_t *a, int newest)
{
    const double le = a->cpr_lat[0] / 131072.0, lo_e = a->cpr_lon[0] / 131072.0;
    const double lo = a->cpr_lat[1] / 131072.0, lo_o = a->cpr_lon[1] / 131072.0;
    double j = floor(59 * le - 60 * lo + 0.5);
    double rlat_e = 360.0 / 60 * (modd(j, 60) + le);
    double rlat_o = 360.0 / 59 * (modd(j, 59) + lo);
    if (rlat_e >= 270) rlat_e -= 360;
    if (rlat_o >= 270) rlat_o -= 360;
    if (cpr_nl(rlat_e) != cpr_nl(rlat_o)) return false;      /* straddles a zone edge */
    double lat = newest ? rlat_o : rlat_e;
    int nl = cpr_nl(lat);
    int ni = newest ? (nl - 1 > 1 ? nl - 1 : 1) : (nl > 1 ? nl : 1);
    double m = floor(lo_e * (nl - 1) - lo_o * nl + 0.5);
    double lon = 360.0 / ni * (modd(m, ni) + (newest ? lo_o : lo_e));
    if (lon >= 180) lon -= 360;
    a->lat = lat;
    a->lon = lon;
    return true;
}

static void decode_df17(const uint8_t *m)
{
    uint32_t icao = bits(m, 9, 32);
    adsb_ac_t *a = aircraft(icao);
    int64_t now = adsb_now_ms();
    a->seen_ms = now;
    a->msgs++;
    int tc = bits(m, 33, 37);
    if (tc >= 1 && tc <= 4) {
        static const char cs[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
        char f[9];
        for (int i = 0; i < 8; i++) f[i] = cs[bits(m, 41 + 6 * i, 46 + 6 * i)];
        int n = 8;
        while (n > 0 && (f[n - 1] == ' ' || f[n - 1] == '#')) n--;
        f[n] = 0;
        memcpy(a->flight, f, sizeof f);
    } else if (tc >= 9 && tc <= 18) {
        uint32_t ac = bits(m, 41, 52);
        if (ac & 0x10) a->alt_ft = (int)(((ac & 0xFE0) >> 1) | (ac & 0x0F)) * 25 - 1000;
        int f = bits(m, 54, 54);
        a->cpr_lat[f] = bits(m, 55, 71);
        a->cpr_lon[f] = bits(m, 72, 88);
        a->cpr_ms[f] = now;
        if (a->cpr_ms[!f] >= 0 && now - a->cpr_ms[!f] < 10000 && cpr_global(a, f)) {
            a->has_pos = true;
            a->pos_ms = now;
            if (a->ntrail == 0 || fabsf(a->trail_lat[(a->ntrail - 1) % ADSB_TRAIL] - (float)a->lat) > 0.002f ||
                fabsf(a->trail_lon[(a->ntrail - 1) % ADSB_TRAIL] - (float)a->lon) > 0.002f) {
                a->trail_lat[a->ntrail % ADSB_TRAIL] = (float)a->lat;
                a->trail_lon[a->ntrail % ADSB_TRAIL] = (float)a->lon;
                a->ntrail++;
            }
        }
    } else if (tc == 19) {
        int st = bits(m, 38, 40);
        if (st == 1 || st == 2) {
            int vew = bits(m, 47, 56), vns = bits(m, 58, 67);
            if (vew && vns) {
                int k = st == 2 ? 4 : 1;
                double vx = (vew - 1) * k * (bits(m, 46, 46) ? -1 : 1);
                double vy = (vns - 1) * k * (bits(m, 57, 57) ? -1 : 1);
                a->speed_kt = (int)lrint(sqrt(vx * vx + vy * vy));
                a->track_deg = (int)lrint(modd(atan2(vx, vy) * 180 / M_PI, 360));
            }
            int vr = bits(m, 70, 78);
            if (vr) a->vrate_fpm = (vr - 1) * 64 * (bits(m, 69, 69) ? -1 : 1);
        }
    }
}

bool adsb_frame(const uint8_t *msg, int bytes)
{
    int df = msg[0] >> 3;
    if (bytes != 14 || (df != 17 && df != 18)) {
        if (df == 11 && bytes == 7 && (residual(msg, 7) & ~0x7Fu) == 0) { s_st.msgs_ok++; return true; }
        return false;
    }
    uint8_t m[14];
    memcpy(m, msg, 14);
    uint32_t r = residual(m, 14);
    if (r) {
        int k = 5;                                 /* never "repair" the DF field */
        while (k < 112 && s_syn112[k] != r) k++;
        if (k == 112) { s_st.msgs_bad++; return false; }
        m[k / 8] ^= 0x80 >> (k % 8);
        /* a repaired frame only counts for an aircraft already heard cleanly (no ghosts) */
        uint32_t icao = bits(m, 9, 32);
        bool known = false;
        for (int i = 0; i < s_nac && !known; i++) known = s_ac[i].icao == icao;
        if (!known) { s_st.msgs_bad++; return false; }
        s_st.msgs_fixed++;
    }
    s_st.msgs_ok++;
    s_st.df17++;
    if (adsb_on_frame) adsb_on_frame(m, 14);
    decode_df17(m);
    return true;
}

/* ------------------------------------------------------------------- demodulator -- */

#define CHUNK 8192                                 /* samples per pass */
#define TAIL  256                                  /* > preamble 16 + 112 bits x 2 */
static uint16_t *s_m;                              /* CHUNK + TAIL power values, |I|^2 + |Q|^2 */
static uint16_t s_sq[256];
static int s_fill, s_skip;

void adsb_init(void)
{
    crc_init();
    for (int i = 0; i < 256; i++) { int c = i - 127; s_sq[i] = (uint16_t)(c * c); }   /* 0..16384 */
    memset(&s_st, 0, sizeof s_st);
    s_nac = 0;
    if (!s_ac) s_ac = BIG_ALLOC(ADSB_MAX_AC * sizeof s_ac[0]);
    if (!s_m) s_m = BIG_ALLOC((CHUNK + TAIL) * sizeof s_m[0]);
    if (!s_m || !s_ac) abort();
    s_fill = TAIL;                                 /* start as if a quiet tail preceded */
    memset(s_m, 0, (CHUNK + TAIL) * sizeof s_m[0]);
    s_skip = 0;
}

static void demod(const uint16_t *m, int n)
{
    int j = s_skip;
    for (; j < n; j++) {
        const uint16_t *p = m + j;
        if (!(p[0] > p[1] && p[1] < p[2] && p[2] > p[3] && p[3] < p[0] && p[4] < p[0] && p[5] < p[0] &&
              p[6] < p[0] && p[7] > p[8] && p[8] < p[9] && p[9] > p[6]))
            continue;
        /* the gaps between the four pulses, and the quiet before the data, must sit low */
        uint32_t high = ((uint32_t)p[0] + p[2] + p[7] + p[9]) / 9;   /* ~ (2/3)^2 of the mean pulse power */
        if (p[4] >= high || p[5] >= high) continue;
        if (p[11] >= high || p[12] >= high || p[13] >= high || p[14] >= high) continue;
        s_st.preambles++;
        uint8_t msg[14] = {0};
        int bit = 0;
        for (int i = 0; i < 112; i++) {
            uint16_t a = p[16 + 2 * i], b = p[17 + 2 * i];
            bit = a > b ? 1 : a < b ? 0 : bit;
            if (bit) msg[i / 8] |= 0x80 >> (i % 8);
        }
        int df = msg[0] >> 3, bytes = df >= 16 ? 14 : 7;
        if (adsb_frame(msg, bytes)) j += 16 + bytes * 16 - 1;
    }
    s_skip = j - n;
}

void adsb_process(const uint8_t *iq, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_m[s_fill++] = s_sq[iq[2 * i]] + s_sq[iq[2 * i + 1]];
        if (s_fill == CHUNK + TAIL) {
            demod(s_m, CHUNK);
            memmove(s_m, s_m + CHUNK, TAIL * sizeof s_m[0]);
            s_fill = TAIL;
            s_st.samples += CHUNK;
        }
    }
}
