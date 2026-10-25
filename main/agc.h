#pragma once
/* Software AGC for the tuner (#44). The R828D's own AGC runs the gain up until a strong FM station
 * sits at the top of the 8-bit ADC, and RDS then loses half its blocks. So the GAIN page's AGC sets
 * the tuner's manual gain from the raw samples instead: down at once when the peak passes -10.5 dBFS,
 * aiming for -13; up after a second below -19 dBFS, never past -13. No ESP-IDF calls, so
 * tools/agc_test runs it on a PC. */
#include <stddef.h>
#include <stdint.h>

#define AGC_SETTLE_MS 300                    /* after a change the ring still holds samples at the old gain */
#define AGC_RISE_MS   1000                   /* this long below the low mark before stepping up */

typedef struct {
    int64_t t_change;                        /* last gain change or retune, ms */
    int64_t t_low;                           /* since when the peak has stayed low, ms; -1 = it hasn't */
} agc_t;

void agc_reset(agc_t *a, int64_t now_ms);    /* after a retune or a manual gain change */
/* Peak of u8 IQ (centred on 127.4, 128 to full scale, as demod.c reads it): 0..1. */
float agc_peak(const uint8_t *iq, size_t n);
/* The gain step to use next. tenths[0..n-1] are the tuner's gains, rising, in tenths of a dB;
 * cur indexes them. Returns cur when nothing should change. */
int agc_next(agc_t *a, float peak, int cur, const int *tenths, int n, int64_t now_ms);
