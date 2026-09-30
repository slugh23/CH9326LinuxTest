/*
 * CH9326 USB-HID to UART bridge library (API-compatible with WCH libch9326).
 *
 * Unless noted, functions return 1 on success and 0 on failure.
 * "index" selects the device: 0 = first device found by ch9326_find(), ...
 * up to 15.
 */
#ifndef _CH9326_LIB_H
#define _CH9326_LIB_H

/* Baud rate */
#define B300        0x01
#define B600        0x02
#define B1200       0x03
#define B2400       0x04
#define B4800       0x05
#define B9600       0x06    /* chip default */
#define B14400      0x07
#define B19200      0x08
#define B28800      0x09
#define B38400      0x0A
#define B57600      0x0B
#define B76800      0x0C
#define B115200     0x0D

/* Parity */
#define P_ODD       0x01
#define P_EVEN      0x02
#define P_SPC       0x03    /* space */
#define P_NONE      0x04    /* default */

/* Data bits */
#define BIT_5       0x01
#define BIT_6       0x02
#define BIT_7       0x03
#define BIT_8       0x04    /* default */

/* Stop bits */
#define STOP_1      0x01    /* default */
#define STOP_2      0x02

#ifdef __cplusplus
extern "C" {
#endif

/* Scan the USB bus for CH9326 devices (1a86:e010). Must be called before
 * ch9326_open(). Returns the number of devices found (max 16), 0 if none. */
unsigned char ch9326_find(void);

/* Open a device, detach the kernel HID driver and start the receive thread. */
unsigned char ch9326_open(unsigned char index);

/* Stop the receive thread, re-attach the kernel HID driver and close. */
unsigned char ch9326_close(unsigned char index);

/* Configure the UART.
 *   rate      : B300 .. B115200
 *   check     : P_ODD, P_EVEN, P_SPC, P_NONE
 *   stop_bits : STOP_1, STOP_2
 *   data_bits : BIT_5 .. BIT_8
 *   interval  : receive packing timeout; vendor docs list 0x10 = 3 ms
 *               (default), 0x20 = 6 ms, 0x30 = 9 ms */
unsigned char ch9326_set(unsigned char index, unsigned char rate,
                         unsigned char check, unsigned char stop_bits,
                         unsigned char data_bits, unsigned char interval);

/* Transmit data on the UART. Returns the number of bytes sent (0 on failure). */
unsigned long ch9326_send(unsigned char index, unsigned char *data,
                          unsigned long length);

/* Non-blocking: copy up to length bytes already received from the UART.
 * Returns the number of bytes copied (0 if nothing is waiting). */
unsigned long ch9326_recv(unsigned char index, char *data, unsigned long length);

/* Set GPIO output levels. Bit n = IOn+1 (bit 0 = IO1, bit 1 = IO2, ...);
 * 1 = high, 0 = low. */
unsigned char ch9326_set_gpiodata(unsigned char index, unsigned char data);

/* Set GPIO directions. Bit n = IOn+1; 1 = output, 0 = input. */
unsigned char ch9326_set_gpiodir(unsigned char index, unsigned char dir);

/* ch9326_set_gpiodir() followed by ch9326_set_gpiodata(). */
unsigned char ch9326_set_gpio(unsigned char index, unsigned char dir,
                              unsigned char data);

/* Read GPIO input levels. Per vendor docs, bit 5 = IO1 and bit 3 = IO2
 * (1 = high). */
unsigned char ch9326_get_gpio(unsigned char index, char *data);

/* 1 if the device is open and has not been unplugged. */
unsigned char ch9326_connected(unsigned char index);

/* Read USB string descriptors 1/2/3 (language 0x0409). The buffer receives
 * the raw descriptor: byte 0 = length, byte 1 = 0x03, then UTF-16LE text. */
unsigned char ch9326_get_manufacturer_string(unsigned char index,
                                             unsigned char *data,
                                             unsigned long length);
unsigned char ch9326_get_product_string(unsigned char index,
                                        unsigned char *data,
                                        unsigned long length);
unsigned char ch9326_get_serial_number_string(unsigned char index,
                                              unsigned char *data,
                                              unsigned long length);

#ifdef __cplusplus
}
#endif

#endif /* _CH9326_LIB_H */
