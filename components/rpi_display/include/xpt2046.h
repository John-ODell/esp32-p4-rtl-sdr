#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t xpt2046_init(void);                     /* shares the LCD SPI bus, own CS */
bool xpt2046_pressed(void);                        /* TP_IRQ low while touched */
/* Raw 12-bit X/Y (median of 5 samples) and pressure-ish Z. Returns false if not pressed. */
bool xpt2046_read_raw(uint16_t *x, uint16_t *y, uint16_t *z);
