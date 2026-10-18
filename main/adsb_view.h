#pragma once
/* ADS-B screens in the current look: aircraft list, and a map around a home point with
 * range rings, heading arrows, callsigns and trails. Big keys that light under the finger
 * and fire on lift (resistive panel). No ESP-IDF calls (tools/preview builds it).
 *
 *   y 0..47   ADS-B  stats      [-] [+] (map only)  [MAP|LIST] [EXIT]
 *   y 48..319 list: six 44 px rows, newest first  |  map */
#include <stdbool.h>
#include <stdint.h>
#include "adsb.h"

enum { AV_NONE, AV_EXIT, AV_TOGGLE, AV_ZOOM_IN, AV_ZOOM_OUT };
enum { AV_PRESS, AV_MOVE, AV_RELEASE };

typedef struct {
    double msgs_per_s;
    int gain_tenths;                         /* -1 = AGC */
    bool have_home;
    double home_lat, home_lon;               /* map centre */
} adsb_view_info_t;

void adsb_view_init(void);
void adsb_view_open(void);                   /* list page, full redraw */
bool adsb_view_is_map(void);
void adsb_view_draw(const adsb_view_info_t *info);
int adsb_view_touch(int ev, int x, int y);   /* AV_* fired on release */
int adsb_view_range_km(void);
