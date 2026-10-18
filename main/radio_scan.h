#pragma once
/* Wi-Fi and BLE scanning on the module's ESP32-C6 (esp_hosted over SDIO: esp_wifi_remote for
 * Wi-Fi, the NimBLE host here with the C6 as its controller). One at a time; start it with the
 * RTL-SDR stream stopped, since the Wi-Fi buffers need the internal RAM the USB transfers use,
 * and stop it (which frees them) before streaming again. */
#include <stdbool.h>
#include "scan_view.h"

/* Bring the radio up and start sweeping. False, with a reason in *why, if the C6 won't answer. */
bool radio_scan_start(int kind, const char **why);
/* Wi-Fi: collect a finished sweep and start the next. Call often; cheap when nothing is new. */
void radio_scan_poll(void);
/* Copy the current table (newest data, entries unheard for 60 s dropped). Returns the count. */
int radio_scan_get_ap(scan_ap_t *out, int max, int *sweeps);
int radio_scan_get_ble(scan_ble_t *out, int max);
void radio_scan_stop(void);
