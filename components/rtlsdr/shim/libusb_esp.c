/*
 * libusb_esp.c: the libusb-1.0 subset librtlsdr needs, on the ESP-IDF USB Host Library.
 *
 *  - One USB Host client for the whole app, pumped by its own task. Transfer completions arrive in
 *    that task; sync transfers wake their caller, async (bulk) ones go on a queue.
 *  - libusb_handle_events_timeout_completed() (called by rtlsdr_read_async's thread) pops that queue
 *    and runs the libusb callbacks there, so librtlsdr resubmits from its own thread as on a PC.
 *  - Zero copy: while the callback runs, transfer->buffer points at the ESP-IDF transfer's DMA
 *    buffer (librtlsdr only reads it there and resubmits after), so no 4.8 MB/s memcpy.
 *  - Cancel = halt + flush endpoint (all queued transfers come back CANCELED), clear when idle.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "libusb.h"
#include "rtlsdr_usb.h"

static const char *TAG = "libusb";

#define MAX_DEVS        8
#define DONEQ_LEN       64
#define CTRL_BUF_LEN    (sizeof(usb_setup_packet_t) + 1024)

struct libusb_context { int unused; };
struct libusb_device {
    uint8_t addr;
    struct libusb_device_descriptor dd;
};
struct libusb_device_handle {
    usb_device_handle_t hdl;
    struct libusb_device dev;
    volatile bool gone;
    int claimed;                /* bitmask of claimed interfaces */
};

typedef struct {
    struct libusb_transfer pub; /* must stay first */
    usb_transfer_t *ut;
    volatile bool owned;        /* submitted and not yet given back by handle_events */
    volatile bool cancel_req;
} xfer_t;

static struct libusb_context s_ctx;
static usb_host_client_handle_t s_client;
static QueueHandle_t s_doneq;
static SemaphoreHandle_t s_ctrl_lock, s_ctrl_done, s_newdev;
static usb_transfer_t *s_ctrl;
static volatile int s_owned;            /* async transfers owned by the shim */
static volatile bool s_halted;          /* bulk IN endpoint halted for cancel */
static libusb_device_handle *s_halted_h;
static uint8_t s_halted_ep;
static libusb_device_handle *s_open[4];
static rtlsdr_usb_stats_t s_stats;
static volatile bool s_dev_gone_any;

/* Freed bulk transfers are kept for reuse: streaming start/stop must not fragment the internal
 * DMA heap (its biggest region is 384 KB and the stream needs ~240 KB of it). */
#define POOL_LEN 16
static usb_transfer_t *s_pool[POOL_LEN];

static usb_transfer_t *pool_get(size_t len)
{
    for (int i = 0; i < POOL_LEN; i++)
        if (s_pool[i] && s_pool[i]->data_buffer_size >= len) { usb_transfer_t *t = s_pool[i]; s_pool[i] = NULL; return t; }
    usb_transfer_t *t = NULL;
    if (usb_host_transfer_alloc(len, 0, &t) == ESP_OK) return t;
    for (int i = 0; i < POOL_LEN; i++)                 /* wrong-size spares: free them and retry */
        if (s_pool[i]) { usb_host_transfer_free(s_pool[i]); s_pool[i] = NULL; }
    return usb_host_transfer_alloc(len, 0, &t) == ESP_OK ? t : NULL;
}

static void pool_put(usb_transfer_t *t)
{
    if (!t) return;
    for (int i = 0; i < POOL_LEN; i++) if (!s_pool[i]) { s_pool[i] = t; return; }
    usb_host_transfer_free(t);
}

/* ------------------------------------------------------------------ host + client bring-up -- */

static void lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG, "device enumerated, address %d", msg->new_dev.address);
        xSemaphoreGive(s_newdev);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "device gone");
        s_dev_gone_any = true;
        for (int i = 0; i < 4; i++)
            if (s_open[i] && s_open[i]->hdl == msg->dev_gone.dev_hdl) s_open[i]->gone = true;
    }
}

static void client_task(void *arg)
{
    for (;;) usb_host_client_handle_events(s_client, portMAX_DELAY);
}

esp_err_t rtlsdr_usb_start(void)
{
    static bool started;
    if (started) return ESP_OK;

    const usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    esp_err_t r = usb_host_install(&hc);
    if (r != ESP_OK) { ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(r)); return r; }
    xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, 22, NULL, 0);

    s_newdev = xSemaphoreCreateCounting(16, 0);
    s_doneq = xQueueCreate(DONEQ_LEN, sizeof(xfer_t *));
    s_ctrl_lock = xSemaphoreCreateMutex();
    s_ctrl_done = xSemaphoreCreateBinary();
    const usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    r = usb_host_client_register(&cc, &s_client);
    if (r != ESP_OK) { ESP_LOGE(TAG, "client_register: %s", esp_err_to_name(r)); return r; }
    ESP_ERROR_CHECK(usb_host_transfer_alloc(CTRL_BUF_LEN, 0, &s_ctrl));
    xTaskCreatePinnedToCore(client_task, "usb_client", 4096, NULL, 21, NULL, 0);
    started = true;
    return ESP_OK;
}

bool rtlsdr_usb_wait_new_device(uint32_t timeout_ms)
{
    if (rtlsdr_usb_start() != ESP_OK) return false;
    return xSemaphoreTake(s_newdev, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void rtlsdr_usb_get_stats(rtlsdr_usb_stats_t *out) { *out = s_stats; }

bool rtlsdr_usb_take_gone(void)
{
    bool g = s_dev_gone_any;
    s_dev_gone_any = false;
    return g;
}

int libusb_init(libusb_context **ctx)
{
    if (rtlsdr_usb_start() != ESP_OK) return LIBUSB_ERROR_OTHER;
    if (ctx) *ctx = &s_ctx;
    return 0;
}

void libusb_exit(libusb_context *ctx) { (void)ctx; /* host + client stay up for the app's life */ }

/* --------------------------------------------------------------------------- enumeration -- */

static void copy_desc(struct libusb_device_descriptor *d, const usb_device_desc_t *s)
{
    d->bLength = s->bLength;              d->bDescriptorType = s->bDescriptorType;
    d->bcdUSB = s->bcdUSB;                d->bDeviceClass = s->bDeviceClass;
    d->bDeviceSubClass = s->bDeviceSubClass; d->bDeviceProtocol = s->bDeviceProtocol;
    d->bMaxPacketSize0 = s->bMaxPacketSize0; d->idVendor = s->idVendor;
    d->idProduct = s->idProduct;          d->bcdDevice = s->bcdDevice;
    d->iManufacturer = s->iManufacturer;  d->iProduct = s->iProduct;
    d->iSerialNumber = s->iSerialNumber;  d->bNumConfigurations = s->bNumConfigurations;
}

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    uint8_t addrs[MAX_DEVS];
    int n = 0;
    usb_host_device_addr_list_fill(MAX_DEVS, addrs, &n);
    libusb_device **l = calloc(n + 1, sizeof(*l));
    if (!l) return LIBUSB_ERROR_NO_MEM;
    int k = 0;
    for (int i = 0; i < n; i++) {
        usb_device_handle_t h;
        if (usb_host_device_open(s_client, addrs[i], &h) != ESP_OK) continue;
        const usb_device_desc_t *dd;
        if (usb_host_get_device_descriptor(h, &dd) == ESP_OK) {
            libusb_device *d = calloc(1, sizeof(*d));
            if (d) { d->addr = addrs[i]; copy_desc(&d->dd, dd); l[k++] = d; }
        }
        usb_host_device_close(s_client, h);
    }
    *list = l;
    return k;
}

void libusb_free_device_list(libusb_device **list, int unref)
{
    if (!list) return;
    for (int i = 0; list[i]; i++) free(list[i]);
    free(list);
}

int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    *desc = dev->dd;
    return 0;
}

int libusb_open(libusb_device *dev, libusb_device_handle **handle)
{
    libusb_device_handle *h = calloc(1, sizeof(*h));
    if (!h) return LIBUSB_ERROR_NO_MEM;
    esp_err_t r = usb_host_device_open(s_client, dev->addr, &h->hdl);
    if (r != ESP_OK) { free(h); return LIBUSB_ERROR_NO_DEVICE; }
    h->dev = *dev;
    usb_device_info_t info;
    if (usb_host_device_info(h->hdl, &info) == ESP_OK) s_stats.speed = info.speed;
    for (int i = 0; i < 4; i++) if (!s_open[i]) { s_open[i] = h; break; }
    *handle = h;
    return 0;
}

void libusb_close(libusb_device_handle *h)
{
    if (!h) return;
    for (int i = 0; i < 4; i++) if (s_open[i] == h) s_open[i] = NULL;
    usb_host_device_close(s_client, h->hdl);
    free(h);
}

libusb_device *libusb_get_device(libusb_device_handle *h) { return &h->dev; }

int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx, unsigned char *data, int length)
{
    usb_device_info_t info;
    if (idx == 0 || length <= 0 || usb_host_device_info(h->hdl, &info) != ESP_OK) return LIBUSB_ERROR_INVALID_PARAM;
    const usb_str_desc_t *s = NULL;
    if (idx == h->dev.dd.iManufacturer) s = info.str_desc_manufacturer;
    else if (idx == h->dev.dd.iProduct) s = info.str_desc_product;
    else if (idx == h->dev.dd.iSerialNumber) s = info.str_desc_serial_num;
    if (!s) return LIBUSB_ERROR_NOT_FOUND;
    int n = (s->bLength - 2) / 2, k = 0;
    for (int i = 0; i < n && k < length - 1; i++) {
        uint16_t c = s->wData[i];
        data[k++] = c < 0x80 ? (unsigned char)c : '?';
    }
    data[k] = 0;
    return k;
}

int libusb_kernel_driver_active(libusb_device_handle *h, int iface) { return 0; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int iface) { return 0; }
int libusb_attach_kernel_driver(libusb_device_handle *h, int iface) { return 0; }
int libusb_reset_device(libusb_device_handle *h) { return LIBUSB_ERROR_NOT_SUPPORTED; }

int libusb_claim_interface(libusb_device_handle *h, int iface)
{
    esp_err_t r = usb_host_interface_claim(s_client, h->hdl, iface, 0);
    if (r != ESP_OK) { ESP_LOGE(TAG, "claim if%d: %s", iface, esp_err_to_name(r)); return LIBUSB_ERROR_BUSY; }
    h->claimed |= 1 << iface;
    return 0;
}

int libusb_release_interface(libusb_device_handle *h, int iface)
{
    if (!(h->claimed & (1 << iface))) return LIBUSB_ERROR_NOT_FOUND;
    h->claimed &= ~(1 << iface);
    return usb_host_interface_release(s_client, h->hdl, iface) == ESP_OK ? 0 : LIBUSB_ERROR_OTHER;
}

/* --------------------------------------------------------------------------- sync transfers -- */

static int map_status(usb_transfer_status_t st)
{
    switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED: return LIBUSB_TRANSFER_COMPLETED;
    case USB_TRANSFER_STATUS_TIMED_OUT: return LIBUSB_TRANSFER_TIMED_OUT;
    case USB_TRANSFER_STATUS_CANCELED:  return LIBUSB_TRANSFER_CANCELLED;
    case USB_TRANSFER_STATUS_STALL:     return LIBUSB_TRANSFER_STALL;
    case USB_TRANSFER_STATUS_NO_DEVICE: return LIBUSB_TRANSFER_NO_DEVICE;
    case USB_TRANSFER_STATUS_OVERFLOW:  return LIBUSB_TRANSFER_OVERFLOW;
    default:                            return LIBUSB_TRANSFER_ERROR;
    }
}

static int status_to_err(usb_transfer_status_t st)
{
    switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED: return 0;
    case USB_TRANSFER_STATUS_TIMED_OUT: return LIBUSB_ERROR_TIMEOUT;
    case USB_TRANSFER_STATUS_STALL:     return LIBUSB_ERROR_PIPE;
    case USB_TRANSFER_STATUS_NO_DEVICE: return LIBUSB_ERROR_NO_DEVICE;
    case USB_TRANSFER_STATUS_OVERFLOW:  return LIBUSB_ERROR_OVERFLOW;
    default:                            return LIBUSB_ERROR_IO;
    }
}

static void sync_cb(usb_transfer_t *t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }

static TickType_t wait_ticks(unsigned int timeout_ms)
{
    return timeout_ms ? pdMS_TO_TICKS(timeout_ms + 200) : portMAX_DELAY;
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, unsigned char *data,
                            uint16_t wLength, unsigned int timeout)
{
    if (h->gone) return LIBUSB_ERROR_NO_DEVICE;
    if (sizeof(usb_setup_packet_t) + wLength > CTRL_BUF_LEN) return LIBUSB_ERROR_INVALID_PARAM;
    xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    usb_transfer_t *t = s_ctrl;
    usb_setup_packet_t *sp = (usb_setup_packet_t *)t->data_buffer;
    sp->bmRequestType = bmRequestType;
    sp->bRequest = bRequest;
    sp->wValue = wValue;
    sp->wIndex = wIndex;
    sp->wLength = wLength;
    bool in = bmRequestType & LIBUSB_ENDPOINT_IN;
    if (!in && wLength) memcpy(t->data_buffer + sizeof(*sp), data, wLength);
    t->num_bytes = sizeof(*sp) + wLength;
    t->device_handle = h->hdl;
    t->bEndpointAddress = 0;
    t->callback = sync_cb;
    t->context = s_ctrl_done;
    t->timeout_ms = timeout;
    int ret;
    if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) {
        ret = LIBUSB_ERROR_IO;
    } else if (xSemaphoreTake(s_ctrl_done, wait_ticks(timeout)) != pdTRUE) {
        /* Still in flight: we cannot free it, so leave it to the stack and use a fresh one. */
        ESP_LOGE(TAG, "control transfer timed out (req %02x %02x)", bmRequestType, bRequest);
        if (usb_host_transfer_alloc(CTRL_BUF_LEN, 0, &s_ctrl) != ESP_OK) abort();
        s_ctrl_done = xSemaphoreCreateBinary();
        ret = LIBUSB_ERROR_TIMEOUT;
    } else if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        ret = status_to_err(t->status);
    } else {
        ret = t->actual_num_bytes - (int)sizeof(*sp);
        if (ret < 0) ret = 0;
        if (in && ret) memcpy(data, t->data_buffer + sizeof(*sp), ret);
    }
    if (ret < 0) s_stats.ctrl_errors++; else s_stats.ctrl_done++;
    xSemaphoreGive(s_ctrl_lock);
    return ret;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data,
                         int length, int *transferred, unsigned int timeout)
{
    if (h->gone) return LIBUSB_ERROR_NO_DEVICE;
    usb_transfer_t *t;
    if (usb_host_transfer_alloc(length, 0, &t) != ESP_OK) return LIBUSB_ERROR_NO_MEM;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!(endpoint & LIBUSB_ENDPOINT_IN)) memcpy(t->data_buffer, data, length);
    t->num_bytes = length;
    t->device_handle = h->hdl;
    t->bEndpointAddress = endpoint;
    t->callback = sync_cb;
    t->context = done;
    t->timeout_ms = timeout;
    int ret = 0;
    if (usb_host_transfer_submit(t) != ESP_OK) {
        ret = LIBUSB_ERROR_IO;
    } else if (xSemaphoreTake(done, wait_ticks(timeout)) != pdTRUE) {
        usb_host_endpoint_halt(h->hdl, endpoint);
        usb_host_endpoint_flush(h->hdl, endpoint);
        xSemaphoreTake(done, portMAX_DELAY);
        usb_host_endpoint_clear(h->hdl, endpoint);
        ret = LIBUSB_ERROR_TIMEOUT;
    } else {
        ret = status_to_err(t->status);
    }
    if (transferred) *transferred = t->actual_num_bytes;
    if (endpoint & LIBUSB_ENDPOINT_IN) memcpy(data, t->data_buffer, t->actual_num_bytes);
    vSemaphoreDelete(done);
    usb_host_transfer_free(t);
    return ret;
}

/* -------------------------------------------------------------------------- async transfers -- */

struct libusb_transfer *libusb_alloc_transfer(int iso_packets)
{
    xfer_t *x = calloc(1, sizeof(*x));
    return x ? &x->pub : NULL;
}

void libusb_free_transfer(struct libusb_transfer *transfer)
{
    xfer_t *x = (xfer_t *)transfer;
    if (!x) return;
    if (x->owned) { ESP_LOGE(TAG, "free of in-flight transfer, leaking it"); return; }
    pool_put(x->ut);
    free(x);
}

static void async_cb(usb_transfer_t *t)
{
    xfer_t *x = t->context;
    xQueueSend(s_doneq, &x, portMAX_DELAY);   /* queue holds DONEQ_LEN >= every owned transfer */
}

static void clear_halt_if_idle(void)
{
    if (s_halted && s_owned == 0) {
        usb_host_endpoint_clear(s_halted_h->hdl, s_halted_ep);
        s_halted = false;
    }
}

int libusb_submit_transfer(struct libusb_transfer *transfer)
{
    xfer_t *x = (xfer_t *)transfer;
    libusb_device_handle *h = transfer->dev_handle;
    if (x->owned) return LIBUSB_ERROR_BUSY;
    if (h->gone) return LIBUSB_ERROR_NO_DEVICE;
    clear_halt_if_idle();
    if (s_halted) return LIBUSB_ERROR_BUSY;               /* a cancel is draining the endpoint */
    if (!x->ut || x->ut->data_buffer_size < (size_t)transfer->length) {
        pool_put(x->ut);
        x->ut = pool_get(transfer->length);
        if (!x->ut) {
            ESP_LOGE(TAG, "no DMA memory for a %d byte transfer (%d owned, DMA free %u, largest %u)",
                     transfer->length, s_owned,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            return LIBUSB_ERROR_NO_MEM;
        }
    }
    usb_transfer_t *t = x->ut;
    if (!(transfer->endpoint & LIBUSB_ENDPOINT_IN)) memcpy(t->data_buffer, transfer->buffer, transfer->length);
    t->num_bytes = transfer->length;
    t->device_handle = h->hdl;
    t->bEndpointAddress = transfer->endpoint;
    t->callback = async_cb;
    t->context = x;
    t->timeout_ms = transfer->timeout;
    x->cancel_req = false;
    x->owned = true;
    s_owned++;
    esp_err_t r = usb_host_transfer_submit(t);
    if (r != ESP_OK) {
        x->owned = false;
        s_owned--;
        ESP_LOGE(TAG, "submit: %s", esp_err_to_name(r));
        return LIBUSB_ERROR_IO;
    }
    return 0;
}

int libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    xfer_t *x = (xfer_t *)transfer;
    if (!x->owned) return LIBUSB_ERROR_NOT_FOUND;
    x->cancel_req = true;
    if (!s_halted) {
        s_halted = true;
        s_halted_h = transfer->dev_handle;
        s_halted_ep = transfer->endpoint;
        usb_host_endpoint_halt(s_halted_h->hdl, s_halted_ep);
        usb_host_endpoint_flush(s_halted_h->hdl, s_halted_ep);
    }
    return 0;
}

static void deliver(xfer_t *x)
{
    usb_transfer_t *t = x->ut;
    struct libusb_transfer *p = &x->pub;
    x->owned = false;
    s_owned--;
    p->status = x->cancel_req ? LIBUSB_TRANSFER_CANCELLED : map_status(t->status);
    p->actual_length = t->actual_num_bytes;
    if (p->status == LIBUSB_TRANSFER_COMPLETED) {
        s_stats.bulk_done++;
        s_stats.bulk_bytes += p->actual_length;
    } else if (p->status != LIBUSB_TRANSFER_CANCELLED) {
        s_stats.bulk_errors++;
    }
    bool in = p->endpoint & LIBUSB_ENDPOINT_IN;
    unsigned char *user_buf = p->buffer;
    if (in) p->buffer = t->data_buffer;                  /* zero copy, see top of file */
    if (p->callback) p->callback(p);
    if (in && p->buffer == t->data_buffer) p->buffer = user_buf;
}

int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed)
{
    int64_t budget_ms = tv ? (int64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000 : 60000;
    xfer_t *x;
    for (;;) {
        UBaseType_t backlog = uxQueueMessagesWaiting(s_doneq);
        if (!s_halted && backlog > s_stats.max_backlog) s_stats.max_backlog = backlog;
        TickType_t wait = budget_ms > 20 ? pdMS_TO_TICKS(20) : pdMS_TO_TICKS(budget_ms);
        if (xQueueReceive(s_doneq, &x, wait) == pdTRUE) {
            deliver(x);
            while (xQueueReceive(s_doneq, &x, 0) == pdTRUE) deliver(x);
            clear_halt_if_idle();
            return 0;
        }
        clear_halt_if_idle();
        budget_ms -= 20;
        if (budget_ms <= 0 || (completed && *completed)) return 0;
    }
}

unsigned char *libusb_dev_mem_alloc(libusb_device_handle *h, size_t length) { return NULL; }
int libusb_dev_mem_free(libusb_device_handle *h, unsigned char *buffer, size_t length) { return 0; }
