#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "touch_cal.h"
#include "xpt2046.h"
#include "rpi_lcd.h"
#include "rpi_text.h"
#include "pins.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_cal";
#define RGB(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define CAL_MAGIC 0x54434131u                     /* "TCA1" */
#define NPTS 5
#define MAX_SAMPLES 64
#define ACCEPT_ERR_PX 12.0f                       /* worst-point residual we accept */

typedef struct { uint32_t magic; touch_cal_t cal; float err; } cal_blob_t;

esp_err_t touch_cal_load(touch_cal_t *cal, float *err) {
    nvs_handle_t h; cal_blob_t b; size_t n = sizeof b;
    esp_err_t r = nvs_open("touch", NVS_READONLY, &h);
    if (r != ESP_OK) return r;
    r = nvs_get_blob(h, "cal", &b, &n);
    nvs_close(h);
    if (r != ESP_OK) return r;
    if (n != sizeof b || b.magic != CAL_MAGIC) return ESP_ERR_INVALID_VERSION;
    *cal = b.cal; if (err) *err = b.err;
    return ESP_OK;
}

esp_err_t touch_cal_save(const touch_cal_t *cal, float err) {
    nvs_handle_t h; cal_blob_t b = {.magic = CAL_MAGIC, .cal = *cal, .err = err};
    esp_err_t r = nvs_open("touch", NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_blob(h, "cal", &b, sizeof b);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

/* The saved calibration maps to landscape pixels. Portrait (rpi_lcd_set_portrait) is the kit
 * turned so its USB ports face up: a quarter turn anticlockwise from the default landscape,
 * clockwise from the 180-degree one. */
void touch_cal_map(const touch_cal_t *c, uint16_t rx, uint16_t ry, int *px, int *py) {
    int x = (int)lroundf(c->a * rx + c->b * ry + c->c);
    int y = (int)lroundf(c->d * rx + c->e * ry + c->f);
    x = x < 0 ? 0 : x > LCD_LONG - 1 ? LCD_LONG - 1 : x;
    y = y < 0 ? 0 : y > LCD_SHORT - 1 ? LCD_SHORT - 1 : y;
    if (LCD_PORTRAIT) {
#if CONFIG_RPI_LCD_ROTATE_180
        int u = LCD_SHORT - 1 - y, v = x;
#else
        int u = y, v = LCD_LONG - 1 - x;
#endif
        x = u; y = v;
    }
    *px = x; *py = y;
}

/* a fit made in portrait, turned back to landscape for saving (the inverse of the above) */
static touch_cal_t to_landscape(touch_cal_t p) {
#if CONFIG_RPI_LCD_ROTATE_180
    touch_cal_t l = {p.d, p.e, p.f, -p.a, -p.b, LCD_SHORT - 1 - p.c};
#else
    touch_cal_t l = {-p.d, -p.e, LCD_LONG - 1 - p.f, p.a, p.b, p.c};
#endif
    return l;
}

static void crosshair(int x, int y, uint16_t col) {
    rpi_lcd_fill_rect(x - 12, y, 25, 1, col);
    rpi_lcd_fill_rect(x, y - 12, 1, 25, col);
    rpi_lcd_fill_rect(x - 2, y - 2, 5, 5, col);
}

/* Centred instruction lines, kept clear of the five targets. */
static void say(int y, const char *msg, uint16_t fg) {
    y += LCD_H / 2 - 160;                         /* laid out for 320 rows; centred on taller */
    rpi_lcd_fill_rect(0, y, LCD_W, 16, RGB(0, 0, 0));
    rpi_lcd_text(LCD_W / 2 - (int)strlen(msg) * 4, y, msg, fg, RGB(0, 0, 0), 1);
}

static int cmpu16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

/* Wait for one deliberate tap; return the median raw X/Y over the steady part of the press. */
static void wait_tap(uint16_t *mx, uint16_t *my) {
    static uint16_t xs[MAX_SAMPLES], ys[MAX_SAMPLES];
    for (;;) {
        while (xpt2046_pressed()) vTaskDelay(pdMS_TO_TICKS(10));       /* finger up first */
        vTaskDelay(pdMS_TO_TICKS(50));
        while (!xpt2046_pressed()) vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(40));                                 /* skip touch-down bounce */
        int n = 0; uint16_t x, y, z;
        int64_t t_end = esp_timer_get_time() + 400000;                 /* sample up to 0.4 s */
        while (n < MAX_SAMPLES && esp_timer_get_time() < t_end) {
            if (!xpt2046_read_raw(&x, &y, &z)) break;
            if (x > 50 && x < 4045 && y > 50 && y < 4045) { xs[n] = x; ys[n] = y; n++; }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (n < 8) { ESP_LOGW(TAG, "tap too short (%d samples), tap again", n); continue; }
        qsort(xs, n, sizeof xs[0], cmpu16); qsort(ys, n, sizeof ys[0], cmpu16);
        *mx = xs[n / 2]; *my = ys[n / 2];
        while (xpt2046_pressed()) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
}

static double det3(const double m[3][3]) {
    return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1]) - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])
         + m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
}
/* Least-squares fit of out = p0*rx + p1*ry + p2 (Cramer's rule on the normal equations). */
static bool fit(const double *rx, const double *ry, const double *out, int n, float p[3]) {
    double A[3][3] = {{0}}, B[3] = {0};
    for (int i = 0; i < n; i++) {
        double v[3] = {rx[i], ry[i], 1.0};
        for (int r = 0; r < 3; r++) { B[r] += v[r] * out[i]; for (int c = 0; c < 3; c++) A[r][c] += v[r] * v[c]; }
    }
    double D = det3(A);
    if (fabs(D) < 1e-6) return false;
    for (int k = 0; k < 3; k++) {
        double M[3][3]; memcpy(M, A, sizeof M);
        for (int r = 0; r < 3; r++) M[r][k] = B[r];
        p[k] = (float)(det3(M) / D);
    }
    return true;
}

esp_err_t touch_cal_run(touch_cal_t *cal, float *max_err) {
    const int tx[NPTS] = {30, LCD_W - 31, LCD_W - 31, 30, LCD_W / 2};
    const int ty[NPTS] = {30, 30, LCD_H - 31, LCD_H - 31, LCD_H / 2};
    for (int attempt = 1; ; attempt++) {
        double rx[NPTS], ry[NPTS], ox[NPTS], oy[NPTS];
        rpi_lcd_fill_rect(0, 0, LCD_W, LCD_H, RGB(0, 0, 0));
        ESP_LOGI(TAG, "calibration attempt %d: tap the centre of each white crosshair", attempt);
        say(88, "TOUCH CALIBRATION", RGB(255, 200, 40));
        say(108, attempt == 1 ? "Tap the centre of each white cross." : "One tap was off. Once more, carefully.",
            attempt == 1 ? RGB(220, 220, 220) : RGB(255, 90, 70));
        say(200, "A stylus or fingernail works best.", RGB(120, 120, 120));
        for (int i = 0; i < NPTS; i++) {
            char m[24];
            snprintf(m, sizeof m, "point %d of %d", i + 1, NPTS);
            say(220, m, RGB(120, 120, 120));
            crosshair(tx[i], ty[i], RGB(255, 255, 255));
            uint16_t x, y; wait_tap(&x, &y);
            crosshair(tx[i], ty[i], RGB(0, 200, 0));                   /* green = captured */
            rx[i] = x; ry[i] = y; ox[i] = tx[i]; oy[i] = ty[i];
            ESP_LOGI(TAG, "point %d target (%d,%d) raw (%u,%u)", i, tx[i], ty[i], x, y);
        }
        float px[3], py[3];
        if (!fit(rx, ry, ox, NPTS, px) || !fit(rx, ry, oy, NPTS, py)) {
            ESP_LOGW(TAG, "degenerate fit (all taps in one spot?), retrying");
            continue;
        }
        touch_cal_t c = {px[0], px[1], px[2], py[0], py[1], py[2]};
        float worst = 0;
        for (int i = 0; i < NPTS; i++) {
            float ex = c.a * rx[i] + c.b * ry[i] + c.c - ox[i];
            float ey = c.d * rx[i] + c.e * ry[i] + c.f - oy[i];
            float e = sqrtf(ex * ex + ey * ey);
            ESP_LOGI(TAG, "point %d residual %.1f px", i, e);
            if (e > worst) worst = e;
        }
        ESP_LOGI(TAG, "fit: px=%.5f*rx%+.5f*ry%+.1f  py=%.5f*rx%+.5f*ry%+.1f  worst %.1f px",
                 c.a, c.b, c.c, c.d, c.e, c.f, worst);
        if (worst > ACCEPT_ERR_PX && attempt < 3) {
            ESP_LOGW(TAG, "worst residual %.1f px > %.0f, a tap was off; redoing", worst, ACCEPT_ERR_PX);
            rpi_lcd_fill_rect(0, 0, LCD_W, LCD_H, RGB(160, 0, 0));    /* red flash = redo */
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (LCD_PORTRAIT) c = to_landscape(c);
        *cal = c; if (max_err) *max_err = worst;
        esp_err_t r = touch_cal_save(&c, worst);
        char m[40];
        snprintf(m, sizeof m, "Done. Worst point off by %.1f px.", worst);
        say(200, m, RGB(60, 230, 90));
        say(220, r == ESP_OK ? "Saved to flash." : "Could not save to flash!", RGB(120, 120, 120));
        vTaskDelay(pdMS_TO_TICKS(1200));
        ESP_LOGI(TAG, "calibration %s to NVS", r == ESP_OK ? "saved" : "NOT saved");
        return r;
    }
}
