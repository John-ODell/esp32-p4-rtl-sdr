#pragma once
/* Controls, drawn over the spectrum and waterfall in the current look. Built for a resistive
 * panel that is only good to a few pixels: big keys that fill the screen, a key lights up
 * under the finger and fires when the finger lifts (slide to the right one first), and the
 * -/+ steppers fire on press and repeat while held.
 *
 *   menu:   GAIN  RANGE  ZOOM  AUDIO / BAND  SCAN  LOOK  DONE   (tiles show the current value)
 *   GAIN:   -  value  +,  AGC / 15 dB / 30 dB / MAX
 *   RANGE:  top -/+, span -/+, AUTO
 *   ZOOM: four choices;  AUDIO: four filters (each with its mode), volume and squelch -/+
 *   BAND: eight presets;  SCAN: WIFI, BLE, SAVE, MEMORY;  LOOK: the looks, TURN
 *   MEMORY: eight channel slots, tap one to tune it, SCAN to hop through them;  SAVE: tap a slot
 *   to store what's tuned now there
 * In portrait the 4-wide grids stand 2 wide.
 * No ESP-IDF calls (tools/preview builds it). */
#include <stdbool.h>
#include <stdint.h>

#define PANEL_ZOOMS 4
#define PANEL_BWS   4
#define PANEL_BANDS 8
#define PANEL_MEMS  8

typedef struct {
    int gain_idx, gain_n;                    /* 0 = AGC */
    const char *(*gain_label)(int i);
    int (*gain_tenths)(int i);               /* tenths of a dB for a step, -1 for AGC */
    bool range_auto;
    int ref_db, range_db;                    /* manual scale: top (dBFS) and height (dB) */
    int zoom, bw, band;                      /* indexes into the tables below; band -1 = none */
    int vol, sql;                            /* volume 0 (off)..10, squelch 0 (open)..9 */
    int look;                                /* set with CTL_LOOK: the look picked */
    uint32_t freq;                           /* tuned now (SAVE stores it) */
    uint32_t mem_hz[PANEL_MEMS];             /* channel memories, 0 = empty */
    int8_t mem_bw[PANEL_MEMS];               /* their filter index */
    int mem;                                 /* with CTL_MEM_*: the slot */
} ctl_t;

enum { CTL_GAIN = 1, CTL_RANGE = 2, CTL_ZOOM = 4, CTL_BW = 8, CTL_BAND = 16, CTL_CLOSE = 32,
       CTL_WIFI = 64, CTL_BLE = 128,         /* WIFI, BLE: open that scanner (the panel stays as is) */
       CTL_LOOK = 256,                       /* switch to c->look, then panel_redraw() */
       CTL_TURN = 512,                       /* switch between landscape and portrait */
       CTL_AUDIO = 1024,                     /* volume or squelch */
       CTL_MEM_SAVE = 2048,                  /* c->freq and c->bw stored in slot c->mem */
       CTL_MEM_TUNE = 4096,                  /* tune slot c->mem (the panel stays as is) */
       CTL_MEM_SCAN = 8192 };                /* scan the memories */
enum { PANEL_PRESS, PANEL_MOVE, PANEL_RELEASE };

void panel_init(void);
void panel_open(const ctl_t *c);             /* menu page, whole panel drawn */
void panel_redraw(const ctl_t *c);           /* same page again (after a look change) */
void panel_open_memory(const ctl_t *c, bool save);   /* straight to MEMORY (or SAVE) */
/* Touch event at (x, y), screen coordinates, now_us for key repeat. Changes c, redraws what
 * changed and returns CTL_* bits. */
int panel_touch(ctl_t *c, int ev, int x, int y, int64_t now_us);

uint32_t panel_zoom_hz(int i);
uint32_t panel_bw_hz(int i);
const char *panel_bw_mode(int i);           /* "WFM", "NFM" or "AM" */
const char *panel_band(int i, uint32_t *freq, int *bw, int *zoom);
