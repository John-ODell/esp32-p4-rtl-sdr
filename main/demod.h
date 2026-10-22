#pragma once
/* Audio demodulator for the tuned frequency (the centre of the IQ stream): 2.4 MS/s IQ in,
 * 48 kHz mono audio out. No ESP-IDF calls, so tools/demod_test runs it on a PC.
 *
 *   2.4 MS/s --FIR /5--> 480 kS/s --+-- WFM: discriminator, /5, 75 us de-emphasis, /2 (15 kHz)
 *                                   +-- NFM, AM: /5, channel filter /2, discriminator or envelope
 *
 * Squelch compares the channel power with a floor that follows the noise: 0 = always open,
 * level n opens 3n dB above the floor. */
#include <stdbool.h>
#include <stdint.h>

#define DEMOD_IN_RATE  2400000
#define DEMOD_OUT_RATE 48000

typedef enum { DEMOD_WFM, DEMOD_NFM, DEMOD_AM } demod_mode_t;

bool demod_init(void);                       /* allocates the filters, false if out of memory */
/* Mode and channel width (Hz). Resets the filters and the squelch floor. */
void demod_set(demod_mode_t mode, uint32_t bw_hz);
demod_mode_t demod_mode(void);
const char *demod_mode_name(demod_mode_t m);
void demod_set_squelch(int level);           /* 0..9 */
void demod_reset_floor(void);                /* after a retune */

/* n IQ pairs of unsigned 8-bit samples. Writes up to n / 50 + 1 samples to out, returns how
 * many. Squelched audio comes out as silence. */
int demod_process(const uint8_t *iq, int n, int16_t *out);

float demod_level_db(void);                  /* channel power, dBFS */
float demod_floor_db(void);
bool demod_open(void);                       /* squelch open */
