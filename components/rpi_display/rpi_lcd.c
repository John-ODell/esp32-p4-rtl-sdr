#include <string.h>
#include "rpi_lcd.h"
#include "pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rpi_lcd";
static spi_device_handle_t s_dev;
#define STRIP_ROWS 16                                 /* 480*16*2 = 15360 B per DMA transfer */
#define STRIP_BYTES (LCD_LONG * STRIP_ROWS * 2)
static uint8_t *s_strip;                              /* DMA-capable internal RAM */

static void IRAM_ATTR pre_cb(spi_transaction_t *t) {
    gpio_set_level(PIN_LCD_DC, (int)(uintptr_t)t->user);
}

static esp_err_t tx(const uint8_t *buf, size_t nbytes, int dc) {
    spi_transaction_t t = {0};
    t.length = nbytes * 8;
    t.tx_buffer = buf;
    t.user = (void *)(uintptr_t)dc;
    return spi_device_polling_transmit(s_dev, &t);
}
static esp_err_t cmd(uint8_t c)   { uint8_t b[2] = {0x00, c}; return tx(b, 2, 0); }
static esp_err_t data8(uint8_t d) { uint8_t b[2] = {0x00, d}; return tx(b, 2, 1); }

/* Decoded from goodtft usr/tft35a-overlay.dtb "init" property (fbtft encoding:
 * 0x1000000|cmd = command, plain = parameter, 0x2000000|ms = delay). */
typedef struct { uint8_t cmd; uint8_t n; const uint8_t *p; uint16_t delay_ms; } init_step_t;
static const uint8_t pF1[] = {0x36,0x04,0x00,0x3c,0x0f,0x8f};
static const uint8_t pF2[] = {0x18,0xa3,0x12,0x02,0xb2,0x12,0xff,0x10,0x00};
static const uint8_t pF8[] = {0x21,0x04};
static const uint8_t pF9[] = {0x00,0x08};
static const uint8_t p36a[] = {0x08};
static const uint8_t pB4[] = {0x00};
static const uint8_t pC1[] = {0x41};
static const uint8_t pC5[] = {0x00,0x91,0x80,0x00};
static const uint8_t pE0[] = {0x0f,0x1f,0x1c,0x0c,0x0f,0x08,0x48,0x98,0x37,0x0a,0x13,0x04,0x11,0x0d,0x00};
static const uint8_t pE1[] = {0x0f,0x32,0x2e,0x0b,0x0d,0x05,0x47,0x75,0x37,0x06,0x10,0x03,0x24,0x20,0x00};
static const uint8_t p3A[] = {0x55};                 /* RGB565 */
#if CONFIG_RPI_LCD_ROTATE_180
static const uint8_t p36b[] = {0xE8};                /* MADCTL: MY=1 MX=1 MV=1 BGR=1 -> landscape, flipped */
#else
static const uint8_t p36b[] = {0x28};                /* MADCTL: MV=1 BGR=1 -> 480x320 landscape */
#endif
static const init_step_t INIT[] = {
    {0xF1, sizeof pF1, pF1, 0}, {0xF2, sizeof pF2, pF2, 0}, {0xF8, sizeof pF8, pF8, 0},
    {0xF9, sizeof pF9, pF9, 0}, {0x36, sizeof p36a, p36a, 0}, {0xB4, sizeof pB4, pB4, 0},
    {0xC1, sizeof pC1, pC1, 0}, {0xC5, sizeof pC5, pC5, 0}, {0xE0, sizeof pE0, pE0, 0},
    {0xE1, sizeof pE1, pE1, 0}, {0x3A, sizeof p3A, p3A, 0}, {0x11, 0, NULL, 0},
    {0x36, sizeof p36b, p36b, 255}, {0x29, 0, NULL, 0},
};

spi_host_device_t rpi_lcd_spi_host(void) { return LCD_SPI_HOST; }

int rpi_lcd_w = LCD_LONG, rpi_lcd_h = LCD_SHORT;

/* Portrait is the kit turned a quarter turn anticlockwise from the default landscape, USB-A and
 * Ethernet ports up. It is the same way up in both builds, so MADCTL is the same: MY, no
 * row/column exchange, 320x480. */
esp_err_t rpi_lcd_set_portrait(bool on) {
#if CONFIG_RPI_LCD_ROTATE_180
    uint8_t m = on ? 0x88 : 0xE8;
#else
    uint8_t m = on ? 0x88 : 0x28;
#endif
    ESP_RETURN_ON_ERROR(cmd(0x36), TAG, "madctl");
    ESP_RETURN_ON_ERROR(data8(m), TAG, "madctl");
    rpi_lcd_w = on ? LCD_SHORT : LCD_LONG;
    rpi_lcd_h = on ? LCD_LONG : LCD_SHORT;
    return ESP_OK;
}

esp_err_t rpi_lcd_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    gpio_set_level(PIN_LCD_DC, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STRIP_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "bus");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 4,
        .pre_cb = pre_cb,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &dev, &s_dev), TAG, "dev");

    s_strip = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_strip, ESP_ERR_NO_MEM, TAG, "strip");

    /* hardware reset */
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof INIT / sizeof INIT[0]; i++) {
        ESP_RETURN_ON_ERROR(cmd(INIT[i].cmd), TAG, "init cmd");
        for (int k = 0; k < INIT[i].n; k++) ESP_RETURN_ON_ERROR(data8(INIT[i].p[k]), TAG, "init data");
        if (INIT[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(INIT[i].delay_ms));
    }
    int khz = 0;
    spi_device_get_actual_freq(s_dev, &khz);
    ESP_LOGI(TAG, "init done: %dx%d, SPI requested %d Hz, actual %d kHz, CS=%d DC=%d RST=%d",
             LCD_W, LCD_H, LCD_SPI_HZ, khz, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
    return ESP_OK;
}

esp_err_t rpi_lcd_set_window(int x0, int y0, int x1, int y1) {
    ESP_RETURN_ON_ERROR(cmd(0x2A), TAG, "caset");
    data8(x0 >> 8); data8(x0); data8(x1 >> 8); data8(x1);
    ESP_RETURN_ON_ERROR(cmd(0x2B), TAG, "raset");
    data8(y0 >> 8); data8(y0); data8(y1 >> 8); data8(y1);
    return cmd(0x2C);
}

esp_err_t rpi_lcd_push_pixels_be(const uint8_t *be, size_t nbytes) {
    while (nbytes) {
        size_t n = nbytes > STRIP_BYTES ? STRIP_BYTES : nbytes;
        ESP_RETURN_ON_ERROR(tx(be, n, 1), TAG, "pix");
        be += n; nbytes -= n;
    }
    return ESP_OK;
}

esp_err_t rpi_lcd_fill_rect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }                      /* clip to the panel */
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return ESP_OK;
    ESP_RETURN_ON_ERROR(rpi_lcd_set_window(x, y, x + w - 1, y + h - 1), TAG, "win");
    size_t total = (size_t)w * h * 2;
    size_t fillb = total > STRIP_BYTES ? STRIP_BYTES : total;
    for (size_t i = 0; i < fillb; i += 2) { s_strip[i] = c >> 8; s_strip[i + 1] = c & 0xff; }
    while (total) {
        size_t n = total > STRIP_BYTES ? STRIP_BYTES : total;
        ESP_RETURN_ON_ERROR(tx(s_strip, n, 1), TAG, "fill");
        total -= n;
    }
    return ESP_OK;
}

esp_err_t rpi_lcd_blit(int x, int y, int w, int h, const uint16_t *pix) {
    if (w <= 0 || h <= 0) return ESP_OK;
    /* The ILI9486 wraps writes that run past the window, scrambling the screen, so refuse
     * rectangles that leave the panel. (Clipping would need a source stride.) */
    if (x < 0 || y < 0 || x + w > LCD_W || y + h > LCD_H) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(rpi_lcd_set_window(x, y, x + w - 1, y + h - 1), TAG, "win");
    size_t npix = (size_t)w * h, done = 0;
    while (done < npix) {
        size_t n = npix - done; if (n * 2 > STRIP_BYTES) n = STRIP_BYTES / 2;
        for (size_t i = 0; i < n; i++) { uint16_t v = pix[done + i]; s_strip[2*i] = v >> 8; s_strip[2*i+1] = v & 0xff; }
        ESP_RETURN_ON_ERROR(tx(s_strip, n * 2, 1), TAG, "blit");
        done += n;
    }
    return ESP_OK;
}
