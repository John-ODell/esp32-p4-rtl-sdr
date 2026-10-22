#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/spi_master.h"

/* ILI9486 behind the Waveshare/goodtft 16-bit serial shift-register bridge.
 * Every transfer is a whole 16-bit word: commands/params as 0x00xx with D/C low/high,
 * pixels as RGB565 big-endian with D/C high. No read-back exists on this panel. */
esp_err_t rpi_lcd_init(void);                 /* bus + reset + init sequence from the tft35a overlay */
esp_err_t rpi_lcd_fill_rect(int x, int y, int w, int h, uint16_t rgb565);
/* pix is w*h RGB565 in native (little-endian) order; it is byte-swapped into a DMA strip buffer. */
esp_err_t rpi_lcd_blit(int x, int y, int w, int h, const uint16_t *pix);
esp_err_t rpi_lcd_set_window(int x0, int y0, int x1, int y1);   /* then rpi_lcd_push_pixels_be */
esp_err_t rpi_lcd_push_pixels_be(const uint8_t *be_bytes, size_t nbytes);
spi_host_device_t rpi_lcd_spi_host(void);
/* 320x480 with the kit's USB ports up, or back to 480x320. Sets LCD_W/LCD_H (pins.h). Touch
 * calibration stays in landscape; touch_cal_map turns points to match. */
esp_err_t rpi_lcd_set_portrait(bool on);
