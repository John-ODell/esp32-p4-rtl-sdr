#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "radio_scan.h"

static const char *TAG = "scan";

#define FORGET_MS 60000

static int s_kind = -1;
static SemaphoreHandle_t s_lock;
static scan_ap_t *s_ap;                        /* PSRAM tables */
static scan_ble_t *s_ble;
static int s_nap, s_nble, s_sweeps;
static volatile bool s_done;                   /* a Wi-Fi sweep finished */
static wifi_ap_record_t *s_rec;                /* one sweep's records */
static esp_event_handler_instance_t s_evh;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void log_ram(const char *when)
{
    ESP_LOGI(TAG, "%s: free internal %u KB (DMA largest %u KB)", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) >> 10));
}

/* ------------------------------------------------------------------------- Wi-Fi -- */

static const char *sec_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_OWE: return "OWE";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-ENT";
    default: return "?";
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_SCAN_DONE) s_done = true;
}

static esp_err_t sweep(void)
{
    wifi_scan_config_t sc = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 60, .max = 120},
    };
    s_done = false;
    return esp_wifi_scan_start(&sc, false);
}

static bool wifi_start(const char **why)
{
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { *why = "no event loop"; return false; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e));
        *why = "the C6 didn't answer";
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, on_wifi, NULL, &s_evh);
    if ((e = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK || (e = esp_wifi_start()) != ESP_OK || (e = sweep()) != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(e));
        *why = "Wi-Fi wouldn't start";
        return false;
    }
    return true;
}

static void wifi_collect(void)
{
    uint16_t n = SCAN_MAX_AP;
    if (esp_wifi_scan_get_ap_records(&n, s_rec) != ESP_OK) n = 0;
    int64_t t = now_ms();
    lock();
    for (int i = 0; i < n; i++) {
        const wifi_ap_record_t *r = &s_rec[i];
        int k = 0;
        while (k < s_nap && memcmp(s_ap[k].bssid, r->bssid, 6)) k++;
        if (k == s_nap) {
            if (s_nap == SCAN_MAX_AP) {            /* full: replace the stalest */
                k = 0;
                for (int j = 1; j < s_nap; j++) if (s_ap[j].seen_ms < s_ap[k].seen_ms) k = j;
            } else {
                s_nap++;
            }
            memset(&s_ap[k], 0, sizeof s_ap[k]);
            memcpy(s_ap[k].bssid, r->bssid, 6);
        }
        scan_ap_t *a = &s_ap[k];
        memcpy(a->ssid, r->ssid, 32);
        a->ssid[32] = 0;
        a->rssi = r->rssi;
        a->ch = r->primary;
        strncpy(a->sec, sec_name(r->authmode), sizeof a->sec - 1);
        a->seen_ms = t;
    }
    for (int k = 0; k < s_nap;)
        if (t - s_ap[k].seen_ms > FORGET_MS) s_ap[k] = s_ap[--s_nap]; else k++;
    s_sweeps++;
    unlock();
}

/* --------------------------------------------------------------------------- BLE -- */

static const struct { uint16_t id; const char *name; } s_company[] = {
    {0x004C, "Apple"}, {0x0006, "Microsoft"}, {0x00E0, "Google"}, {0x0075, "Samsung"},
    {0x0171, "Amazon"}, {0x0087, "Garmin"}, {0x0157, "Huami"}, {0x038F, "Xiaomi"},
    {0x0059, "Nordic"}, {0x02E5, "Espressif"}, {0x0499, "Ruuvi"}, {0x000F, "Broadcom"},
    {0x0002, "Intel"}, {0x001D, "Qualcomm"}, {0x0131, "Cypress"}, {0x00D2, "Dialog"},
    {0x0310, "SGL Italia"}, {0x067C, "Tile"}, {0x0822, "adidas"}, {0x02FF, "Silicon Labs"},
    {0x0000, NULL}};
static const struct { uint16_t uuid; const char *name; } s_service[] = {
    {0xFD6F, "Exposure"}, {0xFEAA, "Eddystone"}, {0xFE2C, "Fast Pair"}, {0xFEED, "Tile"},
    {0xFE9F, "Google"}, {0xFD5A, "Samsung"}, {0x180D, "Heart rate"}, {0x180F, "Battery"},
    {0xFE07, "Sonos"}, {0xFCF1, "Google"}, {0x0000, NULL}};

static int on_gap(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        ESP_LOGW(TAG, "BLE discovery ended (%d)", ev->disc_complete.reason);
        return 0;
    }
    if (ev->type != BLE_GAP_EVENT_DISC) return 0;
    const struct ble_gap_disc_desc *d = &ev->disc;
    struct ble_hs_adv_fields f;
    bool parsed = ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0;
    uint8_t addr[6];
    for (int i = 0; i < 6; i++) addr[i] = d->addr.val[5 - i];  /* NimBLE keeps it little endian */
    int64_t t = now_ms();
    lock();
    int k = 0;
    while (k < s_nble && memcmp(s_ble[k].addr, addr, 6)) k++;
    if (k == s_nble) {
        if (s_nble == SCAN_MAX_BLE) {
            k = 0;
            for (int j = 1; j < s_nble; j++) if (s_ble[j].seen_ms < s_ble[k].seen_ms) k = j;
        } else {
            s_nble++;
        }
        memset(&s_ble[k], 0, sizeof s_ble[k]);
        memcpy(s_ble[k].addr, addr, 6);
    }
    scan_ble_t *b = &s_ble[k];
    b->random = d->addr.type != BLE_ADDR_PUBLIC;
    b->rssi = d->rssi;
    b->seen_ms = t;
    b->count++;
    if (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND || d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND)
        b->connectable = true;
    if (parsed && f.name && f.name_len) {          /* names often come only in scan responses */
        int n = f.name_len < (int)sizeof b->name - 1 ? f.name_len : (int)sizeof b->name - 1;
        for (int i = 0; i < n; i++) b->name[i] = f.name[i] >= 32 && f.name[i] < 127 ? f.name[i] : '?';
        b->name[n] = 0;
    }
    if (parsed && !b->kind[0]) {
        if (f.mfg_data && f.mfg_data_len >= 2) {
            uint16_t id = f.mfg_data[0] | f.mfg_data[1] << 8;
            for (int i = 0; s_company[i].name; i++)
                if (s_company[i].id == id) { strncpy(b->kind, s_company[i].name, sizeof b->kind - 1); break; }
            if (!b->kind[0]) snprintf(b->kind, sizeof b->kind, "maker %04X", id);
        }
        for (int u = 0; u < f.num_uuids16 && !b->kind[0]; u++)
            for (int i = 0; s_service[i].name; i++)
                if (s_service[i].uuid == ble_uuid_u16(&f.uuids16[u].u)) {
                    strncpy(b->kind, s_service[i].name, sizeof b->kind - 1);
                    break;
                }
    }
    unlock();
    return 0;
}

static void ble_disc(void)
{
    struct ble_gap_disc_params p = {
        .itvl = 0x60, .window = 0x60,            /* 60 ms windows back to back: always listening */
        .filter_duplicates = 0,                  /* keep RSSI and counts fresh */
        .passive = 0,                            /* active: ask for scan responses (names) */
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, on_gap, NULL);
    if (rc) ESP_LOGE(TAG, "ble_gap_disc: %d", rc);
    else ESP_LOGI(TAG, "BLE discovery running");
}

/* NimBLE in IDF 5.5 calls this on deinit; esp_hosted 1.4's VHCI transport doesn't define it
 * and has nothing to tear down. */
void ble_transport_ll_deinit(void) {}

static volatile bool s_synced;
static void on_sync(void) { s_synced = true; ble_disc(); }
static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset (%d)", reason); }

static void host_task(void *arg)
{
    nimble_port_run();                           /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static bool ble_start(const char **why)
{
    s_synced = false;
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        *why = "Bluetooth wouldn't start";
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    for (int i = 0; i < 50 && !s_synced; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_synced) {
        ESP_LOGE(TAG, "no BLE sync from the C6 in 5 s");
        *why = "the C6 has no Bluetooth link";
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ public -- */

bool radio_scan_start(int kind, const char **why)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_ap) s_ap = heap_caps_calloc(SCAN_MAX_AP, sizeof *s_ap, MALLOC_CAP_SPIRAM);
    if (!s_ble) s_ble = heap_caps_calloc(SCAN_MAX_BLE, sizeof *s_ble, MALLOC_CAP_SPIRAM);
    if (!s_rec) s_rec = heap_caps_calloc(SCAN_MAX_AP, sizeof *s_rec, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_ap || !s_ble || !s_rec) { *why = "out of memory"; return false; }
    s_nap = s_nble = s_sweeps = 0;
    log_ram(kind == SV_WIFI ? "before Wi-Fi" : "before BLE");
    s_kind = kind;
    bool ok = kind == SV_WIFI ? wifi_start(why) : ble_start(why);
    log_ram(ok ? "running" : "failed");
    if (!ok) radio_scan_stop();
    return ok;
}

void radio_scan_poll(void)
{
    if (s_kind != SV_WIFI || !s_done) return;
    wifi_collect();
    if (sweep() != ESP_OK) ESP_LOGW(TAG, "next sweep didn't start");
}

int radio_scan_get_ap(scan_ap_t *out, int max, int *sweeps)
{
    lock();
    int n = s_nap < max ? s_nap : max;
    memcpy(out, s_ap, n * sizeof *out);
    *sweeps = s_sweeps;
    unlock();
    return n;
}

int radio_scan_get_ble(scan_ble_t *out, int max)
{
    int64_t t = now_ms();
    lock();
    for (int k = 0; k < s_nble;)
        if (t - s_ble[k].seen_ms > FORGET_MS) s_ble[k] = s_ble[--s_nble]; else k++;
    int n = s_nble < max ? s_nble : max;
    memcpy(out, s_ble, n * sizeof *out);
    unlock();
    return n;
}

void radio_scan_stop(void)
{
    if (s_kind == SV_WIFI) {
        esp_wifi_scan_stop();
        esp_wifi_stop();
        if (s_evh) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, s_evh);
        s_evh = NULL;
        esp_wifi_deinit();
    } else if (s_kind == SV_BLE) {
        if (s_synced) ble_gap_disc_cancel();
        if (nimble_port_stop() == 0) nimble_port_deinit();
    }
    s_kind = -1;
    vTaskDelay(pdMS_TO_TICKS(50));
    log_ram("scanner off");
}
