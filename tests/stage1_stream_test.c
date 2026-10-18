/* p4-sdr: RTL-SDR Blog V4 on the ESP32-P4-Module-DEV-KIT with the MPI3501 3.5" screen.
 *
 * Stage 1 (this file): USB stream test.
 *   1. Boot splash. Hold the screen during it to redo the touch calibration.
 *   2. Find the dongle on the Type-A ports (behind the kit's CH334F hub), open it with librtlsdr.
 *   3. Counter test: the RTL2832U's test mode sends an incrementing byte counter instead of samples,
 *      so every byte lost anywhere between the chip and our callback shows up as a jump.
 *   4. Real samples at 100 MHz: byte statistics show the tuner + ADC are alive.
 * Results go to the serial log and the screen. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "xpt2046.h"
#include "touch_cal.h"
#include "pins.h"
#include "rtl-sdr.h"
#include "rtlsdr_usb.h"

static const char *TAG = "p4sdr";

#define SAMPLE_RATE   2400000
#define TEST_FREQ_HZ  100000000
#define BUF_NUM       12
#define BUF_LEN       (32 * 1024)            /* multiple of 512; 6.8 ms of IQ at 2.4 MS/s */
#define COUNTER_SECS  20
#define SAMPLE_SECS   5

#define C_BG    RGB565(0, 0, 0)
#define C_TXT   RGB565(220, 220, 220)
#define C_DIM   RGB565(120, 120, 120)
#define C_OK    RGB565(60, 230, 90)
#define C_BAD   RGB565(255, 70, 50)
#define C_HEAD  RGB565(255, 200, 40)

static rtlsdr_dev_t *s_dev;

/* ---- stream statistics, written by the read_async callback ---- */
static volatile uint64_t s_bytes;
static volatile uint64_t s_lost;           /* counter mode: bytes missing between callbacks */
static volatile uint32_t s_gaps;           /* counter mode: number of discontinuities */
static volatile uint32_t s_cbs;
static uint8_t s_next;                     /* counter mode: expected next byte */
static bool s_counter_mode, s_synced;
static uint64_t s_hist[256];               /* sample mode: byte histogram */

static void rx_cb(unsigned char *buf, uint32_t len, void *ctx)
{
    s_cbs++;
    s_bytes += len;
    if (s_counter_mode) {
        uint8_t n = s_next;
        uint32_t i = 0;
        if (!s_synced) { n = buf[0]; s_synced = true; }
        for (; i < len; i++) {
            if (buf[i] != n) {
                s_lost += (uint8_t)(buf[i] - n);   /* at least this many, modulo 256 */
                s_gaps++;
                n = buf[i];
            }
            n++;
        }
        s_next = n;
    } else {
        for (uint32_t i = 0; i < len; i++) s_hist[buf[i]]++;
    }
}

static void stream_task(void *arg)
{
    int r = rtlsdr_read_async(s_dev, rx_cb, NULL, BUF_NUM, BUF_LEN);
    ESP_LOGI(TAG, "read_async returned %d", r);
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

/* ---- screen helpers ---- */
static int s_row;
static void line_out(uint16_t color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line_out(uint16_t color, const char *fmt, ...)
{
    char buf[61];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
    if (s_row > LCD_H - 16) return;
    int x = rpi_lcd_text(4, s_row, buf, color, C_BG, 1);
    rpi_lcd_fill_rect(x, s_row, LCD_W - x, 16, C_BG);
    s_row += 17;
}
/* Rewrite the same row (live counters). */
static void line_at(int row, uint16_t color, const char *buf)
{
    int x = rpi_lcd_text(4, row, buf, color, C_BG, 1);
    rpi_lcd_fill_rect(x, row, LCD_W - x, 16, C_BG);
}

static void boot_and_touch(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);
    ESP_ERROR_CHECK(rpi_lcd_init());
    ESP_ERROR_CHECK(xpt2046_init());

    rpi_lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
    rpi_lcd_text(LCD_W / 2 - 6 * 16, 110, "P4  SDR", C_HEAD, C_BG, 2);
    rpi_lcd_text(LCD_W / 2 - 18 * 8, 160, "RTL-SDR Blog V4 receiver", C_TXT, C_BG, 1);
    rpi_lcd_text(LCD_W / 2 - 18 * 8, 290, "hold screen to recalibrate", C_DIM, C_BG, 1);

    bool held = false;
    for (int i = 0; i < 200 && !held; i++) { held = xpt2046_pressed(); vTaskDelay(pdMS_TO_TICKS(10)); }
    touch_cal_t cal;
    float err = 0;
    if (touch_cal_load(&cal, &err) == ESP_OK && !held) {
        ESP_LOGI(TAG, "touch calibration loaded (worst %.1f px)", err);
    } else {
        ESP_LOGI(TAG, "%s: running touch calibration", held ? "screen held" : "no calibration saved");
        ESP_ERROR_CHECK(touch_cal_run(&cal, &err));
    }
    rpi_lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
}

/* Stream for `secs`, refreshing a live line once a second. Returns measured bytes/s. */
static double run_stream(int secs, int live_row, bool counter)
{
    s_bytes = s_lost = s_gaps = s_cbs = 0;
    s_counter_mode = counter;
    s_synced = false;
    memset(s_hist, 0, sizeof s_hist);
    rtlsdr_reset_buffer(s_dev);

    xTaskCreatePinnedToCore(stream_task, "rtl_rx", 6144, xTaskGetCurrentTaskHandle(), 20, NULL, 0);
    int64_t t0 = esp_timer_get_time();
    uint64_t last = 0;
    for (int s = 1; s <= secs; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint64_t b = s_bytes;
        rtlsdr_usb_stats_t us;
        rtlsdr_usb_get_stats(&us);
        char buf[61];
        if (counter)
            snprintf(buf, sizeof buf, "%2ds %5.2f MB/s lost %" PRIu64 " gaps %" PRIu32 " q%" PRIu32,
                     s, (b - last) / 1e6, s_lost, s_gaps, us.max_backlog);
        else
            snprintf(buf, sizeof buf, "%2ds %5.2f MB/s usb err %" PRIu32, s, (b - last) / 1e6, us.bulk_errors);
        ESP_LOGI(TAG, "%s", buf);
        line_at(live_row, (counter && s_lost) ? C_BAD : C_TXT, buf);
        last = b;
    }
    rtlsdr_cancel_async(s_dev);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    double dt = (esp_timer_get_time() - t0) / 1e6;
    return s_bytes / dt;
}

void app_main(void)
{
    boot_and_touch();
    s_row = 4;
    line_out(C_HEAD, "P4 SDR  stage 1: USB stream test");
    line_out(C_DIM, "free internal %u KB, PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    ESP_ERROR_CHECK(rtlsdr_usb_start());
    line_out(C_TXT, "waiting for the dongle on the USB-A ports...");
    uint32_t n = 0;
    for (int i = 0; i < 40 && n == 0; i++) {
        rtlsdr_usb_wait_new_device(500);
        n = rtlsdr_get_device_count();
    }
    if (n == 0) {
        line_out(C_BAD, "no RTL-SDR found after 20 s");
        line_out(C_TXT, "check: H3 jumper on HOST, dongle seated");
        return;
    }
    char vendor[256], product[256], serial[256];
    rtlsdr_get_device_usb_strings(0, vendor, product, serial);
    line_out(C_OK, "found: %s %s  SN %s", vendor, product, serial);

    int r = rtlsdr_open(&s_dev, 0);
    if (r < 0) { line_out(C_BAD, "rtlsdr_open failed: %d", r); return; }
    rtlsdr_usb_stats_t us;
    rtlsdr_usb_get_stats(&us);
    static const char *speeds[] = {"low", "full", "high"};
    const char *tuners[] = {"unknown", "E4000", "FC0012", "FC0013", "FC2580", "R820T", "R828D"};
    int tt = rtlsdr_get_tuner_type(s_dev);
    line_out(C_OK, "opened: tuner %s, USB %s speed", tuners[tt < 7 ? tt : 0], speeds[us.speed < 3 ? us.speed : 0]);

    rtlsdr_set_sample_rate(s_dev, SAMPLE_RATE);
    line_out(C_TXT, "sample rate %" PRIu32 " S/s = %.1f MB/s of IQ", rtlsdr_get_sample_rate(s_dev),
             rtlsdr_get_sample_rate(s_dev) * 2 / 1e6);

    /* --- counter test --- */
    line_out(C_HEAD, "counter test, %d s (every lost byte counts)", COUNTER_SECS);
    rtlsdr_set_testmode(s_dev, 1);
    int live = s_row; s_row += 17;
    double bps = run_stream(COUNTER_SECS, live, true);
    rtlsdr_set_testmode(s_dev, 0);
    rtlsdr_usb_get_stats(&us);
    double expect = rtlsdr_get_sample_rate(s_dev) * 2.0;
    bool ok = s_lost == 0 && us.bulk_errors == 0 && bps > expect * 0.99;
    line_out(ok ? C_OK : C_BAD, "%s: %.3f MB/s (%.2f%% of rate), lost %" PRIu64 " B, %" PRIu32 " gaps",
             ok ? "PASS" : "FAIL", bps / 1e6, 100.0 * bps / expect, s_lost, s_gaps);
    line_out(C_DIM, "usb: %" PRIu32 " xfers, %" PRIu32 " err, worst backlog %" PRIu32 "/%d",
             us.bulk_done, us.bulk_errors, us.max_backlog, BUF_NUM);

    /* --- real samples --- */
    rtlsdr_set_center_freq(s_dev, TEST_FREQ_HZ);
    rtlsdr_set_tuner_gain_mode(s_dev, 0);   /* auto */
    line_out(C_HEAD, "samples at %.1f MHz, auto gain, %d s", TEST_FREQ_HZ / 1e6, SAMPLE_SECS);
    live = s_row; s_row += 17;
    bps = run_stream(SAMPLE_SECS, live, false);
    uint64_t tot = 0; double mean = 0, var = 0;
    int lo = 255, hi = 0;
    for (int i = 0; i < 256; i++) {
        tot += s_hist[i];
        mean += (double)i * s_hist[i];
        if (s_hist[i]) { if (i < lo) lo = i; if (i > hi) hi = i; }
    }
    if (tot) mean /= tot;
    for (int i = 0; i < 256; i++) var += (i - mean) * (i - mean) * s_hist[i];
    if (tot) var /= tot;
    line_out(C_TXT, "%.3f MB/s, bytes mean %.1f (ideal 127.5) rms %.1f range %d..%d",
             bps / 1e6, mean, __builtin_sqrt(var), lo, hi);
    line_out(C_OK, "stage 1 done");
}
