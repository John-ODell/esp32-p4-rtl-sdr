#pragma once
/* Speaker audio: an in-order reader of the IQ ring, the demodulator (demod.c) and the kit's
 * ES8311 codec with its NS4150B amplifier. Runs in its own task on core 0, next to the USB
 * stream; the calls below only leave requests for it. */
#include <stdbool.h>
#include <stdint.h>
#include "demod.h"

bool audio_init(void);                       /* false (and logged) when the codec doesn't answer */
void audio_set_mode(demod_mode_t mode, uint32_t bw_hz);
void audio_set_volume(int v);                /* 0 = off, 1..10 */
void audio_set_squelch(int level);           /* 0 = open, 1..9 */
void audio_retuned(void);                    /* new station: RDS starts over */
void audio_pause(bool on);                   /* other modes own the stream: silence, no reading */
bool audio_ok(void);
bool audio_squelch_open(void);
/* the last 5 s: demodulator time as % of real time, and the most IQ the reader was behind (ms) */
void audio_stats(int *load_pct, int *lag_ms);
