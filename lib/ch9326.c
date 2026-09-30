/*
 * Open-source, architecture-independent reimplementation of WCH's
 * libch9326 (CH9326 USB-HID to UART bridge, VID 0x1a86 / PID 0xe010).
 *
 * The wire protocol was recovered from the vendor's x86-64 libch9326.so;
 * see PROTOCOL.md for the details. The public API matches ch9326.h so
 * existing code links against this unchanged.
 *
 * Differences from the vendor library (behavioural fixes only, the bytes
 * on the wire are identical):
 *   - close() stops and joins the receive thread before freeing anything
 *     (the vendor lib freed the buffer while the thread was still running).
 *   - The receive ring buffer is protected by a mutex and its state is kept
 *     per device (the vendor lib used shared statics).
 *   - send()/recv() have no 8 KiB / 4 KiB length limits.
 *   - The kernel's usbhid driver is re-attached when the device is closed.
 */

#include <pthread.h>
#include <string.h>
#include <libusb.h>

#include "ch9326.h"

#define CH9326_VID          0x1a86
#define CH9326_PID          0xe010
#define CH9326_MAX_DEVICES  16
#define CH9326_INTERFACE    0
#define CH9326_EP_OUT       0x02
#define CH9326_EP_IN        0x82
#define CH9326_REPORT_SIZE  32
#define CH9326_PAYLOAD_MAX  (CH9326_REPORT_SIZE - 1)
#define CH9326_RX_BUF_SIZE  0x2000  /* must be a power of two */

#define WRITE_TIMEOUT_MS    1000
#define READ_TIMEOUT_MS     2000
#define POLL_TIMEOUT_MS     250     /* rx thread poll, bounds close() latency */

/* HID class requests */
#define HID_REQ_GET_REPORT  0x01
#define HID_REQ_SET_REPORT  0x09
#define HID_REPORT_INPUT    0x0100
#define HID_REPORT_OUTPUT   0x0200
#define REQTYPE_CLASS_OUT   (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE)
#define REQTYPE_CLASS_IN    (LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE)

/* Command bytes (first byte of a SET_REPORT output report) */
#define CMD_SET_UART        0xff
#define CMD_SET_GPIO_DATA   0xb0
#define CMD_SET_GPIO_DIR    0xc0

struct ch9326_dev {
    libusb_device *dev;
    libusb_device_handle *handle;
    int opened;
    volatile int stop;
    volatile int disconnected;
    pthread_t thread;
    pthread_mutex_t lock;
    size_t head;                /* total bytes written into rx */
    size_t tail;                /* total bytes read out of rx */
    unsigned char rx[CH9326_RX_BUF_SIZE];
};

static libusb_context *ctx;
static struct ch9326_dev devs[CH9326_MAX_DEVICES];

/* Indexed by the B300..B115200 constants: {prescaler byte, divisor byte} */
static const unsigned char baud_table[][2] = {
    [B300]    = {0x80, 0xd9},
    [B600]    = {0x81, 0x64},
    [B1200]   = {0x81, 0xb2},
    [B2400]   = {0x81, 0xd9},
    [B4800]   = {0x82, 0x64},
    [B9600]   = {0x82, 0xb2},
    [B14400]  = {0x82, 0xcc},
    [B19200]  = {0x82, 0xd9},
    [B28800]  = {0x83, 0x30},
    [B38400]  = {0x83, 0x64},
    [B57600]  = {0x83, 0x98},
    [B76800]  = {0x83, 0xb2},
    [B115200] = {0x83, 0xcc},
};

static struct ch9326_dev *get_open(unsigned char index)
{
    if (index >= CH9326_MAX_DEVICES || !devs[index].opened)
        return NULL;
    return &devs[index];
}

static int set_report(struct ch9326_dev *d, const unsigned char *data, int len)
{
    unsigned char report[CH9326_REPORT_SIZE] = {0};
    int r;

    memcpy(report, data, len);
    r = libusb_control_transfer(d->handle, REQTYPE_CLASS_OUT, HID_REQ_SET_REPORT,
                                HID_REPORT_OUTPUT, CH9326_INTERFACE,
                                report, sizeof(report), WRITE_TIMEOUT_MS);
    return r == (int)sizeof(report);
}

static void *rx_thread(void *arg)
{
    struct ch9326_dev *d = arg;
    unsigned char report[CH9326_REPORT_SIZE];

    while (!d->stop) {
        int n = 0;
        int r = libusb_interrupt_transfer(d->handle, CH9326_EP_IN, report,
                                          sizeof(report), &n, POLL_TIMEOUT_MS);
        if (r == LIBUSB_ERROR_NO_DEVICE) {
            d->disconnected = 1;
            break;
        }
        if (r != 0 || n < 1)
            continue;

        /* Input report: byte 0 = number of valid bytes that follow */
        size_t len = report[0];
        if (len > (size_t)n - 1)
            len = n - 1;

        pthread_mutex_lock(&d->lock);
        for (size_t i = 0; i < len; i++) {
            d->rx[d->head++ & (CH9326_RX_BUF_SIZE - 1)] = report[1 + i];
            if (d->head - d->tail > CH9326_RX_BUF_SIZE)
                d->tail++;      /* overflow: drop the oldest byte */
        }
        pthread_mutex_unlock(&d->lock);
    }
    return NULL;
}

unsigned char ch9326_find(void)
{
    libusb_device **list;
    ssize_t cnt;
    unsigned char found = 0;

    if (!ctx && libusb_init(&ctx) != 0)
        return 0;

    cnt = libusb_get_device_list(ctx, &list);
    if (cnt < 0)
        return 0;

    /* Keep open devices in their slots; refill the rest. */
    for (int i = 0; i < CH9326_MAX_DEVICES; i++) {
        if (!devs[i].opened && devs[i].dev) {
            libusb_unref_device(devs[i].dev);
            devs[i].dev = NULL;
        }
    }

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        int slot, already_open = 0;

        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != CH9326_VID || desc.idProduct != CH9326_PID)
            continue;

        for (slot = 0; slot < CH9326_MAX_DEVICES; slot++)
            if (devs[slot].opened && devs[slot].dev == list[i])
                already_open = 1;
        if (already_open) {
            found++;
            continue;
        }

        for (slot = 0; slot < CH9326_MAX_DEVICES && devs[slot].dev; slot++)
            ;
        if (slot == CH9326_MAX_DEVICES)
            break;
        devs[slot].dev = libusb_ref_device(list[i]);
        found++;
    }

    libusb_free_device_list(list, 1);
    return found;
}

unsigned char ch9326_open(unsigned char index)
{
    struct ch9326_dev *d;

    if (index >= CH9326_MAX_DEVICES)
        return 0;
    d = &devs[index];
    if (d->opened || !d->dev)
        return 0;

    if (libusb_open(d->dev, &d->handle) != 0) {
        d->handle = NULL;
        return 0;
    }

    /* The kernel binds usbhid to the chip; take it over while we're open. */
    if (libusb_set_auto_detach_kernel_driver(d->handle, 1) != 0)
        libusb_detach_kernel_driver(d->handle, CH9326_INTERFACE);

    if (libusb_claim_interface(d->handle, CH9326_INTERFACE) != 0)
        goto err_close;

    d->head = d->tail = 0;
    d->stop = 0;
    d->disconnected = 0;
    pthread_mutex_init(&d->lock, NULL);
    if (pthread_create(&d->thread, NULL, rx_thread, d) != 0)
        goto err_release;

    d->opened = 1;
    return 1;

err_release:
    pthread_mutex_destroy(&d->lock);
    libusb_release_interface(d->handle, CH9326_INTERFACE);
err_close:
    libusb_close(d->handle);
    d->handle = NULL;
    return 0;
}

unsigned char ch9326_close(unsigned char index)
{
    struct ch9326_dev *d = get_open(index);

    if (!d)
        return 0;

    d->stop = 1;
    pthread_join(d->thread, NULL);
    pthread_mutex_destroy(&d->lock);
    libusb_release_interface(d->handle, CH9326_INTERFACE);
    libusb_close(d->handle);
    d->handle = NULL;
    d->opened = 0;
    return 1;
}

unsigned char ch9326_set(unsigned char index, unsigned char rate,
                         unsigned char check, unsigned char stop_bits,
                         unsigned char data_bits, unsigned char interval)
{
    struct ch9326_dev *d = get_open(index);
    unsigned char cfg = 0xc0;

    if (!d || rate < B300 || rate > B115200 || check < P_ODD || check > P_NONE ||
        stop_bits < STOP_1 || stop_bits > STOP_2 ||
        data_bits < BIT_5 || data_bits > BIT_8)
        return 0;

    switch (check) {
    case P_ODD:  cfg |= 0x08; break;
    case P_EVEN: cfg |= 0x18; break;
    case P_SPC:  cfg |= 0x38; break;
    case P_NONE: break;
    }
    /* Note: the vendor lib sets bit 2 for ONE stop bit, clears it for two. */
    if (stop_bits == STOP_1)
        cfg |= 0x04;
    cfg |= data_bits - BIT_5;   /* 0 = 5 bits ... 3 = 8 bits */

    unsigned char cmd[5] = {
        CMD_SET_UART, cfg, baud_table[rate][0], baud_table[rate][1], interval
    };
    return set_report(d, cmd, sizeof(cmd));
}

unsigned long ch9326_send(unsigned char index, unsigned char *data,
                          unsigned long length)
{
    struct ch9326_dev *d = get_open(index);
    unsigned long sent = 0;

    if (!d || !data || !length)
        return 0;

    /*
     * Output reports: byte 0 = payload length (1..31), then payload.
     * Full chunks go out as 32-byte reports; the vendor lib sends the final
     * partial chunk as a short (len + 1 byte) packet, and so do we.
     */
    while (sent < length) {
        unsigned char report[CH9326_REPORT_SIZE] = {0};
        unsigned long chunk = length - sent;
        int xfer_len, n = 0;

        if (chunk > CH9326_PAYLOAD_MAX)
            chunk = CH9326_PAYLOAD_MAX;
        report[0] = (unsigned char)chunk;
        memcpy(report + 1, data + sent, chunk);
        xfer_len = chunk == CH9326_PAYLOAD_MAX ? CH9326_REPORT_SIZE : (int)chunk + 1;

        if (libusb_interrupt_transfer(d->handle, CH9326_EP_OUT, report, xfer_len,
                                      &n, WRITE_TIMEOUT_MS) != 0 || n != xfer_len)
            break;
        sent += chunk;
    }
    return sent;
}

unsigned long ch9326_recv(unsigned char index, char *data, unsigned long length)
{
    struct ch9326_dev *d = get_open(index);
    unsigned long n = 0;

    if (!d || !data)
        return 0;

    pthread_mutex_lock(&d->lock);
    while (n < length && d->tail != d->head)
        data[n++] = (char)d->rx[d->tail++ & (CH9326_RX_BUF_SIZE - 1)];
    pthread_mutex_unlock(&d->lock);
    return n;
}

unsigned char ch9326_set_gpiodata(unsigned char index, unsigned char data)
{
    struct ch9326_dev *d = get_open(index);
    unsigned char cmd[2] = {CMD_SET_GPIO_DATA, data};

    return d ? set_report(d, cmd, sizeof(cmd)) : 0;
}

unsigned char ch9326_set_gpiodir(unsigned char index, unsigned char dir)
{
    struct ch9326_dev *d = get_open(index);
    unsigned char cmd[2] = {CMD_SET_GPIO_DIR, dir};

    return d ? set_report(d, cmd, sizeof(cmd)) : 0;
}

unsigned char ch9326_set_gpio(unsigned char index, unsigned char dir,
                              unsigned char data)
{
    return ch9326_set_gpiodir(index, dir) && ch9326_set_gpiodata(index, data);
}

unsigned char ch9326_get_gpio(unsigned char index, char *data)
{
    struct ch9326_dev *d = get_open(index);
    unsigned char report[CH9326_REPORT_SIZE] = {0};
    int r;

    if (!d || !data)
        return 0;

    /* The device answers the 32-byte GET_REPORT with 2 bytes. */
    r = libusb_control_transfer(d->handle, REQTYPE_CLASS_IN, HID_REQ_GET_REPORT,
                                HID_REPORT_INPUT, CH9326_INTERFACE,
                                report, sizeof(report), READ_TIMEOUT_MS);
    if (r < 1)
        return 0;
    *data = (char)report[0];
    return 1;
}

unsigned char ch9326_connected(unsigned char index)
{
    struct ch9326_dev *d = get_open(index);

    return d && !d->disconnected;
}

/* These return the raw USB string descriptor (2-byte header + UTF-16LE). */
static unsigned char get_string(unsigned char index, uint8_t desc_index,
                                unsigned char *data, unsigned long length)
{
    struct ch9326_dev *d = get_open(index);

    if (!d || !data)
        return 0;
    return libusb_get_string_descriptor(d->handle, desc_index, 0x0409,
                                        data, (int)length) >= 0;
}

unsigned char ch9326_get_manufacturer_string(unsigned char index,
                                             unsigned char *data,
                                             unsigned long length)
{
    return get_string(index, 1, data, length);
}

unsigned char ch9326_get_product_string(unsigned char index,
                                        unsigned char *data,
                                        unsigned long length)
{
    return get_string(index, 2, data, length);
}

unsigned char ch9326_get_serial_number_string(unsigned char index,
                                              unsigned char *data,
                                              unsigned long length)
{
    return get_string(index, 3, data, length);
}
