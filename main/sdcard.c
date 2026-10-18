#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "sdcard.h"

#define MOUNT       "/sd"
#define PIN_SD_PWR  45                       /* low = card powered (through Q1) */
#define SD_LDO_CHAN 4

static const char *TAG = "sdcard";

bool sdcard_read(const char *name, uint8_t **out, size_t *len, size_t max)
{
    *out = NULL;
    *len = 0;
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_SD_PWR, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(PIN_SD_PWR, 0);

    sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = SD_LDO_CHAN};
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr) != ESP_OK) {
        ESP_LOGE(TAG, "no LDO %d for the card", SD_LDO_CHAN);
        gpio_set_level(PIN_SD_PWR, 1);
        return false;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;           /* IOMUX pins 39..44 */
    host.pwr_ctrl_handle = pwr;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = 43; slot.cmd = 44; slot.d0 = 39; slot.d1 = 40; slot.d2 = 41; slot.d3 = 42;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 1,
                                           .allocation_unit_size = 16 * 1024};
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &card);
    bool ok = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "card: %s, %llu MB", card->cid.name,
                 (unsigned long long)card->csd.capacity * card->csd.sector_size >> 20);
        char path[48];
        snprintf(path, sizeof path, MOUNT "/%s", name);
        struct stat st;
        FILE *f = NULL;
        if (stat(path, &st) != 0) ESP_LOGW(TAG, "%s: not on the card", name);
        else if ((size_t)st.st_size > max) ESP_LOGW(TAG, "%s: %ld bytes, over %u", name, (long)st.st_size, (unsigned)max);
        else if (!(*out = heap_caps_malloc(st.st_size ? st.st_size : 1, MALLOC_CAP_SPIRAM))) ESP_LOGE(TAG, "no PSRAM");
        else if (!(f = fopen(path, "rb"))) ESP_LOGW(TAG, "%s: can't open", name);
        else if (fread(*out, 1, st.st_size, f) != (size_t)st.st_size) ESP_LOGW(TAG, "%s: short read", name);
        else { *len = st.st_size; ok = true; ESP_LOGI(TAG, "%s: %ld bytes", name, (long)st.st_size); }
        if (f) fclose(f);
        if (!ok) { free(*out); *out = NULL; }
        esp_vfs_fat_sdcard_unmount(MOUNT, card);
    }
    sd_pwr_ctrl_del_on_chip_ldo(pwr);
    gpio_set_level(PIN_SD_PWR, 1);
    return ok;
}
