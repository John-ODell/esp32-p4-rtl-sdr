/* p4-sdr: RTL-SDR Blog V4 receiver on the ESP32-P4-Module-DEV-KIT with the MPI3501 3.5" screen.
 *
 * Boot: splash (hold the screen to redo touch calibration), find the dongle on the USB-A ports,
 * 1.5 s USB link check in counter mode (every lost byte counts), then the receiver screen (ui.c).
 * Core 0: USB host + IQ stream. Core 1: FFT, drawing, touch. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "xpt2046.h"
#include "touch_cal.h"
#include "pins.h"
#include "sdr.h"
#include "ui.h"

static const char *TAG = "p4sdr";
static touch_cal_t s_cal;

#define C_BG   RGB565(0, 0, 0)
#define C_TXT  RGB565(220, 220, 220)
#define C_DIM  RGB565(120, 120, 120)
#define C_OK   RGB565(60, 230, 90)
#define C_BAD  RGB565(255, 70, 50)
#define C_HEAD RGB565(255, 200, 40)

/* rows of the boot screen, laid out for 320 and centred on a taller (portrait) screen */
#define ROW(y) ((y) + LCD_H / 2 - 160)

static void centred(int y, const char *s, uint16_t c)
{
    int x = LCD_W / 2 - (int)strlen(s) * 4;
    rpi_lcd_text(x < 0 ? 0 : x, y, s, c, C_BG, 1);   /* too wide: cut on the right */
}

/* one line, or two split at a space when it is too wide (portrait) */
static void status(uint16_t c, const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
    rpi_lcd_fill_rect(0, ROW(200), LCD_W, 32, C_BG);
    int n = strlen(msg);
    if (n * 8 <= LCD_W) { centred(ROW(200), msg, c); return; }
    int cut = n / 2;
    while (cut > 0 && msg[cut] != ' ') cut--;
    char a[64];
    snprintf(a, sizeof a, "%.*s", cut, msg);
    centred(ROW(200), a, c);
    centred(ROW(216), msg + cut + 1, c);
}

static void splash_and_touch(void)
{
    rpi_lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
    rpi_lcd_text(LCD_W / 2 - 7 * 16 / 2, ROW(100), "P4  SDR", C_HEAD, C_BG, 2);
    centred(ROW(150), "RTL-SDR Blog V4 receiver", C_TXT);
    centred(LCD_H - 24, "hold screen to recalibrate", C_DIM);
    bool held = false;
    for (int i = 0; i < 150 && !held; i++) { held = xpt2046_pressed(); vTaskDelay(pdMS_TO_TICKS(10)); }
    float err = 0;
    if (touch_cal_load(&s_cal, &err) == ESP_OK && !held) {
        ESP_LOGI(TAG, "touch calibration loaded (worst %.1f px)", err);
    } else {
        ESP_LOGI(TAG, "%s: running touch calibration", held ? "screen held" : "no calibration saved");
        ESP_ERROR_CHECK(touch_cal_run(&s_cal, &err));
        rpi_lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
        rpi_lcd_text(LCD_W / 2 - 7 * 16 / 2, ROW(100), "P4  SDR", C_HEAD, C_BG, 2);
    }
    rpi_lcd_fill_rect(0, LCD_H - 30, LCD_W, 30, C_BG);
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("sdr", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t f;
    int32_t g, p;
    if (nvs_get_i32(h, "port", &p) == ESP_OK && p) rpi_lcd_set_portrait(true);
    if (nvs_get_u32(h, "freq", &f) == ESP_OK) sdr_set_freq(f);
    if (nvs_get_i32(h, "gaint", &g) == ESP_OK) sdr_set_gain_tenths(g);   /* applied on open */
    nvs_close(h);
}

static void main_task(void *arg)
{
    load_settings();                         /* first: it holds the screen orientation */
    splash_and_touch();
    status(C_TXT, "looking for the RTL-SDR on the USB-A ports...");
    char desc[96];
    int tries = 0;
    while (!sdr_open(5000, desc, sizeof desc)) {
        if (++tries == 1) status(C_BAD, "no RTL-SDR yet: plug it in (H3 jumper on HOST)");
    }
    centred(ROW(LCD_PORTRAIT ? 240 : 230), desc, C_OK);
    status(C_TXT, "USB link check...");
    uint64_t lost;
    double bps;
    bool ok = sdr_link_check(1500, &lost, &bps);
    char msg[64];
    snprintf(msg, sizeof msg, "USB link %s: %.2f MB/s, %llu bytes lost", ok ? "OK" : "POOR", bps / 1e6,
             (unsigned long long)lost);
    status(ok ? C_OK : C_BAD, msg);
    vTaskDelay(pdMS_TO_TICKS(ok ? 800 : 3000));
    ESP_LOGI(TAG, "free internal %u KB, PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ui_run(&s_cal);
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);
    ESP_ERROR_CHECK(rpi_lcd_init());
    ESP_ERROR_CHECK(xpt2046_init());
    xTaskCreatePinnedToCore(main_task, "sdr_ui", 8192, NULL, 5, NULL, 1);
}
