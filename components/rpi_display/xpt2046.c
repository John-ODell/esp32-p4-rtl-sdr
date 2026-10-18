#include "xpt2046.h"
#include "pins.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"

static const char *TAG = "xpt2046";
static spi_device_handle_t s_dev;

static uint16_t rd12(uint8_t ctrl) {
    uint8_t tx[3] = {ctrl, 0, 0}, rx[3] = {0};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx, .rx_buffer = rx};
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) return 0;
    return ((rx[1] << 8) | rx[2]) >> 3;            /* 12 bits, MSB first after the busy bit */
}
static uint16_t median5(uint8_t ctrl) {
    uint16_t v[5]; for (int i = 0; i < 5; i++) v[i] = rd12(ctrl);
    /* tiny insertion sort */
    for (int i = 1; i < 5; i++) { uint16_t k = v[i]; int j = i - 1; while (j >= 0 && v[j] > k) { v[j+1] = v[j]; j--; } v[j+1] = k; }
    return v[2];
}

esp_err_t xpt2046_init(void) {
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_TP_IRQ, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "irq");
    spi_device_interface_config_t dev = {
        .clock_speed_hz = TP_SPI_HZ, .mode = 0, .spics_io_num = PIN_TP_CS, .queue_size = 2,
    };
    return spi_bus_add_device(LCD_SPI_HOST, &dev, &s_dev);
}
bool xpt2046_pressed(void) { return gpio_get_level(PIN_TP_IRQ) == 0; }
bool xpt2046_read_raw(uint16_t *x, uint16_t *y, uint16_t *z) {
    if (!xpt2046_pressed()) return false;
    /* differential mode, 12-bit, PENIRQ enabled: X=0xD0, Y=0x90, Z1=0xB0, Z2=0xC0 */
    *x = median5(0xD0); *y = median5(0x90);
    uint16_t z1 = rd12(0xB0), z2 = rd12(0xC0);
    *z = (z2 > z1) ? (uint16_t)(4095 - (z2 - z1)) : 0;
    return xpt2046_pressed();
}
