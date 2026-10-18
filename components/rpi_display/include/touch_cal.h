#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Affine map from raw XPT2046 readings to panel pixels:
 *   px = a*rx + b*ry + c,   py = d*rx + e*ry + f
 * Affine (not just scale+offset) so swapped or mirrored axes are handled automatically. */
typedef struct { float a, b, c, d, e, f; } touch_cal_t;

esp_err_t touch_cal_load(touch_cal_t *cal, float *max_err_px);        /* from NVS */
esp_err_t touch_cal_save(const touch_cal_t *cal, float max_err_px);
/* Interactive: shows 5 crosshairs one at a time, user taps each. Saves to NVS on success. */
esp_err_t touch_cal_run(touch_cal_t *cal, float *max_err_px);
void touch_cal_map(const touch_cal_t *cal, uint16_t rx, uint16_t ry, int *px, int *py);
