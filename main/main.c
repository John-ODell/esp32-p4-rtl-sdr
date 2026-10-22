/* p4-sdr: RTL-SDR Blog V4 receiver on the ESP32-P4-Module-DEV-KIT with the MHS-3.5inch Display-IPS.
 *
 * Boot: splash in the saved look (hold the screen to redo touch calibration), find the dongle on
 * the USB-A ports, 1.5 s USB link check in counter mode (every lost byte counts), then the
 * receiver screen (ui.c).
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
#include "xpt2046.h"
#include "touch_cal.h"
#include "pins.h"
#include "sdr.h"
#include "ui.h"
#include "view.h"

static const char *TAG = "p4sdr";
static touch_cal_t s_cal;

static int s_theme;                          /* the saved look: the splash is drawn in it */

#define SUB    "RTL-SDR Blog V4 receiver"
#define FRAMES 15                            /* the splash plays over the 1.5 s touch hold */
#define LINE(n) (view_splash_text_y() + 16 * (n))   /* status lines, then the dongle */

/* one line, or two split at a space when it is too wide (portrait) */
static void status(int tone, const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
    int n = strlen(msg);
    if (n * 8 <= LCD_W) {
        view_splash_line(LINE(0), msg, tone);
        view_splash_line(LINE(1), "", tone);
        return;
    }
    int cut = n / 2;
    while (cut > 0 && msg[cut] != ' ') cut--;
    char a[64];
    snprintf(a, sizeof a, "%.*s", cut, msg);
    view_splash_line(LINE(0), a, tone);
    view_splash_line(LINE(1), msg + cut + 1, tone);
}

static void splash_and_touch(void)
{
    view_init(SDR_SAMPLE_RATE);
    view_set_theme(s_theme);
    bool held = false;
    TickType_t t0 = xTaskGetTickCount();
    for (int i = 0; i < FRAMES && !held; i++) {
        view_splash(i, FRAMES, SUB);
        if (i == 0) view_splash_line(LCD_H - 24, "hold screen to recalibrate", SPLASH_DIM);
        while (!held && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(100 * (i + 1))) {
            held = xpt2046_pressed();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    float err = 0;
    if (touch_cal_load(&s_cal, &err) == ESP_OK && !held) {
        ESP_LOGI(TAG, "touch calibration loaded (worst %.1f px)", err);
    } else {
        ESP_LOGI(TAG, "%s: running touch calibration", held ? "screen held" : "no calibration saved");
        ESP_ERROR_CHECK(touch_cal_run(&s_cal, &err));
    }
    view_splash(0, 1, SUB);                  /* the whole screen, tuned in, no footer */
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
    if (nvs_get_i32(h, "theme", &p) == ESP_OK) s_theme = p;
    nvs_close(h);
}

static void main_task(void *arg)
{
    load_settings();                         /* first: it holds the screen orientation */
    splash_and_touch();
    status(SPLASH_TEXT, "looking for the RTL-SDR on the USB-A ports...");
    char desc[96];
    int tries = 0;
    while (!sdr_open(5000, desc, sizeof desc)) {
        if (++tries == 1) status(SPLASH_BAD, "no RTL-SDR yet: plug it in (H3 jumper on HOST)");
    }
    view_splash_line(LINE(2), desc, SPLASH_OK);
    status(SPLASH_TEXT, "USB link check...");
    uint64_t lost;
    double bps;
    bool ok = sdr_link_check(1500, &lost, &bps);
    char msg[64];
    snprintf(msg, sizeof msg, "USB link %s: %.2f MB/s, %llu bytes lost", ok ? "OK" : "POOR", bps / 1e6,
             (unsigned long long)lost);
    status(ok ? SPLASH_OK : SPLASH_BAD, msg);
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
