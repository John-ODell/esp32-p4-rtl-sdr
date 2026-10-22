#pragma once
/* Receiver screen drawing, 480x320 landscape, in several looks (themes). No ESP-IDF calls
 * besides rpi_lcd/rpi_text, so tools/preview builds it on a PC.
 *
 * Every theme stacks the same four bands, with its own heights:
 *   header   [-1M] [-100k]  frequency  [+100k] [+1M] [gain]   (tap the frequency: next theme)
 *   spectrum 2.4 MHz wide (5 kHz per column)
 *   axis     frequency scale
 *   waterfall, newest line on top */
#include <stdbool.h>
#include <stdint.h>
#include "spectrum.h"

enum { VIEW_DN1M, VIEW_DN100K, VIEW_UP100K, VIEW_UP1M, VIEW_GAIN, VIEW_THEME, VIEW_NBTN };

void view_init(uint32_t sample_rate);        /* allocates the frame buffers, aborts if out of memory */
int view_themes(void);
const char *view_theme_name(int i);
/* Switch look: clears the screen. Redraw with view_header + view_axis after. */
void view_set_theme(int i);
int view_theme(void);
void view_reset(void);                       /* forget floor/peaks (after a reopen) */
/* pressed: the button being held, or -1. */
void view_header(uint32_t freq, const char *gain, int pressed);
void view_axis(uint32_t freq);
/* db: n bins from spectrum_compute. Updates floor, smoothing and peaks, draws off screen. */
void view_render(const float *db, int n, uint32_t freq);
/* Width shown, Hz (sample rate down to 1/8 of it: the caller raises the FFT size to match).
 * Clears the waterfall. */
void view_set_span(uint32_t span_hz);
uint32_t view_span(void);
int view_hz_per_px(void);
void view_set_passband(uint32_t hz);         /* shaded filter width around the centre */
/* plate at the top left of the spectrum: label (NULL = none), live = speaker lit */
void view_set_audio(const char *label, bool live);
/* auto: scale follows the noise floor. Otherwise ref = top of the scale, range = height, dBFS. */
void view_set_range(bool auto_range, float ref_db, float range_db);
void view_blit(void);                        /* push spectrum + waterfall to the panel */
void view_blit_spectrum(void);               /* the two halves, to poll touch in between */
void view_blit_waterfall(void);
void view_message(const char *a, const char *b);
int view_button_at(int x, int y);            /* VIEW_* under a header touch, or -1 */
int view_tune_y(void);                       /* touches at or below this row tune */
float view_floor(void);

/* Theme colours for other screens drawn in the same look (panel.c). */
typedef struct {
    uint16_t bg, btn, btn_on, edge, key_txt, txt_on, txt, dim, accent, track;   /* txt, dim: on bg */
    bool square;                             /* PortaPack: square outlined keys */
    int top;                                 /* first row below the header */
} view_colors_t;
void view_colors(view_colors_t *c);
