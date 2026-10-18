#pragma once
/* Wi-Fi and BLE scanner screens in the current look (the scanning is radio_scan.c, on the
 * module's ESP32-C6). Same bar and big lift-to-fire keys as the ADS-B screen. No ESP-IDF calls
 * (tools/preview builds it).
 *
 *   Wi-Fi:  y 0..47  WI-FI  stats   [MORE] [CHAN|LIST] [EXIT]
 *           list: seven 38 px rows, strongest first  |  channels 1-13, one hump per network
 *   BLE:    y 0..47  BLE    stats   [MORE] [EXIT];  list: seven rows, strongest first */
#include <stdbool.h>
#include <stdint.h>

#define SCAN_MAX_AP  64
#define SCAN_MAX_BLE 96

typedef struct {
    char ssid[33];                           /* "" for a hidden network */
    uint8_t bssid[6];
    int rssi, ch;
    char sec[10];                            /* OPEN, WEP, WPA, WPA2, WPA3, ... */
    int64_t seen_ms;
} scan_ap_t;

typedef struct {
    uint8_t addr[6];                         /* as printed, most significant byte first */
    bool random, connectable;
    int rssi;
    char name[24];                           /* "" if it doesn't send one */
    char kind[16];                           /* maker or service, from the advert; "" if unknown */
    uint32_t count;
    int64_t seen_ms;
} scan_ble_t;

enum { SV_WIFI, SV_BLE };
enum { SV_NONE, SV_EXIT, SV_TOGGLE, SV_MORE };
enum { SV_PRESS, SV_MOVE, SV_RELEASE };

typedef struct {
    int kind;                                /* SV_WIFI / SV_BLE */
    const scan_ap_t *ap;
    int nap;
    const scan_ble_t *ble;
    int nble;
    int64_t now_ms;
    int scans;                               /* Wi-Fi sweeps done */
    const char *status;                      /* shown instead of the list when set (errors, starting) */
} scan_view_info_t;

void scan_view_init(void);
void scan_view_open(int kind);               /* list page, page 1 */
void scan_view_draw(const scan_view_info_t *in);   /* sorts in->ap / in->ble in place */
int scan_view_touch(int ev, int x, int y);   /* SV_* fired on release */
