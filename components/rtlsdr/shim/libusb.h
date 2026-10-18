/*
 * libusb.h: the subset of the libusb-1.0 API that librtlsdr uses, implemented on the ESP-IDF
 * USB Host Library by libusb_esp.c. Written for this project (not libusb's source); the names,
 * constants and struct layouts follow the public libusb-1.0 API so librtlsdr.c builds unmodified.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBUSB_CALL
#define LIBUSB_API_VERSION 0x01000102   /* < 0x01000105: librtlsdr skips its zero-copy path */

enum libusb_error {
    LIBUSB_SUCCESS = 0,
    LIBUSB_ERROR_IO = -1,
    LIBUSB_ERROR_INVALID_PARAM = -2,
    LIBUSB_ERROR_ACCESS = -3,
    LIBUSB_ERROR_NO_DEVICE = -4,
    LIBUSB_ERROR_NOT_FOUND = -5,
    LIBUSB_ERROR_BUSY = -6,
    LIBUSB_ERROR_TIMEOUT = -7,
    LIBUSB_ERROR_OVERFLOW = -8,
    LIBUSB_ERROR_PIPE = -9,
    LIBUSB_ERROR_INTERRUPTED = -10,
    LIBUSB_ERROR_NO_MEM = -11,
    LIBUSB_ERROR_NOT_SUPPORTED = -12,
    LIBUSB_ERROR_OTHER = -99,
};

enum libusb_transfer_status {
    LIBUSB_TRANSFER_COMPLETED = 0,
    LIBUSB_TRANSFER_ERROR,
    LIBUSB_TRANSFER_TIMED_OUT,
    LIBUSB_TRANSFER_CANCELLED,
    LIBUSB_TRANSFER_STALL,
    LIBUSB_TRANSFER_NO_DEVICE,
    LIBUSB_TRANSFER_OVERFLOW,
};

enum libusb_endpoint_direction { LIBUSB_ENDPOINT_OUT = 0x00, LIBUSB_ENDPOINT_IN = 0x80 };
enum libusb_request_type {
    LIBUSB_REQUEST_TYPE_STANDARD = 0x00 << 5,
    LIBUSB_REQUEST_TYPE_CLASS = 0x01 << 5,
    LIBUSB_REQUEST_TYPE_VENDOR = 0x02 << 5,
};
enum libusb_transfer_type { LIBUSB_TRANSFER_TYPE_CONTROL = 0, LIBUSB_TRANSFER_TYPE_BULK = 2 };

struct libusb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

struct libusb_transfer;
typedef void (*libusb_transfer_cb_fn)(struct libusb_transfer *transfer);

struct libusb_transfer {
    libusb_device_handle *dev_handle;
    uint8_t flags;
    unsigned char endpoint;
    unsigned char type;
    unsigned int timeout;
    enum libusb_transfer_status status;
    int length;
    int actual_length;
    libusb_transfer_cb_fn callback;
    void *user_data;
    unsigned char *buffer;
    int num_iso_packets;
};

int  libusb_init(libusb_context **ctx);
void libusb_exit(libusb_context *ctx);

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list);
void libusb_free_device_list(libusb_device **list, int unref_devices);
int  libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc);

int  libusb_open(libusb_device *dev, libusb_device_handle **handle);
void libusb_close(libusb_device_handle *handle);
libusb_device *libusb_get_device(libusb_device_handle *handle);
int  libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx, unsigned char *data, int length);

int  libusb_kernel_driver_active(libusb_device_handle *h, int iface);
int  libusb_detach_kernel_driver(libusb_device_handle *h, int iface);
int  libusb_attach_kernel_driver(libusb_device_handle *h, int iface);
int  libusb_claim_interface(libusb_device_handle *h, int iface);
int  libusb_release_interface(libusb_device_handle *h, int iface);
int  libusb_reset_device(libusb_device_handle *h);

int  libusb_control_transfer(libusb_device_handle *h, uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, unsigned char *data,
                             uint16_t wLength, unsigned int timeout);
int  libusb_bulk_transfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data,
                          int length, int *transferred, unsigned int timeout);

struct libusb_transfer *libusb_alloc_transfer(int iso_packets);
void libusb_free_transfer(struct libusb_transfer *transfer);
int  libusb_submit_transfer(struct libusb_transfer *transfer);
int  libusb_cancel_transfer(struct libusb_transfer *transfer);
int  libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed);

/* librtlsdr only calls these under __linux__ guards; declared so the code parses. */
unsigned char *libusb_dev_mem_alloc(libusb_device_handle *h, size_t length);
int  libusb_dev_mem_free(libusb_device_handle *h, unsigned char *buffer, size_t length);

static inline void libusb_fill_bulk_transfer(struct libusb_transfer *t, libusb_device_handle *h,
        unsigned char endpoint, unsigned char *buffer, int length, libusb_transfer_cb_fn callback,
        void *user_data, unsigned int timeout)
{
    t->dev_handle = h;
    t->endpoint = endpoint;
    t->type = LIBUSB_TRANSFER_TYPE_BULK;
    t->timeout = timeout;
    t->buffer = buffer;
    t->length = length;
    t->user_data = user_data;
    t->callback = callback;
}

#ifdef __cplusplus
}
#endif
