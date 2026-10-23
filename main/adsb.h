#pragma once
/* ADS-B (Mode S extended squitter) receiver: 2 MS/s u8 IQ in, aircraft out.
 * Demodulator after dump1090's classic 2 MHz preamble test, CRC-24 with single-bit repair,
 * DF17/18 decoding: identity, airborne position, altitude, velocity.
 * Position: global CPR from an even/odd pair, then every single frame decoded locally against
 * the last fix, each fix jump-checked. That part is ported from ADS-B Scope by John Stockdale
 * (github.com/jstockdale/T-Display-P4, adsb branch, GPL-3.0); see adsb.c.
 * Pure C, no ESP-IDF calls: tools/adsb_test runs it on a PC. Not thread safe: one task. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ADSB_RATE    2000000
#define ADSB_MAX_AC  64
#define ADSB_TRAIL   16

typedef struct {
    uint32_t icao;
    char flight[9];                          /* callsign, "" until heard */
    int alt_ft;                              /* INT32_MIN until heard */
    int speed_kt, track_deg, vrate_fpm;      /* speed -1 until heard */
    bool has_pos;
    double lat, lon;
    float trail_lat[ADSB_TRAIL], trail_lon[ADSB_TRAIL];
    int ntrail;
    uint32_t msgs;
    int64_t seen_ms, pos_ms;                 /* adsb clock: samples processed / 2000 */
    /* CPR halves */
    uint32_t cpr_lat[2], cpr_lon[2];
    int64_t cpr_ms[2];
    /* position checks: the last fix that passed, and recovery after one that didn't */
    double good_lat, good_lon;
    int64_t good_ms;                         /* -1: none yet */
    bool suspect;                            /* the last fix jumped: wait for a global decode */
    int suspect_count, suspect_agree;
    double suspect_lat, suspect_lon;         /* last global candidate while suspect */
} adsb_ac_t;

typedef struct {
    uint64_t samples, preambles, msgs_ok, msgs_fixed, msgs_bad, df17;
} adsb_stats_t;

void adsb_init(void);
/* n samples (2n bytes) of interleaved u8 IQ at 2 MS/s, any chunk size, in stream order. */
void adsb_process(const uint8_t *iq, size_t n);
/* Drop aircraft not heard for timeout_ms. */
void adsb_expire(int timeout_ms);
int adsb_count(void);
const adsb_ac_t *adsb_get(int i);            /* 0..count-1, newest first is not guaranteed */
void adsb_get_stats(adsb_stats_t *s);
int64_t adsb_now_ms(void);
/* Feed one already-demodulated frame (14 or 7 bytes); for tests. Returns true if it decoded. */
bool adsb_frame(const uint8_t *msg, int bytes);
/* Put an aircraft in the table as is (screen previews). */
void adsb_test_add(const adsb_ac_t *a);
/* optional: called with each good frame (hex logging) */
extern void (*adsb_on_frame)(const uint8_t *msg, int bytes);
