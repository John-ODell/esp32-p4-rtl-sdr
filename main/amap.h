#pragma once
/* Vector base map for the ADS-B screen, made on a Mac by tools/mapgen (Natural Earth, plus
 * OpenStreetMap detail close in) and read from the SD card (MAP.BIN). Coastlines, lakes, rivers, borders, roads, runways and
 * place and airport names, all in a flat km grid around the map centre, which becomes the
 * map's home point. Drawn in the current look's colours. No ESP-IDF calls.
 *
 * File (little endian):
 *   header  "AMAP" u16 version=1, u16 unit_m, i32 lat_e6, i32 lon_e6, u32 nfeat, u32 radius_km,
 *           char name[16]
 *   feature u8 kind, u8 lo, u8 hi, u8 0, u16 n, then n x (i16 x, i16 y) for lines, or
 *           (i16 x, i16 y) and n bytes of text for labels. x east, y north, in unit_m from the
 *           centre: x = dlon * 111.32 cos(lat0) km, y = dlat * 110.57 km.
 *           Shown while 5*lo <= map range (km) <= 5*hi. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rpi_canvas.h"

enum {
    AMAP_COAST = 1, AMAP_WATER, AMAP_RIVER, AMAP_BORDER, AMAP_ROAD, AMAP_MOTORWAY, AMAP_RUNWAY,
    AMAP_CITY = 16, AMAP_TOWN, AMAP_AIRPORT,
};

typedef struct {
    double lat, lon;                         /* centre */
    int radius_km, nfeat;
    char name[17];
} amap_info_t;

typedef struct {
    uint16_t coast, water, river, border, road, motorway, runway, place, airport;
} amap_colors_t;

/* Parse a MAP.BIN image. The buffer must stay alive while the map is in use. */
bool amap_load(const uint8_t *buf, size_t len);
const amap_info_t *amap_info(void);          /* NULL until a map is loaded */
/* Draw into c below row ytop, map centre at (cx, cy), px_per_km scale, range_km picks detail. */
void amap_draw(const rpi_canvas_t *c, int ytop, int cx, int cy, double px_per_km, int range_km,
               const amap_colors_t *k);
