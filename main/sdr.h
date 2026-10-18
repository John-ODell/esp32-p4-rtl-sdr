#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SDR_SAMPLE_RATE 2400000

/* Wait up to timeout_ms for an RTL-SDR on the USB-A ports, open it, set the sample rate. */
bool sdr_open(uint32_t timeout_ms, char *desc, size_t desc_len);
void sdr_close(void);
/* Counter-mode USB check (no streaming running): lost bytes and measured bytes/s. */
bool sdr_link_check(int ms, uint64_t *lost, double *bytes_per_s);

void sdr_start(void);            /* stream IQ into the PSRAM ring */
void sdr_stop(void);
bool sdr_streaming(void);        /* false once the stream died (dongle unplugged) */

void sdr_set_freq(uint32_t hz);
uint32_t sdr_freq(void);
/* Gain steps: index 0 = tuner AGC, then every gain the tuner offers (29 on the R828D). */
int sdr_gain_presets(void);
int sdr_gain_index(void);
void sdr_set_gain_index(int i);
const char *sdr_gain_label(int i);
int sdr_gain_tenths(int i);                 /* tenths of a dB, -1 for AGC */
/* Nearest step to tenths (-1 = AGC). Before sdr_open it is remembered and applied on open. */
void sdr_set_gain_tenths(int tenths);

/* Copy the newest n bytes of IQ (n even, <= 512 KB). Returns the stream's byte count at that point. */
uint64_t sdr_ring_latest(uint8_t *dst, size_t n);
uint64_t sdr_total_bytes(void);
/* In-order reader: copy up to max bytes from stream byte *pos on and advance it. A reader that
 * fell a ring behind jumps ahead; the bytes skipped are added to *dropped. */
size_t sdr_ring_read(uint8_t *dst, uint64_t *pos, size_t max, uint64_t *dropped);
/* Stop the stream before changing it. Kept across sdr_close/sdr_open. */
void sdr_set_sample_rate(uint32_t hz);
uint32_t sdr_sample_rate(void);
