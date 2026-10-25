#pragma once
/* Software AGC for the tuner (#44). The R828D's own AGC runs the gain up until a strong FM station
 * sits at the top of the 8-bit ADC, and RDS then loses half its blocks. So the GAIN page's AGC sets
 * the tuner's manual gain from the raw samples instead. It judges the loudest frame of a stretch, which
 * is steadier than one frame: down when the last second's passes -4 dBFS, aiming for -7; up when the
 * last two seconds' stays under -13, by half the way to -7; down 10 dB at once if a frame hits the
 * rails. No ESP-IDF calls, so tools/agc_test runs it on a PC. */
#include <stddef.h>
#include <stdint.h>

#define AGC_SETTLE_MS 300                    /* after a change the ring still holds samples at the old gain */
#define AGC_HOLD_DOWN 10                     /* frames (1 s) whose loudest peak decides a step down */
#define AGC_HOLD      20                     /* and a step up: longer, so jitter alone can't trigger it */

typedef struct {
    int64_t t_change;                        /* last gain change or retune, ms */
    float pk[AGC_HOLD];                      /* the last frames' peaks since then */
    int n;                                   /* how many of pk are filled */
} agc_t;

void agc_reset(agc_t *a, int64_t now_ms);    /* after a retune or a manual gain change */
/* Peak of u8 IQ (centred on 127.4, 128 to full scale, as demod.c reads it): 0..1. */
float agc_peak(const uint8_t *iq, size_t n);
/* The gain step to use next. tenths[0..n-1] are the tuner's gains, rising, in tenths of a dB;
 * cur indexes them. Returns cur when nothing should change. */
int agc_next(agc_t *a, float peak, int cur, const int *tenths, int n, int64_t now_ms);
