/* ESP-IDF glue for librtlsdr: brings up the USB Host Library (with external-hub support, the kit's
 * Type-A ports sit behind a CH334F hub) and the client that the libusb shim runs on. */
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "rtl-sdr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent. libusb_init() (so rtlsdr_open/get_device_count) calls it too. */
esp_err_t rtlsdr_usb_start(void);

/* Block until a USB device has enumerated since start (or timeout). true if one did. */
bool rtlsdr_usb_wait_new_device(uint32_t timeout_ms);

typedef struct {
    uint32_t bulk_done;        /* bulk transfers handed back to librtlsdr */
    uint32_t bulk_errors;      /* ... with an error/stall/overflow status */
    uint64_t bulk_bytes;
    uint32_t max_backlog;      /* most completions waiting at once (near buf_num = consumer too slow) */
    uint32_t ctrl_done, ctrl_errors;
    uint8_t  speed;            /* usb_speed_t of the opened device: 0 LS, 1 FS, 2 HS */
} rtlsdr_usb_stats_t;
void rtlsdr_usb_get_stats(rtlsdr_usb_stats_t *out);

/* true once after any opened device disappeared (unplug). */
bool rtlsdr_usb_take_gone(void);

#ifdef __cplusplus
}
#endif
