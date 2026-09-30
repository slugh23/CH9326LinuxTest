# CH9326 HID protocol

WCH doesn't publish this. It was recovered by disassembling the vendor's x86-64
`lib/libch9326.so` (it isn't stripped). `lib/ch9326.c` reimplements it. A mock-libusb
test showed the new library sends exactly the same bytes as the vendor binary for
every call.

## Device

| Item | Value |
|---|---|
| VID:PID | `1a86:e010` |
| Interface | 0 (HID class; Linux binds `usbhid` → `/dev/hidrawN`) |
| Interrupt OUT | EP `0x02`, 32-byte reports: UART TX data |
| Interrupt IN | EP `0x82`, 32-byte reports: UART RX data |
| Control pipe | HID SET_REPORT / GET_REPORT: configuration and GPIO |
| Report IDs | none (report ID 0) |

## UART data (interrupt endpoints)

Both directions use the same framing:

```
byte 0      : N = number of valid payload bytes (0..31)
byte 1..N   : payload
byte N+1..31: ignored
```

* **TX** is sent on EP 0x02. The data is split into 31-byte chunks. The vendor lib sends
  full chunks as 32-byte packets and the last partial chunk as a short `N+1`-byte
  packet.
* **RX** is read on EP 0x82 (the vendor lib uses a 32-byte read with a 2 s timeout, in a
  loop on a background thread). Reports with `N = 0` do occur and are ignored.

## Commands: HID SET_REPORT (Output), control pipe

`bmRequestType=0x21, bRequest=0x09 (SET_REPORT), wValue=0x0200 (Output, ID 0),
wIndex=0, wLength=32`. The report is zero-padded to 32 bytes.

### `FF` — UART configuration

```
FF  cfg  baud_hi  baud_lo  interval  00...
```

`cfg` = `0xC0 | parity | stop | databits`:

| Field | Bits | Values |
|---|---|---|
| parity | `0x38` | none `0x00`, odd `0x08`, even `0x18`, space `0x38` (mark `0x28` is probably valid but the vendor lib never sends it) |
| stop | `0x04` | **set = 1 stop bit**, clear = 2 stop bits (the reverse of a 16550 LCR; this is what the vendor lib sends) |
| data bits | `0x03` | 5→`0`, 6→`1`, 7→`2`, 8→`3` |

Baud bytes (same scheme as the CH341: `baud = 6 MHz / 8^(3-ps) / (256-div)`, with
`ps = baud_hi & 3` and `div = baud_lo`, where bit 7 of `baud_hi` is always set):

| Baud | hi lo | Baud | hi lo | Baud | hi lo |
|---|---|---|---|---|---|
| 300 | `80 D9` | 4800 | `82 64` | 38400 | `83 64` |
| 600 | `81 64` | 9600 | `82 B2` | 57600 | `83 98` |
| 1200 | `81 B2` | 14400 | `82 CC` | 76800 | `83 B2` |
| 2400 | `81 D9` | 19200 | `82 D9` | 115200 | `83 CC` |
| | | 28800 | `83 30` | | |

Other baud rates may work if you compute the bytes from the formula, but that's
untested.

`interval` sets the receive packing timeout. The vendor docs say `0x10` = 3 ms
(default), `0x20` = 6 ms, `0x30` = 9 ms.

Example, 115200 8N1, interval 0x10: `FF C7 83 CC 10 00 …`

### `C0` — GPIO direction

`C0 dir 00…`: bit n = IO(n+1), 1 = output, 0 = input.

### `B0` — GPIO output level

`B0 levels 00…`: bit n = IO(n+1), 1 = high.

## GPIO input: HID GET_REPORT (Input), control pipe

`bmRequestType=0xA1, bRequest=0x01 (GET_REPORT), wValue=0x0100 (Input, ID 0),
wIndex=0, wLength=32`. The device returns 2 bytes. In byte 0, per the vendor docs,
**bit 5 = IO1** and **bit 3 = IO2** (1 = high).

## Strings

Standard USB string descriptors 1/2/3 (manufacturer/product/serial), language
0x0409. The vendor API returns the raw descriptor (2-byte header + UTF-16LE).

## Without libusb: hidraw (untested)

Because the chip is a plain HID device, it should also be possible to leave `usbhid`
attached and use `/dev/hidrawN` directly. Report IDs are 0, so every buffer passed to
the kernel starts with an extra `0x00` byte:

| Operation | hidraw call |
|---|---|
| UART TX | `write(fd, [00, N, payload…], 33)` (goes to the interrupt OUT EP) |
| UART RX | `read(fd, buf, 32)` → `[N, payload…]` |
| Config / GPIO (`FF`/`C0`/`B0`) | `ioctl(fd, HIDIOCSOUTPUT(33), [00, cmd…])` (a SET_REPORT Output on the control pipe; needs kernel ≥ 5.11) |
| GPIO read | `ioctl(fd, HIDIOCGINPUT(33), [00, …])` (needs kernel ≥ 5.11) |

hidapi (≥ 0.14) supports this too: `hid_write` for TX, `hid_read` for RX, and
`hid_send_output_report` for the commands. The vendor lib sends commands over the
control pipe, so use SET_REPORT for them. Don't assume the chip accepts commands on
the interrupt endpoint.
