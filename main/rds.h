#pragma once
/* RDS decoder for broadcast FM: the 57 kHz BPSK subcarrier in the demodulated multiplex, into
 * the programme ID (PI), the station name (PS) and RadioText. No ESP-IDF calls (demod_test).
 *
 *   MPX 480 kS/s -> x 57 kHz -> low pass /20 -> carrier phase (squared BPSK) -> biphase bit
 *   timing -> differential decoding -> 26-bit blocks found by their offset words -> groups
 *   0A/0B (PS, two letters each) and 2A/2B (RadioText, four or two). A PS or RadioText
 *   segment counts once it has come in the same twice. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool sync;                               /* locked onto the block stream */
    bool has_pi, has_ps, has_rt;
    uint16_t pi;
    char ps[9];                              /* 8 letters, space padded */
    char rt[65];                             /* up to 64 letters, trailing spaces cut */
    uint32_t blocks, bad_blocks;             /* since the last reset */
} rds_info_t;

bool rds_init(void);
void rds_reset(void);                        /* new station: forget everything */
void rds_process(const float *mpx, int n);   /* n samples at 480 kS/s, 1.0 = 75 kHz deviation */
void rds_get(rds_info_t *out);
