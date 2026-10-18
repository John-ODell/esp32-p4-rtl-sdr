#pragma once
#include <stdint.h>
#define NFFT_MAX 4096
void spectrum_init(void);                   /* starts at 1024 points */
/* n: power of two, 256..NFFT_MAX. Rebuilds the window and twiddles. */
void spectrum_set_size(int n);
int spectrum_size(void);
/* iq: navg blocks of n interleaved u8 I/Q. out_db: n bins, DC in the middle, dBFS. */
void spectrum_compute(const uint8_t *iq, int navg, float *out_db);
