#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "sdcard.h"

#define PIN_SD_PWR  45                       /* low = card powered (through Q1) */
#define SD_LDO_CHAN 4

static const char *TAG = "sdcard";
static SemaphoreHandle_t s_lock;
static int s_users;
static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;

static void lock(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    taskEXIT_CRITICAL(&mux);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void power_off(void)
{
    if (s_pwr) sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
    s_pwr = NULL;
    gpio_set_level(PIN_SD_PWR, 1);
}

bool sdcard_mount(void)
{
    lock();
    if (s_users) {
        s_users++;
        xSemaphoreGive(s_lock);
        return true;
    }
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_SD_PWR, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(PIN_SD_PWR, 0);
    sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = SD_LDO_CHAN};
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) {
        ESP_LOGE(TAG, "no LDO %d for the card", SD_LDO_CHAN);
        power_off();
        xSemaphoreGive(s_lock);
        return false;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();    /* its deinit frees this slot only */
    host.slot = SDMMC_HOST_SLOT_0;               /* IOMUX pins 39..44 */
    host.pwr_ctrl_handle = s_pwr;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = 43; slot.cmd = 44; slot.d0 = 39; slot.d1 = 40; slot.d2 = 41; slot.d3 = 42;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 2,
                                           .allocation_unit_size = 16 * 1024};
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_ROOT, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card (%s)", esp_err_to_name(err));
        s_card = NULL;
        power_off();
        xSemaphoreGive(s_lock);
        return false;
    }
    ESP_LOGI(TAG, "card: %s, %llu MB", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size >> 20);
    s_users = 1;
    xSemaphoreGive(s_lock);
    return true;
}

void sdcard_unmount(void)
{
    lock();
    if (s_users && --s_users == 0) {
        esp_vfs_fat_sdcard_unmount(SDCARD_ROOT, s_card);
        s_card = NULL;
        power_off();
    }
    xSemaphoreGive(s_lock);
}

bool sdcard_read(const char *name, uint8_t **out, size_t *len, size_t max)
{
    *out = NULL;
    *len = 0;
    if (!sdcard_mount()) return false;
    char path[48];
    snprintf(path, sizeof path, SDCARD_ROOT "/%s", name);
    struct stat st;
    FILE *f = NULL;
    bool ok = false;
    if (stat(path, &st) != 0) ESP_LOGW(TAG, "%s: not on the card", name);
    else if ((size_t)st.st_size > max) ESP_LOGW(TAG, "%s: %ld bytes, over %u", name, (long)st.st_size, (unsigned)max);
    else if (!(*out = heap_caps_malloc(st.st_size ? st.st_size : 1, MALLOC_CAP_SPIRAM))) ESP_LOGE(TAG, "no PSRAM");
    else if (!(f = fopen(path, "rb"))) ESP_LOGW(TAG, "%s: can't open", name);
    else if (fread(*out, 1, st.st_size, f) != (size_t)st.st_size) ESP_LOGW(TAG, "%s: short read", name);
    else { *len = st.st_size; ok = true; ESP_LOGI(TAG, "%s: %ld bytes", name, (long)st.st_size); }
    if (f) fclose(f);
    if (!ok) { free(*out); *out = NULL; }
    sdcard_unmount();
    return ok;
}
