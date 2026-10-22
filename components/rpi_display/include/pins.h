#pragma once
/* Pin map for the 3.5" RPi display (tested: MHS-3.5inch Display-IPS; the lcdwiki MPI3501 has the
 * same pinout) plugged DIRECTLY onto the
 * Waveshare ESP32-P4-Module-DEV-KIT 40-pin header (P6), which follows the Raspberry Pi
 * power/ground layout (3V3 on 1/17, 5V on 2/4, GND on 6/9/14/20/25/30/34/39).
 *
 * Panel pin (RPi numbering)  -> P4 header pin -> P4 GPIO     (P4 GPIO per board silkscreen)
 *   11  TP_IRQ               -> 11            -> GPIO21
 *   18  LCD_RS (D/C)         -> 18            -> GPIO4
 *   19  LCD_SI/TP_SI (MOSI)  -> 19            -> GPIO3
 *   21  TP_SO (touch MISO)   -> 21            -> GPIO2
 *   22  RST                  -> 22            -> GPIO1
 *   23  SCK                  -> 23            -> GPIO0
 *   24  LCD_CS               -> 24            -> GPIO36  (boot-strap pin; see README)
 *   26  TP_CS                -> 26            -> GPIO32
 * VERIFY with a meter before power: header pin 1 (square pad) = 3V3, pin 2 = 5V, pin 6 = GND.
 */
#include "sdkconfig.h"
/* Set in menuconfig ("3.5in RPi display, 16-bit SPI"); defaults are the direct-plug pins above. */
#define PIN_LCD_SCK    CONFIG_RPI_LCD_PIN_SCK
#define PIN_LCD_MOSI   CONFIG_RPI_LCD_PIN_MOSI
#define PIN_LCD_MISO   CONFIG_RPI_LCD_PIN_MISO   /* only the touch controller drives this */
#define PIN_LCD_DC     CONFIG_RPI_LCD_PIN_DC
#define PIN_LCD_RST    CONFIG_RPI_LCD_PIN_RST
#define PIN_LCD_CS     CONFIG_RPI_LCD_PIN_CS
#define PIN_TP_CS      CONFIG_RPI_LCD_PIN_TP_CS
#define PIN_TP_IRQ     CONFIG_RPI_LCD_PIN_TP_IRQ

#define LCD_SPI_HOST   SPI2_HOST
/* Rated 32 MHz; the P4 delivers 80/3 = 26.67 MHz for that (verified 2026-10-17). */
#define LCD_SPI_HZ     (CONFIG_RPI_LCD_SPI_MHZ * 1000 * 1000)
#define TP_SPI_HZ      (2 * 1000 * 1000)

/* The panel is 480x320. rpi_lcd_set_portrait() turns it to 320x480, so LCD_W and LCD_H are
 * the current size; size static buffers with LCD_LONG. */
#define LCD_LONG  480
#define LCD_SHORT 320
extern int rpi_lcd_w, rpi_lcd_h;
#define LCD_W rpi_lcd_w
#define LCD_H rpi_lcd_h
#define LCD_PORTRAIT (rpi_lcd_w < rpi_lcd_h)
